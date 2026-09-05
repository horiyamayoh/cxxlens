#include "llvm/clang23_gcc_replay/clangcl_sandbox_process_port_internal.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <aclapi.h>
#include <userenv.h>
#include <windows.h>

namespace cxxlens::detail::clang23_gcc_replay
{
	namespace
	{
		constexpr std::size_t maximum_protocol_output_bytes = 32U * 1024U * 1024U;
		constexpr std::size_t maximum_diagnostic_bytes = 64U * 1024U;
		constexpr DWORD wall_timeout_milliseconds = 120000U;
		constexpr SIZE_T process_memory_bytes = SIZE_T{1024U} * 1024U * 1024U;
		constexpr LONGLONG process_cpu_time_100ns = 120LL * 10000000LL;

		[[nodiscard]] sdk::error failure(std::string field, std::string detail)
		{
			return {
				"application-analysis.replay-provider-failed", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string win32_detail(const DWORD error)
		{
			return "win32-" + std::to_string(error);
		}

		class unique_handle
		{
		  public:
			unique_handle() = default;
			explicit unique_handle(HANDLE value) : value_{value} {}
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
			unique_handle& operator=(unique_handle&& other) noexcept
			{
				if (this != &other)
				{
					unique_handle replacement{std::move(other)};
					std::swap(value_, replacement.value_);
				}
				return *this;
			}
			[[nodiscard]] HANDLE get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] HANDLE release() noexcept
			{
				return std::exchange(value_, nullptr);
			}
			[[nodiscard]] explicit operator bool() const noexcept
			{
				return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
			}

		  private:
			HANDLE value_{};
		};

		struct appcontainer_sid_deleter
		{
			void operator()(void* sid) const noexcept
			{
				if (sid != nullptr)
					FreeSid(sid);
			}
		};
		using unique_sid = std::unique_ptr<void, appcontainer_sid_deleter>;

		struct local_deleter
		{
			void operator()(void* value) const noexcept
			{
				if (value != nullptr)
					LocalFree(value);
			}
		};
		using unique_local = std::unique_ptr<void, local_deleter>;

		class attribute_list
		{
		  public:
			explicit attribute_list(const DWORD count)
			{
				SIZE_T bytes{};
				InitializeProcThreadAttributeList(nullptr, count, 0U, &bytes);
				storage_.resize(bytes);
				value_ = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
				valid_ = InitializeProcThreadAttributeList(value_, count, 0U, &bytes) != FALSE;
			}
			~attribute_list()
			{
				if (valid_)
					DeleteProcThreadAttributeList(value_);
			}
			attribute_list(const attribute_list&) = delete;
			attribute_list& operator=(const attribute_list&) = delete;
			[[nodiscard]] bool valid() const noexcept
			{
				return valid_;
			}
			[[nodiscard]] PPROC_THREAD_ATTRIBUTE_LIST get() const noexcept
			{
				return value_;
			}

		  private:
			std::vector<std::byte> storage_;
			PPROC_THREAD_ATTRIBUTE_LIST value_{};
			bool valid_{};
		};

		class temporary_directory
		{
		  public:
			[[nodiscard]] static sdk::result<temporary_directory> create()
			{
				std::array<wchar_t, MAX_PATH + 1U> temporary_root{};
				const auto root_size =
					GetTempPathW(static_cast<DWORD>(temporary_root.size()), temporary_root.data());
				if (root_size == 0U || root_size >= temporary_root.size())
					return sdk::unexpected(
						failure("sandbox_directory", win32_detail(GetLastError())));
				std::array<wchar_t, MAX_PATH + 1U> candidate{};
				if (GetTempFileNameW(temporary_root.data(), L"cxl", 0U, candidate.data()) == 0U)
					return sdk::unexpected(
						failure("sandbox_directory", win32_detail(GetLastError())));
				if (DeleteFileW(candidate.data()) == FALSE ||
					CreateDirectoryW(candidate.data(), nullptr) == FALSE)
					return sdk::unexpected(
						failure("sandbox_directory", win32_detail(GetLastError())));
				return temporary_directory{std::filesystem::path{candidate.data()}};
			}

			temporary_directory(temporary_directory&& other) noexcept
				: path_{std::move(other.path_)}
			{
				other.path_.clear();
			}
			temporary_directory& operator=(temporary_directory&&) = delete;
			~temporary_directory()
			{
				std::error_code ignored;
				std::filesystem::remove_all(path_, ignored);
			}
			[[nodiscard]] const std::filesystem::path& path() const noexcept
			{
				return path_;
			}

		  private:
			explicit temporary_directory(std::filesystem::path path) : path_{std::move(path)} {}
			std::filesystem::path path_;
		};

		[[nodiscard]] sdk::result<unique_sid> appcontainer_sid()
		{
			PSID raw{};
			const auto created =
				CreateAppContainerProfile(L"cxxlens.clangcl.worker.23",
										  L"cxxlens clang-cl worker 23",
										  L"Isolated cxxlens application-analysis worker",
										  nullptr,
										  0U,
										  &raw);
			if (FAILED(created))
			{
				const auto derived =
					DeriveAppContainerSidFromAppContainerName(L"cxxlens.clangcl.worker.23", &raw);
				if (FAILED(derived))
					return sdk::unexpected(failure("appcontainer", "profile-unavailable"));
			}
			return unique_sid{raw};
		}

		[[nodiscard]] sdk::result<void> grant_appcontainer_access(const std::filesystem::path& path,
																  PSID sid)
		{
			PACL existing_acl{};
			PSECURITY_DESCRIPTOR raw_descriptor{};
			const auto get_error = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),
														 SE_FILE_OBJECT,
														 DACL_SECURITY_INFORMATION,
														 nullptr,
														 nullptr,
														 &existing_acl,
														 nullptr,
														 &raw_descriptor);
			unique_local descriptor{raw_descriptor};
			if (get_error != ERROR_SUCCESS)
				return sdk::unexpected(failure("appcontainer_acl", win32_detail(get_error)));
			EXPLICIT_ACCESSW access{};
			access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_EXECUTE;
			access.grfAccessMode = GRANT_ACCESS;
			access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
			access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
			access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
			access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
			PACL raw_acl{};
			const auto acl_error = SetEntriesInAclW(1U, &access, existing_acl, &raw_acl);
			unique_local acl{raw_acl};
			if (acl_error != ERROR_SUCCESS)
				return sdk::unexpected(failure("appcontainer_acl", win32_detail(acl_error)));
			const auto set_error = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),
														 SE_FILE_OBJECT,
														 DACL_SECURITY_INFORMATION,
														 nullptr,
														 nullptr,
														 static_cast<PACL>(acl.get()),
														 nullptr);
			if (set_error != ERROR_SUCCESS)
				return sdk::unexpected(failure("appcontainer_acl", win32_detail(set_error)));
			return {};
		}

		[[nodiscard]] sdk::result<std::filesystem::path>
		copy_worker_runtime(const std::filesystem::path& sandbox_directory)
		{
			std::vector<wchar_t> executable(32768U);
			const auto size = GetModuleFileNameW(
				nullptr, executable.data(), static_cast<DWORD>(executable.size()));
			if (size == 0U || size >= executable.size())
				return sdk::unexpected(failure("worker_path", win32_detail(GetLastError())));
			const std::filesystem::path source{std::wstring_view{executable.data(), size}};
			const auto destination = sandbox_directory / source.filename();
			std::error_code error;
			std::filesystem::copy_file(
				source, destination, std::filesystem::copy_options::overwrite_existing, error);
			if (error)
				return sdk::unexpected(failure("worker_copy", error.message()));
			for (const auto& entry :
				 std::filesystem::directory_iterator{source.parent_path(), error})
			{
				if (error)
					break;
				if (!entry.is_regular_file(error) || error || entry.path().extension() != L".dll")
					continue;
				std::filesystem::copy_file(entry.path(),
										   sandbox_directory / entry.path().filename(),
										   std::filesystem::copy_options::overwrite_existing,
										   error);
				if (error)
					break;
			}
			if (error)
				return sdk::unexpected(failure("worker_copy", error.message()));
			return destination;
		}

		[[nodiscard]] std::wstring widen_ascii(const std::string_view value)
		{
			return {value.begin(), value.end()};
		}

		[[nodiscard]] std::vector<wchar_t>
		environment_block(const provider_worker_authority& authority,
						  const sdk::import_limits limits)
		{
			std::vector<std::pair<std::wstring, std::wstring>> values{
				{L"CXXLENS_PROVIDER_MANIFEST", widen_ascii(authority.host.provider_manifest)},
				{L"CXXLENS_PROVIDER_ID", widen_ascii(authority.provider_id)},
				{L"CXXLENS_PROVIDER_BINARY_DIGEST", widen_ascii(authority.provider_binary_digest)},
				{L"CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST",
				 widen_ascii(authority.provider_semantic_contract_digest)},
				{L"CXXLENS_PROVIDER_SANDBOX_POLICY_DIGEST",
				 widen_ascii(authority.sandbox_policy_digest)},
				{L"CXXLENS_PROVIDER_TASK_ID", widen_ascii(authority.host.task.task_id)},
				{L"CXXLENS_PROVIDER_TASK_INPUT_DIGEST",
				 widen_ascii(authority.host.task.task_input_digest)},
				{L"CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST",
				 widen_ascii(authority.host.task.normalized_invocation_digest)},
				{L"CXXLENS_PROVIDER_TOOLCHAIN_DIGEST",
				 widen_ascii(authority.host.task.toolchain_digest)},
				{L"CXXLENS_PROVIDER_ENVIRONMENT_DIGEST",
				 widen_ascii(authority.host.task.environment_digest)},
				{L"CXXLENS_PROVIDER_PROTOCOL_MAJOR",
				 std::to_wstring(authority.host.limits.protocol_major)},
				{L"CXXLENS_PROVIDER_PROTOCOL_MINOR",
				 std::to_wstring(authority.host.limits.maximum_minor)},
				{L"CXXLENS_IMPORT_MAXIMUM_BUNDLE_BYTES",
				 std::to_wstring(limits.maximum_bundle_bytes)},
				{L"CXXLENS_IMPORT_MAXIMUM_NESTING_DEPTH",
				 std::to_wstring(limits.maximum_nesting_depth)},
				{L"CXXLENS_IMPORT_MAXIMUM_COMPILE_UNITS",
				 std::to_wstring(limits.maximum_compile_units)},
				{L"CXXLENS_IMPORT_MAXIMUM_ARGUMENTS_PER_UNIT",
				 std::to_wstring(limits.maximum_arguments_per_unit)},
				{L"CXXLENS_IMPORT_MAXIMUM_AUXILIARY_FILES_PER_UNIT",
				 std::to_wstring(limits.maximum_auxiliary_files_per_unit)},
				{L"CXXLENS_IMPORT_MAXIMUM_ENVIRONMENT_EFFECTS_PER_UNIT",
				 std::to_wstring(limits.maximum_environment_effects_per_unit)},
				{L"CXXLENS_IMPORT_MAXIMUM_PATH_MAPPINGS",
				 std::to_wstring(limits.maximum_path_mappings)},
				{L"CXXLENS_IMPORT_MAXIMUM_STRING_BYTES",
				 std::to_wstring(limits.maximum_string_bytes)},
				{L"CXXLENS_IMPORT_MAXIMUM_TOTAL_METADATA_BYTES",
				 std::to_wstring(limits.maximum_total_metadata_bytes)},
				{L"CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_MEMBERS",
				 std::to_wstring(limits.maximum_source_closure_members)},
				{L"CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURES",
				 std::to_wstring(limits.maximum_source_closures)},
				{L"CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_BLOBS",
				 std::to_wstring(limits.maximum_source_closure_blobs)},
				{L"CXXLENS_IMPORT_MAXIMUM_SOURCE_CLOSURE_BYTES",
				 std::to_wstring(limits.maximum_source_closure_bytes)},
			};
			std::array<wchar_t, 32768U> system_root{};
			const auto system_root_size = GetEnvironmentVariableW(
				L"SystemRoot", system_root.data(), static_cast<DWORD>(system_root.size()));
			if (system_root_size != 0U && system_root_size < system_root.size())
				values.emplace_back(L"SystemRoot",
									std::wstring{system_root.data(), system_root_size});
			std::ranges::sort(values, {}, &std::pair<std::wstring, std::wstring>::first);
			std::vector<wchar_t> block;
			for (const auto& [name, value] : values)
			{
				block.insert(block.end(), name.begin(), name.end());
				block.push_back(L'=');
				block.insert(block.end(), value.begin(), value.end());
				block.push_back(L'\0');
			}
			block.push_back(L'\0');
			return block;
		}

		[[nodiscard]] sdk::result<std::pair<unique_handle, unique_handle>> make_pipe()
		{
			SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
			HANDLE read{};
			HANDLE write{};
			if (CreatePipe(&read, &write, &attributes, 0U) == FALSE)
				return sdk::unexpected(failure("pipe", win32_detail(GetLastError())));
			return std::pair{unique_handle{read}, unique_handle{write}};
		}

		[[nodiscard]] sdk::result<void> make_parent_only(HANDLE handle)
		{
			if (SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0U) == FALSE)
				return sdk::unexpected(failure("pipe_inheritance", win32_detail(GetLastError())));
			return {};
		}

		class windows_clangcl_sandbox_process_port final : public clangcl_sandbox_process_port
		{
		  public:
			[[nodiscard]] sdk::result<std::vector<std::byte>>
			execute(const std::span<const std::byte> host_transcript,
					const provider_worker_authority& authority,
					const sdk::import_limits limits) const override
			{
				if (auto valid = limits.validate(); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (host_transcript.size() > limits.maximum_bundle_bytes)
					return sdk::unexpected(failure("stdin", "limit-exceeded"));
				auto sid = appcontainer_sid();
				if (!sid)
					return sdk::unexpected(std::move(sid.error()));
				auto directory = temporary_directory::create();
				if (!directory)
					return sdk::unexpected(std::move(directory.error()));
				if (auto granted = grant_appcontainer_access(directory->path(), sid->get());
					!granted)
					return sdk::unexpected(std::move(granted.error()));
				auto executable = copy_worker_runtime(directory->path());
				if (!executable)
					return sdk::unexpected(std::move(executable.error()));
				std::error_code directory_error;
				for (const auto& entry :
					 std::filesystem::directory_iterator{directory->path(), directory_error})
				{
					if (directory_error)
						break;
					if (auto granted = grant_appcontainer_access(entry.path(), sid->get());
						!granted)
						return sdk::unexpected(std::move(granted.error()));
				}
				if (directory_error)
					return sdk::unexpected(failure("worker_copy", directory_error.message()));

				auto stdin_pipe = make_pipe();
				auto stdout_pipe = make_pipe();
				auto stderr_pipe = make_pipe();
				if (!stdin_pipe || !stdout_pipe || !stderr_pipe)
					return sdk::unexpected(failure("pipe", "creation-failed"));
				if (auto value = make_parent_only(stdin_pipe->second.get()); !value)
					return sdk::unexpected(std::move(value.error()));
				if (auto value = make_parent_only(stdout_pipe->first.get()); !value)
					return sdk::unexpected(std::move(value.error()));
				if (auto value = make_parent_only(stderr_pipe->first.get()); !value)
					return sdk::unexpected(std::move(value.error()));

				unique_handle job{CreateJobObjectW(nullptr, nullptr)};
				if (!job)
					return sdk::unexpected(failure("job_object", win32_detail(GetLastError())));
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits{};
				job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
					JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY |
					JOB_OBJECT_LIMIT_PROCESS_TIME;
				job_limits.BasicLimitInformation.ActiveProcessLimit = 1U;
				job_limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart =
					process_cpu_time_100ns;
				job_limits.ProcessMemoryLimit = process_memory_bytes;
				if (SetInformationJobObject(job.get(),
											JobObjectExtendedLimitInformation,
											&job_limits,
											sizeof(job_limits)) == FALSE)
					return sdk::unexpected(failure("job_object", win32_detail(GetLastError())));

				attribute_list attributes{2U};
				if (!attributes.valid())
					return sdk::unexpected(
						failure("process_attributes", win32_detail(GetLastError())));
				SECURITY_CAPABILITIES capabilities{};
				capabilities.AppContainerSid = sid->get();
				if (UpdateProcThreadAttribute(attributes.get(),
											  0U,
											  PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
											  &capabilities,
											  sizeof(capabilities),
											  nullptr,
											  nullptr) == FALSE)
					return sdk::unexpected(
						failure("appcontainer_attribute", win32_detail(GetLastError())));
				std::array inherited_handles{
					stdin_pipe->first.get(), stdout_pipe->second.get(), stderr_pipe->second.get()};
				if (UpdateProcThreadAttribute(attributes.get(),
											  0U,
											  PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
											  inherited_handles.data(),
											  sizeof(inherited_handles),
											  nullptr,
											  nullptr) == FALSE)
					return sdk::unexpected(failure("handle_list", win32_detail(GetLastError())));

				STARTUPINFOEXW startup{};
				startup.StartupInfo.cb = sizeof(startup);
				startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
				startup.StartupInfo.hStdInput = stdin_pipe->first.get();
				startup.StartupInfo.hStdOutput = stdout_pipe->second.get();
				startup.StartupInfo.hStdError = stderr_pipe->second.get();
				startup.lpAttributeList = attributes.get();
				auto environment = environment_block(authority, limits);
				std::wstring command = L"\"" + executable->wstring() + L"\" --sandbox-child";
				PROCESS_INFORMATION process{};
				if (CreateProcessW(executable->c_str(),
								   command.data(),
								   nullptr,
								   nullptr,
								   TRUE,
								   CREATE_SUSPENDED | CREATE_NO_WINDOW |
									   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
								   environment.data(),
								   directory->path().c_str(),
								   &startup.StartupInfo,
								   &process) == FALSE)
					return sdk::unexpected(failure("process_create", win32_detail(GetLastError())));
				unique_handle process_handle{process.hProcess};
				unique_handle thread_handle{process.hThread};
				if (AssignProcessToJobObject(job.get(), process_handle.get()) == FALSE)
				{
					TerminateProcess(process_handle.get(), 1U);
					return sdk::unexpected(failure("job_assign", win32_detail(GetLastError())));
				}
				unique_handle token;
				HANDLE raw_token{};
				if (OpenProcessToken(process_handle.get(), TOKEN_QUERY, &raw_token) == FALSE)
				{
					TerminateJobObject(job.get(), 1U);
					return sdk::unexpected(
						failure("appcontainer_token", win32_detail(GetLastError())));
				}
				token = unique_handle{raw_token};
				DWORD is_appcontainer{};
				DWORD returned{};
				if (GetTokenInformation(token.get(),
										TokenIsAppContainer,
										&is_appcontainer,
										sizeof(is_appcontainer),
										&returned) == FALSE ||
					is_appcontainer == 0U)
				{
					TerminateJobObject(job.get(), 1U);
					return sdk::unexpected(failure("appcontainer_token", "not-appcontainer"));
				}
				if (ResumeThread(thread_handle.get()) == std::numeric_limits<DWORD>::max())
				{
					TerminateJobObject(job.get(), 1U);
					return sdk::unexpected(failure("process_resume", win32_detail(GetLastError())));
				}
				stdin_pipe->first = {};
				stdout_pipe->second = {};
				stderr_pipe->second = {};

				std::atomic_bool io_failed{};
				std::vector<std::byte> protocol_output;
				std::vector<std::byte> diagnostics;
				auto read_bounded = [&](HANDLE source,
										std::vector<std::byte>& destination,
										const std::size_t maximum)
				{
					std::array<std::byte, 8192U> buffer{};
					for (;;)
					{
						DWORD count{};
						if (ReadFile(source,
									 buffer.data(),
									 static_cast<DWORD>(buffer.size()),
									 &count,
									 nullptr) == FALSE)
						{
							if (GetLastError() != ERROR_BROKEN_PIPE)
								io_failed = true;
							break;
						}
						if (count == 0U)
							break;
						if (destination.size() > maximum - count)
						{
							io_failed = true;
							TerminateJobObject(job.get(), 1U);
							break;
						}
						destination.insert(
							destination.end(), buffer.begin(), buffer.begin() + count);
					}
				};
				std::jthread output_reader{[&]
										   {
											   read_bounded(stdout_pipe->first.get(),
															protocol_output,
															maximum_protocol_output_bytes);
										   }};
				std::jthread diagnostic_reader{[&]
											   {
												   read_bounded(stderr_pipe->first.get(),
																diagnostics,
																maximum_diagnostic_bytes);
											   }};
				std::jthread input_writer{
					[&]
					{
						std::size_t offset{};
						while (offset < host_transcript.size())
						{
							const auto remaining = host_transcript.size() - offset;
							const auto chunk =
								static_cast<DWORD>(std::min<std::size_t>(remaining, 8192U));
							DWORD written{};
							if (WriteFile(stdin_pipe->second.get(),
										  host_transcript.data() + offset,
										  chunk,
										  &written,
										  nullptr) == FALSE ||
								written == 0U)
							{
								io_failed = true;
								break;
							}
							offset += written;
						}
						stdin_pipe->second = {};
					}};

				const auto waited =
					WaitForSingleObject(process_handle.get(), wall_timeout_milliseconds);
				if (waited != WAIT_OBJECT_0)
				{
					TerminateJobObject(job.get(), 1U);
					WaitForSingleObject(process_handle.get(), 5000U);
					return sdk::unexpected(failure(
						"process_wait",
						waited == WAIT_TIMEOUT ? "wall-timeout" : win32_detail(GetLastError())));
				}
				input_writer.join();
				output_reader.join();
				diagnostic_reader.join();
				DWORD exit_code{};
				if (GetExitCodeProcess(process_handle.get(), &exit_code) == FALSE ||
					exit_code != 0U)
					return sdk::unexpected(failure("child_terminal", "failed"));
				if (io_failed)
					return sdk::unexpected(failure("child_io", "failed-or-limit-exceeded"));
				return protocol_output;
			}
		};
	} // namespace

	std::unique_ptr<clangcl_sandbox_process_port> make_windows_clangcl_sandbox_process_port()
	{
		return std::make_unique<windows_clangcl_sandbox_process_port>();
	}
} // namespace cxxlens::detail::clang23_gcc_replay

#endif
