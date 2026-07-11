#pragma once

/** @file failure.hpp @brief Stable failure、result、unresolved value contract。 */

#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <cxxlens/source.hpp>

namespace cxxlens
{
	template <class T>
	class result;

	/** @brief Failure が影響する semantic unit。 */
	enum class failure_scope : std::uint8_t
	{
		operation,
		file,
		compile_unit,
		build_variant,
		symbol,
		workspace,
	};

	/** @brief Lower-case dotted stable error code。 */
	struct error_code
	{
		/** @brief Stable code bytes。 */
		std::string value;
		/** @brief Stable code bytes を比較する。
		 * @param[in] other 比較対象。 @retval value Code bytes の辞書式比較結果。
		 * @pre なし。 @post 両 object を変更しない。 @note Message prose は比較しない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::error_code{"a.a"}<cxxlens::error_code{"b.b"}?0:1;}
		 * @endcode */
		auto operator<=>(const error_code& other) const = default;
	};

	/** @brief Operation を成立不能にした structured failure。 */
	struct error
	{
		/** @brief Machine-control-flow 用 stable code。 */
		error_code code;
		/** @brief Human-facing、identity 非構成の説明。 */
		std::string message;
		/** @brief Failure scope。 */
		failure_scope scope{failure_scope::operation};
		/** @brief Related normalized locations。 */
		std::vector<source_span> locations;
		/** @brief Value-owned nested causes。Cycle は型構造上作れない。 */
		std::vector<error> causes;
		/** @brief Stable machine actions。 */
		std::vector<std::string> suggested_actions;
		/** @brief Stable machine attributes。 */
		std::map<std::string, std::string> attributes;
		/** @brief Same operation を安全に再試行可能か。 */
		bool retryable{};

		/** @brief Registry と構造 invariant を検証する。
		 * @param[in] registered_codes Sorted unique stable code registry。
		 * @retval value Valid なら success、otherwise `core.schema-validation-failed`。
		 * @pre Registry 自身は sorted unique。 @post object を変更しない。
		 * @note Message prose は validation/control flow に使用しない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::error e{{"core.cancelled"},"stopped"};return
		 * e.validate(cxxlens::common_error_codes())?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate(const std::vector<std::string>& registered_codes) const;

		/** @brief Message 非依存の canonical semantic representation を返す。
		 * @retval value Code、scope、location、cause、action、attribute、retryable の表現。
		 * @pre `validate()` が成功する。 @post object を変更しない。
		 * @note Human message は意図的に含めない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::error a{{"core.cancelled"},"a"},b{{"core.cancelled"},"b"};return
		 * a.semantic_representation()==b.semantic_representation()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string semantic_representation() const;

		/** @brief Human-readable cause tree を射影する。
		 * @retval value Stable code と message を含む複数行説明。 @pre なし。
		 * @post object を変更しない。 @note Control flow には使用しない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::error e{{"core.cancelled"},"stopped"};return
		 * e.explain().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string explain() const;

		/** @brief Versioned canonical JSONへ射影する。
		 * @retval value Common writer output。 @pre `validate()` succeeds。 @post
		 * Objectを変更しない。
		 * @note Message remains display data and versions are embedded。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::error e;return e.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
	};

	/** @brief Expected failure を value または original `error` として保持する。 */
	template <class T>
	class result
	{
		static_assert(!std::is_same_v<std::remove_cv_t<T>, error>);

	  public:
		/** @brief Success value から構築する。
		 * @param[in] value Success value。 @pre なし。 @post `has_value()` は true。
		 * @note Allocation failure は例外境界 policy に従う。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<int> r{3};return r.value()==3?0:1;}
		 * @endcode */
		result(T value) : storage_{std::move(value)} {}

		/** @brief Structured failure から構築する。
		 * @param[in] failure Original failure chain。 @pre なし。 @post `has_value()` は false。
		 * @note Failure chain を flatten しない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<int> r{cxxlens::error{{"core.cancelled"},"x"}};return r?1:0;}
		 * @endcode */
		result(error failure) : storage_{std::move(failure)} {}

		/** @brief Success state を返す。
		 * @retval value Success value を保持すれば true。 @pre なし。 @post 変更しない。
		 * @note Empty success と failure を区別する基礎 state。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::result<int>{1}.has_value()?0:1;}
		 * @endcode */
		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<T>(storage_);
		}

		/** @brief Success state を boolean context へ射影する。
		 * @retval value `has_value()` と同じ。 @pre なし。 @post 変更しない。
		 * @note Explicit conversion のみ。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::result<int>{1}?0:1;}
		 * @endcode */
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}

		/** @brief Mutable success value を返す。
		 * @retval value Stored value。 @pre `has_value()` は true。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<int> r{1};r.value()=2;return r.value()==2?0:1;}
		 * @endcode */
		[[nodiscard]] T& value()
		{
			return std::get<T>(storage_);
		}

		/** @brief Const success value を返す。
		 * @retval value Stored value。 @pre `has_value()` は true。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){const cxxlens::result<int> r{2};return r.value()==2?0:1;}
		 * @endcode */
		[[nodiscard]] const T& value() const
		{
			return std::get<T>(storage_);
		}

		/** @brief Mutable failure chain を返す。
		 * @retval value Stored error。 @pre `has_value()` は false。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<int> r{cxxlens::error{{"core.cancelled"},"x"}};return
		 * r.error().code.value=="core.cancelled"?0:1;}
		 * @endcode */
		[[nodiscard]] cxxlens::error& error()
		{
			return std::get<cxxlens::error>(storage_);
		}

		/** @brief Const failure chain を返す。
		 * @retval value Stored error。 @pre `has_value()` は false。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){const cxxlens::result<int> r{cxxlens::error{{"core.cancelled"},"x"}};return
		 * r.error().code.value=="core.cancelled"?0:1;}
		 * @endcode */
		[[nodiscard]] const cxxlens::error& error() const
		{
			return std::get<cxxlens::error>(storage_);
		}

	  private:
		std::variant<T, cxxlens::error> storage_;
	};

	/** @brief Value-less expected operation result。 */
	template <>
	class result<void>
	{
	  public:
		/** @brief Successful void result を作る。
		 * @pre なし。 @post `has_value()` は true。 @note Sentinel bool ではない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::result<void>{}?0:1;}
		 * @endcode */
		result() = default;

		/** @brief Structured failure から構築する。
		 * @param[in] failure Original failure chain。 @pre なし。 @post `has_value()` は false。
		 * @note Cause chain を保持する。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<void> r{cxxlens::error{{"core.cancelled"},"x"}};return r?1:0;}
		 * @endcode */
		result(error failure) : storage_{std::move(failure)} {}

		/** @brief Success state を返す。
		 * @retval value Success なら true。 @pre なし。 @post 変更しない。
		 * @note Failure は structured error を保持する。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::result<void>{}.has_value()?0:1;}
		 * @endcode */
		[[nodiscard]] bool has_value() const noexcept
		{
			return std::holds_alternative<std::monostate>(storage_);
		}

		/** @brief Success state を boolean context へ射影する。
		 * @retval value `has_value()` と同じ。 @pre なし。 @post 変更しない。
		 * @note Explicit conversion のみ。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::result<void>{}?0:1;}
		 * @endcode */
		[[nodiscard]] explicit operator bool() const noexcept
		{
			return has_value();
		}

		/** @brief Mutable failure chain を返す。
		 * @retval value Stored error。 @pre `has_value()` は false。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::result<void> r{cxxlens::error{{"core.cancelled"},"x"}};return
		 * r.error().code.value=="core.cancelled"?0:1;}
		 * @endcode */
		[[nodiscard]] cxxlens::error& error()
		{
			return std::get<cxxlens::error>(storage_);
		}

		/** @brief Const failure chain を返す。
		 * @retval value Stored error。 @pre `has_value()` は false。 @post state を保持する。
		 * @note Invalid access は `std::bad_variant_access`。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){const cxxlens::result<void> r{cxxlens::error{{"core.cancelled"},"x"}};return
		 * r.error().code.value=="core.cancelled"?0:1;}
		 * @endcode */
		[[nodiscard]] const cxxlens::error& error() const
		{
			return std::get<cxxlens::error>(storage_);
		}

	  private:
		std::variant<std::monostate, cxxlens::error> storage_;
	};

	/** @brief Partial semantic uncertainty category。 */
	// Integrated design section 10.2 fixes the public underlying type.
	enum class unresolved_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		missing_compile_command,
		inferred_compile_command,
		malformed_compile_command,
		parse_failed,
		incomplete_ast,
		ambiguous_symbol,
		dependent_type,
		unresolved_overload,
		unsupported_language_extension,
		missing_module_bmi,
		macro_origin_ambiguous,
		macro_edit_unsafe,
		generated_code_read_only,
		function_pointer_target_unknown,
		callback_target_unknown,
		virtual_dynamic_type_unknown,
		open_world_virtual_target,
		alias_analysis_required,
		dataflow_budget_exceeded,
		path_sensitive_budget_exceeded,
		capability_unavailable,
		build_variant_disagreement,
		stale_source,
		custom,
	};

	/** @brief Required semantic precision。 */
	enum class precision_level : std::uint8_t
	{
		ast_structural,
		local_semantic,
		workspace_semantic,
		local_flow,
		interprocedural_summary,
		path_sensitive,
		dynamic_observation,
	};

	/** @brief Operation success 内で保持する machine-actionable uncertainty。 */
	struct unresolved
	{
		/** @brief Standard uncertainty category。 */
		unresolved_kind kind{};
		/** @brief Kind-specific stable code。 */
		std::string stable_code;
		/** @brief Human-facing、identity 非構成の summary。 */
		std::string summary;
		/** @brief Affected scope。 */
		failure_scope scope{failure_scope::operation};
		/** @brief Related normalized source spans。 */
		std::vector<source_span> related;
		/** @brief Missing machine inputs。 */
		std::vector<std::string> missing_inputs;
		/** @brief Stable remediation actions。 */
		std::vector<std::string> suggested_actions;
		/** @brief Required precision if current precision is insufficient。 */
		std::optional<precision_level> required_precision;
		/** @brief Required registered capability ID。 */
		std::optional<std::string> required_capability;
		/** @brief Stable machine attributes。 */
		std::map<std::string, std::string> attributes;

		/** @brief Kind/code/input/capability invariant を検証する。
		 * @param[in] registered_capabilities Sorted unique capability IDs。
		 * @retval value Valid なら success、otherwise structured error。
		 * @pre Registry は sorted unique。 @post object を変更しない。
		 * @note Unresolved は empty success へ変換してはならない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::unresolved u{cxxlens::unresolved_kind::dependent_type,
		 * "semantic.dependent-type","dependent"};return u.validate({})?0:1;}
		 * @endcode */
		[[nodiscard]] result<void>
		validate(const std::vector<std::string>& registered_capabilities) const;

		/** @brief Summary 非依存の canonical semantic representation を返す。
		 * @retval value Kind/code/scope/inputs/requirements/attributes の表現。
		 * @pre `validate()` が成功する。 @post object を変更しない。
		 * @note Human summary は意図的に含めない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::unresolved u{cxxlens::unresolved_kind::dependent_type,
		 * "semantic.dependent-type","x"};return u.semantic_representation().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string semantic_representation() const;

		/** @brief Versioned canonical JSONへ射影する。
		 * @retval value Common writer output。 @pre `validate()` succeeds。 @post
		 * Objectを変更しない。
		 * @note Missing optional requirements are explicit null。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::unresolved u;return u.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
	};

	/** @brief Common/package stable error code registry。 */
	class stable_code_registry
	{
	  public:
		/** @brief Common design/catalog codes を登録済みで作る。
		 * @pre なし。 @post `all()` は sorted unique。 @note Registry は process state 非依存。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::stable_code_registry{}.contains("core.cancelled")?0:1;}
		 * @endcode */
		stable_code_registry();

		/** @brief Package-owned stable code を追加する。
		 * @param[in] code Lower-case dotted namespaced code。
		 * @retval value Added なら success、invalid/duplicate なら error。
		 * @pre Package owner は先頭 namespace を所有する。 @post Success 時 `contains(code)`。
		 * @note Duplicate は hard error。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){cxxlens::stable_code_registry r;return r.register_code("plugin.example")?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> register_code(std::string code);

		/** @brief Stable code の登録状態を返す。
		 * @param[in] code Lookup code。 @retval value Registered なら true。
		 * @pre なし。 @post Registry を変更しない。 @note Message substring lookup ではない。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::stable_code_registry{}.contains("missing")?1:0;}
		 * @endcode */
		[[nodiscard]] bool contains(std::string_view code) const noexcept;

		/** @brief Sorted unique registry snapshot を返す。
		 * @retval value Deterministically ordered codes。 @pre なし。 @post Registry を変更しない。
		 * @note Serialization はこの order を使用できる。
		 * @code{.cpp}
		 * #include <cxxlens/core/failure.hpp>
		 * int main(){return cxxlens::stable_code_registry{}.all().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] const std::vector<std::string>& all() const noexcept;

	  private:
		std::vector<std::string> codes_;
	};

	/** @brief Built-in stable error codes を返す。
	 * @retval value Sorted unique immutable codes。 @pre なし。 @post 外部状態を変更しない。
	 * @note Catalog dangling-reference check の authoritative C++ projection。
	 * @code{.cpp}
	 * #include <cxxlens/core/failure.hpp>
	 * int main(){return cxxlens::common_error_codes().empty()?1:0;}
	 * @endcode */
	[[nodiscard]] const std::vector<std::string>& common_error_codes();

	/** @brief Error result を別 value type へ lossless propagation する。
	 * @param[in] source Failed source result。 @retval value Original error chain を持つ failed
	 * result。
	 * @pre `source.has_value()` は false。 @post Source を変更しない。
	 * @note Message/code/cause/attribute を再構成しない。
	 * @code{.cpp}
	 * #include <cxxlens/core/failure.hpp>
	 * int main(){cxxlens::result<void> a{cxxlens::error{{"core.cancelled"},"x"}};
	 * auto b=cxxlens::propagate_failure<int>(a);return b.error().causes.empty()?0:1;}
	 * @endcode */
	template <class T, class U>
	[[nodiscard]] result<T> propagate_failure(const result<U>& source)
	{
		return result<T>{source.error()};
	}
} // namespace cxxlens
