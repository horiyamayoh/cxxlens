#pragma once

/** @file models.hpp @brief Stable API model contract declarations. */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core.hpp>
#include <cxxlens/core/failure.hpp>
#include <cxxlens/core/finding.hpp>
#include <cxxlens/source.hpp>

namespace cxxlens::flow
{
	class source_model;
	class sink_model;
	class barrier_model;
} // namespace cxxlens::flow

namespace cxxlens::models
{
	enum class effect_kind : std::uint16_t
	{
		reads_memory,
		writes_memory,
		allocates,
		frees,
		locks,
		unlocks,
		blocks,
		performs_io,
		throws_,
		terminates,
		returns_untrusted,
		sanitizes,
		transfers_ownership,
		borrows,
		escapes,
		custom,
	};

	struct parameter_effect
	{
		std::size_t index{};
		std::vector<effect_kind> effects;
		std::optional<std::size_t> related_parameter;
	};

	struct api_effects
	{
		std::vector<effect_kind> function_effects;
		std::vector<parameter_effect> parameters;
		std::vector<std::size_t> outputs;
		std::optional<std::size_t> size_parameter;
		bool unknown_external_effects{true};
	};

	enum class model_binding_state : std::uint8_t
	{
		exact,
		ambiguous,
		unresolved,
		unsupported,
	};

	enum class model_precedence : std::uint8_t
	{
		reject_conflicts,
		prefer_left,
		prefer_right,
	};

	struct model_pack_options
	{
		std::size_t input_budget_bytes{4 * 1024 * 1024};
		bool allow_forward_compatible_fields{};
		bool allow_migration{};
		bool trusted_input{};
	};

	class api_model_pack
	{
	  public:
		[[nodiscard]] static result<api_model_pack> empty(std::string id, semantic_version version);
		[[nodiscard]] result<api_model_pack> function(std::string qualified_name,
													  api_effects effects) const;
		[[nodiscard]] result<api_model_pack>
		method(std::string receiver_type, std::string method_name, api_effects effects) const;
		[[nodiscard]] result<api_model_pack> replacement(std::string old_api,
														 std::string new_api) const;
		[[nodiscard]] result<api_model_pack> source(std::string name,
													flow::source_model model) const;
		[[nodiscard]] result<api_model_pack> sink(std::string name, flow::sink_model model) const;
		[[nodiscard]] result<api_model_pack> barrier(std::string name,
													 flow::barrier_model model) const;
		[[nodiscard]] result<api_model_pack>
		merge(const api_model_pack& other,
			  model_precedence precedence = model_precedence::reject_conflicts) const;
		[[nodiscard]] static result<api_model_pack> load(path yaml_or_json,
														 model_pack_options options = {});
		[[nodiscard]] result<void> save(path destination, model_pack_options options = {}) const;
		[[nodiscard]] std::string_view id() const noexcept;
		[[nodiscard]] semantic_version version() const noexcept;
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		std::shared_ptr<const data> data_;
	};
} // namespace cxxlens::models
