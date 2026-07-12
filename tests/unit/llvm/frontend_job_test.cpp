#include "llvm/clang22/frontend_job.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>

#include <cxxlens/interop/clang.hpp>

#include "llvm/common/borrowed_lifetime.hpp"

namespace
{
	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::ofstream output{file};
		output << content;
		require(output.good(), "fixture write failed");
	}

	[[nodiscard]] cxxlens::workspace make_workspace(const std::filesystem::path& root,
													const std::string& filename,
													const std::string& compiler)
	{
		const auto source = root / filename;
		write(source, "this physical content must be replaced by the VFS snapshot\n");
		const auto database = root / "compile_commands.json";
		write(database,
			  "[{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
				  source.generic_string() + "\",\"arguments\":[\"" + compiler +
				  "\",\"-std=" + (filename.ends_with(".c") ? std::string{"c17"} : "c++23") +
				  "\",\"" + source.generic_string() + "\"]}]");
		auto opened = cxxlens::workspace::open(
			cxxlens::workspace_options::from_compilation_database(database));
		require(opened.has_value(), "frontend workspace did not open");
		return std::move(opened.value());
	}

	void check_lifetime_token()
	{
		cxxlens::detail::frontend::borrowed_lifetime_token token;
		require(token.check().has_value(), "owning callback thread rejected");
		bool rejected = false;
		std::thread other{[&]
						  {
							  auto result = token.check();
							  rejected = !result &&
								  result.error().code.value ==
									  "interop.borrowed-lifetime-violation" &&
								  result.error().attributes.at("reason") == "wrong-thread";
						  }};
		other.join();
		require(rejected, "cross-thread borrow was not rejected");
		token.retire();
		auto retired = token.check();
		require(!retired && retired.error().attributes.at("reason") == "retired",
				"post-callback borrow was not rejected");
	}

	void check_failure_seams()
	{
		using cxxlens::detail::frontend::frontend_fault;
		using cxxlens::detail::frontend::parse_task;
		for (const auto [fault, code] : {
				 std::pair{frontend_fault::timeout, "parse.timeout"},
				 std::pair{frontend_fault::cancellation, "core.cancelled"},
				 std::pair{frontend_fault::crash, "parse.crashed"},
			 })
		{
			parse_task task;
			task.injected_fault = fault;
			auto outcome = cxxlens::detail::clang22::execute(std::move(task));
			require(!outcome && outcome.error().code.value == code,
					"failure injection lost its stable code");
		}

		std::stop_source source;
		source.request_stop();
		cxxlens::execution_context cancelled;
		cancelled.cancellation = source.get_token();
		auto cancelled_result = cxxlens::detail::clang22::execute(parse_task{}, cancelled);
		require(!cancelled_result && cancelled_result.error().code.value == "core.cancelled",
				"cancellation seam failed");
		cxxlens::execution_context expired;
		expired.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1};
		auto deadline_result = cxxlens::detail::clang22::execute(parse_task{}, expired);
		require(!deadline_result && deadline_result.error().code.value == "core.deadline-exceeded",
				"deadline seam failed");
	}

	void check_detached_batch()
	{
		using namespace cxxlens;
		detail::frontend::observation_batch batch;
		batch.adapter_id = "clang22.frontend";
		batch.adapter_version = "1.0.0";
		batch.unit = compile_unit_id{"cu_" + std::string(64U, 'a')};
		batch.variant = build_variant_id{"variant_" + std::string(64U, 'b')};
		batch.coverage.parsed = 1U;
		detail::facts::observation_record observation;
		observation.adapter_id = batch.adapter_id;
		observation.adapter_version = batch.adapter_version;
		observation.llvm_major = 22U;
		observation.compile_unit = batch.unit;
		observation.variant = batch.variant;
		observation.kind = fact_kind::custom;
		observation.payload_version = 1U;
		observation.payload = {{"semantic_key", "detached"}};
		batch.observations = {observation};
		require(batch.validate().has_value(), "detached observation batch was rejected");
		batch.observations.front().payload = {{"clang_ast_pointer", "0x1234"}};
		require(!batch.validate(), "native AST pointer escaped through frontend batch");
	}
} // namespace

int main()
{
	check_lifetime_token();
	check_failure_seams();
	check_detached_batch();
	const auto capability = cxxlens::detail::clang22::capability();
	require(std::ranges::is_sorted(capability.explicit_components),
			"component map is not canonical");
	require(capability.explicit_components.size() == 12U, "component map is incomplete");
	require(std::ranges::find(capability.explicit_components, "clang-cpp") ==
				capability.explicit_components.end(),
			"monolithic transitive Clang dependency accepted");
	const auto linked = cxxlens::interop::linked_clang_version();
	if (!capability.available)
	{
		require(linked.llvm_major == 0U && linked.clang_revision == "unavailable" &&
					!capability.limitation.empty(),
				"unavailable capability was not structural");
		return 0;
	}

	require(linked.llvm_major == 22U && capability.llvm_major == 22U,
			"linked Clang major does not match build reality");
	const auto base = std::filesystem::temp_directory_path() / "cxxlens-frontend-job";
	std::filesystem::remove_all(base);
	std::filesystem::create_directories(base / "cpp");
	auto cpp_workspace = make_workspace(base / "cpp", "main.cpp", "clang++-22");
	const auto cpp_unit = cpp_workspace.compile_units().front();
	cxxlens::detail::frontend::parse_task cpp_task{
		cpp_unit, {{cpp_unit.command().file, "#define VALUE 7\nint value(){return VALUE;}\n"}}};
	bool callback_ran = false;
	auto callback =
		[&callback_ran](cxxlens::interop::borrowed_clang_tu& view) -> cxxlens::result<void>
	{
		callback_ran = view.unit().id().valid();
		(void)view.compiler();
		(void)view.ast_context();
		(void)view.source_manager();
		(void)view.preprocessor();
		(void)view.language_options();
		return {};
	};
	auto first = cxxlens::detail::clang22::execute(cpp_task, {}, std::move(callback));
	if (!first || !callback_ran || first.value().coverage.parsed != 1U)
	{
		std::cerr << "real C++ TU did not parse through the borrowed callback";
		if (first)
			std::cerr << ": " << first.value().semantic_representation();
		else
			std::cerr << ": " << first.error().semantic_representation();
		std::cerr << '\n';
		return 1;
	}
	auto second = cxxlens::detail::clang22::execute(cpp_task);
	require(second && first.value().debug_context_identity != second.value().debug_context_identity,
			"fresh frontend context identity was reused");
	require(first.value().semantic_representation() == second.value().semantic_representation(),
			"repeated frontend batches were nondeterministic");

	std::filesystem::create_directories(base / "c");
	auto c_workspace = make_workspace(base / "c", "main.c", "clang-22");
	const auto c_unit = c_workspace.compile_units().front();
	auto c_result = cxxlens::detail::clang22::execute(
		{c_unit, {{c_unit.command().file, "int value(void){return 0;}\n"}}});
	require(c_result && c_result.value().coverage.parsed == 1U, "real C TU did not parse");

	auto invalid = cxxlens::detail::clang22::execute(
		{cpp_unit, {{cpp_unit.command().file, "int broken( {\n"}}});
	require(invalid && invalid.value().coverage.failed == 1U &&
				!invalid.value().diagnostics.empty() && invalid.value().validate(),
			"parse error did not return a validated diagnostic/coverage batch");

	std::filesystem::create_directories(base / "variants");
	const auto variant_root = base / "variants";
	const auto variant_source = variant_root / "variant.cpp";
	write(variant_source, "physical input is replaced\n");
	write(variant_root / "compile_commands.json",
		  "[{\"directory\":\"" + variant_root.generic_string() + "\",\"file\":\"" +
			  variant_source.generic_string() +
			  "\",\"arguments\":[\"clang++-22\",\"-DVALUE=1\",\"" +
			  variant_source.generic_string() + "\"]},{\"directory\":\"" +
			  variant_root.generic_string() + "\",\"file\":\"" + variant_source.generic_string() +
			  "\",\"arguments\":[\"clang++-22\",\"-DVALUE=2\",\"" +
			  variant_source.generic_string() + "\"]}]");
	auto variants = cxxlens::workspace::open(
		cxxlens::workspace_options::from_compilation_database(variant_root));
	require(variants && variants.value().compile_units().size() == 2U,
			"variant commands were not retained");
	const auto variant_units = variants.value().compile_units();
	for (const auto& unit : variant_units)
	{
		auto parsed = cxxlens::detail::clang22::execute(
			{unit, {{unit.command().file, "int variant_value=VALUE;\n"}}});
		require(parsed && parsed.value().coverage.parsed == 1U, "build variant did not parse");
	}
	require(variant_units[0].variant_id() != variant_units[1].variant_id(),
			"distinct frontend variants collapsed");

	bool public_callback = false;
	write(cpp_unit.command().file, "int public_value(){return 1;}\n");
	auto public_result = cxxlens::interop::with_translation_unit(
		cpp_workspace,
		cpp_unit.id(),
		[&public_callback](cxxlens::interop::borrowed_clang_tu&) -> cxxlens::result<void>
		{
			public_callback = true;
			return {};
		});
	require(public_result && public_callback, "public borrowed corridor failed");
	std::filesystem::remove_all(base);
	return 0;
}
