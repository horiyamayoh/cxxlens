#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <clang/AST/Expr.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Tooling/Tooling.h>

#include "project_object_recorder.hpp"

namespace native = cxxlens::detail::clang22::object_semantics;
namespace hook = cxxlens_object_semantics_hook;

namespace
{
	void require(bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	constexpr std::string_view source = R"cpp(
void orders(_Atomic(int)* pointer, int dynamic) {
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_RELAXED);
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_CONSUME);
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_ACQUIRE);
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_RELEASE);
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_ACQ_REL);
  __c11_atomic_fetch_add(pointer, 1, __ATOMIC_SEQ_CST);
  __c11_atomic_load(pointer, dynamic);
  __c11_atomic_load(pointer, __ATOMIC_RELEASE);
  __c11_atomic_init(pointer, 1);
}
)cpp";
	std::unique_ptr<clang::ASTUnit> observe(native::project_object_observations& observations,
											native::object_facet_limits limits = {},
											bool installed = true)
	{
		native::project_object_event_scope scope{observations, limits, false, installed};
		auto ast = clang::tooling::buildASTFromCodeWithArgs(
			std::string{source}, {"-std=c++23", "-Wno-atomic-memory-ordering"}, "atomic-sema.cpp");
		require(bool(ast), "original atomic AST unavailable");
		scope.freeze();
		return ast;
	}
	std::unique_ptr<clang::ASTUnit> observe_fences(native::project_object_observations& out,
												   native::object_facet_limits limits = {},
												   bool installed = true)
	{
		native::project_object_event_scope scope{out, limits, false, false, installed};
		auto ast = clang::tooling::buildASTFromCodeWithArgs(R"cpp(
void fence_sites(int order) {
 __atomic_thread_fence(5); __atomic_signal_fence(order);
 __c11_atomic_thread_fence(0); __c11_atomic_signal_fence(-1);
 __scoped_atomic_thread_fence(2, __MEMORY_SCOPE_SYSTEM);
}
void discarded_fence() { if constexpr(false) __atomic_thread_fence(5); }
int unevaluated_fence() { return sizeof((__atomic_signal_fence(5), 0)); }
int global_fence = (__atomic_thread_fence(5), 0);
void default_fence(int = (__atomic_signal_fence(5), 0));
)cpp",
															{"-std=c++23"},
															"fence-sema.cpp");
		require(bool(ast), "original fence AST unavailable");
		scope.freeze();
		return ast;
	}

} // namespace

int main()
{
	unsigned cases{};
	{
		native::project_object_observations stock;
		auto ast = observe(stock, {}, false);
		require(stock.frozen && !stock.atomic_hooks_installed && stock.atomics.empty(),
				"stock route fabricated original observations");
		++cases;
	}
	if (!native::original_atomic_hooks_available())
	{
		native::project_object_observations stock;
		auto ast = observe(stock);
		require(!stock.atomic_hooks_installed && stock.atomics.empty(),
				"unlinked compiler route reported available");
		std::cout << cases + 1U << " stock atomic route controls PASS\n";
		return 0;
	}
	std::size_t work{}, bytes{};
	{
		native::project_object_observations actual;
		auto ast = observe(actual);
		require(actual.frozen && actual.atomic_hooks_installed && !actual.atomic_partial &&
					actual.atomics.size() == 9U,
				"original atomic callback population changed");
		constexpr std::array classes{hook::atomic_order_class::relaxed,
									 hook::atomic_order_class::consume,
									 hook::atomic_order_class::acquire,
									 hook::atomic_order_class::release,
									 hook::atomic_order_class::acq_rel,
									 hook::atomic_order_class::seq_cst};
		for (std::size_t i{}; i < classes.size(); ++i)
		{
			const auto& original = actual.atomics[i];
			require(
				original.expression &&
					original.expression->getOp() == clang::AtomicExpr::AO__c11_atomic_fetch_add &&
					original.value.operation == hook::atomic_operation_kind::rmw &&
					original.value.success.kind == classes[i] &&
					original.value.success.validation == hook::atomic_validation::valid &&
					original.value.failure.validation == hook::atomic_validation::not_applicable &&
					original.evaluation_context == "potentially_evaluated" && !original.discarded,
				"actual compiler order/context observation changed");
		}
		require(actual.atomics[6].value.success.validation == hook::atomic_validation::unknown &&
					actual.atomics[6].value.success.kind == hook::atomic_order_class::unknown,
				"nonconstant original order promoted");
		require(actual.atomics[7].value.success.validation == hook::atomic_validation::invalid &&
					actual.atomics[7].value.success.kind == hook::atomic_order_class::unknown,
				"invalid original load order promoted");
		require(actual.atomics[8].value.operation == hook::atomic_operation_kind::init &&
					actual.atomics[8].value.success.validation ==
						hook::atomic_validation::not_applicable,
				"orderless initialization fabricated an order");
		work = actual.operations;
		bytes = actual.retained_bytes_bound;
		++cases;
	}
	{
		native::object_facet_limits exact;
		exact.maximum_operations = work;
		exact.maximum_retained_bytes = bytes;
		native::project_object_observations repeated;
		auto ast = observe(repeated, exact);
		require(!repeated.atomic_partial && repeated.atomics.size() == 9U &&
					repeated.operations == work && repeated.retained_bytes_bound == bytes,
				"exact recorder bounds or deterministic charges changed");
		++cases;
	}
	for (const bool reduce_work : {false, true})
	{
		native::object_facet_limits below;
		below.maximum_operations = work - (reduce_work ? 1U : 0U);
		below.maximum_retained_bytes = bytes - (reduce_work ? 0U : 1U);
		native::project_object_observations bounded;
		auto ast = observe(bounded, below);
		require(bounded.frozen && bounded.atomic_partial && bounded.atomics.size() < 9U &&
					bounded.operations <= below.maximum_operations &&
					bounded.retained_bytes_bound <= below.maximum_retained_bytes,
				"recorder exceeded a charged bound or hid a dropped member");
		++cases;
	}
	{
		native::object_facet_limits one;
		one.maximum_fields = 1U;
		native::project_object_observations bounded;
		auto ast = observe(bounded, one);
		require(bounded.atomic_partial && bounded.atomics.size() == 1U,
				"original atomic field cap fabricated empty closure");
		++cases;
	}
	{
		native::object_facet_limits cancelled;
		cancelled.cancelled = []
		{
			return true;
		};
		native::project_object_observations bounded;
		auto ast = observe(bounded, cancelled);
		require(bounded.frozen && bounded.atomic_partial && bounded.atomics.empty(),
				"cancelled recorder fabricated complete empty observations");
		++cases;
	}

	{
		native::project_object_observations stock;
		auto ast = observe_fences(stock, {}, false);
		require(stock.frozen && !stock.fence_hooks_installed && stock.fences.empty(),
				"stock route fabricated original fence observations");
		++cases;
	}
	if (native::original_fence_hooks_available())
	{
		native::project_object_observations actual;
		auto ast = observe_fences(actual);
		require(actual.frozen && actual.fence_hooks_installed && !actual.fence_partial &&
					actual.fences.size() == 9U,
				"original fence callback population changed");
		for (const auto& original : actual.fences)
			require(original.expression && original.callee && original.builtin_id &&
						original.expression->getBuiltinCallee() == original.builtin_id &&
						original.expression->getDirectCallee() == original.callee,
					"original fence callee/identity association changed");
		require(actual.fences[5].discarded &&
					actual.fences[5].evaluation_context == "discarded_statement" &&
					actual.fences[6].evaluation_context == "unevaluated" &&
					actual.fences[8].default_context,
				"original fence evaluation context changed");
		work = actual.operations;
		bytes = actual.retained_bytes_bound;
		++cases;
		native::object_facet_limits exact;
		exact.maximum_operations = work;
		exact.maximum_retained_bytes = bytes;
		native::project_object_observations repeated;
		auto repeat_ast = observe_fences(repeated, exact);
		require(!repeated.fence_partial && repeated.fences.size() == 9U &&
					repeated.operations == work && repeated.retained_bytes_bound == bytes,
				"exact original fence recorder bounds changed");
		++cases;
		for (const bool reduce_work : {false, true})
		{
			native::object_facet_limits below;
			below.maximum_operations = work - (reduce_work ? 1U : 0U);
			below.maximum_retained_bytes = bytes - (reduce_work ? 0U : 1U);
			native::project_object_observations bounded;
			auto bounded_ast = observe_fences(bounded, below);
			require(bounded.frozen && bounded.fence_partial && bounded.fences.size() < 9U &&
						bounded.operations <= below.maximum_operations &&
						bounded.retained_bytes_bound <= below.maximum_retained_bytes,
					"fence recorder exceeded a bound or hid a dropped candidate");
			++cases;
		}
		native::object_facet_limits one;
		one.maximum_fields = 1U;
		native::project_object_observations bounded;
		auto bounded_ast = observe_fences(bounded, one);
		require(bounded.fence_partial && bounded.fences.size() == 1U,
				"fence field cap fabricated empty closure");
		++cases;
		native::object_facet_limits cancelled;
		cancelled.cancelled = []
		{
			return true;
		};
		native::project_object_observations stopped;
		auto stopped_ast = observe_fences(stopped, cancelled);
		require(stopped.frozen && stopped.fence_partial && stopped.fences.empty(),
				"cancelled fence recorder fabricated closure");
		++cases;
	}
	std::cout << cases << " original atomic recorder controls PASS\n";
}
