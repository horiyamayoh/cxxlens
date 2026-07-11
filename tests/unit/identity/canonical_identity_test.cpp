#include <algorithm>
#include <array>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <vector>

#include "core/canonical_encoding.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::identity;
	using cxxlens::detail::runtime::fnv1a_hash_adapter;

	auto check(bool condition, const char* message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	auto payload(std::string domain, bool reverse = false) -> identity_result<canonical_payload>
	{
		canonical_encoder encoder{std::move(domain), {3U, 7U}};
		if (reverse)
		{
			encoder.string_set("tags", {"z", "a", "m"})
				.string_map("attributes", {{"z", "9"}, {"a", "1"}})
				.unsigned_field("count", 0x0102030405060708ULL)
				.string_field(
					"source_key", "src/main.cpp", identity_field_role::project_relative_source);
		}
		else
		{
			encoder
				.string_field(
					"source_key", "src/main.cpp", identity_field_role::project_relative_source)
				.unsigned_field("count", 0x0102030405060708ULL)
				.string_map("attributes", {{"a", "1"}, {"z", "9"}})
				.string_set("tags", {"m", "z", "a"});
		}
		return encoder.finish();
	}

	auto stable_fact_id() -> std::string
	{
		fnv1a_hash_adapter hashes;
		identity_service service{hashes};
		collision_registry collisions;
		auto encoded = payload("cxxlens.fact-id.v1");
		return service.make_id("fact", encoded.value(), collisions).value();
	}
} // namespace

auto main(int argc, char** argv) -> int
{
	using namespace cxxlens::detail::identity;
	auto normal = payload("cxxlens.fact-id.v1");
	auto reordered = payload("cxxlens.fact-id.v1", true);
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
	{
		std::cout << hexadecimal(normal.value().bytes) << '\n' << stable_fact_id() << '\n';
		return 0;
	}
	bool passed = check(normal && reordered && normal.value().bytes == reordered.value().bytes,
						"field/map/set insertion order changed canonical bytes");

	fnv1a_hash_adapter hashes;
	identity_service service{hashes};
	collision_registry fact_collisions;
	collision_registry finding_collisions;
	auto fact = service.make_id("fact", normal.value(), fact_collisions);
	auto finding_payload = payload("cxxlens.finding-id.v1");
	auto finding = service.make_id("finding", finding_payload.value(), finding_collisions);
	passed &= check(fact && finding && fact.value() != finding.value(), "domain separation failed");
	fact_id typed{fact.value()};
	passed &= check(typed.valid() && typed.full_digest().size() == 64U &&
						typed.short_display(8U).size() < typed.value().size(),
					"full digest and short display were conflated");

	canonical_encoder string_encoder{"cxxlens.boundary.v1", {1U, 1U}};
	canonical_encoder unsigned_encoder{"cxxlens.boundary.v1", {1U, 1U}};
	string_encoder.string_field("value", "1");
	unsigned_encoder.unsigned_field("value", 1U);
	passed &=
		check(string_encoder.finish().value().bytes != unsigned_encoder.finish().value().bytes,
			  "field type boundary was ambiguous");
	canonical_encoder left{"cxxlens.boundary.v1", {1U, 1U}};
	canonical_encoder right{"cxxlens.boundary.v1", {1U, 1U}};
	left.string_field("a", "bc");
	right.string_field("ab", "c");
	passed &= check(left.finish().value().bytes != right.finish().value().bytes,
					"field concatenation boundary was ambiguous");
	passed &=
		check(hexadecimal(normal.value().bytes).find("02080102030405060708") != std::string::npos,
			  "integer encoding was not fixed-width big-endian");

	canonical_encoder scalar_encoder{"cxxlens.scalars.v1", {1U, 1U}};
	const std::array raw_bytes{std::byte{0x00}, std::byte{0xFF}};
	scalar_encoder.signed_field("signed_value", -7)
		.enum_field("enum_value", 3)
		.boolean_field("enabled", true)
		.bytes_field("raw", raw_bytes)
		.optional_string("present", std::string{"yes"})
		.optional_string("absent", std::nullopt);
	passed &=
		check(static_cast<bool>(scalar_encoder.finish()), "scalar/bytes/optional encoding failed");

	canonical_encoder sequence_a{"cxxlens.sequence.v1", {1U, 1U}};
	canonical_encoder sequence_b{"cxxlens.sequence.v1", {1U, 1U}};
	sequence_a.string_sequence("values", {"a", "b"});
	sequence_b.string_sequence("values", {"b", "a"});
	passed &= check(sequence_a.finish().value().bytes != sequence_b.finish().value().bytes,
					"semantic sequence order was silently discarded");

	for (const auto role : {identity_field_role::absolute_path,
							identity_field_role::wall_time,
							identity_field_role::process_id,
							identity_field_role::thread_id,
							identity_field_role::pointer_value,
							identity_field_role::cache_state,
							identity_field_role::observation_order,
							identity_field_role::diagnostic_message})
	{
		canonical_encoder invalid{"cxxlens.invalid.v1", {1U, 1U}};
		invalid.string_field("input", "value", role);
		passed &= check(!invalid.finish() &&
							invalid.finish().error().code ==
								identity_error_code::forbidden_identity_input,
						"forbidden identity role accepted");
	}
	canonical_encoder absolute{"cxxlens.invalid.v1", {1U, 1U}};
	absolute.string_field(
		"source_key", "/checkout/src/a.cpp", identity_field_role::project_relative_source);
	passed &= check(!absolute.finish(), "absolute source path accepted");
	canonical_encoder message{"cxxlens.invalid.v1", {1U, 1U}};
	message.string_field("message", "diagnostic prose");
	passed &= check(!message.finish(), "diagnostic message field accepted");
	passed &=
		check(!canonical_encoder{"bad", {0U, 0U}}.finish(), "missing domain/version accepted");
	canonical_encoder duplicate_field{"cxxlens.invalid.v1", {1U, 1U}};
	duplicate_field.string_field("value", "a").string_field("value", "b");
	passed &=
		check(!duplicate_field.finish() &&
				  duplicate_field.finish().error().code == identity_error_code::duplicate_field,
			  "duplicate field accepted");
	canonical_encoder duplicate_map{"cxxlens.invalid.v1", {1U, 1U}};
	duplicate_map.string_map("values", {{"same", "a"}, {"same", "b"}});
	passed &=
		check(!duplicate_map.finish() &&
				  duplicate_map.finish().error().code == identity_error_code::duplicate_map_key,
			  "duplicate map key accepted");

	collision_registry forced;
	passed &= check(forced.check_and_register("forced_deadbeef", "payload-a") &&
						!forced.check_and_register("forced_deadbeef", "payload-b") &&
						forced.check_and_register("forced_deadbeef", "payload-a"),
					"distinct payload collision did not hard fail");

	collision_registry files;
	auto file_a = service.make_file_id("src/main.cpp", files);
	auto file_b = service.make_file_id("src/main.cpp", files);
	passed &= check(file_a && file_b && file_a.value() == file_b.value() && file_a.value().valid(),
					"root-independent file identity failed");
	passed &= check(!service.make_file_id("/checkout/src/main.cpp", files),
					"absolute checkout root entered file identity");

	std::array<std::string, 8> parallel{};
	std::vector<std::jthread> workers;
	for (std::size_t index = 0; index < parallel.size(); ++index)
		workers.emplace_back(
			[&, index]
			{
				parallel[index] = stable_fact_id();
			});
	workers.clear();
	passed &= check(std::ranges::all_of(parallel,
										[&](const auto& id)
										{
											return id == fact.value();
										}),
					"jobs 1/2/8 or thread order changed stable ID");

	std::locale::global(std::locale::classic());
	passed &= check(stable_fact_id() == fact.value(), "locale changed canonical ID");
	return passed ? 0 : 1;
}
