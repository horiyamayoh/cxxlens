#pragma once

/**
 * @file compile_commands_capture_internal.hpp
 * @brief Bounded, shell-free projection of a GCC compilation database.
 */

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk::detail
{
	struct compile_command_entry
	{
		std::string directory;
		std::string file;
		std::optional<std::string> output;
		std::vector<std::string> arguments;
		bool decoded_from_command{};

		[[nodiscard]] bool operator==(const compile_command_entry&) const = default;
	};

	class compile_commands_capture
	{
	  public:
		[[nodiscard]] const std::vector<compile_command_entry>& entries() const noexcept;

	  private:
		explicit compile_commands_capture(std::vector<compile_command_entry> entries);
		std::vector<compile_command_entry> entries_;

		friend result<compile_commands_capture> decode_compile_commands(std::string_view,
																		import_limits);
		friend result<compile_commands_capture> make_explicit_compile_capture(compile_command_entry,
																			  import_limits);
	};

	/** Decode one Clang-compatible compilation database without invoking a shell. */
	[[nodiscard]] result<compile_commands_capture>
	decode_compile_commands(std::string_view input, import_limits limits = {});

	/** Validate one already tokenized shell-free compiler invocation. */
	[[nodiscard]] result<compile_commands_capture>
	make_explicit_compile_capture(compile_command_entry entry, import_limits limits = {});
} // namespace cxxlens::sdk::detail
