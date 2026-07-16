#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <ranges>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] error
		relation_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool namespaced(const std::string_view value)
		{
			if (value.empty() || value.front() == '.' || value.back() == '.')
				return false;
			bool saw_dot = false;
			for (const auto byte : value)
			{
				if (byte == '.')
				{
					saw_dot = true;
					continue;
				}
				if (std::islower(static_cast<unsigned char>(byte)) == 0 &&
					std::isdigit(static_cast<unsigned char>(byte)) == 0 && byte != '_' &&
					byte != '-')
					return false;
			}
			return saw_dot;
		}

		[[nodiscard]] std::string escape(const std::string_view input)
		{
			std::ostringstream output;
			for (const auto byte : input)
			{
				switch (byte)
				{
					case '\\':
						output << "\\\\";
						break;
					case '"':
						output << "\\\"";
						break;
					case '\n':
						output << "\\n";
						break;
					case '\r':
						output << "\\r";
						break;
					case '\t':
						output << "\\t";
						break;
					default:
						if (static_cast<unsigned char>(byte) < 0x20U)
							output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
								   << static_cast<unsigned int>(static_cast<unsigned char>(byte));
						else
							output << byte;
				}
			}
			return output.str();
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
					return std::holds_alternative<std::vector<std::byte>>(value);
				case scalar_kind::utf8_string:
				case scalar_kind::digest:
				case scalar_kind::semantic_version:
				case scalar_kind::typed_id:
				case scalar_kind::open_symbol:
				case scalar_kind::condition_ref:
				case scalar_kind::source_span_id:
				case scalar_kind::evidence_id:
					return std::holds_alternative<std::string>(value);
			}
			return false;
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
	} // namespace

	std::string value_type::canonical_name() const
	{
		static constexpr std::array<std::string_view, 12U> names{
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
		if (!namespaced(name))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "name", "namespaced"));
		if (id != name + ".v" + std::to_string(semantic_major))
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "id", "semantic-major"));
		if (version.major != semantic_major)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "version", "major"));
		if (columns.empty())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.relation-invalid", "columns", "empty"));
		std::vector<std::string> ids;
		std::vector<std::string> names;
		for (const auto& value : columns)
		{
			if (value.id.empty() || value.name.empty() || !value.id.starts_with(id + "."))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.id, "identity"));
			if (!value.required && !value.type.optional)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.column-invalid", value.id, "optional-contract"));
			ids.push_back(value.id);
			names.push_back(value.name);
		}
		std::ranges::sort(ids);
		std::ranges::sort(names);
		if (std::ranges::adjacent_find(ids) != ids.end() ||
			std::ranges::adjacent_find(names) != names.end())
			return cxxlens::sdk::unexpected(relation_error("sdk.duplicate-column", "columns"));
		const auto expected = semantic_digest("cxxlens.relation-descriptor.v1", canonical_form());
		if (descriptor_digest != expected)
			return cxxlens::sdk::unexpected(
				relation_error("sdk.descriptor-digest-mismatch", "descriptor_digest"));
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
				   << escape(item.type.canonical_name()) << "\"}";
		}
		output << R"(],"id":")" << escape(id) << R"(","name":")" << escape(name)
			   << R"(","semantic_major":)" << semantic_major << R"(,"version":")"
			   << version.string() << "\"}";
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
		if (descriptor.descriptor_digest.empty())
			descriptor.descriptor_digest =
				semantic_digest("cxxlens.relation-descriptor.v1", descriptor.canonical_form());
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
		if (state == cell_state::present)
		{
			if (!value || unknown_reason || !matches_scalar(type.scalar, *value))
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-invalid", "value", "present"));
		}
		else if (state == cell_state::absent)
		{
			if (!type.optional || value || unknown_reason)
				return cxxlens::sdk::unexpected(
					relation_error("sdk.cell-invalid", "state", "absent"));
		}
		else if (value || !unknown_reason || unknown_reason->empty())
			return cxxlens::sdk::unexpected(relation_error("sdk.cell-invalid", "state", "unknown"));
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

	struct snapshot_handle::data
	{
		std::string id;
		std::map<std::string, relation_descriptor, std::less<>> descriptors;
		std::map<std::string, std::vector<detached_row>, std::less<>> rows;
	};

	snapshot_handle::snapshot_handle(std::shared_ptr<const data> data) : data_{std::move(data)} {}

	std::string_view snapshot_handle::id() const noexcept
	{
		return data_ ? std::string_view{data_->id} : std::string_view{};
	}

	result<row_cursor> snapshot_handle::open(const dynamic_relation& relation) const
	{
		if (!data_)
			return cxxlens::sdk::unexpected(relation_error("sdk.snapshot-empty", "snapshot"));
		const auto descriptor = data_->descriptors.find(relation.descriptor().id);
		if (descriptor == data_->descriptors.end() || descriptor->second != relation.descriptor())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.snapshot-relation-mismatch", relation.descriptor().id));
		const auto rows = data_->rows.find(relation.descriptor().id);
		static const std::vector<detached_row> empty_rows;
		return row_cursor{data_, rows == data_->rows.end() ? &empty_rows : &rows->second};
	}

	bool snapshot_handle::empty() const noexcept
	{
		return !data_;
	}

	row_view::row_view(const detached_row* row,
					   std::weak_ptr<const std::uint64_t> generation,
					   const std::uint64_t expected)
		: row_{row}, generation_{std::move(generation)}, expected_{expected}
	{
	}

	result<detached_row> row_view::copy() const
	{
		const auto current = generation_.lock();
		if (!current || *current != expected_ || row_ == nullptr)
			return cxxlens::sdk::unexpected(relation_error("sdk.row-view-expired", "row_view"));
		return *row_;
	}

	row_cursor::row_cursor(std::shared_ptr<const snapshot_handle::data> snapshot,
						   const std::vector<detached_row>* rows)
		: snapshot_{std::move(snapshot)}, rows_{rows}, owner_{std::this_thread::get_id()},
		  generation_{std::make_shared<std::uint64_t>(0U)}
	{
	}

	result<std::optional<row_view>> row_cursor::next()
	{
		if (owner_ != std::this_thread::get_id())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.cursor-thread-violation", "cursor"));
		++*generation_;
		if (rows_ == nullptr || index_ >= rows_->size())
			return std::optional<row_view>{};
		return std::optional<row_view>{row_view{&(*rows_)[index_++], generation_, *generation_}};
	}

	snapshot_builder::snapshot_builder(relation_registry registry) : registry_{std::move(registry)}
	{
	}

	result<void> snapshot_builder::add(detached_row row)
	{
		const auto descriptors = registry_.descriptors();
		const auto descriptor =
			std::ranges::find(descriptors, row.descriptor_id, &relation_descriptor::id);
		if (descriptor == descriptors.end())
			return cxxlens::sdk::unexpected(
				relation_error("sdk.snapshot-unknown-relation", row.descriptor_id));
		if (auto valid = validate_row(*descriptor, row); !valid)
			return valid;
		rows_[row.descriptor_id].push_back(std::move(row));
		return {};
	}

	result<snapshot_handle> snapshot_builder::publish() &&
	{
		auto value = std::make_shared<snapshot_handle::data>();
		std::string semantic;
		for (const auto& descriptor : registry_.descriptors())
		{
			value->descriptors.emplace(descriptor.id, descriptor);
			semantic += descriptor.descriptor_digest;
			semantic.push_back('\n');
		}
		for (auto& [descriptor, rows] : rows_)
		{
			std::ranges::sort(rows,
							  [](const detached_row& left, const detached_row& right)
							  {
								  return left.canonical_form() < right.canonical_form();
							  });
			for (const auto& row : rows)
			{
				semantic += row.canonical_form();
				semantic.push_back('\n');
			}
			value->rows.emplace(descriptor, std::move(rows));
		}
		value->id = semantic_digest("cxxlens.sdk-snapshot.v1", semantic);
		return snapshot_handle{std::move(value)};
	}
} // namespace cxxlens::sdk
