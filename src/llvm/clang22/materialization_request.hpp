#pragma once

#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <cxxlens/sdk/store.hpp>

#include "materialization_json.hpp"
#include "provider_task_v3.hpp"

namespace cxxlens::detail::clang22::materialization
{
	struct validated_task_request
	{
		clang22_task_input worker_input;
		std::string provider_task_id;
		std::string provider_execution_id;
		std::string task_input_digest;
		sdk::provider::sandbox_requirement sandbox;
		std::vector<std::byte> worker_payload;
	};

	struct validated_publication_request
	{
		std::string backend;
		sdk::snapshot_series_selector selector;
		std::string series_id;
		bool genesis{};
		std::optional<std::string> expected_parent_publication;
		std::optional<std::string> sqlite_path;
	};

	/** Fully bound request. The raw JSON document remains the report/source authority. */
	struct validated_materialization_request
	{
		json_document document;
		sdk::relation_engine engine;
		std::vector<sdk::relation_descriptor> output_descriptors;
		std::vector<validated_task_request> tasks;
		validated_publication_request publication;
	};

	/** Validate exact v2 shape and recompute all pre-effect identities bottom-up. */
	[[nodiscard]] sdk::result<validated_materialization_request>
	validate_materialization_request(json_document document);
} // namespace cxxlens::detail::clang22::materialization
