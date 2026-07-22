#include "llvm/clang22/materialization_request_v2_1.hpp"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "llvm/clang22/materialization_identity.hpp"
#include "llvm/clang22/materialization_request_identity.hpp"
#include "llvm/clang22/materialization_task_spool.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::span<const std::byte> bytes(const std::string_view value)
	{
		return std::as_bytes(std::span{value.data(), value.size()});
	}

	void replace_once(std::string& value, const std::string_view from, const std::string_view to)
	{
		const auto offset = value.find(from);
		require(offset != std::string::npos, "fixture replacement target missing");
		value.replace(offset, from.size(), to);
	}

	void replace_all(std::string& value, const std::string_view from, const std::string_view to)
	{
		std::size_t offset{};
		std::size_t count{};
		while ((offset = value.find(from, offset)) != std::string::npos)
		{
			value.replace(offset, from.size(), to);
			offset += to.size();
			++count;
		}
		require(count != 0U, "fixture replacement set was empty");
	}

	void replace_nth(std::string& value,
					 const std::string_view from,
					 const std::string_view to,
					 const std::size_t occurrence)
	{
		std::size_t offset{};
		for (std::size_t index{}; index <= occurrence; ++index)
		{
			offset = value.find(from, offset);
			require(offset != std::string::npos, "fixture replacement occurrence missing");
			if (index != occurrence)
				offset += from.size();
		}
		value.replace(offset, from.size(), to);
	}

	class fragmented_spool final : public materialization_replayable_spool
	{
	  public:
		fragmented_spool(std::unique_ptr<materialization_replayable_spool> delegate,
						 const std::size_t maximum_fragment)
			: delegate_{std::move(delegate)}, maximum_fragment_{maximum_fragment}
		{
		}

		materialization_io_result<void> append(std::span<const std::byte> input) override
		{
			return delegate_->append(input);
		}

		materialization_io_result<void> seal() override
		{
			return delegate_->seal();
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			return delegate_->read(
				destination.first(std::min(destination.size(), maximum_fragment_)));
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			if (failure_offset_ && offset == *failure_offset_)
			{
				if (failure_kind_)
					return materialization_io_failure{*failure_kind_,
													  materialization_io_operation::spool_read};
				return std::size_t{};
			}
			auto result = delegate_->read_at(
				offset, destination.first(std::min(destination.size(), maximum_fragment_)));
			if (result)
			{
				++read_at_calls_;
				read_at_bytes_ += *result;
			}
			return result;
		}

		materialization_io_result<void> rewind() override
		{
			return delegate_->rewind();
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return delegate_->size_bytes();
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return delegate_->sealed();
		}

		std::unique_ptr<materialization_replayable_spool> delegate_;
		std::size_t maximum_fragment_{};
		std::optional<std::uint64_t> failure_offset_;
		std::optional<materialization_io_failure_kind> failure_kind_;
		std::uint64_t read_at_calls_{};
		std::uint64_t read_at_bytes_{};
	};

	class toggled_index_read_spool final : public materialization_replayable_spool
	{
	  public:
		explicit toggled_index_read_spool(
			std::unique_ptr<materialization_replayable_spool> delegate)
			: delegate_{std::move(delegate)}
		{
		}

		materialization_io_result<void> append(const std::span<const std::byte> input) override
		{
			return delegate_->append(input);
		}

		materialization_io_result<void> seal() override
		{
			return delegate_->seal();
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			return delegate_->read(destination);
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			if (fail_read_)
				return materialization_io_failure{materialization_io_failure_kind::spool,
												  materialization_io_operation::spool_read};
			return delegate_->read_at(offset, destination);
		}

		materialization_io_result<void> rewind() override
		{
			return delegate_->rewind();
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return delegate_->size_bytes();
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return delegate_->sealed();
		}

		std::unique_ptr<materialization_replayable_spool> delegate_;
		bool fail_read_{};
	};

	enum class auxiliary_fault : std::uint8_t
	{
		none,
		create,
		create_allocation,
		create_invalid_configuration,
		create_null,
		append,
		append_allocation,
		append_invalid_configuration,
		append_success_drop,
		seal,
		seal_allocation,
		seal_invalid_configuration,
		read,
		read_allocation,
		read_invalid_configuration,
		read_success_zero,
		read_success_overreport,
		read_success_corrupt,
		size_success_drift,
		digest_update,
		digest_update_allocation,
		digest_update_invalid_configuration,
		digest_finalize,
		digest_finalize_allocation,
		digest_finalize_invalid_configuration,
		digest_finalize_malformed,
		corrupt_record_size,
		corrupt_ordinal,
		corrupt_raw_span,
	};

	[[nodiscard]] materialization_io_failure
	injected_failure(const materialization_io_operation operation)
	{
		return {materialization_io_failure_kind::spool, operation};
	}

	class fault_injecting_auxiliary_spool final : public materialization_replayable_spool
	{
	  public:
		fault_injecting_auxiliary_spool(std::unique_ptr<materialization_replayable_spool> delegate,
										const auxiliary_fault fault)
			: delegate_{std::move(delegate)}, fault_{fault}
		{
		}

		materialization_io_result<void> append(std::span<const std::byte> input) override
		{
			if (fault_ == auxiliary_fault::append_success_drop)
				return {};
			if (fault_ == auxiliary_fault::append || fault_ == auxiliary_fault::append_allocation ||
				fault_ == auxiliary_fault::append_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::append_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == auxiliary_fault::append_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_write};
			return delegate_->append(input);
		}

		materialization_io_result<void> seal() override
		{
			if (fault_ == auxiliary_fault::seal || fault_ == auxiliary_fault::seal_allocation ||
				fault_ == auxiliary_fault::seal_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::seal_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == auxiliary_fault::seal_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_seal};
			return delegate_->seal();
		}

		materialization_io_result<std::size_t> read(std::span<std::byte> destination) override
		{
			if (fault_ == auxiliary_fault::read || fault_ == auxiliary_fault::read_allocation ||
				fault_ == auxiliary_fault::read_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::read_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == auxiliary_fault::read_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_read};
			return delegate_->read(destination);
		}

		materialization_io_result<std::size_t> read_at(const std::uint64_t offset,
													   std::span<std::byte> destination) override
		{
			if (fault_ == auxiliary_fault::read || fault_ == auxiliary_fault::read_allocation ||
				fault_ == auxiliary_fault::read_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::read_allocation
						? materialization_io_failure_kind::allocation
						: fault_ == auxiliary_fault::read_invalid_configuration
						? materialization_io_failure_kind::invalid_configuration
						: materialization_io_failure_kind::spool,
					materialization_io_operation::spool_read};
			auto read = delegate_->read_at(offset, destination);
			if (read && *read != 0U && fault_ == auxiliary_fault::read_success_zero)
				return 0U;
			if (read && fault_ == auxiliary_fault::read_success_overreport)
				return destination.size() + 1U;
			if (read && *read != 0U && fault_ == auxiliary_fault::read_success_corrupt)
				destination.front() ^= std::byte{0x01U};
			if (read &&
				(fault_ == auxiliary_fault::corrupt_ordinal ||
				 fault_ == auxiliary_fault::corrupt_raw_span))
				for (std::size_t index{}; index < *read; ++index)
				{
					const auto record_byte = (offset + index) % 64U;
					if ((fault_ == auxiliary_fault::corrupt_ordinal && record_byte == 47U) ||
						(fault_ == auxiliary_fault::corrupt_raw_span && record_byte == 48U))
						destination[index] ^= std::byte{0x01U};
				}
			return read;
		}

		materialization_io_result<void> rewind() override
		{
			return delegate_->rewind();
		}

		[[nodiscard]] std::uint64_t size_bytes() const noexcept override
		{
			return delegate_->size_bytes() +
				(fault_ == auxiliary_fault::corrupt_record_size ||
						 fault_ == auxiliary_fault::size_success_drift
					 ? 1U
					 : 0U);
		}

		[[nodiscard]] bool sealed() const noexcept override
		{
			return delegate_->sealed();
		}

	  private:
		std::unique_ptr<materialization_replayable_spool> delegate_;
		auxiliary_fault fault_{};
	};

	class fault_injecting_digest final : public materialization_digest_accumulator
	{
	  public:
		fault_injecting_digest(std::unique_ptr<materialization_digest_accumulator> delegate,
							   const auxiliary_fault fault)
			: delegate_{std::move(delegate)}, fault_{fault}
		{
		}

		materialization_io_result<void> update(std::span<const std::byte> input) override
		{
			if (fault_ == auxiliary_fault::digest_update ||
				fault_ == auxiliary_fault::digest_update_allocation ||
				fault_ == auxiliary_fault::digest_update_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::digest_update ? materialization_io_failure_kind::hash
						: fault_ == auxiliary_fault::digest_update_allocation
						? materialization_io_failure_kind::allocation
						: materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::digest_update};
			return delegate_->update(input);
		}

		materialization_io_result<std::string> finish() override
		{
			if (fault_ == auxiliary_fault::digest_finalize ||
				fault_ == auxiliary_fault::digest_finalize_allocation ||
				fault_ == auxiliary_fault::digest_finalize_invalid_configuration)
				return materialization_io_failure{
					fault_ == auxiliary_fault::digest_finalize
						? materialization_io_failure_kind::hash
						: fault_ == auxiliary_fault::digest_finalize_allocation
						? materialization_io_failure_kind::allocation
						: materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::digest_finalize};
			if (fault_ == auxiliary_fault::digest_finalize_malformed)
				return std::string{"SHA256:not-canonical"};
			return delegate_->finish();
		}

	  private:
		std::unique_ptr<materialization_digest_accumulator> delegate_;
		auxiliary_fault fault_{};
	};

	class auxiliary_factory : public materialization_v2_1_auxiliary_spool_factory
	{
	  public:
		auxiliary_factory(const materialization_v2_1_auxiliary_spool_purpose target =
							  materialization_v2_1_auxiliary_spool_purpose::task_unique_index,
						  const auxiliary_fault fault = auxiliary_fault::none)
			: target_{target}, fault_{fault}
		{
		}

		materialization_io_result<std::unique_ptr<materialization_replayable_spool>>
		create(const materialization_v2_1_auxiliary_spool_purpose purpose) override
		{
			if (purpose == materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata)
				++collision_comparison_spools_;
			if (purpose == target_ && fault_ == auxiliary_fault::create)
				return injected_failure(materialization_io_operation::spool_create);
			if (purpose == target_ && fault_ == auxiliary_fault::create_allocation)
				return materialization_io_failure{materialization_io_failure_kind::allocation,
												  materialization_io_operation::spool_create};
			if (purpose == target_ && fault_ == auxiliary_fault::create_invalid_configuration)
				return materialization_io_failure{
					materialization_io_failure_kind::invalid_configuration,
					materialization_io_operation::spool_create};
			if (purpose == target_ && fault_ == auxiliary_fault::create_null)
				return std::unique_ptr<materialization_replayable_spool>{};
			auto created = make_materialization_private_spool();
			if (!created)
				return created.error();
			if (purpose == target_ && fault_ != auxiliary_fault::none)
				return std::unique_ptr<materialization_replayable_spool>{
					std::make_unique<fault_injecting_auxiliary_spool>(std::move(*created), fault_)};
			return std::move(*created);
		}

		std::unique_ptr<materialization_digest_accumulator>
		make_digest(const materialization_v2_1_auxiliary_spool_purpose purpose) override
		{
			auto digest = materialization_v2_1_auxiliary_spool_factory::make_digest(purpose);
			if (purpose == target_ && fault_ >= auxiliary_fault::digest_update &&
				fault_ <= auxiliary_fault::digest_finalize_malformed)
				return std::make_unique<fault_injecting_digest>(std::move(digest), fault_);
			return digest;
		}

		std::uint64_t collision_comparison_spools_{};

	  protected:
		materialization_v2_1_auxiliary_spool_purpose target_;
		auxiliary_fault fault_{};
	};

	class constant_digest final : public materialization_digest_accumulator
	{
	  public:
		materialization_io_result<void> update(std::span<const std::byte>) override
		{
			return {};
		}

		materialization_io_result<std::string> finish() override
		{
			return "sha256:" + std::string(64U, '0');
		}
	};

	class forced_collision_factory final : public auxiliary_factory
	{
	  public:
		explicit forced_collision_factory(const materialization_v2_1_auxiliary_spool_purpose target)
			: auxiliary_factory{target, auxiliary_fault::none}
		{
		}

		std::unique_ptr<materialization_digest_accumulator>
		make_digest(const materialization_v2_1_auxiliary_spool_purpose purpose) override
		{
			if (purpose == target_)
				return std::make_unique<constant_digest>();
			return materialization_v2_1_auxiliary_spool_factory::make_digest(purpose);
		}
	};

	struct scanned_request
	{
		std::unique_ptr<fragmented_spool> spool;
		materialization_request_envelope envelope;
		std::unique_ptr<materialization_request_task_index> index;
	};

	[[nodiscard]] scanned_request scan(std::string_view raw, const std::size_t fragment)
	{
		auto storage = make_materialization_private_spool();
		require(storage.has_value(), "request spool creation failed");
		auto spool = std::make_unique<fragmented_spool>(std::move(*storage), fragment);
		require(spool->append(bytes(raw)).has_value(), "request spool write failed");
		require(spool->seal().has_value(), "request spool seal failed");
		auto index = make_materialization_request_task_index(spool->size_bytes());
		require(index.has_value(), "task index creation failed");
		auto envelope = scan_materialization_request_envelope(*spool, {17U}, index->get());
		require(envelope.has_value(), "request pass-one scan failed");
		return {std::move(spool), std::move(*envelope), std::move(*index)};
	}

	[[nodiscard]] sdk::result<prevalidated_materialization_request_v2_1>
	prevalidate(std::string raw, const std::size_t fragment = 7U)
	{
		auto scanned = scan(raw, fragment);
		return prevalidate_materialization_request_v2_1(
			std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
	}

	[[nodiscard]] sdk::result<validated_materialization_request_v2_1>
	validate(std::string raw, const std::size_t fragment = 7U)
	{
		auto scanned = scan(raw, fragment);
		return validate_materialization_request_v2_1(
			std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
	}

	void replace_first_string_member(std::string& value,
									 const std::string_view name,
									 const std::string_view replacement)
	{
		const auto prefix = '"' + std::string{name} + "\":\"";
		const auto member = value.find(prefix);
		require(member != std::string::npos, "fixture string member missing");
		const auto begin = member + prefix.size();
		const auto end = value.find('"', begin);
		require(end != std::string::npos, "fixture string member is unterminated");
		value.replace(begin, end - begin, replacement);
	}

	[[nodiscard]] std::string fixture_v2_0()
	{
		return R"cxxlens_json({"engine":{"admitted_descriptors":[{"descriptor_id":"build.compile_unit.v1","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679"},{"descriptor_id":"build.project.v1","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9"},{"descriptor_id":"build.toolchain_context.v1","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0"},{"descriptor_id":"build.variant.v1","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6"},{"descriptor_id":"cc.call_direct_target.v1","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"descriptor_id":"cc.call_site.v1","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"descriptor_id":"cc.entity.v1","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"descriptor_id":"frontend.clang22.call_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"descriptor_id":"frontend.clang22.entity_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"descriptor_id":"frontend.clang22.type_observation.v2","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"},{"descriptor_id":"source.file.v1","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609"},{"descriptor_id":"source.span.v1","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86"}],"engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","engine_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","generation_contract":"cxxlens.clang22-materialization-engine.v2"},"group_topology":{"atomic_output_group":"clang22-atomic","dependency_groups":["canonical","observation"],"partial_policy":"forbid"},"interpretation_policy":{"interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","policy_id":"cxxlens.clang22-interpretation-policy.v1","selected_domain":"cc.clang22-canonical-1"},"materialization_request_id":"materialization:semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","project":{"catalog_compile_unit_census_digest":"semantic-v2:sha256:806e8f7964f77dcec9a30078129430a733c89e39488e7fae80b68c7a50d186ba","catalog_compile_units":[{"catalog_compile_unit_id":"catalog-unit:0000","effective_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","source_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196"},{"catalog_compile_unit_id":"catalog-unit:0001","effective_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","source_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49"}],"catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","logical_root":"project://fixture","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3"},"publication":{"backend":"memory","expected_parent_publication":null,"genesis":true,"partial_policy":"forbid","reopen_before_success":true,"selector":{"catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","channel_id":"channel:clang22-production","condition_universe_id":"condition-universe:one","engine_generation_id":"engine-generation:sha256:984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb","interpretation_policy_digest":"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f","relation_registry_digest":"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a","trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053"},"series_id":"snapshot-series:sha256:c405319c06ab507d9ec7bff97664b5ddac4d211549f4cd72f4fc56621666cdd3","sqlite_path":null,"transaction_count":1},"registry":{"authority_registry_digest":"sha256:4caf626ec6f198118802f22d9cac62b02b2c3bb392fdc8d68b1a58f8101c342e","base_descriptors":[{"contract_digest":"sha256:a0b4b380ab0f5b631fa8ff59c39dcfbd859e26f849d169ae5d6a428e2f9eff5f","descriptor_id":"build.project.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:97e5d3d4546803be5de464e5d5de7617b9f4ed29bcb81e503dc6c5a613277cd9","stage_order":0},{"contract_digest":"sha256:06383e29854c5ce463c996a7a36b6954a4d6388b8384ddc39ad62688bdac0663","descriptor_id":"build.toolchain_context.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3e8895ed57aca936310888a256c4ed31911b46fe5bbac5e045a80f80801cc4e0","stage_order":1},{"contract_digest":"sha256:1594c6f7ee0f80fdb59f11a9ab45a9521a8aab889052ba3fa40cf1d790aa66a1","descriptor_id":"build.variant.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:56c59d76bd7921d01c54118470d2643eee5ff8e4ed0ce275f69e9d6ef45500e6","stage_order":2},{"contract_digest":"sha256:3c325526160c00ceccd0c43f384689fff95187ef97f926871917ce6b4f7f429a","descriptor_id":"source.file.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:3aebbb05303ba924f1c25547242a656c59d95c265fe99cc3fd77db8633af8609","stage_order":3},{"contract_digest":"sha256:8b019f86c953ce3d08475a726b16dcb355e1474238b6a4300d7dd3dc9fc299b3","descriptor_id":"build.compile_unit.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:1dde734221f3db42a0bdadd531740c35e6f30c15fe196e0b20e1b60c2cf54679","stage_order":4},{"contract_digest":"sha256:645a46ad50ee0c84276ff4e09b2818486bfafe8c631f66368d45aa47cbe659ff","descriptor_id":"source.span.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","owner":"installed-tool","runtime_descriptor_digest":"semantic-v2:sha256:055e5a6997fef2d1c2dcebfe10baa41813c0ccec091409ad84a1081fd8894a86","stage_order":5}],"descriptors":[{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_direct_target.v1-batch","contract_digest":"sha256:e2960ef9dff7a1190aa6b687281e0b1aeaddfcc684f35a9870323d5716697b2b","dependency_group_id":"canonical","descriptor_id":"cc.call_direct_target.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:888196009a7344c3cfb198c0c01a359f49e4f042b998d34efc4057c3ba4e56d4"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.call_site.v1-batch","contract_digest":"sha256:4b8f7b76ef8087485462762bfef006e3fad50354da2738a61402441e9e53510e","dependency_group_id":"canonical","descriptor_id":"cc.call_site.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:8377b659e3703eef0acb446ab6b07e94aa4655aba33aa5b430e5cf65491163f2"},{"atomic_output_group_id":"clang22-atomic","batch_id":"cc.entity.v1-batch","contract_digest":"sha256:89813f031dbe91daed64d5c9d3fa1aef22a1ddcf74cf00a29f292971541f9020","dependency_group_id":"canonical","descriptor_id":"cc.entity.v1","descriptor_version":"1.0.0","output_stage":"canonical_claim","runtime_descriptor_digest":"semantic-v2:sha256:4537eb3f074379aa8c2222c9d2ed5dc530340bf1b2b5c862b4cf52b0c37b1b3e"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.call_observation.v2-batch","contract_digest":"sha256:07ea48a7f00e80972ba59c14ee96f916772ad9ed57fc84e313e3958f08fa548a","dependency_group_id":"observation","descriptor_id":"frontend.clang22.call_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:8b79a9fb3d59e750c51310d6f32935701a36c68fd5830228516482b0e7d2cd65"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.entity_observation.v2-batch","contract_digest":"sha256:4a5012801fcde26110a9f6350177d74d7d6975edde96337d4d3918ca7a004d51","dependency_group_id":"observation","descriptor_id":"frontend.clang22.entity_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:eb909eec97cec22586f4ac67dc7c56cc29390857df9355186feae5e9ce7700fb"},{"atomic_output_group_id":"clang22-atomic","batch_id":"frontend.clang22.type_observation.v2-batch","contract_digest":"sha256:53c54f967eb041e75ea98463c212d259fed0d3a310038ac9c93209749e72387f","dependency_group_id":"observation","descriptor_id":"frontend.clang22.type_observation.v2","descriptor_version":"2.0.0","output_stage":"assertion","runtime_descriptor_digest":"semantic-v2:sha256:94b6f6efcd46dad74c0cec1c761a2d363c6acdfe135862c37d0b7e28b01b6026"}],"path":"schemas/cxxlens_ng_relation_registry.yaml"},"request_digest":"semantic-v2:sha256:09a36429bc4dc0f74ef0bf23a6751837d8b0277c06392c9ac5e64c9dab66f95a","request_version":"2.0.0","schema":"cxxlens.clang22-materialization-request.v2","semantic_request_digest":"semantic-v2:sha256:7d79bd07fade21afe4701e0b55814701792b4b285ab823282e70e229c82e0bdd","tasks":[{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:be42bfee446b271dd490ce3477e4c2f74e8a6125a6f3ec8a03bbcbe349161e99","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://main.cpp"],"environment_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:dd5bdb2f9fd85376546c2f486a1ac3ebeed4bdb922351f3c2f4a7bf89be94acb","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:b7a7f77301033ac30084e3fa657eac3914344e6ab26f3fdc6b52272e10d4c0b3","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0000","source":{"content_base64":"aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg==","content_digest":"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196","encoding":"utf8","file_id":"file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06","line_index_id":"line-index:sha256:99cec457c4ced432a4db1dbb3c30bc291044469abf025737b306ccc7980a3510","logical_path":"project://main.cpp","read_only":false,"size_bytes":25,"source_snapshot_id":"source-snapshot:sha256:cb28d4d99af02e2bf0d1efc7288f211f595ef0a0caeaed889b66cb0fe995086d"},"task_input_digest":"sha256:39bd328764ad9f47c49e0efdfee4d232c410dfa79f096bbed16ea6ca02fd8056","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"},{"budget":{"address_space_bytes":1073741824,"cpu_ms":10000,"diagnostics":128,"open_files":64,"output_bytes":1048576,"rows":1024,"subprocesses":1,"transport_bytes":2097152,"wall_ms":10000},"build_variant_id":"build-variant:sha256:d0d2c433d8c558923be73e7655f2faa65ea94e330c9aa722d0e7d831d6907e01","catalog_digest":"semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","catalog_id":"catalog:semantic-v2:sha256:88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464","compile_unit_id":"compile-unit:sha256:3c5db06dbb85f42d2c2d89246ccf445078097e49250efb894a0270ad2b0cd553","condition_id":"condition:all","condition_universe_id":"condition-universe:one","dependency_groups":["canonical","observation"],"effective_argv":["clang++","-std=c++23","project://unit_1.cpp"],"environment_digest":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","interpretation_domain":"cc.clang22-canonical-1","language":"cxx","normalized_invocation_digest":"semantic-v2:sha256:68f779154ad8159b42f2ccc79b7e74999742e0c05a05563421e50d0cae028c09","project_id":"project:sha256:9b8cdb2a5afab245af006c61b1bbf0a758687ed969b42d349caf98bcdb6f01c3","provider_execution_id":"provider-execution:sha256:8bab585913137bb9106c76c4e46e5934aa07161cffc37784592af415fd7eb784","provider_task_id":"task:semantic-v2:sha256:5fb6b47f3aec5abedd658b2acd86f9a9e3af418712ed9fbf80e30cb3c7306118","requested_descriptor_ids":["cc.call_direct_target.v1","cc.call_site.v1","cc.entity.v1","frontend.clang22.call_observation.v2","frontend.clang22.entity_observation.v2","frontend.clang22.type_observation.v2"],"sandbox":{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"selected_catalog_compile_unit_id":"catalog-unit:0001","source":{"content_base64":"aW50IHVuaXRfMSgpIHsgcmV0dXJuIDE7IH0K","content_digest":"sha256:76af64be58a1f67608cb4c34771305ad773b173cc4cde76261d749928ad4ea49","encoding":"utf8","file_id":"file:sha256:c07309e8ee43ccbf4412cd0bbbb99099df12cd1e1a89ac84b7093308ac760b71","line_index_id":"line-index:sha256:5fe3bf322112a1740a8a1e95ed148bc1d8db4ad217b3574d6540c2de296da3a3","logical_path":"project://unit_1.cpp","read_only":false,"size_bytes":27,"source_snapshot_id":"source-snapshot:sha256:3663a8dd373452f9a641715395076673985bd5f2897e7b95983d0888464f9a93"},"task_input_digest":"sha256:6f097c492800c8a785ce8b69ff61ce8c106ab80b296be12c0ae6b92d3017b650","toolchain":{"abi_digest":"sha256:4444444444444444444444444444444444444444444444444444444444444444","builtin_headers_digest":"sha256:3333333333333333333333333333333333333333333333333333333333333333","exact_version":"22.0.0","family":"clang","plugin_spec_digest":"sha256:5555555555555555555555555555555555555555555555555555555555555555","sysroot":null,"target_triple":"x86_64-unknown-linux-gnu"},"toolchain_context_id":"toolchain-context:sha256:78f64803fb0f0f1ab7f10321ebc90aa52aafb99705407b92e005ff7d6ae82b9a","toolchain_digest":"semantic-v2:sha256:d84b82c787577126d2fbbc4e19f1608f77d1725216cf7647c6ace444d1917dbb","variant":{"include_search_digest":"sha256:7777777777777777777777777777777777777777777777777777777777777777","language":"cxx","language_standard":"cxx23","predefined_macros_digest":"sha256:6666666666666666666666666666666666666666666666666666666666666666","semantic_flags_digest":"sha256:8888888888888888888888888888888888888888888888888888888888888888","target_triple":"x86_64-unknown-linux-gnu"},"working_directory":"project://fixture"}],"tool":{"distribution_version":"1.0.0","executable":"cxxlens-clang22-materialize","installed_executable_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","interface_version":"2.0.0","package_configuration":"static","prefix_manifest_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","relocated_prefix_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","source_revision":"1111111111111111111111111111111111111111","source_tree":"2222222222222222222222222222222222222222"},"trust_policy":{"execution_profile":"trust.native-worker","policy_id":"cxxlens.clang22-installed-native-worker-trust.v1","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","required_qualification":"canonical-semantic-qualified","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","task_sandbox_requirements":[{"minimum":"enforced","policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}],"trust_policy_digest":"semantic-v2:sha256:a0b190b934d43470d18cbbf326601174fe8a23e52e825c904b8d265dc990d053","worker_sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},"worker":{"executable":"cxxlens-clang-worker-22","installed_binary_digest":"sha256:1111111111111111111111111111111111111111111111111111111111111111","protocol_major":1,"protocol_minor":0,"provider_id":"cxxlens.clang22.reference","provider_version":"1.0.0","sandbox_policy_digest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","semantic_contract_digest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})cxxlens_json";
	}

	[[nodiscard]] std::string expected_trust_digest()
	{
		using sdk::canonical_value;
		auto projection = canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.clang22-installed-native-worker-trust.v1"),
			canonical_value::from_string("trust.native-worker"),
			canonical_value::from_string("cxxlens.clang22.reference"),
			canonical_value::from_string("1.0.0"),
			canonical_value::from_string("sha256:" + std::string(64U, 'a')),
			canonical_value::from_integer(1),
			canonical_value::from_integer(1),
			canonical_value::from_tuple({canonical_value::from_string("task-input-chunks-v1")}),
			canonical_value::from_string("canonical-semantic-qualified"),
			canonical_value::from_string("sha256:" + std::string(64U, 'b')),
			canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string("enforced"),
				canonical_value::from_string("sha256:" + std::string(64U, 'b')),
			})}),
		});
		auto encoded = sdk::canonical_binary(projection);
		require(encoded.has_value(), "trust projection encoding failed");
		auto digest = sdk::semantic_digest(
			"cxxlens.clang22-installed-native-worker-trust.v1",
			std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		require(digest.has_value(), "trust projection digest failed");
		return std::move(*digest);
	}

	[[nodiscard]] const std::string& member_string(const json_value& value,
												   const std::string_view name)
	{
		const auto* member = value.member(name);
		require(member != nullptr && member->as_string() != nullptr,
				"fixture string member missing");
		return *member->as_string();
	}

	[[nodiscard]] std::string upgrade_fixture()
	{
		auto raw = fixture_v2_0();
		replace_once(raw, "\"request_version\":\"2.0.0\"", "\"request_version\":\"2.1.0\"");
		replace_once(raw, "\"interface_version\":\"2.0.0\"", "\"interface_version\":\"2.1.0\"");
		const auto digest = "sha256:" + std::string(64U, '1');
		replace_once(raw,
					 ",\"prefix_manifest_digest\":\"" + digest +
						 "\",\"relocated_prefix_digest\":\"" + digest + "\"",
					 ",\"occurrence_manifest_digest\":\"" + digest + "\"");
		replace_all(raw,
					"\"protocol_minor\":0",
					"\"protocol_minor\":1,\"required_features\":[\"task-input-chunks-v1\"]");

		json_limits limits;
		limits.max_input_bytes = raw.size();
		limits.max_string_bytes = raw.size();
		limits.max_total_string_bytes = raw.size();
		auto before = parse_json_object(raw, limits);
		require(before.has_value(), "upgraded fixture did not parse");
		const auto* trust = before->root().member("trust_policy");
		const auto* publication = before->root().member("publication");
		require(trust != nullptr && publication != nullptr, "fixture globals missing");
		const auto old_trust = member_string(*trust, "trust_policy_digest");
		const auto new_trust = expected_trust_digest();
		replace_all(raw, old_trust, new_trust);

		sdk::snapshot_series_selector selector{
			"catalog:semantic-v2:sha256:"
			"88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464",
			"channel:clang22-production",
			"engine-generation:sha256:"
			"984ec980908d8a3e3d14fb81b06e06009249e909bc7a6d323b447de825da08eb",
			"condition-universe:one",
			"semantic-v2:sha256:051823ea2f538bf38656afefb81d22950e5a6ca671fa4d57d89fffd8cfba171a",
			"semantic-v2:sha256:3e97b2cb497e80e0f59953844b4050930e3919f36ac3aab7403d391ab4cc087f",
			new_trust,
		};
		const auto old_series = member_string(*publication, "series_id");
		replace_once(raw, old_series, selector.id());

		auto unbound = scan(raw, 3U);
		auto identity = derive_streamed_materialization_request_identity(
			*unbound.spool, unbound.envelope, *unbound.index);
		require(identity.has_value(), "v2.1 streamed identity derivation failed");
		auto parsed = parse_json_object(raw, limits);
		require(parsed.has_value(), "identity fixture did not parse");
		replace_once(raw,
					 member_string(parsed->root(), "materialization_request_id"),
					 identity->materialization_request_id);
		replace_once(
			raw, member_string(parsed->root(), "request_digest"), identity->request_digest);
		replace_once(raw,
					 member_string(parsed->root(), "semantic_request_digest"),
					 identity->semantic_request_digest);
		return raw;
	}

	[[nodiscard]] std::string duplicate_second_task(std::string raw)
	{
		auto indexed = scan(raw, 13U);
		auto first = indexed.index->at(0U);
		auto second = indexed.index->at(1U);
		require(first && second, "duplicate-task fixture spans unavailable");
		const auto first_task = raw.substr(static_cast<std::size_t>(first->value_offset),
										   static_cast<std::size_t>(first->value_size_bytes));
		raw.replace(static_cast<std::size_t>(second->value_offset),
					static_cast<std::size_t>(second->value_size_bytes),
					first_task);
		return raw;
	}

	[[nodiscard]] std::uint64_t task_schema_payload_size(const std::string& raw,
														 const std::uint64_t task_number)
	{
		auto indexed = scan(raw, 31U);
		auto task = indexed.index->at(task_number);
		require(task.has_value(), "task payload span unavailable");
		auto metadata = replay_materialization_task_metadata(*indexed.spool, *task);
		require(metadata.has_value(), "task payload metadata replay failed");
		auto source = make_materialization_task_source_spool();
		require(source.has_value(), "task payload source spool failed");
		auto receipt = decode_materialization_task_source(*indexed.spool, *task, **source);
		require(receipt.has_value(), "task payload source decode failed");
		return 8U + canonical_json(metadata->root()).size() + receipt->size_bytes;
	}

	[[nodiscard]] std::string indexed_digest(const std::uint64_t value)
	{
		constexpr std::string_view digits{"0123456789abcdef"};
		std::string digest(64U, '0');
		auto remaining = value;
		for (std::size_t index{}; index < 16U; ++index)
		{
			digest[digest.size() - 1U - index] = digits[remaining & 0xfU];
			remaining >>= 4U;
		}
		return "sha256:" + digest;
	}

	void positive_two_task_and_short_read()
	{
		auto accepted = prevalidate(upgrade_fixture(), 1U);
		require(accepted.has_value(),
				accepted ? ""
						 : accepted.error().code + ":" + accepted.error().field + ":" +
						accepted.error().detail);
		require(accepted->task_count() == 2U && accepted->declared_source_bytes() == 52U,
				"two-task count or declared source census changed");
		require(accepted->tool().interface_version == "2.1.0" &&
					accepted->tool().occurrence_manifest_digest ==
						"sha256:" + std::string(64U, '1'),
				"tool 2.1 occurrence authority was not retained");
		require(accepted->worker().protocol_minor == 1U &&
					accepted->worker().required_features ==
						std::vector<std::string>{"task-input-chunks-v1"},
				"worker minor/features were not retained");
		auto first = accepted->task_metadata(0U);
		auto second = accepted->task_metadata(1U);
		require(first && second && first->selected_catalog_compile_unit_id == "catalog-unit:0000" &&
					second->selected_catalog_compile_unit_id == "catalog-unit:0001" &&
					first->condition_id == "condition:all" &&
					first->interpretation_domain == "cc.clang22-canonical-1",
				"spool-backed task metadata replay lost order or semantic context");
		require(!accepted->task_metadata(2U), "out-of-range metadata replay was accepted");
	}

	void selected_schema_shape_errors_follow_admission_phase()
	{
		std::vector<std::string> malformed;

		auto extra_root = upgrade_fixture();
		require(!extra_root.empty() && extra_root.back() == '}',
				"extra-root shape fixture is not an object");
		extra_root.insert(extra_root.size() - 1U, ",\"unexpected\":true");
		malformed.push_back(std::move(extra_root));

		auto missing_root = upgrade_fixture();
		const auto publication = missing_root.find(",\"publication\":");
		const auto registry = missing_root.find(",\"registry\":", publication);
		require(publication != std::string::npos && registry != std::string::npos,
				"missing-root shape fixture markers unavailable");
		missing_root.erase(publication, registry - publication);
		malformed.push_back(std::move(missing_root));

		auto missing_source = upgrade_fixture();
		const auto source = missing_source.find(",\"source\":{");
		const auto task_input = missing_source.find(",\"task_input_digest\":", source);
		require(source != std::string::npos && task_input != std::string::npos,
				"missing-source shape fixture markers unavailable");
		missing_source.erase(source, task_input - source);
		malformed.push_back(std::move(missing_source));

		auto missing_content = upgrade_fixture();
		const auto content = missing_content.find("\"content_base64\":");
		const auto content_digest = missing_content.find(",\"content_digest\":", content);
		require(content != std::string::npos && content_digest != std::string::npos,
				"missing-content shape fixture markers unavailable");
		missing_content.erase(content, content_digest + 1U - content);
		malformed.push_back(std::move(missing_content));

		for (const auto replacement : {std::string_view{"{}"}, std::string_view{"7"}})
		{
			auto non_string_content = upgrade_fixture();
			const auto member = non_string_content.find("\"content_base64\":");
			require(member != std::string::npos,
					"non-string-content shape fixture marker unavailable");
			const auto value_begin = member + std::string_view{"\"content_base64\":"}.size();
			require(value_begin < non_string_content.size() &&
						non_string_content[value_begin] == '"',
					"non-string-content shape fixture value is not a string");
			const auto value_end = non_string_content.find('"', value_begin + 1U);
			require(value_end != std::string::npos,
					"non-string-content shape fixture string is unterminated");
			non_string_content.replace(value_begin, value_end + 1U - value_begin, replacement);
			malformed.push_back(std::move(non_string_content));
		}

		for (auto& raw : malformed)
		{
			auto rejected = prevalidate(std::move(raw), 3U);
			require(!rejected && rejected.error().code == "materialization.request-invalid" &&
						rejected.error().field == "request-schema",
					"selected root/task/source/content shape escaped request-schema invalidity");
		}
	}

	void shared_catalog_owner_and_single_task_replay()
	{
		auto scanned = scan(upgrade_fixture(), 4096U);
		auto first_span = scanned.index->at(0U);
		require(first_span && first_span->source_content_size_bytes,
				"catalog-residency fixture lacks its first source span");
		const auto expected_metadata_read_bytes =
			first_span->value_size_bytes - *first_span->source_content_size_bytes + 2U;
		auto* observed_spool = scanned.spool.get();
		auto accepted = prevalidate_materialization_request_v2_1(
			std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
		require(accepted.has_value(),
				accepted ? ""
						 : accepted.error().code + ":" + accepted.error().field + ":" +
						accepted.error().detail);

		const auto* catalog_owner = std::addressof(accepted->catalog());
		const auto* compile_unit_owner = accepted->catalog().compile_units.data();
		const auto first_read_start = observed_spool->read_at_bytes_;
		auto first = accepted->task_metadata(0U);
		require(first &&
					observed_spool->read_at_bytes_ - first_read_start ==
						expected_metadata_read_bytes,
				"one task metadata receipt performed overlapping shape/binding replays");

		const auto second_read_start = observed_spool->read_at_bytes_;
		auto replayed = accepted->task_metadata(0U);
		require(replayed &&
					observed_spool->read_at_bytes_ - second_read_start ==
						expected_metadata_read_bytes &&
					replayed->task_index == first->task_index &&
					replayed->project_id == first->project_id &&
					replayed->selected_catalog_compile_unit_id ==
						first->selected_catalog_compile_unit_id &&
					replayed->source_content_digest == first->source_content_digest &&
					replayed->task_input_digest == first->task_input_digest,
				"replaying one metadata window changed its receipt or read it more than once");
		require(std::addressof(accepted->catalog()) == catalog_owner &&
					accepted->catalog().compile_units.data() == compile_unit_owner &&
					accepted->catalog().compile_units.size() == 2U &&
					observed_spool->read_at_calls_ != 0U,
				"task metadata replay moved, replaced, or mutated the shared catalog owner");
	}

	void source_dependent_production_admission()
	{
		auto accepted = validate(upgrade_fixture(), 1U);
		require(accepted.has_value(),
				accepted ? ""
						 : accepted.error().code + ":" + accepted.error().field + ":" +
						accepted.error().detail);
		require(accepted->request().task_count() == 2U &&
					accepted->request().declared_source_bytes() == 52U &&
					accepted->identity().materialization_request_id.starts_with(
						"materialization:semantic-v2:sha256:"),
				"source-dependent admission did not retain exact request authority");
		auto first = accepted->task_metadata(0U);
		require(first && first->task_input_digest.starts_with("sha256:") &&
					first->provider_execution_id.starts_with("provider-execution:"),
				"validated admission lost its task metadata receipt");

		auto source_drift = upgrade_fixture();
		replace_once(source_drift,
					 "aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg==",
					 "aW50IG1haW4oKSB7IHJldHVybiAxOyB9Cg==");
		auto source_rejected = validate(std::move(source_drift));
		require(!source_rejected && source_rejected.error().field == "task.source.content_digest",
				"sealed source receipt was not cross-bound before admission");

		auto task_input_drift = upgrade_fixture();
		replace_first_string_member(
			task_input_drift, "task_input_digest", "sha256:" + std::string(64U, '0'));
		auto task_input_rejected = validate(std::move(task_input_drift));
		require(!task_input_rejected &&
					task_input_rejected.error().field == "task.task_input_digest",
				"exact canonical task.v3 digest drift was not rejected");

		auto provider_task_drift = upgrade_fixture();
		replace_first_string_member(provider_task_drift,
									"provider_task_id",
									"task:semantic-v2:sha256:" + std::string(64U, '0'));
		auto provider_task_rejected = validate(std::move(provider_task_drift));
		require(!provider_task_rejected &&
					provider_task_rejected.error().field == "task.provider_task_id",
				"portable provider task identity drift was not rejected");

		auto execution_drift = upgrade_fixture();
		replace_first_string_member(execution_drift,
									"provider_execution_id",
									"provider-execution:sha256:" + std::string(64U, '0'));
		auto execution_rejected = validate(std::move(execution_drift));
		require(!execution_rejected &&
					execution_rejected.error().field == "task.provider_execution_id",
				"provider execution identity drift was not rejected");

		auto root_drift = upgrade_fixture();
		replace_first_string_member(
			root_drift, "semantic_request_digest", "semantic-v2:sha256:" + std::string(64U, '0'));
		auto root_rejected = validate(std::move(root_drift));
		require(!root_rejected && root_rejected.error().field == "request.semantic_request_digest",
				"root request identity drift was not rejected after task admission");

		auto schema_first = upgrade_fixture();
		replace_once(schema_first,
					 "aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg==",
					 "aW50IG1haW4oKSB7IHJldHVybiAwOyB9Ch==");
		replace_first_string_member(
			schema_first, "task_input_digest", "sha256:" + std::string(64U, '0'));
		auto schema_rejected = validate(std::move(schema_first));
		require(!schema_rejected && schema_rejected.error().field == "request-schema",
				"derived task binding preceded selected full-schema Base64 validation");
	}

	void protocol_catalog_and_source_metadata_negatives()
	{
		auto minor = upgrade_fixture();
		replace_all(minor, "\"protocol_minor\":1", "\"protocol_minor\":0");
		auto minor_rejected = prevalidate(std::move(minor));
		require(!minor_rejected && minor_rejected.error().field == "worker.protocol",
				"minor-0 request was not rejected");

		auto feature = upgrade_fixture();
		replace_all(feature, "task-input-chunks-v1", "unsupported-feature-v1");
		auto feature_rejected = prevalidate(std::move(feature));
		require(!feature_rejected && feature_rejected.error().field == "worker.protocol",
				"missing exact required feature was not rejected");

		auto source = upgrade_fixture();
		const auto first_digest =
			"sha256:deac66ccb79f6d31c0fa7d358de48e083c15c02ff50ec1ebd4b64314b9e6e196";
		replace_nth(source, first_digest, "sha256:" + std::string(64U, 'e'), 1U);
		auto source_rejected = prevalidate(std::move(source));
		require(!source_rejected &&
					source_rejected.error().field == "task.selected_catalog_compile_unit_id",
				"task/catalog source metadata drift was not rejected");

		auto census = upgrade_fixture();
		auto indexed = scan(census, 11U);
		auto first = indexed.index->at(0U);
		auto second = indexed.index->at(1U);
		require(first && second, "fixture task spans unavailable");
		const auto first_end = first->value_offset + first->value_size_bytes;
		const auto second_end = second->value_offset + second->value_size_bytes;
		require(first_end < second->value_offset && second_end <= census.size(),
				"fixture task spans invalid");
		census.erase(static_cast<std::size_t>(first_end),
					 static_cast<std::size_t>(second_end - first_end));
		auto census_rejected = prevalidate(std::move(census));
		require(!census_rejected &&
					census_rejected.error().code == "materialization.catalog-census-mismatch",
				"task/catalog cardinality mismatch was not rejected");
	}

	void schema_before_binding_and_version_dispatch()
	{
		auto raw = upgrade_fixture();
		replace_once(raw,
					 "file:sha256:83e065cbf0d8f742fe73a01155b02057c0de0fbe747f88b35ea5e96efe8faf06",
					 "file:sha256:" + std::string(64U, 'f'));
		replace_nth(raw, "\"rows\":1024", "\"rows\":0", 1U);
		auto rejected = prevalidate(std::move(raw));
		require(!rejected && rejected.error().field == "rows" &&
					rejected.error().detail == "positive",
				"later task schema failure did not precede earlier task binding drift");

		auto legacy = upgrade_fixture();
		replace_once(legacy, "\"request_version\":\"2.1.0\"", "\"request_version\":\"2.0.0\"");
		auto storage = make_materialization_private_spool();
		require(storage.has_value(), "legacy spool creation failed");
		require((*storage)->append(bytes(legacy)).has_value() && (*storage)->seal().has_value(),
				"legacy spool setup failed");
		auto index = make_materialization_request_task_index((*storage)->size_bytes());
		require(index.has_value(), "legacy index creation failed");
		auto envelope = scan_materialization_request_envelope(**storage, {19U}, index->get());
		require(!envelope, "legacy v2.0 DOM path remained production-adoptable");
	}

	void full_schema_and_external_uniqueness_adversarial()
	{
		const auto catalog_id = "catalog:semantic-v2:sha256:"
								"88dc78c51c338486857a2e282701263bda48781f08669378abaf41b80c9bc464";
		auto global_schema_first = upgrade_fixture();
		replace_once(global_schema_first, catalog_id, "catalog:schema-valid-but-unbound");
		replace_once(global_schema_first,
					 "\"publication\":{\"backend\"",
					 "\"publication\":{\"unexpected\":true,\"backend\"");
		auto global_rejected = prevalidate(std::move(global_schema_first));
		require(!global_rejected &&
					global_rejected.error().code == "materialization.request-invalid" &&
					global_rejected.error().field == "publication",
				"global derived catalog binding masked a later publication schema failure");

		auto malformed_catalog = upgrade_fixture();
		const auto catalog_member = "\"catalog_id\":\"" + std::string{catalog_id} + "\"";
		replace_nth(malformed_catalog, catalog_member, "\"catalog_id\":\"\"", 2U);
		auto catalog_rejected = prevalidate(std::move(malformed_catalog));
		require(!catalog_rejected &&
					catalog_rejected.error().code == "materialization.request-invalid" &&
					catalog_rejected.error().field == "task.catalog_id",
				"malformed task catalog authority leaked an identity/provider taxonomy");

		auto malformed_sandbox = upgrade_fixture();
		replace_once(malformed_sandbox,
					 "\"sandbox\":{\"minimum\":\"enforced\",\"policy_digest\":\"sha256:" +
						 std::string(64U, 'b') + "\"}",
					 "\"sandbox\":{\"minimum\":\"enforced\",\"policy_digest\":\"bad\"}");
		auto sandbox_rejected = prevalidate(std::move(malformed_sandbox));
		require(!sandbox_rejected &&
					sandbox_rejected.error().code == "materialization.request-invalid",
				"malformed task sandbox authority leaked an identity/provider taxonomy");

		auto duplicate_task = duplicate_second_task(upgrade_fixture());
		auto duplicate_rejected = prevalidate(std::move(duplicate_task));
		require(!duplicate_rejected &&
					duplicate_rejected.error().code == "materialization.request-invalid" &&
					duplicate_rejected.error().field == "tasks" &&
					duplicate_rejected.error().detail == "uniqueItems",
				"tasks uniqueItems was not proved exactly before task/catalog binding");
	}

	void canonical_base64_source_authority()
	{
		const auto decode_first_source = [](scanned_request& scanned)
			-> sdk::result<detail::clang22::clang22_task_source_receipt>
		{
			auto task = scanned.index->at(0U);
			if (!task)
				return sdk::unexpected(std::move(task.error()));
			auto source = make_materialization_task_source_spool();
			if (!source)
				return sdk::unexpected(std::move(source.error()));
			return decode_materialization_task_source(*scanned.spool, *task, **source);
		};

		const std::string_view first_source{"aW50IG1haW4oKSB7IHJldHVybiAwOyB9Cg=="};
		auto direct_raw = upgrade_fixture();
		auto direct = scan(direct_raw, 1U);
		auto direct_identity = derive_streamed_materialization_request_identity(
			*direct.spool, direct.envelope, *direct.index);
		auto direct_source = decode_first_source(direct);
		require(direct_identity && direct_source && direct_source->size_bytes == 25U,
				"canonical two-padding v2.1 source authority was rejected");

		auto escaped_raw = direct_raw;
		replace_once(escaped_raw,
					 '"' + std::string{first_source} + '"',
					 '"' + std::string{first_source.substr(0U, first_source.size() - 2U)} +
						 "\\u003d\\u003d\"");
		auto escaped = scan(escaped_raw, 1U);
		auto escaped_identity = derive_streamed_materialization_request_identity(
			*escaped.spool, escaped.envelope, *escaped.index);
		auto escaped_source = decode_first_source(escaped);
		require(escaped_identity && escaped_source && *escaped_identity == *direct_identity &&
					*escaped_source == *direct_source && prevalidate(std::move(escaped_raw), 1U),
				"raw JSON Base64 escaping changed v2.1 source/request identity");

		auto two_pad_alias_raw = direct_raw;
		replace_once(two_pad_alias_raw,
					 first_source,
					 std::string{first_source.substr(0U, first_source.size() - 3U)} + "h==");
		auto two_pad_alias = scan(two_pad_alias_raw, 1U);
		auto two_pad_identity = derive_streamed_materialization_request_identity(
			*two_pad_alias.spool, two_pad_alias.envelope, *two_pad_alias.index);
		auto two_pad_source = decode_first_source(two_pad_alias);
		require(two_pad_identity && *two_pad_identity != *direct_identity && !two_pad_source,
				"v2.1 accepted non-zero discarded bits in a two-padding Base64 spelling");

		auto one_pad_raw = direct_raw;
		replace_once(one_pad_raw, '"' + std::string{first_source} + '"', "\"YWI=\"");
		auto one_pad = scan(one_pad_raw, 1U);
		auto one_pad_identity = derive_streamed_materialization_request_identity(
			*one_pad.spool, one_pad.envelope, *one_pad.index);
		auto one_pad_source = decode_first_source(one_pad);
		require(one_pad_identity && one_pad_source && one_pad_source->size_bytes == 2U &&
					one_pad_source->content_digest == sdk::content_digest(bytes("ab")),
				"canonical one-padding v2.1 source authority was rejected");

		auto one_pad_alias_raw = direct_raw;
		replace_once(one_pad_alias_raw, '"' + std::string{first_source} + '"', "\"YWJ=\"");
		auto one_pad_alias = scan(one_pad_alias_raw, 1U);
		auto one_pad_alias_identity = derive_streamed_materialization_request_identity(
			*one_pad_alias.spool, one_pad_alias.envelope, *one_pad_alias.index);
		auto one_pad_alias_source = decode_first_source(one_pad_alias);
		require(one_pad_alias_identity && *one_pad_alias_identity != *one_pad_identity &&
					!one_pad_alias_source,
				"v2.1 accepted non-zero discarded bits in a one-padding Base64 spelling");
	}

	void bounded_requirements_split_schema_and_derived_binding()
	{
		const auto singleton =
			"[{\"minimum\":\"enforced\",\"policy_digest\":\"sha256:" + std::string(64U, 'b') +
			"\"}]";
		const auto with_requirement_count = [&](const std::uint64_t count)
		{
			auto raw = upgrade_fixture();
			std::string requirements{"["};
			for (std::uint64_t index{}; index < count; ++index)
			{
				if (index != 0U)
					requirements.push_back(',');
				requirements += "{\"minimum\":\"enforced\",\"policy_digest\":\"" +
					indexed_digest(index) + "\"}";
			}
			requirements.push_back(']');
			replace_once(raw, singleton, requirements);
			return raw;
		};

		auto at_limit = prevalidate(with_requirement_count(4096U), 97U);
		require(!at_limit && at_limit.error().code == "materialization.identity-mismatch" &&
					at_limit.error().field == "trust_policy.task_sandbox_requirements",
				"schema-valid 4096-item trust array did not reach derived binding");

		auto over_limit = prevalidate(with_requirement_count(4097U), 97U);
		require(!over_limit && over_limit.error().code == "materialization.request-invalid" &&
					over_limit.error().field == "request-schema",
				"4097-item trust array did not fail at selected-schema validation");
	}

	void compact_index_digest_collision_is_never_equality()
	{
		auto raw = upgrade_fixture();
		const auto left_size = task_schema_payload_size(raw, 0U);
		const auto right_size = task_schema_payload_size(raw, 1U);
		require(left_size != right_size, "collision fixture unexpectedly started equal-sized");
		const auto difference =
			left_size > right_size ? left_size - right_size : right_size - left_size;
		constexpr std::string_view directory{"project://fixture"};
		require(difference < directory.size() - std::string_view{"project://"}.size(),
				"collision fixture cannot be equalized with a valid project URI");
		const auto shortened = std::string{
			directory.substr(0U, directory.size() - static_cast<std::size_t>(difference))};
		const auto member = "\"working_directory\":\"" + std::string{directory} + "\"";
		const auto replacement = "\"working_directory\":\"" + shortened + "\"";
		replace_nth(raw, member, replacement, left_size > right_size ? 0U : 1U);
		require(task_schema_payload_size(raw, 0U) == task_schema_payload_size(raw, 1U),
				"collision fixture logical payload lengths remain unequal");

		forced_collision_factory task_collision{
			materialization_v2_1_auxiliary_spool_purpose::task_unique_index};
		auto scanned = scan(raw, 23U);
		auto task_result = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	std::move(scanned.envelope),
																	std::move(scanned.index),
																	task_collision);
		require(task_collision.collision_comparison_spools_ != 0U &&
					(task_result || task_result.error().detail != "uniqueItems"),
				"unequal equal-length digest collision was treated as JSON Schema equality");

		auto execution_request = prevalidate(upgrade_fixture(), 19U);
		require(execution_request.has_value(), "execution collision fixture prevalidation failed");
		auto first = execution_request->task_metadata(0U);
		auto second = execution_request->task_metadata(1U);
		require(first && second &&
					first->provider_task_id.size() + first->task_input_digest.size() +
							first->provider_execution_id.size() ==
						second->provider_task_id.size() + second->task_input_digest.size() +
							second->provider_execution_id.size(),
				"execution collision fixture payload lengths differ");
		forced_collision_factory execution_collision{
			materialization_v2_1_auxiliary_spool_purpose::execution_unique_index};
		auto admitted =
			admit_materialization_request_v2_1(std::move(*execution_request), execution_collision);
		require(admitted.has_value(),
				"unequal execution digest collision was treated as a duplicate tuple");
	}

	void auxiliary_spool_faults_are_phase_authentic()
	{
		constexpr std::array faults{auxiliary_fault::create,
									auxiliary_fault::append,
									auxiliary_fault::seal,
									auxiliary_fault::read};
		for (const auto fault : faults)
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index, fault};
			auto scanned = scan(upgrade_fixture(), 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-schema" &&
						rejected.error().detail.starts_with("task-unique-index:"),
					"task uniqueness spool fault escaped request-schema infrastructure taxonomy");
		}

		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index,
				auxiliary_fault::create_allocation};
			auto scanned = scan(upgrade_fixture(), 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"auxiliary allocation failure escaped the no-response boundary");
		}
		for (const auto fault :
			 {auxiliary_fault::create_invalid_configuration, auxiliary_fault::create_null})
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index, fault};
			auto scanned = scan(upgrade_fixture(), 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"auxiliary create contradiction escaped the no-response boundary");
		}
		constexpr std::array allocation_faults{
			auxiliary_fault::append_allocation,
			auxiliary_fault::seal_allocation,
			auxiliary_fault::read_allocation,
			auxiliary_fault::digest_update_allocation,
			auxiliary_fault::digest_finalize_allocation,
		};
		for (const auto fault : allocation_faults)
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index, fault};
			auto scanned = scan(upgrade_fixture(), 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"task uniqueness allocation fault escaped the no-response boundary");
		}
		constexpr std::array invalid_configuration_faults{
			auxiliary_fault::append_invalid_configuration,
			auxiliary_fault::seal_invalid_configuration,
			auxiliary_fault::read_invalid_configuration,
			auxiliary_fault::digest_update_invalid_configuration,
			auxiliary_fault::digest_finalize_invalid_configuration,
			auxiliary_fault::digest_finalize_malformed,
		};
		for (const auto fault : invalid_configuration_faults)
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index, fault};
			auto scanned = scan(upgrade_fixture(), 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"task uniqueness configuration drift escaped the no-response boundary");
		}

		for (const auto fault : faults)
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "execution fault fixture prevalidation failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(
				!rejected && rejected.error().code == "materialization.spool-failure" &&
					rejected.error().field == "request-binding" &&
					rejected.error().detail.starts_with("execution-unique-index:"),
				"execution uniqueness spool fault escaped request-binding infrastructure taxonomy");
		}
		for (const auto fault : allocation_faults)
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "execution allocation fixture prevalidation failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"execution uniqueness allocation fault escaped the no-response boundary");
		}
		for (const auto fault : invalid_configuration_faults)
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "execution configuration fixture prevalidation failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"execution uniqueness configuration drift escaped no-response");
		}
		for (const auto fault :
			 {auxiliary_fault::create_invalid_configuration, auxiliary_fault::create_null})
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "execution create contradiction fixture failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"execution create contradiction escaped the no-response boundary");
		}

		for (const auto fault : {auxiliary_fault::digest_update, auxiliary_fault::digest_finalize})
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "task-input hash failure fixture prevalidation failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_input_digest, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-binding" &&
						rejected.error().detail.starts_with("task-input:digest-"),
					"actual task-input hash failure lost request-binding taxonomy");
		}
		for (const auto fault : {auxiliary_fault::digest_update_allocation,
								 auxiliary_fault::digest_update_invalid_configuration,
								 auxiliary_fault::digest_finalize_allocation,
								 auxiliary_fault::digest_finalize_invalid_configuration,
								 auxiliary_fault::digest_finalize_malformed})
		{
			auto request = prevalidate(upgrade_fixture(), 29U);
			require(request.has_value(), "task-input private failure fixture prevalidation failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_input_digest, fault};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"task-input digest contradiction escaped the no-response boundary");
		}

		for (const auto fault : {auxiliary_fault::corrupt_record_size,
								 auxiliary_fault::corrupt_ordinal,
								 auxiliary_fault::corrupt_raw_span})
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_unique_index, fault};
			auto scanned = scan(upgrade_fixture(), 31U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"successful task-index corruption escaped the no-response boundary");
		}
		{
			auto request = prevalidate(upgrade_fixture(), 31U);
			require(request.has_value(), "execution raw-span corruption fixture failed");
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::execution_unique_index,
				auxiliary_fault::corrupt_raw_span};
			auto rejected = admit_materialization_request_v2_1(std::move(*request), factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"execution compact record accepted a nonzero raw-span field");
		}

		const auto duplicate = duplicate_second_task(upgrade_fixture());
		for (const auto fault : faults)
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata, fault};
			auto scanned = scan(duplicate, 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-schema" &&
						rejected.error().detail.starts_with("task-collision-metadata:"),
					"collision comparison spool fault escaped request-schema taxonomy");
		}
		for (const auto fault : {auxiliary_fault::append_allocation,
								 auxiliary_fault::seal_allocation,
								 auxiliary_fault::read_allocation})
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata, fault};
			auto scanned = scan(duplicate, 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"collision metadata allocation escaped the no-response boundary");
		}
		for (const auto fault : {auxiliary_fault::create_invalid_configuration,
								 auxiliary_fault::create_null,
								 auxiliary_fault::append_invalid_configuration,
								 auxiliary_fault::seal_invalid_configuration,
								 auxiliary_fault::read_invalid_configuration})
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata, fault};
			auto scanned = scan(duplicate, 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"collision metadata configuration drift escaped no-response");
		}
		for (const auto fault : {auxiliary_fault::append_success_drop,
								 auxiliary_fault::size_success_drift,
								 auxiliary_fault::read_success_zero,
								 auxiliary_fault::read_success_overreport,
								 auxiliary_fault::read_success_corrupt})
		{
			auxiliary_factory factory{
				materialization_v2_1_auxiliary_spool_purpose::task_collision_metadata, fault};
			auto scanned = scan(duplicate, 29U);
			auto rejected = prevalidate_materialization_request_v2_1(std::move(scanned.spool),
																	 std::move(scanned.envelope),
																	 std::move(scanned.index),
																	 factory);
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"successful collision reverse-read contradiction escaped no-response");
		}
	}

	void spool_read_failure_is_not_request_invalid()
	{
		{
			auto scanned = scan(upgrade_fixture(), 5U);
			scanned.spool->failure_offset_ = 0U;
			auto rejected = prevalidate_materialization_request_v2_1(
				std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"successful short raw replay escaped the no-response boundary");
		}
		{
			auto scanned = scan(upgrade_fixture(), 5U);
			scanned.spool->failure_offset_ = 0U;
			scanned.spool->failure_kind_ = materialization_io_failure_kind::spool;
			auto rejected = prevalidate_materialization_request_v2_1(
				std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-schema" &&
						rejected.error().detail.starts_with("raw-replay:"),
					"actual raw replay I/O fault escaped request-schema taxonomy");
		}
		{
			auto scanned = scan(upgrade_fixture(), 5U);
			scanned.spool->failure_offset_ = 0U;
			scanned.spool->failure_kind_ = materialization_io_failure_kind::allocation;
			auto rejected = prevalidate_materialization_request_v2_1(
				std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
			require(!rejected && is_materialization_admission_no_response(rejected.error()),
					"raw replay allocation escaped the no-response boundary");
		}
	}

	void task_index_and_binding_replay_failures_are_phase_authentic()
	{
		const auto scan_with_injected_index = [](const std::string_view raw)
		{
			auto raw_storage = make_materialization_private_spool();
			require(raw_storage.has_value(), "injected-index raw spool creation failed");
			auto spool = std::make_unique<fragmented_spool>(std::move(*raw_storage), 13U);
			require(spool->append(bytes(raw)).has_value() && spool->seal().has_value(),
					"injected-index raw spool setup failed");
			auto index_storage = make_materialization_private_spool();
			require(index_storage.has_value(), "injected-index storage creation failed");
			auto injected = std::make_unique<toggled_index_read_spool>(std::move(*index_storage));
			auto* control = injected.get();
			auto index =
				make_materialization_request_task_index(std::move(injected), spool->size_bytes());
			require(index.has_value(), "injected-index adapter creation failed");
			auto envelope = scan_materialization_request_envelope(*spool, {17U}, index->get());
			require(envelope.has_value(), "injected-index pass-one scan failed");
			return std::tuple{std::move(spool), std::move(*envelope), std::move(*index), control};
		};

		{
			auto [spool, envelope, index, control] = scan_with_injected_index(upgrade_fixture());
			control->fail_read_ = true;
			auto rejected = prevalidate_materialization_request_v2_1(
				std::move(spool), std::move(envelope), std::move(index));
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-schema" &&
						rejected.error().detail.starts_with("task-index:"),
					"task-index schema replay fault escaped request-schema taxonomy");
		}
		{
			auto [spool, envelope, index, control] = scan_with_injected_index(upgrade_fixture());
			auto request = prevalidate_materialization_request_v2_1(
				std::move(spool), std::move(envelope), std::move(index));
			require(request.has_value(), "task-index binding replay fixture prevalidation failed");
			control->fail_read_ = true;
			auto rejected = admit_materialization_request_v2_1(std::move(*request));
			require(!rejected && rejected.error().code == "materialization.spool-failure" &&
						rejected.error().field == "request-binding" &&
						rejected.error().detail.starts_with("task-index:"),
					"task-index binding replay fault escaped request-binding taxonomy");
		}
		constexpr std::array<std::optional<materialization_io_failure_kind>, 4U> failure_kinds{
			std::nullopt,
			materialization_io_failure_kind::allocation,
			materialization_io_failure_kind::invalid_configuration,
			materialization_io_failure_kind::spool,
		};
		for (const auto failure_kind : failure_kinds)
		{
			auto scanned = scan(upgrade_fixture(), 19U);
			auto first = scanned.index->at(0U);
			require(first.has_value(), "raw binding replay fixture task index failed");
			auto* control = scanned.spool.get();
			auto request = prevalidate_materialization_request_v2_1(
				std::move(scanned.spool), std::move(scanned.envelope), std::move(scanned.index));
			require(request.has_value(), "raw binding replay fixture prevalidation failed");
			control->failure_offset_ = first->value_offset;
			control->failure_kind_ = failure_kind;
			auto rejected = request->task_metadata(0U);
			if (failure_kind == materialization_io_failure_kind::spool)
			{
				require(!rejected && rejected.error().code == "materialization.spool-failure" &&
							rejected.error().field == "request-binding" &&
							rejected.error().detail.starts_with("raw-replay:"),
						"actual task binding replay I/O fault escaped request-binding taxonomy");
			}
			else
			{
				require(!rejected && is_materialization_admission_no_response(rejected.error()),
						"task binding replay corruption escaped the no-response boundary");
			}
		}
	}
} // namespace

static_assert(!std::copy_constructible<prevalidated_materialization_request_v2_1>);
static_assert(std::move_constructible<prevalidated_materialization_request_v2_1>);
static_assert(!std::copy_constructible<validated_materialization_request_v2_1>);
static_assert(std::move_constructible<validated_materialization_request_v2_1>);

int main()
{
	positive_two_task_and_short_read();
	selected_schema_shape_errors_follow_admission_phase();
	shared_catalog_owner_and_single_task_replay();
	source_dependent_production_admission();
	protocol_catalog_and_source_metadata_negatives();
	schema_before_binding_and_version_dispatch();
	full_schema_and_external_uniqueness_adversarial();
	canonical_base64_source_authority();
	bounded_requirements_split_schema_and_derived_binding();
	compact_index_digest_collision_is_never_equality();
	auxiliary_spool_faults_are_phase_authentic();
	spool_read_failure_is_not_request_invalid();
	task_index_and_binding_replay_failures_are_phase_authentic();
}
