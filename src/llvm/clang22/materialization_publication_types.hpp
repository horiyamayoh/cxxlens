#pragma once

/** @file materialization_publication_types.hpp
 *  @brief Validated publication input shared by request admission and the Store boundary.
 *
 * This header intentionally contains only the typed publication fields needed by Store.  It
 * does not import a task codec, JSON request model, or a compatibility request implementation.
 */

#include <optional>
#include <string>

#include <cxxlens/sdk/store.hpp>

namespace cxxlens::detail::clang22::materialization
{
	/** Publication configuration after request validation and before Store effects. */
	struct validated_publication_request
	{
		std::string backend;
		sdk::snapshot_series_selector selector;
		std::string series_id;
		bool genesis{};
		std::optional<std::string> expected_parent_publication;
		std::optional<std::string> sqlite_path;
	};
} // namespace cxxlens::detail::clang22::materialization
