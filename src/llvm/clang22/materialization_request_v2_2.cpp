#include "materialization_request_v2_2.hpp"

#include <algorithm>
#include <charconv>
#include <iterator>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::source_closure
{
	namespace
	{
		[[nodiscard]] validation_error error(
			std::string code, std::string field = {}, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		void append_field(std::string& output, const std::string_view value)
		{
			char digits[32]{};
			const auto converted =
				std::to_chars(std::begin(digits), std::end(digits), value.size());
			output.append(digits, converted.ptr);
			output.push_back(':');
			output.append(value);
		}

		[[nodiscard]] bool exact_required_features(
			const std::span<const std::string> features)
		{
			return std::ranges::equal(features, request_v2_2_required_features());
		}

		[[nodiscard]] std::expected<std::uint64_t, validation_error>
		unique_blob_size(
			std::span<const std::shared_ptr<const validated_snapshot>> closures,
			const request_v2_2_limits limits)
		{
			std::map<std::string, std::span<const std::byte>, std::less<>> unique;
			std::uint64_t total{};
			for (const auto& closure : closures)
			{
				for (const auto& blob : closure->blobs)
				{
					const auto [found, inserted] =
						unique.emplace(blob.content_digest, blob.content);
					if (!inserted)
					{
						if (!std::ranges::equal(found->second, blob.content))
							return std::unexpected(error(
								"source-closure.request-v2_2-blob-conflict",
								"content_digest", blob.content_digest));
						continue;
					}
					if (blob.content.size() > limits.maximum_unique_blob_bytes ||
						total > limits.maximum_unique_blob_bytes - blob.content.size())
						return std::unexpected(error(
							"source-closure.request-v2_2-aggregate-size", "closures"));
					total += blob.content.size();
				}
			}
			return total;
		}
	} // namespace

	std::vector<std::string> request_v2_2_required_features()
	{
		return {"task-input-chunks-v1", "task-source-closure-v1"};
	}

	std::expected<std::vector<std::string>, validation_error>
	negotiate_request_v2_2_features(
		const std::span<const std::string> advertised_features)
	{
		std::set<std::string, std::less<>> advertised;
		for (const auto& feature : advertised_features)
			if (feature.empty() || !advertised.emplace(feature).second)
				return std::unexpected(error(
					"source-closure.request-v2_2-feature-invalid",
					"advertised_features", feature));
		for (const auto& required : request_v2_2_required_features())
			if (!advertised.contains(required))
				return std::unexpected(error(
					"source-closure.request-v2_2-feature-missing",
					"required_features", required));
		return request_v2_2_required_features();
	}

	std::expected<std::string, validation_error>
	derive_request_v2_2_digest(const materialization_request_v2_2& request)
	{
		if (request.schema != "cxxlens.clang22-materialization-request.v2_2" ||
			request.request_version != "2.2.0" ||
			!exact_required_features(request.required_features))
			return std::unexpected(error(
				"source-closure.request-v2_2-contract", "schema-or-features"));
		if (request.closures.empty() || request.tasks.empty())
			return std::unexpected(error(
				"source-closure.request-v2_2-empty", "closures-or-tasks"));

		std::vector<std::string> closure_ids;
		closure_ids.reserve(request.closures.size());
		for (const auto& closure : request.closures)
		{
			if (!closure || closure->snapshot_id.empty() ||
				closure->snapshot_digest.empty())
				return std::unexpected(error(
					"source-closure.request-v2_2-closure-invalid", "closures"));
			closure_ids.push_back(closure->snapshot_id);
		}
		std::ranges::sort(closure_ids);
		if (std::ranges::adjacent_find(closure_ids) != closure_ids.end())
			return std::unexpected(error(
				"source-closure.request-v2_2-closure-duplicate", "closures"));

		std::vector<std::string> task_ids;
		task_ids.reserve(request.tasks.size());
		for (const auto& task : request.tasks)
		{
			auto derived = derive_task_v4_id(task);
			if (!derived || task.task_id != *derived)
				return std::unexpected(error(
					"source-closure.request-v2_2-task-invalid", "task_id",
					task.task_id));
			task_ids.push_back(task.task_id);
		}
		std::ranges::sort(task_ids);
		if (std::ranges::adjacent_find(task_ids) != task_ids.end())
			return std::unexpected(error(
				"source-closure.request-v2_2-task-duplicate", "tasks"));

		std::string projection{
			"cxxlens.clang22-materialization-request.v2_2.identity.v1"};
		append_field(projection, request.schema);
		append_field(projection, request.request_version);
		for (const auto& feature : request.required_features)
			append_field(projection, feature);
		for (const auto& closure : closure_ids)
			append_field(projection, closure);
		for (const auto& task : task_ids)
			append_field(projection, task);
		return sha256_digest(std::as_bytes(std::span{projection}));
	}

	std::expected<validated_request_v2_2, validation_error>
	validate_request_v2_2(materialization_request_v2_2 request,
		const std::span<const std::string> advertised_features,
		const request_v2_2_limits configured_limits)
	{
		if (configured_limits.maximum_closures == 0U ||
			configured_limits.maximum_tasks == 0U ||
			configured_limits.maximum_unique_blob_bytes == 0U)
			return std::unexpected(error(
				"source-closure.request-v2_2-limits", "limits"));
		if (request.closures.empty() ||
			request.closures.size() > configured_limits.maximum_closures)
			return std::unexpected(error(
				"source-closure.request-v2_2-closure-count", "closures"));
		if (request.tasks.empty() ||
			request.tasks.size() > configured_limits.maximum_tasks)
			return std::unexpected(error(
				"source-closure.request-v2_2-task-count", "tasks"));
		auto negotiated = negotiate_request_v2_2_features(advertised_features);
		if (!negotiated)
			return std::unexpected(std::move(negotiated.error()));
		if (!exact_required_features(request.required_features))
			return std::unexpected(error(
				"source-closure.request-v2_2-required-feature-drift",
				"required_features"));

		std::map<std::string, const validated_snapshot*, std::less<>> closure_by_id;
		for (const auto& closure : request.closures)
		{
			if (!closure ||
				closure->snapshot_id != "source-closure:" + closure->snapshot_digest ||
				!closure_by_id.emplace(closure->snapshot_id, closure.get()).second)
				return std::unexpected(error(
					"source-closure.request-v2_2-closure-invalid", "closures"));
		}
		for (const auto& task : request.tasks)
		{
			if (!task.closure)
				return std::unexpected(error(
					"source-closure.request-v2_2-task-closure", "tasks"));
			const auto found = closure_by_id.find(task.closure->snapshot_id);
			if (found == closure_by_id.end() || found->second != task.closure.get())
				return std::unexpected(error(
					"source-closure.request-v2_2-task-closure", "tasks",
					task.task_id));
		}
		auto unique_bytes = unique_blob_size(request.closures, configured_limits);
		if (!unique_bytes)
			return std::unexpected(std::move(unique_bytes.error()));
		auto digest = derive_request_v2_2_digest(request);
		if (!digest)
			return std::unexpected(std::move(digest.error()));
		const auto expected_id = "materialization-request:" + *digest;
		if (request.request_digest != *digest || request.request_id != expected_id)
			return std::unexpected(error(
				"source-closure.request-v2_2-id-mismatch", "request_id"));

		std::ranges::sort(request.closures, {}, [](const auto& closure)
		{
			return closure->snapshot_id;
		});
		std::ranges::sort(request.tasks, {}, &task_v4::task_id);
		return validated_request_v2_2{request.request_id, request.request_digest,
			std::move(*negotiated), std::move(request.closures),
			std::move(request.tasks), *unique_bytes};
	}
} // namespace cxxlens::detail::clang22::source_closure
