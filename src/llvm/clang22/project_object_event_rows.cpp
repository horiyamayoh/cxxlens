#include "project_object_event_rows.hpp"

#include <array>
#include <limits>

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <cxxlens/relations/cc_declaration_inventory.hpp>
#include <cxxlens/relations/cc_object_state_observation.hpp>
#include <cxxlens/relations/cc_sequence_context.hpp>
#include <cxxlens/relations/cc_sequence_pair.hpp>

#include "ByteCode/State.h"
namespace cxxlens::detail::clang22::object_semantics
{
	namespace
	{
		namespace r = cxxlens::cc::relations;
		using sdk::detached_cell;
		constexpr std::string_view sequence_profile =
			"clang22-original-sequence-checker-candidates/1";
		constexpr std::string_view context_profile =
			"clang22-original-argument-sequencing-context/1";
		constexpr std::string_view object_profile = "clang22-legacy-interpreter-object-state/1";
		struct writer
		{
			object_facet_limits limits;
			const object_event_bindings& bindings;
			project_object_event_rows out;
			std::optional<sdk::error> failure;
			std::size_t fields{};
			bool charge(std::size_t work = 1U, std::size_t bytes = 0U)
			{
				if (failure)
					return false;
				if (limits.cancelled && limits.cancelled())
				{
					failure = sdk::error{"native.object-cancelled", "rows", "cancelled"};
					return false;
				}
				if (out.operations > limits.maximum_operations ||
					work > limits.maximum_operations - out.operations ||
					out.retained_bytes_bound > limits.maximum_retained_bytes ||
					bytes > limits.maximum_retained_bytes - out.retained_bytes_bound)
				{
					failure = sdk::error{"native.object-budget", "rows", "resource-limit"};
					return false;
				}
				out.operations += work;
				out.retained_bytes_bound += bytes;
				return true;
			}
			bool text(std::string_view s)
			{
				if (s.size() > limits.maximum_text_bytes)
				{
					failure = sdk::error{"native.object-budget", "text", "resource-limit"};
					return false;
				}
				return charge(s.size() + 1U, s.size() + sizeof(std::string) * 2U);
			}
			sdk::detached_row* row(const sdk::relation_descriptor& descriptor)
			{
				if (out.rows.size() >= limits.maximum_fields || !text(descriptor.id) ||
					!charge(1U, sizeof(sdk::detached_row) * 2U))
				{
					if (!failure)
						failure = sdk::error{"native.object-budget", "members", "resource-limit"};
					return nullptr;
				}
				sdk::detached_row result{descriptor.id, {}};
				for (const auto& column : descriptor.columns)
				{
					if (fields >= limits.maximum_fields || !text(column.id) || !charge(1U, 256U))
					{
						if (!failure)
							failure =
								sdk::error{"native.object-budget", "fields", "resource-limit"};
						return nullptr;
					}
					++fields;
					result.cells.emplace(
						column.id,
						column.type.optional
							? detached_cell::absent(column.type)
							: detached_cell::unknown(column.type, "original-fact-unavailable"));
				}
				out.rows.push_back(std::move(result));
				return &out.rows.back();
			}
			const sdk::column_descriptor* column(const sdk::relation_descriptor& d,
												 std::string_view name)
			{
				if (!charge(d.columns.size() + 1U))
					return nullptr;
				auto at = std::ranges::find(d.columns, name, &sdk::column_descriptor::name);
				if (at == d.columns.end())
				{
					failure = sdk::error{
						"native.object-input", std::string{name}, "descriptor-column-missing"};
					return nullptr;
				}
				return &*at;
			}
			void put(const sdk::relation_descriptor& d,
					 sdk::detached_row* row,
					 std::string_view name,
					 detached_cell value)
			{
				if (!row || failure)
					return;
				const auto* c = column(d, name);
				if (!c)
					return;
				if (!charge(1U, 256U))
					return;
				value.type = c->type;
				row->cells.insert_or_assign(c->id, std::move(value));
			}
			void string(const sdk::relation_descriptor& d,
						sdk::detached_row* row,
						std::string_view name,
						std::string_view value)
			{
				if (!row || failure)
					return;
				const auto* c = column(d, name);
				if (!c)
					return;
				if (value.empty())
				{
					put(d,
						row,
						name,
						detached_cell::unknown(c->type, "original-binding-unavailable"));
					return;
				}
				if (text(value))
					put(d, row, name, detached_cell::utf8(std::string{value}));
			}
			void binary(const sdk::relation_descriptor& d,
						sdk::detached_row* row,
						std::string_view name,
						std::span<const std::byte> value,
						bool known = false)
			{
				if (!row || failure)
					return;
				if (value.empty() && !known)
					return;
				if (value.size() > limits.maximum_text_bytes ||
					!charge(value.size() + 1U, value.size() * 2U + sizeof(std::vector<std::byte>)))
				{
					if (!failure)
						failure = sdk::error{"native.object-budget", "bytes", "resource-limit"};
					return;
				}
				put(d,
					row,
					name,
					detached_cell::bytes(std::vector<std::byte>(value.begin(), value.end())));
			}
			void number(const sdk::relation_descriptor& d,
						sdk::detached_row* row,
						std::string_view name,
						std::size_t v)
			{
				put(d, row, name, detached_cell::unsigned_integer(v));
			}
			void boolean(const sdk::relation_descriptor& d,
						 sdk::detached_row* row,
						 std::string_view name,
						 bool v)
			{
				put(d, row, name, detached_cell::boolean(v));
			}
			std::string identity(const sdk::relation_descriptor& d, sdk::detached_row* row)
			{
				if (!row || failure)
					return {};
				// Canonical encoding used by derive_domain_identity owns a temporary copy.
				for (const auto& [key, value] : row->cells)
				{
					if (!charge(key.size() + 1U, key.size() + 256U))
						return {};
					if (value.value)
					{
						if (auto s = std::get_if<std::string>(&*value.value);
							s && !charge(s->size() + 1U, s->size() * 2U))
							return {};
						if (auto b = std::get_if<std::vector<std::byte>>(&*value.value);
							b && !charge(b->size() + 1U, b->size() * 2U))
							return {};
					}
				}
				auto id = sdk::derive_domain_identity(d, *row);
				if (!id)
				{
					failure = id.error();
					return {};
				}
				if (!text(*id))
					return {};
				if (!d.domain_identity.result_column)
				{
					failure =
						sdk::error{"native.object-input", "identity", "missing-original-domain"};
					return {};
				}
				auto result_column = d.column(*d.domain_identity.result_column);
				if (!result_column)
				{
					failure = result_column.error();
					return {};
				}
				auto result = *id;
				put(d,
					row,
					result_column->name,
					detached_cell::typed(result_column->type.parameter, std::move(*id)));
				if (failure)
					return {};
				auto valid = sdk::validate_row(d, *row);
				if (!valid)
				{
					failure = valid.error();
					return {};
				}
				return result;
			}
			std::span<const std::byte> usr(const clang::NamedDecl* d)
			{
				if (!charge() || !d || !bindings.usr)
					return {};
				return bindings.usr(*d);
			}
			std::string_view decl(const clang::Decl* d)
			{
				if (!charge() || !d || !bindings.declaration)
					return {};
				return bindings.declaration(*d);
			}
			std::string_view entity(const clang::NamedDecl* d)
			{
				if (!charge() || !d || !bindings.entity)
					return {};
				return bindings.entity(*d);
			}
			std::string_view syntax(const clang::Stmt* e)
			{
				if (!charge() || !e || !bindings.syntax)
					return {};
				return bindings.syntax(*e);
			}
			std::string_view source(const clang::Stmt* e)
			{
				if (!charge() || !e || !bindings.source)
					return {};
				return bindings.source(*e);
			}
			std::string_view type(clang::QualType t)
			{
				if (!charge() || t.isNull() || !bindings.canonical_type)
					return {};
				return bindings.canonical_type(t);
			}
			bool base(const sdk::relation_descriptor& d,
					  sdk::detached_row* row,
					  const clang::FunctionDecl* owner,
					  const clang::Expr* expression,
					  std::string_view profile)
			{
				string(d, row, "compile_unit", bindings.compile_unit);
				string(d, row, "profile", profile);
				string(d, row, "membership_state", "complete");
				const auto original_source = source(expression),
						   original_syntax = syntax(expression), original_decl = decl(owner),
						   original_entity = entity(owner);
				const auto raw = usr(owner);
				string(d, row, "source", original_source);
				string(d, row, "expression", original_syntax);
				string(d, row, "declaration", original_decl);
				string(d, row, "owner_entity", original_entity);
				binary(d, row, "owner_usr", raw);
				std::optional<bool> system;
				if (expression && bindings.is_system && charge())
					system = bindings.is_system(*expression);
				if (system)
					boolean(d, row, "is_system", *system);
				if (owner && bindings.body && charge())
				{
					const auto body = bindings.body(*owner);
					if (!body.empty())
						string(d, row, "body", body);
				}
				const bool complete = !original_source.empty() && !original_syntax.empty() &&
					!original_decl.empty() && !original_entity.empty() && !raw.empty() &&
					system.has_value();
				string(d, row, "binding_state", complete ? "complete" : "partial");
				return complete;
			}
			detached_cell list(std::vector<std::string>& values)
			{
				if (!charge(values.size() * (sizeof(std::size_t) * 8U + 1U)))
					return {};
				// Bound actual string comparisons by total bytes times the bounded passes.
				for (const auto& value : values)
					for (unsigned pass{}; pass < 64U; ++pass)
						if (!charge(value.size()))
							return {};
				std::ranges::sort(values);
				values.erase(std::ranges::unique(values).begin(), values.end());
				std::vector<std::byte> encoded;
				for (const auto& value : values)
				{
					if (value.size() > std::numeric_limits<std::uint32_t>::max() ||
						!charge(value.size() + 4U, value.size() * 2U + 8U))
						return {};
					for (unsigned shift{}; shift < 32U; shift += 8U)
						encoded.push_back(static_cast<std::byte>((value.size() >> shift) & 255U));
					for (char octet : value)
						encoded.push_back(
							static_cast<std::byte>(static_cast<unsigned char>(octet)));
				}
				return detached_cell::bytes(std::move(encoded));
			}
			void inventory(std::string_view name, detached_cell value)
			{
				if (failure)
					return;
				if (fields >= limits.maximum_fields)
				{
					failure = sdk::error{"native.object-budget", "fields", "resource-limit"};
					return;
				}
				const auto* c = column(r::declaration_inventory::descriptor(), name);
				if (!c)
					return;
				if (!text(c->id) || !charge(1U, 256U))
					return;
				value.type = c->type;
				++fields;
				out.inventory_facets.emplace(c->id, std::move(value));
			}
		};
		std::string_view access_kind(unsigned access)
		{
			switch (access)
			{
				case clang::AK_Read:
					return "read";
				case clang::AK_ReadObjectRepresentation:
					return "representation_read";
				case clang::AK_Assign:
					return "assign";
				case clang::AK_Increment:
					return "increment";
				case clang::AK_Decrement:
					return "decrement";
				case clang::AK_MemberCall:
					return "member_call";
				case clang::AK_DynamicCast:
					return "dynamic_cast";
				case clang::AK_TypeId:
					return "typeid";
				case clang::AK_Construct:
					return "construct";
				case clang::AK_Destroy:
					return "destroy";
				case clang::AK_IsWithinLifetime:
					return "within_lifetime";
				case clang::AK_Dereference:
					return "dereference";
			}
			return "unknown";
		}
		bool storage_path(writer& w,
						  const sdk::relation_descriptor& d,
						  sdk::detached_row* row,
						  clang::QualType type,
						  const std::vector<clang::APValue::LValuePathEntry>& path)
		{
			if (!row || type.isNull() || path.size() > 128U)
				return false;
			constexpr std::string_view magic{"cxxlens-object-path/1\0", 22U};
			std::vector<std::byte> encoded;
			if (!w.charge(magic.size() + 5U, magic.size() * 2U + 16U))
				return false;
			for (char v : magic)
				encoded.push_back(static_cast<std::byte>(static_cast<unsigned char>(v)));
			auto number = [&](std::uint64_t value, unsigned bytes)
			{
				if (!w.charge(bytes, bytes * 2U))
					return false;
				for (unsigned shift{}; shift < bytes * 8U; shift += 8U)
					encoded.push_back(static_cast<std::byte>((value >> shift) & 255U));
				return true;
			};
			if (!number(path.size(), 4U))
				return false;
			type = type.getNonReferenceType().getCanonicalType();
			for (auto entry : path)
			{
				if (!w.charge() || type.isNull())
					return false;
				unsigned kind{};
				std::uint64_t index{};
				bool virt{};
				std::span<const std::byte> raw;
				if (const auto* array = llvm::dyn_cast<clang::ArrayType>(type.getTypePtr()))
				{
					kind = 0U;
					index = entry.getAsArrayIndex();
					type = array->getElementType();
				}
				else if (const auto* complex = type->getAs<clang::ComplexType>())
				{
					kind = 1U;
					index = entry.getAsArrayIndex();
					type = complex->getElementType();
				}
				else if (const auto* vector = type->getAs<clang::VectorType>())
				{
					kind = 2U;
					index = entry.getAsArrayIndex();
					type = vector->getElementType();
				}
				else
				{
					auto member = entry.getAsBaseOrMember();
					if (auto field = llvm::dyn_cast_or_null<clang::FieldDecl>(member.getPointer()))
					{
						kind = 3U;
						raw = w.usr(field);
						type = field->getType();
					}
					else if (auto base =
								 llvm::dyn_cast_or_null<clang::CXXRecordDecl>(member.getPointer()))
					{
						kind = 4U;
						virt = member.getInt();
						raw = w.usr(base);
						type = base->getASTContext().getCanonicalTagType(base);
					}
					else
						return false;
					if (raw.empty())
						return false;
					if (raw.size() > w.limits.maximum_text_bytes)
					{
						w.failure = sdk::error{"native.object-budget", "USR", "resource-limit"};
						return false;
					}
				}
				type = type.getCanonicalType();
				if (!number(kind, 1U) || !number(index, 8U) || !number(virt ? 1U : 0U, 1U) ||
					!number(raw.size(), 4U) || !w.charge(raw.size(), raw.size() * 2U))
					return false;
				encoded.insert(encoded.end(), raw.begin(), raw.end());
			}
			w.binary(d, row, "storage_path", encoded, true);
			return !w.failure;
		}
	} // namespace
	sdk::result<project_object_event_rows>
	detach_project_object_events(const project_object_observations& observed,
								 const object_event_bindings& bindings,
								 object_facet_limits limits)
	{
		writer w{std::move(limits), bindings, {}, {}, 0U};
		w.out.operations = observed.operations;
		w.out.retained_bytes_bound = observed.retained_bytes_bound;
		if (!observed.frozen || bindings.compile_unit.empty())
			return sdk::error{"native.object-input", "events", "phase-or-original-unit"};
		if (!w.charge())
			return *w.failure;
		const auto& context = r::sequence_context::descriptor();
		const auto& pair = r::sequence_pair::descriptor();
		const auto& object = r::object_state_observation::descriptor();
		std::vector<std::string> pairs, objects;
		auto append = [&](std::vector<std::string>& values, std::string id)
		{
			if (!w.text(id) || !w.charge(1U, sizeof(std::string) * 2U))
				return;
			values.push_back(std::move(id));
		};
		for (const auto& original : observed.sequence)
		{
			if (!w.charge())
				break;
			std::string language = original.cplusplus_version
				? "c++" + std::to_string(original.cplusplus_version)
				: "c";
			w.text(language);
			auto contexts = [&](const clang::Expr* expression,
								std::size_t access,
								const auto& values) -> std::string
			{
				std::string previous;
				for (std::size_t depth{}; depth < values.size(); ++depth)
				{
					auto* row = w.row(context);
					w.base(context, row, original.owner, expression, context_profile);
					w.number(context, row, "checker_root", original.root_ordinal);
					w.number(context, row, "access_ordinal", access);
					w.number(context, row, "depth", depth);
					const auto& argument = values[depth];
					w.string(context, row, "invocation", w.syntax(argument.invocation));
					w.string(context, row, "argument", w.syntax(argument.argument));
					w.number(context, row, "argument_index", argument.index);
					w.boolean(context, row, "indeterminately_sequenced", argument.indeterminate);
					w.string(context, row, "language", language);
					if (!previous.empty())
						w.string(context, row, "parent_context", previous);
					previous = w.identity(context, row);
				}
				return previous;
			};
			auto left = contexts(original.left, original.left_access, original.left_contexts);
			auto right = contexts(original.right, original.right_access, original.right_contexts);
			auto* row = w.row(pair);
			w.base(pair, row, original.owner, original.root, sequence_profile);
			w.number(pair, row, "checker_root", original.root_ordinal);
			w.number(pair, row, "ordinal", original.ordinal);
			w.string(pair, row, "left_expression", w.syntax(original.left));
			w.string(pair, row, "right_expression", w.syntax(original.right));
			w.string(pair, row, "left_source", w.source(original.left));
			w.string(pair, row, "right_source", w.source(original.right));
			if (!left.empty())
				w.string(pair, row, "left_context", left);
			if (!right.empty())
				w.string(pair, row, "right_context", right);
			w.string(pair, row, "left_context_state", "complete");
			w.string(pair, row, "right_context_state", "complete");
			w.binary(pair, row, "storage_usr", w.usr(original.storage));
			w.string(pair, row, "storage_declaration", w.decl(original.storage));
			w.string(pair, row, "storage_entity", w.entity(original.storage));
			w.boolean(pair, row, "left_modifies", true);
			w.boolean(pair, row, "right_modifies", original.right_modifies);
			if (original.scalar_nonreference_storage)
				w.boolean(pair,
						  row,
						  "scalar_nonreference_storage",
						  *original.scalar_nonreference_storage);
			w.boolean(pair, row, "jointly_potentially_evaluated", !original.unevaluated);
			w.string(pair, row, "checker_relation", "unsequenced");
			w.string(pair, row, "language", language);
			append(pairs, w.identity(pair, row));
		}
		constexpr std::array<std::string_view, 6U> kinds{"inactive_union_member",
														 "absent_subobject",
														 "ended_call_frame",
														 "deleted_allocation",
														 "incompatible_static_downcast",
														 "before_dynamic_construction"};
		for (const auto& original : observed.objects)
		{
			if (!w.charge())
				break;
			const auto& value = original.value;
			auto kind = static_cast<std::size_t>(value.kind);
			if (kind >= kinds.size())
				return sdk::error{"native.object-input", "state_kind", "unsupported-original-kind"};
			auto* row = w.row(object);
			w.base(object, row, value.owner, value.expression, object_profile);
			w.number(object, row, "ordinal", original.ordinal);
			w.string(object, row, "state_kind", kinds[kind]);
			if (bindings.evaluation_root && w.charge())
				w.string(object,
						 row,
						 "evaluation_root",
						 bindings.evaluation_root(original.root_ordinal));
			auto access = access_kind(value.access_kind);
			if (value.kind ==
				cxxlens_object_semantics_hook::interpreter_kind::incompatible_static_downcast)
			{
				const auto* cast =
					llvm::dyn_cast_or_null<clang::CXXStaticCastExpr>(value.expression);
				access = cast && cast->getTypeAsWritten()->isReferenceType()
					? "static_reference_downcast"
					: "static_pointer_downcast";
			}
			w.string(object, row, "access_kind", access);
			w.number(object, row, "call_index", value.storage.getCallIndex());
			w.number(object, row, "generation", value.storage.getVersion());
			const auto* declaration = value.storage.dyn_cast<const clang::ValueDecl*>();
			if (declaration)
			{
				w.string(object, row, "storage_kind", "declaration");
				w.binary(object, row, "storage_usr", w.usr(declaration));
				w.string(object, row, "storage_declaration", w.decl(declaration));
				w.string(object, row, "storage_entity", w.entity(declaration));
			}
			else if (auto allocation = value.storage.dyn_cast<clang::DynamicAllocLValue>())
			{
				w.string(object, row, "storage_kind", "allocation");
				w.number(object, row, "allocation_ordinal", allocation.getIndex());
			}
			else if (auto temporary = value.storage.dyn_cast<const clang::Expr*>())
			{
				w.string(object, row, "storage_kind", "temporary");
				w.string(object, row, "storage_source", w.source(temporary));
			}
			else
				w.string(object, row, "storage_kind", "unknown");
			const bool path =
				storage_path(w, object, row, value.complete_type, original.original_path);
			w.string(object, row, "complete_type", w.type(value.complete_type));
			w.string(object, row, "subobject_type", w.type(value.subobject_type));
			if (!value.target_type.isNull())
				w.string(object, row, "target_type", w.type(value.target_type));
			w.boolean(object, row, "actual_polymorphic_use", value.actual_polymorphic_use);
			w.string(object, row, "storage_state", path && value.storage ? "complete" : "partial");
			w.string(object, row, "semantic_state", "complete");
			if (value.kind ==
				cxxlens_object_semantics_hook::interpreter_kind::inactive_union_member)
			{
				w.string(
					object, row, "active_member_state", value.active_field ? "active" : "none");
				w.binary(object,
						 row,
						 "active_field_usr",
						 w.usr(value.active_field),
						 !value.active_field);
				w.binary(object, row, "selected_field_usr", w.usr(value.selected_field));
				if (value.selected_field && value.selected_field->getType()->isScalarType() &&
					(!value.active_field || value.active_field->getType()->isScalarType()))
					w.boolean(object, row, "common_initial_sequence_permitted", false);
			}
			append(objects, w.identity(object, row));
		}
		auto census = [&](std::string_view prefix,
						  std::vector<std::string>& ids,
						  std::string_view profile,
						  bool partial)
		{
			if (!w.text(prefix) || !w.text(profile))
				return;
			const auto p = std::string{prefix};
			w.inventory(p + "_profile", detached_cell::utf8(std::string{profile}));
			w.inventory(p + "_state",
						detached_cell::utf8(!observed.hooks_installed ? "unavailable"
												: partial			  ? "partial"
																	  : "complete"));
			if (observed.hooks_installed)
			{
				auto values = w.list(ids);
				w.inventory(p + "_count", detached_cell::unsigned_integer(ids.size()));
				w.inventory(p + "_ids", std::move(values));
			}
		};
		census("sequence_pair", pairs, sequence_profile, observed.sequence_partial);
		census("object_state", objects, object_profile, observed.object_partial);
		if (w.failure)
			return *w.failure;
		return std::move(w.out);
	}
} // namespace cxxlens::detail::clang22::object_semantics
