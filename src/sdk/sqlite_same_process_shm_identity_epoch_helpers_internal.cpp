#include "sqlite_same_process_shm_identity_epoch_helpers_internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <unordered_map>
#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		using rejection_reason = sqlite_shm_identity_epoch_rejection_reason;

		[[nodiscard]] sqlite_shm_identity_epoch_rejection
		reject(const rejection_reason reason) noexcept
		{
			return {reason};
		}

		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] bool
		valid_process_shape(const sqlite_shm_process_identity_sample& process) noexcept
		{
			return process.pid != 0U && process.process_start_ticks != 0U &&
				process.fork_epoch != 0U && valid_identity(process.process_instance) &&
				valid_identity(process.pidfd_identity);
		}

		[[nodiscard]] bool valid_process(const sqlite_shm_process_identity_sample& process) noexcept
		{
			return valid_process_shape(process) && process.pidfd_live;
		}

		[[nodiscard]] bool valid_vfs(const sqlite_shm_vfs_identity_sample& vfs) noexcept
		{
			return vfs.forwarding_vfs != nullptr && vfs.underlying_vfs != nullptr &&
				vfs.underlying_app_data != nullptr && valid_identity(vfs.runtime_image) &&
				valid_identity(vfs.sqlite_source_id) &&
				valid_identity(vfs.shared_runtime_vfs_cohort) &&
				valid_identity(vfs.alias_lifetime) && valid_identity(vfs.registration_epoch);
		}

		[[nodiscard]] bool valid_anchor(const sqlite_shm_identity_epoch_anchor& anchor) noexcept
		{
			return valid_process(anchor.process) && valid_vfs(anchor.vfs) &&
				valid_identity(anchor.exact_file_family) && valid_identity(anchor.open_epoch) &&
				valid_identity(anchor.callback_cohort);
		}

		[[nodiscard]] bool
		valid_anchor_shape(const sqlite_shm_identity_epoch_anchor& anchor) noexcept
		{
			return valid_process_shape(anchor.process) && valid_vfs(anchor.vfs) &&
				valid_identity(anchor.exact_file_family) && valid_identity(anchor.open_epoch) &&
				valid_identity(anchor.callback_cohort);
		}

		[[nodiscard]] bool valid_page_range(const int page_number,
											const int page_size,
											const std::uint64_t byte_offset,
											const std::uint64_t byte_count) noexcept
		{
			if (page_number < 0 || page_size <= 0 ||
				byte_count != static_cast<std::uint64_t>(page_size))
				return false;
			const auto page = static_cast<std::uint64_t>(page_number);
			const auto size = static_cast<std::uint64_t>(page_size);
			if (page > std::numeric_limits<std::uint64_t>::max() / size)
				return false;
			const auto expected_offset = page * size;
			return byte_offset == expected_offset &&
				expected_offset <= std::numeric_limits<std::uint64_t>::max() - byte_count;
		}

		[[nodiscard]] bool
		valid_request(const sqlite_shm_identity_epoch_reservation_request& request) noexcept
		{
			return valid_anchor_shape(request.anchor) && request.generation != 0U &&
				valid_identity(request.generation_identity) &&
				valid_page_range(request.page_number,
								 request.page_size,
								 request.byte_offset,
								 request.byte_count);
		}

		[[nodiscard]] bool
		valid_observation(const sqlite_shm_identity_epoch_observation& observation) noexcept
		{
			return valid_anchor_shape(observation.anchor) && observation.generation != 0U &&
				valid_identity(observation.generation_identity) &&
				observation.mapping_epoch != 0U && observation.attachment_epoch != 0U &&
				valid_page_range(observation.page_number,
								 observation.page_size,
								 observation.byte_offset,
								 observation.byte_count);
		}

		template <class Value>
		[[nodiscard]] bool take_counter(Value& counter, Value& output) noexcept
		{
			if (counter == 0U)
				return false;
			output = counter;
			counter = counter == std::numeric_limits<Value>::max() ? 0U : counter + 1U;
			return true;
		}
	} // namespace

	namespace detail
	{
		enum class sqlite_shm_identity_epoch_row_phase : std::uint8_t
		{
			reserved,
			mapped,
			unmap_validated,
			retired,
			quarantined,
		};

		struct sqlite_shm_identity_epoch_row
		{
			sqlite_shm_identity_epoch_binding binding;
			sqlite_shm_identity_epoch_row_phase phase{
				sqlite_shm_identity_epoch_row_phase::reserved};
		};

		struct sqlite_shm_identity_epoch_registry_control
		{
			explicit sqlite_shm_identity_epoch_registry_control(
				sqlite_shm_identity_epoch_anchor value)
				: anchor{std::move(value)}
			{
			}

			sqlite_shm_identity_epoch_anchor anchor;
			mutable std::mutex mutex;
			std::atomic_bool quarantined{false};
			std::uint64_t next_sequence{1U};
			std::uint64_t next_mapping_epoch{1U};
			std::uint64_t next_attachment_epoch{1U};
			std::uint64_t highest_generation{};
			std::unordered_map<std::uint64_t, sqlite_backend_opaque_identity> generation_identities;
			std::unordered_map<std::uint64_t, sqlite_shm_identity_epoch_row> rows;
		};
	} // namespace detail

	sqlite_shm_identity_epoch_receipt::sqlite_shm_identity_epoch_receipt(
		std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control,
		const std::uint64_t sequence,
		sqlite_shm_identity_epoch_binding binding) noexcept
		: control_{std::move(control)}, sequence_{sequence}, binding_{std::move(binding)}
	{
	}

	bool sqlite_shm_identity_epoch_receipt::valid() const noexcept
	{
		return control_ && sequence_ != 0U &&
			!control_->quarantined.load(std::memory_order_acquire);
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::sequence() const noexcept
	{
		return sequence_;
	}

	const sqlite_shm_identity_epoch_anchor&
	sqlite_shm_identity_epoch_receipt::anchor() const noexcept
	{
		return binding_.anchor;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::generation() const noexcept
	{
		return binding_.generation;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_identity_epoch_receipt::generation_identity() const noexcept
	{
		return binding_.generation_identity;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::mapping_epoch() const noexcept
	{
		return binding_.mapping_epoch;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::attachment_epoch() const noexcept
	{
		return binding_.attachment_epoch;
	}

	int sqlite_shm_identity_epoch_receipt::page_number() const noexcept
	{
		return binding_.page_number;
	}

	int sqlite_shm_identity_epoch_receipt::page_size() const noexcept
	{
		return binding_.page_size;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::byte_offset() const noexcept
	{
		return binding_.byte_offset;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::byte_count() const noexcept
	{
		return binding_.byte_count;
	}

	const volatile void* sqlite_shm_identity_epoch_receipt::native_mapping() const noexcept
	{
		return binding_.native_mapping;
	}

	std::uint64_t sqlite_shm_identity_epoch_receipt::sealed_shm_size() const noexcept
	{
		return binding_.sealed_shm_size;
	}

	bool sqlite_shm_identity_epoch_receipt::extension_requested() const noexcept
	{
		return binding_.extension_requested;
	}

	sqlite_shm_identity_epoch_result<void> validate_sqlite_shm_process_identity(
		const sqlite_shm_process_identity_sample& expected,
		const sqlite_shm_process_identity_sample& observed) noexcept
	{
		if (expected.pid == 0U || expected.process_start_ticks == 0U || expected.fork_epoch == 0U ||
			observed.pid == 0U || observed.process_start_ticks == 0U || observed.fork_epoch == 0U ||
			!valid_identity(expected.process_instance) ||
			!valid_identity(observed.process_instance) ||
			!valid_identity(expected.pidfd_identity) || !valid_identity(observed.pidfd_identity))
			return reject(rejection_reason::invalid_process);
		if (!expected.pidfd_live || !observed.pidfd_live)
			return reject(rejection_reason::pidfd_not_live);
		if (expected.pid != observed.pid)
			return reject(rejection_reason::pid_mismatch);
		if (expected.process_start_ticks != observed.process_start_ticks)
			return reject(rejection_reason::process_start_mismatch);
		if (expected.pidfd_identity != observed.pidfd_identity)
			return reject(rejection_reason::pidfd_mismatch);
		if (expected.fork_epoch != observed.fork_epoch)
			return reject(rejection_reason::fork_epoch_mismatch);
		if (expected.process_instance != observed.process_instance)
			return reject(rejection_reason::process_instance_mismatch);
		return {};
	}

	sqlite_shm_identity_epoch_result<void>
	validate_sqlite_shm_vfs_identity(const sqlite_shm_vfs_identity_sample& expected,
									 const sqlite_shm_vfs_identity_sample& observed) noexcept
	{
		if (!valid_vfs(expected) || !valid_vfs(observed))
			return reject(rejection_reason::invalid_identity);
		if (expected.forwarding_vfs != observed.forwarding_vfs ||
			expected.underlying_vfs != observed.underlying_vfs)
			return reject(rejection_reason::vfs_mismatch);
		if (expected.underlying_app_data != observed.underlying_app_data)
			return reject(rejection_reason::vfs_app_data_mismatch);
		if (expected.runtime_image != observed.runtime_image)
			return reject(rejection_reason::runtime_image_mismatch);
		if (expected.sqlite_source_id != observed.sqlite_source_id)
			return reject(rejection_reason::source_id_mismatch);
		if (expected.shared_runtime_vfs_cohort != observed.shared_runtime_vfs_cohort)
			return reject(rejection_reason::vfs_cohort_mismatch);
		if (expected.alias_lifetime != observed.alias_lifetime)
			return reject(rejection_reason::alias_lifetime_mismatch);
		if (expected.registration_epoch != observed.registration_epoch)
			return reject(rejection_reason::registration_epoch_mismatch);
		return {};
	}

	namespace
	{
		using row_phase = detail::sqlite_shm_identity_epoch_row_phase;
		using control = detail::sqlite_shm_identity_epoch_registry_control;

		void quarantine_locked(control& state) noexcept
		{
			state.quarantined.store(true, std::memory_order_release);
			for (auto& [sequence, row] : state.rows)
			{
				(void)sequence;
				if (row.phase != row_phase::retired)
					row.phase = row_phase::quarantined;
			}
		}

		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		check_anchor_locked(const control& state,
							const sqlite_shm_identity_epoch_anchor& observed) noexcept
		{
			auto process =
				validate_sqlite_shm_process_identity(state.anchor.process, observed.process);
			if (!process)
				return process;
			auto vfs = validate_sqlite_shm_vfs_identity(state.anchor.vfs, observed.vfs);
			if (!vfs)
				return vfs;
			if (state.anchor.exact_file_family != observed.exact_file_family)
				return reject(rejection_reason::file_family_mismatch);
			if (state.anchor.open_epoch != observed.open_epoch)
				return reject(rejection_reason::open_epoch_mismatch);
			if (state.anchor.callback_cohort != observed.callback_cohort)
				return reject(rejection_reason::callback_cohort_mismatch);
			return {};
		}

		[[nodiscard]] sqlite_shm_identity_epoch_result<void>
		check_row_identity_locked(const detail::sqlite_shm_identity_epoch_row& row,
								  const sqlite_shm_identity_epoch_observation& observed,
								  const bool allow_null_expected_pointer,
								  const bool allow_growth) noexcept
		{
			if (row.binding.generation != observed.generation ||
				row.binding.generation_identity != observed.generation_identity)
				return row.binding.generation == observed.generation
					? reject(rejection_reason::generation_mismatch)
					: reject(observed.generation < row.binding.generation
								 ? rejection_reason::stale_generation
								 : rejection_reason::generation_mismatch);
			if (row.binding.mapping_epoch != observed.mapping_epoch)
				return reject(rejection_reason::mapping_epoch_mismatch);
			if (row.binding.attachment_epoch != observed.attachment_epoch)
				return reject(rejection_reason::attachment_epoch_mismatch);
			if (row.binding.page_number != observed.page_number ||
				row.binding.page_size != observed.page_size)
				return reject(rejection_reason::page_mismatch);
			if (row.binding.byte_offset != observed.byte_offset ||
				row.binding.byte_count != observed.byte_count)
				return reject(rejection_reason::range_mismatch);
			if (row.binding.native_mapping != observed.native_mapping &&
				!(allow_null_expected_pointer && row.binding.native_mapping == nullptr &&
				  observed.native_mapping != nullptr))
				return reject(rejection_reason::pointer_aba);
			if (observed.sealed_shm_size < row.binding.sealed_shm_size)
				return reject(rejection_reason::resize_regression);
			if (!allow_growth && observed.sealed_shm_size != row.binding.sealed_shm_size)
				return reject(rejection_reason::resize_not_requested);
			return {};
		}

	} // namespace

	sqlite_shm_identity_epoch_registry::sqlite_shm_identity_epoch_registry(
		std::shared_ptr<detail::sqlite_shm_identity_epoch_registry_control> control) noexcept
		: control_{std::move(control)}
	{
	}

	sqlite_shm_identity_epoch_result<sqlite_shm_identity_epoch_registry>
	sqlite_shm_identity_epoch_registry::create(sqlite_shm_identity_epoch_anchor anchor)
	{
		if (!valid_anchor(anchor))
			return reject(rejection_reason::invalid_identity);
		try
		{
			auto state = std::make_shared<detail::sqlite_shm_identity_epoch_registry_control>(
				std::move(anchor));
			return sqlite_shm_identity_epoch_registry{std::move(state)};
		}
		catch (...)
		{
			return reject(rejection_reason::invalid_request);
		}
	}

	bool sqlite_shm_identity_epoch_registry::valid() const noexcept
	{
		return control_ && !control_->quarantined.load(std::memory_order_acquire);
	}

	bool sqlite_shm_identity_epoch_registry::quarantined() const noexcept
	{
		return !control_ || control_->quarantined.load(std::memory_order_acquire);
	}

	const sqlite_shm_identity_epoch_anchor&
	sqlite_shm_identity_epoch_registry::anchor() const noexcept
	{
		static const sqlite_shm_identity_epoch_anchor empty{};
		return control_ ? control_->anchor : empty;
	}

	sqlite_shm_identity_epoch_result<void> sqlite_shm_identity_epoch_registry::check_receipt(
		detail::sqlite_shm_identity_epoch_registry_control& state,
		sqlite_shm_identity_epoch_receipt& receipt,
		detail::sqlite_shm_identity_epoch_row*& row) noexcept
	{
		if (receipt.control_.get() != &state || receipt.sequence_ == 0U)
			return reject(rejection_reason::stale_epoch);
		auto found = state.rows.find(receipt.sequence_);
		if (found == state.rows.end())
			return reject(rejection_reason::stale_epoch);
		row = &found->second;
		if (state.quarantined.load(std::memory_order_acquire) ||
			row->phase == row_phase::quarantined)
			return reject(rejection_reason::quarantined);
		return {};
	}

	sqlite_shm_identity_epoch_result<sqlite_shm_identity_epoch_receipt>
	sqlite_shm_identity_epoch_registry::reserve(
		const sqlite_shm_identity_epoch_reservation_request& request)
	{
		if (!control_)
			return reject(rejection_reason::invalid_request);
		if (!valid_request(request))
			return reject(rejection_reason::invalid_request);
		auto process =
			validate_sqlite_shm_process_identity(control_->anchor.process, request.anchor.process);
		if (!process)
		{
			control_->quarantined.store(true, std::memory_order_release);
			return reject(process.error().reason);
		}
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);

		auto vfs = validate_sqlite_shm_vfs_identity(control_->anchor.vfs, request.anchor.vfs);
		if (!vfs)
		{
			quarantine_locked(*control_);
			return reject(vfs.error().reason);
		}
		if (control_->anchor.exact_file_family != request.anchor.exact_file_family)
			return reject(rejection_reason::file_family_mismatch);
		if (control_->anchor.open_epoch != request.anchor.open_epoch)
			return reject(rejection_reason::open_epoch_mismatch);
		if (control_->anchor.callback_cohort != request.anchor.callback_cohort)
			return reject(rejection_reason::callback_cohort_mismatch);

		const auto generation = control_->generation_identities.find(request.generation);
		if (generation != control_->generation_identities.end())
		{
			if (generation->second != request.generation_identity)
				return reject(rejection_reason::generation_mismatch);
		}
		else
		{
			if (control_->highest_generation != 0U &&
				request.generation < control_->highest_generation)
				return reject(rejection_reason::stale_generation);
			try
			{
				control_->generation_identities.emplace(request.generation,
														request.generation_identity);
				control_->highest_generation =
					std::max(control_->highest_generation, request.generation);
			}
			catch (...)
			{
				quarantine_locked(*control_);
				return reject(rejection_reason::generation_exhausted);
			}
		}

		std::uint64_t sequence{};
		std::uint64_t mapping_epoch{};
		std::uint64_t attachment_epoch{};
		if (!take_counter(control_->next_sequence, sequence) ||
			!take_counter(control_->next_mapping_epoch, mapping_epoch) ||
			!take_counter(control_->next_attachment_epoch, attachment_epoch))
		{
			quarantine_locked(*control_);
			return reject(rejection_reason::generation_exhausted);
		}

		sqlite_shm_identity_epoch_binding binding{
			request.anchor,
			request.generation,
			request.generation_identity,
			mapping_epoch,
			attachment_epoch,
			request.page_number,
			request.page_size,
			request.byte_offset,
			request.byte_count,
			request.native_mapping,
			request.sealed_shm_size,
			request.extension_requested,
		};
		try
		{
			control_->rows.emplace(
				sequence, detail::sqlite_shm_identity_epoch_row{binding, row_phase::reserved});
		}
		catch (...)
		{
			quarantine_locked(*control_);
			return reject(rejection_reason::generation_exhausted);
		}
		return sqlite_shm_identity_epoch_receipt{control_, sequence, std::move(binding)};
	}

	sqlite_shm_identity_epoch_result<void> sqlite_shm_identity_epoch_registry::validate_map(
		sqlite_shm_identity_epoch_receipt& receipt,
		const sqlite_shm_identity_epoch_observation& observation) noexcept
	{
		if (!control_ || !valid_observation(observation))
			return reject(rejection_reason::invalid_request);
		auto process = validate_sqlite_shm_process_identity(control_->anchor.process,
															observation.anchor.process);
		if (!process)
		{
			control_->quarantined.store(true, std::memory_order_release);
			return reject(process.error().reason);
		}
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);
		detail::sqlite_shm_identity_epoch_row* row{};
		auto present = check_receipt(*control_, receipt, row);
		if (!present)
			return present;
		if (row->phase != row_phase::reserved)
			return reject(rejection_reason::stale_epoch);
		auto anchor = check_anchor_locked(*control_, observation.anchor);
		if (!anchor)
		{
			quarantine_locked(*control_);
			return anchor;
		}
		auto identity = check_row_identity_locked(*row, observation, true, true);
		if (!identity)
		{
			if (identity.error().reason == rejection_reason::pointer_aba ||
				identity.error().reason == rejection_reason::resize_regression ||
				identity.error().reason == rejection_reason::resize_not_requested)
				quarantine_locked(*control_);
			return identity;
		}
		if (observation.native_mapping == nullptr)
		{
			quarantine_locked(*control_);
			return reject(rejection_reason::pointer_aba);
		}
		const auto range_end = observation.byte_offset + observation.byte_count;
		if (range_end > observation.sealed_shm_size)
			return reject(rejection_reason::resize_regression);
		if (!row->binding.extension_requested &&
			observation.sealed_shm_size != row->binding.sealed_shm_size)
			return reject(rejection_reason::resize_not_requested);
		if (row->binding.extension_requested && range_end > observation.sealed_shm_size)
			return reject(rejection_reason::resize_regression);
		row->binding.native_mapping = observation.native_mapping;
		row->binding.sealed_shm_size = observation.sealed_shm_size;
		row->phase = row_phase::mapped;
		receipt.binding_ = row->binding;
		return {};
	}

	sqlite_shm_identity_epoch_result<void> sqlite_shm_identity_epoch_registry::validate_resize(
		sqlite_shm_identity_epoch_receipt& receipt,
		const sqlite_shm_identity_epoch_observation& observation,
		const std::uint64_t requested_range_end) noexcept
	{
		if (!control_ || !valid_observation(observation) || requested_range_end == 0U)
			return reject(rejection_reason::invalid_request);
		auto process = validate_sqlite_shm_process_identity(control_->anchor.process,
															observation.anchor.process);
		if (!process)
		{
			control_->quarantined.store(true, std::memory_order_release);
			return reject(process.error().reason);
		}
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);
		detail::sqlite_shm_identity_epoch_row* row{};
		auto present = check_receipt(*control_, receipt, row);
		if (!present)
			return present;
		if (row->phase != row_phase::mapped)
			return reject(rejection_reason::stale_epoch);
		if (!row->binding.extension_requested)
			return reject(rejection_reason::resize_not_requested);
		auto anchor = check_anchor_locked(*control_, observation.anchor);
		if (!anchor)
		{
			quarantine_locked(*control_);
			return anchor;
		}
		auto identity = check_row_identity_locked(*row, observation, false, true);
		if (!identity)
		{
			if (identity.error().reason == rejection_reason::pointer_aba ||
				identity.error().reason == rejection_reason::resize_regression)
				quarantine_locked(*control_);
			return identity;
		}
		if (requested_range_end <= row->binding.sealed_shm_size ||
			observation.sealed_shm_size < requested_range_end ||
			observation.sealed_shm_size <= row->binding.sealed_shm_size)
			return reject(rejection_reason::resize_not_requested);
		row->binding.sealed_shm_size = observation.sealed_shm_size;
		receipt.binding_ = row->binding;
		return {};
	}

	sqlite_shm_identity_epoch_result<void> sqlite_shm_identity_epoch_registry::validate_unmap(
		sqlite_shm_identity_epoch_receipt& receipt,
		const sqlite_shm_identity_epoch_observation& observation,
		const bool delete_flag) noexcept
	{
		if (!control_ || !valid_observation(observation))
			return reject(rejection_reason::invalid_request);
		if (delete_flag)
			return reject(rejection_reason::unmap_delete_flag);
		auto process = validate_sqlite_shm_process_identity(control_->anchor.process,
															observation.anchor.process);
		if (!process)
		{
			control_->quarantined.store(true, std::memory_order_release);
			return reject(process.error().reason);
		}
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);
		detail::sqlite_shm_identity_epoch_row* row{};
		auto present = check_receipt(*control_, receipt, row);
		if (!present)
			return present;
		if (row->phase != row_phase::mapped)
			return reject(rejection_reason::stale_epoch);
		auto anchor = check_anchor_locked(*control_, observation.anchor);
		if (!anchor)
		{
			quarantine_locked(*control_);
			return anchor;
		}
		auto identity = check_row_identity_locked(*row, observation, false, false);
		if (!identity)
		{
			if (identity.error().reason == rejection_reason::pointer_aba ||
				identity.error().reason == rejection_reason::resize_regression ||
				identity.error().reason == rejection_reason::resize_not_requested)
				quarantine_locked(*control_);
			return identity;
		}
		row->phase = row_phase::unmap_validated;
		return {};
	}

	sqlite_shm_identity_epoch_result<void>
	sqlite_shm_identity_epoch_registry::cancel(sqlite_shm_identity_epoch_receipt& receipt) noexcept
	{
		if (!control_)
			return reject(rejection_reason::stale_epoch);
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);
		detail::sqlite_shm_identity_epoch_row* row{};
		auto present = check_receipt(*control_, receipt, row);
		if (!present)
			return present;
		if (row->phase != row_phase::reserved)
			return reject(rejection_reason::stale_epoch);
		row->phase = row_phase::retired;
		receipt.control_.reset();
		receipt.sequence_ = 0U;
		return {};
	}

	sqlite_shm_identity_epoch_result<void>
	sqlite_shm_identity_epoch_registry::retire(sqlite_shm_identity_epoch_receipt& receipt) noexcept
	{
		if (!control_)
			return reject(rejection_reason::stale_epoch);
		std::lock_guard lock{control_->mutex};
		if (control_->quarantined.load(std::memory_order_acquire))
			return reject(rejection_reason::quarantined);
		detail::sqlite_shm_identity_epoch_row* row{};
		auto present = check_receipt(*control_, receipt, row);
		if (!present)
			return present;
		if (row->phase != row_phase::unmap_validated)
			return reject(rejection_reason::stale_epoch);
		row->phase = row_phase::retired;
		receipt.control_.reset();
		receipt.sequence_ = 0U;
		return {};
	}

	void sqlite_shm_identity_epoch_registry::quarantine() noexcept
	{
		if (!control_)
			return;
		// Keep this transition lock-free so a fork child can quarantine an inherited coordinator
		// even when another parent thread held the mutex at the fork cut.
		control_->quarantined.store(true, std::memory_order_release);
	}
} // namespace cxxlens::sdk
