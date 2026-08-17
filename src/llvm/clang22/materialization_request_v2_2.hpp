#pragma once

#include "provider_task_v4.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace cxxlens::detail::clang22::source_closure
{
	struct request_v2_2_limits
	{
		std::size_t maximum_closures{1024U};
		std::size_t maximum_tasks{4096U};
		std::uint64_t maximum_unique_blob_bytes{512U * 1024U * 1024U};
	};

	struct materialization_request_v2_2
	{
		std::string schema{"cxxlens.clang22-materialization-request.v2_2"};
		std::string request_version{"2.2.0"};
		std::string request_id;
		std::string request_digest;
		std::vector<std::string> required_features;
		std::vector<std::shared_ptr<const validated_snapshot>> closures;
		std::vector<task_v4> tasks;
	};

	struct validated_request_v2_2
	{
		std::string request_id;
		std::string request_digest;
		std::vector<std::string> negotiated_features;
		std::vector<std::shared_ptr<const validated_snapshot>> closures;
		std::vector<task_v4> tasks;
		std::uint64_t unique_blob_bytes{};
	};

	[[nodiscard]] std::vector<std::string> request_v2_2_required_features();

	[[nodiscard]] std::expected<std::vector<std::string>, validation_error>
	negotiate_request_v2_2_features(std::span<const std::string> advertised_features);

	[[nodiscard]] std::expected<std::string, validation_error>
	derive_request_v2_2_digest(const materialization_request_v2_2& request);

	[[nodiscard]] std::expected<validated_request_v2_2, validation_error>
	validate_request_v2_2(materialization_request_v2_2 request,
		std::span<const std::string> advertised_features,
		request_v2_2_limits configured_limits = {});
} // namespace cxxlens::detail::clang22::source_closure
