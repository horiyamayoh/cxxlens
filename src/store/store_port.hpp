#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>

#include "../facts/reducer.hpp"

namespace cxxlens::detail::store
{
	struct snapshot_metadata
	{
		std::string workspace_key;
		std::string schema_version{"cxxlens.fact-snapshot.v1"};
		std::string semantics_version{"1.0.0"};
		std::string adapter_id{"clang22.frontend"};
		std::string adapter_version{"1.0.0"};
		std::map<std::string, std::string> extractor_versions;
		std::uint64_t generation{};
		bool operator==(const snapshot_metadata&) const = default;
	};

	struct snapshot_data
	{
		snapshot_metadata metadata;
		std::vector<facts::detached_fact_record> facts;
		coverage_report coverage;
		[[nodiscard]] result<void> validate() const;
	};

	enum class transaction_state : std::uint8_t
	{
		created,
		staged,
		validated,
		committed,
		rolled_back,
	};

	enum class transaction_fault : std::uint8_t
	{
		none,
		after_begin,
		after_stage,
		after_validate,
		before_publish,
		during_publish,
	};

	enum class compatibility_state : std::uint8_t
	{
		compatible,
		rebuild_required,
		corrupt,
	};

	class snapshot_transaction
	{
	  public:
		virtual ~snapshot_transaction() = default;
		[[nodiscard]] virtual transaction_state state() const noexcept = 0;
		[[nodiscard]] virtual result<void> stage(const facts::reduction_result& reduction) = 0;
		[[nodiscard]] virtual result<void> validate() = 0;
		[[nodiscard]] virtual result<void> commit() = 0;
		virtual void rollback() noexcept = 0;
	};

	class fact_store_port
	{
	  public:
		virtual ~fact_store_port() = default;
		[[nodiscard]] virtual std::string backend_id() const = 0;
		[[nodiscard]] virtual compatibility_state compatibility() const noexcept = 0;
		[[nodiscard]] virtual result<std::shared_ptr<const snapshot_data>> read() const = 0;
		[[nodiscard]] virtual result<std::unique_ptr<snapshot_transaction>>
		begin(snapshot_metadata metadata, transaction_fault fault = transaction_fault::none) = 0;
		[[nodiscard]] virtual result<void> rebuild(snapshot_metadata metadata) = 0;
		[[nodiscard]] virtual result<void> compact() = 0;
	};

	[[nodiscard]] result<std::shared_ptr<fact_store_port>>
	make_in_memory_store(snapshot_metadata expected);
	[[nodiscard]] result<std::shared_ptr<fact_store_port>>
	open_sqlite_store(const path& database, const snapshot_metadata& expected);
} // namespace cxxlens::detail::store
