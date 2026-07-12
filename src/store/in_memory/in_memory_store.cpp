#include <algorithm>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <tuple>
#include <utility>

#include "../store_port.hpp"

namespace cxxlens::detail::store
{
	namespace
	{
		[[nodiscard]] error transaction_error(std::string reason)
		{
			error failure;
			failure.code.value = "facts.transaction-failed";
			failure.message = "Fact snapshot transaction failed";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] bool fact_less(const facts::detached_fact_record& left,
									 const facts::detached_fact_record& right)
		{
			return std::tuple{left.kind, left.stable_key, left.id.value()} <
				std::tuple{right.kind, right.stable_key, right.id.value()};
		}

		struct memory_state
		{
			mutable std::mutex mutex;
			std::shared_ptr<const snapshot_data> current;
			bool writer{};
		};

		class memory_transaction final : public snapshot_transaction
		{
		  public:
			memory_transaction(std::shared_ptr<memory_state> state,
							   snapshot_metadata metadata,
							   const transaction_fault fault)
				: shared_{std::move(state)}, metadata_{std::move(metadata)}, fault_{fault}
			{
			}
			~memory_transaction() override
			{
				rollback();
			}

			[[nodiscard]] transaction_state state() const noexcept override
			{
				return state_;
			}

			result<void> stage(const facts::reduction_result& reduction) override
			{
				if (state_ != transaction_state::created)
					return transaction_error("illegal-stage-transition");
				if (auto checked = reduction.validate(); !checked)
					return fail("invalid-reduction");
				auto snapshot = std::make_shared<snapshot_data>();
				snapshot->metadata = metadata_;
				{
					const std::scoped_lock lock{shared_->mutex};
					snapshot->metadata.generation = shared_->current->metadata.generation + 1U;
				}
				snapshot->facts = reduction.facts;
				std::ranges::sort(snapshot->facts, fact_less);
				snapshot->coverage = reduction.coverage;
				staged_ = std::move(snapshot);
				state_ = transaction_state::staged;
				if (fault_ == transaction_fault::after_stage)
					return fail("injected-after-stage");
				return {};
			}

			result<void> validate() override
			{
				if (state_ != transaction_state::staged || !staged_)
					return transaction_error("illegal-validate-transition");
				if (auto checked = staged_->validate(); !checked)
					return fail("snapshot-validation-failed");
				state_ = transaction_state::validated;
				if (fault_ == transaction_fault::after_validate)
					return fail("injected-after-validate");
				return {};
			}

			result<void> commit() override
			{
				if (state_ != transaction_state::validated || !staged_)
					return transaction_error("illegal-commit-transition");
				if (fault_ == transaction_fault::before_publish)
					return fail("injected-before-publish");
				const std::scoped_lock lock{shared_->mutex};
				if (fault_ == transaction_fault::during_publish)
					return fail_locked("injected-during-publish");
				shared_->current = std::move(staged_);
				shared_->writer = false;
				owns_writer_ = false;
				state_ = transaction_state::committed;
				return {};
			}

			void rollback() noexcept override
			{
				if (!owns_writer_)
					return;
				const std::scoped_lock lock{shared_->mutex};
				shared_->writer = false;
				owns_writer_ = false;
				staged_.reset();
				state_ = transaction_state::rolled_back;
			}

		  private:
			result<void> fail(std::string reason)
			{
				rollback();
				return transaction_error(std::move(reason));
			}

			result<void> fail_locked(std::string reason)
			{
				shared_->writer = false;
				owns_writer_ = false;
				staged_.reset();
				state_ = transaction_state::rolled_back;
				return transaction_error(std::move(reason));
			}

			std::shared_ptr<memory_state> shared_;
			snapshot_metadata metadata_;
			transaction_fault fault_{transaction_fault::none};
			transaction_state state_{transaction_state::created};
			std::shared_ptr<snapshot_data> staged_;
			bool owns_writer_{true};
		};

		class in_memory_store final : public fact_store_port
		{
		  public:
			explicit in_memory_store(snapshot_metadata expected)
				: expected_{std::move(expected)}, shared_{std::make_shared<memory_state>()}
			{
				auto initial = std::make_shared<snapshot_data>();
				initial->metadata = expected_;
				shared_->current = std::move(initial);
			}

			[[nodiscard]] std::string backend_id() const override
			{
				return "in-memory";
			}

			[[nodiscard]] compatibility_state compatibility() const noexcept override
			{
				return compatibility_state::compatible;
			}

			[[nodiscard]] result<std::shared_ptr<const snapshot_data>> read() const override
			{
				const std::scoped_lock lock{shared_->mutex};
				return shared_->current;
			}

			result<std::unique_ptr<snapshot_transaction>>
			begin(snapshot_metadata metadata, const transaction_fault fault) override
			{
				if (!compatible(metadata))
					return transaction_error("metadata-incompatible");
				const std::scoped_lock lock{shared_->mutex};
				if (shared_->writer)
					return transaction_error("writer-lock-conflict");
				shared_->writer = true;
				if (fault == transaction_fault::after_begin)
				{
					shared_->writer = false;
					return transaction_error("injected-after-begin");
				}
				return std::unique_ptr<snapshot_transaction>{
					std::make_unique<memory_transaction>(shared_, std::move(metadata), fault)};
			}

			result<void> rebuild(snapshot_metadata metadata) override
			{
				const std::scoped_lock lock{shared_->mutex};
				if (shared_->writer)
					return transaction_error("writer-lock-conflict");
				expected_ = std::move(metadata);
				auto replacement = std::make_shared<snapshot_data>();
				replacement->metadata = expected_;
				replacement->metadata.generation = shared_->current->metadata.generation + 1U;
				shared_->current = std::move(replacement);
				return {};
			}

			result<void> compact() override
			{
				return {};
			}

		  private:
			[[nodiscard]] bool compatible(const snapshot_metadata& value) const
			{
				return value.workspace_key == expected_.workspace_key &&
					value.schema_version == expected_.schema_version &&
					value.semantics_version == expected_.semantics_version &&
					value.adapter_id == expected_.adapter_id &&
					value.adapter_version == expected_.adapter_version &&
					value.extractor_versions == expected_.extractor_versions;
			}

			snapshot_metadata expected_;
			std::shared_ptr<memory_state> shared_;
		};
	} // namespace

	result<std::shared_ptr<fact_store_port>> make_in_memory_store(snapshot_metadata expected)
	{
		if (expected.workspace_key.empty())
			return transaction_error("empty-workspace-key");
		return std::shared_ptr<fact_store_port>{
			std::make_shared<in_memory_store>(std::move(expected))};
	}
} // namespace cxxlens::detail::store
