#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>

#include "facts/fact_contract.hpp"
#include "store/fact_store_access.hpp"

namespace cxxlens::test::query_fixture
{
	[[nodiscard]] inline std::string id(const std::string_view prefix, const std::uint64_t value)
	{
		std::ostringstream output;
		output << prefix << std::hex << std::nouppercase << std::setfill('0') << std::setw(64)
			   << value;
		return output.str();
	}

	inline const compile_unit_id compile_unit{id("cu_", 1U)};
	inline const build_variant_id variant_a{id("variant_", 1U)};
	inline const build_variant_id variant_b{id("variant_", 2U)};
	inline const file_id file{id("file_", 1U)};
	inline const symbol_id base_record{id("symbol_", 1U)};
	inline const symbol_id derived_record{id("symbol_", 2U)};
	inline const symbol_id other_record{id("symbol_", 3U)};
	inline const symbol_id base_step{id("symbol_", 4U)};
	inline const symbol_id derived_step{id("symbol_", 5U)};
	inline const symbol_id other_step{id("symbol_", 6U)};
	inline const type_id base_type{id("type_", 1U)};
	inline const type_id derived_type{id("type_", 2U)};
	inline const type_id other_type{id("type_", 3U)};
	inline const fact_id base_call{id("fact_", 10U)};
	inline const fact_id derived_call{id("fact_", 11U)};
	inline const fact_id other_call{id("fact_", 12U)};

	[[nodiscard]] inline source_span span(const std::uint64_t offset)
	{
		source_span value;
		value.primary = {
			source_point::at(file, offset, 1U, static_cast<std::uint32_t>(offset + 1U)),
			source_point::at(file, offset + 1U, 1U, static_cast<std::uint32_t>(offset + 2U)),
			source_range_kind::token};
		value.origin = source_origin::directly_spelled;
		value.digest = {"fnv1a64", 1U, "0123456789abcdef"};
		return value;
	}

	[[nodiscard]] inline detail::facts::detached_fact_record
	fact(const fact_kind kind,
		 const std::uint64_t number,
		 std::string key,
		 std::map<std::string, std::string> payload,
		 const std::optional<source_span>& source = std::nullopt)
	{
		detail::facts::detached_fact_record value;
		value.id = fact_id{id("fact_", number)};
		value.kind = kind;
		value.stable_key = std::move(key);
		value.source = source;
		value.origin.compile_units = {compile_unit};
		value.origin.variants = {variant_a};
		value.origin.extractor_id = "fixture.query";
		value.origin.extractor_version = "1.0.0";
		value.payload_version = 1U;
		value.payload = std::move(payload);
		return value;
	}

	[[nodiscard]] inline detail::facts::detached_fact_record symbol_fact(const std::uint64_t number,
																		 const symbol_id& symbol,
																		 std::string name,
																		 std::string qualified,
																		 std::string kind)
	{
		auto value = fact(fact_kind::symbol,
						  number,
						  "symbol:" + qualified,
						  {{"symbol.id", std::string{symbol.value()}},
						   {"symbol.kind", std::move(kind)},
						   {"symbol.linkage", "external"},
						   {"symbol.name", std::move(name)}});
		value.name =
			detail::facts::name_identity{std::move(qualified),
										 "cxxlens.query.fixture.usr." + std::to_string(number),
										 {},
										 {},
										 {}};
		return value;
	}

	[[nodiscard]] inline detail::facts::detached_fact_record type_fact(const std::uint64_t number,
																	   const type_id& type,
																	   const symbol_id& declaration,
																	   std::string spelling)
	{
		auto value = fact(fact_kind::type,
						  number,
						  "type:" + spelling,
						  {{"type.id", std::string{type.value()}},
						   {"type.kind", "record"},
						   {"type.const", "false"},
						   {"type.volatile", "false"},
						   {"type.indirection", "false"},
						   {"type.reference", "false"}});
		value.type =
			detail::facts::type_identity{spelling, std::move(spelling), declaration, {}, false};
		return value;
	}

	[[nodiscard]] inline detail::facts::detached_fact_record call_fact(const std::uint64_t number,
																	   const symbol_id& target,
																	   const type_id& receiver,
																	   const std::uint64_t offset)
	{
		return fact(fact_kind::call,
					number,
					"call:" + std::to_string(number),
					{{"call.kind", "virtual_member"},
					 {"call.direct_callee", std::string{target.value()}},
					 {"call.possible_callees", std::string{target.value()}},
					 {"call.receiver_static_type", std::string{receiver.value()}},
					 {"call.dispatch", "static_member_target"}},
					span(offset));
	}

	[[nodiscard]] inline std::shared_ptr<const detail::store::snapshot_data>
	snapshot(const bool complete = true, const bool reverse = false)
	{
		auto value = std::make_shared<detail::store::snapshot_data>();
		value->metadata.workspace_key = "query-fixture";
		value->metadata.extractor_versions.emplace("fixture.query", "1.0.0");
		value->facts = {symbol_fact(1U, base_record, "Base", "Base", "struct"),
						symbol_fact(2U, derived_record, "Derived", "Derived", "struct"),
						symbol_fact(3U, other_record, "Other", "Other", "struct"),
						symbol_fact(4U, base_step, "step", "Base::step", "method"),
						symbol_fact(5U, derived_step, "step", "Derived::step", "method"),
						symbol_fact(6U, other_step, "step", "Other::step", "method"),
						type_fact(7U, base_type, base_record, "Base"),
						type_fact(8U, derived_type, derived_record, "Derived"),
						type_fact(9U, other_type, other_record, "Other"),
						call_fact(10U, base_step, base_type, 10U),
						call_fact(11U, derived_step, derived_type, 11U),
						call_fact(12U, other_step, other_type, 12U),
						fact(fact_kind::inheritance,
							 13U,
							 "inheritance:Derived:Base",
							 {{"inheritance.derived", std::string{derived_record.value()}},
							  {"inheritance.base", std::string{base_record.value()}},
							  {"inheritance.access", "public"},
							  {"inheritance.virtual", "false"}},
							 span(13U)),
						fact(fact_kind::override_relation,
							 14U,
							 "override:Derived:Base",
							 {{"override.overriding", std::string{derived_step.value()}},
							  {"override.overridden", std::string{base_step.value()}}},
							 span(14U))};
		if (reverse)
			std::ranges::reverse(value->facts);
		std::ranges::sort(value->facts,
						  [](const auto& left, const auto& right)
						  {
							  return std::tuple{left.kind, left.stable_key, left.id.value()} <
								  std::tuple{right.kind, right.stable_key, right.id.value()};
						  });
		value->coverage.request({"compile-unit", std::string{compile_unit.value()}})
			.classify({"compile-unit",
					   std::string{compile_unit.value()},
					   complete ? coverage_state::covered : coverage_state::unresolved,
					   complete ? std::nullopt
								: std::optional<std::string>{"facts.coverage-incomplete"}});
		return value;
	}

	[[nodiscard]] inline fact_store fact_store_fixture(const bool complete = true,
													   const bool reverse = false)
	{
		return detail::fact_store_access::make_store(snapshot(complete, reverse));
	}
} // namespace cxxlens::test::query_fixture
