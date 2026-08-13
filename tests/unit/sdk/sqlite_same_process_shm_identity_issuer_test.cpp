#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <barrier>
#include <sys/wait.h>
#include <unistd.h>

#include "sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_registry_test_peer
	{
	  public:
		[[nodiscard]] static sqlite_shm_registry_process_owner
		process_owner(sqlite_backend_opaque_identity process)
		{
			return sqlite_shm_registry_process_owner{std::move(process)};
		}

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
		adopt_runtime(sqlite_same_process_shm_mapping_registry& registry,
					  sqlite_backend_opaque_identity identity,
					  sqlite_backend_opaque_identity pin,
					  std::shared_ptr<void> owner)
		{
			return registry.adopt_runtime_lifetime_for_testing(
				std::move(identity), std::move(pin), std::move(owner));
		}

		[[nodiscard]] static sqlite_shm_registry_alias_binding
		alias_binding(sqlite_backend_opaque_identity process,
					  sqlite_backend_opaque_identity cohort,
					  sqlite_backend_opaque_identity alias,
					  sqlite_shm_registry_runtime_lifetime_pin runtime)
		{
			return {std::move(process), std::move(cohort), std::move(alias), std::move(runtime)};
		}

		[[nodiscard]] static sqlite_shm_verified_alias_registration_receipt
		registration_receipt(sqlite_backend_opaque_identity process,
							 sqlite_backend_opaque_identity cohort,
							 sqlite_backend_opaque_identity alias,
							 sqlite_backend_opaque_identity runtime,
							 sqlite_backend_opaque_identity runtime_pin,
							 sqlite_backend_opaque_identity epoch)
		{
			return {std::move(process),
					std::move(cohort),
					std::move(alias),
					std::move(runtime),
					std::move(runtime_pin),
					std::move(epoch)};
		}

		[[nodiscard]] static sqlite_shm_process_global_identity_issuer
		issuer(const sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			return registry.identity_issuer_for_testing();
		}

		[[nodiscard]] static sqlite_shm_reader_lifecycle_identity_scope
		scope(const sqlite_same_process_shm_mapping_registry& registry,
			  const sqlite_shm_registry_family_pin& family,
			  const sqlite_backend_opaque_identity& cohort,
			  const sqlite_backend_opaque_identity& request,
			  const std::uint64_t open_token,
			  const sqlite_shm_reader_lifecycle_owner_kind kind,
			  const std::uint64_t owner_token,
			  const std::uint64_t generation)
		{
			return registry.seal_reader_lifecycle_identity_scope_for_testing(
				family, cohort, request, open_token, kind, owner_token, generation);
		}

		static void exhaust(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.exhaust_identity_issuer_for_testing();
		}

		static void invalidate(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.invalidate_process_instance_for_testing();
		}

		static void lock_registry(sqlite_same_process_shm_mapping_registry& registry)
		{
			registry.lock_registry_mutex_for_fork_testing();
		}

		static void unlock_registry(sqlite_same_process_shm_mapping_registry& registry) noexcept
		{
			registry.unlock_registry_mutex_for_fork_testing();
		}

		static void set_scope_count(sqlite_shm_process_global_identity_issuer& issuer,
									const sqlite_shm_reader_lifecycle_identity_scope& scope,
									const std::size_t value) noexcept
		{
			issuer.set_scope_live_records_for_testing(scope, value);
		}

		[[nodiscard]] static std::size_t
		scope_count(const sqlite_shm_process_global_identity_issuer& issuer,
					const sqlite_shm_reader_lifecycle_identity_scope& scope) noexcept
		{
			return issuer.scope_live_records_for_testing(scope);
		}

		static void set_child_count(sqlite_shm_process_global_identity_issuer& issuer,
									const sqlite_shm_issued_reader_callback_identity& callback,
									const std::size_t value) noexcept
		{
			issuer.set_callback_live_children_for_testing(callback, value);
		}

		[[nodiscard]] static std::size_t
		child_count(const sqlite_shm_process_global_identity_issuer& issuer,
					const sqlite_shm_issued_reader_callback_identity& callback) noexcept
		{
			return issuer.callback_live_children_for_testing(callback);
		}

		static void
		arm_pause(sqlite_shm_process_global_identity_issuer& issuer,
				  const sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept
		{
			issuer.arm_pause_for_testing(point);
		}

		[[nodiscard]] static bool
		pause_entered(const sqlite_shm_process_global_identity_issuer& issuer,
					  const sqlite_shm_identity_issuer_pause_point_for_testing point) noexcept
		{
			return issuer.pause_entered_for_testing(point);
		}

		static void release_pause(sqlite_shm_process_global_identity_issuer& issuer) noexcept
		{
			issuer.release_pause_for_testing();
		}

		[[nodiscard]] static bool
		inject_duplicate_family(sqlite_same_process_shm_mapping_registry& registry,
								const sqlite_shm_lease_family_binding& family) noexcept
		{
			return registry.inject_duplicate_family_for_testing(family);
		}
	};

	namespace
	{
		using callback_role = sqlite_shm_reader_callback_identity_role;
		using effect_role = sqlite_shm_reader_effect_identity_role;
		using terminal_role = sqlite_shm_reader_session_terminal_identity_role;
		using owner_kind = sqlite_shm_reader_lifecycle_owner_kind;
		using rejection_reason = sqlite_shm_lease_rejection_reason;

		void require(const bool condition, const std::string_view message)
		{
			if (!condition)
				throw std::runtime_error{std::string{message}};
		}

		template <class Value>
		void require_rejection(const sqlite_shm_lease_result<Value>& result,
							   const rejection_reason reason,
							   const std::string_view message)
		{
			require(!result.has_value() && result.error().reason == reason, message);
		}

		[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
															  const std::uint8_t marker)
		{
			return {std::string{profile}, {static_cast<std::byte>(marker)}};
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		framed_identity(std::string profile, std::initializer_list<std::uint8_t> values)
		{
			std::vector<std::byte> bytes;
			bytes.reserve(values.size());
			for (const auto value : values)
				bytes.push_back(static_cast<std::byte>(value));
			return {std::move(profile), std::move(bytes)};
		}

		struct fixture
		{
			sqlite_backend_opaque_identity process;
			sqlite_backend_opaque_identity cohort;
			sqlite_backend_opaque_identity alias_identity;
			sqlite_backend_opaque_identity runtime_identity;
			sqlite_backend_opaque_identity runtime_pin_identity;
			sqlite_backend_opaque_identity registration_epoch;
			sqlite_shm_lease_family_binding family;
			std::unique_ptr<sqlite_same_process_shm_mapping_registry> registry;
			std::shared_ptr<void> runtime_owner;
			std::optional<sqlite_shm_registry_alias_pin> alias;
			std::optional<sqlite_shm_registry_family_pin> family_pin;
		};

		[[nodiscard]] fixture
		make_fixture(const std::uint8_t marker,
					 std::optional<sqlite_backend_opaque_identity> process_override = std::nullopt)
		{
			fixture value;
			value.process = process_override ? std::move(*process_override)
											 : identity("test.identity-issuer.process", marker);
			value.cohort = identity("test.identity-issuer.cohort", marker);
			value.alias_identity = identity("test.identity-issuer.alias", marker);
			value.runtime_identity = identity("test.identity-issuer.runtime", marker);
			value.runtime_pin_identity = identity("test.identity-issuer.runtime-pin", marker);
			value.registration_epoch = identity("test.identity-issuer.registration", marker);
			value.family = {
				value.process, value.cohort, identity("test.identity-issuer.file-family", marker)};

			auto owner = sqlite_same_process_shm_registry_test_peer::process_owner(value.process);
			auto created = sqlite_same_process_shm_mapping_registry::create(std::move(owner));
			require(created.has_value(), "create identity issuer registry");
			value.registry = std::move(created.value());
			value.runtime_owner = std::make_shared<int>(marker);
			auto runtime = sqlite_same_process_shm_registry_test_peer::adopt_runtime(
				*value.registry,
				value.runtime_identity,
				value.runtime_pin_identity,
				value.runtime_owner);
			require(runtime.has_value(), "adopt identity issuer runtime");
			auto binding = sqlite_same_process_shm_registry_test_peer::alias_binding(
				value.process, value.cohort, value.alias_identity, std::move(runtime.value()));
			auto alias = value.registry->reserve_alias(std::move(binding));
			require(alias.has_value(), "reserve identity issuer alias");
			value.alias.emplace(std::move(alias.value()));
			require(value.registry->begin_alias_register(*value.alias).has_value(),
					"begin identity issuer alias registration");
			const auto receipt = sqlite_same_process_shm_registry_test_peer::registration_receipt(
				value.process,
				value.cohort,
				value.alias_identity,
				value.runtime_identity,
				value.runtime_pin_identity,
				value.registration_epoch);
			require(value.registry->confirm_alias_registered(*value.alias, receipt).has_value(),
					"confirm identity issuer alias registration");
			auto family = value.registry->install_or_join_family(*value.alias, value.family);
			require(family.has_value(), "install identity issuer family");
			value.family_pin.emplace(std::move(family.value()));
			return value;
		}

		[[nodiscard]] sqlite_shm_reader_lifecycle_identity_scope
		make_scope(const fixture& value,
				   const owner_kind kind,
				   const std::uint64_t token,
				   const sqlite_backend_opaque_identity* cohort = nullptr,
				   const sqlite_backend_opaque_identity* request = nullptr)
		{
			const auto default_cohort = identity("test.identity-issuer.callback-cohort", 1U);
			const auto default_request = identity("test.identity-issuer.request", 1U);
			return sqlite_same_process_shm_registry_test_peer::scope(
				*value.registry,
				*value.family_pin,
				cohort ? *cohort : default_cohort,
				request ? *request : default_request,
				1000U + token,
				kind,
				2000U + token,
				3000U + token);
		}

		[[nodiscard]] sqlite_shm_issued_reader_callback_identity
		issue_callback(sqlite_shm_process_global_identity_issuer& issuer,
					   const sqlite_shm_reader_lifecycle_identity_scope& scope,
					   const callback_role role,
					   const std::uint8_t marker,
					   const std::uint64_t depth = 0U)
		{
			auto permit = issuer.reserve_callback(
				scope, role, identity("test.identity-issuer.thread", marker), depth);
			require(permit.has_value() && permit->valid(), "reserve callback identity");
			auto callback = issuer.seal_callback(permit.value(), scope, role);
			require(callback.has_value() && callback->valid() && !permit->valid(),
					"seal callback identity");
			return std::move(callback.value());
		}

		void retire_callback_and_scope(sqlite_shm_process_global_identity_issuer& issuer,
									   sqlite_shm_reader_lifecycle_identity_scope& scope,
									   sqlite_shm_issued_reader_callback_identity& callback,
									   const callback_role role)
		{
			require(issuer.retire_callback(scope, callback, role).has_value(),
					"retire callback identity");
			require(issuer.retire_scope(scope).has_value(), "retire callback scope");
		}

		[[nodiscard]] std::uint64_t
		projection_sequence(const sqlite_backend_opaque_identity& projection)
		{
			require(projection.bytes.size() >= 10U, "identity projection has sequence trailer");
			const auto offset = projection.bytes.size() - 10U;
			std::uint64_t value{};
			for (std::size_t index = 0; index < sizeof(value); ++index)
				value |= static_cast<std::uint64_t>(
							 std::to_integer<std::uint8_t>(projection.bytes[offset + index]))
					<< static_cast<unsigned>(index * 8U);
			return value;
		}

		struct decoded_projection_hidden
		{
			std::uint64_t incarnation{};
			std::uint64_t process_epoch{};
			std::uint64_t family_epoch{};
			std::uint64_t family_pin_token{};
			std::uint64_t sequence{};
		};

		[[nodiscard]] std::uint64_t take_u64(const std::vector<std::byte>& bytes,
											 std::size_t& offset,
											 const std::string_view message)
		{
			require(offset <= bytes.size() && bytes.size() - offset >= sizeof(std::uint64_t),
					message);
			std::uint64_t value{};
			for (std::size_t index = 0; index < sizeof(value); ++index)
				value |=
					static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
					<< static_cast<unsigned>(index * 8U);
			offset += sizeof(value);
			return value;
		}

		void take_identity(const std::vector<std::byte>& bytes,
						   std::size_t& offset,
						   const sqlite_backend_opaque_identity& expected,
						   const std::string_view message)
		{
			const auto profile_size = take_u64(bytes, offset, message);
			require(profile_size == expected.profile.size() && offset <= bytes.size() &&
						bytes.size() - offset >= profile_size,
					message);
			for (std::size_t index = 0; index < profile_size; ++index)
				require(std::to_integer<unsigned char>(bytes[offset + index]) ==
							static_cast<unsigned char>(expected.profile[index]),
						message);
			offset += static_cast<std::size_t>(profile_size);
			const auto byte_size = take_u64(bytes, offset, message);
			require(byte_size == expected.bytes.size() && offset <= bytes.size() &&
						bytes.size() - offset >= byte_size,
					message);
			for (std::size_t index = 0; index < byte_size; ++index)
				require(bytes[offset + index] == expected.bytes[index], message);
			offset += static_cast<std::size_t>(byte_size);
		}

		[[nodiscard]] decoded_projection_hidden
		verify_projection_encoding(const sqlite_backend_opaque_identity& projection,
								   const sqlite_shm_lease_family_binding& family,
								   const sqlite_backend_opaque_identity& callback_cohort,
								   const sqlite_backend_opaque_identity& request_seal,
								   const std::uint64_t registry_open_token,
								   const owner_kind kind,
								   const std::uint64_t lifecycle_owner_token,
								   const std::uint64_t writer_mapping_generation,
								   const std::uint64_t expected_sequence,
								   const sqlite_shm_reader_lifecycle_identity_domain domain,
								   const std::uint8_t role)
		{
			require(projection.profile ==
						"cxxlens.sqlite.reader-lifecycle.process-issued-identity.v1",
					"projection uses exact closed identity profile");
			std::size_t offset{};
			take_identity(projection.bytes,
						  offset,
						  family.process_instance,
						  "decode exact framed process identity");
			take_identity(projection.bytes,
						  offset,
						  family.shared_runtime_vfs_cohort,
						  "decode exact framed runtime/VFS cohort");
			take_identity(projection.bytes,
						  offset,
						  family.exact_file_family,
						  "decode exact framed file family");
			take_identity(
				projection.bytes, offset, callback_cohort, "decode exact framed callback cohort");
			take_identity(
				projection.bytes, offset, request_seal, "decode exact framed request seal");
			decoded_projection_hidden hidden;
			hidden.incarnation = take_u64(projection.bytes, offset, "decode issuer incarnation");
			hidden.process_epoch = take_u64(projection.bytes, offset, "decode process epoch");
			hidden.family_epoch = take_u64(projection.bytes, offset, "decode family epoch");
			hidden.family_pin_token = take_u64(projection.bytes, offset, "decode family pin token");
			const auto decoded_open = take_u64(projection.bytes, offset, "decode open token");
			require(offset < projection.bytes.size(), "decode owner kind");
			const auto decoded_kind = std::to_integer<std::uint8_t>(projection.bytes[offset++]);
			const auto decoded_owner = take_u64(projection.bytes, offset, "decode owner token");
			const auto decoded_generation =
				take_u64(projection.bytes, offset, "decode writer generation");
			hidden.sequence = take_u64(projection.bytes, offset, "decode sequence");
			require(offset <= projection.bytes.size() && projection.bytes.size() - offset == 2U,
					"projection has exact domain/role trailer");
			const auto decoded_domain = std::to_integer<std::uint8_t>(projection.bytes[offset++]);
			const auto decoded_role = std::to_integer<std::uint8_t>(projection.bytes[offset++]);
			require(hidden.incarnation != 0U && hidden.process_epoch != 0U &&
						hidden.family_epoch != 0U && hidden.family_pin_token != 0U &&
						decoded_open == registry_open_token &&
						decoded_kind == static_cast<std::uint8_t>(kind) &&
						decoded_owner == lifecycle_owner_token &&
						decoded_generation == writer_mapping_generation &&
						hidden.sequence == expected_sequence &&
						decoded_domain == static_cast<std::uint8_t>(domain) &&
						decoded_role == role && offset == projection.bytes.size(),
					"projection encoding binds exact hidden/public coordinates and trailer");
			return hidden;
		}

		void verify_type_traits()
		{
			static_assert(std::is_same_v<std::underlying_type_t<callback_role>, std::uint8_t>);
			static_assert(std::is_same_v<std::underlying_type_t<effect_role>, std::uint8_t>);
			static_assert(std::is_same_v<std::underlying_type_t<terminal_role>, std::uint8_t>);
			static_assert(std::is_same_v<std::underlying_type_t<owner_kind>, std::uint8_t>);
#define CXXLENS_REQUIRE_MOVE_ONLY(Type) \
	static_assert(!std::is_default_constructible_v<Type>); \
	static_assert(!std::is_copy_constructible_v<Type>); \
	static_assert(!std::is_copy_assignable_v<Type>); \
	static_assert(std::is_nothrow_move_constructible_v<Type>); \
	static_assert(!std::is_move_assignable_v<Type>)
			CXXLENS_REQUIRE_MOVE_ONLY(sqlite_shm_reader_lifecycle_identity_scope);
			CXXLENS_REQUIRE_MOVE_ONLY(sqlite_shm_reader_callback_identity_permit);
			CXXLENS_REQUIRE_MOVE_ONLY(sqlite_shm_issued_reader_callback_identity);
			CXXLENS_REQUIRE_MOVE_ONLY(sqlite_shm_issued_reader_effect_identity);
			CXXLENS_REQUIRE_MOVE_ONLY(sqlite_shm_issued_reader_session_terminal_identity);
#undef CXXLENS_REQUIRE_MOVE_ONLY
			static_assert(std::is_copy_constructible_v<sqlite_shm_process_global_identity_issuer>);
		}

		void verify_owner_and_callback_role_table()
		{
			auto value = make_fixture(10U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			require(issuer.valid(), "identity issuer facade is current");
			constexpr std::array owners{
				owner_kind::map,
				owner_kind::unpublished_cleanup,
				owner_kind::attachment,
				owner_kind::close,
				owner_kind::logical_ack,
				owner_kind::late_outer_unwind,
			};
			constexpr std::array callbacks{
				callback_role::map,
				callback_role::unpublished_cleanup_unmap,
				callback_role::attachment_unmap,
				callback_role::close,
				callback_role::logical_ack_unmap,
				callback_role::late_outer_unmap,
			};
			std::uint64_t token{1U};
			for (std::size_t owner_index = 0; owner_index < owners.size(); ++owner_index)
			{
				for (std::size_t role_index = 0; role_index < callbacks.size(); ++role_index)
				{
					auto scope = make_scope(value, owners[owner_index], token++);
					require(scope.valid(), "seal owner-role table scope");
					auto permit =
						issuer.reserve_callback(scope,
												callbacks[role_index],
												identity("test.identity-issuer.role-thread",
														 static_cast<std::uint8_t>(token)),
												role_index);
					if (owner_index != role_index)
					{
						require_rejection(permit,
										  rejection_reason::invalid_identity,
										  "reject incompatible owner/callback role");
						auto exact = issue_callback(issuer,
													scope,
													callbacks[owner_index],
													static_cast<std::uint8_t>(token));
						retire_callback_and_scope(issuer, scope, exact, callbacks[owner_index]);
					}
					else
					{
						require(permit.has_value(), "accept exact owner/callback role");
						auto callback =
							issuer.seal_callback(permit.value(), scope, callbacks[role_index]);
						require(callback.has_value(), "seal exact owner/callback role");
						retire_callback_and_scope(
							issuer, scope, callback.value(), callbacks[role_index]);
					}
				}
			}

			auto invalid_scope = sqlite_same_process_shm_registry_test_peer::scope(
				*value.registry,
				*value.family_pin,
				identity("test.identity-issuer.callback-cohort", 91U),
				identity("test.identity-issuer.request", 91U),
				1U,
				static_cast<owner_kind>(0xffU),
				2U,
				3U);
			require(!invalid_scope.valid(), "reject invalid owner kind before issuance");
			auto scope = make_scope(value, owner_kind::map, token++);
			auto invalid =
				issuer.reserve_callback(scope,
										static_cast<callback_role>(0xffU),
										identity("test.identity-issuer.invalid-role-thread", 1U),
										0U);
			require_rejection(invalid,
							  rejection_reason::invalid_identity,
							  "reject invalid callback enum without claiming exact role");
			auto callback = issue_callback(issuer, scope, callback_role::map, 92U);
			retire_callback_and_scope(issuer, scope, callback, callback_role::map);
		}

		void verify_projection_framing_and_hidden_registry_incarnation()
		{
			auto first = make_fixture(25U);
			auto second = make_fixture(25U, first.process);
			require(first.process == second.process && first.cohort == second.cohort &&
						first.family == second.family,
					"fresh registries start from identical copied public family coordinates");
			auto first_issuer = sqlite_same_process_shm_registry_test_peer::issuer(*first.registry);
			auto second_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*second.registry);
			const auto framed_cohort = framed_identity("a", {0x62U, 0x63U});
			const auto framed_request = framed_identity("ab", {0x63U});
			constexpr std::uint64_t open_token = 7001U;
			constexpr std::uint64_t owner_token = 7002U;
			constexpr std::uint64_t generation = 7003U;
			auto first_scope = sqlite_same_process_shm_registry_test_peer::scope(*first.registry,
																				 *first.family_pin,
																				 framed_cohort,
																				 framed_request,
																				 open_token,
																				 owner_kind::map,
																				 owner_token,
																				 generation);
			auto second_scope =
				sqlite_same_process_shm_registry_test_peer::scope(*second.registry,
																  *second.family_pin,
																  framed_cohort,
																  framed_request,
																  open_token,
																  owner_kind::map,
																  owner_token,
																  generation);
			auto first_callback =
				issue_callback(first_issuer, first_scope, callback_role::map, 1U, 9U);
			auto second_callback =
				issue_callback(second_issuer, second_scope, callback_role::map, 1U, 9U);
			const auto first_projection = first_callback.receipt().invocation_token;
			const auto second_projection = second_callback.receipt().invocation_token;
			const auto first_hidden = verify_projection_encoding(
				first_projection,
				first.family,
				framed_cohort,
				framed_request,
				open_token,
				owner_kind::map,
				owner_token,
				generation,
				1U,
				sqlite_shm_reader_lifecycle_identity_domain::callback_invocation,
				static_cast<std::uint8_t>(callback_role::map));
			const auto second_hidden = verify_projection_encoding(
				second_projection,
				second.family,
				framed_cohort,
				framed_request,
				open_token,
				owner_kind::map,
				owner_token,
				generation,
				1U,
				sqlite_shm_reader_lifecycle_identity_domain::callback_invocation,
				static_cast<std::uint8_t>(callback_role::map));
			require(
				first_hidden.sequence == 1U && second_hidden.sequence == 1U &&
					first_hidden.process_epoch == second_hidden.process_epoch &&
					first_hidden.family_epoch == second_hidden.family_epoch &&
					first_hidden.family_pin_token == second_hidden.family_pin_token &&
					first_hidden.incarnation != second_hidden.incarnation &&
					first_projection != second_projection,
				"hidden incarnation alone separates identical first-sequence public projections");
			require_rejection(
				first_issuer.validate_callback(first_scope, second_callback, callback_role::map),
				rejection_reason::receipt_mismatch,
				"first registry rejects second hidden control at the same sequence");
			require_rejection(
				second_issuer.validate_callback(second_scope, first_callback, callback_role::map),
				rejection_reason::receipt_mismatch,
				"second registry rejects first hidden control at the same sequence");
			require(first_issuer.validate_callback(first_scope, first_callback, callback_role::map)
							.has_value() &&
						second_issuer
							.validate_callback(second_scope, second_callback, callback_role::map)
							.has_value(),
					"foreign-control rejection preserves both exact first-sequence presenters");
			retire_callback_and_scope(
				first_issuer, first_scope, first_callback, callback_role::map);
			retire_callback_and_scope(
				second_issuer, second_scope, second_callback, callback_role::map);
		}

		void verify_session_terminal_roles_share_the_process_sequence()
		{
			auto value = make_fixture(26U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			auto map_scope = make_scope(value, owner_kind::map, 1U);
			auto callback = issue_callback(issuer, map_scope, callback_role::map, 1U);
			auto effect = issuer.issue_effect(map_scope, callback, effect_role::mapped_result);
			require(effect && effect->valid(), "issue callback/effect sequence prefix");
			const auto callback_sequence = projection_sequence(callback.receipt().invocation_token);
			const auto effect_sequence = projection_sequence(effect->identity());

			constexpr std::array roles{terminal_role::success,
									   terminal_role::failure,
									   terminal_role::cancelled_before_authority_read};
			for (std::size_t index = 0U; index < roles.size(); ++index)
			{
				auto scope = make_scope(value, owner_kind::session, 10U + index);
				auto terminal = issuer.issue_session_terminal(scope, roles[index]);
				require(terminal && terminal->valid() &&
							projection_sequence(terminal->identity()) ==
								effect_sequence + index + 1U &&
							issuer.validate_session_terminal(scope, *terminal, roles[index])
								.has_value(),
						"session terminal role uses the common checked process sequence");
				auto duplicate = issuer.issue_session_terminal(scope, roles[index]);
				require_rejection(duplicate,
								  rejection_reason::stale_token,
								  "session terminal scope is mutually exclusive and one-shot");
				const auto wrong_role = roles[(index + 1U) % roles.size()];
				require_rejection(issuer.validate_session_terminal(scope, *terminal, wrong_role),
								  rejection_reason::receipt_mismatch,
								  "session terminal proof rejects a different terminal role");
				require(
					issuer.retire_session_terminal(scope, *terminal, roles[index]).has_value() &&
						!terminal->valid() && issuer.retire_scope(scope).has_value(),
					"retire exact session-terminal proof and scope");
			}
			require(callback_sequence + 1U == effect_sequence,
					"callback and effect consume adjacent positions before session terminals");
			auto map_terminal = issuer.issue_session_terminal(map_scope, terminal_role::success);
			require_rejection(map_terminal,
							  rejection_reason::invalid_identity,
							  "non-session owner cannot mint a session terminal");
			auto invalid_scope = make_scope(value, owner_kind::session, 20U);
			auto invalid =
				issuer.issue_session_terminal(invalid_scope, static_cast<terminal_role>(0xffU));
			require_rejection(invalid,
							  rejection_reason::invalid_identity,
							  "invalid session-terminal enum claims no sequence or role");
			auto valid_after_invalid =
				issuer.issue_session_terminal(invalid_scope, terminal_role::success);
			require(valid_after_invalid &&
						projection_sequence(valid_after_invalid->identity()) ==
							effect_sequence + 4U,
					"valid session terminal follows invalid enum without a sequence gap");
			require(issuer.retire_session_terminal(
							  invalid_scope, *valid_after_invalid, terminal_role::success)
							.has_value() &&
						issuer.retire_scope(invalid_scope).has_value(),
					"retire valid terminal after invalid enum");
			require(issuer.retire_effect(map_scope, callback, *effect, effect_role::mapped_result)
						.has_value(),
					"retire shared-sequence effect");
			retire_callback_and_scope(issuer, map_scope, callback, callback_role::map);
		}

		void retire_effects_callback_and_scope(
			sqlite_shm_process_global_identity_issuer& issuer,
			sqlite_shm_reader_lifecycle_identity_scope& scope,
			sqlite_shm_issued_reader_callback_identity& callback,
			std::vector<sqlite_shm_issued_reader_effect_identity>& effects,
			const std::vector<effect_role>& roles,
			const callback_role callback_kind)
		{
			require(effects.size() == roles.size(), "effect retirement oracle shape");
			for (std::size_t index = effects.size(); index > 0U; --index)
				require(
					issuer.retire_effect(scope, callback, effects[index - 1U], roles[index - 1U])
						.has_value(),
					"retire exact effect identity");
			retire_callback_and_scope(issuer, scope, callback, callback_kind);
		}

		void verify_effect_role_table_and_duplicates()
		{
			auto value = make_fixture(11U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			constexpr std::array effects{
				effect_role::mapped_result,
				effect_role::zero_attachment_result,
				effect_role::native_unmap,
				effect_role::latch_reset,
				effect_role::native_close,
			};

			for (const auto selected :
				 {effect_role::mapped_result, effect_role::zero_attachment_result})
			{
				auto scope = make_scope(
					value, owner_kind::map, selected == effect_role::mapped_result ? 1U : 2U);
				auto callback = issue_callback(issuer, scope, callback_role::map, 1U);
				auto issued = issuer.issue_effect(scope, callback, selected);
				require(issued.has_value() && issued->valid(),
						"map accepts selected mapped-or-zero effect");
				auto duplicate = issuer.issue_effect(scope, callback, selected);
				require_rejection(
					duplicate, rejection_reason::stale_token, "map effect role is one-shot");
				for (const auto candidate : effects)
				{
					if (candidate == selected)
						continue;
					else
					{
						auto rejected = issuer.issue_effect(scope, callback, candidate);
						const auto nominal_alternative =
							(candidate == effect_role::mapped_result ||
							 candidate == effect_role::zero_attachment_result);
						require_rejection(rejected,
										  nominal_alternative ? rejection_reason::stale_token
															  : rejection_reason::receipt_mismatch,
										  "map rejects incompatible or mutually-exclusive effect");
					}
				}
				require(issuer.validate_effect(scope, callback, *issued, selected).has_value(),
						"rejections preserve exact map effect");
				require(issuer.retire_effect(scope, callback, *issued, selected).has_value(),
						"retire selected map effect");
				retire_callback_and_scope(issuer, scope, callback, callback_role::map);
			}

			for (const auto [kind, role, token] :
				 {std::tuple{owner_kind::unpublished_cleanup,
							 callback_role::unpublished_cleanup_unmap,
							 3U},
				  std::tuple{owner_kind::attachment, callback_role::attachment_unmap, 4U}})
			{
				auto scope = make_scope(value, kind, token);
				auto callback =
					issue_callback(issuer, scope, role, static_cast<std::uint8_t>(token));
				std::vector<sqlite_shm_issued_reader_effect_identity> issued_effects;
				std::vector<effect_role> issued_roles;
				for (const auto candidate : effects)
				{
					auto issued = issuer.issue_effect(scope, callback, candidate);
					if (candidate == effect_role::native_unmap ||
						candidate == effect_role::latch_reset)
					{
						require(issued.has_value(),
								"cleanup callback accepts native and latch effects");
						auto duplicate = issuer.issue_effect(scope, callback, candidate);
						require_rejection(duplicate,
										  rejection_reason::stale_token,
										  "each cleanup effect role is one-shot");
						require(
							issuer.validate_effect(scope, callback, *issued, candidate).has_value(),
							"cleanup duplicate rejection preserves exact effect");
						issued_effects.push_back(std::move(issued.value()));
						issued_roles.push_back(candidate);
					}
					else
						require_rejection(issued,
										  rejection_reason::receipt_mismatch,
										  "cleanup callback rejects unrelated effect");
				}
				retire_effects_callback_and_scope(
					issuer, scope, callback, issued_effects, issued_roles, role);
			}

			{
				auto scope = make_scope(value, owner_kind::close, 5U);
				auto callback = issue_callback(issuer, scope, callback_role::close, 5U);
				std::vector<sqlite_shm_issued_reader_effect_identity> issued_effects;
				std::vector<effect_role> issued_roles;
				for (const auto candidate : effects)
				{
					auto issued = issuer.issue_effect(scope, callback, candidate);
					if (candidate == effect_role::native_close)
					{
						require(issued.has_value(), "close accepts only native-close effect");
						auto duplicate = issuer.issue_effect(scope, callback, candidate);
						require_rejection(duplicate,
										  rejection_reason::stale_token,
										  "native-close effect role is one-shot");
						require(
							issuer.validate_effect(scope, callback, *issued, candidate).has_value(),
							"close duplicate rejection preserves exact effect");
						issued_effects.push_back(std::move(issued.value()));
						issued_roles.push_back(candidate);
					}
					else
						require_rejection(issued,
										  rejection_reason::receipt_mismatch,
										  "close rejects non-close effect");
				}
				retire_effects_callback_and_scope(
					issuer, scope, callback, issued_effects, issued_roles, callback_role::close);
			}

			for (const auto [kind, role, token] :
				 {std::tuple{owner_kind::logical_ack, callback_role::logical_ack_unmap, 6U},
				  std::tuple{owner_kind::late_outer_unwind, callback_role::late_outer_unmap, 7U}})
			{
				auto scope = make_scope(value, kind, token);
				auto callback =
					issue_callback(issuer, scope, role, static_cast<std::uint8_t>(token));
				for (const auto candidate : effects)
					require_rejection(
						issuer.issue_effect(scope, callback, candidate),
						rejection_reason::receipt_mismatch,
						"zero-native acknowledgement callback rejects every effect identity");
				retire_callback_and_scope(issuer, scope, callback, role);
			}

			auto scope = make_scope(value, owner_kind::map, 8U);
			auto callback = issue_callback(issuer, scope, callback_role::map, 8U);
			auto invalid = issuer.issue_effect(scope, callback, static_cast<effect_role>(0xffU));
			require_rejection(invalid,
							  rejection_reason::receipt_mismatch,
							  "invalid effect enum does not claim mapped role");
			auto mapped = issuer.issue_effect(scope, callback, effect_role::mapped_result);
			require(mapped.has_value(), "valid map effect follows invalid enum");
			require(
				issuer.retire_effect(scope, callback, mapped.value(), effect_role::mapped_result)
					.has_value(),
				"retire valid map effect after invalid enum");
			retire_callback_and_scope(issuer, scope, callback, callback_role::map);
		}

		void verify_wrong_presenter_scope_claim_and_sequence()
		{
			auto value = make_fixture(12U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			const auto cohort = identity("test.identity-issuer.same-cohort", 1U);
			const auto request = identity("test.identity-issuer.same-request", 1U);
			auto first_scope = sqlite_same_process_shm_registry_test_peer::scope(*value.registry,
																				 *value.family_pin,
																				 cohort,
																				 request,
																				 101U,
																				 owner_kind::map,
																				 202U,
																				 303U);
			auto copied_assertion_scope =
				sqlite_same_process_shm_registry_test_peer::scope(*value.registry,
																  *value.family_pin,
																  cohort,
																  request,
																  101U,
																  owner_kind::map,
																  202U,
																  303U);
			require(first_scope.valid() && copied_assertion_scope.valid(),
					"for-testing mint creates independent private controls");

			auto first_permit =
				issuer.reserve_callback(first_scope,
										callback_role::map,
										identity("test.identity-issuer.presenter-thread", 1U),
										7U);
			require(first_permit.has_value(), "reserve first presenter callback");
			auto wrong_seal = issuer.seal_callback(
				first_permit.value(), copied_assertion_scope, callback_role::map);
			require_rejection(wrong_seal,
							  rejection_reason::receipt_mismatch,
							  "copied raw scope assertions cannot present another private control");
			require(first_permit->valid(), "wrong scope seal is nonmutating");
			auto first_callback =
				issuer.seal_callback(first_permit.value(), first_scope, callback_role::map);
			require(first_callback.has_value(), "exact scope seals after wrong presenter");
			const auto first_projection = first_callback->receipt().invocation_token;

			auto duplicate =
				issuer.reserve_callback(first_scope,
										callback_role::map,
										identity("test.identity-issuer.presenter-thread", 2U),
										7U);
			require_rejection(duplicate,
							  rejection_reason::stale_token,
							  "same callback role is one-shot within one scope");
			require(issuer.validate_callback(first_scope, *first_callback, callback_role::map)
						.has_value(),
					"duplicate reserve preserves first callback");
			require_rejection(issuer.validate_callback(
								  copied_assertion_scope, *first_callback, callback_role::map),
							  rejection_reason::receipt_mismatch,
							  "private control rejects identical copied scope fields");
			require(issuer.validate_callback(first_scope, *first_callback, callback_role::map)
						.has_value(),
					"wrong validation leaves exact validation intact");

			auto second_callback =
				issue_callback(issuer, copied_assertion_scope, callback_role::map, 3U, 7U);
			const auto second_projection = second_callback.receipt().invocation_token;
			auto first_effect =
				issuer.issue_effect(first_scope, *first_callback, effect_role::mapped_result);
			require(first_effect.has_value(), "issue exact effect for presenter tests");
			const auto effect_projection = first_effect->identity();
			require_rejection(
				issuer.validate_effect(
					first_scope, second_callback, *first_effect, effect_role::mapped_result),
				rejection_reason::receipt_mismatch,
				"effect rejects wrong callback presenter");
			require_rejection(issuer.retire_effect(first_scope,
												   *first_callback,
												   *first_effect,
												   effect_role::zero_attachment_result),
							  rejection_reason::receipt_mismatch,
							  "wrong effect role does not retire exact effect");
			require(issuer
						.validate_effect(
							first_scope, *first_callback, *first_effect, effect_role::mapped_result)
						.has_value(),
					"wrong effect presenters are nonmutating");
			require_rejection(
				issuer.retire_callback(first_scope, *first_callback, callback_role::close),
				rejection_reason::receipt_mismatch,
				"wrong callback role does not retire callback");
			require_rejection(
				issuer.retire_callback(first_scope, *first_callback, callback_role::map),
				rejection_reason::retiring,
				"live effect blocks callback retirement");
			require(first_callback->valid() && first_effect->valid(),
					"blocked callback retirement restores exact live phase");

			require(first_projection != second_projection &&
						first_projection != effect_projection &&
						second_projection != effect_projection,
					"callback/effect domains share a unique full identity sequence");
			const std::array sequences{
				projection_sequence(first_projection),
				projection_sequence(second_projection),
				projection_sequence(effect_projection),
			};
			require(sequences[0] != 0U && sequences[1] == sequences[0] + 1U &&
						sequences[2] == sequences[1] + 1U,
					"one registry sequence spans scopes, callbacks, and effects");

			require(issuer
						.retire_effect(
							first_scope, *first_callback, *first_effect, effect_role::mapped_result)
						.has_value(),
					"retire exact effect after presenter rejections");
			retire_callback_and_scope(issuer, first_scope, *first_callback, callback_role::map);
			retire_callback_and_scope(
				issuer, copied_assertion_scope, second_callback, callback_role::map);

			const auto collision_a = framed_identity("a", {0x62U, 0x63U});
			const auto collision_b = framed_identity("ab", {0x63U});
			auto collision_scope_a =
				sqlite_same_process_shm_registry_test_peer::scope(*value.registry,
																  *value.family_pin,
																  collision_a,
																  collision_b,
																  404U,
																  owner_kind::map,
																  505U,
																  606U);
			auto collision_scope_b =
				sqlite_same_process_shm_registry_test_peer::scope(*value.registry,
																  *value.family_pin,
																  collision_b,
																  collision_a,
																  404U,
																  owner_kind::map,
																  505U,
																  606U);
			auto collision_callback_a =
				issue_callback(issuer, collision_scope_a, callback_role::map, 4U);
			auto collision_callback_b =
				issue_callback(issuer, collision_scope_b, callback_role::map, 5U);
			require(collision_callback_a.receipt().invocation_token !=
						collision_callback_b.receipt().invocation_token,
					"distinct framed scope tuple issuance remains non-reusable");
			auto foreign = make_fixture(12U, value.process);
			auto foreign_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*foreign.registry);
			auto foreign_scope =
				sqlite_same_process_shm_registry_test_peer::scope(*foreign.registry,
																  *foreign.family_pin,
																  collision_a,
																  collision_b,
																  404U,
																  owner_kind::map,
																  505U,
																  606U);
			auto foreign_callback =
				issue_callback(foreign_issuer, foreign_scope, callback_role::map, 6U);
			require(foreign_callback.receipt().invocation_token !=
						collision_callback_a.receipt().invocation_token,
					"hidden registry incarnation separates identical copied public coordinates");
			require_rejection(
				foreign_issuer.validate_callback(
					foreign_scope, collision_callback_a, callback_role::map),
				rejection_reason::receipt_mismatch,
				"foreign registry rejects callback even after public value collision attempt");
			retire_callback_and_scope(
				foreign_issuer, foreign_scope, foreign_callback, callback_role::map);
			retire_callback_and_scope(
				issuer, collision_scope_a, collision_callback_a, callback_role::map);
			retire_callback_and_scope(
				issuer, collision_scope_b, collision_callback_b, callback_role::map);
		}

		void verify_family_release_recreation_and_quarantine()
		{
			auto value = make_fixture(14U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			sqlite_backend_opaque_identity retired_projection;
			{
				auto scope = make_scope(value, owner_kind::map, 1U);
				auto callback = issue_callback(issuer, scope, callback_role::map, 1U);
				auto effect = issuer.issue_effect(scope, callback, effect_role::mapped_result);
				require(effect.has_value(), "issue effect before family retirement");
				retired_projection = callback.receipt().invocation_token;
				require(value.registry->release_family(*value.family_pin).has_value(),
						"release exact family pin with issuer owners");
				require(!value.family_pin->valid() && !scope.valid() && !callback.valid() &&
							!effect->valid(),
						"family retirement immediately stales scope and descendants");
				require(
					!issuer.validate_callback(scope, callback, callback_role::map).has_value() &&
						!issuer
							 .validate_effect(scope, callback, *effect, effect_role::mapped_result)
							 .has_value(),
					"retired family rejects old validation without revival");
			}
			value.family_pin.reset();
			auto recreated = value.registry->install_or_join_family(*value.alias, value.family);
			require(recreated.has_value(), "recreate exact public family after retirement");
			value.family_pin.emplace(std::move(recreated.value()));
			auto recreated_scope = make_scope(value, owner_kind::map, 1U);
			auto recreated_callback =
				issue_callback(issuer, recreated_scope, callback_role::map, 2U);
			require(recreated_callback.receipt().invocation_token != retired_projection,
					"family recreation cannot reuse retired callback identity");
			retire_callback_and_scope(
				issuer, recreated_scope, recreated_callback, callback_role::map);

			auto quarantined = make_fixture(15U);
			auto quarantined_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*quarantined.registry);
			auto callback_scope = make_scope(quarantined, owner_kind::map, 1U);
			auto callback =
				issue_callback(quarantined_issuer, callback_scope, callback_role::map, 3U);
			auto effect = quarantined_issuer.issue_effect(
				callback_scope, callback, effect_role::mapped_result);
			require(effect.has_value(), "issue effect before registry quarantine");
			auto permit_scope = make_scope(quarantined, owner_kind::close, 2U);
			auto permit = quarantined_issuer.reserve_callback(
				permit_scope,
				callback_role::close,
				identity("test.identity-issuer.quarantine-thread", 1U),
				0U);
			require(permit.has_value(), "reserve permit before registry quarantine");
			require(sqlite_same_process_shm_registry_test_peer::inject_duplicate_family(
						*quarantined.registry, quarantined.family),
					"inject deterministic duplicate-family quarantine");
			auto duplicate_admission =
				quarantined.registry->acquire_activity(*quarantined.family_pin);
			require_rejection(
				duplicate_admission,
				rejection_reason::lifecycle_ambiguous,
				"duplicate family lookup deterministically enters registry quarantine");
			require(!quarantined_issuer.valid() && !callback_scope.valid() &&
						!permit_scope.valid() && !permit->valid() && !callback.valid() &&
						!effect->valid(),
					"registry quarantine invalidates issuer, scopes, permit, callback, and effect");
			require(
				!quarantined_issuer.validate_callback(callback_scope, callback, callback_role::map)
						.has_value() &&
					!quarantined_issuer
						 .issue_effect(
							 callback_scope, callback, effect_role::zero_attachment_result)
						 .has_value(),
				"quarantined registry cannot validate or issue descendants");
		}

		void verify_simultaneous_cross_family_rejection()
		{
			auto value = make_fixture(23U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			const sqlite_shm_lease_family_binding second_family{
				value.process,
				value.cohort,
				identity("test.identity-issuer.second-file-family", 1U),
			};
			auto second_pin_result =
				value.registry->install_or_join_family(*value.alias, second_family);
			require(second_pin_result.has_value(), "install simultaneous second family");
			auto second_pin = std::move(second_pin_result.value());
			const auto cohort = identity("test.identity-issuer.cross-family-cohort", 1U);
			const auto request = identity("test.identity-issuer.cross-family-request", 1U);
			auto first_scope = sqlite_same_process_shm_registry_test_peer::scope(*value.registry,
																				 *value.family_pin,
																				 cohort,
																				 request,
																				 100U,
																				 owner_kind::map,
																				 200U,
																				 300U);
			auto second_scope = sqlite_same_process_shm_registry_test_peer::scope(
				*value.registry, second_pin, cohort, request, 100U, owner_kind::map, 200U, 300U);
			auto first_callback = issue_callback(issuer, first_scope, callback_role::map, 1U);
			auto second_callback = issue_callback(issuer, second_scope, callback_role::map, 2U);
			auto first_effect =
				issuer.issue_effect(first_scope, first_callback, effect_role::mapped_result);
			auto second_effect =
				issuer.issue_effect(second_scope, second_callback, effect_role::mapped_result);
			require(first_effect.has_value() && second_effect.has_value() &&
						first_callback.receipt().invocation_token !=
							second_callback.receipt().invocation_token &&
						first_effect->identity() != second_effect->identity(),
					"simultaneous families receive distinct callback and effect identities");
			require_rejection(
				issuer.validate_callback(second_scope, first_callback, callback_role::map),
				rejection_reason::receipt_mismatch,
				"family B rejects family A callback");
			require_rejection(
				issuer.validate_callback(first_scope, second_callback, callback_role::map),
				rejection_reason::receipt_mismatch,
				"family A rejects family B callback");
			require_rejection(
				issuer.validate_effect(
					second_scope, second_callback, *first_effect, effect_role::mapped_result),
				rejection_reason::receipt_mismatch,
				"family B rejects family A effect");
			require_rejection(
				issuer.validate_effect(
					first_scope, first_callback, *second_effect, effect_role::mapped_result),
				rejection_reason::receipt_mismatch,
				"family A rejects family B effect");
			require(
				issuer.validate_effect(
						  first_scope, first_callback, *first_effect, effect_role::mapped_result)
						.has_value() &&
					issuer
						.validate_effect(second_scope,
										 second_callback,
										 *second_effect,
										 effect_role::mapped_result)
						.has_value(),
				"cross-family rejections leave exact presenters live");
			require(
				issuer.retire_effect(
						  first_scope, first_callback, *first_effect, effect_role::mapped_result)
						.has_value() &&
					issuer
						.retire_effect(second_scope,
									   second_callback,
									   *second_effect,
									   effect_role::mapped_result)
						.has_value(),
				"retire exact simultaneous-family effects");
			retire_callback_and_scope(issuer, first_scope, first_callback, callback_role::map);
			retire_callback_and_scope(issuer, second_scope, second_callback, callback_role::map);
			require(value.registry->release_family(second_pin).has_value(),
					"release simultaneous second family");
		}

		void verify_move_and_abandon_accounting()
		{
			auto value = make_fixture(24U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			auto original_scope = make_scope(value, owner_kind::map, 1U);
			auto moved_scope = std::move(original_scope);
			require(!original_scope.valid() && moved_scope.valid(),
					"scope move transfers its private presenter exactly once");
			{
				auto permit_result =
					issuer.reserve_callback(moved_scope,
											callback_role::map,
											identity("test.identity-issuer.move-permit", 1U),
											0U);
				require(permit_result.has_value() &&
							sqlite_same_process_shm_registry_test_peer::scope_count(
								issuer, moved_scope) == 1U,
						"permit reservation increments scope count once");
				auto moved_permit = std::move(permit_result.value());
				require(!permit_result->valid() && moved_permit.valid(),
						"permit move transfers reserved phase exactly once");
			}
			require(sqlite_same_process_shm_registry_test_peer::scope_count(issuer, moved_scope) ==
						0U,
					"abandoned moved permit decrements scope count once");
			require(issuer.retire_scope(moved_scope).has_value(),
					"scope retires after permit abandonment");

			auto callback_scope = make_scope(value, owner_kind::map, 2U);
			auto callback = issue_callback(issuer, callback_scope, callback_role::map, 2U);
			auto effect_result =
				issuer.issue_effect(callback_scope, callback, effect_role::mapped_result);
			require(effect_result.has_value(), "issue effect before move accounting");
			auto moved_effect = std::move(effect_result.value());
			require(!effect_result->valid() && moved_effect.valid(),
					"effect move transfers exact live child");
			require(issuer
						.retire_effect(
							callback_scope, callback, moved_effect, effect_role::mapped_result)
						.has_value(),
					"retire moved effect exactly once");
			auto moved_callback = std::move(callback);
			require(!callback.valid() && moved_callback.valid(),
					"callback move transfers exact live record");
			retire_callback_and_scope(issuer, callback_scope, moved_callback, callback_role::map);

			auto dropped_effect_scope = make_scope(value, owner_kind::map, 3U);
			auto dropped_effect_callback =
				issue_callback(issuer, dropped_effect_scope, callback_role::map, 3U);
			{
				auto dropped_effect_result = issuer.issue_effect(
					dropped_effect_scope, dropped_effect_callback, effect_role::mapped_result);
				require(dropped_effect_result.has_value(),
						"issue effect for destructor accounting");
				auto dropped_effect = std::move(dropped_effect_result.value());
				require(sqlite_same_process_shm_registry_test_peer::scope_count(
							issuer, dropped_effect_scope) == 2U &&
							sqlite_same_process_shm_registry_test_peer::child_count(
								issuer, dropped_effect_callback) == 1U,
						"live effect owns one parent child and one scope record");
				(void)dropped_effect;
			}
			require(sqlite_same_process_shm_registry_test_peer::scope_count(
						issuer, dropped_effect_scope) == 1U &&
						sqlite_same_process_shm_registry_test_peer::child_count(
							issuer, dropped_effect_callback) == 0U,
					"effect drop decrements parent and scope exactly once");
			retire_callback_and_scope(
				issuer, dropped_effect_scope, dropped_effect_callback, callback_role::map);

			auto dropped_callback_scope = make_scope(value, owner_kind::map, 4U);
			{
				auto dropped_callback =
					issue_callback(issuer, dropped_callback_scope, callback_role::map, 4U);
				require(sqlite_same_process_shm_registry_test_peer::scope_count(
							issuer, dropped_callback_scope) == 1U,
						"live callback owns one scope record");
				(void)dropped_callback;
			}
			require(sqlite_same_process_shm_registry_test_peer::scope_count(
						issuer, dropped_callback_scope) == 0U,
					"callback drop decrements scope exactly once");
			require(issuer.retire_scope(dropped_callback_scope).has_value(),
					"scope retires after callback abandonment");

			std::optional<sqlite_shm_reader_lifecycle_identity_scope> abandoned_scope;
			abandoned_scope.emplace(make_scope(value, owner_kind::map, 5U));
			std::optional<sqlite_shm_issued_reader_callback_identity> abandoned_callback;
			abandoned_callback.emplace(
				issue_callback(issuer, *abandoned_scope, callback_role::map, 5U));
			abandoned_scope.reset();
			require(!abandoned_callback->valid(),
					"scope drop invalidates a still-retained callback descendant");
			abandoned_callback.reset();
		}

		void verify_concurrent_shared_sequence_success_waves()
		{
			constexpr std::size_t count = 16U;
			auto value = make_fixture(26U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			std::vector<sqlite_shm_reader_lifecycle_identity_scope> scopes;
			scopes.reserve(count);
			for (std::size_t index = 0; index < count; ++index)
				scopes.push_back(make_scope(value, owner_kind::map, 100U + index));
			std::vector<std::optional<sqlite_shm_issued_reader_callback_identity>> callbacks(count);
			std::atomic_size_t callback_failures{};
			std::barrier<> callback_start{static_cast<std::ptrdiff_t>(count)};
			std::vector<std::thread> workers;
			workers.reserve(count);
			for (std::size_t index = 0; index < count; ++index)
			{
				workers.emplace_back(
					[&, index]
					{
						callback_start.arrive_and_wait();
						auto permit = issuer.reserve_callback(
							scopes[index],
							callback_role::map,
							identity("test.identity-issuer.concurrent-thread",
									 static_cast<std::uint8_t>(index + 1U)),
							index);
						if (!permit)
						{
							callback_failures.fetch_add(1U, std::memory_order_relaxed);
							return;
						}
						auto callback =
							issuer.seal_callback(permit.value(), scopes[index], callback_role::map);
						if (!callback)
						{
							callback_failures.fetch_add(1U, std::memory_order_relaxed);
							return;
						}
						callbacks[index].emplace(std::move(callback.value()));
					});
			}
			for (auto& worker : workers)
				worker.join();
			require(callback_failures.load(std::memory_order_relaxed) == 0U,
					"all concurrent independent callback issuances succeed");

			std::vector<std::optional<sqlite_shm_issued_reader_effect_identity>> effects(count);
			std::atomic_size_t effect_failures{};
			std::barrier<> effect_start{static_cast<std::ptrdiff_t>(count)};
			workers.clear();
			for (std::size_t index = 0; index < count; ++index)
			{
				workers.emplace_back(
					[&, index]
					{
						effect_start.arrive_and_wait();
						auto effect = issuer.issue_effect(
							scopes[index], *callbacks[index], effect_role::mapped_result);
						if (!effect)
						{
							effect_failures.fetch_add(1U, std::memory_order_relaxed);
							return;
						}
						effects[index].emplace(std::move(effect.value()));
					});
			}
			for (auto& worker : workers)
				worker.join();
			require(effect_failures.load(std::memory_order_relaxed) == 0U,
					"all concurrent independent effect issuances succeed");

			std::vector<std::uint64_t> sequences;
			std::vector<sqlite_backend_opaque_identity> projections;
			sequences.reserve(count * 2U);
			projections.reserve(count * 2U);
			for (std::size_t index = 0; index < count; ++index)
			{
				require(callbacks[index].has_value() && effects[index].has_value(),
						"concurrent waves publish every exact presenter");
				projections.push_back(callbacks[index]->receipt().invocation_token);
				projections.push_back(effects[index]->identity());
				sequences.push_back(projection_sequence(projections[projections.size() - 2U]));
				sequences.push_back(projection_sequence(projections.back()));
			}
			std::ranges::sort(sequences);
			for (std::size_t index = 0; index < sequences.size(); ++index)
				require(sequences[index] == index + 1U,
						"concurrent callback/effect waves form one contiguous nonzero sequence");
			for (std::size_t first = 0; first < projections.size(); ++first)
				for (std::size_t second = first + 1U; second < projections.size(); ++second)
					require(projections[first] != projections[second],
							"concurrent callback/effect projections are pairwise unique");

			for (std::size_t index = 0; index < count; ++index)
			{
				require(issuer
							.retire_effect(scopes[index],
										   *callbacks[index],
										   *effects[index],
										   effect_role::mapped_result)
							.has_value(),
						"retire concurrent effect presenter exactly");
				retire_callback_and_scope(
					issuer, scopes[index], *callbacks[index], callback_role::map);
			}
		}

		void verify_exhaustion_and_counter_quarantine()
		{
			auto value = make_fixture(16U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			auto existing_scope = make_scope(value, owner_kind::map, 1U);
			auto existing = issue_callback(issuer, existing_scope, callback_role::map, 1U);
			sqlite_same_process_shm_registry_test_peer::exhaust(*value.registry);
			auto max_scope = make_scope(value, owner_kind::map, 2U);
			auto max_permit = issuer.reserve_callback(
				max_scope, callback_role::map, identity("test.identity-issuer.max-thread", 1U), 0U);
			require(max_permit.has_value(), "checked sequence permits UINT64_MAX exactly once");
			auto max_callback =
				issuer.seal_callback(max_permit.value(), max_scope, callback_role::map);
			require(max_callback.has_value() &&
						projection_sequence(max_callback->receipt().invocation_token) ==
							std::numeric_limits<std::uint64_t>::max(),
					"forced boundary issues exact nonzero UINT64_MAX identity");
			auto exhausted_scope = make_scope(value, owner_kind::map, 3U);
			auto exhausted =
				issuer.reserve_callback(exhausted_scope,
										callback_role::map,
										identity("test.identity-issuer.exhausted-thread", 1U),
										0U);
			require_rejection(exhausted,
							  rejection_reason::generation_exhausted,
							  "sequence remains permanently exhausted after UINT64_MAX");
			auto exhausted_terminal_scope = make_scope(value, owner_kind::session, 30U);
			auto exhausted_terminal =
				issuer.issue_session_terminal(exhausted_terminal_scope, terminal_role::success);
			require_rejection(exhausted_terminal,
							  rejection_reason::generation_exhausted,
							  "session-terminal domain shares permanent no-wrap exhaustion");
			sqlite_same_process_shm_registry_test_peer::exhaust(*value.registry);
			auto replay_scope = make_scope(value, owner_kind::map, 4U);
			auto replay =
				issuer.reserve_callback(replay_scope,
										callback_role::map,
										identity("test.identity-issuer.reexhaust-thread", 1U),
										0U);
			require_rejection(replay,
							  rejection_reason::generation_exhausted,
							  "repeated exhaustion hook cannot resurrect zero to UINT64_MAX");
			require(issuer.validate_callback(existing_scope, existing, callback_role::map)
							.has_value() &&
						issuer.validate_callback(max_scope, *max_callback, callback_role::map)
							.has_value(),
					"exhaustion preserves preexisting and UINT64_MAX validation");
			retire_callback_and_scope(issuer, max_scope, *max_callback, callback_role::map);
			retire_callback_and_scope(issuer, existing_scope, existing, callback_role::map);
			require(issuer.retire_scope(exhausted_scope).has_value() &&
						issuer.retire_scope(exhausted_terminal_scope).has_value() &&
						issuer.retire_scope(replay_scope).has_value(),
					"failed exhaustion scopes retain no live owners");

			auto overflow = make_fixture(17U);
			auto overflow_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*overflow.registry);
			auto scope_overflow = make_scope(overflow, owner_kind::map, 1U);
			sqlite_same_process_shm_registry_test_peer::set_scope_count(
				overflow_issuer, scope_overflow, std::numeric_limits<std::size_t>::max());
			auto scope_rejected = overflow_issuer.reserve_callback(
				scope_overflow,
				callback_role::map,
				identity("test.identity-issuer.scope-overflow", 1U),
				0U);
			require_rejection(scope_rejected,
							  rejection_reason::lifecycle_ambiguous,
							  "scope live-record overflow quarantines instead of wrapping");
			require(!scope_overflow.valid() &&
						sqlite_same_process_shm_registry_test_peer::scope_count(overflow_issuer,
																				scope_overflow) ==
							std::numeric_limits<std::size_t>::max(),
					"scope overflow is sticky and leaves counter unwrapped");
			sqlite_same_process_shm_registry_test_peer::set_scope_count(
				overflow_issuer, scope_overflow, 0U);

			auto child_overflow_scope = make_scope(overflow, owner_kind::map, 2U);
			auto child_overflow_callback =
				issue_callback(overflow_issuer, child_overflow_scope, callback_role::map, 2U);
			sqlite_same_process_shm_registry_test_peer::set_child_count(
				overflow_issuer, child_overflow_callback, std::numeric_limits<std::size_t>::max());
			auto child_rejected = overflow_issuer.issue_effect(
				child_overflow_scope, child_overflow_callback, effect_role::mapped_result);
			require_rejection(child_rejected,
							  rejection_reason::lifecycle_ambiguous,
							  "callback child overflow quarantines instead of wrapping");
			require(!child_overflow_scope.valid() && !child_overflow_callback.valid() &&
						sqlite_same_process_shm_registry_test_peer::child_count(
							overflow_issuer, child_overflow_callback) ==
							std::numeric_limits<std::size_t>::max(),
					"child overflow invalidates descendants without counter wrap");
			sqlite_same_process_shm_registry_test_peer::set_child_count(
				overflow_issuer, child_overflow_callback, 0U);
			sqlite_same_process_shm_registry_test_peer::set_scope_count(
				overflow_issuer, child_overflow_scope, 1U);

			auto underflow = make_fixture(18U);
			auto underflow_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*underflow.registry);
			auto underflow_scope = make_scope(underflow, owner_kind::map, 1U);
			auto underflow_callback =
				issue_callback(underflow_issuer, underflow_scope, callback_role::map, 3U);
			sqlite_same_process_shm_registry_test_peer::set_scope_count(
				underflow_issuer, underflow_scope, 0U);
			auto underflow_rejected = underflow_issuer.retire_callback(
				underflow_scope, underflow_callback, callback_role::map);
			require_rejection(underflow_rejected,
							  rejection_reason::lifecycle_ambiguous,
							  "scope live-record underflow quarantines instead of wrapping");
			require(!underflow_scope.valid() && !underflow_callback.valid() &&
						sqlite_same_process_shm_registry_test_peer::scope_count(
							underflow_issuer, underflow_scope) == 0U,
					"underflow remains zero and cannot revive owner");

			auto effect_underflow = make_fixture(19U);
			auto effect_underflow_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*effect_underflow.registry);
			auto effect_underflow_scope = make_scope(effect_underflow, owner_kind::map, 1U);
			auto effect_underflow_callback = issue_callback(
				effect_underflow_issuer, effect_underflow_scope, callback_role::map, 4U);
			auto effect_underflow_identity = effect_underflow_issuer.issue_effect(
				effect_underflow_scope, effect_underflow_callback, effect_role::mapped_result);
			require(effect_underflow_identity.has_value(), "issue effect before child underflow");
			sqlite_same_process_shm_registry_test_peer::set_child_count(
				effect_underflow_issuer, effect_underflow_callback, 0U);
			auto effect_retire = effect_underflow_issuer.retire_effect(effect_underflow_scope,
																	   effect_underflow_callback,
																	   *effect_underflow_identity,
																	   effect_role::mapped_result);
			require_rejection(effect_retire,
							  rejection_reason::lifecycle_ambiguous,
							  "effect retirement child underflow quarantines without wrapping");
			require(
				!effect_underflow_scope.valid() && !effect_underflow_callback.valid() &&
					!effect_underflow_identity->valid() &&
					sqlite_same_process_shm_registry_test_peer::child_count(
						effect_underflow_issuer, effect_underflow_callback) == 0U &&
					sqlite_same_process_shm_registry_test_peer::scope_count(
						effect_underflow_issuer, effect_underflow_scope) == 1U,
				"effect underflow quarantines every descendant and preserves exact scope count");
		}

		void wait_for_pause(sqlite_shm_process_global_identity_issuer& issuer,
							const sqlite_shm_identity_issuer_pause_point_for_testing point,
							const std::string_view message)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
			while (std::chrono::steady_clock::now() < deadline)
			{
				if (sqlite_same_process_shm_registry_test_peer::pause_entered(issuer, point))
					return;
				std::this_thread::yield();
			}
			throw std::runtime_error{std::string{message}};
		}

		void verify_deterministic_phase_races_and_wrapper_lifetime()
		{
			using pause_point = sqlite_shm_identity_issuer_pause_point_for_testing;
			auto value = make_fixture(20U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			auto effect_scope = make_scope(value, owner_kind::unpublished_cleanup, 1U);
			auto callback =
				issue_callback(issuer, effect_scope, callback_role::unpublished_cleanup_unmap, 1U);
			std::optional<sqlite_shm_issued_reader_effect_identity> native_effect;
			std::optional<sqlite_shm_lease_rejection> native_error;
			sqlite_same_process_shm_registry_test_peer::arm_pause(
				issuer, pause_point::effect_after_callback_phase);
			std::thread issuing_thread{[&]
									   {
										   auto result = issuer.issue_effect(
											   effect_scope, callback, effect_role::native_unmap);
										   if (result)
											   native_effect.emplace(std::move(result.value()));
										   else
											   native_error.emplace(result.error());
									   }};
			wait_for_pause(issuer,
						   pause_point::effect_after_callback_phase,
						   "effect issuance did not enter deterministic pause");
			auto racing_retire = issuer.retire_callback(
				effect_scope, callback, callback_role::unpublished_cleanup_unmap);
			require(!racing_retire.has_value(),
					"callback retirement cannot cross paused effect issuance");
			sqlite_same_process_shm_registry_test_peer::release_pause(issuer);
			issuing_thread.join();
			require(native_effect.has_value() && !native_error.has_value(),
					"paused effect issuance linearizes successfully");
			auto latch_effect =
				issuer.issue_effect(effect_scope, callback, effect_role::latch_reset);
			require(latch_effect.has_value(),
					"losing retire presenter burns neither callback nor second effect role");
			require_rejection(issuer.retire_callback(
								  effect_scope, callback, callback_role::unpublished_cleanup_unmap),
							  rejection_reason::retiring,
							  "successful effects block callback retirement");
			require(issuer.retire_effect(
							  effect_scope, callback, *latch_effect, effect_role::latch_reset)
							.has_value() &&
						issuer
							.retire_effect(
								effect_scope, callback, *native_effect, effect_role::native_unmap)
							.has_value(),
					"retire both effects after deterministic race");
			retire_callback_and_scope(
				issuer, effect_scope, callback, callback_role::unpublished_cleanup_unmap);

			auto reserve_scope = make_scope(value, owner_kind::map, 2U);
			std::optional<sqlite_shm_reader_callback_identity_permit> reserved;
			std::optional<sqlite_shm_lease_rejection> reserve_error;
			sqlite_same_process_shm_registry_test_peer::arm_pause(
				issuer, pause_point::reserve_after_scope_count);
			std::thread reserve_thread{[&]
									   {
										   auto result = issuer.reserve_callback(
											   reserve_scope,
											   callback_role::map,
											   identity("test.identity-issuer.race-thread", 1U),
											   0U);
										   if (result)
											   reserved.emplace(std::move(result.value()));
										   else
											   reserve_error.emplace(result.error());
									   }};
			wait_for_pause(issuer,
						   pause_point::reserve_after_scope_count,
						   "callback reservation did not enter deterministic pause");
			require_rejection(issuer.retire_scope(reserve_scope),
							  rejection_reason::retiring,
							  "scope retirement cannot cross paused reservation");
			sqlite_same_process_shm_registry_test_peer::release_pause(issuer);
			reserve_thread.join();
			require(reserved.has_value() && !reserve_error.has_value(),
					"reservation wins exact scope-retire total order");
			auto reserved_callback =
				issuer.seal_callback(*reserved, reserve_scope, callback_role::map);
			require(reserved_callback.has_value(), "seal callback after reserve/scope race");
			retire_callback_and_scope(
				issuer, reserve_scope, *reserved_callback, callback_role::map);

			auto wrapper = make_fixture(21U);
			auto wrapper_issuer =
				sqlite_same_process_shm_registry_test_peer::issuer(*wrapper.registry);
			auto wrapper_scope = make_scope(wrapper, owner_kind::map, 1U);
			std::optional<sqlite_shm_reader_callback_identity_permit> wrapper_permit;
			std::optional<sqlite_shm_lease_rejection> wrapper_error;
			sqlite_same_process_shm_registry_test_peer::arm_pause(
				wrapper_issuer, pause_point::reserve_after_scope_count);
			std::thread wrapper_thread{[&]
									   {
										   auto result = wrapper_issuer.reserve_callback(
											   wrapper_scope,
											   callback_role::map,
											   identity("test.identity-issuer.wrapper-thread", 1U),
											   0U);
										   if (result)
											   wrapper_permit.emplace(std::move(result.value()));
										   else
											   wrapper_error.emplace(result.error());
									   }};
			wait_for_pause(wrapper_issuer,
						   pause_point::reserve_after_scope_count,
						   "wrapper reservation did not enter deterministic pause");
			wrapper.registry.reset();
			require(!wrapper_issuer.valid() && !wrapper_scope.valid(),
					"registry wrapper destructor invalidates facade and scope before release");
			sqlite_same_process_shm_registry_test_peer::release_pause(wrapper_issuer);
			wrapper_thread.join();
			require(!wrapper_permit.has_value() && wrapper_error.has_value() &&
						wrapper_error->reason == rejection_reason::stale_token &&
						sqlite_same_process_shm_registry_test_peer::scope_count(
							wrapper_issuer, wrapper_scope) == 0U,
					"in-flight reservation unwinds exactly after wrapper destruction");
		}

		void verify_fork_stale_fast_path_and_destructors()
		{
			auto value = make_fixture(22U);
			auto issuer = sqlite_same_process_shm_registry_test_peer::issuer(*value.registry);
			auto callback_scope = make_scope(value, owner_kind::map, 1U);
			auto callback = issue_callback(issuer, callback_scope, callback_role::map, 1U);
			auto effect = issuer.issue_effect(callback_scope, callback, effect_role::mapped_result);
			require(effect.has_value(), "issue live effect before fork");
			auto permit_scope = make_scope(value, owner_kind::close, 2U);
			auto permit_result =
				issuer.reserve_callback(permit_scope,
										callback_role::close,
										identity("test.identity-issuer.fork-thread", 1U),
										0U);
			require(permit_result.has_value(), "reserve live permit before fork");

			sqlite_same_process_shm_registry_test_peer::lock_registry(*value.registry);
			const auto child = ::fork();
			require(child >= 0, "fork identity issuer stale path");
			if (child == 0)
			{
				::alarm(5U);
				sqlite_same_process_shm_registry_test_peer::invalidate(*value.registry);
				const auto rejected =
					issuer.reserve_callback(permit_scope,
											callback_role::close,
											identity("test.identity-issuer.fork-child", 1U),
											0U);
				const bool stale = !issuer.valid() && !callback_scope.valid() &&
					!permit_scope.valid() && !permit_result->valid() && !callback.valid() &&
					!effect->valid() && !rejected.has_value() &&
					!issuer.validate_callback(callback_scope, callback, callback_role::map)
						 .has_value();
				{
					auto stale_callback_scope = std::move(callback_scope);
					auto stale_permit_scope = std::move(permit_scope);
					auto stale_callback = std::move(callback);
					auto stale_effect = std::move(effect.value());
					auto stale_permit = std::move(permit_result.value());
					(void)stale_callback_scope;
					(void)stale_permit_scope;
					(void)stale_callback;
					(void)stale_effect;
					(void)stale_permit;
				}
				// The parent-held registry mutex remains inherited and locked here. The stale-child
				// wrapper/state deleters must observe the invalidated process epoch and avoid it.
				value.registry.reset();
				::alarm(0U);
				::_exit(stale ? 0 : 1);
			}
			int status{};
			require(::waitpid(child, &status, 0) == child, "wait fork identity issuer child");
			sqlite_same_process_shm_registry_test_peer::unlock_registry(*value.registry);
			require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
					"fork child rejects and destroys stale owners without inherited mutex");
			require(issuer.valid() && callback_scope.valid() && permit_scope.valid() &&
						permit_result->valid() && callback.valid() && effect->valid() &&
						issuer
							.validate_effect(
								callback_scope, callback, *effect, effect_role::mapped_result)
							.has_value(),
					"fork child invalidation leaves parent identity graph live");
			require(
				issuer.retire_effect(callback_scope, callback, *effect, effect_role::mapped_result)
					.has_value(),
				"retire parent effect after child exit");
			retire_callback_and_scope(issuer, callback_scope, callback, callback_role::map);
			{
				auto abandoned = std::move(permit_result.value());
				(void)abandoned;
			}
			require(issuer.retire_scope(permit_scope).has_value(),
					"permit abandonment drains parent scope exactly once");
		}
	} // namespace
} // namespace cxxlens::sdk

int main()
{
	try
	{
		cxxlens::sdk::verify_type_traits();
		cxxlens::sdk::verify_owner_and_callback_role_table();
		cxxlens::sdk::verify_projection_framing_and_hidden_registry_incarnation();
		cxxlens::sdk::verify_session_terminal_roles_share_the_process_sequence();
		cxxlens::sdk::verify_effect_role_table_and_duplicates();
		cxxlens::sdk::verify_wrong_presenter_scope_claim_and_sequence();
		cxxlens::sdk::verify_family_release_recreation_and_quarantine();
		cxxlens::sdk::verify_simultaneous_cross_family_rejection();
		cxxlens::sdk::verify_move_and_abandon_accounting();
		cxxlens::sdk::verify_concurrent_shared_sequence_success_waves();
		cxxlens::sdk::verify_exhaustion_and_counter_quarantine();
		cxxlens::sdk::verify_deterministic_phase_races_and_wrapper_lifetime();
		cxxlens::sdk::verify_fork_stale_fast_path_and_destructors();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
