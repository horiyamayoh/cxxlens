#include "llvm/clang22/materialization_rooted_vfs.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <cxxlens/sdk.hpp>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "llvm/clang22/materialization_sqlite_abi.hpp"
#include "sdk/sqlite_backend_observation_internal.hpp"
#include "sdk/sqlite_private_snapshot_internal.hpp"

#if defined(CXXLENS_TEST_WRAP_ROOTED_VFS_EFFECTS)
extern "C" int __real_unlinkat(int directory_descriptor, const char* path, int flags);

namespace
{
	std::mutex unlink_barrier_mutex;
	std::condition_variable unlink_barrier_condition;
	bool unlink_barrier_armed{};
	bool unlink_barrier_reached{};
	bool unlink_barrier_released{};
	unsigned int unlink_barrier_unexpected_calls{};
	constexpr std::string_view unlink_barrier_leaf{"lease.sqlite-journal"};
	constexpr auto unlink_barrier_timeout = std::chrono::seconds{10};
} // namespace

extern "C" int __wrap_unlinkat(const int directory_descriptor, const char* path, const int flags)
{
	{
		std::unique_lock lock{unlink_barrier_mutex};
		if (unlink_barrier_armed && path != nullptr && unlink_barrier_leaf == path)
		{
			if (unlink_barrier_reached)
			{
				++unlink_barrier_unexpected_calls;
				errno = EACCES;
				return -1;
			}
			unlink_barrier_reached = true;
			unlink_barrier_condition.notify_all();
			if (!unlink_barrier_condition.wait_for(lock,
												   unlink_barrier_timeout,
												   []
												   {
													   return unlink_barrier_released;
												   }))
			{
				unlink_barrier_armed = false;
				errno = EACCES;
				return -1;
			}
		}
	}
	return __real_unlinkat(directory_descriptor, path, flags);
}

#endif

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

#if defined(CXXLENS_TEST_WRAP_ROOTED_VFS_EFFECTS)
	void arm_unlink_barrier()
	{
		std::scoped_lock lock{unlink_barrier_mutex};
		unlink_barrier_armed = true;
		unlink_barrier_reached = false;
		unlink_barrier_released = false;
		unlink_barrier_unexpected_calls = 0U;
	}

	void wait_for_unlink_barrier()
	{
		std::unique_lock lock{unlink_barrier_mutex};
		require(unlink_barrier_condition.wait_for(lock,
												  unlink_barrier_timeout,
												  []
												  {
													  return unlink_barrier_reached;
												  }),
				"rooted SQLite xDelete authority barrier timed out");
	}

	void release_unlink_barrier()
	{
		{
			std::scoped_lock lock{unlink_barrier_mutex};
			unlink_barrier_released = true;
			unlink_barrier_armed = false;
		}
		unlink_barrier_condition.notify_all();
	}
#endif

	class temporary_directory
	{
	  public:
		temporary_directory()
		{
			std::array<char, 48U> pattern{};
			constexpr std::string_view value{"/tmp/cxxlens-rooted-vfs-XXXXXX"};
			std::ranges::copy(value, pattern.begin());
			const auto* created = ::mkdtemp(pattern.data());
			require(created != nullptr, "temporary rooted-VFS directory creation failed");
			path_ = created;
		}
		~temporary_directory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
		}
		[[nodiscard]] const std::string& path() const noexcept
		{
			return path_;
		}

	  private:
		std::string path_;
	};

	class long_directory_chain
	{
	  public:
		explicit long_directory_chain(const std::string_view path)
		{
			const auto root = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
			require(root >= 0, "long-path root directory open failed");
			descriptors_.push_back(root);
			std::size_t begin{};
			while (begin < path.size())
			{
				const auto end = path.find('/', begin);
				components_.emplace_back(path.substr(
					begin, end == std::string_view::npos ? path.size() - begin : end - begin));
				require(!components_.back().empty(), "long-path fixture has an empty component");
				require(::mkdirat(descriptors_.back(), components_.back().c_str(), 0700) == 0,
						"long-path directory creation failed");
				const auto child = ::openat(descriptors_.back(),
											components_.back().c_str(),
											O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				require(child >= 0, "long-path directory open failed");
				descriptors_.push_back(child);
				if (end == std::string_view::npos)
					break;
				begin = end + 1U;
			}
		}

		long_directory_chain(const long_directory_chain&) = delete;
		long_directory_chain& operator=(const long_directory_chain&) = delete;

		~long_directory_chain()
		{
			for (auto index = descriptors_.size(); index > 1U; --index)
			{
				(void)::close(descriptors_[index - 1U]);
				(void)::unlinkat(
					descriptors_[index - 2U], components_[index - 2U].c_str(), AT_REMOVEDIR);
			}
			if (!descriptors_.empty())
				(void)::close(descriptors_.front());
		}

		[[nodiscard]] bool is_regular_file(const std::string_view leaf) const
		{
			struct stat observed{};
			const std::string terminated{leaf};
			return ::fstatat(
					   descriptors_.back(), terminated.c_str(), &observed, AT_SYMLINK_NOFOLLOW) ==
				0 &&
				S_ISREG(observed.st_mode);
		}

		void remove_file(const std::string_view leaf) const
		{
			const std::string terminated{leaf};
			require(::unlinkat(descriptors_.back(), terminated.c_str(), 0) == 0 || errno == ENOENT,
					"long-path fixture file cleanup failed");
		}

	  private:
		std::vector<int> descriptors_;
		std::vector<std::string> components_;
	};

	void create_regular_file(const std::string& path);

	[[nodiscard]] sdk::sqlite_backend_opaque_identity
	effect_test_receipt(const std::string_view label)
	{
		sdk::sqlite_backend_opaque_identity output{"rooted-vfs-effect-test.v1", {}};
		for (const auto value : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		if (output.bytes.empty())
			output.bytes.push_back(std::byte{});
		return output;
	}

	class rooted_test_arm_authority final : public sdk::sqlite_backend_effect_arm_authority
	{
	  public:
		rooted_test_arm_authority(sdk::sqlite_backend_observation_capability& capability,
								  std::string locator,
								  sdk::sqlite_backend_stat_only_entry_observation expected,
								  const bool succeeds,
								  const int competing_descriptor = -1) noexcept
			: capability_{&capability}, locator_{std::move(locator)},
			  expected_{std::move(expected)}, succeeds_{succeeds},
			  competing_descriptor_{competing_descriptor}
		{
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_opaque_identity> recheck_and_seal(
			const sdk::sqlite_backend_effect_arm_request& request,
			const sdk::sqlite_backend_connection_observation_scope& connection) const override
		{
			++calls;
			auto observed = capability_->observe_entry_state_without_open(
				locator_, sdk::sqlite_backend_file_role::main_database);
			stat_only_recheck_succeeded = observed.has_value() &&
				observed->role == expected_.role && observed->state == expected_.state &&
				observed->parent_namespace_identity == expected_.parent_namespace_identity &&
				observed->object_identity == expected_.object_identity &&
				observed->directory_entry_identity == expected_.directory_entry_identity;
			lock_was_held = competing_descriptor_ < 0;
#if defined(F_OFD_SETLK)
			if (competing_descriptor_ >= 0)
			{
				struct flock competing_lock{};
				competing_lock.l_type = F_WRLCK;
				competing_lock.l_whence = SEEK_SET;
				competing_lock.l_start = 0x40000000 + 2;
				competing_lock.l_len = 510;
				errno = 0;
				const auto status = ::fcntl(competing_descriptor_, F_OFD_SETLK, &competing_lock);
				lock_was_held = status == -1 && (errno == EACCES || errno == EAGAIN);
				if (status == 0)
				{
					competing_lock.l_type = F_UNLCK;
					(void)::fcntl(competing_descriptor_, F_OFD_SETLK, &competing_lock);
				}
			}
#endif
			if (!succeeds_ || request.connection_token != connection.token() ||
				!stat_only_recheck_succeeded || !lock_was_held)
				return sdk::unexpected(
					sdk::error{"test.rooted-effect-recheck", "rooted-vfs", "validation-failed"});
			return effect_test_receipt("validated");
		}

		mutable int calls{};
		mutable bool stat_only_recheck_succeeded{};
		mutable bool lock_was_held{};

	  private:
		sdk::sqlite_backend_observation_capability* capability_{};
		std::string locator_;
		sdk::sqlite_backend_stat_only_entry_observation expected_;
		bool succeeds_{};
		int competing_descriptor_{-1};
	};

	[[nodiscard]] sdk::sqlite_backend_effect_arm_request make_effect_arm_request(
		sdk::sqlite_backend_observation_capability& capability,
		const sdk::sqlite_backend_connection_observation_scope& connection,
		const std::string_view locator,
		const sdk::sqlite_backend_effect_stage stage,
		std::shared_ptr<const sdk::sqlite_backend_effect_arm_authority> authority,
		const std::string_view label)
	{
		return {stage,
				capability.capability_token(),
				connection.token(),
				std::string{locator},
				effect_test_receipt(label),
				std::move(authority)};
	}

	class direct_sqlite
	{
	  public:
		explicit direct_sqlite(sdk::sqlite_backend_observation_capability& observation)
			: observation_{&observation}
		{
			library_ = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
			require(library_ != nullptr, "direct SQLite test library load failed");
			open_ = reinterpret_cast<open_function>(::dlsym(library_, "sqlite3_open_v2"));
			execute_ = reinterpret_cast<execute_function>(::dlsym(library_, "sqlite3_exec"));
			close_ = reinterpret_cast<close_function>(::dlsym(library_, "sqlite3_close"));
			free_ = reinterpret_cast<free_function>(::dlsym(library_, "sqlite3_free"));
			find_vfs_ = reinterpret_cast<find_vfs_function>(::dlsym(library_, "sqlite3_vfs_find"));
			require(open_ != nullptr && execute_ != nullptr && close_ != nullptr &&
						free_ != nullptr && find_vfs_ != nullptr,
					"direct SQLite test symbols unavailable");
		}

		~direct_sqlite()
		{
			if (sidecar_file_ != nullptr && sidecar_file_->methods != nullptr &&
				sidecar_file_->methods->close != nullptr)
				(void)sidecar_file_->methods->close(sidecar_file_);
			if (authority_file_ != nullptr && authority_file_->methods != nullptr &&
				authority_file_->methods->close != nullptr)
				(void)authority_file_->methods->close(authority_file_);
			if (database_ != nullptr)
				(void)close_(database_);
			if (library_ != nullptr)
				(void)::dlclose(library_);
		}

		void open(const std::string& path)
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			const auto rooted_path = synthetic_path(path);
			prepare_main_scope(rooted_path, true);
			require(open_(rooted_path.c_str(), &database_, read_write_create, rooted_vfs_name) == 0,
					"direct rooted SQLite journal database open failed");
			arm_main_now(rooted_path);
		}

		void reject_open(const std::string& path) const
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			void* rejected{};
			const auto rooted_path = synthetic_path(path);
			const auto result =
				open_(rooted_path.c_str(), &rejected, read_write_create, rooted_vfs_name);
			if (rejected != nullptr)
				(void)close_(rejected);
			require(result != 0, "direct rooted SQLite accepted an invalid database path");
		}

		void execute(const char* statement)
		{
			char* message{};
			const auto result = execute_(database_, statement, nullptr, nullptr, &message);
			if (message != nullptr)
				free_(message);
			require(result == 0, "direct rooted SQLite statement failed");
		}

		void close()
		{
			require(database_ != nullptr && close_(database_) == 0,
					"direct rooted SQLite database close failed");
			database_ = nullptr;
			reset_main_scope();
		}

		void reject_vfs_access_and_delete(const std::string& path) const
		{
			auto* vfs = rooted_vfs();
			const auto rooted_path = synthetic_path(path);
			int exists = -1;
			require(vfs->access(vfs, rooted_path.c_str(), 0, &exists) == 0 && exists == 0,
					"rooted SQLite VFS exposed an unauthenticated path through xAccess");
			require(vfs->remove(vfs, rooted_path.c_str(), 0) != 0,
					"rooted SQLite VFS deleted an unauthenticated path through xDelete");
		}

		void reject_named_open_role(const std::string& path, const int role) const
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			auto* vfs = rooted_vfs();
			require(vfs->os_file_bytes > 0, "rooted SQLite VFS reported an invalid file size");
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			const auto rooted_path = synthetic_path(path);
			const auto result =
				vfs->open(vfs, rooted_path.c_str(), file, read_write_create | role, &output_flags);
			if (result == 0 && file->methods != nullptr && file->methods->close != nullptr)
				(void)file->methods->close(file);
			require(result != 0, "rooted SQLite VFS accepted an unsupported named open role");
			require(file->methods == nullptr && output_flags == 0,
					"failed rooted SQLite xOpen exposed stale output state");
		}

		void reject_raw_named_open(const std::string& path, const int role) const
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			require(vfs->open(vfs, path.c_str(), file, read_write_create | role, &output_flags) !=
							sqlite_ok &&
						file->methods == nullptr && output_flags == 0,
					"rooted SQLite VFS accepted a raw or special SQLite filename");
			require(vfs->open(vfs, path.c_str(), nullptr, read_write_create | role, nullptr) !=
						sqlite_ok,
					"rooted SQLite VFS accepted a null sqlite3_file output");
			int exists{-1};
			require(vfs->access(vfs, path.c_str(), 0, &exists) == sqlite_ok && exists == 0,
					"rooted SQLite VFS exposed a raw name through xAccess");
			require(vfs->remove(vfs, path.c_str(), 0) != sqlite_ok,
					"rooted SQLite VFS exposed a raw name through xDelete");
			std::array<char, 128U> full_path{};
			full_path.fill('x');
			require(vfs->full_pathname(
						vfs, path.c_str(), static_cast<int>(full_path.size()), full_path.data()) !=
							sqlite_ok &&
						full_path.front() == '\0',
					"rooted SQLite VFS exposed a raw name through xFullPathname");
		}

		void reject_nonregular_named_open(const std::string& path, const int role) const
		{
			constexpr int read_only = 0x00000001;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			const auto rooted_path = synthetic_path(path);
			require(vfs->open(vfs, rooted_path.c_str(), file, read_only | role, &output_flags) !=
							sqlite_ok &&
						file->methods == nullptr && output_flags == 0,
					"rooted SQLite VFS accepted a non-regular named file");
		}

		void open_main_database_handle(const std::string& path)
		{
			const auto rooted_path = synthetic_path(path);
			prepare_main_scope(rooted_path, true);
			open_main_handle(rooted_path);
			arm_main_now(rooted_path);
		}

		void close_main_database_handle()
		{
			require(authority_file_ != nullptr && authority_file_->methods != nullptr &&
						authority_file_->methods->close != nullptr &&
						authority_file_->methods->close(authority_file_) == sqlite_ok,
					"rooted SQLite authority fixture main close failed");
			authority_file_ = nullptr;
			authority_storage_.clear();
			reset_main_scope();
		}

		void open_sidecar_handle(const std::string& path, const int role)
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			auto* vfs = rooted_vfs();
			require(sidecar_file_ == nullptr,
					"rooted SQLite authority fixture already owns a sidecar handle");
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			sidecar_storage_.resize(words);
			sidecar_file_ = reinterpret_cast<sqlite3_file*>(sidecar_storage_.data());
			sidecar_file_->methods = &poison_methods;
			const auto rooted_path = synthetic_path(path);
			int output_flags{-1};
			require(vfs->open(vfs,
							  rooted_path.c_str(),
							  sidecar_file_,
							  read_write_create | role,
							  &output_flags) == sqlite_ok &&
						sidecar_file_->methods != nullptr &&
						sidecar_file_->methods->write != nullptr,
					"rooted SQLite authority fixture sidecar open failed");
		}

		[[nodiscard]] int write_sidecar_handle(const long long offset = 0) const
		{
			constexpr std::array payload{std::byte{'i'}, std::byte{'o'}};
			require(sidecar_file_ != nullptr && sidecar_file_->methods != nullptr &&
						sidecar_file_->methods->write != nullptr,
					"rooted SQLite authority fixture has no writable sidecar handle");
			return sidecar_file_->methods->write(
				sidecar_file_, payload.data(), static_cast<int>(payload.size()), offset);
		}

		[[nodiscard]] int sync_sidecar_handle() const
		{
			require(sidecar_file_ != nullptr && sidecar_file_->methods != nullptr &&
						sidecar_file_->methods->sync != nullptr,
					"rooted SQLite authority fixture has no syncable sidecar handle");
			return sidecar_file_->methods->sync(sidecar_file_, 0);
		}

		[[nodiscard]] int lock_sidecar_handle() const
		{
			require(sidecar_file_ != nullptr && sidecar_file_->methods != nullptr &&
						sidecar_file_->methods->lock != nullptr,
					"rooted SQLite authority fixture has no lockable sidecar handle");
			return sidecar_file_->methods->lock(sidecar_file_, 1);
		}

		void close_sidecar_handle()
		{
			require(sidecar_file_ != nullptr && sidecar_file_->methods != nullptr &&
						sidecar_file_->methods->close != nullptr &&
						sidecar_file_->methods->close(sidecar_file_) == sqlite_ok,
					"rooted SQLite authority fixture sidecar close failed");
			sidecar_file_ = nullptr;
			sidecar_storage_.clear();
		}

		void exercise_effect_gate_matrix(const std::string& root_directory)
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			constexpr int main_database = 0x00000100;
			constexpr int main_journal = 0x00000800;
			constexpr int write_ahead_log = 0x00080000;
			constexpr int readonly = 8;
			constexpr int io_error = 10;
			constexpr int exclusive_lock = 4;
			constexpr int size_hint_control = 5;
			constexpr int shm_unlock = 1;
			constexpr int shm_lock = 2;
			constexpr int shm_shared = 4;
			constexpr int shm_exclusive = 8;

			const auto missing_relative = std::string{"safe/gate-missing.sqlite"};
			const auto missing_locator = synthetic_path(missing_relative);
			ensure_main_exists(missing_locator);
			{
				auto* vfs = rooted_vfs();
				const auto words =
					(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
					sizeof(std::max_align_t);
				std::vector<std::max_align_t> storage(words);
				auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
				file->methods = &poison_methods;
				int output_flags{-1};
				require(vfs->open(vfs,
								  missing_locator.c_str(),
								  file,
								  read_write_create | main_database,
								  &output_flags) == readonly &&
							file->methods == nullptr && output_flags == 0,
						"missing rooted observation scope did not deny CREATE before delegation");
			}

			const auto forgotten_relative = std::string{"safe/gate-forgotten.sqlite"};
			const auto forgotten_locator = synthetic_path(forgotten_relative);
			const auto forgotten_absolute = root_directory + "/" + forgotten_relative;
			prepare_main_scope(forgotten_locator, false);
			open_main_handle(forgotten_locator);
			require(main_gate_->stage() == sdk::sqlite_backend_effect_stage::denied &&
						!main_gate_->latest_receipt(),
					"rooted scope construction was not immediately deny-by-default");
			std::array<std::byte, 1U> read_buffer{};
			long long forgotten_hint{64};
			require(authority_file_->methods->read(authority_file_, read_buffer.data(), 1, 0) !=
							readonly &&
						authority_file_->methods->write(
							authority_file_, read_buffer.data(), 1, 0) == readonly &&
						authority_file_->methods->truncate(authority_file_, 0) == readonly &&
						authority_file_->methods->file_control(
							authority_file_, size_hint_control, &forgotten_hint) == readonly,
					"forgotten activation did not preserve read-only callback boundaries");
			volatile void* forgotten_mapping = authority_storage_.data();
			const auto forgotten_nonextend =
				authority_file_->methods->shm_map(authority_file_, 0, 4096, 0, &forgotten_mapping);
			require(forgotten_nonextend == sqlite_ok && forgotten_mapping == nullptr &&
						!std::filesystem::exists(forgotten_absolute + "-shm"),
					"non-extending rooted SHM lookup created an absent sidecar");
			forgotten_mapping = authority_storage_.data();
			require(authority_file_->methods->shm_map(
						authority_file_, 0, 4096, 1, &forgotten_mapping) == readonly &&
						forgotten_mapping == nullptr,
					"forgotten activation admitted SHM extension");
			create_regular_file(forgotten_absolute + "-journal");
			require(try_open_sidecar(forgotten_relative + "-journal", main_journal) == readonly &&
						remove_named_path(forgotten_relative + "-journal") == readonly &&
						std::filesystem::is_regular_file(forgotten_absolute + "-journal"),
					"forgotten activation admitted a sidecar create or delete");
			close_main_database_handle();
			require(::unlink((forgotten_absolute + "-journal").c_str()) == 0,
					"forgotten activation fixture cleanup failed");

			const auto rejected_relative = std::string{"safe/gate-rejected.sqlite"};
			const auto rejected_locator = synthetic_path(rejected_relative);
			const auto rejected_absolute = root_directory + "/" + rejected_relative;
			prepare_main_scope(rejected_locator, true);
			open_main_handle(rejected_locator);
			auto rejected_expected = observation_->observe_entry_state_without_open(
				rejected_locator, sdk::sqlite_backend_file_role::main_database);
			require(rejected_expected.has_value(),
					"rejected rooted effect fixture stat-only observation failed");
			const auto rejected_competitor =
				::open(rejected_absolute.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
			require(rejected_competitor >= 0,
					"rejected rooted effect fixture competing descriptor failed");
			auto rejecting_authority =
				std::make_shared<rooted_test_arm_authority>(*observation_,
															rejected_locator,
															std::move(*rejected_expected),
															false,
															rejected_competitor);
			require(main_gate_
						->install_arm_on_exclusive_lock(
							make_effect_arm_request(*observation_,
													*main_scope_,
													rejected_locator,
													sdk::sqlite_backend_effect_stage::fully_armed,
													rejecting_authority,
													"reject-full"))
						.has_value(),
					"rejected rooted effect fixture failed to install exclusive arm");
			require(authority_file_->methods->lock(authority_file_, exclusive_lock) == io_error &&
						rejecting_authority->calls == 1 &&
						rejecting_authority->stat_only_recheck_succeeded &&
						rejecting_authority->lock_was_held &&
						main_gate_->stage() == sdk::sqlite_backend_effect_stage::denied &&
						main_gate_->latest_receipt().has_value() &&
						main_gate_->latest_receipt()->sequence == 1U &&
						authority_file_->methods->write(
							authority_file_, read_buffer.data(), 1, 0) == readonly,
					"failed exclusive recheck did not stay denied with the lock retained");
			close_main_database_handle();
			require(competing_lock_can_be_acquired(rejected_competitor),
					"failed exclusive recheck leaked the rooted SQLite lock after xClose");
			release_competing_lock(rejected_competitor);
			require(::close(rejected_competitor) == 0,
					"rejected rooted effect fixture competing descriptor close failed");

			const auto matrix_relative = std::string{"safe/gate-matrix.sqlite"};
			const auto matrix_locator = synthetic_path(matrix_relative);
			const auto matrix_absolute = root_directory + "/" + matrix_relative;
			prepare_main_scope(matrix_locator, true);
			open_main_handle(matrix_locator);
			auto coordination_expected = observation_->observe_entry_state_without_open(
				matrix_locator, sdk::sqlite_backend_file_role::main_database);
			require(coordination_expected.has_value(),
					"rooted coordination fixture stat-only observation failed");
			auto coordination_authority = std::make_shared<rooted_test_arm_authority>(
				*observation_, matrix_locator, std::move(*coordination_expected), true);
			auto coordinated = main_gate_->arm_now(
				make_effect_arm_request(*observation_,
										*main_scope_,
										matrix_locator,
										sdk::sqlite_backend_effect_stage::wal_shm_coordination_only,
										coordination_authority,
										"coordination"));
			require(coordinated.has_value() && coordinated->sequence == 2U &&
						coordinated->stage ==
							sdk::sqlite_backend_effect_stage::wal_shm_coordination_only &&
						!coordinated->armed_after_underlying_exclusive_lock &&
						coordination_authority->calls == 1 &&
						coordination_authority->stat_only_recheck_succeeded,
					"rooted coordination receipt was not exact");
			volatile void* mapping = authority_storage_.data();
			require(authority_file_->methods->shm_map(authority_file_, 0, 4096, 0, &mapping) ==
							sqlite_ok &&
						mapping == nullptr &&
						std::filesystem::is_regular_file(matrix_absolute + "-shm") &&
						std::filesystem::file_size(matrix_absolute + "-shm") == 0U,
					"rooted coordination nonextend did not prepare a zero-byte SHM lock object");
			require(authority_file_->methods->shm_lock != nullptr &&
						authority_file_->methods->shm_lock(
							authority_file_, 0, 1, shm_lock | shm_exclusive) == sqlite_ok,
					"rooted coordination stage did not permit an exclusive SHM lock");
			auto exclusive_shm_snapshot = main_scope_->snapshot();
			require(exclusive_shm_snapshot.has_value() && exclusive_shm_snapshot->complete &&
						exclusive_shm_snapshot->held_shm_locks.size() == 1U &&
						exclusive_shm_snapshot->held_shm_locks.front().offset == 0 &&
						exclusive_shm_snapshot->held_shm_locks.front().count == 1 &&
						exclusive_shm_snapshot->held_shm_locks.front().mode ==
							sdk::sqlite_backend_shm_lock_mode::exclusive,
					"rooted exclusive SHM lock observation was not exact");
			require(authority_file_->methods->shm_lock(
						authority_file_, 0, 1, shm_unlock | shm_exclusive) == sqlite_ok,
					"rooted coordination stage did not permit an exclusive SHM unlock");
			auto exclusive_shm_unlocked = main_scope_->snapshot();
			require(exclusive_shm_unlocked.has_value() && exclusive_shm_unlocked->complete &&
						exclusive_shm_unlocked->held_shm_locks.empty(),
					"rooted exclusive SHM unlock left a stale observation");
			mapping = nullptr;
			require(authority_file_->methods->shm_map(authority_file_, 0, 4096, 1, &mapping) ==
							sqlite_ok &&
						mapping != nullptr,
					"rooted coordination stage did not permit SHM extension");
			require(authority_file_->methods->shm_lock(
						authority_file_, 1, 1, shm_lock | shm_shared) == sqlite_ok,
					"rooted coordination stage did not permit a shared SHM lock");
			auto shared_shm_snapshot = main_scope_->snapshot();
			require(shared_shm_snapshot.has_value() && shared_shm_snapshot->complete &&
						shared_shm_snapshot->held_shm_locks.size() == 1U &&
						shared_shm_snapshot->held_shm_locks.front().offset == 1 &&
						shared_shm_snapshot->held_shm_locks.front().count == 1 &&
						shared_shm_snapshot->held_shm_locks.front().mode ==
							sdk::sqlite_backend_shm_lock_mode::shared,
					"rooted shared SHM lock observation was not exact");
			require(authority_file_->methods->shm_lock(
						authority_file_, 1, 1, shm_unlock | shm_shared) == sqlite_ok,
					"rooted coordination stage did not permit a shared SHM unlock");
			auto shared_shm_unlocked = main_scope_->snapshot();
			require(shared_shm_unlocked.has_value() && shared_shm_unlocked->complete &&
						shared_shm_unlocked->held_shm_locks.empty(),
					"rooted shared SHM unlock left a stale observation");
			const auto coordination_wal_flags = read_write_create | write_ahead_log;
			require(try_open_sidecar(matrix_relative + "-wal",
									 write_ahead_log,
									 coordination_wal_flags) == sqlite_ok,
					"rooted coordination stage did not permit exact WAL create");
			auto coordination_snapshot = main_scope_->snapshot();
			require(
				coordination_snapshot.has_value() && coordination_snapshot->complete &&
					coordination_snapshot->shared_memory_object_identity.has_value() &&
					coordination_snapshot->shared_memory_entry_identity.has_value() &&
					!coordination_snapshot->open_events.empty() &&
					coordination_snapshot->open_events.back().role ==
						sdk::sqlite_backend_file_role::write_ahead_log &&
					coordination_snapshot->open_events.back().input_flags ==
						coordination_wal_flags &&
					coordination_snapshot->open_events.back().outcome ==
						sdk::sqlite_backend_open_outcome::succeeded &&
					coordination_snapshot->open_events.back().returned_flags.has_value() &&
					((*coordination_snapshot->open_events.back().returned_flags & 0x00000002) !=
					 0) &&
					((*coordination_snapshot->open_events.back().returned_flags & 0x00000001) ==
					 0) &&
					coordination_snapshot->open_events.back().object_identity.has_value() &&
					coordination_snapshot->open_events.back().directory_entry_identity.has_value(),
				"rooted coordination WAL/SHM observations were not identity-complete");
			require(try_open_sidecar(matrix_relative + "-wal",
									 write_ahead_log,
									 0x00000001 | 0x00000004 | write_ahead_log) == readonly,
					"rooted coordination stage admitted non-exact WAL create flags");
			long long coordination_hint{64};
			create_regular_file(matrix_absolute + "-journal");
			require(authority_file_->methods->write(authority_file_, read_buffer.data(), 1, 0) ==
							readonly &&
						authority_file_->methods->truncate(authority_file_, 0) == readonly &&
						authority_file_->methods->file_control(
							authority_file_, size_hint_control, &coordination_hint) == readonly &&
						try_open_sidecar(matrix_relative + "-journal", main_journal) == readonly &&
						remove_named_path(matrix_relative + "-journal") == readonly &&
						authority_file_->methods->shm_unmap(authority_file_, 1) == readonly &&
						std::filesystem::is_regular_file(matrix_absolute + "-journal"),
					"rooted coordination stage admitted a durable filesystem effect");

			const auto full_competitor =
				::open(matrix_absolute.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK);
			require(full_competitor >= 0, "rooted full-arm fixture competing descriptor failed");
			auto full_expected = observation_->observe_entry_state_without_open(
				matrix_locator, sdk::sqlite_backend_file_role::main_database);
			require(full_expected.has_value(), "rooted full-arm stat-only observation failed");
			auto full_authority = std::make_shared<rooted_test_arm_authority>(
				*observation_, matrix_locator, std::move(*full_expected), true, full_competitor);
			require(main_gate_
						->install_arm_on_exclusive_lock(
							make_effect_arm_request(*observation_,
													*main_scope_,
													matrix_locator,
													sdk::sqlite_backend_effect_stage::fully_armed,
													full_authority,
													"full"))
						.has_value(),
					"rooted full-arm plan installation failed");
			require(authority_file_->methods->lock(authority_file_, exclusive_lock) == sqlite_ok &&
						full_authority->calls == 1 && full_authority->stat_only_recheck_succeeded &&
						full_authority->lock_was_held &&
						main_gate_->stage() == sdk::sqlite_backend_effect_stage::fully_armed,
					"rooted exclusive lock did not recheck stat-only authority before full arm");
			auto full_receipt = main_gate_->latest_receipt();
			require(full_receipt.has_value() && full_receipt->sequence == 3U &&
						full_receipt->stage == sdk::sqlite_backend_effect_stage::fully_armed &&
						full_receipt->armed_after_underlying_exclusive_lock &&
						full_receipt->capability_token == observation_->capability_token() &&
						full_receipt->connection_token == main_scope_->token() &&
						full_receipt->prerequisite_receipt == effect_test_receipt("full") &&
						full_receipt->validation_receipt == effect_test_receipt("validated"),
					"rooted full-arm receipt/token binding was not exact");
			long long full_hint{64};
			require(authority_file_->methods->write(authority_file_, read_buffer.data(), 1, 0) ==
							sqlite_ok &&
						authority_file_->methods->truncate(authority_file_, 0) == sqlite_ok &&
						authority_file_->methods->file_control(
							authority_file_, size_hint_control, &full_hint) == sqlite_ok,
					"rooted full stage did not permit durable main-file effects");
			open_sidecar_handle(matrix_relative + "-journal", main_journal);
			require(write_sidecar_handle() == sqlite_ok,
					"rooted full stage did not permit sidecar write");
			close_sidecar_handle();
			require(remove_named_path(matrix_relative + "-journal") == sqlite_ok &&
						!std::filesystem::exists(matrix_absolute + "-journal"),
					"rooted full stage did not permit xDelete");
			require(authority_file_->methods->shm_unmap(authority_file_, 1) == sqlite_ok &&
						!std::filesystem::exists(matrix_absolute + "-shm"),
					"rooted full stage did not permit SHM removal");
			close_main_database_handle();
			require(competing_lock_can_be_acquired(full_competitor),
					"rooted full-arm xClose did not release the SQLite lock");
			release_competing_lock(full_competitor);
			require(::close(full_competitor) == 0,
					"rooted full-arm competing descriptor close failed");
		}

		[[nodiscard]] int remove_named_path(const std::string& path) const
		{
			auto* vfs = rooted_vfs();
			const auto rooted_path = synthetic_path(path);
			return vfs->remove(vfs, rooted_path.c_str(), 0);
		}

		[[nodiscard]] int
		access_named_path(const std::string& path, int& exists, const int flags = 0) const
		{
			auto* vfs = rooted_vfs();
			const auto rooted_path = synthetic_path(path);
			return vfs->access(vfs, rooted_path.c_str(), flags, &exists);
		}

		void exercise_anonymous_open_role(const int role) const
		{
			constexpr int read_write_create_delete = 0x00000002 | 0x00000004 | 0x00000008;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			require(vfs->open(vfs, nullptr, file, read_write_create_delete | role, &output_flags) ==
							sqlite_ok &&
						file->methods != nullptr && file->methods->write != nullptr &&
						file->methods->close != nullptr,
					"rooted SQLite VFS rejected an allowed anonymous transient role");
			constexpr std::array payload{std::byte{'t'}, std::byte{'m'}, std::byte{'p'}};
			require(file->methods->write(
						file, payload.data(), static_cast<int>(payload.size()), 0) == sqlite_ok &&
						file->methods->close(file) == sqlite_ok,
					"anonymous rooted SQLite memfd was not writable and closeable");
		}

		void reject_anonymous_open_role(const int role) const
		{
			constexpr int read_write_create_delete = 0x00000002 | 0x00000004 | 0x00000008;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			require(vfs->open(vfs, nullptr, file, read_write_create_delete | role, &output_flags) !=
							sqlite_ok &&
						file->methods == nullptr && output_flags == 0,
					"rooted SQLite VFS accepted a forbidden anonymous role");
		}

		void reject_delegate_escape_callbacks() const
		{
			auto* vfs = rooted_vfs();
			require(vfs->dl_open(vfs, "libsqlite3.so.0") == nullptr,
					"rooted SQLite VFS exposed dynamic-library loading");
			std::array<char, 8U> diagnostic{};
			diagnostic.fill('x');
			vfs->dl_error(vfs, static_cast<int>(diagnostic.size()), diagnostic.data());
			require(diagnostic.front() == '\0',
					"rooted SQLite VFS exposed a delegated dynamic-loader diagnostic");
			vfs->dl_error(vfs, 0, nullptr);
			require(vfs->dl_sym(vfs, library_, "sqlite3_open_v2") == nullptr,
					"rooted SQLite VFS exposed dynamic symbol lookup");

			vfs->dl_close(vfs, nullptr);

			require(vfs->set_system_call(vfs, "open", &poison_system_call) == sqlite_not_found,
					"rooted SQLite VFS exposed system-call replacement");
			require(vfs->get_system_call(vfs, "open") == nullptr &&
						vfs->next_system_call(vfs, nullptr) == nullptr,
					"rooted SQLite VFS exposed system-call discovery");
		}

		void require_main_database_replacement_detection(const std::string& path,
														 const std::string& rooted_absolute_path)
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			constexpr int main_database = 0x00000100;
			constexpr int has_moved = 20;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			const auto rooted_path = synthetic_path(path);
			prepare_main_scope(rooted_path, true);
			int output_flags{-1};
			require(vfs->open(vfs,
							  rooted_path.c_str(),
							  file,
							  read_write_create | main_database,
							  &output_flags) == sqlite_ok &&
						file->methods != nullptr && file->methods->file_control != nullptr,
					"rooted SQLite replacement-detection fixture open failed");
			int moved{-1};
			require(file->methods->file_control(file, has_moved, &moved) == sqlite_ok && moved == 0,
					"rooted SQLite VFS misclassified an unchanged main database");
			const auto displaced = rooted_absolute_path + ".displaced";
			require(::rename(rooted_absolute_path.c_str(), displaced.c_str()) == 0,
					"rooted SQLite replacement fixture rename failed");
			create_regular_file(rooted_absolute_path);
			moved = 0;
			require(file->methods->file_control(file, has_moved, &moved) == sqlite_ok && moved == 1,
					"rooted SQLite VFS did not detect main database path replacement");
			require(file->methods->close != nullptr && file->methods->close(file) == sqlite_ok,
					"rooted SQLite replacement-detection fixture close failed");
			reset_main_scope();
			require(::unlink(displaced.c_str()) == 0,
					"rooted SQLite replacement fixture cleanup failed");
		}

		void reject_shm_map_on_non_main_handle(const char* path, const int role) const
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			int output_flags{};
			const auto rooted_path = path == nullptr ? std::string{} : synthetic_path(path);
			require(vfs->open(vfs,
							  path == nullptr ? nullptr : rooted_path.c_str(),
							  file,
							  read_write_create | role,
							  &output_flags) == 0 &&
						file->methods != nullptr && file->methods->shm_map != nullptr &&
						file->methods->shm_unmap != nullptr,
					"rooted SQLite VFS non-main SHM fixture open failed");
			volatile void* mapping = storage.data();
			require(file->methods->shm_map(file, -1, 4096, 1, &mapping) != 0 && mapping == nullptr,
					"rooted SQLite VFS left a stale xShmMap failure output");
			require(file->methods->shm_map(file, 0, 4096, 1, &mapping) != 0 && mapping == nullptr,
					"rooted SQLite VFS admitted xShmMap on a non-main handle");
			require(file->methods->close != nullptr && file->methods->close(file) == 0,
					"rooted SQLite VFS non-main SHM fixture close failed");
		}

		void reject_shm_unmap_on_non_main_handle(const char* path, const int role) const
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			int output_flags{};
			const auto rooted_path = path == nullptr ? std::string{} : synthetic_path(path);
			require(vfs->open(vfs,
							  path == nullptr ? nullptr : rooted_path.c_str(),
							  file,
							  read_write_create | role,
							  &output_flags) == 0 &&
						file->methods != nullptr && file->methods->shm_unmap != nullptr,
					"rooted SQLite VFS non-main SHM-unmap fixture open failed");
			require(file->methods->shm_unmap(file, 1) != 0,
					"rooted SQLite VFS admitted xShmUnmap deletion on a non-main handle");
			require(file->methods->close != nullptr && file->methods->close(file) == 0,
					"rooted SQLite VFS non-main SHM-unmap fixture close failed");
		}

	  private:
		static constexpr const char* rooted_vfs_name = "cxxlens-rooted-vfs-v1";
		static constexpr std::string_view rooted_name_prefix{"/cxxlens-rooted-vfs-v1/"};
		static constexpr int sqlite_ok = 0;
		static constexpr int sqlite_not_found = 12;
		static constexpr sqlite3_io_methods poison_methods{};
		static void poison_system_call() {}
		[[nodiscard]] static std::string synthetic_path(const std::string_view path)
		{
			return std::string{rooted_name_prefix} + std::string{path};
		}
		using open_function = int (*)(const char*, void**, int, const char*);
		using execute_function =
			int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
		using close_function = int (*)(void*);
		using free_function = void (*)(void*);
		using find_vfs_function = sqlite3_vfs* (*)(const char*);

		[[nodiscard]] sqlite3_vfs* rooted_vfs() const
		{
			auto* output = find_vfs_(rooted_vfs_name);
			require(output != nullptr, "rooted SQLite VFS is not registered");
			return output;
		}

		void prepare_main_scope(const std::string& rooted_path, const bool activate)
		{
			require(!main_scope_ && main_gate_ == nullptr,
					"rooted SQLite fixture leaked a prior connection scope");
			ensure_main_exists(rooted_path);
			auto census = observation_->capture_namespace(rooted_path);
			require(census.has_value(), "rooted SQLite fixture namespace census failed");
			const auto main = std::ranges::find(census->entries,
												sdk::sqlite_backend_file_role::main_database,
												&sdk::sqlite_backend_entry_observation::role);
			require(main != census->entries.end(), "rooted SQLite fixture main census missing");
			require(main->state == sdk::sqlite_backend_entry_state::held_regular,
					"rooted SQLite fixture main is not a held regular object");
			auto scope = observation_->begin_connection_observation(rooted_path);
			require(scope.has_value(), "rooted SQLite fixture observation scope failed");
			main_scope_ = std::move(*scope);
			main_gate_ = main_scope_->effect_gate_port();
			require(main_gate_ != nullptr && main_gate_->enforcement_active() &&
						main_gate_->stage() == sdk::sqlite_backend_effect_stage::denied,
					"rooted SQLite fixture scope was not deny-by-default");
			if (activate)
			{
				auto denied = main_gate_->activate_denied(
					observation_->capability_token(), main_scope_->token(), rooted_path);
				require(denied.has_value() && denied->sequence == 1U,
						"rooted SQLite fixture deny activation failed");
			}
		}

		void ensure_main_exists(const std::string& rooted_path)
		{
			auto census = observation_->capture_namespace(rooted_path);
			require(census.has_value(), "rooted SQLite fixture namespace census failed");
			const auto main = std::ranges::find(census->entries,
												sdk::sqlite_backend_file_role::main_database,
												&sdk::sqlite_backend_entry_observation::role);
			require(main != census->entries.end(), "rooted SQLite fixture main census missing");
			if (main->state != sdk::sqlite_backend_entry_state::absent)
			{
				require(main->state == sdk::sqlite_backend_entry_state::held_regular,
						"rooted SQLite fixture main is not a held regular object");
				return;
			}
			auto created = observation_->exclusive_create_sync_zero_main(rooted_path);
			require(created.has_value(), "rooted SQLite fixture bootstrap failed");
			bootstrap_receipt_.emplace(std::move(*created));
		}

		void open_main_handle(const std::string& rooted_path)
		{
			constexpr int read_write_create = 0x00000002 | 0x00000004;
			constexpr int main_database = 0x00000100;
			auto* vfs = rooted_vfs();
			require(authority_file_ == nullptr,
					"rooted SQLite authority fixture already owns a main handle");
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			authority_storage_.resize(words);
			authority_file_ = reinterpret_cast<sqlite3_file*>(authority_storage_.data());
			authority_file_->methods = &poison_methods;
			int output_flags{-1};
			require(vfs->open(vfs,
							  rooted_path.c_str(),
							  authority_file_,
							  read_write_create | main_database,
							  &output_flags) == sqlite_ok &&
						authority_file_->methods != nullptr,
					"rooted SQLite authority fixture main open failed");
		}

		[[nodiscard]] int try_open_sidecar(const std::string& path,
										   const int role,
										   const int open_flags = 0x00000002 | 0x00000004) const
		{
			auto* vfs = rooted_vfs();
			const auto words =
				(static_cast<std::size_t>(vfs->os_file_bytes) + sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			std::vector<std::max_align_t> storage(words);
			auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
			file->methods = &poison_methods;
			int output_flags{-1};
			const auto rooted_path = synthetic_path(path);
			const auto status =
				vfs->open(vfs, rooted_path.c_str(), file, open_flags | role, &output_flags);
			if (status == sqlite_ok && file->methods != nullptr && file->methods->close != nullptr)
				(void)file->methods->close(file);
			return status;
		}

		[[nodiscard]] static bool competing_lock_can_be_acquired(const int descriptor) noexcept
		{
#if defined(F_OFD_SETLK)
			struct flock lock{};
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			lock.l_start = 0x40000000 + 2;
			lock.l_len = 510;
			return ::fcntl(descriptor, F_OFD_SETLK, &lock) == 0;
#else
			(void)descriptor;
			return true;
#endif
		}

		static void release_competing_lock(const int descriptor) noexcept
		{
#if defined(F_OFD_SETLK)
			struct flock lock{};
			lock.l_type = F_UNLCK;
			lock.l_whence = SEEK_SET;
			lock.l_start = 0x40000000 + 2;
			lock.l_len = 510;
			(void)::fcntl(descriptor, F_OFD_SETLK, &lock);
#else
			(void)descriptor;
#endif
		}

		void arm_main_now(const std::string& rooted_path)
		{
			auto expected = observation_->observe_entry_state_without_open(
				rooted_path, sdk::sqlite_backend_file_role::main_database);
			require(expected.has_value(), "rooted SQLite fixture stat-only observation failed");
			auto authority = std::make_shared<rooted_test_arm_authority>(
				*observation_, rooted_path, std::move(*expected), true);
			auto armed = main_gate_->arm_now(
				make_effect_arm_request(*observation_,
										*main_scope_,
										rooted_path,
										sdk::sqlite_backend_effect_stage::fully_armed,
										authority,
										"legacy-full"));
			require(armed.has_value() && armed->sequence == 2U && authority->calls == 1 &&
						authority->stat_only_recheck_succeeded,
					"rooted SQLite fixture full arm failed");
		}

		void reset_main_scope() noexcept
		{
			main_gate_ = nullptr;
			main_scope_.reset();
			bootstrap_receipt_.reset();
		}

		sdk::sqlite_backend_observation_capability* observation_{};
		void* library_{};
		void* database_{};
		std::vector<std::max_align_t> authority_storage_;
		sqlite3_file* authority_file_{};
		std::vector<std::max_align_t> sidecar_storage_;
		sqlite3_file* sidecar_file_{};
		std::shared_ptr<sdk::sqlite_backend_connection_observation_scope> main_scope_;
		sdk::sqlite_backend_effect_gate* main_gate_{};
		std::optional<sdk::sqlite_backend_zero_main_receipt> bootstrap_receipt_;
		open_function open_{};
		execute_function execute_{};
		close_function close_{};
		free_function free_{};
		find_vfs_function find_vfs_{};
	};

	void create_regular_file(const std::string& path)
	{
		const auto descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		require(descriptor >= 0, "rooted-VFS sentinel creation failed");
		require(::close(descriptor) == 0, "rooted-VFS sentinel close failed");
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_descriptor descriptor;
		descriptor.id = "company.test.rooted_vfs.v1";
		descriptor.name = "company.test.rooted_vfs";
		descriptor.version = {1U, 0U, 0U};
		descriptor.semantic_major = 1U;
		descriptor.semantics = "company.test.rooted_vfs/1";
		descriptor.owner_namespace = "company.test";
		descriptor.columns = {
			{"company.test.rooted_vfs.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "rooted_vfs_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.rooted_vfs.v1.value",
			 "value",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		descriptor.key_columns = {"company.test.rooted_vfs.v1.key"};
		descriptor.merge = sdk::merge_mode::set;
		descriptor.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  descriptor.contract_digest + "\n" + descriptor.canonical_form());
		sdk::relation_registry registry;
		require(registry.add(std::move(descriptor)).has_value(),
				"rooted-VFS descriptor registration failed");
		auto built = registry.build("engine-materialization-rooted-vfs-test");
		require(built.has_value(), "rooted-VFS relation engine build failed");
		return std::move(*built);
	}

	[[nodiscard]] std::string maximum_length_parent_path(const std::size_t leaf_bytes)
	{
		constexpr std::size_t maximum_path_bytes = 4095U;
		constexpr std::size_t maximum_component_bytes = 200U;
		require(leaf_bytes + 1U < maximum_path_bytes, "maximum-path fixture leaf is too long");
		const auto target = maximum_path_bytes - leaf_bytes - 1U;
		std::string output;
		while (output.size() < target)
		{
			if (!output.empty())
				output.push_back('/');
			const auto remaining = target - output.size();
			require(remaining != 0U, "maximum-path fixture cannot terminate with a separator");
			output.append(std::min(maximum_component_bytes, remaining), 'd');
		}
		return output;
	}

	void stable_parent_namespace_identity()
	{
		temporary_directory directory;
		const auto original = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(original >= 0, "original cwd capture for namespace identity failed");
		require(::chdir(directory.path().c_str()) == 0,
				"namespace identity root cwd switch failed");
		require(::mkdir("safe", 0700) == 0, "namespace identity parent creation failed");

		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "namespace identity effect-root capture failed");
		auto opener = materialization_rooted_store_opener::create(*root);
		require(opener.has_value(), "namespace identity rooted VFS registration failed");
		auto& observation = (*opener)->observation_capability();
		constexpr std::string_view locator{
			"/cxxlens-rooted-vfs-v1/safe/identity.sqlite"};

		auto absent = observation.capture_namespace(locator);
		require(absent.has_value() &&
				absent->entries.front().state == sdk::sqlite_backend_entry_state::absent,
				"namespace identity absent census failed");
		const auto stable_parent = absent->parent_namespace_identity;
		require(::chmod("safe", 0750) == 0,
				"namespace identity parent metadata mutation failed");
		auto metadata_changed = observation.capture_namespace(locator);
		require(metadata_changed.has_value() &&
				metadata_changed->parent_namespace_identity == stable_parent,
				"parent metadata epoch changed stable namespace identity");

		auto created = observation.exclusive_create_sync_zero_main(locator);
		require(created.has_value() && created->parent_namespace_identity == stable_parent,
				"zero-main receipt did not retain stable parent namespace identity");
		auto after_create = observation.capture_namespace(locator);
		require(after_create.has_value() &&
				after_create->parent_namespace_identity == stable_parent,
				"child creation changed stable parent namespace identity");
		const auto& main_after_create = after_create->entries.front();
		require(main_after_create.state == sdk::sqlite_backend_entry_state::held_regular &&
				main_after_create.directory_entry_identity == created->directory_entry_identity &&
				main_after_create.object_identity == created->object_identity,
				"zero-main receipt did not bind the created leaf epoch");

		create_regular_file(directory.path() + "/safe/identity.sqlite-wal");
		auto after_sibling = observation.capture_namespace(locator);
		require(after_sibling.has_value() &&
				after_sibling->parent_namespace_identity == stable_parent &&
				after_sibling->entries.front().directory_entry_identity ==
					main_after_create.directory_entry_identity,
				"sibling creation changed the existing main entry identity");
		auto same_entry = created->held_main->recheck_current_entry();
		require(same_entry.has_value() &&
				*same_entry == sdk::sqlite_backend_replacement_state::exact_same_entry_and_object,
				"sibling creation falsely replaced the held main entry");

		require(::chmod((directory.path() + "/safe/identity.sqlite").c_str(), 0640) == 0,
				"namespace identity main metadata mutation failed");
		auto leaf_changed = observation.observe_entry_state_without_open(
			locator, sdk::sqlite_backend_file_role::main_database);
		require(leaf_changed.has_value() && leaf_changed->directory_entry_identity &&
				*leaf_changed->directory_entry_identity != created->directory_entry_identity,
				"main leaf metadata epoch was omitted from directory-entry identity");

		require(::fchdir(original) == 0, "namespace identity cwd restore failed");
		(void)::close(original);
	}

	void exact_path_policy()
	{
		for (const auto value : {std::string_view{"db/store.sqlite"},
								 std::string_view{"store.sqlite"},
								 std::string_view{"data-1/x_2.sqlite"}})
			require(static_cast<bool>(validate_materialization_sqlite_path(value)),
					"canonical SQLite relative path was rejected");
		for (const auto value : {std::string_view{},
								 std::string_view{"/absolute"},
								 std::string_view{"C:/drive"},
								 std::string_view{":memory:"},
								 std::string_view{"file:safe.sqlite?vfs=unix"},
								 std::string_view{"file:/tmp/escape.sqlite?vfs=unix"},
								 std::string_view{"file:safe%2F..%2Fescape.sqlite?vfs=unix"},
								 std::string_view{"."},
								 std::string_view{".."},
								 std::string_view{"a/./b"},
								 std::string_view{"a/../b"},
								 std::string_view{"a//b"},
								 std::string_view{"a\\b"},
								 std::string_view{"cafe\xCC\x81.sqlite"}})
			require(!validate_materialization_sqlite_path(value),
					"noncanonical SQLite path was accepted");
	}

	void captured_root_and_symlink_escape()
	{
		temporary_directory directory;
		const auto original = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(original >= 0, "original cwd capture failed");
		require(::chdir(directory.path().c_str()) == 0, "temporary cwd switch failed");
		require(::mkdir("safe", 0700) == 0, "safe directory creation failed");
		const auto created = ::open("safe/store.sqlite", O_WRONLY | O_CREAT | O_EXCL, 0600);
		require(created >= 0, "rooted test database creation failed");
		const std::array payload{std::byte{'d'}, std::byte{'b'}};
		require(::write(created, payload.data(), payload.size()) ==
					static_cast<ssize_t>(payload.size()),
				"rooted test database write failed");
		(void)::close(created);
		require(::symlink("/tmp", "escape") == 0, "escape symlink creation failed");

		auto root = materialization_effect_root::capture_startup();
		require(root && root->observation_digest().starts_with("sha256:"),
				"startup effect root capture failed");
		require(::fchdir(original) == 0, "original cwd restore failed");
		(void)::close(original);

		auto opened = root->open_beneath("safe/store.sqlite", O_RDONLY);
		require(opened.has_value(), "captured root followed the later ambient cwd");
		auto identity = materialization_fd_identity(opened->get(), true);
		require(identity && identity->size_bytes == payload.size(),
				"rooted file identity differs from opened bytes");
		require(!root->open_beneath("escape/forbidden.sqlite", O_RDONLY | O_CREAT, 0600U),
				"openat2 rooted lookup followed a parent symlink escape");
		require(!root->open_beneath("safe/../escape", O_RDONLY),
				"rooted lookup accepted dotdot traversal");
	}

	void sqlite_db_wal_and_shm_are_rooted()
	{
		temporary_directory directory;
		temporary_directory decoy;
		const auto original = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(original >= 0, "original cwd capture for SQLite failed");
		require(::chdir(directory.path().c_str()) == 0, "SQLite root cwd switch failed");
		require(::mkdir("safe", 0700) == 0, "SQLite parent directory creation failed");

		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "SQLite effect-root capture failed");
		auto opener = materialization_rooted_store_opener::create(*root);
		require(opener.has_value(), "rooted SQLite VFS registration failed");
		{
			auto recovery = (*opener)->observation_capability().create_wal_recovery_workspace();
			require(recovery.has_value(),
					"rooted observation capability did not bind a WAL recovery workspace");
		}
		{
			auto& capability = (*opener)->observation_capability();
			auto ephemeral = capability.begin_ephemeral_connection_observation();
			require(ephemeral.has_value(),
					"rooted observation capability did not create an ephemeral scope");
			auto snapshot = (*ephemeral)->snapshot();
			require(snapshot.has_value() && snapshot->profile == "rooted-ephemeral-v1" &&
						snapshot->capability_token == capability.capability_token() &&
						snapshot->connection_token == (*ephemeral)->token() && snapshot->complete &&
						!snapshot->main_handle_open && snapshot->open_events.empty() &&
						!snapshot->shared_memory_object_identity &&
						!snapshot->shared_memory_entry_identity && snapshot->held_shm_locks.empty(),
					"rooted ephemeral scope was not an identity-bound zero-event receipt");
			auto* gate = (*ephemeral)->effect_gate_port();
			require(gate != nullptr && gate->enforcement_active() &&
						gate->stage() == sdk::sqlite_backend_effect_stage::denied &&
						!gate->latest_receipt(),
					"rooted ephemeral scope was not deny-by-default");
			auto denied = gate->activate_denied(
				capability.capability_token(), (*ephemeral)->token(), ":memory:");
			require(denied.has_value() && denied->sequence == 1U &&
						denied->capability_token == capability.capability_token() &&
						denied->connection_token == (*ephemeral)->token() &&
						denied->canonical_vfs_locator == ":memory:",
					"rooted ephemeral deny receipt was not token-bound");
		}
		require(::chdir(decoy.path().c_str()) == 0, "SQLite decoy cwd switch failed");
		require(::mkdir("safe", 0700) == 0, "SQLite decoy directory creation failed");
		const auto absolute_uri_escape = "file:" + decoy.path() + "/uri-escape.sqlite?vfs=unix";
		for (const auto& special_name : {std::string{":memory:"},
										 std::string{"file:safe/uri.sqlite?mode=memory&vfs=unix"},
										 absolute_uri_escape,
										 std::string{"file:safe%2F..%2Fescape.sqlite?vfs=unix"}})
			require(!(*opener)->open_sqlite(special_name, engine()),
					"rooted SQLite opener accepted a reserved SQLite filename");
		require(!std::filesystem::exists(decoy.path() + "/uri-escape.sqlite") &&
					!std::filesystem::exists(decoy.path() + "/safe/uri.sqlite"),
				"reserved SQLite filename escaped before rooted VFS dispatch");

		{
			auto store = (*opener)->open_sqlite("safe/store.sqlite", engine());
			require(store.has_value(), "rooted SQLite Store open failed");
			require(std::filesystem::is_regular_file(directory.path() + "/safe/store.sqlite"),
					"rooted SQLite database was not created beneath the captured root");
			require(std::filesystem::is_regular_file(directory.path() + "/safe/store.sqlite-wal"),
					"rooted SQLite WAL was not created beneath the captured root");
			require(std::filesystem::is_regular_file(directory.path() + "/safe/store.sqlite-shm"),
					"rooted SQLite SHM was not created beneath the captured root");
			require(!std::filesystem::exists(decoy.path() + "/safe/store.sqlite") &&
						!std::filesystem::exists(decoy.path() + "/safe/store.sqlite-wal") &&
						!std::filesystem::exists(decoy.path() + "/safe/store.sqlite-shm"),
					"SQLite DB/WAL/SHM escaped to the later ambient cwd");
		}

		direct_sqlite boundary{(*opener)->observation_capability()};
		boundary.reject_delegate_escape_callbacks();
		constexpr int main_database = 0x00000100;
		constexpr int main_journal = 0x00000800;
		constexpr int delete_on_close = 0x00000008;
		boundary.exercise_effect_gate_matrix(directory.path());
		for (const auto& raw : {std::string{"safe/raw.sqlite"},
								std::string{":memory:"},
								std::string{"file:safe.sqlite?vfs=unix"},
								std::string{"file:safe%2F..%2Fescape.sqlite?vfs=unix"}})
			boundary.reject_raw_named_open(raw, main_database);
		boundary.reject_named_open_role("safe/delete-main.sqlite", main_database | delete_on_close);
		boundary.reject_named_open_role("safe/store.sqlite-journal",
										main_journal | delete_on_close);
		require(!std::filesystem::exists(directory.path() + "/safe/delete-main.sqlite") &&
					!std::filesystem::exists(directory.path() + "/safe/store.sqlite-journal"),
				"named SQLITE_OPEN_DELETEONCLOSE created or removed a rooted file");
		const auto fifo_sidecar = directory.path() + "/safe/store.sqlite-journal";
		boundary.open_main_database_handle("safe/store.sqlite");
		require(::mkfifo(fifo_sidecar.c_str(), 0600) == 0,
				"rooted SQLite non-regular sidecar fixture creation failed");
		boundary.reject_nonregular_named_open("safe/store.sqlite-journal", main_journal);
		int fifo_exists{-1};
		require(boundary.access_named_path("safe/store.sqlite-journal", fifo_exists, 2) == 0 &&
					fifo_exists == 0 &&
					boundary.remove_named_path("safe/store.sqlite-journal") != 0 &&
					std::filesystem::is_fifo(fifo_sidecar),
				"rooted SQLite VFS accessed or deleted a non-regular named sidecar");
		boundary.close_main_database_handle();
		require(::unlink(fifo_sidecar.c_str()) == 0,
				"rooted SQLite non-regular sidecar fixture cleanup failed");
		boundary.require_main_database_replacement_detection(
			"safe/moved.sqlite", directory.path() + "/safe/moved.sqlite");
		boundary.open_main_database_handle("safe/sidecar-authority.sqlite");
		boundary.open_sidecar_handle("safe/sidecar-authority.sqlite-journal", main_journal);
		require(boundary.write_sidecar_handle() == 0,
				"active main database did not authorize sidecar FD I/O");
		boundary.close_main_database_handle();
		require(boundary.write_sidecar_handle(64) != 0 && boundary.sync_sidecar_handle() != 0 &&
					boundary.lock_sidecar_handle() != 0 &&
					std::filesystem::file_size(directory.path() +
											   "/safe/sidecar-authority.sqlite-journal") == 2U,
				"opened sidecar FD retained I/O authority after the last main close");
		boundary.close_sidecar_handle();
#if defined(CXXLENS_TEST_WRAP_ROOTED_VFS_EFFECTS)
		boundary.open_main_database_handle("safe/lease.sqlite");
		create_regular_file(directory.path() + "/safe/lease.sqlite-journal");
		arm_unlink_barrier();
		int leased_delete_result{-1};
		std::thread leased_delete{[&]
								  {
									  leased_delete_result =
										  boundary.remove_named_path("safe/lease.sqlite-journal");
								  }};
		wait_for_unlink_barrier();
		boundary.close_main_database_handle();
		int revoked_exists{-1};
		require(boundary.access_named_path("safe/lease.sqlite-journal", revoked_exists) == 0 &&
					revoked_exists == 0,
				"last main close did not revoke a new xAccess authority acquisition");
		require(boundary.remove_named_path("safe/lease.sqlite-journal") != 0 &&
					unlink_barrier_unexpected_calls == 0U,
				"last main close did not revoke a new xDelete authority acquisition");
		release_unlink_barrier();
		leased_delete.join();
		require(leased_delete_result == 0 &&
					!std::filesystem::exists(directory.path() + "/safe/lease.sqlite-journal"),
				"pre-close authority lease did not complete its serialized filesystem effect");
		revoked_exists = -1;
		require(boundary.access_named_path("safe/lease.sqlite-journal", revoked_exists) == 0 &&
					revoked_exists == 0 &&
					boundary.remove_named_path("safe/lease.sqlite-journal") != 0,
				"drained authority lease remained usable after last main close");
#endif
		for (const auto* relative : {"safe/untrusted.sqlite", "safe/store.sqlite-unknown"})
		{
			create_regular_file(directory.path() + "/" + relative);
			boundary.reject_vfs_access_and_delete(relative);
			require(std::filesystem::is_regular_file(directory.path() + "/" + relative),
					"unauthenticated rooted-VFS sentinel was changed");
		}
		constexpr std::array unsupported_named_roles{
			0x00000000, // no SQLite file-type role
			0x00000200, // SQLITE_OPEN_TEMP_DB
			0x00000400, // SQLITE_OPEN_TRANSIENT_DB
			0x00001000, // SQLITE_OPEN_TEMP_JOURNAL
			0x00002000, // SQLITE_OPEN_SUBJOURNAL
			0x00004000, // SQLITE_OPEN_SUPER_JOURNAL
		};
		for (std::size_t index{}; index < unsupported_named_roles.size(); ++index)
		{
			const auto relative = "safe/unsupported-role-" + std::to_string(index);
			boundary.reject_named_open_role(relative, unsupported_named_roles[index]);
			require(!std::filesystem::exists(directory.path() + "/" + relative),
					"unsupported named SQLite role created a rooted side effect");
		}
		auto rooted_entry_count = [&]
		{
			std::size_t count{};
			for (const auto& ignored :
				 std::filesystem::recursive_directory_iterator{directory.path()})
			{
				(void)ignored;
				++count;
			}
			return count;
		};
		const auto entries_before_anonymous = rooted_entry_count();
		for (const auto role : {0x00000200,	 // SQLITE_OPEN_TEMP_DB
								0x00000400,	 // SQLITE_OPEN_TRANSIENT_DB
								0x00001000,	 // SQLITE_OPEN_TEMP_JOURNAL
								0x00002000}) // SQLITE_OPEN_SUBJOURNAL
			boundary.exercise_anonymous_open_role(role);
		for (const auto role : {0x00000000,	 // no SQLite file-type role
								0x00000100,	 // SQLITE_OPEN_MAIN_DB
								0x00000800,	 // SQLITE_OPEN_MAIN_JOURNAL
								0x00080000,	 // SQLITE_OPEN_WAL
								0x00004000}) // SQLITE_OPEN_SUPER_JOURNAL
			boundary.reject_anonymous_open_role(role);
		require(rooted_entry_count() == entries_before_anonymous,
				"anonymous SQLite transient roles created a rooted pathname effect");
		boundary.reject_shm_map_on_non_main_handle(nullptr, 0x00000200); // SQLITE_OPEN_TEMP_DB
		require(!std::filesystem::exists(directory.path() + "/-shm"),
				"anonymous SQLite handle created a suffix-only SHM side effect");
		boundary.open_main_database_handle("safe/store.sqlite");
		boundary.reject_shm_map_on_non_main_handle("safe/store.sqlite-journal",
												   0x00000800); // SQLITE_OPEN_MAIN_JOURNAL
		require(!std::filesystem::exists(directory.path() + "/safe/store.sqlite-journal-shm"),
				"named non-main SQLite handle created a nested SHM side effect");
		create_regular_file(directory.path() + "/-shm");
		boundary.reject_shm_unmap_on_non_main_handle(nullptr, 0x00000200);
		require(std::filesystem::is_regular_file(directory.path() + "/-shm"),
				"anonymous SQLite handle deleted a suffix-only SHM sentinel");
		create_regular_file(directory.path() + "/safe/store.sqlite-journal-shm");
		boundary.reject_shm_unmap_on_non_main_handle("safe/store.sqlite-journal", 0x00000800);
		require(
			std::filesystem::is_regular_file(directory.path() + "/safe/store.sqlite-journal-shm"),
			"named non-main SQLite handle deleted a nested SHM sentinel");
		boundary.close_main_database_handle();

		direct_sqlite journal{(*opener)->observation_capability()};
		journal.open("safe/journal.sqlite");
		journal.execute("PRAGMA journal_mode=DELETE;CREATE TABLE item(value);BEGIN IMMEDIATE;"
						"INSERT INTO item VALUES(1);");
		require(std::filesystem::is_regular_file(directory.path() + "/safe/journal.sqlite-journal"),
				"rollback journal was not created beneath the captured root");
		require(!std::filesystem::exists(decoy.path() + "/safe/journal.sqlite") &&
					!std::filesystem::exists(decoy.path() + "/safe/journal.sqlite-journal"),
				"SQLite DB/journal escaped to the later ambient cwd");
		journal.execute("ROLLBACK;");
		journal.close();
		create_regular_file(directory.path() + "/safe/journal.sqlite-wal");
		boundary.reject_vfs_access_and_delete("safe/journal.sqlite");
		boundary.reject_vfs_access_and_delete("safe/journal.sqlite-wal");
		require(std::filesystem::is_regular_file(directory.path() + "/safe/journal.sqlite") &&
					std::filesystem::is_regular_file(directory.path() + "/safe/journal.sqlite-wal"),
				"closed main-DB authentication remained usable");
		require((*opener)->receipt() &&
					(*opener)->receipt()->exact_relative_path == "safe/store.sqlite" &&
					(*opener)->receipt()->mount_device_inode_observation_digest ==
						root->observation_digest(),
				"rooted SQLite receipt did not bind the root and exact path");
		require(!(*opener)->open_sqlite("safe/other.sqlite", engine()),
				"rooted SQLite opener accepted a changed reopen path");
		require(::fchdir(original) == 0, "ambient cwd restore after SQLite test failed");
		(void)::close(original);
	}

	void returned_store_owns_rooted_vfs_lifetime()
	{
		temporary_directory directory;
		const auto original = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(original >= 0, "original cwd capture for VFS lifetime failed");
		require(::chdir(directory.path().c_str()) == 0, "VFS lifetime root cwd switch failed");
		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "VFS lifetime effect-root capture failed");

		std::optional<sdk::snapshot_store> retained_store;
		{
			auto opener = materialization_rooted_store_opener::create(*root);
			require(opener.has_value(), "VFS lifetime opener creation failed");
			auto opened = (*opener)->open_sqlite("lifetime.sqlite", engine());
			require(opened.has_value(), "VFS lifetime Store open failed");
			retained_store.emplace(std::move(*opened));
		}
		bool compacted{};
		std::thread destroyer{
			[store = std::optional<sdk::snapshot_store>{std::move(*retained_store)},
			 &compacted]() mutable
			{
				compacted = store->compact().has_value();
				store.reset();
			}};
		retained_store.reset();
		destroyer.join();
		require(compacted,
				"Store could not use its rooted VFS after opener destruction on another thread");

		auto next_opener = materialization_rooted_store_opener::create(*root);
		require(next_opener.has_value(),
				"rooted VFS lifetime token did not release registration after Store close");
		require(::fchdir(original) == 0, "ambient cwd restore after VFS lifetime test failed");
		(void)::close(original);
	}

	void sealed_private_snapshot_vfs_is_read_only_and_unregisters()
	{
#if defined(__linux__)
		using open_function = int (*)(const char*, void**, int, const char*);
		using execute_function =
			int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
		using close_function = int (*)(void*);
		using free_function = void (*)(void*);
		using find_function = sqlite3_vfs* (*)(const char*);
		using register_function = int (*)(sqlite3_vfs*, int);
		using unregister_function = int (*)(sqlite3_vfs*);

		void* raw_library = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
		if (raw_library == nullptr)
			raw_library = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
		require(raw_library != nullptr, "private snapshot SQLite runtime load failed");
		std::shared_ptr<void> runtime_lifetime{raw_library,
											   [](void* value)
											   {
												   (void)::dlclose(value);
											   }};
		auto symbol = [&](const char* name)
		{
			auto* output = ::dlsym(raw_library, name);
			require(output != nullptr, "private snapshot SQLite symbol resolution failed");
			return output;
		};
		const auto open = reinterpret_cast<open_function>(symbol("sqlite3_open_v2"));
		const auto execute = reinterpret_cast<execute_function>(symbol("sqlite3_exec"));
		const auto close = reinterpret_cast<close_function>(symbol("sqlite3_close_v2"));
		const auto free_memory = reinterpret_cast<free_function>(symbol("sqlite3_free"));
		const auto find = reinterpret_cast<find_function>(symbol("sqlite3_vfs_find"));
		const auto register_vfs =
			reinterpret_cast<register_function>(symbol("sqlite3_vfs_register"));
		const auto unregister_vfs =
			reinterpret_cast<unregister_function>(symbol("sqlite3_vfs_unregister"));
		auto* pinned_default = find(nullptr);
		require(pinned_default != nullptr, "private snapshot default VFS pin failed");

		temporary_directory directory;
		const auto source_path = directory.path() + "/source.sqlite";
		constexpr int read_only = 0x00000001;
		constexpr int read_write = 0x00000002;
		constexpr int create = 0x00000004;
		constexpr int uri = 0x00000040;
		constexpr int full_mutex = 0x00010000;
		constexpr int private_cache = 0x00040000;
		void* source{};
		require(open(source_path.c_str(),
					 &source,
					 read_write | create | full_mutex | private_cache,
					 nullptr) == 0 &&
					source != nullptr,
				"private snapshot source database open failed");
		char* message{};
		require(execute(source,
						"CREATE TABLE item(value INTEGER NOT NULL);INSERT INTO item VALUES(7);",
						nullptr,
						nullptr,
						&message) == 0,
				"private snapshot source database creation failed");
		if (message != nullptr)
			free_memory(message);
		require(close(source) == 0, "private snapshot source database close failed");

		const auto descriptor = ::open(source_path.c_str(), O_RDONLY | O_CLOEXEC);
		require(descriptor >= 0, "private snapshot source bytes open failed");
		struct stat source_stat{};
		require(::fstat(descriptor, &source_stat) == 0 && source_stat.st_size > 0,
				"private snapshot source bytes stat failed");
		std::vector<std::byte> source_bytes(static_cast<std::size_t>(source_stat.st_size));
		std::size_t consumed{};
		while (consumed < source_bytes.size())
		{
			const auto count = ::pread(descriptor,
									   source_bytes.data() + consumed,
									   source_bytes.size() - consumed,
									   static_cast<off_t>(consumed));
			require(count > 0, "private snapshot source bytes read failed");
			consumed += static_cast<std::size_t>(count);
		}
		require(::close(descriptor) == 0, "private snapshot source bytes close failed");

		sdk::sqlite_backend_opaque_identity source_token{"rooted-vfs-v1.observation-capability.v1",
														 {std::byte{0x5aU}}};
		sdk::sqlite_private_snapshot_registry_binding registry;
		registry.runtime_identity = raw_library;
		registry.pinned_default_vfs = pinned_default;
		registry.find =
			reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::find_function>(find);
		registry.register_vfs =
			reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::register_function>(
				register_vfs);
		registry.unregister_vfs =
			reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::unregister_function>(
				unregister_vfs);
		registry.runtime_lifetime = runtime_lifetime;
		auto builder = sdk::make_sqlite_private_snapshot_builder(source_token, registry);
		require(builder.has_value(), "private snapshot builder creation failed");
		for (std::size_t offset{}; offset < source_bytes.size();)
		{
			const auto count = std::min<std::size_t>(113U, source_bytes.size() - offset);
			require((*builder)->append(std::span{source_bytes}.subspan(offset, count)).has_value(),
					"private snapshot bounded append failed");
			offset += count;
		}
		const auto digest = sdk::content_digest(source_bytes);
		auto snapshot = (*builder)->seal(source_bytes.size(), digest);
		require(snapshot.has_value(), "private snapshot seal failed");
		require((*snapshot)->receipt() ==
					sdk::sqlite_backend_copy_receipt{source_bytes.size(), digest},
				"private snapshot receipt mismatch");
		require((*snapshot)->application_generated_uri().starts_with(
					"file:/cxxlens-sqlite-private-v1/") &&
					!(*snapshot)->application_generated_uri().contains(source_path),
				"private snapshot URI exposed or reused the source locator");
		const std::string private_vfs_name{(*snapshot)->registered_vfs_name()};
		require(find(private_vfs_name.c_str()) == (*snapshot)->vfs_implementation_identity(),
				"private snapshot VFS registration identity mismatch");
		require(!(*builder)->append(std::span{source_bytes}.first(1U)),
				"private snapshot admitted append after irreversible seal");

		const std::string private_uri{(*snapshot)->application_generated_uri()};
		void* private_database{};
		require(open(private_uri.c_str(),
					 &private_database,
					 read_only | uri | full_mutex | private_cache,
					 private_vfs_name.c_str()) == 0 &&
					private_database != nullptr,
				"private snapshot read-only SQLite open failed");
		message = nullptr;
		require(execute(private_database, "SELECT value FROM item;", nullptr, nullptr, &message) ==
					0,
				"private snapshot read failed");
		if (message != nullptr)
			free_memory(message);
		message = nullptr;
		require(
			execute(
				private_database, "CREATE TABLE forbidden(value);", nullptr, nullptr, &message) !=
				0,
			"private snapshot VFS admitted a write");
		if (message != nullptr)
			free_memory(message);
		require(close(private_database) == 0,
				"private snapshot connection did not close before VFS lifetime release");

		builder->reset();
		snapshot->reset();
		require(find(private_vfs_name.c_str()) == nullptr,
				"private snapshot VFS remained registered after owned lifetime release");
#endif
	}

	void maximum_length_sqlite_sidecars_are_rooted()
	{
		constexpr std::string_view store_leaf{"store.sqlite"};
		constexpr std::string_view journal_leaf{"other.sqlite"};
		constexpr std::size_t maximum_path_bytes = 4095U;
		const auto parent = maximum_length_parent_path(store_leaf.size());
		const auto store_path = parent + "/" + std::string{store_leaf};
		const auto journal_path = parent + "/" + std::string{journal_leaf};
		require(store_path.size() == maximum_path_bytes &&
					journal_path.size() == maximum_path_bytes,
				"maximum-path fixture does not meet the public boundary");
		require(static_cast<bool>(validate_materialization_sqlite_path(store_path)),
				"4095-byte public SQLite path was rejected");
		require(!validate_materialization_sqlite_path(store_path + "x") &&
					!validate_materialization_sqlite_path(store_path + "-wal") &&
					!validate_materialization_sqlite_path(store_path + "-journal") &&
					!validate_materialization_sqlite_path(store_path + "-shm"),
				"public SQLite path policy admitted bytes beyond 4095");

		temporary_directory directory;
		const auto original = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		require(original >= 0, "original cwd capture for maximum SQLite path failed");
		require(::chdir(directory.path().c_str()) == 0,
				"maximum SQLite path root cwd switch failed");
		long_directory_chain chain{parent};
		auto root = materialization_effect_root::capture_startup();
		require(root.has_value(), "maximum SQLite path effect-root capture failed");
		auto opener = materialization_rooted_store_opener::create(*root);
		require(opener.has_value(), "maximum SQLite path rooted VFS registration failed");

		{
			auto store = (*opener)->open_sqlite(store_path, engine());
			require(store.has_value(), "4095-byte rooted SQLite Store open failed");
			require(chain.is_regular_file("store.sqlite") &&
						chain.is_regular_file("store.sqlite-wal") &&
						chain.is_regular_file("store.sqlite-shm"),
					"limit-adjacent SQLite DB/WAL/SHM files were not created");

			direct_sqlite rejected{(*opener)->observation_capability()};
			rejected.reject_open(journal_path + "-wal");
			rejected.reject_open(store_path + "-unknown");
			rejected.reject_open(parent + "/../store.sqlite-wal");
			require(!chain.is_regular_file("other.sqlite-wal") &&
						!chain.is_regular_file("store.sqlite-unknown"),
					"rejected over-limit path created a side effect");

			direct_sqlite journal{(*opener)->observation_capability()};
			journal.open(journal_path);
			journal.execute("PRAGMA journal_mode=DELETE;CREATE TABLE item(value);BEGIN IMMEDIATE;"
							"INSERT INTO item VALUES(1);");
			require(chain.is_regular_file("other.sqlite-journal"),
					"limit-adjacent SQLite rollback journal was not created");
			journal.execute("ROLLBACK;");
		}

		for (const auto leaf : {std::string_view{"store.sqlite"},
								std::string_view{"store.sqlite-wal"},
								std::string_view{"store.sqlite-shm"},
								std::string_view{"other.sqlite"},
								std::string_view{"other.sqlite-journal"}})
			chain.remove_file(leaf);
		require(::fchdir(original) == 0, "ambient cwd restore after maximum SQLite path failed");
		(void)::close(original);
	}
} // namespace

int main()
{
	exact_path_policy();
	captured_root_and_symlink_escape();
	stable_parent_namespace_identity();
	sealed_private_snapshot_vfs_is_read_only_and_unregisters();
	returned_store_owns_rooted_vfs_lifetime();
	maximum_length_sqlite_sidecars_are_rooted();
	sqlite_db_wal_and_shm_are_rooted();
	return 0;
}
