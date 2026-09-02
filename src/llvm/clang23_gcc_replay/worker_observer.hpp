#pragma once

/**
 * @file worker_observer.hpp
 * @brief Value-owned Clang 23 observations without relation or publication authority.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace clang
{
	class ASTContext;
} // namespace clang

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::size_t observer_product_maximum_observations = 100000U;
	inline constexpr std::size_t observer_product_maximum_traversal_entries = 2000000U;
	inline constexpr std::size_t observer_product_maximum_traversal_depth = 4096U;
	inline constexpr std::size_t observer_product_maximum_logical_bytes =
		std::size_t{16U} * 1024U * 1024U;

	struct observer_limits
	{
		std::size_t maximum_observations{observer_product_maximum_observations};
		std::size_t maximum_traversal_entries{observer_product_maximum_traversal_entries};
		std::size_t maximum_traversal_depth{observer_product_maximum_traversal_depth};
		std::size_t maximum_logical_bytes{observer_product_maximum_logical_bytes};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	struct observed_source_span
	{
		std::string logical_path;
		std::uint64_t begin{};
		std::uint64_t end{};
		bool macro_spelling{};

		[[nodiscard]] bool operator==(const observed_source_span&) const = default;
	};

	struct observed_entity
	{
		std::string provider_local_key;
		std::string kind;
		std::string qualified_name;
		std::string canonical_type;
		observed_source_span source;
		bool definition{};

		[[nodiscard]] bool operator==(const observed_entity&) const = default;
	};

	struct observed_declaration
	{
		std::string entity_provider_local_key;
		std::string kind;
		std::string storage;
		std::string linkage;
		observed_source_span source;
		bool deleted{};
		bool defaulted{};

		[[nodiscard]] bool operator==(const observed_declaration&) const = default;
	};

	struct observed_type
	{
		std::string provider_local_key;
		std::string owning_entity_provider_local_key;
		std::string constructor;
		std::string canonical_spelling;
		bool dependent{};

		[[nodiscard]] bool operator==(const observed_type&) const = default;
	};

	struct observed_direct_call
	{
		std::optional<std::string> caller_provider_local_key;
		std::string target_provider_local_key;
		std::string kind;
		observed_source_span source;

		[[nodiscard]] bool operator==(const observed_direct_call&) const = default;
	};

	/**
	 * Raw compiler observations detached while ASTContext is alive.
	 *
	 * Provider-local keys are not canonical cxxlens entity/type IDs. This value carries no
	 * relation, coverage, closure, guarantee, trust, or publication authority.
	 */
	struct observation_batch
	{
		std::vector<observed_entity> entities;
		std::vector<observed_declaration> declarations;
		std::vector<observed_type> types;
		std::vector<observed_direct_call> direct_calls;
		std::vector<std::string> limitations;
		std::size_t traversal_entries{};

		[[nodiscard]] bool operator==(const observation_batch&) const = default;
	};

	/** Observe only source locations mounted below the canonical replay VFS root. */
	[[nodiscard]] sdk::result<observation_batch>
	observe_translation_unit(clang::ASTContext& context, observer_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
