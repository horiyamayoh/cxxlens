#include "sqlite_same_process_shm_vfs_alias_registration_internal.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace cxxlens::sdk
{
	namespace
	{
		constexpr int sqlite_ok = 0;
		constexpr std::string_view registration_epoch_profile =
			"cxxlens.sqlite.shm.vfs-alias-registration-epoch.v1";
		constexpr std::string_view unregistration_epoch_profile =
			"cxxlens.sqlite.shm.vfs-alias-unregistration-epoch.v1";

		constexpr auto native_alias_lifecycle_wait_limit = std::chrono::milliseconds{100};
		std::atomic<std::uint64_t> native_alias_lifecycle_gate_state{};
		std::atomic<std::uint64_t> alias_lifecycle_sequence_process_key{};
		std::atomic<std::uint64_t> next_alias_lifecycle_sequence{1U};
		thread_local std::uint64_t native_alias_lifecycle_active_process_key{};

		[[nodiscard]] sqlite_shm_lease_rejection rejection(
			const sqlite_shm_lease_rejection_reason reason,
			const sqlite_shm_lease_recovery_action action =
				sqlite_shm_lease_recovery_action::deny_before_native_map) noexcept
		{
			return {reason, action};
		}

		[[nodiscard]] sqlite_shm_lease_rejection ambiguous_rejection() noexcept
		{
			return rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}

		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool valid_name(const std::string& value) noexcept
		{
			return !value.empty() && value.find('\0') == std::string::npos;
		}

		[[nodiscard]] std::uint64_t current_process_key() noexcept
		{
#if defined(__linux__)
			return static_cast<std::uint64_t>(::getpid());
#else
			return 1U;
#endif
		}

		class native_alias_lifecycle_scope
		{
		  public:
			native_alias_lifecycle_scope() noexcept : process_key_{current_process_key()}
			{
				if (native_alias_lifecycle_active_process_key == process_key_)
				{
					reentrant_ = true;
					return;
				}
				native_alias_lifecycle_active_process_key = process_key_;
				const auto deadline =
					std::chrono::steady_clock::now() + native_alias_lifecycle_wait_limit;
				auto observed =
					native_alias_lifecycle_gate_state.load(std::memory_order_acquire);
				for (;;)
				{
					const auto observed_process = observed >> 1U;
					const auto locked = (observed & 1U) != 0U;
					if (observed_process != process_key_ || !locked)
					{
						const auto desired = (process_key_ << 1U) | 1U;
						if (native_alias_lifecycle_gate_state.compare_exchange_weak(
								observed,
								desired,
								std::memory_order_acq_rel,
								std::memory_order_acquire))
						{
							acquired_ = true;
							prepare_sequence_source();
							return;
						}
						continue;
					}
					if (std::chrono::steady_clock::now() >= deadline)
					{
						timed_out_ = true;
						return;
					}
					std::this_thread::yield();
					observed =
						native_alias_lifecycle_gate_state.load(std::memory_order_acquire);
				}
			}

			~native_alias_lifecycle_scope() noexcept
			{
				if (acquired_)
					native_alias_lifecycle_gate_state.store(
						process_key_ << 1U, std::memory_order_release);
				if (!reentrant_)
					native_alias_lifecycle_active_process_key = 0U;
			}

			[[nodiscard]] bool acquired() const noexcept
			{
				return acquired_;
			}

			[[nodiscard]] bool reentrant() const noexcept
			{
				return reentrant_;
			}

			[[nodiscard]] bool timed_out() const noexcept
			{
				return timed_out_;
			}

		  private:
			void prepare_sequence_source() const noexcept
			{
				const auto prior = alias_lifecycle_sequence_process_key.load(
					std::memory_order_acquire);
				if (prior == process_key_)
					return;
				next_alias_lifecycle_sequence.store(1U, std::memory_order_relaxed);
				alias_lifecycle_sequence_process_key.store(
					process_key_, std::memory_order_release);
			}

			std::uint64_t process_key_{};
			bool acquired_{};
			bool reentrant_{};
			bool timed_out_{};
		};

		[[nodiscard]] sqlite_shm_lease_rejection gate_rejection(
			const native_alias_lifecycle_scope& scope) noexcept
		{
			if (scope.reentrant())
				return rejection(sqlite_shm_lease_rejection_reason::invalid_request);
			if (scope.timed_out())
				return rejection(
					sqlite_shm_lease_rejection_reason::retiring,
					sqlite_shm_lease_recovery_action::
						await_complete_attachment_gate_boundary);
			return ambiguous_rejection();
		}

		[[nodiscard]] std::uint64_t reserve_lifecycle_sequence() noexcept
		{
			auto current = next_alias_lifecycle_sequence.load(std::memory_order_relaxed);
			for (;;)
			{
				if (current == std::numeric_limits<std::uint64_t>::max())
					return 0U;
				if (next_alias_lifecycle_sequence.compare_exchange_weak(
						current,
						current + 1U,
						std::memory_order_acq_rel,
						std::memory_order_relaxed))
					return current;
			}
		}

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (auto shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_bytes(std::vector<std::byte>& output, const std::span<const std::byte> bytes)
		{
			append_u64(output, static_cast<std::uint64_t>(bytes.size()));
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		void append_string(std::vector<std::byte>& output, const std::string_view value)
		{
			append_u64(output, static_cast<std::uint64_t>(value.size()));
			const auto* begin = reinterpret_cast<const std::byte*>(value.data());
			output.insert(output.end(), begin, begin + value.size());
		}

		void append_identity(std::vector<std::byte>& output,
						 const sqlite_backend_opaque_identity& identity)
		{
			append_string(output, identity.profile);
			append_bytes(output, identity.bytes);
		}

		template <class Value>
		void append_object_representation(std::vector<std::byte>& output, const Value& value)
		{
			static_assert(std::is_trivially_copyable_v<Value>);
			const auto* begin = reinterpret_cast<const std::byte*>(std::addressof(value));
			append_bytes(output, {begin, sizeof(Value)});
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity> mint_epoch(
			const std::string_view profile,
			const sqlite_backend_opaque_identity& process_instance,
			const sqlite_backend_opaque_identity& shared_runtime_vfs_cohort,
			const sqlite_backend_opaque_identity& alias_lifetime,
			const sqlite_backend_opaque_identity& runtime_lifetime_identity,
			const sqlite_backend_opaque_identity& runtime_lifetime_pin_identity,
			const sqlite_backend_opaque_identity* registration_epoch,
			const std::string_view registered_vfs_name,
			void* const vfs_implementation,
			const sqlite_shm_vfs_alias_lifecycle_binding::find_function find,
			const sqlite_shm_vfs_alias_lifecycle_binding::register_function register_vfs,
			const sqlite_shm_vfs_alias_lifecycle_binding::unregister_function unregister_vfs) noexcept
		{
			const auto sequence = reserve_lifecycle_sequence();
			if (sequence == 0U)
				return std::nullopt;
			try
			{
				sqlite_backend_opaque_identity output;
				output.profile = std::string{profile};
				output.bytes.reserve(256U + process_instance.profile.size() +
									 process_instance.bytes.size() +
									 shared_runtime_vfs_cohort.profile.size() +
									 shared_runtime_vfs_cohort.bytes.size() +
									 alias_lifetime.profile.size() + alias_lifetime.bytes.size() +
									 runtime_lifetime_identity.profile.size() +
									 runtime_lifetime_identity.bytes.size() +
									 runtime_lifetime_pin_identity.profile.size() +
									 runtime_lifetime_pin_identity.bytes.size() +
									 registered_vfs_name.size());
				append_u64(output.bytes, sequence);
				append_identity(output.bytes, process_instance);
				append_identity(output.bytes, shared_runtime_vfs_cohort);
				append_identity(output.bytes, alias_lifetime);
				append_identity(output.bytes, runtime_lifetime_identity);
				append_identity(output.bytes, runtime_lifetime_pin_identity);
				if (registration_epoch != nullptr)
					append_identity(output.bytes, *registration_epoch);
				else
					append_u64(output.bytes, 0U);
				append_string(output.bytes, registered_vfs_name);
				append_object_representation(output.bytes, vfs_implementation);
				append_object_representation(output.bytes, find);
				append_object_representation(output.bytes, register_vfs);
				append_object_representation(output.bytes, unregister_vfs);
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

	} // namespace

	sqlite_shm_vfs_alias_lifecycle_binding::sqlite_shm_vfs_alias_lifecycle_binding(
		sqlite_shm_process_registry_handle process,
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity runtime_lifetime_identity,
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
		std::shared_ptr<void> runtime_lifetime_owner,
		std::string registered_vfs_name,
		void* const vfs_implementation,
		const find_function find,
		const register_function register_vfs,
		const unregister_function unregister_vfs) noexcept
		: process_{std::move(process)},
		  shared_runtime_vfs_cohort_{std::move(shared_runtime_vfs_cohort)},
		  alias_lifetime_{std::move(alias_lifetime)},
		  runtime_lifetime_identity_{std::move(runtime_lifetime_identity)},
		  runtime_lifetime_pin_identity_{std::move(runtime_lifetime_pin_identity)},
		  runtime_lifetime_owner_{std::move(runtime_lifetime_owner)},
		  registered_vfs_name_{std::move(registered_vfs_name)},
		  vfs_implementation_{vfs_implementation}, find_{find}, register_vfs_{register_vfs},
		  unregister_vfs_{unregister_vfs}
	{
	}

	bool sqlite_shm_vfs_alias_lifecycle_binding::valid() const noexcept
	{
		return process_.valid() && process_.registry() != nullptr &&
			valid_identity(process_.process_instance()) &&
			valid_identity(shared_runtime_vfs_cohort_) && valid_identity(alias_lifetime_) &&
			valid_identity(runtime_lifetime_identity_) &&
			valid_identity(runtime_lifetime_pin_identity_) &&
			runtime_lifetime_identity_ != runtime_lifetime_pin_identity_ &&
			runtime_lifetime_owner_ && valid_name(registered_vfs_name_) &&
			vfs_implementation_ != nullptr && find_ != nullptr && register_vfs_ != nullptr &&
			unregister_vfs_ != nullptr;
	}

	sqlite_shm_registered_vfs_alias::sqlite_shm_registered_vfs_alias(
		sqlite_shm_process_registry_handle process,
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
		sqlite_backend_opaque_identity alias_lifetime,
		sqlite_backend_opaque_identity runtime_lifetime_identity,
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
		sqlite_backend_opaque_identity registration_epoch,
		std::string registered_vfs_name,
		void* const vfs_implementation,
		const sqlite_shm_vfs_alias_lifecycle_binding::find_function find,
		const sqlite_shm_vfs_alias_lifecycle_binding::unregister_function unregister_vfs,
		sqlite_shm_registry_alias_pin alias) noexcept
		: process_{std::move(process)},
		  shared_runtime_vfs_cohort_{std::move(shared_runtime_vfs_cohort)},
		  alias_lifetime_{std::move(alias_lifetime)},
		  runtime_lifetime_identity_{std::move(runtime_lifetime_identity)},
		  runtime_lifetime_pin_identity_{std::move(runtime_lifetime_pin_identity)},
		  registration_epoch_{std::move(registration_epoch)},
		  registered_vfs_name_{std::move(registered_vfs_name)},
		  vfs_implementation_{vfs_implementation}, find_{find}, unregister_vfs_{unregister_vfs},
		  alias_{std::move(alias)}
	{
	}

	bool sqlite_shm_registered_vfs_alias::valid() const noexcept
	{
		return !terminal_failure_ && process_.valid() && alias_ && alias_->valid() &&
			vfs_implementation_ != nullptr && find_ != nullptr && unregister_vfs_ != nullptr &&
			valid_name(registered_vfs_name_) && valid_identity(registration_epoch_);
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::process_instance() const noexcept
	{
		return process_.process_instance();
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::shared_runtime_vfs_cohort() const noexcept
	{
		return shared_runtime_vfs_cohort_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::alias_lifetime() const noexcept
	{
		return alias_lifetime_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::runtime_lifetime_identity() const noexcept
	{
		return runtime_lifetime_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::runtime_lifetime_pin_identity() const noexcept
	{
		return runtime_lifetime_pin_identity_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_registered_vfs_alias::registration_epoch() const noexcept
	{
		return registration_epoch_;
	}

	std::string_view sqlite_shm_registered_vfs_alias::registered_vfs_name() const noexcept
	{
		return registered_vfs_name_;
	}

	const void* sqlite_shm_registered_vfs_alias::vfs_implementation_identity() const noexcept
	{
		return vfs_implementation_;
	}

	sqlite_same_process_shm_mapping_registry*
	sqlite_shm_registered_vfs_alias::registry() const noexcept
	{
		return process_.registry();
	}

	sqlite_shm_lease_result<void>
	sqlite_shm_registered_vfs_alias::unregister_alias() noexcept
	{
		return sqlite_same_process_shm_vfs_alias_registration_port::unregister_alias(*this);
	}

	void sqlite_shm_registered_vfs_alias::quarantine_owner() noexcept
	{
		terminal_failure_ = true;
		alias_.reset();
	}

	sqlite_shm_lease_result<sqlite_shm_registered_vfs_alias>
	sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
		sqlite_shm_vfs_alias_lifecycle_binding binding) noexcept
	{
		if (!binding.valid())
			return rejection(sqlite_shm_lease_rejection_reason::invalid_identity);
		native_alias_lifecycle_scope lifecycle_scope;
		if (!lifecycle_scope.acquired())
			return gate_rejection(lifecycle_scope);

		try
		{
			auto* const registry = binding.process_.registry();
			if (registry == nullptr)
				return rejection(sqlite_shm_lease_rejection_reason::stale_token);

			void* before{};
			try
			{
				before = binding.find_(binding.registered_vfs_name_.c_str());
			}
			catch (...)
			{
				return ambiguous_rejection();
			}
			if (before != nullptr)
				return rejection(sqlite_shm_lease_rejection_reason::invalid_request);

			auto registration_epoch = mint_epoch(registration_epoch_profile,
				binding.process_.process_instance(),
				binding.shared_runtime_vfs_cohort_,
				binding.alias_lifetime_,
				binding.runtime_lifetime_identity_,
				binding.runtime_lifetime_pin_identity_,
				nullptr,
				binding.registered_vfs_name_,
				binding.vfs_implementation_,
				binding.find_,
				binding.register_vfs_,
				binding.unregister_vfs_);
			if (!registration_epoch)
				return rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);

			const auto receipt = sqlite_shm_verified_alias_registration_receipt{
				binding.process_.process_instance(),
				binding.shared_runtime_vfs_cohort_,
				binding.alias_lifetime_,
				binding.runtime_lifetime_identity_,
				binding.runtime_lifetime_pin_identity_,
				*registration_epoch,
			};

			auto runtime_lifetime = binding.process_.adopt_runtime_lifetime(
				binding.runtime_lifetime_identity_,
				binding.runtime_lifetime_pin_identity_,
				binding.runtime_lifetime_owner_);
			if (!runtime_lifetime)
				return runtime_lifetime.error();

			auto alias_binding = sqlite_shm_registry_alias_binding{
				binding.process_.process_instance(),
				binding.shared_runtime_vfs_cohort_,
				binding.alias_lifetime_,
				std::move(runtime_lifetime.value()),
			};
			auto reserved = registry->reserve_alias(std::move(alias_binding));
			if (!reserved)
				return reserved.error();

			auto output = sqlite_shm_registered_vfs_alias{
				std::move(binding.process_),
				std::move(binding.shared_runtime_vfs_cohort_),
				std::move(binding.alias_lifetime_),
				std::move(binding.runtime_lifetime_identity_),
				std::move(binding.runtime_lifetime_pin_identity_),
				std::move(*registration_epoch),
				std::move(binding.registered_vfs_name_),
				binding.vfs_implementation_,
				binding.find_,
				binding.unregister_vfs_,
				std::move(reserved.value()),
			};

			const auto armed = registry->begin_alias_register(*output.alias_);
			if (!armed)
				return armed.error();

			int native_status{};
			void* discovered{};
			try
			{
				native_status = binding.register_vfs_(binding.vfs_implementation_, 0);
				discovered = binding.find_(output.registered_vfs_name_.c_str());
			}
			catch (...)
			{
				return ambiguous_rejection();
			}
			if (native_status != sqlite_ok || discovered != binding.vfs_implementation_)
				return ambiguous_rejection();

			const auto confirmed = registry->confirm_alias_registered(*output.alias_, receipt);
			if (!confirmed)
				return confirmed.error();
			return output;
		}
		catch (...)
		{
			return ambiguous_rejection();
		}
	}

	sqlite_shm_lease_result<void>
	sqlite_same_process_shm_vfs_alias_registration_port::unregister_alias(
		sqlite_shm_registered_vfs_alias& alias) noexcept
	{
		native_alias_lifecycle_scope lifecycle_scope;
		if (!lifecycle_scope.acquired())
			return gate_rejection(lifecycle_scope);
		if (alias.terminal_failure_ || !alias.alias_ || alias.native_unregister_started_)
			return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		if (!alias.process_.valid() || !alias.alias_->valid())
		{
			alias.quarantine_owner();
			return rejection(sqlite_shm_lease_rejection_reason::stale_token,
						 sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}

		try
		{
			auto* const registry = alias.process_.registry();
			if (registry == nullptr)
			{
				alias.quarantine_owner();
				return rejection(sqlite_shm_lease_rejection_reason::stale_token,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}
			if (!alias.unregistering_)
			{
				const auto begun = registry->begin_alias_unregister(*alias.alias_);
				if (!begun)
					return begun.error();
				alias.unregistering_ = true;
			}

			const auto quiescent = registry->poll_alias_unregister(*alias.alias_);
			if (!quiescent)
				return quiescent.error();

			auto unregistration_epoch = mint_epoch(unregistration_epoch_profile,
				alias.process_.process_instance(),
				alias.shared_runtime_vfs_cohort_,
				alias.alias_lifetime_,
				alias.runtime_lifetime_identity_,
				alias.runtime_lifetime_pin_identity_,
				&alias.registration_epoch_,
				alias.registered_vfs_name_,
				alias.vfs_implementation_,
				alias.find_,
				nullptr,
				alias.unregister_vfs_);
			if (!unregistration_epoch)
			{
				alias.quarantine_owner();
				return rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry);
			}
			const auto receipt = sqlite_shm_verified_alias_unregistration_receipt{
				alias.process_.process_instance(),
				alias.shared_runtime_vfs_cohort_,
				alias.alias_lifetime_,
				alias.runtime_lifetime_identity_,
				alias.runtime_lifetime_pin_identity_,
				alias.registration_epoch_,
				std::move(*unregistration_epoch),
			};

			alias.native_unregister_started_ = true;
			int native_status{};
			void* discovered{};
			try
			{
				native_status = alias.unregister_vfs_(alias.vfs_implementation_);
				discovered = alias.find_(alias.registered_vfs_name_.c_str());
			}
			catch (...)
			{
				alias.quarantine_owner();
				return ambiguous_rejection();
			}
			if (native_status != sqlite_ok || discovered != nullptr)
			{
				alias.quarantine_owner();
				return ambiguous_rejection();
			}

			const auto confirmed = registry->confirm_alias_unregistered(*alias.alias_, receipt);
			if (!confirmed)
			{
				alias.quarantine_owner();
				return confirmed.error();
			}
			alias.alias_.reset();
			return {};
		}
		catch (...)
		{
			alias.quarantine_owner();
			return ambiguous_rejection();
		}
	}

	void sqlite_same_process_shm_vfs_alias_registration_port::
	exhaust_lifecycle_sequence_for_testing() noexcept
	{
		alias_lifecycle_sequence_process_key.store(
			current_process_key(), std::memory_order_release);
		next_alias_lifecycle_sequence.store(
			std::numeric_limits<std::uint64_t>::max(), std::memory_order_release);
	}
} // namespace cxxlens::sdk
