#include <algorithm>
#include <charconv>
#include <functional>
#include <iomanip>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/relation.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] error
		relation_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool lower_identifier(const std::string_view value)
		{
			if (value.empty() || value.front() < 'a' || value.front() > 'z')
				return false;
			for (const auto byte : value.substr(1U))
			{
				const bool valid =
					(byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
				if (!valid)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool relation_name(const std::string_view value)
		{
			std::size_t begin = 0U;
			bool saw_dot = false;
			while (begin <= value.size())
			{
				const auto end = value.find('.', begin);
				const auto segment = value.substr(
					begin, end == std::string_view::npos ? value.size() - begin : end - begin);
				if (!lower_identifier(segment))
					return false;
				if (end == std::string_view::npos)
					break;
				saw_dot = true;
				begin = end + 1U;
			}
			return saw_dot;
		}

		[[nodiscard]] bool stable_id(const std::string_view value)
		{
			if (value.empty() || value.front() < 'a' || value.front() > 'z')
				return false;
			for (const auto byte : value.substr(1U))
			{
				const bool valid = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
					byte == '_' || byte == '.' || byte == '-';
				if (!valid)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool semantics_id(const std::string_view value)
		{
			const auto slash = value.find('/');
			if (slash == std::string_view::npos || slash < 2U || slash + 1U >= value.size() ||
				value.find('/', slash + 1U) != std::string_view::npos)
				return false;
			if (!stable_id(value.substr(0U, slash)))
				return false;
			return std::ranges::all_of(value.substr(slash + 1U),
									   [](const char byte)
									   {
										   return byte >= '0' && byte <= '9';
									   });
		}

		[[nodiscard]] bool unique_strings(const std::vector<std::string>& values)
		{
			auto ordered = values;
			std::ranges::sort(ordered);
			return std::ranges::adjacent_find(ordered) == ordered.end();
		}

		[[nodiscard]] std::string escape(const std::string_view input)
		{
			return detail::canonical_json_escape(input);
		}

		[[nodiscard]] bool matches_scalar(const scalar_kind kind, const scalar_value& value)
		{
			switch (kind)
			{
				case scalar_kind::boolean:
					return std::holds_alternative<bool>(value);
				case scalar_kind::signed_integer:
					return std::holds_alternative<std::int64_t>(value);
				case scalar_kind::unsigned_integer:
					return std::holds_alternative<std::uint64_t>(value);
				case scalar_kind::bytes:
				case scalar_kind::set:
					return std::holds_alternative<std::vector<std::byte>>(value);
				case scalar_kind::utf8_string:
				case scalar_kind::digest:
				case scalar_kind::semantic_version:
				case scalar_kind::typed_id:
				case scalar_kind::open_symbol:
				case scalar_kind::condition_ref:
				case scalar_kind::source_span_id:
				case scalar_kind::evidence_id:
				case scalar_kind::closed_symbol:
					return std::holds_alternative<std::string>(value);
			}
			return false;
		}

		[[nodiscard]] bool canonical_digest_value(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool canonical_semantic_version(const std::string_view value)
		{
			std::size_t begin{};
			for (std::size_t component{}; component < 3U; ++component)
			{
				const auto end = component == 2U ? value.size() : value.find('.', begin);
				if (end == std::string_view::npos || end == begin ||
					(end - begin > 1U && value[begin] == '0'))
					return false;
				std::uint32_t parsed{};
				const auto result =
					std::from_chars(value.data() + begin, value.data() + end, parsed);
				if (result.ec != std::errc{} || result.ptr != value.data() + end)
					return false;
				begin = end + 1U;
			}
			return begin == value.size() + 1U;
		}

		[[nodiscard]] bool control_free_text(const std::string_view value)
		{
			return !value.empty() && detail::valid_utf8(value) &&
				std::ranges::none_of(value,
									 [](const char byte)
									 {
										 const auto code = static_cast<unsigned char>(byte);
										 return code < 0x20U || code == 0x7fU;
									 });
		}

		[[nodiscard]] const auto& closed_symbol_contracts()
		{
			using symbol_values = std::set<std::string_view, std::less<>>;
			static const std::map<std::string_view, symbol_values, std::less<>> contracts{
				{"claim-stage/1", {"assertion", "canonical_claim", "derived_claim"}},
				{"cc.canonicalization-state/1",
				 {"canonicalized", "provider_local", "ambiguous_equivalence_set"}},
				{"core.claim-conflict-kind/1", {"authoritative_payload_mismatch"}},
			};
			return contracts;
		}

		[[nodiscard]] bool closed_symbol_value(const std::string_view contract,
											   const std::string_view value)
		{
			const auto& contracts = closed_symbol_contracts();
			const auto found = contracts.find(contract);
			return found != contracts.end() && found->second.contains(value);
		}

		[[nodiscard]] bool supported_set_element_type(const std::string_view type)
		{
			if (type == "content_digest" || type == "digest" || type == "assertion_id" ||
				type == "source_span_id" || type == "evidence_id" || type == "condition_ref" ||
				type.ends_with("_id"))
				return true;
			constexpr std::string_view open_prefix{"open_symbol<"};
			constexpr std::string_view closed_prefix{"closed_symbol<"};
			if (type.starts_with(open_prefix) && type.ends_with('>'))
				return semantics_id(
					type.substr(open_prefix.size(), type.size() - open_prefix.size() - 1U));
			if (type.starts_with(closed_prefix) && type.ends_with('>'))
			{
				const auto contract =
					type.substr(closed_prefix.size(), type.size() - closed_prefix.size() - 1U);
				return semantics_id(contract) && closed_symbol_contracts().contains(contract);
			}
			return false;
		}

		[[nodiscard]] bool set_element_value(const std::string_view type,
											 const std::string_view value)
		{
			if (type == "content_digest" || type == "digest")
				return canonical_digest_value(value);
			if (type == "assertion_id" || type == "source_span_id" || type == "evidence_id" ||
				type == "condition_ref" || type.ends_with("_id"))
				return control_free_text(value);
			constexpr std::string_view open_prefix{"open_symbol<"};
			constexpr std::string_view closed_prefix{"closed_symbol<"};
			if (type.starts_with(open_prefix) && type.ends_with('>'))
				return semantics_id(type.substr(open_prefix.size(),
												type.size() - open_prefix.size() - 1U)) &&
					control_free_text(value);
			if (type.starts_with(closed_prefix) && type.ends_with('>'))
			{
				const auto contract =
					type.substr(closed_prefix.size(), type.size() - closed_prefix.size() - 1U);
				return semantics_id(contract) && closed_symbol_value(contract, value);
			}
			return false;
		}

		[[nodiscard]] bool canonical_set_bytes(const std::string_view element_type,
											   const std::vector<std::byte>& encoded)
		{
			if (!supported_set_element_type(element_type))
				return false;
			if (encoded.empty())
				return true;
			std::size_t offset{};
			std::string previous;
			while (offset < encoded.size())
			{
				if (encoded.size() - offset < sizeof(std::uint32_t))
					return false;
				std::uint32_t length{};
				for (std::size_t byte{}; byte < sizeof(length); ++byte)
					length |= std::to_integer<std::uint32_t>(encoded[offset + byte]) << (byte * 8U);
				offset += sizeof(length);
				if (length == 0U || length > encoded.size() - offset)
					return false;
				std::string value;
				value.reserve(length);
				for (std::size_t byte{}; byte < length; ++byte)
					value.push_back(static_cast<char>(encoded[offset + byte]));
				offset += length;
				if (!set_element_value(element_type, value) ||
					(!previous.empty() && previous >= value))
					return false;
				previous = std::move(value);
			}
			return true;
		}

		[[nodiscard]] bool valid_scalar_type(const value_type& type)
		{
			switch (type.scalar)
			{
				case scalar_kind::typed_id:
					return lower_identifier(type.parameter) && type.parameter.ends_with("_id");
				case scalar_kind::open_symbol:
					return semantics_id(type.parameter);
				case scalar_kind::closed_symbol:
					return semantics_id(type.parameter) &&
						closed_symbol_contracts().contains(type.parameter);
				case scalar_kind::set:
					return supported_set_element_type(type.parameter);
				case scalar_kind::boolean:
				case scalar_kind::signed_integer:
				case scalar_kind::unsigned_integer:
				case scalar_kind::utf8_string:
				case scalar_kind::bytes:
				case scalar_kind::digest:
				case scalar_kind::semantic_version:
				case scalar_kind::condition_ref:
				case scalar_kind::source_span_id:
				case scalar_kind::evidence_id:
					return type.parameter.empty();
			}
			return false;
		}

		[[nodiscard]] std::optional<std::string_view>
		invalid_scalar_detail(const value_type& type, const scalar_value& value)
		{
			if (!valid_scalar_type(type))
				return "type-parameter";
			if (!matches_scalar(type.scalar, value))
				return "type-shape";
			if (const auto* text = std::get_if<std::string>(&value);
				text != nullptr && !detail::valid_utf8(*text))
				return "invalid-utf8";
			const auto* text = std::get_if<std::string>(&value);
			switch (type.scalar)
			{
				case scalar_kind::digest:
					return type.parameter.empty() && canonical_digest_value(*text)
						? std::nullopt
						: std::optional<std::string_view>{"digest"};
				case scalar_kind::semantic_version:
					return type.parameter.empty() && canonical_semantic_version(*text)
						? std::nullopt
						: std::optional<std::string_view>{"semantic-version"};
				case scalar_kind::typed_id:
					if (!lower_identifier(type.parameter) || !type.parameter.ends_with("_id"))
						return "typed-id-parameter";
					return control_free_text(*text) ? std::nullopt
													: std::optional<std::string_view>{"identity"};
				case scalar_kind::condition_ref:
				case scalar_kind::source_span_id:
				case scalar_kind::evidence_id:
					return type.parameter.empty() && control_free_text(*text)
						? std::nullopt
						: std::optional<std::string_view>{"identity"};
				case scalar_kind::open_symbol:
					return semantics_id(type.parameter) && control_free_text(*text)
						? std::nullopt
						: std::optional<std::string_view>{"open-symbol"};
				case scalar_kind::closed_symbol:
					return semantics_id(type.parameter) &&
							closed_symbol_value(type.parameter, *text)
						? std::nullopt
						: std::optional<std::string_view>{"closed-symbol"};
				case scalar_kind::set:
					return !type.parameter.empty() &&
							canonical_set_bytes(type.parameter,
												std::get<std::vector<std::byte>>(value))
						? std::nullopt
						: std::optional<std::string_view>{"set-encoding"};
				case scalar_kind::boolean:
				case scalar_kind::signed_integer:
				case scalar_kind::unsigned_integer:
				case scalar_kind::utf8_string:
				case scalar_kind::bytes:
					return type.parameter.empty()
						? std::nullopt
						: std::optional<std::string_view>{"unexpected-parameter"};
			}
			return "scalar-kind";
		}

		[[nodiscard]] std::string scalar_text(const scalar_value& value)
		{
			return std::visit(
				[](const auto& item) -> std::string
				{
					using value_type = std::remove_cvref_t<decltype(item)>;
					if constexpr (std::same_as<value_type, bool>)
						return item ? "true" : "false";
					else if constexpr (std::integral<value_type>)
						return std::to_string(item);
					else if constexpr (std::same_as<value_type, std::string>)
						return "\"" + escape(item) + "\"";
					else
					{
						std::ostringstream output;
						output << '"' << std::hex << std::setfill('0');
						for (const auto byte : item)
							output << std::setw(2) << std::to_integer<unsigned int>(byte);
						output << '"';
						return output.str();
					}
				},
				value);
		}

		[[nodiscard]] std::string_view role_name(const column_role role)
		{
			switch (role)
			{
				case column_role::claim_key:
					return "claim_key";
				case column_role::authoritative_payload:
					return "authoritative_payload";
				case column_role::display:
					return "display";
				case column_role::auxiliary:
					return "auxiliary";
			}
			return "invalid";
		}

		[[nodiscard]] std::string_view merge_name(const merge_mode mode)
		{
			switch (mode)
			{
				case merge_mode::set:
					return "set";
				case merge_mode::multiset:
					return "multiset";
				case merge_mode::functional_assertion:
					return "functional_assertion";
				case merge_mode::keyed_union:
					return "keyed_union";
			}
			return "invalid";
		}

		[[nodiscard]] std::string_view reference_name(const reference_strength strength)
		{
			return strength == reference_strength::hard ? "hard" : "soft_semantic";
		}

		[[nodiscard]] result<std::string> descriptor_binding(const relation_descriptor& descriptor)
		{
			return semantic_digest("cxxlens.relation-descriptor-binding.v2",
								   descriptor.contract_digest + "\n" + descriptor.canonical_form());
		}

		[[nodiscard]] result<canonical_value> identity_value(const detached_cell& cell,
															 const std::string_view column)
		{
			if (cell.state == cell_state::absent)
				return canonical_value::null();
			if (cell.state != cell_state::present || !cell.value)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.domain-identity-unresolved", std::string{column}));
			return std::visit(
				[&](const auto& value) -> result<canonical_value>
				{
					using type = std::remove_cvref_t<decltype(value)>;
					if constexpr (std::same_as<type, bool>)
						return canonical_value::from_boolean(value);
					else if constexpr (std::same_as<type, std::int64_t>)
						return canonical_value::from_integer(value);
					else if constexpr (std::same_as<type, std::uint64_t>)
					{
						if (value >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
							return cxxlens::sdk::unexpected(relation_error(
								"sdk.domain-identity-out-of-range", std::string{column}));
						return canonical_value::from_integer(static_cast<std::int64_t>(value));
					}
					else if constexpr (std::same_as<type, std::string>)
						return canonical_value::from_string(value);
					else
						return canonical_value::from_bytes(value);
				},
				*cell.value);
		}

		[[nodiscard]] result<std::string> identity_kind(const relation_descriptor& descriptor)
		{
			if (!descriptor.domain_identity.result_column)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.domain-identity-unavailable", descriptor.id));
			auto result_column = descriptor.column(*descriptor.domain_identity.result_column);
			if (!result_column || result_column->type.scalar != scalar_kind::typed_id ||
				!result_column->type.parameter.ends_with("_id"))
				return cxxlens::sdk::unexpected(relation_error(
					"sdk.domain-identity-invalid", *descriptor.domain_identity.result_column));
			std::string output = result_column->type.parameter.substr(
				0U, result_column->type.parameter.size() - std::string_view{"_id"}.size());
			std::ranges::replace(output, '_', '-');
			return output;
		}
	} // namespace

	std::string value_type::canonical_name() const
	{
		static constexpr std::array<std::string_view, 14U> names{
			"bool",
			"int64",
			"uint64",
			"utf8_string",
			"bytes",
			"digest",
			"semantic_version",
			"typed_id",
			"open_symbol",
			"condition_ref",
			"source_span_id",
			"evidence_id",
			"closed_symbol",
			"set",
		};
		std::string output{names.at(static_cast<std::size_t>(scalar))};
		if (!parameter.empty())
			output += "<" + parameter + ">";
		if (optional)
			output = "optional<" + output + ">";
		return output;
	}

	result<void> relation_descriptor::validate() const
	{
		if (contract_canonical.empty() != contract_digest.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.descriptor-digest-mismatch", "contract_digest"));
		if (!contract_canonical.empty())
		{
			if (contract_digest != content_digest(std::as_bytes(std::span{contract_canonical})))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.descriptor-digest-mismatch", "contract_digest"));
			auto expected = descriptor_binding(*this);
			if (!expected || descriptor_digest != *expected)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.descriptor-digest-mismatch", "descriptor_digest"));
		}
		if (!relation_name(name))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "name", "relation-name-pattern"));
		if (semantic_major == 0U)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "semantic_major", "minimum-1"));
		if (id != name + ".v" + std::to_string(semantic_major))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "id", "semantic-major"));
		if (version.major != semantic_major)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "version", "major"));
		if (columns.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "columns", "empty"));
		if (!semantics_id(semantics))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "semantics", "semantics-pattern"));
		if (!stable_id(owner_namespace))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "owner_namespace", "namespace-pattern"));
		if (key_columns.empty() || !unique_strings(key_columns))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "key_columns", "unique-nonempty"));
		if (domain_identity.contract.empty() != domain_identity.projection.empty() ||
			(domain_identity.contract.empty() && domain_identity.result_column))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.domain-identity-invalid", id, "shape"));
		if (!domain_identity.contract.empty())
		{
			if (domain_identity.contract != "canonical-binary-tuple-v1" ||
				!unique_strings(domain_identity.projection))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.domain-identity-invalid", id, "contract"));
			for (const auto& projection : domain_identity.projection)
				if (!column(projection))
					return cxxlens::sdk::unexpected(
						relation_error("sdk.domain-identity-invalid", projection, "projection"));
			if (domain_identity.result_column)
			{
				auto result = column(*domain_identity.result_column);
				if (!result || result->role != column_role::claim_key ||
					std::ranges::find(domain_identity.projection, *domain_identity.result_column) !=
						domain_identity.projection.end() ||
					!identity_kind(*this))
					return cxxlens::sdk::unexpected(relation_error(
						"sdk.domain-identity-invalid", *domain_identity.result_column, "result"));
			}
		}
		std::vector<std::string> ids;
		std::vector<std::string> names;
		for (const auto& value : columns)
		{
			if (!stable_id(value.id) || !value.id.starts_with(id + "."))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.id, "identity"));
			if (!lower_identifier(value.name))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.name, "name-pattern"));
			if (!value.required && !value.type.optional)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.id, "optional-contract"));
			if (value.type.parameter == "native_pointer" ||
				value.type.parameter == "native_address")
				return cxxlens::sdk::unexpected(
					relation_error("sdk.native-address-payload", value.id, value.type.parameter));
			if (!valid_scalar_type(value.type))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.id, "scalar-type"));
			ids.push_back(value.id);
			names.push_back(value.name);
		}
		std::ranges::sort(ids);
		std::ranges::sort(names);
		if (std::ranges::adjacent_find(ids) != ids.end() ||
			std::ranges::adjacent_find(names) != names.end())
			return cxxlens::sdk::unexpected(relation_error("sdk.duplicate-column", "columns"));
		std::vector<std::string> role_keys;
		std::vector<std::string> authoritative_payload;
		for (const auto& value : columns)
		{
			if (value.role == column_role::claim_key)
				role_keys.push_back(value.id);
			if (value.role == column_role::authoritative_payload)
				authoritative_payload.push_back(value.id);
		}
		for (const auto& key : key_columns)
		{
			if (!stable_id(key))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.relation-invalid", key, "claim-key-pattern"));
			auto found = column(key);
			if (!found || found->role != column_role::claim_key || !found->required)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.relation-invalid", key, "claim-key"));
		}
		std::ranges::sort(role_keys);
		auto ordered_keys = key_columns;
		std::ranges::sort(ordered_keys);
		if (role_keys != ordered_keys)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "key_columns", "claim-key-role-parity"));
		if (!unique_strings(conflict_columns))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "conflict_columns", "unique"));
		for (const auto& conflict : conflict_columns)
			if (!stable_id(conflict) || !column(conflict))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.relation-invalid", conflict, "conflict-column"));
		std::ranges::sort(authoritative_payload);
		auto ordered_conflicts = conflict_columns;
		std::ranges::sort(ordered_conflicts);
		if (merge == merge_mode::functional_assertion && authoritative_payload != ordered_conflicts)
			return cxxlens::sdk::unexpected(relation_error(
				"sdk.relation-invalid", "conflict_columns", "functional-payload-projection"));
		if (merge != merge_mode::functional_assertion && !conflict_columns.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "conflict_columns", "nonfunctional-empty"));
		for (const auto& reference : references)
		{
			if (reference.source_columns.empty() ||
				reference.source_columns.size() != reference.target_columns.size() ||
				!relation_name(reference.target_relation))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.reference-invalid", name, "shape"));
			if (!unique_strings(reference.source_columns))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.reference-invalid", name, "source-columns-unique"));
			if (!unique_strings(reference.target_columns))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.reference-invalid", name, "target-columns-unique"));
			for (const auto& source : reference.source_columns)
				if (!stable_id(source) || !column(source))
					return cxxlens::sdk::unexpected(
						relation_error("sdk.reference-invalid", source, "source-column"));
			if (std::ranges::any_of(reference.target_columns,
									[](const std::string& target)
									{
										return !stable_id(target);
									}))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.reference-invalid", name, "target-column-pattern"));
		}
		if (contract_canonical.empty())
		{
			auto expected = descriptor_binding(*this);
			if (!expected || descriptor_digest != *expected)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.descriptor-digest-mismatch", "descriptor_digest"));
		}
		return {};
	}

	result<column_descriptor> relation_descriptor::column(const std::string_view name_or_id) const
	{
		const auto found =
			std::ranges::find_if(columns,
								 [&](const column_descriptor& value)
								 {
									 return value.name == name_or_id || value.id == name_or_id;
								 });
		if (found == columns.end())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.column-not-found", std::string{name_or_id}, id));
		return *found;
	}

	std::string relation_descriptor::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"columns\":[";
		auto ordered = columns;
		std::ranges::sort(ordered, {}, &column_descriptor::id);
		for (std::size_t index = 0U; index < ordered.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& item = ordered[index];
			output << R"({"id":")" << escape(item.id) << R"(","name":")" << escape(item.name)
				   << R"(","required":)" << (item.required ? "true" : "false") << R"(,"type":")"
				   << escape(item.type.canonical_name()) << R"(","role":")" << role_name(item.role)
				   << "\"}";
		}
		output << R"(],"conflict_columns":[)";
		for (std::size_t index = 0U; index < conflict_columns.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << '"' << escape(conflict_columns[index]) << '"';
		}
		output << R"(],"domain_identity":{"contract":")" << escape(domain_identity.contract)
			   << R"(","projection":[)";
		for (std::size_t index = 0U; index < domain_identity.projection.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << '"' << escape(domain_identity.projection[index]) << '"';
		}
		output << R"(],"result_column":)";
		if (domain_identity.result_column)
			output << '"' << escape(*domain_identity.result_column) << '"';
		else
			output << "null";
		output << R"(},"id":")" << escape(id) << R"(","key_columns":[)";
		for (std::size_t index = 0U; index < key_columns.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << '"' << escape(key_columns[index]) << '"';
		}
		output << R"(],"merge":")" << merge_name(merge) << R"(","name":")" << escape(name)
			   << R"(","owner_namespace":")" << escape(owner_namespace) << R"(","references":[)";
		auto ordered_references = references;
		std::ranges::sort(ordered_references,
						  [](const auto& left, const auto& right)
						  {
							  return std::tie(left.target_relation, left.source_columns) <
								  std::tie(right.target_relation, right.source_columns);
						  });
		for (std::size_t index = 0U; index < ordered_references.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& reference = ordered_references[index];
			output << R"({"source":[)";
			for (std::size_t column = 0U; column < reference.source_columns.size(); ++column)
			{
				if (column != 0U)
					output << ',';
				output << '"' << escape(reference.source_columns[column]) << '"';
			}
			output << R"(],"strength":")" << reference_name(reference.strength)
				   << R"(","target_relation":")" << escape(reference.target_relation)
				   << R"(","target":[)";
			for (std::size_t column = 0U; column < reference.target_columns.size(); ++column)
			{
				if (column != 0U)
					output << ',';
				output << '"' << escape(reference.target_columns[column]) << '"';
			}
			output << "]}";
		}
		output << R"(],"semantic_major":)" << semantic_major << R"(,"semantics":")"
			   << escape(semantics) << R"(","version":")" << version.string() << "\"}";
		return output.str();
	}

	dynamic_relation::dynamic_relation(std::shared_ptr<const relation_descriptor> descriptor)
		: descriptor_{std::move(descriptor)}
	{
	}

	const relation_descriptor& dynamic_relation::descriptor() const noexcept
	{
		return *descriptor_;
	}

	result<column_ref> dynamic_relation::column(const std::string_view name_or_id) const
	{
		auto found = descriptor_->column(name_or_id);
		if (!found)
			return cxxlens::sdk::unexpected(std::move(found.error()));
		return column_ref{descriptor_->id, found->id, found->type};
	}

	result<void> relation_registry::add(relation_descriptor descriptor)
	{
		if (!frozen_ || *frozen_)
			return cxxlens::sdk::unexpected(relation_error("sdk.registry-frozen", descriptor.name));
		if (descriptor.descriptor_digest.empty())
		{
			if (!descriptor.contract_canonical.empty())
				return cxxlens::sdk::unexpected(
					relation_error("sdk.descriptor-digest-mismatch", "descriptor_digest"));
			descriptor.descriptor_digest = *descriptor_binding(descriptor);
		}
		if (auto valid = descriptor.validate(); !valid)
			return valid;
		if (const auto found = descriptors_.find(descriptor.name); found != descriptors_.end())
		{
			if (*found->second == descriptor)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.duplicate-descriptor", descriptor.name));
			return cxxlens::sdk::unexpected(
				relation_error("sdk.descriptor-conflict", descriptor.name, found->second->id));
		}
		const auto name = descriptor.name;
		descriptors_.emplace(name,
							 std::make_shared<const relation_descriptor>(std::move(descriptor)));
		return {};
	}

	result<dynamic_relation> relation_registry::require(const std::string_view name,
														const std::uint32_t semantic_major) const
	{
		const auto found = descriptors_.find(name);
		if (found == descriptors_.end())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-not-found", std::string{name}));
		if (found->second->semantic_major != semantic_major)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-major-mismatch",
							   std::string{name},
							   std::to_string(found->second->semantic_major)));
		return dynamic_relation{found->second};
	}

	std::vector<relation_descriptor> relation_registry::descriptors() const
	{
		std::vector<relation_descriptor> output;
		output.reserve(descriptors_.size());
		for (const auto& [name, descriptor] : descriptors_)
		{
			(void)name;
			output.push_back(*descriptor);
		}
		return output;
	}

	result<relation_engine> relation_registry::build(std::string generation)
	{
		if (!frozen_ || *frozen_)
			return cxxlens::sdk::unexpected(relation_error("sdk.registry-frozen", "registry"));
		if (generation.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.engine-generation-invalid", "generation"));
		if (descriptors_.empty())
			return cxxlens::sdk::unexpected(relation_error("sdk.registry-empty", "registry"));
		for (const auto& [name, descriptor] : descriptors_)
			for (const auto& reference : descriptor->references)
			{
				const auto target = descriptors_.find(reference.target_relation);
				if (target == descriptors_.end())
				{
					if (reference.strength == reference_strength::hard)
						return cxxlens::sdk::unexpected(relation_error(
							"sdk.registry-reference-missing", name, reference.target_relation));
					continue;
				}
				for (std::size_t index = 0U; index < reference.source_columns.size(); ++index)
				{
					auto source_column = descriptor->column(reference.source_columns[index]);
					auto target_column = target->second->column(reference.target_columns[index]);
					if (!source_column || !target_column ||
						source_column->type.scalar != target_column->type.scalar ||
						source_column->type.parameter != target_column->type.parameter)
						return cxxlens::sdk::unexpected(relation_error(
							"sdk.reference-invalid", name, reference.target_relation));
				}
			}
		enum class visit_state : std::uint8_t
		{
			unvisited,
			visiting,
			visited,
		};
		std::map<std::string, visit_state, std::less<>> states;
		for (const auto& [name, descriptor] : descriptors_)
		{
			(void)descriptor;
			states.emplace(name, visit_state::unvisited);
		}
		std::function<result<void>(const std::string&)> visit = [&](const std::string& name)
		{
			if (states[name] == visit_state::visiting)
				return result<void>{relation_error("sdk.hard-reference-cycle", name)};
			if (states[name] == visit_state::visited)
				return result<void>{};
			states[name] = visit_state::visiting;
			for (const auto& reference : descriptors_.at(name)->references)
				if (reference.strength == reference_strength::hard &&
					descriptors_.contains(reference.target_relation))
					if (auto valid = visit(reference.target_relation); !valid)
						return valid;
			states[name] = visit_state::visited;
			return result<void>{};
		};
		for (const auto& [name, descriptor] : descriptors_)
		{
			(void)descriptor;
			if (auto valid = visit(name); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
		}
		std::string canonical;
		for (const auto& [name, descriptor] : descriptors_)
		{
			canonical += name;
			canonical += '=';
			canonical += descriptor->descriptor_digest;
			canonical.push_back('\n');
		}
		*frozen_ = true;
		return relation_engine{descriptors_,
							   *semantic_digest("cxxlens.relation-registry.v1", canonical),
							   std::move(generation)};
	}

	bool relation_registry::frozen() const noexcept
	{
		return !frozen_ || *frozen_;
	}

	relation_engine::relation_engine(
		std::map<std::string, std::shared_ptr<const relation_descriptor>, std::less<>> descriptors,
		std::string digest,
		std::string generation)
		: descriptors_{std::move(descriptors)}, registry_digest_{std::move(digest)},
		  generation_{std::move(generation)}
	{
	}

	result<dynamic_relation> relation_engine::require(const std::string_view name,
													  const std::uint32_t semantic_major) const
	{
		const auto found = descriptors_.find(name);
		if (found == descriptors_.end())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-not-found", std::string{name}));
		if (found->second->semantic_major != semantic_major)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-major-mismatch", std::string{name}));
		return dynamic_relation{found->second};
	}

	result<dynamic_relation> relation_engine::require_id(const std::string_view descriptor_id) const
	{
		for (const auto& [name, descriptor] : descriptors_)
		{
			(void)name;
			if (descriptor->id == descriptor_id)
				return dynamic_relation{descriptor};
		}
		return cxxlens::sdk::unexpected(
			relation_error("sdk.relation-not-found", std::string{descriptor_id}));
	}

	std::vector<relation_descriptor> relation_engine::descriptors() const
	{
		std::vector<relation_descriptor> output;
		output.reserve(descriptors_.size());
		for (const auto& [name, descriptor] : descriptors_)
		{
			(void)name;
			output.push_back(*descriptor);
		}
		return output;
	}

	std::string_view relation_engine::registry_digest() const noexcept
	{
		return registry_digest_;
	}

	std::string_view relation_engine::generation() const noexcept
	{
		return generation_;
	}

	detached_cell detached_cell::boolean(const bool value)
	{
		return {{scalar_kind::boolean, {}, false}, cell_state::present, scalar_value{value}, {}};
	}
	detached_cell detached_cell::signed_integer(const std::int64_t value)
	{
		return {
			{scalar_kind::signed_integer, {}, false}, cell_state::present, scalar_value{value}, {}};
	}
	detached_cell detached_cell::unsigned_integer(const std::uint64_t value)
	{
		return {{scalar_kind::unsigned_integer, {}, false},
				cell_state::present,
				scalar_value{value},
				{}};
	}
	detached_cell detached_cell::utf8(std::string value)
	{
		return {{scalar_kind::utf8_string, {}, false},
				cell_state::present,
				scalar_value{std::move(value)},
				{}};
	}
	detached_cell detached_cell::bytes(std::vector<std::byte> value)
	{
		return {{scalar_kind::bytes, {}, false},
				cell_state::present,
				scalar_value{std::move(value)},
				{}};
	}
	detached_cell detached_cell::typed(std::string type, std::string value)
	{
		return {{scalar_kind::typed_id, std::move(type), false},
				cell_state::present,
				scalar_value{std::move(value)},
				{}};
	}
	detached_cell detached_cell::absent(value_type type)
	{
		type.optional = true;
		return {std::move(type), cell_state::absent, std::nullopt, std::nullopt};
	}
	detached_cell detached_cell::unknown(value_type type, std::string reason)
	{
		return {std::move(type), cell_state::unknown, std::nullopt, std::move(reason)};
	}

	result<void> detached_cell::validate() const
	{
		if (!valid_scalar_type(type))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.cell-invalid", "type", "type-parameter"));
		if (state == cell_state::present)
		{
			if (!value || unknown_reason)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-invalid", "value", "present"));
			if (const auto detail = invalid_scalar_detail(type, *value))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-invalid", "value", std::string{*detail}));
		}
		else if (state == cell_state::absent)
		{
			if (!type.optional || value || unknown_reason)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-invalid", "state", "absent"));
		}
		else if (value || !unknown_reason || unknown_reason->empty())
			return cxxlens::sdk::unexpected(relation_error("sdk.cell-invalid", "state", "unknown"));
		else if (!detail::valid_utf8(*unknown_reason))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.cell-invalid", "unknown_reason", "invalid-utf8"));
		else if (!control_free_text(*unknown_reason))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.cell-invalid", "unknown_reason", "control-character"));
		return {};
	}

	std::string detached_cell::canonical_form() const
	{
		std::ostringstream output;
		output << R"({"state":")";
		switch (state)
		{
			case cell_state::present:
				output << "present";
				break;
			case cell_state::absent:
				output << "absent";
				break;
			case cell_state::unknown:
				output << "unknown";
				break;
		}
		output << R"(","type":")" << escape(type.canonical_name()) << '"';
		if (value)
			output << ",\"value\":" << scalar_text(*value);
		if (unknown_reason)
			output << R"(,"unknown_reason":")" << escape(*unknown_reason) << '"';
		output << '}';
		return output.str();
	}

	std::string detached_row::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"cells\":{";
		std::size_t index = 0U;
		for (const auto& [id, cell] : cells)
		{
			if (index++ != 0U)
				output << ',';
			output << '"' << escape(id) << "\":" << cell.canonical_form();
		}
		output << R"(},"descriptor_id":")" << escape(descriptor_id) << "\"}";
		return output.str();
	}

	row_builder::row_builder(relation_descriptor descriptor) : descriptor_{std::move(descriptor)}
	{
		row_.descriptor_id = descriptor_.id;
	}

	result<void> row_builder::set(column_ref column, detached_cell value)
	{
		if (column.descriptor_id != descriptor_.id)
			return cxxlens::sdk::unexpected(relation_error("sdk.foreign-column", column.column_id));
		auto expected = descriptor_.column(column.column_id);
		if (!expected)
			return cxxlens::sdk::unexpected(std::move(expected.error()));
		if (!(column.type == expected->type) || !(value.type == expected->type))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.cell-type-mismatch", column.column_id));
		if (auto valid = value.validate(); !valid)
			return valid;
		if (!row_.cells.emplace(column.column_id, std::move(value)).second)
			return cxxlens::sdk::unexpected(relation_error("sdk.duplicate-cell", column.column_id));
		return {};
	}

	result<detached_row> row_builder::finish() &&
	{
		if (auto valid = validate_row(descriptor_, row_); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		return std::move(row_);
	}

	result<void> validate_row(const relation_descriptor& descriptor, const detached_row& row)
	{
		if (row.descriptor_id != descriptor.id)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.row-descriptor-mismatch", "descriptor_id"));
		for (const auto& column : descriptor.columns)
		{
			const auto found = row.cells.find(column.id);
			if (found == row.cells.end())
			{
				if (column.required)
					return cxxlens::sdk::unexpected(
						relation_error("sdk.required-cell-missing", column.id));
				continue;
			}
			if (!(found->second.type == column.type))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-type-mismatch", column.id));
			if (auto valid = found->second.validate(); !valid)
				return valid;
		}
		for (const auto& [id, cell] : row.cells)
		{
			(void)cell;
			if (!descriptor.column(id))
				return cxxlens::sdk::unexpected(relation_error("sdk.unknown-cell", id));
		}
		return {};
	}

	result<std::string> derive_domain_identity(const relation_descriptor& descriptor,
											   const detached_row& row)
	{
		if (row.descriptor_id != descriptor.id ||
			descriptor.domain_identity.contract != "canonical-binary-tuple-v1")
			return cxxlens::sdk::unexpected(
				relation_error("sdk.domain-identity-invalid", descriptor.id, "authority"));
		auto kind = identity_kind(descriptor);
		if (!kind)
			return cxxlens::sdk::unexpected(std::move(kind.error()));
		std::vector<canonical_value> projection;
		projection.reserve(descriptor.domain_identity.projection.size());
		for (const auto& column_id : descriptor.domain_identity.projection)
		{
			auto column = descriptor.column(column_id);
			if (!column)
				return cxxlens::sdk::unexpected(std::move(column.error()));
			const auto found = row.cells.find(column_id);
			if (found == row.cells.end())
			{
				if (!column->type.optional)
					return cxxlens::sdk::unexpected(
						relation_error("sdk.domain-identity-missing", column_id));
				projection.push_back(canonical_value::null());
				continue;
			}
			if (!(found->second.type == column->type))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-type-mismatch", column_id));
			if (auto valid = found->second.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			auto value = identity_value(found->second, column_id);
			if (!value)
				return cxxlens::sdk::unexpected(std::move(value.error()));
			projection.push_back(std::move(*value));
		}
		return canonical_identity_digest(*kind, projection);
	}

	result<void> validate_domain_identity(const relation_descriptor& descriptor,
										  const detached_row& row)
	{
		auto expected = derive_domain_identity(descriptor, row);
		if (!expected)
			return cxxlens::sdk::unexpected(std::move(expected.error()));
		const auto& result_column = *descriptor.domain_identity.result_column;
		const auto found = row.cells.find(result_column);
		if (found == row.cells.end() || found->second.state != cell_state::present ||
			!found->second.value)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.domain-identity-missing", result_column));
		const auto* actual = std::get_if<std::string>(&*found->second.value);
		if (actual == nullptr || *actual != *expected)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.domain-identity-mismatch", result_column));
		return {};
	}

	result<void> project_catalog::validate() const
	{
		if (catalog_id.empty() || catalog_digest.empty() || logical_root.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.project-catalog-invalid", "identity"));
		if (!std::ranges::is_sorted(compile_units) ||
			std::ranges::adjacent_find(compile_units) != compile_units.end())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.project-catalog-invalid", "compile_units", "canonical-order"));
		return {};
	}

} // namespace cxxlens::sdk
