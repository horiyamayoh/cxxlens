#pragma once

/**
 * @file bounded_store_v6_sqlite_internal.hpp
 * @brief SQLite physical port for the bounded Store v6 phase core.
 *
 * This is a source-private production port.  It owns only bounded staging rows and a
 * physical cursor; semantic expected records are supplied by the independent materialization
 * authority through bounded_store_v6_phase_core.
 */

#include "bounded_store_v6_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** Open one authenticated SQLite bounded Store v6 backend at the exact configured path. */
	[[nodiscard]] result<std::unique_ptr<bounded_store_v6_backend_port>>
	make_bounded_store_v6_sqlite_backend_port(bounded_store_v6_session_metadata metadata);
} // namespace cxxlens::sdk::detail
