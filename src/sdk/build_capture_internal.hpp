#pragma once

/**
 * @file build_capture_internal.hpp
 * @brief Compiler-neutral, value-owned build-capture authority.
 *
 * This source-private model separates capture fidelity from provider execution. It owns no
 * compiler-native object, process handle, filesystem capability, or Store writer. External
 * decoders first populate a draft and receive an immutable validated value; downstream
 * materialization code must not reconstruct project, toolchain, variant, invocation, or source
 * meaning from a frontend-specific transport document.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>
#include <cxxlens/sdk/relation.hpp>

namespace cxxlens::sdk::detail
{
	/** Field-level capture fidelity. Missing and redacted values are never represented as empty. */
	enum class capture_field_state : std::uint8_t
	{
		observed,
		derived,
		redacted,
		unavailable,
	};

	/** A field whose presence and fidelity are explicit. */
	template <class T>
	struct captured_value
	{
		capture_field_state state{capture_field_state::unavailable};
		std::optional<T> value;
		std::string reason;
		std::string completion_action;

		[[nodiscard]] static captured_value observed(T input)
		{
			return {capture_field_state::observed, std::move(input), {}, {}};
		}

		[[nodiscard]] static captured_value derived(T input)
		{
			return {capture_field_state::derived, std::move(input), {}, {}};
		}

		[[nodiscard]] static captured_value redacted(std::string why, std::string action)
		{
			return {capture_field_state::redacted, std::nullopt, std::move(why), std::move(action)};
		}

		[[nodiscard]] static captured_value unavailable(std::string why, std::string action)
		{
			return {
				capture_field_state::unavailable, std::nullopt, std::move(why), std::move(action)};
		}

		[[nodiscard]] bool operator==(const captured_value&) const = default;
	};

	/** Replay fidelity of one normalized compiler option. */
	enum class replay_option_class : std::uint8_t
	{
		exact,
		semantics_preserving,
		approximation,
		unsupported,
		nonsemantic,
	};

	struct normalized_build_option
	{
		std::string token;
		replay_option_class classification{replay_option_class::unsupported};
		std::string reason;
		std::string completion_action;

		[[nodiscard]] bool operator==(const normalized_build_option&) const = default;
	};

	/** Metadata-only response/config file entry. Source bytes remain in the source closure. */
	struct build_capture_auxiliary_file
	{
		std::string logical_path;
		captured_value<std::string> content_digest;
		std::uint64_t size_bytes{};
		/** Earlier entry whose token stream referenced this file. */
		std::optional<std::size_t> parent_index;

		[[nodiscard]] bool operator==(const build_capture_auxiliary_file&) const = default;
	};

	/** One allowlisted normalized environment effect; raw environment values are not retained. */
	struct build_capture_environment_effect
	{
		std::string name;
		captured_value<std::string> semantic_value;

		[[nodiscard]] bool operator==(const build_capture_environment_effect&) const = default;
	};

	struct build_capture_invocation
	{
		captured_value<std::vector<std::string>> original_arguments;
		captured_value<std::vector<normalized_build_option>> normalized_semantic_options;
		captured_value<std::vector<std::string>> effective_replay_arguments;
		captured_value<std::vector<build_capture_auxiliary_file>> response_files;
		captured_value<std::vector<build_capture_auxiliary_file>> config_files;
		captured_value<std::vector<build_capture_environment_effect>> environment_effects;
		std::string effective_invocation_digest;
		std::string environment_digest;
		std::string language;
		std::string logical_working_directory;
		/** Explicit host roots used only by the replay port; excluded from semantic identity. */
		std::vector<std::string> qualified_read_roots;

		[[nodiscard]] bool operator==(const build_capture_invocation&) const = default;
	};

	struct build_capture_toolchain
	{
		std::string family;
		std::string exact_version;
		std::string target_triple;
		std::string builtin_headers_digest;
		std::optional<std::string> sysroot;
		std::string abi_digest;
		std::string plugin_spec_digest;
		captured_value<std::string> production_compiler_path;
		captured_value<std::string> production_compiler_binary_digest;

		[[nodiscard]] bool operator==(const build_capture_toolchain&) const = default;
	};

	struct build_capture_variant
	{
		std::string language;
		std::string language_standard;
		std::string target_triple;
		std::string predefined_macros_digest;
		std::string include_search_digest;
		std::string semantic_flags_digest;

		[[nodiscard]] bool operator==(const build_capture_variant&) const = default;
	};

	struct build_capture_source
	{
		std::string source_snapshot_id;
		std::string file_id;
		std::string logical_path;
		std::string content_digest;
		std::uint64_t size_bytes{};
		std::string encoding;
		std::string line_index_id;
		bool read_only{};

		[[nodiscard]] bool operator==(const build_capture_source&) const = default;
	};

	struct build_capture_source_closure
	{
		std::string closure_id;
		std::string closure_digest;
		std::string manifest_digest;
		std::uint64_t member_count{};
		std::uint64_t blob_count{};
		std::uint64_t unique_blob_bytes{};

		[[nodiscard]] bool operator==(const build_capture_source_closure&) const = default;
	};

	/** Mutable decoder output. It is never accepted directly by a materialization consumer. */
	struct build_capture_draft
	{
		std::string project_id;
		project_catalog catalog;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string build_variant_id;
		std::string toolchain_context_id;
		std::string toolchain_digest;
		build_capture_toolchain toolchain;
		build_capture_variant variant;
		build_capture_invocation invocation;
		build_capture_source source;
		build_capture_source_closure source_closure;
	};

	struct build_capture_limits
	{
		std::size_t maximum_catalog_compile_units{4096U};
		std::size_t maximum_arguments{4096U};
		std::size_t maximum_options{4096U};
		std::size_t maximum_auxiliary_files{4096U};
		std::size_t maximum_environment_effects{1024U};
		std::size_t maximum_auxiliary_depth{32U};
		std::size_t maximum_string_bytes{4096U};
		std::size_t maximum_total_metadata_bytes{std::size_t{64U} * 1024U * 1024U};
		std::uint64_t maximum_source_closure_members{4096U};
		std::uint64_t maximum_source_closure_blobs{4096U};
		std::uint64_t maximum_source_closure_bytes{std::uint64_t{48U} * 1024U * 1024U};
	};

	struct build_capture_gap
	{
		std::string field;
		capture_field_state state{capture_field_state::unavailable};
		std::string reason;
		std::string completion_action;

		[[nodiscard]] bool operator==(const build_capture_gap&) const = default;
	};

	/** Immutable value created only after all bounds, identities, and cross-field bindings pass. */
	class validated_build_capture
	{
	  public:
		[[nodiscard]] const build_capture_draft& value() const noexcept
		{
			return value_;
		}
		[[nodiscard]] std::string_view semantic_identity() const noexcept
		{
			return semantic_identity_;
		}
		[[nodiscard]] std::span<const build_capture_gap> gaps() const noexcept
		{
			return gaps_;
		}

	  private:
		validated_build_capture(build_capture_draft value,
								std::string semantic_identity,
								std::vector<build_capture_gap> gaps)
			: value_{std::move(value)}, semantic_identity_{std::move(semantic_identity)},
			  gaps_{std::move(gaps)}
		{
		}

		build_capture_draft value_;
		std::string semantic_identity_;
		std::vector<build_capture_gap> gaps_;

		friend result<validated_build_capture> validate_build_capture(build_capture_draft,
																	  build_capture_limits);
	};

	/** Validate one draft and derive its machine-location-independent semantic identity. */
	[[nodiscard]] result<validated_build_capture>
	validate_build_capture(build_capture_draft draft, build_capture_limits limits = {});

	/** Validate a complete capture census and reject duplicate/conflicting semantic authorities. */
	[[nodiscard]] result<std::vector<validated_build_capture>>
	validate_build_capture_set(std::vector<build_capture_draft> drafts,
							   build_capture_limits limits = {});
} // namespace cxxlens::sdk::detail
