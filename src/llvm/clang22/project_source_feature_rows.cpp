#include "project_source_feature_rows.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <cxxlens/relations/cc_source_feature.hpp>
#include <cxxlens/relations/cc_source_feature_inventory.hpp>

#include "original_source_feature_references.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		namespace r = cxxlens::cc::relations;
		constexpr std::string_view profile = "clang22-original-static-source-features/1";
		using fields = std::map<std::string, sdk::detached_cell, std::less<>>;
		using ids = std::set<std::string, std::less<>>;
		using native_key =
			std::tuple<source_feature_class, const void*, const void*, std::uint32_t>;

		struct writer
		{
			const source_feature_bindings& bindings;
			const source_feature_row_limits& limits;
			project_source_feature_rows out;
			std::map<native_key, std::uint64_t> native_nodes;
			std::map<std::string, ids, std::less<>> file_members;
			ids members, unbound;
			std::optional<sdk::error> failure;
			bool stopped{}, metadata{};
			std::size_t metadata_reserve{}, metadata_work_reserve{};

			bool work(std::size_t operations = 1U)
			{
				if (metadata)
				{
					if (operations > metadata_work_reserve)
					{
						failure = sdk::error{
							"native.source-feature-budget", "inventory", "work-reserve-exhausted"};
						return false;
					}
					metadata_work_reserve -= operations;
					return true;
				}
				if (stopped || operations > limits.traversal.maximum_operations - out.operations)
				{
					stopped = true;
					return false;
				}
				out.operations += operations;
				return true;
			}

			bool charge(std::size_t bytes)
			{
				if (!work())
					return false;
				if (metadata)
				{
					if (bytes > metadata_reserve)
					{
						failure = sdk::error{
							"native.source-feature-budget", "inventory", "reserve-exhausted"};
						return false;
					}
					metadata_reserve -= bytes;
					return true;
				}
				if (stopped || bytes > limits.maximum_retained_bytes - out.retained_bytes_bound)
				{
					stopped = true;
					return false;
				}
				out.retained_bytes_bound += bytes;
				return true;
			}
			bool text(std::string_view value)
			{
				if (value.size() > limits.maximum_binding_bytes)
				{
					stopped = true;
					return false;
				}
				// Owned value, map/frame ownership and canonical output/index copies.
				return work(value.size() * 6U + 1U) && charge(256U + value.size() * 6U);
			}
			bool put(fields& values, std::string_view name, sdk::detached_cell value)
			{
				if (!charge(256U + name.size() * 3U))
					return false;
				values.emplace(std::string{name}, std::move(value));
				return true;
			}
			bool string(fields& values, std::string_view name, std::string_view value)
			{
				return text(value) &&
					put(values, name, sdk::detached_cell::utf8(std::string{value}));
			}
			bool number(fields& values, std::string_view name, std::size_t value)
			{
				return put(values, name, sdk::detached_cell::unsigned_integer(value));
			}
			bool state(fields& values, std::string_view name, std::string_view value)
			{
				// Descriptor rebinding supplies the exact open-symbol type.
				return string(values, name, value);
			}
			bool set(fields& values, std::string_view name, const ids& members_to_encode)
			{
				std::size_t bytes{};
				for (const auto& value : members_to_encode)
				{
					if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
						bytes > std::numeric_limits<std::size_t>::max() - value.size() - 4U)
						return false;
					bytes += value.size() + 4U;
				}
				if (!work(bytes * 4U + 1U) || !charge(bytes * 4U + 256U))
					return false;
				std::vector<std::byte> encoded;
				encoded.reserve(bytes);
				for (const auto& value : members_to_encode)
				{
					const auto size = static_cast<std::uint32_t>(value.size());
					for (unsigned shift{}; shift < 32U; shift += 8U)
						encoded.push_back(static_cast<std::byte>((size >> shift) & 255U));
					for (const auto c : value)
						encoded.push_back(static_cast<std::byte>(c));
				}
				return put(values, name, sdk::detached_cell::bytes(std::move(encoded)));
			}
			std::optional<sdk::detached_row> row(const sdk::relation_descriptor& descriptor,
												 fields values)
			{
				if (!text(descriptor.id) || !charge(sizeof(sdk::detached_row) * 2U))
					return {};
				sdk::detached_row output{descriptor.id, {}};
				for (const auto& column : descriptor.columns)
				{
					if (!text(column.id) || !text(column.type.parameter) || !charge(256U))
						return {};
					auto found = values.find(column.name);
					if (found != values.end())
					{
						found->second.type = column.type;
						output.cells.emplace(column.id, std::move(found->second));
					}
					else if (column.type.optional)
						output.cells.emplace(column.id, sdk::detached_cell::absent(column.type));
				}
				const auto identity = sdk::derive_domain_identity(descriptor, output);
				if (!identity)
				{
					failure = identity.error();
					return {};
				}
				const auto column = descriptor.column(*descriptor.domain_identity.result_column);
				if (!column || !text(*identity))
					return {};
				output.cells.emplace(column->id,
									 sdk::detached_cell::typed(column->type.parameter, *identity));
				const auto valid = sdk::validate_row(descriptor, output);
				if (!valid)
				{
					failure = valid.error();
					return {};
				}
				return output;
			}
			bool binding(fields& values,
						 std::string_view name,
						 std::string_view axis,
						 std::string_view value,
						 std::string_view missing = "unknown")
			{
				return state(values, axis, value.empty() ? missing : "complete") &&
					(value.empty() || string(values, name, value));
			}
			bool observe(const original_source_feature_view& feature)
			{
				if (failure || stopped || !charge(256U))
					return false;
				const auto key = feature.feature_class == source_feature_class::type_location
					? native_key{feature.feature_class,
								 feature.type_location.getOpaqueData(),
								 feature.type_location.getType().getAsOpaquePtr(),
								 feature.compiler_kind}
					: native_key{feature.feature_class,
								 feature.declaration ? static_cast<const void*>(feature.declaration)
									 : feature.statement
									 ? static_cast<const void*>(feature.statement)
									 : static_cast<const void*>(feature.attribute),
								 nullptr,
								 feature.compiler_kind};
				const auto [original, inserted] =
					native_nodes.try_emplace(key, native_nodes.size());
				(void)inserted;
				fields values;
				const auto feature_class =
					feature.feature_class == source_feature_class::declaration ? "declaration"
					: feature.feature_class == source_feature_class::statement ? "statement"
					: feature.feature_class == source_feature_class::attribute ? "attribute"
																			   : "type_location";
				if (!string(values, "compile_unit", bindings.compile_unit) ||
					!string(values, "profile", profile) ||
					!number(values, "ordinal", feature.ordinal) ||
					!number(values, "original_node_ordinal", original->second) ||
					!state(values, "feature_class", feature_class) ||
					!number(values, "compiler_kind", feature.compiler_kind) ||
					!string(values, "kind", feature.compiler_kind_name) ||
					!state(values, "state", "complete") || !state(values, "evaluation", "unknown"))
					return false;
				std::optional<bool> implicit;
				if (feature.declaration)
					implicit = feature.declaration->isImplicit();
				else if (feature.attribute)
					implicit = feature.attribute->isImplicit();
				// These original compiler classes expressly wrap an omitted argument
				// or member initializer. Their invalid source range is a grammar fact;
				// the underlying written expression is still visited independently.
				const bool default_activation =
					llvm::isa_and_nonnull<clang::CXXDefaultArgExpr, clang::CXXDefaultInitExpr>(
						feature.statement);
				// A TypeLoc has no isImplicit flag. Admit this origin only when the
				// exact location is in the implicit declaration's own TypeSourceInfo
				// chain. An arbitrary child of an implicit callable is not excluded.
				bool implicit_type_source_info{};
				if (feature.feature_class == source_feature_class::type_location &&
					feature.declaration_context && feature.declaration_context->isImplicit())
				{
					const clang::TypeSourceInfo* type_source{};
					if (const auto* declaration =
							llvm::dyn_cast<clang::DeclaratorDecl>(feature.declaration_context))
						type_source = declaration->getTypeSourceInfo();
					else if (const auto* typedef_declaration =
								 llvm::dyn_cast<clang::TypedefNameDecl>(
									 feature.declaration_context))
						type_source = typedef_declaration->getTypeSourceInfo();
					if (type_source)
					{
						std::size_t depth{};
						for (auto location = type_source->getTypeLoc(); !location.isNull();
							 location = location.getNextTypeLoc())
						{
							if (++depth > limits.traversal.maximum_depth || !work())
							{
								stopped = true;
								return false;
							}
							if (location.getOpaqueData() == feature.type_location.getOpaqueData() &&
								location.getType() == feature.type_location.getType() &&
								location.getTypeLocClass() ==
									feature.type_location.getTypeLocClass())
							{
								implicit_type_source_info = true;
								break;
							}
						}
					}
				}
				if (!state(values,
						   "origin",
						   default_activation ? "activated"
							   : (implicit && *implicit) || implicit_type_source_info
							   ? "implicit"
							   : "unknown") ||
					(implicit &&
					 !put(values, "is_implicit", sdk::detached_cell::boolean(*implicit))))
					return false;
				std::string_view file;
				bool source_unknown{};
				if (feature.source.isInvalid() &&
					((implicit && *implicit) || default_activation || implicit_type_source_info))
				{
					if (!state(values, "source_binding_state", "none") ||
						!string(values,
								"source_none_reason",
								default_activation
									? "clang22-invalid-range-default-activation-wrapper/1"
									: implicit_type_source_info
									? "clang22-invalid-range-implicit-type-source-info/1"
									: "clang22-invalid-range-implicit-origin/1"))
						return false;
				}
				else
				{
					// The source callback may retain one bounded original span and its
					// existing ID-map strings before returning borrowed views.
					if (!charge(8192U))
						return false;
					const auto source = feature.source.isValid() && bindings.source
						? bindings.source(feature)
						: source_feature_source_binding{};
					const bool complete =
						!source.file.empty() && !source.snapshot.empty() && !source.source.empty();
					source_unknown = !complete;
					if (!state(values, "source_binding_state", complete ? "complete" : "unknown"))
						return false;
					if (complete)
					{
						file = source.file;
						if (!string(values, "file", file) ||
							!string(values, "source_snapshot", source.snapshot) ||
							!string(values, "source", source.source) ||
							(source.is_system &&
							 !put(values,
								  "is_system",
								  sdk::detached_cell::boolean(*source.is_system))))
							return false;
					}
				}
				const auto declaration = feature.declaration && bindings.declaration
					? bindings.declaration(*feature.declaration)
					: std::string_view{};
				const auto context = feature.declaration_context && bindings.declaration
					? bindings.declaration(*feature.declaration_context)
					: std::string_view{};
				if (!binding(values,
							 "declaration",
							 "declaration_binding_state",
							 declaration,
							 feature.declaration ? "unknown" : "none") ||
					!binding(values, "context_declaration", "context_binding_state", context))
					return false;
				const auto refs = observe_original_source_feature_references(feature);
				const auto* subject = llvm::dyn_cast_or_null<clang::NamedDecl>(
					refs.referenced_declaration ? refs.referenced_declaration
												: feature.declaration);
				const auto entity =
					subject && bindings.entity ? bindings.entity(*subject) : std::string_view{};
				const auto call = refs.selected_callable && bindings.entity
					? bindings.entity(*refs.selected_callable)
					: std::string_view{};
				const auto type = !refs.type.isNull() && bindings.canonical_type
					? bindings.canonical_type(refs.type)
					: std::string_view{};
				constexpr std::string_view reference_kinds[]{"unsupported",
															 "declaration_reference",
															 "member_reference",
															 "selected_constructor",
															 "direct_callee",
															 "indirect_call"};
				constexpr std::string_view type_roles[]{
					"unsupported", "declared", "expression", "located"};
				if (!state(values,
						   "reference_kind",
						   reference_kinds[static_cast<unsigned>(refs.reference_kind)]) ||
					!state(
						values, "type_role", type_roles[static_cast<unsigned>(refs.type_role)]) ||
					!binding(values, "subject_entity", "entity_binding_state", entity) ||
					!binding(values,
							 "call_target",
							 "call_binding_state",
							 call,
							 refs.reference_kind == source_feature_reference_kind::indirect_call
								 ? "unknown"
								 : "unsupported") ||
					!binding(values,
							 "subject_type",
							 "type_binding_state",
							 type,
							 refs.type_role == source_feature_type_role::unsupported ? "unsupported"
																					 : "unknown"))
					return false;
				if (feature.statement && bindings.syntax)
					if (const auto syntax = bindings.syntax(*feature.statement);
						!syntax.empty() && !string(values, "syntax", syntax))
						return false;
				auto output = row(r::source_feature::descriptor(), std::move(values));
				if (!output)
					return false;
				const auto& identity =
					std::get<std::string>(*output->cells.at("cc.source_feature.v1.feature").value);
				const auto inventory_bytes = identity.size() * 16U + 512U;
				const auto inventory_work = identity.size() * 16U + 512U;
				if (!charge(1024U + inventory_bytes) || !work(inventory_work))
					return false;
				members.insert(identity);
				if (source_unknown)
					unbound.insert(identity);
				if (!file.empty())
					file_members[std::string{file}].insert(identity);
				// Reserve future inventory set/canonical copies before adding a member.
				metadata_reserve += inventory_bytes;
				metadata_work_reserve += inventory_work;
				out.rows.push_back(std::move(*output));
				return true;
			}
			bool inventory(std::string_view scope,
						   const source_feature_entered_file* file,
						   const ids& scope_members,
						   bool entry_complete,
						   const ids& files,
						   const ids& snapshots)
			{
				fields values;
				const bool complete =
					out.traversal.completed && (scope == "translation_unit" || out.source_complete);
				const ids no_unbound_members;
				const auto& scope_unbound =
					scope == "translation_unit" ? unbound : no_unbound_members;
				if (!string(values, "compile_unit", bindings.compile_unit) ||
					!string(values, "profile", profile) || !state(values, "scope", scope) ||
					!number(values, "feature_count", scope_members.size()) ||
					!set(values, "feature_ids", scope_members) ||
					!set(values, "unbound_feature_ids", scope_unbound) ||
					!number(values, "unbound_feature_count", scope_unbound.size()) ||
					!state(values, "enumeration_state", complete ? "complete" : "partial") ||
					!state(values,
						   "traversal_state",
						   out.traversal.completed ? "complete" : "partial") ||
					!state(values, "entry_state", entry_complete ? "complete" : "partial") ||
					!state(values,
						   "source_binding_state",
						   out.source_complete ? "complete" : "partial"))
					return false;
				if (file)
				{
					if (!string(values, "file", file->file) ||
						!string(values, "source_snapshot", file->snapshot))
						return false;
				}
				else if (!number(values, "entered_file_count", files.size()) ||
						 !set(values, "entered_file_ids", files) ||
						 !set(values, "entered_source_snapshots", snapshots) ||
						 !state(
							 values, "entered_file_state", entry_complete ? "complete" : "partial"))
					return false;
				if (!complete || !entry_complete)
					if (!string(values,
								"reason",
								!out.traversal.completed ? out.traversal.frontier
									: !entry_complete	 ? "original-entered-file-frontier"
														 : "original-source-binding-frontier"))
						return false;
				auto output = row(r::source_feature_inventory::descriptor(), std::move(values));
				if (!output)
					return false;
				out.rows.push_back(std::move(*output));
				return true;
			}
		};
	} // namespace

	sdk::result<project_source_feature_rows>
	detach_original_source_features(clang::ASTContext& context,
									const source_feature_bindings& bindings,
									std::span<const source_feature_entered_file> entered,
									bool entered_files_complete,
									source_feature_row_limits limits)
	{
		writer output{bindings, limits, {}, {}, {}, {}, {}, {}, false, false, 0U, 0U};
		if (bindings.compile_unit.empty() || entered.size() > limits.traversal.maximum_members ||
			entered.size() > std::numeric_limits<std::size_t>::max() / (48U * 1024U) - 1U)
			return sdk::unexpected(
				sdk::error{"native.source-feature-input", "unit", "missing-or-invalid-membership"});
		// Inventory frames survive a traversal resource frontier. Its eventual set
		// payload is additionally precharged with each admitted feature below.
		const auto reserve = (entered.size() + 1U) * 48U * 1024U;
		if (!output.charge(reserve) || !output.work(reserve))
			return sdk::unexpected(
				sdk::error{"native.source-feature-budget", "inventory", "resource-limit"});
		output.metadata_reserve = reserve;
		output.metadata_work_reserve = reserve;
		auto traversal = limits.traversal;
		traversal.try_charge_operation = [](void* raw) noexcept
		{
			auto& current = *static_cast<writer*>(raw);
			if (current.limits.traversal.try_charge_operation &&
				!current.limits.traversal.try_charge_operation(
					current.limits.traversal.operation_context))
				return false;
			return current.work();
		};
		traversal.operation_context = &output;
		output.out.traversal = walk_original_source_features(
			context,
			[](void* raw, const original_source_feature_view& feature) noexcept
			{
				auto& observer = *static_cast<writer*>(raw);
				try
				{
					return observer.observe(feature);
				}
				catch (...)
				{
					observer.failure =
						sdk::error{"native.source-feature-input", "binding", "observer-failed"};
					return false;
				}
			},
			&output,
			traversal);
		if (output.failure)
			return sdk::unexpected(std::move(*output.failure));
		output.out.source_complete = entered_files_complete && output.unbound.empty();
		ids files, snapshots;
		output.metadata = true;
		for (const auto& file : entered)
		{
			if (file.file.empty() || file.snapshot.empty() || !output.text(file.file) ||
				!output.text(file.snapshot) || !files.insert(std::string{file.file}).second ||
				!snapshots.insert(std::string{file.snapshot}).second)
				return sdk::unexpected(sdk::error{"native.source-feature-input",
												  "entered-file",
												  "missing-or-duplicate-membership"});
		}
		for (const auto& [file, members] : output.file_members)
		{
			(void)members;
			if (!files.contains(file))
				output.out.source_complete = false;
		}
		if (!output.inventory("translation_unit",
							  nullptr,
							  output.members,
							  entered_files_complete,
							  files,
							  snapshots))
			return sdk::unexpected(output.failure.value_or(
				sdk::error{"native.source-feature-budget", "inventory", "resource-limit"}));
		for (const auto& file : entered)
			if (!output.inventory("entered_file",
								  &file,
								  output.file_members[std::string{file.file}],
								  true,
								  {},
								  {}))
				return sdk::unexpected(output.failure.value_or(
					sdk::error{"native.source-feature-budget", "inventory", "resource-limit"}));
		return std::move(output.out);
	}
} // namespace cxxlens::detail::clang22
