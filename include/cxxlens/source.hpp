#pragma once

/**
 * @file source.hpp
 * @brief cxxlens の source identity に共通する最小公開型。
 *
 * @details M0 では filesystem path の語彙だけを公開する。source span、macro mapping、stable
 * identity は対応する契約と validator を実装する後続 M0 slice で追加する。
 */

#include <filesystem>

namespace cxxlens
{

	/** @brief cxxlens API が受け渡す filesystem path 型。 */
	using path = std::filesystem::path;

} // namespace cxxlens
