#include <chrono>
#include <memory>
#include <string>
#include <utility>

#include "dynamic_library_port.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace cxxlens::detail::runtime
{
	namespace
	{
		[[nodiscard]] unexpected
		library_error(std::string operation,
					  std::string detail,
					  const runtime_status status = runtime_status::platform_failure)
		{
			return {{status, std::move(operation) + ":" + std::move(detail), 0}};
		}

#if defined(__unix__) || defined(__APPLE__)
		class system_dynamic_library final : public dynamic_library
		{
		  public:
			explicit system_dynamic_library(void* handle) : handle_{handle} {}
			~system_dynamic_library() override
			{
				if (handle_ != nullptr)
					(void)::dlclose(handle_);
			}
			system_dynamic_library(const system_dynamic_library&) = delete;
			system_dynamic_library& operator=(const system_dynamic_library&) = delete;

			[[nodiscard]] runtime_result<void*> symbol(const std::string_view name) const override
			{
				const std::string stable_name{name};
				(void)::dlerror();
				void* value = ::dlsym(handle_, stable_name.c_str());
				if (const auto* failure = ::dlerror(); failure != nullptr || value == nullptr)
					return library_error("runtime.dynamic-library-symbol-missing", stable_name);
				return value;
			}

		  private:
			void* handle_{};
		};
#endif
	} // namespace

	runtime_result<std::shared_ptr<const dynamic_library>>
	system_dynamic_library_adapter::open(const std::vector<std::string>& candidates,
										 const request_context& context) const
	{
		if (context.cancelled())
			return library_error(
				"runtime.dynamic-library-open", "cancelled", runtime_status::cancelled);
		if (context.deadline && std::chrono::steady_clock::now() >= *context.deadline)
			return library_error(
				"runtime.dynamic-library-open", "timed-out", runtime_status::timed_out);
#if defined(__unix__) || defined(__APPLE__)
		for (const auto& candidate : candidates)
			if (void* handle = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
				handle != nullptr)
				return std::shared_ptr<const dynamic_library>{
					std::make_shared<system_dynamic_library>(handle)};
		return library_error("runtime.dynamic-library-unavailable", "no-candidate-loaded");
#else
		(void)candidates;
		return library_error("runtime.dynamic-library-unavailable", "platform-unsupported");
#endif
	}
} // namespace cxxlens::detail::runtime
