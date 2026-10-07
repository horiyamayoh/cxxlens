#include "project_object_recorder.hpp"

#include <map>

#include <clang/AST/DeclCXX.h>
#include <clang/Sema/Sema.h>

#include "object_compiler_hooks.hpp"
#include "object_interpreter_hooks.hpp"
namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		struct sink
		{
			project_object_observations& out;
			object_facet_limits limits;
			clang::ASTContext* context{};
			const clang::Expr* checker{};
			std::size_t checker_ordinal{};
			std::vector<object_argument_context> arguments;
			std::vector<std::vector<object_argument_context>> accesses;
			struct root_view
			{
				std::size_t ordinal{};
				bool eligible{};
			};
			std::map<const void*, root_view> roots;
			bool failed{};
			void fail() noexcept
			{
				failed = true;
				out.sequence_partial = out.object_partial = true;
			}
			bool charge(std::size_t work = 1U, std::size_t bytes = 0U)
			{
				if (out.frozen || failed)
					return false;
				if ((limits.cancelled && limits.cancelled()) ||
					out.operations > limits.maximum_operations ||
					work > limits.maximum_operations - out.operations ||
					out.retained_bytes_bound > limits.maximum_retained_bytes ||
					bytes > limits.maximum_retained_bytes - out.retained_bytes_bound)
				{
					fail();
					return false;
				}
				out.operations += work;
				out.retained_bytes_bound += bytes;
				return true;
			}
			bool bind(clang::ASTContext& actual)
			{
				if (context && context != &actual)
				{
					fail();
					return false;
				}
				context = &actual;
				return charge();
			}
			void sequence_root(clang::Sema& sema, const clang::Expr* expression, bool begin)
			{
				if (!out.hooks_installed || !bind(sema.getASTContext()))
					return;
				if (begin)
				{
					if (checker || !expression)
					{
						out.sequence_partial = true;
						return;
					}
					if (out.checker_roots == limits.maximum_fields)
					{
						out.sequence_partial = true;
						return;
					}
					checker = expression;
					checker_ordinal = ++out.checker_roots;
					arguments.clear();
					accesses.clear();
				}
				else
				{
					if (checker != expression || !arguments.empty())
						out.sequence_partial = true;
					checker = nullptr;
					arguments.clear();
					accesses.clear();
				}
			}
			std::uint64_t access(clang::Sema& sema, const clang::Expr*)
			{
				if (!out.hooks_installed || !bind(sema.getASTContext()) || !checker)
					return 0U;
				if (accesses.size() == limits.maximum_fields || arguments.size() > 128U ||
					!charge(1U,
							sizeof(std::vector<object_argument_context>) * 2U +
								arguments.size() * sizeof(object_argument_context) * 2U))
				{
					out.sequence_partial = true;
					return 0U;
				}
				accesses.push_back(arguments);
				return accesses.size();
			}
			void argument(clang::Sema& sema,
						  const clang::Expr* invocation,
						  const clang::Expr* argument,
						  unsigned index,
						  bool indeterminate,
						  bool begin)
			{
				if (!out.hooks_installed || !bind(sema.getASTContext()) || !checker)
					return;
				if (begin)
				{
					if (arguments.size() == 128U ||
						!charge(1U, sizeof(object_argument_context) * 2U))
					{
						out.sequence_partial = true;
						return;
					}
					arguments.push_back({invocation, argument, index, indeterminate});
				}
				else
				{
					if (arguments.empty() || arguments.back().invocation != invocation ||
						arguments.back().argument != argument || arguments.back().index != index)
					{
						out.sequence_partial = true;
						return;
					}
					arguments.pop_back();
				}
			}
			void pair(clang::Sema& sema,
					  const clang::NamedDecl* storage,
					  const clang::Expr* left,
					  const clang::Expr* right,
					  bool right_modifies,
					  std::uint64_t left_access,
					  std::uint64_t right_access)
			{
				if (!out.hooks_installed || !bind(sema.getASTContext()) || !checker)
					return;
				if (out.sequence.size() == limits.maximum_fields || !left_access || !right_access ||
					left_access > accesses.size() || right_access > accesses.size())
				{
					out.sequence_partial = true;
					return;
				}
				const auto& lc = accesses[static_cast<std::size_t>(left_access - 1U)];
				const auto& rc = accesses[static_cast<std::size_t>(right_access - 1U)];
				if (!charge(1U,
							sizeof(original_sequence_candidate) * 2U +
								(lc.size() + rc.size()) * sizeof(object_argument_context) * 2U))
					return;
				original_sequence_candidate value;
				value.owner = sema.getCurFunctionDecl();
				value.storage = storage;
				value.root = checker;
				value.left = left;
				value.right = right;
				value.root_ordinal = checker_ordinal;
				value.ordinal = out.sequence.size();
				value.left_access = static_cast<std::size_t>(left_access);
				value.right_access = static_cast<std::size_t>(right_access);
				value.left_contexts = lc;
				value.right_contexts = rc;
				value.right_modifies = right_modifies;
				value.unevaluated = sema.isUnevaluatedContext();
				if (const auto* var = llvm::dyn_cast_or_null<clang::VarDecl>(storage))
					value.scalar_nonreference_storage =
						var->getType()->isScalarType() && !var->getType()->isReferenceType();
				const auto& lang = sema.getLangOpts();
				value.cplusplus_version = lang.CPlusPlus26 ? 26U
					: lang.CPlusPlus23					   ? 23U
					: lang.CPlusPlus20					   ? 20U
					: lang.CPlusPlus17					   ? 17U
					: lang.CPlusPlus14					   ? 14U
					: lang.CPlusPlus11					   ? 11U
					: lang.CPlusPlus					   ? 98U
														   : 0U;
				out.sequence.push_back(std::move(value));
			}
			void root(clang::ASTContext& actual,
					  const void* native,
					  std::size_t ordinal,
					  bool begin,
					  bool constant,
					  bool potential,
					  bool bytecode,
					  bool completed)
			{
				if (!out.hooks_installed || !bind(actual))
					return;
				if (begin)
				{
					if (!native || roots.contains(native) ||
						roots.size() == limits.maximum_fields ||
						!charge(1U, sizeof(root_view) + sizeof(void*) + 128U))
					{
						out.object_partial = true;
						return;
					}
					roots.emplace(native, root_view{ordinal, constant && !potential && !bytecode});
				}
				else
				{
					const auto at = roots.find(native);
					if (at == roots.end() || at->second.ordinal != ordinal)
					{
						out.object_partial = true;
						return;
					}
					// EvalInfo's constructor precedes callers setting InConstantContext.
					// Final original axes close the root; actual point hooks separately
					// check eligibility at the interpreter access itself.
					const bool eligible = constant && !potential && !bytecode;
					// Completion failure makes only this observed compiler stream partial.
					// Actual retained pointwise witnesses remain inspectable independently.
					if ((eligible || at->second.eligible) && !completed)
						out.object_partial = true;
					roots.erase(at);
				}
			}
			void object(clang::ASTContext& actual,
						const void* native,
						const cxxlens_object_semantics_hook::interpreter_access_view& view)
			{
				if (!out.hooks_installed || !bind(actual))
					return;
				const auto at = roots.find(native);
				if (at == roots.end())
				{
					out.object_partial = true;
					return;
				}
				// The exact compiler hook invokes this only after checking current
				// constant/non-potential/non-speculative/legacy context. A begin-time
				// flag cannot establish that because caller setup follows construction.
				at->second.eligible = true;
				if (out.objects.size() == limits.maximum_fields || view.path.size() > 128U ||
					!charge(view.path.size() + 1U,
							sizeof(original_interpreter_observation) * 2U +
								view.path.size() * sizeof(clang::APValue::LValuePathEntry) * 2U))
				{
					out.object_partial = true;
					return;
				}
				original_interpreter_observation value;
				value.value = view;
				value.original_path.assign(view.path.begin(), view.path.end());
				value.root_ordinal = at->second.ordinal;
				value.ordinal = out.objects.size();
				value.value.path = {};
				out.objects.push_back(std::move(value));
			}
		};
		thread_local sink* active{};
		template <class F>
		void invoke(F&& operation) noexcept
		{
			if (!active)
				return;
			try
			{
				operation(*active);
			}
			catch (...)
			{
				active->fail();
			}
		}
	} // namespace
	class project_object_event_scope::implementation
	{
	  public:
		sink value;
		sink* previous{};
		implementation(project_object_observations& out,
					   object_facet_limits limits,
					   bool instrumented)
			: value{out, std::move(limits), nullptr, nullptr, 0U, {}, {}, {}}
		{
			out.hooks_installed = instrumented;
			previous = active;
			if (previous)
			{
				out.sequence_partial = true;
				out.object_partial = true;
			}
			active = &value;
		}
		void freeze() noexcept
		{
			if (value.checker)
				value.out.sequence_partial = true;
			if (!value.roots.empty())
				value.out.object_partial = true;
			value.out.frozen = true;
			value.checker = nullptr;
			value.arguments.clear();
			value.accesses.clear();
			value.roots.clear();
			if (active == &value)
				active = previous;
		}
	};
	bool original_object_hooks_available() noexcept
	{
#if defined(CXXLENS_CLANG_TEMPLATE_EVENTS) && CXXLENS_CLANG_TEMPLATE_EVENTS
		return cxxlens_object_semantics_hook::sequence_routes() == 0x0fU &&
			cxxlens_object_semantics_hook::interpreter_routes() == 0x3fU;
#else
		return false;
#endif
	}
	project_object_event_scope::project_object_event_scope(project_object_observations& out,
														   object_facet_limits limits,
														   bool instrumented)
	{
		try
		{
			const auto storage = sizeof(implementation) * 2U;
			if (!limits.maximum_operations || out.operations >= limits.maximum_operations ||
				out.retained_bytes_bound > limits.maximum_retained_bytes ||
				storage > limits.maximum_retained_bytes - out.retained_bytes_bound ||
				(limits.cancelled && limits.cancelled()))
			{
				out.sequence_partial = out.object_partial = out.frozen = true;
				return;
			}
			++out.operations;
			out.retained_bytes_bound += storage;
			state_ = std::make_unique<implementation>(out, std::move(limits), instrumented);
		}
		catch (...)
		{
			// This independent observer cannot invalidate original template events.
			out.sequence_partial = out.object_partial = out.frozen = true;
		}
	}
	project_object_event_scope::~project_object_event_scope()
	{
		freeze();
	}
	void project_object_event_scope::freeze() noexcept
	{
		if (state_)
			state_->freeze();
	}
	void observe_object_evaluation_root(clang::ASTContext& c,
										const void* native,
										std::size_t ordinal,
										bool begin,
										bool constant,
										bool potential,
										bool bytecode,
										bool completed) noexcept
	{
		invoke(
			[&](sink& value)
			{
				value.root(c, native, ordinal, begin, constant, potential, bytecode, completed);
			});
	}
	void record_sequence_root(clang::Sema& s, const clang::Expr* e, bool begin) noexcept
	{
		invoke(
			[&](sink& value)
			{
				value.sequence_root(s, e, begin);
			});
	}
	std::uint64_t record_access(clang::Sema& s, const clang::Expr* e) noexcept
	{
		std::uint64_t result{};
		invoke(
			[&](sink& value)
			{
				result = value.access(s, e);
			});
		return result;
	}
	void record_argument(clang::Sema& s,
						 const clang::Expr* i,
						 const clang::Expr* a,
						 unsigned index,
						 bool indeterminate,
						 bool begin) noexcept
	{
		invoke(
			[&](sink& value)
			{
				value.argument(s, i, a, index, indeterminate, begin);
			});
	}
	void record_pair(clang::Sema& s,
					 const clang::NamedDecl* d,
					 const clang::Expr* l,
					 const clang::Expr* r,
					 bool modifies,
					 std::uint64_t li,
					 std::uint64_t ri) noexcept
	{
		invoke(
			[&](sink& value)
			{
				value.pair(s, d, l, r, modifies, li, ri);
			});
	}
	void record_object(clang::ASTContext& c,
					   const void* root,
					   const cxxlens_object_semantics_hook::interpreter_access_view& v) noexcept
	{
		invoke(
			[&](sink& value)
			{
				value.object(c, root, v);
			});
	}
} // namespace cxxlens::detail::clang22::object_semantics
namespace cxxlens_object_semantics_hook
{
	namespace n = cxxlens::detail::clang22::object_semantics;
	void sequence_root(clang::Sema& s, const clang::Expr* e, bool begin)
	{
		n::record_sequence_root(s, e, begin);
	}
	std::uint64_t sequence_access(clang::Sema& s, const clang::Expr* e)
	{
		return n::record_access(s, e);
	}
	void sequence_argument(clang::Sema& s,
						   const clang::Expr* i,
						   const clang::Expr* a,
						   unsigned index,
						   bool indeterminate,
						   bool begin)
	{
		n::record_argument(s, i, a, index, indeterminate, begin);
	}
	void unsequenced(clang::Sema& s,
					 const clang::NamedDecl* d,
					 const clang::Expr* l,
					 const clang::Expr* r,
					 bool modifies,
					 std::uint64_t li,
					 std::uint64_t ri)
	{
		n::record_pair(s, d, l, r, modifies, li, ri);
	}
	void
	interpreter_access(clang::ASTContext& c, const void* root, const interpreter_access_view& v)
	{
		n::record_object(c, root, v);
	}
} // namespace cxxlens_object_semantics_hook
