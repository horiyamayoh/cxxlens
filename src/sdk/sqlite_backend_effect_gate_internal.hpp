#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/** Shared fail-closed state machine used by every filesystem-backed SQLite VFS wrapper. */
	class sqlite_backend_effect_gate_state final : public sqlite_backend_effect_gate
	{
	  public:
		sqlite_backend_effect_gate_state(
			const sqlite_backend_connection_observation_scope& connection,
			sqlite_backend_opaque_identity capability_token,
			sqlite_backend_opaque_identity connection_token,
			std::string canonical_vfs_locator,
			std::string receipt_profile,
			const bool main_handle_required,
			const std::uint64_t initial_sequence = 1U)
			: connection_{&connection}, capability_token_{std::move(capability_token)},
			  connection_token_{std::move(connection_token)},
			  canonical_vfs_locator_{std::move(canonical_vfs_locator)},
			  receipt_profile_{std::move(receipt_profile)},
			  main_handle_required_{main_handle_required}, next_sequence_{initial_sequence}
		{
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_receipt>
		activate_denied(const sqlite_backend_opaque_identity& capability_token,
						const sqlite_backend_opaque_identity& connection_token,
						const std::string_view canonical_vfs_locator) override
		{
			try
			{
				std::scoped_lock lock{mutex_};
				if (!sequence_available_locked())
				{
					validation_failed_ = true;
					return unexpected(effect_error("effect-gate-activation"));
				}
				if (activation_sealed_ || transition_in_progress_ || pending_exclusive_arm_ ||
					latest_receipt_ || capability_token != capability_token_ ||
					connection_token != connection_token_ ||
					canonical_vfs_locator != canonical_vfs_locator_)
					return unexpected(effect_error("effect-gate-activation"));

				auto validation = make_denied_validation();
				sqlite_backend_effect_arm_receipt receipt{
					receipt_profile_ + ".effect-denied.v1",
					capability_token_,
					connection_token_,
					canonical_vfs_locator_,
					{},
					std::move(validation),
					sqlite_backend_effect_stage::denied,
					next_sequence_,
					false,
				};
				latest_receipt_ = receipt;
				advance_sequence_locked();
				stage_ = sqlite_backend_effect_stage::denied;
				activation_sealed_ = true;
				return receipt;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
			catch (const std::length_error&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_receipt>
		arm_now(sqlite_backend_effect_arm_request request) override
		{
			auto observed = connection_->snapshot();
			if (!observed)
				return unexpected(std::move(observed.error()));
			try
			{
				std::scoped_lock lock{mutex_};
				if (!sequence_available_locked())
				{
					validation_failed_ = true;
					return unexpected(effect_error("effect-gate-request"));
				}
				if (transition_in_progress_ || pending_exclusive_arm_ ||
					!request_matches_locked(request, *observed))
					return unexpected(effect_error("effect-gate-request"));
				transition_in_progress_ = true;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
			catch (const std::length_error&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
			return finish_arm(std::move(request), false);
		}

		[[nodiscard]] result<void>
		install_arm_on_exclusive_lock(sqlite_backend_effect_arm_request request) override
		{
			auto observed = connection_->snapshot();
			if (!observed)
				return unexpected(std::move(observed.error()));
			try
			{
				std::scoped_lock lock{mutex_};
				if (!sequence_available_locked())
				{
					validation_failed_ = true;
					return unexpected(effect_error("effect-gate-request"));
				}
				if (transition_in_progress_ || pending_exclusive_arm_ ||
					!request_matches_locked(request, *observed))
					return unexpected(effect_error("effect-gate-request"));
				pending_exclusive_arm_ = std::move(request);
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
			catch (const std::length_error&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
		}

		[[nodiscard]] sqlite_backend_effect_stage stage() const noexcept override
		{
			try
			{
				std::scoped_lock lock{mutex_};
				return stage_;
			}
			catch (...)
			{
				return sqlite_backend_effect_stage::denied;
			}
		}

		[[nodiscard]] bool enforcement_active() const noexcept override
		{
			return true;
		}

		[[nodiscard]] std::optional<sqlite_backend_effect_arm_receipt>
		latest_receipt() const override
		{
			try
			{
				std::scoped_lock lock{mutex_};
				return latest_receipt_;
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
			catch (const std::length_error&)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool permits_persistent_effect(const bool shm_coordination) const noexcept
		{
			try
			{
				std::scoped_lock lock{mutex_};
				if (!activation_sealed_ || transition_in_progress_ || validation_failed_)
					return false;
				return stage_ == sqlite_backend_effect_stage::fully_armed ||
					(shm_coordination &&
					 stage_ == sqlite_backend_effect_stage::wal_shm_coordination_only);
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] bool has_pending_exclusive_arm() const noexcept
		{
			try
			{
				std::scoped_lock lock{mutex_};
				return pending_exclusive_arm_.has_value();
			}
			catch (...)
			{
				return true;
			}
		}

		[[nodiscard]] result<bool> apply_pending_exclusive_arm()
		{
			auto observed = connection_->snapshot();
			if (!observed)
				return unexpected(std::move(observed.error()));
			sqlite_backend_effect_arm_request request;
			try
			{
				std::scoped_lock lock{mutex_};
				if (!pending_exclusive_arm_)
					return false;
				if (transition_in_progress_ ||
					!request_matches_locked(*pending_exclusive_arm_, *observed))
					return unexpected(effect_error("effect-gate-request"));
				transition_in_progress_ = true;
				request = std::move(*pending_exclusive_arm_);
				pending_exclusive_arm_.reset();
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}
			catch (const std::length_error&)
			{
				return unexpected(effect_error("effect-gate-allocation"));
			}

			auto armed = finish_arm(std::move(request), true);
			if (!armed)
				return unexpected(std::move(armed.error()));
			return true;
		}

	  private:
		[[nodiscard]] static error effect_error(std::string detail)
		{
			return {"store.backend-unavailable", "sqlite", std::move(detail)};
		}

		static void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (std::uint32_t shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		static void append_bytes(std::vector<std::byte>& output, const std::string_view value)
		{
			append_u64(output, static_cast<std::uint64_t>(value.size()));
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		}

		[[nodiscard]] sqlite_backend_opaque_identity make_denied_validation() const
		{
			sqlite_backend_opaque_identity output{receipt_profile_ + ".effect-denied.v1", {}};
			append_bytes(output.bytes, "denied");
			append_bytes(output.bytes, capability_token_.profile);
			append_u64(output.bytes, static_cast<std::uint64_t>(capability_token_.bytes.size()));
			output.bytes.insert(
				output.bytes.end(), capability_token_.bytes.begin(), capability_token_.bytes.end());
			append_bytes(output.bytes, connection_token_.profile);
			append_u64(output.bytes, static_cast<std::uint64_t>(connection_token_.bytes.size()));
			output.bytes.insert(
				output.bytes.end(), connection_token_.bytes.begin(), connection_token_.bytes.end());
			append_bytes(output.bytes, canonical_vfs_locator_);
			return output;
		}

		[[nodiscard]] bool
		request_matches_locked(const sqlite_backend_effect_arm_request& request,
							   const sqlite_backend_connection_observation& observed) const noexcept
		{
			return activation_sealed_ && !validation_failed_ && sequence_available_locked() &&
				observed.complete && (!main_handle_required_ || observed.main_handle_open) &&
				request.target_stage != sqlite_backend_effect_stage::denied &&
				static_cast<std::uint8_t>(request.target_stage) >
				static_cast<std::uint8_t>(stage_) &&
				request.capability_token == capability_token_ &&
				request.connection_token == connection_token_ &&
				request.canonical_vfs_locator == canonical_vfs_locator_ &&
				!request.prerequisite_receipt.profile.empty() &&
				!request.prerequisite_receipt.bytes.empty() && request.authority != nullptr;
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_receipt>
		finish_arm(sqlite_backend_effect_arm_request request,
				   const bool after_underlying_exclusive_lock)
		{
			auto validation = [&]() -> result<sqlite_backend_opaque_identity>
			{
				try
				{
					return request.authority->recheck_and_seal(request, *connection_);
				}
				catch (...)
				{
					return unexpected(effect_error("effect-gate-validation"));
				}
			}();
			auto observed = connection_->snapshot();
			if (!validation || validation->profile.empty() || validation->bytes.empty() ||
				!observed || !observed->complete ||
				(main_handle_required_ && !observed->main_handle_open))
			{
				std::scoped_lock lock{mutex_};
				transition_in_progress_ = false;
				validation_failed_ = true;
				if (!validation)
					return unexpected(std::move(validation.error()));
				if (!observed)
					return unexpected(std::move(observed.error()));
				return unexpected(effect_error("effect-gate-validation"));
			}

			try
			{
				std::scoped_lock lock{mutex_};
				if (!transition_in_progress_ || validation_failed_ ||
					!sequence_available_locked() ||
					static_cast<std::uint8_t>(request.target_stage) <=
						static_cast<std::uint8_t>(stage_))
				{
					transition_in_progress_ = false;
					validation_failed_ = true;
					return unexpected(effect_error("effect-gate-transition"));
				}

				const auto sequence = next_sequence_;
				advance_sequence_locked();
				sqlite_backend_effect_arm_receipt receipt{
					receipt_profile_ + ".effect-arm.v1",
					capability_token_,
					connection_token_,
					canonical_vfs_locator_,
					std::move(request.prerequisite_receipt),
					std::move(*validation),
					request.target_stage,
					sequence,
					after_underlying_exclusive_lock,
				};
				latest_receipt_ = receipt;
				stage_ = request.target_stage;
				transition_in_progress_ = false;
				return receipt;
			}
			catch (const std::bad_alloc&)
			{
				std::scoped_lock lock{mutex_};
				transition_in_progress_ = false;
				validation_failed_ = true;
				return unexpected(effect_error("effect-gate-allocation"));
			}
			catch (const std::length_error&)
			{
				std::scoped_lock lock{mutex_};
				transition_in_progress_ = false;
				validation_failed_ = true;
				return unexpected(effect_error("effect-gate-allocation"));
			}
		}

		[[nodiscard]] bool sequence_available_locked() const noexcept
		{
			return next_sequence_ != std::numeric_limits<std::uint64_t>::max();
		}

		void advance_sequence_locked() noexcept
		{
			++next_sequence_;
		}

		const sqlite_backend_connection_observation_scope* connection_{};
		sqlite_backend_opaque_identity capability_token_;
		sqlite_backend_opaque_identity connection_token_;
		std::string canonical_vfs_locator_;
		std::string receipt_profile_;
		bool main_handle_required_{};
		mutable std::mutex mutex_;
		std::optional<sqlite_backend_effect_arm_request> pending_exclusive_arm_;
		std::optional<sqlite_backend_effect_arm_receipt> latest_receipt_;
		sqlite_backend_effect_stage stage_{sqlite_backend_effect_stage::denied};
		std::uint64_t next_sequence_;
		bool activation_sealed_{};
		bool transition_in_progress_{};
		bool validation_failed_{};
	};
} // namespace cxxlens::sdk
