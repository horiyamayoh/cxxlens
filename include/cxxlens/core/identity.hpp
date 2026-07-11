#pragma once

/** @file identity.hpp @brief Stable typed ID value types。 */

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace cxxlens
{
	/** @brief Domain tag 付き full-digest stable identifier。 */
	template <class Tag>
	class typed_id
	{
	  public:
		/** @brief Empty ID を作る。
		 * @pre なし。 @post `empty()` は true。 @note Unresolved/default value 専用。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::fact_id{}.empty()?0:1;}
		 * @endcode */
		typed_id() = default;

		/** @brief Prefixed full ID を保持する。
		 * @param[in] value `prefix_` + 64 lower-case hex 形式。
		 * @pre Factory で生成または `valid()` で検証する。 @post input bytes を保持する。
		 * @note Short display は identity ではない。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * #include <string>
		 * int main(){cxxlens::fact_id id{"fact_"+std::string(64,'a')};return id.valid()?0:1;}
		 * @endcode */
		explicit typed_id(std::string value) : value_{std::move(value)} {}

		/** @brief Empty state を返す。
		 * @retval value ID が未設定なら true。 @pre なし。 @post object を変更しない。
		 * @note Invalid non-empty value とは区別される。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::plan_id{}.empty()?0:1;}
		 * @endcode */
		[[nodiscard]] bool empty() const noexcept
		{
			return value_.empty();
		}

		/** @brief Prefix と full digest の構文を検証する。
		 * @retval value Canonical ID syntax なら true。 @pre なし。 @post object を変更しない。
		 * @note Payload collision safety は collision registry が検証する。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::symbol_id{"bad"}.valid()?1:0;}
		 * @endcode */
		[[nodiscard]] bool valid() const noexcept
		{
			const auto separator = value_.rfind('_');
			if (separator == std::string::npos || separator == 0U ||
				value_.size() - separator - 1U != 64U)
				return false;
			for (std::size_t index = 0; index < separator; ++index)
			{
				const char value = value_[index];
				if (!valid_prefix_character(value))
					return false;
			}
			for (std::size_t index = separator + 1U; index < value_.size(); ++index)
			{
				const char value = value_[index];
				if (!valid_hexadecimal(value))
					return false;
			}
			return true;
		}

		/** @brief Full stable ID を参照する。
		 * @retval value Prefix と 64-hex digest。 @pre なし。 @post object を変更しない。
		 * @note Serialization/identity comparison は常にこの値を使う。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::rule_id{}.value().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string_view value() const noexcept
		{
			return value_;
		}

		/** @brief Full digest 部分を参照する。
		 * @retval value Valid ID なら 64-hex、otherwise empty。 @pre なし。
		 * @post object を変更しない。 @note Domain metadata は factory contract が固定する。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::artifact_id{}.full_digest().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string_view full_digest() const noexcept
		{
			const auto separator = value_.rfind('_');
			return valid() ? std::string_view{value_}.substr(separator + 1U) : std::string_view{};
		}

		/** @brief UI 専用の短縮表示を返す。
		 * @param[in] hexadecimal_digits 表示する digest 桁数。
		 * @retval value Prefix と指定桁数。 @pre なし。 @post object を変更しない。
		 * @note 戻り値を identity/collision 判定に使用してはならない。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * #include <string>
		 * int main(){cxxlens::fact_id id{"fact_"+std::string(64,'a')};return
		 * id.short_display(8)=="fact_aaaaaaaa"?0:1;}
		 * @endcode */
		[[nodiscard]] std::string short_display(std::size_t hexadecimal_digits = 12U) const
		{
			if (!valid())
				return {};
			const auto separator = value_.rfind('_');
			const auto count = hexadecimal_digits < 64U ? hexadecimal_digits : 64U;
			return value_.substr(0U, separator + 1U + count);
		}

		/** @brief 同じ tag の full ID を比較する。
		 * @param[in] other 比較対象。 @retval value Full ID が同一なら true。
		 * @pre なし。 @post object を変更しない。 @note 異なる tag は型として比較不能。
		 * @code{.cpp}
		 * #include <cxxlens/core/identity.hpp>
		 * int main(){return cxxlens::fact_id{}==cxxlens::fact_id{}?0:1;}
		 * @endcode */
		[[nodiscard]] bool operator==(const typed_id& other) const noexcept = default;

	  private:
		[[nodiscard]] static bool valid_prefix_character(const char value) noexcept
		{
			return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-';
		}
		[[nodiscard]] static bool valid_hexadecimal(const char value) noexcept
		{
			return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
		}
		std::string value_;
	};

	/** @brief File ID domain tag。 */ struct file_id_tag;
	/** @brief Compile-unit ID domain tag。 */ struct compile_unit_id_tag;
	/** @brief Build-variant ID domain tag。 */ struct build_variant_id_tag;
	/** @brief Fact ID domain tag。 */ struct fact_id_tag;
	/** @brief Symbol ID domain tag。 */ struct symbol_id_tag;
	/** @brief Type ID domain tag。 */ struct type_id_tag;
	/** @brief Finding ID domain tag。 */ struct finding_id_tag;
	/** @brief Plan ID domain tag。 */ struct plan_id_tag;
	/** @brief Artifact ID domain tag。 */ struct artifact_id_tag;
	/** @brief Surface ID domain tag。 */ struct surface_id_tag;
	/** @brief Rule ID domain tag。 */ struct rule_id_tag;
	/** @brief Operation ID domain tag。 */ struct operation_id_tag;
	/** @brief Stable file ID。 */ using file_id = typed_id<file_id_tag>;
	/** @brief Stable compile-unit ID。 */ using compile_unit_id = typed_id<compile_unit_id_tag>;
	/** @brief Stable build-variant ID。 */ using build_variant_id = typed_id<build_variant_id_tag>;
	/** @brief Stable fact ID。 */ using fact_id = typed_id<fact_id_tag>;
	/** @brief Stable symbol ID。 */ using symbol_id = typed_id<symbol_id_tag>;
	/** @brief Stable type ID。 */ using type_id = typed_id<type_id_tag>;
	/** @brief Stable finding ID。 */ using finding_id = typed_id<finding_id_tag>;
	/** @brief Stable plan ID。 */ using plan_id = typed_id<plan_id_tag>;
	/** @brief Stable artifact ID。 */ using artifact_id = typed_id<artifact_id_tag>;
	/** @brief Stable surface ID。 */ using surface_id = typed_id<surface_id_tag>;
	/** @brief Stable rule ID。 */ using rule_id = typed_id<rule_id_tag>;
	/** @brief Stable operation ID。 */ using operation_id = typed_id<operation_id_tag>;
} // namespace cxxlens
