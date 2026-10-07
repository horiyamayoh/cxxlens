#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "object_interpreter_hooks.hpp"
#include "project_object_facets.hpp"
namespace clang
{
	class ASTContext;
	class Expr;
	class FunctionDecl;
	class NamedDecl;
	class Sema;
} // namespace clang
namespace cxxlens::detail::clang22::object_semantics
{
	struct object_argument_context
	{
		const clang::Expr* invocation{};
		const clang::Expr* argument{};
		unsigned index{};
		bool indeterminate{};
	};
	struct original_sequence_candidate
	{
		const clang::FunctionDecl* owner{};
		const clang::NamedDecl* storage{};
		const clang::Expr* root{};
		const clang::Expr* left{};
		const clang::Expr* right{};
		std::size_t root_ordinal{}, ordinal{}, left_access{}, right_access{};
		std::vector<object_argument_context> left_contexts, right_contexts;
		bool right_modifies{}, unevaluated{};
		std::optional<bool> scalar_nonreference_storage;
		unsigned cplusplus_version{};
	};
	struct original_interpreter_observation
	{
		cxxlens_object_semantics_hook::interpreter_access_view value;
		std::vector<clang::APValue::LValuePathEntry> original_path;
		std::size_t root_ordinal{}, ordinal{};
	};
	/** Borrowed compiler views belong only to this native job, including the
	 * frozen extraction phase. Detachment must finish before AST destruction. */
	struct project_object_observations
	{
		std::vector<original_sequence_candidate> sequence;
		std::vector<original_interpreter_observation> objects;
		std::size_t operations{}, retained_bytes_bound{}, checker_roots{};
		bool hooks_installed{}, sequence_partial{}, object_partial{}, frozen{};
	};
	/** Capability of the genuinely linked original compiler closure. */
	[[nodiscard]] bool original_object_hooks_available() noexcept;
	class project_object_event_scope
	{
		class implementation;
		std::unique_ptr<implementation> state_;

	  public:
		project_object_event_scope(project_object_observations&,
								   object_facet_limits = {},
								   bool instrumented = false);
		~project_object_event_scope();
		project_object_event_scope(const project_object_event_scope&) = delete;
		project_object_event_scope& operator=(const project_object_event_scope&) = delete;
		void freeze() noexcept;
	};
	/** Called from the existing template recorder's root handler, after its
	 * original root ordinal is admitted and before removal. No second root ID or
	 * event stream is synthesized. Unsupported/speculative roots are not eligible. */
	void observe_object_evaluation_root(clang::ASTContext&,
										const void* native,
										std::size_t ordinal,
										bool begin,
										bool constant_context,
										bool potential,
										bool bytecode,
										bool completed) noexcept;
} // namespace cxxlens::detail::clang22::object_semantics
