#include "project_template_events.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <tuple>

#include <clang/AST/APValue.h>
#include <clang/AST/ExprCXX.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Sema/Sema.h>
#include <llvm/ADT/SmallString.h>

#include "project_object_recorder.hpp"
namespace cxxlens_template_hook
{
	unsigned routes_overload();
	unsigned routes_template();
	unsigned routes_instantiation();
} // namespace cxxlens_template_hook
namespace cxxlens_constexpr_hook
{
	unsigned legacy_routes();
} // namespace cxxlens_constexpr_hook
namespace cxxlens::detail::clang22
{
	namespace
	{
		constexpr std::array<std::string_view, 9U> routes{"method_overload",
														  "function_overload",
														  "conversion_overload",
														  "address_target",
														  "single_template_selection",
														  "variable_partial",
														  "explicit_function_specialization",
														  "explicit_function_instantiation",
														  "class_partial"};
		constexpr std::array<std::string_view, 19U> deductions{"success",
															   "invalid",
															   "instantiation_depth",
															   "incomplete",
															   "incomplete_pack",
															   "inconsistent",
															   "underqualified",
															   "substitution_failure",
															   "deduced_mismatch",
															   "deduced_mismatch_nested",
															   "non_deduced_mismatch",
															   "too_many_arguments",
															   "too_few_arguments",
															   "invalid_explicit_arguments",
															   "nondependent_conversion_failure",
															   "constraints_not_satisfied",
															   "miscellaneous_failure",
															   "cuda_target_mismatch",
															   "already_diagnosed"};
		sdk::error invalid(std::string field)
		{
			return {"native.template-event-invalid", std::move(field), "original-compiler-event"};
		}
	} // namespace
	struct project_template_event_scope::implementation
	{
		clang::Sema& sema;
		project_template_event_observations& out;
		template_event_limits limits;
		static thread_local implementation* active;
		std::optional<sdk::error> failure;
		struct admission
		{
			const clang::NamedDecl* declaration;
			unsigned source, route;
			std::size_t event;
		};
		std::vector<admission> pending;
		std::map<const void*, std::size_t> roots;
		// Borrowed identities exist only during this parser job. They are erased on
		// freeze; only ordinal/source/USR/context values reach detached output.
		using path_key = std::vector<std::tuple<unsigned, std::uintptr_t, std::uint64_t, bool>>;
		using site_key = std::tuple<const clang::Expr*,
									const clang::Decl*,
									const clang::Decl*,
									const clang::Expr*,
									unsigned,
									unsigned,
									unsigned,
									path_key>;
		std::map<const clang::Decl*, std::string> usr_cache;
		std::map<site_key, std::size_t> sites;
		implementation(clang::Sema& s,
					   project_template_event_observations& o,
					   template_event_limits l)
			: sema(s), out(o), limits(std::move(l))
		{
			if (active || !limits.maximum_operations || !limits.maximum_events ||
				!limits.maximum_retained_bytes || out.hooks_installed)
				throw invalid("phase-or-limits");
			work();
#if defined(CXXLENS_CLANG_TEMPLATE_EVENTS) && CXXLENS_CLANG_TEMPLATE_EVENTS
			out.hooks_installed = (cxxlens_template_hook::routes_overload() |
								   cxxlens_template_hook::routes_template() |
								   cxxlens_template_hook::routes_instantiation()) == 0x1ffU;
			if (!out.hooks_installed || cxxlens_constexpr_hook::legacy_routes() != 0x7fU)
				throw invalid("compiler-route-installation");
			active = this;
#else
			out.candidate_reason = "original_template_event_hooks_unavailable";
			out.evaluation_reason = "original_template_event_hooks_unavailable";
#endif
		}
		~implementation()
		{
			if (active == this)
				active = nullptr;
		}
		void work(std::size_t n = 1U)
		{
			if (limits.cancelled && limits.cancelled())
				throw sdk::error{"native.template-event-cancelled", "events", "cancelled"};
			if (out.operations > limits.maximum_operations ||
				n > limits.maximum_operations - out.operations)
				throw sdk::error{"native.template-event-budget", "operations", "limit"};
			out.operations += n;
		}
		void retain(std::size_t n)
		{
			work();
			if (out.retained_bytes > limits.maximum_retained_bytes ||
				n > limits.maximum_retained_bytes - out.retained_bytes)
				throw sdk::error{"native.template-event-budget", "retained-bytes", "limit"};
			out.retained_bytes += n;
		}
		void member(std::size_t n)
		{
			work();
			if (n >= limits.maximum_events)
				throw sdk::error{"native.template-event-budget", "events", "limit"};
		}
		void event()
		{
			work();
			std::size_t remaining = limits.maximum_events;
			for (auto count : {out.candidates.size(), out.roots.size(), out.calls.size()})
			{
				if (count > remaining)
					throw sdk::error{"native.template-event-budget", "events", "limit"};
				remaining -= count;
			}
			if (!remaining)
				throw sdk::error{"native.template-event-budget", "events", "limit"};
		}
		std::string string(std::string_view s)
		{
			retain(s.size() + sizeof(std::string));
			return std::string{s};
		}
		const std::string& usr(const clang::Decl* d)
		{
			static const std::string empty;
			if (!d)
				return empty;
			work();
			d = d->getCanonicalDecl();
			if (auto i = usr_cache.find(d); i != usr_cache.end())
				return i->second;
			member(usr_cache.size());
			retain(sizeof(std::pair<const clang::Decl* const, std::string>) + 128U);
			llvm::SmallString<128> value;
			if (clang::index::generateUSRForDecl(d, value))
				return empty;
			auto encoded = string(value.str());
			return usr_cache.emplace(d, std::move(encoded)).first->second;
		}
		std::optional<bool> source_is_system(clang::SourceLocation s)
		{
			work();
			if (s.isInvalid())
				return std::nullopt;
			auto& sm = sema.getSourceManager();
			const auto expansion = sm.getExpansionLoc(s);
			if (expansion.isInvalid())
				return std::nullopt;
			return sm.isInSystemHeader(expansion);
		}
		void context(const clang::ASTContext& c)
		{
			work();
			if (&c != &sema.getASTContext())
				throw invalid("foreign-compiler-context");
		}
		void
		admit(clang::Sema& s, const clang::NamedDecl* d, clang::SourceLocation loc, unsigned kind)
		{
			context(s.getASTContext());
			if (out.frozen || kind >= routes.size() || !d)
				throw invalid("candidate-admission");
			event();
			retain(sizeof(template_candidate_event) + sizeof(admission) + 256U);
			template_candidate_event e;
			e.source = {loc.getRawEncoding(), loc.getRawEncoding()};
			e.candidate_usr = string(usr(d));
			e.route = string(routes[kind]);
			e.is_system = source_is_system(loc);
			e.ordinal = out.candidates.size();
			e.binding_complete =
				!e.candidate_usr.empty() && loc.isValid() && e.is_system.has_value();
			pending.push_back({d, loc.getRawEncoding(), kind, e.ordinal});
			out.candidates.push_back(std::move(e));
		}
		void candidate(clang::Sema& s,
					   const clang::NamedDecl* d,
					   clang::SourceLocation loc,
					   unsigned kind,
					   unsigned result,
					   unsigned original_failure,
					   bool success)
		{
			context(s.getASTContext());
			if (out.frozen || pending.empty() || result >= deductions.size())
				throw invalid("candidate-result");
			const auto previous = pending.back();
			if (previous.declaration != d || previous.source != loc.getRawEncoding() ||
				previous.route != kind || (result == 0U) != success)
				throw invalid("candidate-admission-result-differs");
			pending.pop_back();
			auto& e = out.candidates[previous.event];
			e.completed = true;
			e.deduction_result = string(deductions[result]);
			e.disposition = string(success			   ? "deduction_success"
									   : result == 7U  ? "substitution_exclusion"
									   : result == 15U ? "constraint_exclusion"
													   : "other_exclusion");
			if (kind <= 2U && !success)
				e.overload_failure = original_failure;
		}
		void root(clang::ASTContext& c,
				  const void* native,
				  unsigned mode,
				  bool begin,
				  bool constant,
				  bool potential,
				  bool bytecode,
				  bool fold_failed)
		{
			context(c);
			if (out.frozen || !native || mode > 3U)
				throw invalid("evaluation-root");
			if (begin)
			{
				event();
				retain(sizeof(constant_evaluation_root) + 128U);
				const auto id = out.roots.size();
				if (!roots.emplace(native, id).second)
					throw invalid("duplicate-active-evaluation-root");
				out.roots.push_back({mode, false, constant, potential, bytecode, fold_failed, {}});
				object_semantics::observe_object_evaluation_root(
					c, native, id, true, constant, potential, bytecode, false);
			}
			else
			{
				auto i = roots.find(native);
				if (i == roots.end())
					throw invalid("missing-evaluation-root");
				auto& r = out.roots[i->second];
				if (r.mode != mode)
					throw invalid("evaluation-mode-differs");
				r.completed = true;
				r.constant_context = constant;
				r.potential_check = potential;
				r.bytecode = bytecode;
				r.fold_failure = fold_failed;
				object_semantics::observe_object_evaluation_root(
					c, native, i->second, false, constant, potential, bytecode, !fold_failed);
				roots.erase(i);
			}
		}
		std::pair<path_key, std::vector<constant_lifetime_step>>
		lifetime_path(clang::QualType type, llvm::ArrayRef<clang::APValue::LValuePathEntry> entries)
		{
			path_key identities;
			std::vector<constant_lifetime_step> detached;
			type = type.getNonReferenceType();
			for (auto entry : entries)
			{
				member(identities.size());
				retain(2U * sizeof(path_key::value_type) + sizeof(constant_lifetime_step));
				if (type.isNull())
					throw invalid("lifetime-path-type-missing");
				constant_lifetime_step value;
				unsigned kind{};
				const clang::Decl* member_decl{};
				if (const auto* array = sema.getASTContext().getAsArrayType(type))
				{
					kind = 0U;
					value.kind = string("array");
					value.index = entry.getAsArrayIndex();
					type = array->getElementType();
				}
				else if (const auto* complex = type->getAs<clang::ComplexType>())
				{
					kind = 1U;
					value.kind = string("complex");
					value.index = entry.getAsArrayIndex();
					type = complex->getElementType();
				}
				else if (const auto* vector = type->getAs<clang::VectorType>())
				{
					kind = 2U;
					value.kind = string("vector");
					value.index = entry.getAsArrayIndex();
					type = vector->getElementType();
				}
				else
				{
					const auto original = entry.getAsBaseOrMember();
					member_decl = original.getPointer();
					if (const auto* field = llvm::dyn_cast_or_null<clang::FieldDecl>(member_decl))
					{
						kind = 3U;
						value.kind = string("field");
						value.member_usr = string(usr(field));
						type = field->getType();
					}
					else if (const auto* base =
								 llvm::dyn_cast_or_null<clang::CXXRecordDecl>(member_decl))
					{
						kind = 4U;
						value.kind = string("base");
						value.member_usr = string(usr(base));
						value.virtual_base = original.getInt();
						type = sema.getASTContext().getCanonicalTagType(base);
					}
					else
						throw invalid("lifetime-path-member-invalid");
				}
				identities.emplace_back(kind,
										reinterpret_cast<std::uintptr_t>(member_decl),
										value.index,
										value.virtual_base);
				detached.push_back(std::move(value));
			}
			return {std::move(identities), std::move(detached)};
		}
		void reached(clang::ASTContext& c,
					 const void* native,
					 const clang::FunctionDecl* callee,
					 const clang::Expr* expression,
					 clang::SourceRange source,
					 unsigned depth,
					 unsigned kind,
					 const clang::FunctionDecl* owner,
					 const clang::ValueDecl* lifetime_decl,
					 const clang::Expr* lifetime_expr,
					 clang::QualType lifetime_type,
					 llvm::ArrayRef<clang::APValue::LValuePathEntry> original_path)
		{
			context(c);
			if (out.frozen || kind > 6U)
				throw invalid("evaluation-call");
			const auto r = roots.find(native);
			if (r == roots.end())
				throw invalid("call-root-missing");
			const auto* enclosing = owner ? owner->getCanonicalDecl() : nullptr;
			const auto* origin = lifetime_decl ? lifetime_decl->getCanonicalDecl() : nullptr;
			// A loop/retry preserves one instantiated AST occurrence. Only an implicit
			// invocation has no expression, so its actual lifetime and typed designator
			// distinguish sibling fields/bases/array entries. No dynamic receiver
			// guess.
			auto paths = expression ? std::pair<path_key, std::vector<constant_lifetime_step>>{}
									: lifetime_path(lifetime_type, original_path);
			site_key key{expression,
						 enclosing,
						 expression ? nullptr : origin,
						 expression ? nullptr : lifetime_expr,
						 kind,
						 expression ? 0U : source.getBegin().getRawEncoding(),
						 expression ? 0U : source.getEnd().getRawEncoding(),
						 std::move(paths.first)};
			if (!expression && !origin && !lifetime_expr)
				throw invalid("implicit-call-origin-missing");
			auto i = sites.find(key);
			std::size_t id{};
			if (i == sites.end())
			{
				event();
				retain(sizeof(constant_evaluated_occurrence) + sizeof(site_key) + 256U);
				id = out.calls.size();
				constant_evaluated_occurrence e;
				e.source = {source.getBegin().getRawEncoding(), source.getEnd().getRawEncoding()};
				e.owner_usr = string(usr(owner));
				if (!expression)
				{
					e.lifetime_usr = string(usr(lifetime_decl));
					e.lifetime_path = std::move(paths.second);
					if (lifetime_decl)
						e.lifetime_source = {lifetime_decl->getBeginLoc().getRawEncoding(),
											 lifetime_decl->getEndLoc().getRawEncoding(),
											 template_event_source_role::declaration};
					else if (lifetime_expr)
						e.lifetime_source = {lifetime_expr->getBeginLoc().getRawEncoding(),
											 lifetime_expr->getEndLoc().getRawEncoding()};
				}
				else
					e.expression_kind = string(expression->getStmtClassName());
				e.is_system = source_is_system(source.getBegin());
				e.kind = kind;
				e.depth = depth;
				e.ordinal = id;
				e.binding_complete = source.isValid() && e.is_system.has_value();
				for (const auto& step : e.lifetime_path)
					if ((step.kind == "field" || step.kind == "base") && step.member_usr.empty())
						e.binding_complete = false;
				out.calls.push_back(std::move(e));
				sites.emplace(std::move(key), id);
			}
			else
			{
				id = i->second;
				out.calls[id].depth = std::max(out.calls[id].depth, depth);
			}
			auto& call = out.calls[id];
			// Target alternatives are product data independent from finite occurrence
			// membership. Never keep the first target and discard a genuine
			// alternative.
			const auto& target = usr(callee);
			if (!target.empty())
			{
				work(call.callee_usrs.size() + 1U);
				if (std::ranges::find(call.callee_usrs, target) == call.callee_usrs.end())
				{
					member(call.callee_usrs.size());
					call.callee_usrs.push_back(string(target));
					std::ranges::sort(call.callee_usrs);
				}
			}
			else if (kind != 6U)
				call.binding_complete = false;
			auto& roots_for_site = call.roots;
			auto& calls_for_root = out.roots[r->second].calls;
			work(roots_for_site.size() + calls_for_root.size() + 1U);
			if (std::ranges::find(roots_for_site, r->second) == roots_for_site.end())
			{
				member(roots_for_site.size());
				retain(2U * sizeof(std::size_t));
				roots_for_site.push_back(r->second);
				calls_for_root.push_back(id);
			}
		}
	};
	thread_local project_template_event_scope::implementation*
		project_template_event_scope::implementation::active{};
	project_template_event_scope::project_template_event_scope(
		clang::Sema& s, project_template_event_observations& o, template_event_limits l)
		: state_(std::make_unique<implementation>(s, o, std::move(l)))
	{
	}
	project_template_event_scope::~project_template_event_scope() = default;
	sdk::result<void> project_template_event_scope::freeze(bool parser_completed, bool fatal)
	{
		try
		{
			auto& s = *state_;
			if (s.failure)
			{
				s.out.frozen = true;
				s.out.candidate_reason = s.failure->code;
				s.out.evaluation_reason = s.failure->code;
				s.pending.clear();
				s.roots.clear();
				s.sites.clear();
				s.usr_cache.clear();
				return *s.failure;
			}
			s.work();
			if (s.out.frozen)
				throw invalid("duplicate-freeze");
			s.out.frozen = true;
			s.out.candidate_census_complete =
				s.out.hooks_installed && parser_completed && !fatal && s.pending.empty();
			s.out.evaluation_census_complete = s.out.hooks_installed && parser_completed &&
				!fatal && s.roots.empty() && !s.sema.getLangOpts().EnableNewConstInterp;
			bool failed_constant_root{};
			for (const auto& root : s.out.roots)
			{
				s.work();
				if (root.completed && root.constant_context && !root.potential_check &&
					root.fold_failure)
					failed_constant_root = true;
			}
			if (failed_constant_root)
				s.out.evaluation_census_complete = false;
			if (!s.out.candidate_census_complete)
				s.out.candidate_reason = s.out.hooks_installed
					? "original_candidate_phase_incomplete"
					: "original_template_event_hooks_unavailable";
			if (!s.out.evaluation_census_complete)
				s.out.evaluation_reason = !s.out.hooks_installed
					? "original_template_event_hooks_unavailable"
					: s.sema.getLangOpts().EnableNewConstInterp
					? "bytecode_interpreter_uninstrumented"
					: failed_constant_root ? "original_qualifying_evaluation_failed"
										   : "original_evaluation_phase_incomplete";
			s.pending.clear();
			s.roots.clear();
			s.sites.clear();
			s.usr_cache.clear();
			if (implementation::active == &s)
				implementation::active = nullptr;
			return {};
		}
		catch (const sdk::error& e)
		{
			auto& s = *state_;
			s.out.frozen = true;
			s.out.candidate_census_complete = false;
			s.out.evaluation_census_complete = false;
			s.out.candidate_reason = e.code;
			s.out.evaluation_reason = e.code;
			s.pending.clear();
			s.roots.clear();
			s.sites.clear();
			s.usr_cache.clear();
			if (implementation::active == &s)
				implementation::active = nullptr;
			return e;
		}
	}
} // namespace cxxlens::detail::clang22
namespace
{
	template <class Callback>
	void capture_template_event(Callback&& callback) noexcept
	{
		using sink = cxxlens::detail::clang22::project_template_event_scope::implementation;
		auto* state = sink::active;
		if (!state)
			return;
		try
		{
			callback(*state);
		}
		catch (const cxxlens::sdk::error& failure)
		{
			state->failure = failure;
			sink::active = nullptr;
		}
		catch (...)
		{
			state->failure = cxxlens::sdk::error{
				"native.template-event-failed", "callback", "original-compiler-event"};
			sink::active = nullptr;
		}
	}
} // namespace
namespace cxxlens_template_hook
{
	void admit(clang::Sema& s, const clang::NamedDecl* d, clang::SourceLocation l, unsigned k)
	{
		capture_template_event(
			[&](auto& state)
			{
				state.admit(s, d, l, k);
			});
	}
	void candidate(clang::Sema& s,
				   const clang::NamedDecl* d,
				   clang::SourceLocation l,
				   unsigned k,
				   unsigned r,
				   unsigned f,
				   bool success)
	{
		capture_template_event(
			[&](auto& state)
			{
				state.candidate(s, d, l, k, r, f, success);
			});
	}
} // namespace cxxlens_template_hook
namespace cxxlens_constexpr_hook
{
	void
	root(clang::ASTContext& c, const void* n, unsigned m, bool b, bool cc, bool p, bool bc, bool f)
	{
		capture_template_event(
			[&](auto& state)
			{
				state.root(c, n, m, b, cc, p, bc, f);
			});
	}
	void reached(clang::ASTContext& c,
				 const void* n,
				 const clang::FunctionDecl* d,
				 const clang::Expr* e,
				 clang::SourceRange r,
				 unsigned depth,
				 unsigned k,
				 const clang::FunctionDecl* owner,
				 const clang::ValueDecl* ld,
				 const clang::Expr* le,
				 clang::QualType type,
				 llvm::ArrayRef<clang::APValue::LValuePathEntry> path)
	{
		capture_template_event(
			[&](auto& state)
			{
				state.reached(c, n, d, e, r, depth, k, owner, ld, le, type, path);
			});
	}
} // namespace cxxlens_constexpr_hook
