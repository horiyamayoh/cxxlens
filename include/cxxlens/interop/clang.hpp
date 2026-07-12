#pragma once

/** @file clang.hpp @brief Explicit, LLVM-major-scoped borrowed Clang interop corridor. */

#include <cstdint>
#include <functional>
#include <string>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/workspace.hpp>

namespace clang
{
	class ASTContext;
	class CompilerInstance;
	class LangOptions;
	class Preprocessor;
	class SourceManager;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct borrowed_tu_access;
} // namespace cxxlens::detail::clang22

namespace cxxlens::interop
{
	/** @brief Runtime version of the Clang libraries linked into the adapter. */
	struct clang_api_version
	{
		/** @brief Linked LLVM major, or zero when the adapter is unavailable. */
		std::uint32_t llvm_major{};
		/** @brief Linked LLVM minor, or zero when the adapter is unavailable. */
		std::uint32_t llvm_minor{};
		/** @brief Linked LLVM patch, or zero when the adapter is unavailable. */
		std::uint32_t llvm_patch{};
		/** @brief Build revision, or `unavailable` when no Clang adapter is linked. */
		std::string clang_revision;
	};

	/** @brief Thread-confined non-owning access to one live Clang frontend job.
	 *
	 * The object and every reference returned from it are valid only during the callback supplied
	 * to `with_translation_unit`, on the callback's owning thread. Nothing obtained here may be
	 * stored, moved to another thread, or retained across coroutine suspension.
	 */
	class borrowed_clang_tu
	{
	  public:
		/** @brief Disallow copying the callback-scoped borrow.
		 * @param[in] other Another borrow. @pre None. @post Construction is ill-formed.
		 * @note Native frontend state cannot be duplicated or retained.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * #include <type_traits>
		 * int
		 * main(){static_assert(!std::is_copy_constructible_v<cxxlens::interop::borrowed_clang_tu>);}
		 * @endcode */
		borrowed_clang_tu(const borrowed_clang_tu& other) = delete;
		/** @brief Disallow assigning the callback-scoped borrow.
		 * @param[in] other Another borrow. @retval value No value; assignment is deleted.
		 * @pre None. @post Assignment is ill-formed. @note This prevents borrow escape by copying.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * #include <type_traits>
		 * int
		 * main(){static_assert(!std::is_copy_assignable_v<cxxlens::interop::borrowed_clang_tu>);}
		 * @endcode */
		borrowed_clang_tu& operator=(const borrowed_clang_tu& other) = delete;

		/** @brief Access the live compiler instance.
		 * @retval value Borrowed compiler reference. @pre Called inside the owning callback thread.
		 * @post No ownership is transferred. @note A debug lifetime violation terminates
		 * immediately.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * #include <type_traits>
		 * int main(){using
		 * view=cxxlens::interop::borrowed_clang_tu;static_assert(!std::is_copy_constructible_v<view>);}
		 * @endcode */
		[[nodiscard]] clang::CompilerInstance& compiler() const noexcept;
		/** @brief Access the live AST context.
		 * @retval value Borrowed AST context. @pre Called inside the owning callback thread.
		 * @post No ownership is transferred. @note Never persist its address.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] clang::ASTContext& ast_context() const noexcept;
		/** @brief Access the live source manager.
		 * @retval value Borrowed source manager. @pre Called inside the owning callback thread.
		 * @post No ownership is transferred. @note Normalize locations before returning.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] clang::SourceManager& source_manager() const noexcept;
		/** @brief Access the live preprocessor.
		 * @retval value Borrowed preprocessor. @pre Called inside the owning callback thread.
		 * @post No ownership is transferred. @note Never retain macro or token pointers.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] clang::Preprocessor& preprocessor() const noexcept;
		/** @brief Access the live language options.
		 * @retval value Borrowed language options. @pre Called inside the owning callback thread.
		 * @post No ownership is transferred. @note Copy stable scalar values when needed later.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const clang::LangOptions& language_options() const noexcept;
		/** @brief Access the detached compile-unit value for the current job.
		 * @retval value Borrowed compile-unit reference. @pre Called inside the owning callback
		 * thread.
		 * @post No state changes. @note The compile unit itself is LLVM-free and value-owned.
		 * @code{.cpp}
		 * #include <cxxlens/interop/clang.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const compile_unit& unit() const noexcept;

	  private:
		struct state;
		explicit borrowed_clang_tu(state* value) noexcept : state_{value} {}
		void require_live() const noexcept;
		state* state_{};
		friend struct cxxlens::detail::clang22::borrowed_tu_access;
	};

	/** @brief Move-only synchronous callback for one borrowed translation unit. */
	using clang_tu_callback = std::move_only_function<result<void>(borrowed_clang_tu&)>;

	/** @brief Report the actually linked Clang adapter version.
	 * @retval value Exact linked version, or zeroes with `unavailable` when the optional adapter
	 * was not built. @pre None. @post No state changes. @note Never reports the design baseline as
	 * if it were linked reality.
	 * @code{.cpp}
	 * #include <cxxlens/interop/clang.hpp>
	 * int main(){const auto v=cxxlens::interop::linked_clang_version();return
	 * v.llvm_major==0U||v.llvm_major==22U?0:1;}
	 * @endcode */
	[[nodiscard]] clang_api_version linked_clang_version() noexcept;

	/** @brief Parse one catalogued compile unit in a fresh Clang context and invoke a borrowed
	 * callback.
	 * @param[in] workspace Catalog and immutable input snapshot. @param[in] unit Exact compile-unit
	 * ID.
	 * @param[in] callback Move-only synchronous callback. @param[in] context Cancellation and
	 * deadline.
	 * @retval value Success only after parsing and callback completion; otherwise a structured
	 * error.
	 * @pre `unit` belongs to `workspace`; callback does not suspend or let borrowed state escape.
	 * @post All Clang objects are destroyed before return and no native pointer enters persistent
	 * data.
	 * @note Missing Clang 22 is `core.capability-unavailable`, never a silent adjacent-major
	 * fallback.
	 * @code{.cpp}
	 * #include <cxxlens/interop/clang.hpp>
	 * int main(){return cxxlens::interop::linked_clang_version().llvm_major==0U?0:0;}
	 * @endcode */
	[[nodiscard]] result<void> with_translation_unit(workspace& workspace,
													 compile_unit_id unit,
													 clang_tu_callback callback,
													 execution_context context = {});
} // namespace cxxlens::interop
