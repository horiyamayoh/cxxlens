#pragma once

/**
 * @file core.hpp
 * @brief cxxlens の version 契約を提供する。
 *
 * @details LLVM 非依存で利用できる最小 M0 API。schema と LLVM baseline を library version から
 * 分離し、report や manifest が互換性を個別に判断できるようにする。
 */

#include <compare>
#include <cstdint>
#include <string>

namespace cxxlens
{

	/**
	 * @brief Semantic Versioning 形式の version 値。
	 * @note prerelease は先頭の `-` を含まない。比較は field の辞書式比較であり、SemVer の
	 * prerelease precedence 判定を提供するものではない。
	 */
	struct semantic_version
	{
		/** @brief Major version。 */
		std::uint16_t major{};
		/** @brief Minor version。 */
		std::uint16_t minor{};
		/** @brief Patch version。 */
		std::uint16_t patch{};
		/** @brief `alpha.1` などの任意 prerelease identifier。 */
		std::string prerelease;

		/**
		 * @brief version を表示用文字列へ変換する。
		 * @retval value `major.minor.patch` と任意の `-prerelease` からなる文字列。
		 * @pre `prerelease` は先頭の `-` を含まない。
		 * @post object と外部状態を変更しない。
		 * @note 文字列の生成に失敗した場合は `std::bad_alloc` が伝播する。
		 * @code{.cpp}
		 * #include <cxxlens/core.hpp>
		 * int main()
		 * {
		 *     const cxxlens::semantic_version version{1U, 2U, 3U, "rc.1"};
		 *     return version.to_string() == "1.2.3-rc.1" ? 0 : 1;
		 * }
		 * @endcode
		 */
		[[nodiscard]] std::string to_string() const;

		/**
		 * @brief 全 field を辞書式に比較する。
		 * @param[in] other 比較対象の version。
		 * @retval value 全 field の辞書式比較結果。
		 * @pre なし。
		 * @post 両方の object と外部状態を変更しない。
		 * @note SemVer の prerelease precedence ではなく `std::string` の辞書式順序を使用する。
		 * @code{.cpp}
		 * #include <cxxlens/core.hpp>
		 * int main()
		 * {
		 *     const cxxlens::semantic_version older{1U, 0U, 0U, {}};
		 *     const cxxlens::semantic_version newer{1U, 1U, 0U, {}};
		 *     return older < newer ? 0 : 1;
		 * }
		 * @endcode
		 */
		auto operator<=>(const semantic_version& other) const = default;
	};

	/**
	 * @brief cxxlens product と公開 schema の version 集合。
	 * @note 各 axis は独立して versioning され、library version だけから schema compatibility を
	 * 推測してはならない。
	 */
	struct api_versions
	{
		/** @brief cxxlens library version。 */
		semantic_version library;
		/** @brief Public JSON/YAML schema version。 */
		semantic_version public_schema;
		/** @brief Immutable fact schema version。 */
		semantic_version fact_schema;
		/** @brief Finding schema version。 */
		semantic_version finding_schema;
		/** @brief Edit plan schema version。 */
		semantic_version edit_plan_schema;
		/** @brief Generation plan schema version。 */
		semantic_version generation_plan_schema;
		/** @brief Primary LLVM/Clang validation baseline。 */
		semantic_version llvm;
	};

	/**
	 * @brief 現在の product と schema の version 群を取得する。
	 * @retval value library、schema、LLVM baseline の version 群。
	 * @pre なし。
	 * @post 外部状態を変更しない。同一 build では同じ値を返す。
	 * @note LLVM version は現在 link された adapter の検出結果ではなく、M0 設計 baseline を示す。
	 * @code{.cpp}
	 * #include <cxxlens/core.hpp>
	 * int main()
	 * {
	 *     const auto product_versions = cxxlens::versions();
	 *     return product_versions.llvm.major == 22U ? 0 : 1;
	 * }
	 * @endcode
	 */
	[[nodiscard]] api_versions versions();

} // namespace cxxlens
