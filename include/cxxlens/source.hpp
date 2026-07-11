#pragma once

/** @file source.hpp @brief LLVM 非依存の source location と macro origin 契約。 */

#include <compare>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/identity.hpp>

namespace cxxlens
{
	/** @brief Public filesystem path value。Semantic identity には直接使用しない。 */
	using path = std::filesystem::path;

	/** @brief Location が real、invalid、unknown のいずれかを示す。 */
	enum class source_location_state : std::uint8_t
	{
		valid,
		invalid,
		unknown,
	};

	/** @brief 0-based byte offset と 1-based line/UTF-8 byte column。 */
	struct source_point
	{
		/** @brief Project-relative semantic file ID。 */
		file_id file;
		/** @brief File 先頭からの 0-based byte offset。 */
		std::uint64_t byte_offset{};
		/** @brief 1-based line。 */
		std::uint32_t line{};
		/** @brief 1-based UTF-8 byte column。 */
		std::uint32_t column{};
		/** @brief Coordinate validity state。 */
		source_location_state state{source_location_state::invalid};
		/** @brief Valid な byte location を作る。
		 * @param[in] file Semantic file key。 @param[in] byte_offset 0-based byte offset。
		 * @param[in] line 1-based line。 @param[in] utf8_byte_column 1-based UTF-8 byte column。
		 * @retval value state=valid の point。 @pre 引数は未検証。
		 * @post 値をそのまま保持する。 @note validator が zero coordinate を拒否する。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){auto
		 * p=cxxlens::source_point::at(cxxlens::file_id{"file_"+std::string(64,'a')},2,1,3);return
		 * p.column==3?0:1;}
		 * @endcode */
		[[nodiscard]] static source_point at(file_id file,
											 std::uint64_t byte_offset,
											 std::uint32_t line,
											 std::uint32_t utf8_byte_column);
		/** @brief Invalid location を作る。
		 * @retval value state=invalid の point。 @pre なし。 @post fabricated file/offset
		 * を持たない。
		 * @note offset zero の valid point とは異なる。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){return
		 * cxxlens::source_point::invalid().state==cxxlens::source_location_state::invalid?0:1;}
		 * @endcode */
		[[nodiscard]] static source_point invalid() noexcept;
		/** @brief Unknown location を作る。
		 * @retval value state=unknown の point。 @pre なし。 @post invalid と区別される。
		 * @note 後続 adapter は推測座標で置換してはならない。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){return
		 * cxxlens::source_point::unknown().state==cxxlens::source_location_state::unknown?0:1;}
		 * @endcode */
		[[nodiscard]] static source_point unknown() noexcept;
	};

	/** @brief End の計算が character または token 単位かを示す。 */
	enum class source_range_kind : std::uint8_t
	{
		character,
		token,
	};

	/** @brief 同一 file 上の half-open byte range `[begin,end)`。 */
	struct file_range
	{
		/** @brief Inclusive begin point。 */
		source_point begin;
		/** @brief Exclusive end point。 */
		source_point end;
		/** @brief Range endpoint derivation kind。 */
		source_range_kind kind{source_range_kind::token};
	};

	/** @brief 一段の macro invocation/definition/argument mapping。 */
	struct macro_frame
	{
		/** @brief C/C++ macro identifier。 */
		std::string macro_name;
		/** @brief Invocation range。 */
		file_range invocation;
		/** @brief Definition range（取得できた場合）。 */
		std::optional<file_range> definition;
		/** @brief 0-based argument index（argument expansion の場合）。 */
		std::optional<std::uint32_t> argument_index;
	};

	/** @brief Source materialization/origin category。 */
	enum class source_origin : std::uint8_t
	{
		directly_spelled,
		macro_argument,
		macro_body,
		macro_expansion,
		implicit_compiler_node,
		generated_file,
		system_header,
		builtin,
		unknown,
	};

	/** @brief Versioned source-content digest precondition。 */
	struct source_digest
	{
		/** @brief Lower-case algorithm identifier。 */
		std::string algorithm;
		/** @brief Algorithm/encoding contract version。 */
		std::uint32_t version{};
		/** @brief Lower-case hexadecimal digest。 */
		std::string value;
	};

	/** @brief Stable source invariant failure category。 */
	enum class source_validation_code : std::uint8_t
	{
		invalid_semantic_file_key,
		invalid_point_coordinates,
		different_files,
		reversed_range,
		invalid_macro_frame,
		invalid_digest,
		origin_stack_mismatch,
	};

	/** @brief Structured source validation failure。 */
	struct source_validation_error
	{
		/** @brief Stable failure category。 */
		source_validation_code code{};
		/** @brief Invalid canonical field path。 */
		std::string field;
	};

	/** @brief Primary/spelling/expansion と ordered macro stack を保持する source span。 */
	struct source_span
	{
		/** @brief Consumer が表示・identity に使う primary range。 */
		file_range primary;
		/** @brief Token spelling range（異なる場合）。 */
		std::optional<file_range> spelling;
		/** @brief Macro expansion range（異なる場合）。 */
		std::optional<file_range> expansion;
		/** Outermost invocation first, innermost invocation last. */
		std::vector<macro_frame> macro_stack;
		/** @brief Source origin category。 */
		source_origin origin{source_origin::unknown};
		/** @brief Source snapshot digest。 */
		source_digest digest;
		/** @brief Generated/system/policy read-only marker。 */
		bool read_only{};

		/** @brief Span invariant を独立検証する。
		 * @retval value valid なら nullopt、otherwise structured field/code。
		 * @pre なし。 @post object を変更しない。 @note Range は half-open byte range。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){cxxlens::source_span s;return s.validate()?0:1;}
		 * @endcode */
		[[nodiscard]] std::optional<source_validation_error> validate() const;
		/** @brief Direct edit の conservative eligibility を返す。
		 * @retval value valid/direct/non-macro/non-read-only span のみ true。
		 * @pre なし。 @post object を変更しない。 @note macro/generated/system/invalid は false。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){cxxlens::source_span s;return s.is_directly_editable()?1:0;}
		 * @endcode */
		[[nodiscard]] bool is_directly_editable() const noexcept;
		/** @brief Canonical source-span JSON を射影する。
		 * @retval value schema version と canonical field order を持つ JSON。
		 * @pre `validate()` が nullopt。 @post object と外部状態を変更しない。
		 * @note Absolute display path を出力しない。
		 * @code{.cpp}
		 * #include <cxxlens/source.hpp>
		 * int main(){cxxlens::source_span s;return s.to_canonical_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_canonical_json() const;
	};

} // namespace cxxlens
