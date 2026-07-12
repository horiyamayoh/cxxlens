#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "../core/canonical_json.hpp"
#include "../core/json_projections.hpp"
#include "fact_store_access.hpp"

namespace cxxlens
{
	namespace
	{
		[[nodiscard]] error projection_error(std::string reason)
		{
			error failure;
			failure.code.value = "facts.store-corrupt";
			failure.message = "Stored fact projection failed";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string payload(const detail::facts::detached_fact_record& fact,
										  const std::string_view key)
		{
			if (const auto found = fact.payload.find(std::string{key}); found != fact.payload.end())
				return found->second;
			return {};
		}

		[[nodiscard]] bool boolean(const std::string_view value)
		{
			return value == "true";
		}

		[[nodiscard]] std::string_view kind_name(const fact_kind kind)
		{
			constexpr std::array<std::string_view, 21U> names{"file",
															  "compile_command",
															  "symbol",
															  "type",
															  "declaration",
															  "definition",
															  "reference",
															  "call",
															  "construction",
															  "conversion",
															  "inheritance",
															  "override_relation",
															  "include_relation",
															  "macro_definition",
															  "macro_expansion",
															  "cfg_summary",
															  "flow_summary",
															  "effect_summary",
															  "dynamic_observation",
															  "coverage_region",
															  "custom"};
			const auto index = static_cast<std::size_t>(kind);
			return index < names.size() ? names.at(index) : names.back();
		}

		[[nodiscard]] symbol_kind parse_symbol_kind(const std::string_view value)
		{
			const std::map<std::string_view, symbol_kind> values{
				{"namespace", symbol_kind::namespace_},
				{"record", symbol_kind::record},
				{"class", symbol_kind::class_},
				{"struct", symbol_kind::struct_},
				{"union", symbol_kind::union_},
				{"function", symbol_kind::function},
				{"method", symbol_kind::method},
				{"constructor", symbol_kind::constructor},
				{"destructor", symbol_kind::destructor},
				{"variable", symbol_kind::variable},
				{"field", symbol_kind::field},
				{"enum", symbol_kind::enum_type},
				{"enum_constant", symbol_kind::enum_constant},
				{"typedef", symbol_kind::typedef_},
				{"type_alias", symbol_kind::type_alias},
				{"template", symbol_kind::template_},
				{"concept", symbol_kind::concept_},
				{"parameter", symbol_kind::parameter}};
			if (const auto found = values.find(value); found != values.end())
				return found->second;
			return symbol_kind::unknown;
		}

		[[nodiscard]] linkage_kind parse_linkage(const std::string_view value)
		{
			if (value == "none")
				return linkage_kind::none;
			if (value == "internal")
				return linkage_kind::internal;
			if (value == "unique_external")
				return linkage_kind::unique_external;
			if (value == "external")
				return linkage_kind::external;
			if (value == "module")
				return linkage_kind::module;
			return linkage_kind::unknown;
		}

		[[nodiscard]] type_kind parse_type_kind(const std::string_view value)
		{
			const std::map<std::string_view, type_kind> values{
				{"builtin", type_kind::builtin},
				{"pointer", type_kind::pointer},
				{"lvalue_reference", type_kind::lvalue_reference},
				{"rvalue_reference", type_kind::rvalue_reference},
				{"array", type_kind::array},
				{"function", type_kind::function},
				{"record", type_kind::record},
				{"enum", type_kind::enum_},
				{"typedef", type_kind::typedef_},
				{"auto", type_kind::auto_},
				{"decltype", type_kind::decltype_},
				{"template_parameter", type_kind::template_parameter},
				{"template_specialization", type_kind::template_specialization},
				{"dependent", type_kind::dependent}};
			if (const auto found = values.find(value); found != values.end())
				return found->second;
			return type_kind::unknown;
		}

		[[nodiscard]] reference_kind parse_reference_kind(const std::string_view value)
		{
			if (value == "read")
				return reference_kind::read;
			if (value == "write")
				return reference_kind::write;
			if (value == "read_write")
				return reference_kind::read_write;
			if (value == "call")
				return reference_kind::call;
			if (value == "address_taken")
				return reference_kind::address_taken;
			return reference_kind::unknown;
		}

		[[nodiscard]] call_kind parse_call_kind(const std::string_view value)
		{
			if (value == "direct_function")
				return call_kind::direct_function;
			if (value == "member")
				return call_kind::member;
			if (value == "virtual_member")
				return call_kind::virtual_member;
			if (value == "constructor")
				return call_kind::constructor;
			if (value == "destructor")
				return call_kind::destructor;
			if (value == "overloaded_operator")
				return call_kind::overloaded_operator;
			if (value == "builtin_operator")
				return call_kind::builtin_operator;
			if (value == "function_pointer")
				return call_kind::function_pointer;
			return call_kind::unknown;
		}

		[[nodiscard]] dispatch_kind parse_dispatch(const std::string_view value)
		{
			if (value == "direct_exact")
				return dispatch_kind::direct_exact;
			if (value == "static_member_target")
				return dispatch_kind::static_member_target;
			if (value == "virtual_candidate_set")
				return dispatch_kind::virtual_candidate_set;
			if (value == "indirect_candidate_set")
				return dispatch_kind::indirect_candidate_set;
			return dispatch_kind::unresolved;
		}

		[[nodiscard]] access_kind parse_access(const std::string_view value)
		{
			if (value == "public")
				return access_kind::public_;
			if (value == "protected")
				return access_kind::protected_;
			if (value == "private")
				return access_kind::private_;
			return access_kind::none;
		}

		[[nodiscard]] std::vector<symbol_id> symbol_list(const std::string_view value)
		{
			std::vector<symbol_id> output;
			std::size_t begin{};
			while (begin < value.size())
			{
				const auto end = value.find(',', begin);
				const auto token =
					value.substr(begin, end == value.npos ? value.size() - begin : end - begin);
				symbol_id candidate{std::string{token}};
				if (candidate.valid())
					output.push_back(std::move(candidate));
				if (end == value.npos)
					break;
				begin = end + 1U;
			}
			std::ranges::sort(output,
							  {},
							  [](const symbol_id& candidate)
							  {
								  return candidate.value();
							  });
			const auto duplicate = std::ranges::unique(output);
			output.erase(duplicate.begin(), duplicate.end());
			return output;
		}

		[[nodiscard]] evidence stored_evidence(const fact_id& id,
											   evidence_kind kind,
											   const std::optional<source_span>& source)
		{
			evidence output;
			evidence_item item;
			item.kind = kind;
			item.summary = "stored canonical fact";
			item.source = source;
			item.supporting_facts.push_back(id);
			item.attributes.emplace("source", "fact-store");
			output.add(std::move(item));
			return output;
		}
	} // namespace

	struct fact::data
	{
		detail::facts::detached_fact_record record;
	};
	struct symbol::data
	{
		symbol_id id;
		symbol_kind kind{symbol_kind::unknown};
		std::string name;
		std::string qualified_name;
		std::optional<std::string> usr;
		linkage_kind linkage{linkage_kind::unknown};
		std::optional<source_span> declaration;
		std::optional<source_span> definition;
		std::vector<build_variant_id> variants;
	};
	struct type_ref::data
	{
		type_id id;
		type_kind kind{type_kind::unknown};
		std::string spelling;
		std::string canonical;
		std::optional<symbol_id> declaration;
		bool is_const{};
		bool is_volatile{};
		bool is_pointer{};
		bool is_reference{};
		std::vector<type_ref> arguments;
	};
	struct reference::data
	{
		fact_id id;
		symbol_id target;
		reference_kind kind{reference_kind::unknown};
		source_span location;
		bool from_macro{};
		evidence why;
	};
	struct call_site::data
	{
		fact_id id;
		call_kind kind{call_kind::unknown};
		source_span location;
		std::optional<symbol_id> caller;
		std::optional<symbol_id> direct_callee;
		std::vector<symbol_id> possible_callees;
		std::optional<type_ref> receiver;
		dispatch_kind dispatch{dispatch_kind::unresolved};
		confidence certainty{confidence::possible};
		result_guarantee guarantee{result_guarantee::best_effort};
		evidence why;
	};
	struct fact_query::data
	{
		std::optional<fact_kind> kind;
		std::optional<file_id> file;
		std::optional<symbol_id> owner;
		std::optional<source_span> range;
		std::optional<build_variant_id> variant;
		std::map<std::string, std::string> custom;
	};
	struct fact_store::data
	{
		std::shared_ptr<const detail::store::snapshot_data> snapshot;
	};

	fact::fact(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	fact_id fact::id() const
	{
		return data_ ? data_->record.id : fact_id{};
	}
	fact_kind fact::kind() const noexcept
	{
		return data_ ? data_->record.kind : fact_kind::custom;
	}
	std::string_view fact::stable_key() const noexcept
	{
		return data_ ? std::string_view{data_->record.stable_key} : std::string_view{};
	}
	std::optional<source_span> fact::source() const
	{
		return data_ ? data_->record.source : std::nullopt;
	}
	const provenance& fact::origin() const noexcept
	{
		static const provenance empty;
		return data_ ? data_->record.origin : empty;
	}
	std::string fact::to_json() const
	{
		if (!data_)
			return "{}";
		detail::json::json_value::object payload_fields;
		for (const auto& [key, value] : data_->record.payload)
			payload_fields.emplace_back(key, value);
		detail::json::json_value::array compile_units;
		for (const auto& value : data_->record.origin.compile_units)
			compile_units.emplace_back(std::string{value.value()});
		detail::json::json_value::array variants;
		for (const auto& value : data_->record.origin.variants)
			variants.emplace_back(std::string{value.value()});
		detail::json::json_value::object fields{
			{"schema", "cxxlens.fact.v1"},
			{"id", std::string{data_->record.id.value()}},
			{"kind", std::string{kind_name(data_->record.kind)}},
			{"stable_key", data_->record.stable_key},
			{"source",
			 data_->record.source ? detail::json::source_span_value(*data_->record.source)
								  : detail::json::json_value{}},
			{"provenance",
			 detail::json::json_value::object{
				 {"compile_units", std::move(compile_units)},
				 {"variants", std::move(variants)},
				 {"extractor_id", data_->record.origin.extractor_id},
				 {"extractor_version", data_->record.origin.extractor_version}}},
			{"payload",
			 detail::json::json_value::object{
				 {"version", static_cast<std::uint64_t>(data_->record.payload_version)},
				 {"fields", std::move(payload_fields)}}},
		};
		if (data_->record.name)
		{
			detail::json::json_value::object name{
				{"display_qualified_name", data_->record.name->display_qualified_name}};
			if (data_->record.name->usr)
				name.emplace_back("usr", *data_->record.name->usr);
			if (data_->record.name->semantic_owner)
				name.emplace_back("semantic_owner", *data_->record.name->semantic_owner);
			if (data_->record.name->declaration_kind)
				name.emplace_back("declaration_kind", *data_->record.name->declaration_kind);
			if (data_->record.name->signature_structure)
				name.emplace_back("signature_structure", *data_->record.name->signature_structure);
			fields.emplace_back("name_identity", std::move(name));
		}
		if (data_->record.type)
		{
			detail::json::json_value::array components;
			for (const auto& value : data_->record.type->components)
				components.emplace_back(std::string{value.value()});
			detail::json::json_value::object type{
				{"display_spelling", data_->record.type->display_spelling},
				{"canonical_structure", data_->record.type->canonical_structure},
				{"builtin", data_->record.type->builtin},
				{"components", std::move(components)}};
			if (data_->record.type->declaration)
				type.emplace_back("declaration_id",
								  std::string{data_->record.type->declaration->value()});
			fields.emplace_back("type_identity", std::move(type));
		}
		auto encoded = detail::json::write(detail::json::json_value{std::move(fields)});
		return encoded ? std::move(encoded.value()) : std::string{};
	}

	symbol::symbol(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	symbol_id symbol::id() const
	{
		return data_ ? data_->id : symbol_id{};
	}
	symbol_kind symbol::kind() const noexcept
	{
		return data_ ? data_->kind : symbol_kind::unknown;
	}
	std::string_view symbol::name() const noexcept
	{
		return data_ ? data_->name : std::string_view{};
	}
	std::string_view symbol::qualified_name() const noexcept
	{
		return data_ ? data_->qualified_name : std::string_view{};
	}
	std::optional<std::string_view> symbol::usr() const noexcept
	{
		if (!data_)
			return std::nullopt;
		return data_->usr.transform(
			[](const std::string& value)
			{
				return std::string_view{value};
			});
	}
	linkage_kind symbol::linkage() const noexcept
	{
		return data_ ? data_->linkage : linkage_kind::unknown;
	}
	std::optional<source_span> symbol::declaration() const
	{
		return data_ ? data_->declaration : std::nullopt;
	}
	std::optional<source_span> symbol::definition() const
	{
		return data_ ? data_->definition : std::nullopt;
	}
	std::span<const build_variant_id> symbol::variants() const noexcept
	{
		return data_ ? std::span<const build_variant_id>{data_->variants}
					 : std::span<const build_variant_id>{};
	}
	std::string symbol::display_name() const
	{
		return data_ ? data_->qualified_name : std::string{};
	}

	type_ref::type_ref(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	type_id type_ref::id() const
	{
		return data_ ? data_->id : type_id{};
	}
	type_kind type_ref::kind() const noexcept
	{
		return data_ ? data_->kind : type_kind::unknown;
	}
	std::string_view type_ref::spelling() const noexcept
	{
		return data_ ? data_->spelling : std::string_view{};
	}
	std::string_view type_ref::canonical_spelling() const noexcept
	{
		return data_ ? data_->canonical : std::string_view{};
	}
	std::optional<symbol_id> type_ref::declaration() const
	{
		return data_ ? data_->declaration : std::nullopt;
	}
	bool type_ref::is_const() const noexcept
	{
		return data_ && data_->is_const;
	}
	bool type_ref::is_volatile() const noexcept
	{
		return data_ && data_->is_volatile;
	}
	bool type_ref::is_pointer() const noexcept
	{
		return data_ && data_->is_pointer;
	}
	bool type_ref::is_reference() const noexcept
	{
		return data_ && data_->is_reference;
	}
	std::vector<type_ref> type_ref::template_arguments() const
	{
		return data_ ? data_->arguments : std::vector<type_ref>{};
	}

	reference::reference(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	fact_id reference::id() const
	{
		return data_ ? data_->id : fact_id{};
	}
	symbol_id reference::target() const
	{
		return data_ ? data_->target : symbol_id{};
	}
	reference_kind reference::kind() const noexcept
	{
		return data_ ? data_->kind : reference_kind::unknown;
	}
	const source_span& reference::location() const noexcept
	{
		static const source_span empty;
		return data_ ? data_->location : empty;
	}
	bool reference::from_macro() const noexcept
	{
		return data_ && data_->from_macro;
	}
	const evidence& reference::why() const noexcept
	{
		static const evidence empty;
		return data_ ? data_->why : empty;
	}

	call_site::call_site(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	fact_id call_site::id() const
	{
		return data_ ? data_->id : fact_id{};
	}
	call_kind call_site::kind() const noexcept
	{
		return data_ ? data_->kind : call_kind::unknown;
	}
	const source_span& call_site::location() const noexcept
	{
		static const source_span empty;
		return data_ ? data_->location : empty;
	}
	std::optional<symbol_id> call_site::caller() const
	{
		return data_ ? data_->caller : std::nullopt;
	}
	std::optional<symbol_id> call_site::direct_callee() const
	{
		return data_ ? data_->direct_callee : std::nullopt;
	}
	std::span<const symbol_id> call_site::possible_callees() const noexcept
	{
		return data_ ? std::span<const symbol_id>{data_->possible_callees}
					 : std::span<const symbol_id>{};
	}
	std::optional<type_ref> call_site::receiver_static_type() const
	{
		return data_ ? data_->receiver : std::nullopt;
	}
	dispatch_kind call_site::dispatch() const noexcept
	{
		return data_ ? data_->dispatch : dispatch_kind::unresolved;
	}
	confidence call_site::certainty() const noexcept
	{
		return data_ ? data_->certainty : confidence::possible;
	}
	result_guarantee call_site::guarantee() const noexcept
	{
		return data_ ? data_->guarantee : result_guarantee::best_effort;
	}
	const evidence& call_site::why() const noexcept
	{
		static const evidence empty;
		return data_ ? data_->why : empty;
	}

	fact_query::fact_query(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	fact_query fact_query::all()
	{
		return fact_query{std::make_shared<data>()};
	}
	fact_query fact_query::kind(const fact_kind value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->kind = value;
		return fact_query{std::move(copy)};
	}
	fact_query fact_query::file(file_id value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->file = std::move(value);
		return fact_query{std::move(copy)};
	}
	fact_query fact_query::owner(symbol_id value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->owner = std::move(value);
		return fact_query{std::move(copy)};
	}
	fact_query fact_query::range_intersects(source_span value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->range = std::move(value);
		return fact_query{std::move(copy)};
	}
	fact_query fact_query::variant(build_variant_id value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->variant = std::move(value);
		return fact_query{std::move(copy)};
	}
	fact_query fact_query::custom(std::string key, std::string value) const
	{
		auto copy = std::make_shared<data>(data_ ? *data_ : data{});
		copy->custom.emplace(std::move(key), std::move(value));
		return fact_query{std::move(copy)};
	}

	fact_store::fact_store(std::shared_ptr<const data> value) : data_{std::move(value)} {}

	namespace detail
	{
		namespace
		{
			[[nodiscard]] bool intersects(const source_span& left, const source_span& right)
			{
				return left.primary.begin.file == right.primary.begin.file &&
					left.primary.begin.byte_offset < right.primary.end.byte_offset &&
					right.primary.begin.byte_offset < left.primary.end.byte_offset;
			}

		} // namespace

		fact_store
		fact_store_access::make_store(std::shared_ptr<const store::snapshot_data> snapshot)
		{
			auto data = std::make_shared<fact_store::data>();
			data->snapshot = std::move(snapshot);
			return fact_store{std::move(data)};
		}

		fact fact_store_access::make_fact(facts::detached_fact_record record)
		{
			auto data = std::make_shared<fact::data>();
			data->record = std::move(record);
			return fact{std::move(data)};
		}

		result<type_ref> fact_store_access::make_type(const facts::detached_fact_record& record)
		{
			if (!record.type)
				return projection_error("type-identity-missing");
			auto data = std::make_shared<type_ref::data>();
			data->id = type_id{payload(record, "type.id")};
			if (!data->id.valid())
				return projection_error("type-id-invalid");
			data->kind = parse_type_kind(payload(record, "type.kind"));
			data->spelling = record.type->display_spelling;
			data->canonical = record.type->canonical_structure;
			data->declaration = record.type->declaration;
			data->is_const = boolean(payload(record, "type.const"));
			data->is_volatile = boolean(payload(record, "type.volatile"));
			data->is_pointer = boolean(payload(record, "type.indirection"));
			data->is_reference = boolean(payload(record, "type.reference"));
			return type_ref{std::move(data)};
		}
	} // namespace detail

	result<std::vector<fact>>
	fact_store::find(const fact_query query) const // NOLINT(performance-unnecessary-value-param)
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		if (!query.data_)
			return find(fact_query::all());
		const auto& filter = *query.data_;
		for (const auto& [key, unused] : filter.custom)
		{
			(void)unused;
			const std::string stable_key{key};
			if (!std::ranges::any_of(data_->snapshot->facts,
									 [&stable_key](const auto& record)
									 {
										 return record.payload.contains(stable_key);
									 }))
				return projection_error("unknown-custom-filter");
		}
		const auto matches = [&filter](const detail::facts::detached_fact_record& record)
		{
			if (filter.kind && record.kind != *filter.kind)
				return false;
			if (filter.file &&
				(!record.source || record.source->primary.begin.file != *filter.file))
				return false;
			if (filter.range &&
				(!record.source || !detail::intersects(*record.source, *filter.range)))
				return false;
			if (filter.variant &&
				std::ranges::find(record.origin.variants, *filter.variant) ==
					record.origin.variants.end())
				return false;
			if (filter.owner)
			{
				const auto owner = filter.owner->value();
				if (!std::ranges::any_of(record.payload,
										 [owner](const auto& field)
										 {
											 return field.second == owner;
										 }))
					return false;
			}
			for (const auto& [key, value] : filter.custom)
				if (const auto found = record.payload.find(key);
					found == record.payload.end() || found->second != value)
					return false;
			return true;
		};
		std::vector<fact> output;
		for (const auto& record : data_->snapshot->facts)
			if (matches(record))
				output.push_back(detail::fact_store_access::make_fact(record));
		return output;
	}

	result<std::vector<symbol>> fact_store::symbols() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::map<std::string, std::optional<source_span>> declarations;
		std::map<std::string, std::optional<source_span>> definitions;
		const auto select_canonical =
			[](auto& values, const std::string& id, const std::optional<source_span>& candidate)
		{
			const auto candidate_key = candidate ? candidate->to_canonical_json() : std::string{};
			if (const auto found = values.find(id); found == values.end())
				values.emplace(id, candidate);
			else
			{
				const auto existing_key =
					found->second ? found->second->to_canonical_json() : std::string{};
				if ((!found->second && candidate) || (candidate && candidate_key < existing_key))
					found->second = candidate;
			}
		};
		for (const auto& fact : data_->snapshot->facts)
		{
			const auto id = payload(fact, "symbol.id");
			if (fact.kind == fact_kind::declaration && !id.empty())
				select_canonical(declarations, id, fact.source);
			if (fact.kind == fact_kind::definition && !id.empty())
				select_canonical(definitions, id, fact.source);
		}
		std::vector<symbol> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::symbol)
				continue;
			if (!fact.name)
				return projection_error("symbol-name-identity-missing");
			auto value = std::make_shared<symbol::data>();
			value->id = symbol_id{payload(fact, "symbol.id")};
			if (!value->id.valid())
				return projection_error("symbol-id-invalid");
			value->kind = parse_symbol_kind(payload(fact, "symbol.kind"));
			value->name = payload(fact, "symbol.name");
			value->qualified_name = fact.name->display_qualified_name;
			value->usr = fact.name->usr;
			value->linkage = parse_linkage(payload(fact, "symbol.linkage"));
			value->variants = fact.origin.variants;
			if (const auto found = declarations.find(std::string{value->id.value()});
				found != declarations.end())
				value->declaration = found->second;
			if (const auto found = definitions.find(std::string{value->id.value()});
				found != definitions.end())
				value->definition = found->second;
			output.emplace_back(symbol{std::move(value)});
		}
		return output;
	}

	result<std::vector<reference>> fact_store::references(
		const symbol_id target) const // NOLINT(performance-unnecessary-value-param)
	{
		if (!target.valid() || !data_ || !data_->snapshot)
			return projection_error("invalid-reference-query");
		std::vector<reference> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::reference ||
				payload(fact, "reference.target") != target.value())
				continue;
			if (!fact.source)
				return projection_error("reference-source-missing");
			auto value = std::make_shared<reference::data>();
			value->id = fact.id;
			value->target = target;
			value->kind = parse_reference_kind(payload(fact, "reference.role"));
			value->location = *fact.source;
			value->from_macro = boolean(payload(fact, "reference.from_macro"));
			value->why = stored_evidence(fact.id, evidence_kind::canonical_symbol, fact.source);
			output.emplace_back(reference{std::move(value)});
		}
		return output;
	}

	result<std::vector<call_site>> fact_store::calls() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::map<std::string, type_ref> types;
		for (const auto& fact : data_->snapshot->facts)
			if (fact.kind == fact_kind::type)
			{
				auto projected = detail::fact_store_access::make_type(fact);
				if (!projected)
					return std::move(projected.error());
				types.emplace(std::string{projected.value().id().value()}, projected.value());
			}
		std::vector<call_site> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::call)
				continue;
			if (!fact.source)
				return projection_error("call-source-missing");
			auto value = std::make_shared<call_site::data>();
			value->id = fact.id;
			value->kind = parse_call_kind(payload(fact, "call.kind"));
			value->location = *fact.source;
			const symbol_id caller{payload(fact, "call.caller")};
			if (caller.valid())
				value->caller = caller;
			const symbol_id callee{payload(fact, "call.direct_callee")};
			if (callee.valid())
				value->direct_callee = callee;
			value->possible_callees = symbol_list(payload(fact, "call.possible_callees"));
			const auto receiver = payload(fact, "call.receiver_static_type");
			if (const auto found = types.find(receiver); found != types.end())
				value->receiver = found->second;
			value->dispatch = parse_dispatch(payload(fact, "call.dispatch"));
			value->certainty = value->dispatch == dispatch_kind::unresolved ? confidence::possible
																			: confidence::certain;
			value->guarantee = value->dispatch == dispatch_kind::unresolved
				? result_guarantee::best_effort
				: result_guarantee::exact_within_coverage;
			value->why = stored_evidence(fact.id, evidence_kind::call_resolution, fact.source);
			output.emplace_back(call_site{std::move(value)});
		}
		return output;
	}

	result<std::vector<inheritance_edge>> fact_store::inheritance() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::vector<inheritance_edge> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::inheritance)
				continue;
			inheritance_edge edge;
			edge.derived = symbol_id{payload(fact, "inheritance.derived")};
			edge.base = symbol_id{payload(fact, "inheritance.base")};
			if (!edge.derived.valid() || !edge.base.valid() || !fact.source)
				return projection_error("inheritance-payload-invalid");
			edge.access = parse_access(payload(fact, "inheritance.access"));
			edge.is_virtual = boolean(payload(fact, "inheritance.virtual"));
			edge.source = *fact.source;
			edge.why = stored_evidence(fact.id, evidence_kind::inheritance_relation, fact.source);
			output.push_back(std::move(edge));
		}
		return output;
	}

	result<std::vector<override_edge>> fact_store::overrides() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::vector<override_edge> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::override_relation)
				continue;
			override_edge edge;
			edge.overriding_method = symbol_id{payload(fact, "override.overriding")};
			edge.overridden_method = symbol_id{payload(fact, "override.overridden")};
			if (!edge.overriding_method.valid() || !edge.overridden_method.valid() || !fact.source)
				return projection_error("override-payload-invalid");
			edge.source = *fact.source;
			edge.why = stored_evidence(fact.id, evidence_kind::override_relation, fact.source);
			output.push_back(std::move(edge));
		}
		return output;
	}

	result<std::vector<include_relation>> fact_store::includes() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::vector<include_relation> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::include_relation)
				continue;
			if (!fact.source)
				return projection_error("include-source-missing");
			include_relation relation;
			relation.includer = fact.source->primary.begin.file;
			relation.spelling = payload(fact, "include.spelling");
			const file_id resolved{payload(fact, "include.resolved_file_id")};
			if (resolved.valid())
				relation.resolved = resolved;
			relation.source = *fact.source;
			relation.angled = boolean(payload(fact, "include.angled"));
			relation.system = boolean(payload(fact, "include.system"));
			relation.conditional = !payload(fact, "conditional.context").empty();
			relation.used = boolean(payload(fact, "include.used"));
			relation.symbols_using_it = symbol_list(payload(fact, "include.symbols"));
			output.push_back(std::move(relation));
		}
		return output;
	}

	result<std::vector<macro_expansion>> fact_store::macros() const
	{
		if (!data_ || !data_->snapshot)
			return projection_error("unbound-store");
		std::vector<macro_expansion> output;
		for (const auto& fact : data_->snapshot->facts)
		{
			if (fact.kind != fact_kind::macro_expansion)
				continue;
			if (!fact.source)
				return projection_error("macro-expansion-source-missing");
			macro_expansion expansion;
			expansion.name = payload(fact, "macro.name");
			expansion.expansion = *fact.source;
			expansion.function_like = boolean(payload(fact, "macro.function_like"));
			output.push_back(std::move(expansion));
		}
		return output;
	}

	// The frozen public API intentionally takes both immutable filter values by value.
	// NOLINTBEGIN(performance-unnecessary-value-param)
	coverage_report fact_store::coverage(fact_profile, analysis_scope) const
	{
		return data_ && data_->snapshot ? data_->snapshot->coverage : coverage_report{};
	}
	// NOLINTEND(performance-unnecessary-value-param)

	std::string
	fact_store::to_json(const fact_query query) const // NOLINT(performance-unnecessary-value-param)
	{
		auto selected = find(query);
		if (!selected)
			return selected.error().to_json();
		detail::json::json_value::array rows;
		for (const auto& value : selected.value())
			rows.emplace_back(detail::json::json_value::object{
				{"id", std::string{value.id().value()}},
				{"kind", static_cast<std::uint64_t>(value.kind())},
				{"stable_key", std::string{value.stable_key()}},
			});
		auto encoded =
			detail::json::write(detail::json::json_value{detail::json::json_value::object{
				{"schema", "cxxlens.fact-store.v1"}, {"facts", std::move(rows)}}});
		return encoded ? std::move(encoded.value()) : std::string{};
	}
} // namespace cxxlens
