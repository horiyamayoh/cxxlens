#include <algorithm>
#include <set>
#include <string>
#include <utility>

#include <cxxlens/testing.hpp>

#include "../core/canonical_json.hpp"
#include "../runtime/filesystem_port.hpp"
#include "json_parser.hpp"

namespace cxxlens::testing
{
	namespace
	{
		using cxxlens::detail::json::json_value;

		[[nodiscard]] error assertion_error(std::string expectation,
											std::string actual,
											const result_observation& observation)
		{
			error failure;
			failure.code.value = "testing.assertion-failed";
			failure.message = "typed result assertion failed";
			failure.attributes.emplace("actual", std::move(actual));
			failure.attributes.emplace("expectation", std::move(expectation));
			failure.attributes.emplace("result_count", std::to_string(observation.result_count));
			failure.attributes.emplace("coverage", observation.coverage.to_json());
			failure.attributes.emplace("evidence", observation.supporting_evidence.to_json());
			if (observation.failure)
				failure.attributes.emplace("original_error_code", observation.failure->code.value);
			std::string unresolved_codes;
			for (const auto& item : observation.unresolved_items)
			{
				if (!unresolved_codes.empty())
					unresolved_codes.push_back(',');
				unresolved_codes += item.stable_code;
			}
			failure.attributes.emplace("unresolved_codes", std::move(unresolved_codes));
			return failure;
		}

		[[nodiscard]] std::string kind_name(const unresolved_kind kind)
		{
			return std::to_string(static_cast<unsigned>(kind));
		}

		[[nodiscard]] const json_value* field(const json_value::object& object,
											  const std::string_view name)
		{
			const auto found =
				std::ranges::find(object, name, &std::pair<std::string, json_value>::first);
			return found == object.end() ? nullptr : &found->second;
		}

		[[nodiscard]] error
		schema_error(std::string schema, std::string field_path, std::string reason)
		{
			error failure;
			failure.code.value = "testing.schema-mismatch";
			failure.message = "JSON does not conform to the registered schema";
			failure.attributes.emplace("field", std::move(field_path));
			failure.attributes.emplace("reason", std::move(reason));
			failure.attributes.emplace("schema", std::move(schema));
			return failure;
		}

		[[nodiscard]] std::string
		replace_root(std::string value, const path& root, const std::string_view token)
		{
			if (root.empty())
				return value;
			const auto prefix = root.lexically_normal().generic_string();
			if (value == prefix)
				return std::string{token};
			if (value.starts_with(prefix + "/"))
				return std::string{token} + value.substr(prefix.size());
			return value;
		}

		void normalize_value(json_value& value,
							 const golden_context& context,
							 const std::string_view field_name = {})
		{
			static const std::set<std::string, std::less<>> runtime_fields{
				"cache_hit", "elapsed", "elapsed_ms", "pid", "thread", "thread_id", "timestamp"};
			static const std::set<std::string, std::less<>> path_fields{"build_root",
																		"directory",
																		"file",
																		"path",
																		"resource_root",
																		"root",
																		"workspace_root"};
			if (runtime_fields.contains(field_name))
			{
				value = "<runtime>";
				return;
			}
			if (auto* string = std::get_if<std::string>(&value.value);
				string != nullptr && path_fields.contains(field_name))
			{
				*string = replace_root(std::move(*string), context.resource_root, "$RESOURCE");
				*string = replace_root(std::move(*string), context.build_root, "$BUILD");
				*string = replace_root(std::move(*string), context.workspace_root, "$WORKSPACE");
			}
			else if (auto* array = std::get_if<json_value::array>(&value.value))
			{
				for (auto& child : *array)
					normalize_value(child, context, field_name);
			}
			else if (auto* object = std::get_if<json_value::object>(&value.value))
			{
				for (auto& [name, child] : *object)
					normalize_value(child, context, name);
			}
		}
	} // namespace

	result_assertion result_assertion::has_exactly(const std::size_t value) const
	{
		auto output = *this;
		output.exact_count_ = value;
		return output;
	}

	result_assertion result_assertion::has_no_errors() const
	{
		auto output = *this;
		output.require_no_errors_ = true;
		return output;
	}

	result_assertion result_assertion::is_complete() const
	{
		auto output = *this;
		output.require_complete_ = true;
		return output;
	}

	result_assertion result_assertion::is_partial_with(const unresolved_kind kind) const
	{
		auto output = *this;
		output.partial_kind_ = kind;
		return output;
	}

	result_assertion result_assertion::json_matches(path golden) const
	{
		auto output = *this;
		output.golden_ = std::move(golden);
		return output;
	}

	result<void> result_assertion::check(const result_observation& observation) const
	{
		if (exact_count_ && observation.result_count != *exact_count_)
			return assertion_error(
				"exact-count=" + std::to_string(*exact_count_), "count-mismatch", observation);
		if (require_no_errors_ && observation.failure)
			return assertion_error("no-errors", "failed-error", observation);
		if (require_complete_ && !observation.coverage.complete())
			return assertion_error("complete", "partial-coverage", observation);
		if (partial_kind_)
		{
			const bool found = std::ranges::any_of(observation.unresolved_items,
												   [this](const unresolved& item)
												   {
													   return item.kind == *partial_kind_;
												   });
			if (!found || observation.coverage.complete())
				return assertion_error("partial-with=" + kind_name(*partial_kind_),
									   "partial-kind-missing",
									   observation);
		}
		if (golden_)
		{
			cxxlens::detail::runtime::standard_filesystem_adapter filesystem;
			cxxlens::detail::runtime::request_context context;
			context.operation = "testing.golden.read";
			auto golden = filesystem.read(*golden_, context);
			if (!golden)
			{
				auto failure =
					assertion_error("golden-readable", "golden-read-failed", observation);
				failure.attributes.emplace("golden", golden_->filename().generic_string());
				return failure;
			}
			if (golden.value() != observation.canonical_json)
				return assertion_error("golden-equal", "json-mismatch", observation);
		}
		return {};
	}

	result<std::string> normalize_golden(const std::string_view canonical_json,
										 const golden_context& context)
	{
		auto parsed = detail::parse_json(canonical_json);
		if (!parsed)
			return std::move(parsed.error());
		normalize_value(parsed.value(), context);
		return cxxlens::detail::json::write(parsed.value());
	}

	result<void> assert_schema_conforms(const std::string_view schema_id,
										std::string_view canonical_json)
	{
		schema_registry registry;
		auto descriptor = registry.find(schema_id, {1U, 0U, 0U, {}});
		if (!descriptor)
			return std::move(descriptor.error());
		auto parsed = detail::parse_json(canonical_json);
		if (!parsed)
			return std::move(parsed.error());
		const auto* object = std::get_if<json_value::object>(&parsed.value().value);
		if (object == nullptr)
			return schema_error(std::string{schema_id}, "$", "root-not-object");
		for (const auto& required : descriptor.value().required_fields)
			if (field(*object, required) == nullptr)
				return schema_error(
					std::string{schema_id}, "$." + required, "required-field-missing");
		const auto* schema = field(*object, "schema");
		const auto* actual = schema == nullptr ? nullptr : std::get_if<std::string>(&schema->value);
		if (actual == nullptr || *actual != schema_id)
			return schema_error(std::string{schema_id}, "$.schema", "schema-id-mismatch");
		return {};
	}
} // namespace cxxlens::testing
