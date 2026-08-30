#include "llvm/clang22/provider_worker_v4_ast_observer.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/provider_task_v4.hpp"

#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
namespace allocation_fault_test
{
	std::atomic<std::size_t> failed_allocation_size{};

	[[nodiscard]] bool should_fail(const std::size_t size) noexcept
	{
		auto expected = size;
		return failed_allocation_size.compare_exchange_strong(
			expected, 0U, std::memory_order_relaxed);
	}

	void arm(const std::size_t size) noexcept
	{
		failed_allocation_size.store(size, std::memory_order_relaxed);
	}

	void disarm() noexcept
	{
		failed_allocation_size.store(0U, std::memory_order_relaxed);
	}
} // namespace allocation_fault_test

void* operator new(const std::size_t size)
{
	if (allocation_fault_test::should_fail(size))
		throw std::bad_alloc{};
	if (auto* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr)
		return memory;
	throw std::bad_alloc{};
}

void* operator new[](const std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* allocation) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
	::operator delete(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
	::operator delete(allocation);
}
#endif

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::vector<std::string>
	arguments(const std::string_view logical_path = "project://src/main.cpp")
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			std::string{logical_path},
		};
#else
		return {"/usr/bin/clang++",
				"-std=c++23",
				"-nostdinc",
				"-nostdinc++",
				std::string{logical_path}};
#endif
	}

	struct fixture
	{
		source_closure_task_v4_decoded metadata;
		std::string source;
		std::string file_id;
		std::vector<std::string> effective_arguments;
	};

	[[nodiscard]] fixture
	make_fixture(std::string source = "int leaf(int value) { return value + 1; }\n"
									  "int main() { return leaf(41); }\n",
				 const std::string_view logical_path = "project://src/main.cpp")
	{
		fixture output;
		output.source = std::move(source);
		auto closure = make_source_closure_snapshot({
			{std::string{logical_path},
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content(output.source)},
		});
		require(closure.has_value(), "ast observer closure fixture was rejected");
		output.file_id = closure->members.front().file_id;
		output.effective_arguments = arguments(logical_path);
		source_closure_task_v4_input input;
		input.base_task_index = 0U;
		input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		input.task_input_digest = "sha256:" + std::string(64U, '2');
		input.logical_working_directory = "project://src";
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, output.effective_arguments);
		require(invocation.has_value(), "ast observer invocation digest was rejected");
		input.normalized_invocation_digest = std::move(*invocation);
		input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		input.environment_digest = "sha256:" + std::string(64U, '5');
		input.closure = std::move(*closure);
		input.main_logical_path = std::string{logical_path};
		auto identity = derive_source_closure_task_v4_identity(input);
		require(identity.has_value(), "ast observer task identity was rejected");
		output.metadata = {std::move(input), std::move(*identity)};
		return output;
	}

	[[nodiscard]] sdk::result<provider_worker_v4_ast_observation_batch>
	run_once(const fixture& value,
			 provider_worker_v4_ast_observer_limits limits = {},
			 const bool inject_bad_alloc = false,
			 const std::string_view compile_unit_value = "compile-unit:v4-ast-observer")
	{
		const auto* main =
			value.metadata.input.closure.find_member(value.metadata.input.main_logical_path);
		require(main != nullptr, "ast observer fixture lost main member");
		provider::clang22::translation_unit_input input{
			value.metadata.input.closure.snapshot_id,
			main->file_id,
			value.metadata.input.main_logical_path,
			value.source,
			value.effective_arguments,
		};
		std::optional<provider_worker_v4_ast_observation_batch> observed;
		auto executed = provider::clang22::with_translation_unit(
			input,
			[&](provider::clang22::borrowed_translation_unit& unit) -> sdk::result<void>
			{
				std::string compile_unit = inject_bad_alloc
					? "compile-unit:" + std::string(490U, 'x')
					: std::string{compile_unit_value};
#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
				if (inject_bad_alloc)
					allocation_fault_test::arm(compile_unit.size() + 1U);
#endif
				auto result = observe_provider_worker_v4_ast(
					unit, value.metadata, std::move(compile_unit), limits);
#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
				allocation_fault_test::disarm();
#endif
				if (!result)
					return sdk::unexpected(std::move(result.error()));
				observed.emplace(std::move(*result));
				return {};
			});
		if (!executed)
			return sdk::unexpected(std::move(executed.error()));
		if (!observed)
			return sdk::unexpected(
				sdk::error{"test.ast-observer-no-output", "callback", "not-invoked"});
		return std::move(*observed);
	}

	void require_error(const sdk::result<provider_worker_v4_ast_observation_batch>& value,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view message)
	{
		if (value || value.error().code != code || value.error().field != field)
		{
			if (!value)
				std::cerr << "unexpected error: " << value.error().code << " / "
						  << value.error().field << " / " << value.error().detail << '\n';
			require(false, message);
		}
	}

	[[nodiscard]] std::size_t total_origins(const provider_worker_v4_ast_observation_batch& value)
	{
		std::size_t output{};
		for (const auto& observation : value.observations)
			output += observation.origins.size();
		return output;
	}

	[[nodiscard]] std::size_t
	maximum_observation_origins(const provider_worker_v4_ast_observation_batch& value)
	{
		std::size_t output{};
		for (const auto& observation : value.observations)
			output = std::max(output, observation.origins.size());
		return output;
	}

	enum class measured_limit
	{
		logical_bytes,
		traversal_entries,
		traversal_depth,
	};

	[[nodiscard]] std::size_t minimum_passing_limit(const fixture& value, measured_limit measured)
	{
		std::size_t lower{1U};
		const auto upper_for = [&]()
		{
			switch (measured)
			{
				case measured_limit::logical_bytes:
					return provider_worker_v4_ast_product_maximum_logical_bytes;
				case measured_limit::traversal_entries:
					return provider_worker_v4_ast_product_maximum_traversal_entries;
				case measured_limit::traversal_depth:
					return provider_worker_v4_ast_product_maximum_traversal_depth;
			}
			return std::size_t{};
		};
		std::size_t upper = upper_for();
		while (lower < upper)
		{
			const auto middle = lower + (upper - lower) / 2U;
			provider_worker_v4_ast_observer_limits limits;
			if (measured == measured_limit::logical_bytes)
				limits.maximum_logical_bytes = middle;
			else if (measured == measured_limit::traversal_entries)
				limits.maximum_traversal_entries = middle;
			else
				limits.maximum_traversal_depth = middle;
			auto result = run_once(value, limits);
			if (result)
				upper = middle;
			else
			{
				const auto expected_field = measured == measured_limit::logical_bytes ? "bytes"
					: measured == measured_limit::traversal_entries ? "traversal-count"
																	: "depth";
				require(result.error().code == "provider-worker-v4.ast-resource-limit" &&
							result.error().field == expected_field,
						"minimal passing search encountered a non-resource failure");
				lower = middle + 1U;
			}
		}
		return lower;
	}
} // namespace

int main()
{
#if !defined(CXXLENS_TEST_CLANGXX22_PATH)
	std::cerr << "exact Clang 22 path is required by the AST observer test\n";
	return EXIT_FAILURE;
#else
	provider_worker_v4_ast_observer_limits product_limits;
	require(product_limits.validate().has_value(), "observer product limits were invalid");
	auto invalid_limits = product_limits;
	invalid_limits.maximum_observations = 0U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().code == "provider-worker-v4.ast-limit-invalid" &&
				invalid_limits.validate().error().field == "observations",
			"observer accepted a zero observation limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_rows = 0U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "rows",
			"observer accepted a zero row limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_diagnostics = 0U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "diagnostics",
			"observer accepted a zero diagnostic limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_origins = 0U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "origins",
			"observer accepted a zero origin limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_origins_per_observation = 0U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().field == "origins-per-observation",
			"observer accepted a zero per-observation origin limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_logical_bytes = 0U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "bytes",
			"observer accepted a zero logical byte limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_traversal_entries = 0U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().field == "traversal-count",
			"observer accepted a zero traversal entry limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_traversal_depth = 0U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "depth",
			"observer accepted a zero traversal depth limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_logical_bytes = std::numeric_limits<std::size_t>::max();
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "bytes" &&
				invalid_limits.validate().error().detail == "product-maximum",
			"observer accepted an overflowing caller byte limit");
	invalid_limits = product_limits;
	invalid_limits.maximum_observations = provider_worker_v4_ast_product_maximum_observations + 1U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "observations",
			"observer allowed a caller to widen the product observation maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_rows = provider_worker_v4_ast_product_maximum_rows + 1U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "rows",
			"observer allowed a caller to widen the product row maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_diagnostics = provider_worker_v4_ast_product_maximum_diagnostics + 1U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "diagnostics",
			"observer allowed a caller to widen the product diagnostic maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_origins = provider_worker_v4_ast_product_maximum_origins + 1U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "origins",
			"observer allowed a caller to widen the product origin maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_origins_per_observation =
		provider_worker_v4_ast_product_maximum_origins_per_observation + 1U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().field == "origins-per-observation",
			"observer allowed a caller to widen the per-observation origin maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_traversal_entries =
		provider_worker_v4_ast_product_maximum_traversal_entries + 1U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().field == "traversal-count",
			"observer allowed a caller to widen the traversal-count maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_traversal_depth =
		provider_worker_v4_ast_product_maximum_traversal_depth + 1U;
	require(!invalid_limits.validate() && invalid_limits.validate().error().field == "depth",
			"observer allowed a caller to widen the traversal-depth maximum");
	invalid_limits = product_limits;
	invalid_limits.maximum_origins = 1U;
	invalid_limits.maximum_origins_per_observation = 2U;
	require(!invalid_limits.validate() &&
				invalid_limits.validate().error().field == "origins-per-observation",
			"observer accepted a per-observation origin limit above its aggregate");

	const auto baseline_fixture = make_fixture();
	auto first = run_once(baseline_fixture);
	if (!first)
	{
		std::cerr << "ast observer positive failed: " << first.error().code << " / "
				  << first.error().field << " / " << first.error().detail << '\n';
	}
	require(first.has_value(), "ast observer did not detach positive output");
	require(first->validate().has_value(), "ast observer batch failed validation");
	require(!first->observations.empty(), "ast observer produced no observations");
	require(first->observations.size() == first->rows.size(),
			"ast observer observation/row cardinality diverged");
	bool saw_entity = false;
	bool saw_call = false;
	for (const auto& observation : first->observations)
	{
		saw_entity |= observation.kind == provider_worker_v4_ast_observation_kind::entity;
		saw_call |= observation.kind == provider_worker_v4_ast_observation_kind::call;
		require(observation.validate().has_value(), "ast observer emitted invalid observation");
	}
	for (const auto& row : first->rows)
		require(provider::clang22::detect_native_escape(row).has_value(),
				"ast observer row retained native state");
	require(saw_entity, "ast observer missed function declaration");
	require(saw_call, "ast observer missed direct call");

	auto second = run_once(baseline_fixture);
	require(second.has_value(), "ast observer repeat execution failed");
	require(first->observations.size() == second->observations.size(),
			"ast observer repeat cardinality changed");
	require(first->rows.size() == second->rows.size(), "ast observer repeat row count changed");
	require(first->failed_count == second->failed_count,
			"ast observer repeat failed count changed");
	require(first->diagnostics == second->diagnostics, "ast observer repeat diagnostics changed");
	for (std::size_t index = 0U; index < first->observations.size(); ++index)
	{
		require(first->observations[index] == second->observations[index],
				"ast observer output was not deterministic");
		require(first->rows[index].canonical_form() == second->rows[index].canonical_form(),
				"ast observer detached rows were not deterministic");
	}

	const auto observation_count = first->observations.size();
	require(observation_count > 1U, "observer fixture needs multiple observations");
	auto limited = product_limits;
	limited.maximum_observations = observation_count - 1U;
	require_error(run_once(baseline_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "observations",
				  "observer retained one observation above the caller limit");
	limited.maximum_observations = observation_count;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected the exact observation boundary");
	limited.maximum_observations = observation_count + 1U;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected one observation of caller headroom");

	limited = product_limits;
	limited.maximum_rows = observation_count - 1U;
	require_error(run_once(baseline_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "rows",
				  "observer allocated rows above the caller limit");
	limited.maximum_rows = observation_count;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected the exact row boundary");
	limited.maximum_rows = observation_count + 1U;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected one row of caller headroom");

	const auto minimum_bytes =
		minimum_passing_limit(baseline_fixture, measured_limit::logical_bytes);
	require(minimum_bytes > 1U &&
				minimum_bytes < provider_worker_v4_ast_product_maximum_logical_bytes,
			"observer logical-byte boundary was not measurable");
	limited = product_limits;
	limited.maximum_logical_bytes = minimum_bytes - 1U;
	require_error(run_once(baseline_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer exceeded the logical byte reservation");
	limited.maximum_logical_bytes = minimum_bytes;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected the exact logical byte reservation");
	limited.maximum_logical_bytes = minimum_bytes + 1U;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected one byte of caller headroom");

	std::string row_overflow_source;
	for (std::size_t index = 0U; index < 64U; ++index)
		row_overflow_source +=
			"int row_" + std::to_string(index) + "() { return " + std::to_string(index) + "; }\n";
	row_overflow_source += "int main() { return row_0(); }\n";
	const auto row_overflow_fixture = make_fixture(std::move(row_overflow_source));
	limited = product_limits;
	limited.maximum_logical_bytes = 512U * 1024U;
	auto row_overflow = run_once(row_overflow_fixture, limited);
	require_error(row_overflow,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer did not reject aggregate row bytes before allocation");
	require(row_overflow.error().detail == "row-reservation-overflow",
			"observer did not exercise checked aggregate row overflow before rows.reserve");

	const auto minimum_entries =
		minimum_passing_limit(baseline_fixture, measured_limit::traversal_entries);
	require(minimum_entries > 1U &&
				minimum_entries < provider_worker_v4_ast_product_maximum_traversal_entries,
			"observer traversal-count boundary was not measurable");
	limited = product_limits;
	limited.maximum_traversal_entries = minimum_entries - 1U;
	require_error(run_once(baseline_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "traversal-count",
				  "observer traversed one entry above the caller count limit");
	limited.maximum_traversal_entries = minimum_entries;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected the exact traversal-count boundary");
	limited.maximum_traversal_entries = minimum_entries + 1U;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected one traversal entry of caller headroom");

	const auto minimum_depth =
		minimum_passing_limit(baseline_fixture, measured_limit::traversal_depth);
	require(minimum_depth > 1U &&
				minimum_depth < provider_worker_v4_ast_product_maximum_traversal_depth,
			"observer traversal-depth boundary was not measurable");
	limited = product_limits;
	limited.maximum_traversal_depth = minimum_depth - 1U;
	require_error(run_once(baseline_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "depth",
				  "observer traversed one node above the caller depth limit");
	limited.maximum_traversal_depth = minimum_depth;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected the exact traversal-depth boundary");
	limited.maximum_traversal_depth = minimum_depth + 1U;
	require(run_once(baseline_fixture, limited).has_value(),
			"observer rejected one traversal-depth unit of caller headroom");

	const auto template_fixture =
		make_fixture("template <class T> struct box { T value; };\n"
					 "template <class T> T select(box<T> input) { return input.value; }\n"
					 "int main() { box<int> value{42}; return select<int>(value); }\n");
	auto template_output = run_once(template_fixture);
	if (!template_output)
		std::cerr << "template observer failed: " << template_output.error().code << " / "
				  << template_output.error().field << " / " << template_output.error().detail
				  << '\n';
	require(template_output.has_value() && !template_output->observations.empty(),
			"observer rejected the template/type traversal fixture");
	const auto template_entries =
		minimum_passing_limit(template_fixture, measured_limit::traversal_entries);
	limited = product_limits;
	limited.maximum_traversal_entries = template_entries - 1U;
	require_error(run_once(template_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "traversal-count",
				  "template traversal exceeded its caller entry count");
	limited.maximum_traversal_entries = template_entries;
	require(run_once(template_fixture, limited).has_value(),
			"template traversal rejected its exact entry count");
	limited.maximum_traversal_entries = template_entries + 1U;
	require(run_once(template_fixture, limited).has_value(),
			"template traversal rejected one entry of caller headroom");
	const auto template_depth =
		minimum_passing_limit(template_fixture, measured_limit::traversal_depth);
	limited = product_limits;
	limited.maximum_traversal_depth = template_depth - 1U;
	require_error(run_once(template_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "depth",
				  "template traversal exceeded its caller depth");
	limited.maximum_traversal_depth = template_depth;
	require(run_once(template_fixture, limited).has_value(),
			"template traversal rejected its exact depth");
	limited.maximum_traversal_depth = template_depth + 1U;
	require(run_once(template_fixture, limited).has_value(),
			"template traversal rejected one depth unit of caller headroom");

	const auto lambda_return_fixture =
		make_fixture("auto make_lambda() { return []() { return 7; }; }\n"
					 "int main() { return make_lambda()(); }\n");
	require(run_once(lambda_return_fixture).has_value(),
			"USR preflight rejected a non-template return type that Clang does not encode");
	const std::string long_return_name(2048U, 'r');
	const auto long_return_fixture =
		make_fixture("struct " + long_return_name + " {};\n" + long_return_name +
					 " make_long_return() { return {}; }\nint main() { return 0; }\n");
	require(run_once(long_return_fixture).has_value(),
			"USR preflight charged a non-template return type absent from the Clang USR");
	const auto enclosing_long_return_fixture =
		make_fixture("struct " + long_return_name + " {};\n" + long_return_name +
					 " make_enclosing_return() {\n"
					 "  auto nested = []() { return 7; };\n"
					 "  (void)nested();\n"
					 "  return {};\n"
					 "}\nint main() { return 0; }\n");
	require(run_once(enclosing_long_return_fixture).has_value(),
			"USR preflight charged an enclosing non-template return type absent from a nested "
			"function USR");

	std::string nested_namespace_source{"namespace n0"};
	for (std::size_t depth = 1U; depth < 64U; ++depth)
		nested_namespace_source += "::n" + std::to_string(depth);
	nested_namespace_source += " { int deeply_nested() { return 1; } }\nint main() { return 0; }\n";
	require(run_once(make_fixture(std::move(nested_namespace_source))).has_value(),
			"USR preflight grew namespace context profiling quadratically");

	const auto usr_envelope_fixture = make_fixture("int " + std::string(2048U, 'u') +
												   "() { return 1; }\nint main() { return 0; }\n");
	auto usr_envelope = run_once(usr_envelope_fixture);
	require_error(usr_envelope,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer did not reject a USR whose proved inline envelope was too large");
	require(usr_envelope.error().detail == "clang-usr-envelope",
			"observer reached Clang USR generation before proving its inline envelope");

	// The exact Clang USR necessarily contains this identifier.  Its qualified-name preflight
	// rejects the declaration before generateUSRForDecl can grow its 64 KiB inline vector.
	const auto over_inline_usr_fixture = make_fixture(
		"int " + std::string(70U * 1024U, 'v') + "() { return 1; }\nint main() { return 0; }\n");
	auto over_inline_usr = run_once(over_inline_usr_fixture);
	require_error(over_inline_usr,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer allowed a greater-than-64-KiB USR input to reach Clang generation");
	require(over_inline_usr.error().detail == "clang-usr-structure",
			"observer did not reject the oversized USR input during fixed-buffer preflight");

	const std::string long_default_type_name(70U * 1024U, 'd');
	const auto long_default_type_fixture = make_fixture("struct " + long_default_type_name +
														" {};\n"
														"template <class T = " +
														long_default_type_name +
														"> void default_template_target();\n"
														"int main() { return 0; }\n");
	auto long_default_type = run_once(long_default_type_fixture);
	require(long_default_type.has_value(),
			"observer charged default-template prose that exact Clang excludes from the USR");
	bool saw_default_template_exact_usr = false;
	for (const auto& observation : long_default_type->observations)
		saw_default_template_exact_usr |=
			observation.kind == provider_worker_v4_ast_observation_kind::entity &&
			observation.payload.contains("symbol.qualified_name") &&
			observation.payload.at("symbol.qualified_name") == "default_template_target" &&
			observation.payload.contains("symbol.identity_confidence") &&
			observation.payload.at("symbol.identity_confidence") == "exact-usr";
	require(saw_default_template_exact_usr,
			"default-template fixture did not exercise its short exact Clang USR");

	const auto fixed_string_definition = []
	{
		return std::string{
			"template <unsigned long N> struct fixed_string {\n"
			"  char value[N];\n"
			"  consteval fixed_string(const char (&input)[N]) {\n"
			"    for (unsigned long index = 0; index < N; ++index) value[index] = input[index];\n"
			"  }\n"
			"};\n"};
	};
	const std::string structural_value(70U * 1024U, 's');
	const auto class_structural_nttp_fixture = make_fixture(
		fixed_string_definition() +
		"template <fixed_string Value> struct structural_box { static int observed(); };\n"
		"template <> struct structural_box<fixed_string{\"" +
		structural_value +
		"\"}> { static int observed() { return 1; } };\n"
		"int main() { return 0; }\n");
	auto class_structural_nttp = run_once(class_structural_nttp_fixture);
	require(class_structural_nttp.has_value(),
			"observer charged class-specialization structural-value prose excluded from its USR");
	bool saw_class_structural_specialization = false;
	for (const auto& observation : class_structural_nttp->observations)
		saw_class_structural_specialization |=
			observation.kind == provider_worker_v4_ast_observation_kind::entity &&
			observation.payload.contains("symbol.qualified_name") &&
			observation.payload.at("symbol.qualified_name").ends_with("::observed") &&
			observation.payload.at("symbol.qualified_name").starts_with("structural_box<");
	require(saw_class_structural_specialization,
			"class-specialization fixture did not traverse the structural NTTP declaration");

	const auto function_structural_nttp_fixture =
		make_fixture(fixed_string_definition() +
					 "template <fixed_string Value> int structural_function_target();\n"
					 "template <> int structural_function_target<fixed_string{\"" +
					 structural_value +
					 "\"}>() { return 1; }\n"
					 "int main() { return 0; }\n");
	auto function_structural_nttp = run_once(function_structural_nttp_fixture);
	require(
		function_structural_nttp.has_value(),
		"observer charged function-specialization structural-value prose excluded from its USR");
	bool saw_function_structural_specialization = false;
	for (const auto& observation : function_structural_nttp->observations)
		saw_function_structural_specialization |=
			observation.kind == provider_worker_v4_ast_observation_kind::entity &&
			observation.payload.contains("symbol.qualified_name") &&
			observation.payload.at("symbol.qualified_name")
				.starts_with("structural_function_target") &&
			observation.payload.contains("symbol.is_definition") &&
			observation.payload.at("symbol.is_definition") == "true";
	require(saw_function_structural_specialization,
			"function-specialization fixture did not traverse the structural NTTP declaration");
	auto function_structural_repeat = run_once(function_structural_nttp_fixture);
	require(function_structural_repeat.has_value() &&
				function_structural_repeat->observations ==
					function_structural_nttp->observations &&
				function_structural_repeat->diagnostics == function_structural_nttp->diagnostics &&
				function_structural_repeat->rows.size() == function_structural_nttp->rows.size(),
			"bounded structural-template identity was not deterministic");
	for (std::size_t index{}; index < function_structural_nttp->rows.size(); ++index)
		require(function_structural_repeat->rows[index].canonical_form() ==
					function_structural_nttp->rows[index].canonical_form(),
				"bounded structural-template rows were not deterministic");

	const auto external_usr_source = [](const std::size_t usr_bytes)
	{
		return "[[clang::external_source_symbol(language=\"Swift\", defined_in=\"M\", USR=\"" +
			std::string(usr_bytes, 's') +
			"\")]] void external_symbol();\n"
			"int main() { return 0; }\n";
	};
	const auto external_usr_boundary_fixture = make_fixture(external_usr_source(64U * 1024U));
	require(run_once(external_usr_boundary_fixture).has_value(),
			"observer rejected an exact-boundary explicit Clang external USR");
	const auto external_usr_overflow_fixture = make_fixture(external_usr_source(64U * 1024U + 1U));
	auto external_usr_overflow = run_once(external_usr_overflow_fixture);
	require_error(external_usr_overflow,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer allocated an oversized explicit external USR before its bypass bound");
	require(external_usr_overflow.error().detail == "clang-usr",
			"observer routed an oversized explicit external USR through Clang generation");

	const std::string hidden_enumerator_envelope(2048U, 'h');
	const auto hidden_usr_envelope_fixture = make_fixture(
		"enum { " + hidden_enumerator_envelope + " };\nvoid hidden_usr_envelope(decltype(" +
		hidden_enumerator_envelope + ") value) { (void)value; }\nint main() { return 0; }\n");
	auto hidden_usr_envelope = run_once(hidden_usr_envelope_fixture);
	require_error(hidden_usr_envelope,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer did not include a hidden enumerator in its USR envelope");
	require(hidden_usr_envelope.error().detail == "clang-usr-envelope",
			"observer did not reject the hidden enumerator at the proved USR envelope");

	const std::string hidden_enumerator(70U * 1024U, 'e');
	const auto hidden_usr_fixture =
		make_fixture("enum { " + hidden_enumerator + " };\nvoid hidden_usr(decltype(" +
					 hidden_enumerator + ") value) { (void)value; }\nint main() { return 0; }\n");
	auto hidden_usr = run_once(hidden_usr_fixture);
	require_error(
		hidden_usr,
		"provider-worker-v4.ast-resource-limit",
		"bytes",
		"observer missed a long anonymous-enum enumerator hidden by canonical type prose");
	require(hidden_usr.error().detail == "clang-usr-structure",
			"observer did not preflight hidden declaration text followed by Clang USR generation");

	const std::string partial_parameter_name(70U * 1024U, 'p');
	const auto partial_specialization_fixture =
		make_fixture("template <class> struct " + partial_parameter_name +
					 " {};\n"
					 "template <class A, auto B> struct partial_target;\n"
					 "template <class T, " +
					 partial_parameter_name +
					 "<T>* P>\n"
					 "struct partial_target<T, P> { static void observed() {} };\n"
					 "int main() { return 0; }\n");
	auto partial_specialization = run_once(partial_specialization_fixture);
	require_error(partial_specialization,
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer missed template parameters carried only by a class partial "
				  "specialization context");
	require(partial_specialization.error().detail == "clang-usr-structure",
			"observer reached Clang USR generation before bounding partial-specialization "
			"parameters");

	const std::string embedded_enumerator_name(2048U, 'm');
	const auto embedded_enum_fixture =
		make_fixture("enum { " + embedded_enumerator_name +
					 " } embedded_value;\n"
					 "void use_embedded_enum(decltype(embedded_value) value) { (void)value; }\n"
					 "int main() { return 0; }\n");
	require(run_once(embedded_enum_fixture).has_value(),
			"USR preflight charged an enumerator name excluded by Clang's embedded-tag "
			"location identity");

	const auto macro_fixture = make_fixture("#define TYPE_INNER int\n"
											"#define TYPE_MIDDLE TYPE_INNER\n"
											"#define TYPE TYPE_MIDDLE\n"
											"TYPE alpha(int value) { return value + 1; }\n"
											"TYPE beta(int value) { return value + 1; }\n"
											"int main() { return alpha(1) + beta(2); }\n");
	auto macro_output = run_once(macro_fixture);
	if (!macro_output)
		std::cerr << "macro observer failed: " << macro_output.error().code << " / "
				  << macro_output.error().field << " / " << macro_output.error().detail << '\n';
	require(macro_output.has_value(), "observer rejected the macro provenance fixture");
	const auto origin_count = total_origins(*macro_output);
	const auto origins_per_observation = maximum_observation_origins(*macro_output);
	require(origin_count > origins_per_observation && origins_per_observation > 1U,
			"macro fixture did not retain aggregate and nested origin provenance");
	for (const auto& observation : macro_output->observations)
		for (const auto& origin : observation.origins)
			require(origin.read_only && !origin.kind.empty() && !origin.logical_path.empty(),
					"observer dropped typed macro origin provenance");

	limited = product_limits;
	limited.maximum_origins = origin_count - 1U;
	limited.maximum_origins_per_observation = std::min(
		provider_worker_v4_ast_product_maximum_origins_per_observation, limited.maximum_origins);
	require(limited.maximum_origins_per_observation >= origins_per_observation,
			"macro fixture cannot isolate the aggregate origin boundary");
	require_error(run_once(macro_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "origins",
				  "observer retained one aggregate origin above the caller limit");
	limited.maximum_origins = origin_count;
	limited.maximum_origins_per_observation = origins_per_observation;
	require(run_once(macro_fixture, limited).has_value(),
			"observer rejected the exact aggregate origin boundary");
	limited.maximum_origins = origin_count + 1U;
	require(run_once(macro_fixture, limited).has_value(),
			"observer rejected one aggregate origin of caller headroom");

	limited = product_limits;
	limited.maximum_origins_per_observation = origins_per_observation - 1U;
	require_error(run_once(macro_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "origins",
				  "observer retained one per-observation origin above the caller limit");
	limited.maximum_origins_per_observation = origins_per_observation;
	require(run_once(macro_fixture, limited).has_value(),
			"observer rejected the exact per-observation origin boundary");
	limited.maximum_origins_per_observation = origins_per_observation + 1U;
	require(run_once(macro_fixture, limited).has_value(),
			"observer rejected one per-observation origin of caller headroom");

	std::string bounded_origin_source{"#define ORIGIN_0(value) value\n"};
	for (std::size_t depth = 1U; depth <= 109U; ++depth)
		bounded_origin_source += "#define ORIGIN_" + std::to_string(depth) + "(value) ORIGIN_" +
			std::to_string(depth - 1U) + "(value)\n";
	bounded_origin_source += "ORIGIN_109(int) f() { return 1; }\n";
	bounded_origin_source += "int main() { return f(); }\n";
	const auto bounded_origin_fixture =
		make_fixture(std::move(bounded_origin_source), "project://a.cpp");
	auto bounded_origin_output = run_once(bounded_origin_fixture, product_limits, false, "cu");
	if (!bounded_origin_output)
		std::cerr << "bounded origin observer failed: " << bounded_origin_output.error().code
				  << " / " << bounded_origin_output.error().field << " / "
				  << bounded_origin_output.error().detail << '\n';
	require(bounded_origin_output.has_value(),
			"observer under-reserved per-origin canonical framing before row allocation");
	require(maximum_observation_origins(*bounded_origin_output) >= 109U,
			"bounded origin fixture did not retain its deep exact provenance chain");
	const auto bounded_origin_bytes =
		minimum_passing_limit(bounded_origin_fixture, measured_limit::logical_bytes);
	limited = product_limits;
	limited.maximum_logical_bytes = bounded_origin_bytes - 1U;
	require_error(run_once(bounded_origin_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "bytes",
				  "observer allocated a deep origin row one byte above its reservation");
	limited.maximum_logical_bytes = bounded_origin_bytes;
	require(run_once(bounded_origin_fixture, limited).has_value(),
			"observer rejected the exact deep-origin row reservation");
	limited.maximum_logical_bytes = bounded_origin_bytes + 1U;
	require(run_once(bounded_origin_fixture, limited).has_value(),
			"observer rejected one byte of deep-origin row headroom");

	std::string canonical_overflow_source{"#define WRAP_0(value) value\n"};
	for (std::size_t depth = 1U; depth <= 109U; ++depth)
		canonical_overflow_source += "#define WRAP_" + std::to_string(depth) + "(value) WRAP_" +
			std::to_string(depth - 1U) + "(value)\n";
	canonical_overflow_source +=
		"[[clang::external_source_symbol(language=\"Swift\", defined_in=\"M\", USR=\"x\")]]\n"
		"WRAP_109(int) f() { return 1; }\n"
		"int main() { return f(); }\n";
	const auto canonical_overflow_fixture =
		make_fixture(std::move(canonical_overflow_source), "project://a.cpp");
	limited = product_limits;
	limited.maximum_logical_bytes = 2900U;
	auto canonical_overflow = run_once(canonical_overflow_fixture, limited, false, "cu");
	require_error(
		canonical_overflow,
		"provider-worker-v4.ast-resource-limit",
		"bytes",
		"observer lost a reachable canonical-size overflow behind a generic traversal error");
	require(canonical_overflow.error().detail == "observation-canonical-key-overflow",
			"observer did not type the reachable canonical-size overflow before key allocation");

	std::string deep_macro_source{"#define WRAP_0(value) value\n"};
	for (std::size_t depth = 1U; depth <= 130U; ++depth)
		deep_macro_source += "#define WRAP_" + std::to_string(depth) + "(value) WRAP_" +
			std::to_string(depth - 1U) + "(value)\n";
	deep_macro_source += "WRAP_130(int) deep_one() { return 1; }\n";
	deep_macro_source += "WRAP_130(int) deep_two() { return 2; }\n";
	deep_macro_source += "int main() { return deep_one() + deep_two(); }\n";
	const auto diagnostic_fixture = make_fixture(std::move(deep_macro_source));
	auto diagnostic_output = run_once(diagnostic_fixture);
	if (!diagnostic_output)
		std::cerr << "diagnostic observer failed: " << diagnostic_output.error().code << " / "
				  << diagnostic_output.error().field << " / " << diagnostic_output.error().detail
				  << '\n';
	require(diagnostic_output.has_value(), "observer rejected the diagnostic fixture");
	const auto diagnostic_count = diagnostic_output->diagnostics.size();
	require(diagnostic_count > 1U && diagnostic_output->failed_count == diagnostic_count,
			"deep macro fixture did not preserve typed diagnostics and failed count");
	limited = product_limits;
	limited.maximum_diagnostics = diagnostic_count - 1U;
	require_error(run_once(diagnostic_fixture, limited),
				  "provider-worker-v4.ast-resource-limit",
				  "diagnostics",
				  "observer appended one diagnostic above the caller limit");
	limited.maximum_diagnostics = diagnostic_count;
	require(run_once(diagnostic_fixture, limited).has_value(),
			"observer rejected the exact diagnostic boundary");
	limited.maximum_diagnostics = diagnostic_count + 1U;
	require(run_once(diagnostic_fixture, limited).has_value(),
			"observer rejected one diagnostic of caller headroom");

#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
	auto allocation_failure = run_once(baseline_fixture, product_limits, true);
	require_error(allocation_failure,
				  "provider-worker-v4.ast-allocation",
				  "observer",
				  "observer did not type an allocation failure");
#endif

	// A task identity changed after transport admission must not be accepted by the AST seam.
	auto tampered = baseline_fixture;
	tampered.metadata.identity.task_v4_digest.back() =
		tampered.metadata.identity.task_v4_digest.back() == '0' ? '1' : '0';
	auto rejected = run_once(tampered);
	require(!rejected && rejected.error().code == "provider-worker-v4.ast-input-invalid",
			"ast observer accepted a tampered task identity");

	return EXIT_SUCCESS;
#endif
}
