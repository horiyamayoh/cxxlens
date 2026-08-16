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
		/**
		 * Source-private v2.1 binding.  When present, the task.v3 bytes and decoded source stay in
		 * their sealed spools; claims validation must use this receipt instead of reconstructing a
		 * resident worker payload.  The legacy v2 path leaves this empty and retains its exact
		 * payload check.
		 */
		std::optional<clang22_task_source_receipt> source_receipt;
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
		/** One request-owned catalog authority shared by all source-private task bindings. */
		sdk::project_catalog catalog;
		sdk::relation_engine engine;
		std::vector<sdk::relation_descriptor> output_descriptors;
		std::vector<validated_task_request> tasks;
		validated_publication_request publication;
	};

	/** Validate exact v2 shape and recompute all pre-effect identities bottom-up. */
	[[nodiscard]] sdk::result<validated_materialization_request>
	validate_materialization_request(json_document document);

	/** Rebind every legacy projection to the retained request document before dispatch. */
	[[nodiscard]] sdk::result<void> validate_materialization_legacy_request_binding(
		const validated_materialization_request& request);
} // namespace cxxlens::detail::clang22::materialization
