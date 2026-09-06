#include "msvc_process_port.hpp"

#include <limits>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace cxxlens::application_analysis_worker
{
	namespace
	{
		class unique_handle
		{
		  public:
			explicit unique_handle(HANDLE value = nullptr) noexcept : value_{value} {}
			~unique_handle()
			{
				if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
					CloseHandle(value_);
			}
			unique_handle(const unique_handle&) = delete;
			unique_handle& operator=(const unique_handle&) = delete;
			unique_handle(unique_handle&& other) noexcept
				: value_{std::exchange(other.value_, nullptr)}
			{
			}
			[[nodiscard]] HANDLE get() const noexcept
			{
				return value_;
			}

		  private:
			HANDLE value_{};
		};

		[[nodiscard]] sdk::error process_error(std::string field, std::string detail)
		{
			return {
				"application-analysis.msvc-process-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<std::wstring> canonical_executable(const std::wstring& path)
		{
			if (path.empty() || path.size() > 32767U)
				return sdk::unexpected(process_error("executable", "path-length"));
			unique_handle file{CreateFileW(path.c_str(),
										   0,
										   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
										   nullptr,
										   OPEN_EXISTING,
										   FILE_ATTRIBUTE_NORMAL,
										   nullptr)};
			if (file.get() == INVALID_HANDLE_VALUE)
				return sdk::unexpected(process_error("executable", "open"));
			const auto required =
				GetFinalPathNameByHandleW(file.get(), nullptr, 0U, FILE_NAME_NORMALIZED);
			if (required == 0U || required > 32767U)
				return sdk::unexpected(process_error("executable", "canonicalize"));
			std::wstring output(required, L'\0');
			const auto written = GetFinalPathNameByHandleW(
				file.get(), output.data(), required, FILE_NAME_NORMALIZED);
			if (written == 0U || written >= required)
				return sdk::unexpected(process_error("executable", "canonicalize"));
			output.resize(written);
			return output;
		}

		void append_quoted(std::wstring& command, const std::wstring& argument)
		{
			command.push_back(L'"');
			std::size_t backslashes{};
			for (const auto character : argument)
			{
				if (character == L'\\')
				{
					++backslashes;
					continue;
				}
				if (character == L'"')
					command.append(backslashes * 2U + 1U, L'\\');
				else
					command.append(backslashes, L'\\');
				backslashes = 0U;
				command.push_back(character);
			}
			command.append(backslashes * 2U, L'\\');
			command.push_back(L'"');
		}

		[[nodiscard]] sdk::result<std::wstring>
		command_line(const std::wstring& executable, const std::vector<std::wstring>& arguments)
		{
			std::size_t size = executable.size() + 3U;
			for (const auto& argument : arguments)
			{
				if (argument.size() > 32767U || size > 32767U - argument.size() - 3U)
					return sdk::unexpected(process_error("arguments", "command-line-length"));
				size += argument.size() + 3U;
			}
			std::wstring output;
			output.reserve(size);
			append_quoted(output, executable);
			for (const auto& argument : arguments)
			{
				output.push_back(L' ');
				append_quoted(output, argument);
			}
			return output;
		}
	} // namespace

	sdk::result<msvc_process_result> run_msvc_process(const std::wstring& executable,
													  const std::vector<std::wstring>& arguments)
	{
		auto canonical = canonical_executable(executable);
		if (!canonical)
			return sdk::unexpected(std::move(canonical.error()));
		wchar_t module[32768]{};
		const auto module_size = GetModuleFileNameW(nullptr, module, 32768U);
		if (module_size == 0U || module_size == 32768U)
			return sdk::unexpected(process_error("proxy", "module-path"));
		auto proxy = canonical_executable(std::wstring{module, module_size});
		if (!proxy)
			return sdk::unexpected(std::move(proxy.error()));
		if (CompareStringOrdinal(proxy->c_str(), -1, canonical->c_str(), -1, TRUE) == CSTR_EQUAL)
			return sdk::unexpected(process_error("executable", "self-recursion"));
		auto command = command_line(*canonical, arguments);
		if (!command)
			return sdk::unexpected(std::move(command.error()));
		command->push_back(L'\0');
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION process{};
		if (!CreateProcessW(canonical->c_str(),
							command->data(),
							nullptr,
							nullptr,
							TRUE,
							0U,
							nullptr,
							nullptr,
							&startup,
							&process))
			return sdk::unexpected(process_error("executable", "launch"));
		unique_handle thread{process.hThread};
		unique_handle child{process.hProcess};
		if (WaitForSingleObject(child.get(), INFINITE) != WAIT_OBJECT_0)
			return sdk::unexpected(process_error("process", "wait"));
		DWORD exit_code{};
		if (!GetExitCodeProcess(child.get(), &exit_code))
			return sdk::unexpected(process_error("process", "exit-code"));
		return msvc_process_result{exit_code, std::move(*canonical)};
	}

	sdk::result<std::wstring> configured_msvc_compiler()
	{
		constexpr auto name = L"CXXLENS_MSVC_REAL_CL";
		const auto required = GetEnvironmentVariableW(name, nullptr, 0U);
		if (required == 0U || required > 32768U)
			return sdk::unexpected(process_error("configuration", "real-compiler-required"));
		std::wstring value(required, L'\0');
		const auto written = GetEnvironmentVariableW(name, value.data(), required);
		if (written == 0U || written >= required)
			return sdk::unexpected(process_error("configuration", "real-compiler-required"));
		value.resize(written);
		return value;
	}
} // namespace cxxlens::application_analysis_worker
