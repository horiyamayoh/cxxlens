#include "sdk/gcc_replay_planner_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view input)
	{
		std::vector<std::byte> output;
		output.reserve(input.size());
		for (const auto value : input)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		return output;
	}

	[[nodiscard]] cxxlens::sdk::detail::decoded_capture_projection projection()
	{
		using namespace cxxlens::sdk::detail;
		decoded_capture_projection value;
		value.path_mappings.push_back({"/workspace/example", "project://"});
		decoded_capture_source_closure closure;
		closure.id = "source-closure:main";
		value.source_closures.push_back(std::move(closure));
		return value;
	}

	[[nodiscard]] cxxlens::sdk::detail::decoded_capture_unit
	unit_with(std::vector<std::string> arguments)
	{
		using namespace cxxlens::sdk::detail;
		decoded_capture_unit value;
		value.compile_unit_id = "compile-unit:main";
		value.source_logical_path = "project://src/main.cpp";
		value.logical_working_directory = "project://build";
		value.language = "c++";
		value.original_arguments = std::move(arguments);
		value.language_standard = "c++23";
		value.extension_mode = "strict";
		value.source_closure_id = "source-closure:main";
		return value;
	}

	[[nodiscard]] bool has_reason(const std::span<const cxxlens::sdk::capture_gap> gaps,
								  const std::string_view reason)
	{
		return std::ranges::any_of(gaps,
								   [&](const auto& gap)
								   {
									   return gap.reason == reason;
								   });
	}

	void exact_and_semantics_preserving_options_are_mapped_in_order()
	{
		auto capture = projection();
		auto unit = unit_with({"/opt/gcc-16.2.0/bin/g++",
							   "-std=c++23",
							   "-DVALUE=1",
							   "-U",
							   "OLD_VALUE",
							   "-I",
							   "../include",
							   "-include",
							   "../config.hpp",
							   "-c",
							   "/workspace/example/src/main.cpp",
							   "-o",
							   "main.o",
							   "-MD",
							   "-MF",
							   "main.d"});
		auto mapped = cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, unit, 0U);
		require(mapped);
		const std::vector<std::string> expected{"clang++",
												"-fsyntax-only",
												"-std=c++23",
												"-DVALUE=1",
												"-U",
												"OLD_VALUE",
												"-I",
												"project://include",
												"-include",
												"project://config.hpp",
												"project://src/main.cpp"};
		require(mapped->effective_arguments == expected);
		require(mapped->option_mappings.size() == unit.original_arguments->size());
		require(mapped->option_mappings[1].fidelity == cxxlens::sdk::replay_fidelity::exact);
		require(mapped->option_mappings[6].fidelity ==
				cxxlens::sdk::replay_fidelity::semantics_preserving);
		require(mapped->option_mappings[9].fidelity == cxxlens::sdk::replay_fidelity::nonsemantic);
		require(
			has_reason(mapped->unresolved, "analysis-frontend-differs-from-production-compiler"));
	}

	void response_files_use_the_same_bounded_mapping_authority()
	{
		auto capture = projection();
		const auto content = bytes("-DRESPONSE=1 -I../generated");
		cxxlens::sdk::detail::decoded_capture_source_member member;
		member.logical_path = "project://build/options.rsp";
		member.content = content;
		capture.source_closures.front().members.push_back(std::move(member));
		auto unit = unit_with(
			{"/opt/gcc-16.2.0/bin/g++", "@options.rsp", "/workspace/example/src/main.cpp"});
		cxxlens::sdk::detail::decoded_capture_auxiliary_file response;
		response.logical_path = "project://build/options.rsp";
		response.content_digest = cxxlens::sdk::content_digest(content);
		response.size_bytes = content.size();
		unit.response_files.push_back(std::move(response));
		auto mapped = cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, unit, 0U);
		require(mapped);
		require(std::ranges::find(mapped->effective_arguments, "-DRESPONSE=1") !=
				mapped->effective_arguments.end());
		require(std::ranges::find(mapped->effective_arguments, "-Iproject://generated") !=
				mapped->effective_arguments.end());
		const auto again = cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, unit, 0U);
		require(again && again->effective_arguments == mapped->effective_arguments &&
				again->unresolved == mapped->unresolved);
	}

	void unsupported_options_remain_actionable_and_are_not_replayed()
	{
		auto capture = projection();
		auto unit = unit_with({"/opt/gcc-16.2.0/bin/g++",
							   "--specs",
							   "/workspace/example/config/specs",
							   "-isystem",
							   "/usr/include",
							   "-fvendor-mode",
							   "/workspace/example/src/main.cpp"});
		auto mapped = cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, unit, 0U);
		require(mapped);
		require(has_reason(mapped->unresolved, "gcc-specification-file-not-replayable"));
		require(has_reason(mapped->unresolved, "gcc-path-option-outside-logical-project"));
		require(has_reason(mapped->unresolved, "gcc-option-not-classified"));
		require(std::ranges::find(mapped->effective_arguments, "-fvendor-mode") ==
				mapped->effective_arguments.end());
		require(std::ranges::find(mapped->effective_arguments, "/usr/include") ==
				mapped->effective_arguments.end());
	}

	void malformed_pairs_and_effective_output_overflow_fail_closed()
	{
		auto capture = projection();
		auto missing = unit_with({"g++", "-MF"});
		auto missing_result =
			cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, missing, 0U);
		require(!missing_result &&
				missing_result.error().detail == "missing-dependency-option-argument");
		auto missing_external = unit_with({"g++", "--specs"});
		auto missing_external_result =
			cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, missing_external, 0U);
		require(!missing_external_result &&
				missing_external_result.error().detail == "missing-external-option-argument");
		auto empty_path = unit_with({"g++", "-I", ""});
		auto empty_path_result =
			cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, empty_path, 0U);
		require(!empty_path_result &&
				empty_path_result.error().detail == "empty-path-option-argument");

		auto bounded = unit_with({"g++", "/workspace/example/src/main.cpp"});
		cxxlens::sdk::import_limits limits;
		limits.maximum_arguments_per_unit = 2U;
		auto bounded_result =
			cxxlens::sdk::detail::map_gcc_16_2_replay_arguments(capture, bounded, 0U, limits);
		require(!bounded_result &&
				bounded_result.error().code == "application-analysis.import-limit-exceeded" &&
				bounded_result.error().field == "replay_plan.effective_arguments");
	}
} // namespace

int main()
{
	exact_and_semantics_preserving_options_are_mapped_in_order();
	response_files_use_the_same_bounded_mapping_authority();
	unsupported_options_remain_actionable_and_are_not_replayed();
	malformed_pairs_and_effective_output_overflow_fail_closed();
}
