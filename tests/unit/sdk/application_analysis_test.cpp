#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <new>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_declaration.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk/application_analysis.hpp>

#include "msvc_worker/msvc_capture_bundle.hpp"
#include "msvc_worker/msvc_response_arguments.hpp"
#include "msvc_worker/msvc_source_dependencies.hpp"
#include "sdk/application_materialization_execution_internal.hpp"
#include "sdk/detached_provider_run_adoption_internal.hpp"
#include "sdk/detached_provider_run_builder_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"

#if defined(CXXLENS_TEST_CLANG23_WORKER_PATH)
#include "runtime/detached_application_materialization_file_service_internal.hpp"
#include "sdk/openssl_detached_run_crypto_internal.hpp"
#endif

#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
namespace
{
	thread_local bool fail_next_allocation{};
} // namespace

void* operator new(const std::size_t size)
{
	if (fail_next_allocation)
	{
		fail_next_allocation = false;
		throw std::bad_alloc{};
	}
	if (auto* allocation = std::malloc(size == 0U ? 1U : size); allocation != nullptr)
		return allocation;
	throw std::bad_alloc{};
}

void* operator new[](const std::size_t size)
{
	return ::operator new(size);
}

void operator delete(void* allocation) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
	::operator delete(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
	std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
	::operator delete(allocation);
}
#endif

namespace
{
	using cxxlens::sdk::canonical_binary;
	using cxxlens::sdk::canonical_value;

	template <class value_type>
	void require(const value_type& condition,
				 const std::source_location location = std::source_location::current())
	{
		if (!static_cast<bool>(condition))
		{
			std::cerr << "require failed at " << location.file_name() << ':' << location.line()
					  << '\n';
			std::abort();
		}
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] canonical_value observed(canonical_value value)
	{
		return canonical_value::from_tuple({canonical_value::from_string("observed"),
											std::move(value),
											canonical_value::from_string({}),
											canonical_value::from_string({})});
	}

	[[nodiscard]] std::vector<std::byte> source_bytes()
	{
		const std::string source{"int main() { return 0; }\n"};
		std::vector<std::byte> output;
		output.reserve(source.size());
		for (const char byte : source)
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		return output;
	}

	[[nodiscard]] canonical_value derived(canonical_value value)
	{
		return canonical_value::from_tuple({canonical_value::from_string("derived"),
											std::move(value),
											canonical_value::from_string({}),
											canonical_value::from_string({})});
	}

	[[nodiscard]] canonical_value unavailable(std::string reason, std::string action)
	{
		return canonical_value::from_tuple({canonical_value::from_string("unavailable"),
											canonical_value::null(),
											canonical_value::from_string(std::move(reason)),
											canonical_value::from_string(std::move(action))});
	}

	[[nodiscard]] canonical_value gap(std::string field, std::string reason, std::string action)
	{
		return canonical_value::from_tuple({canonical_value::from_string(std::move(field)),
											canonical_value::from_string("unavailable"),
											canonical_value::from_string(std::move(reason)),
											canonical_value::from_string(std::move(action))});
	}

	void rebind_source_closure(canonical_value& bundle)
	{
		auto& closure = bundle.tuple[6].tuple[0];
		auto encoded_members = canonical_binary(closure.tuple[6]);
		require(encoded_members);
		closure.tuple[2] =
			canonical_value::from_string(cxxlens::sdk::content_digest(*encoded_members));
		const std::array fields{closure.tuple[2],
								closure.tuple[3],
								closure.tuple[4],
								closure.tuple[5],
								closure.tuple[7]};
		auto closure_digest =
			cxxlens::sdk::canonical_identity_digest("application-source-closure", fields);
		require(closure_digest);
		closure.tuple[1] = canonical_value::from_string(std::move(*closure_digest));
		closure.tuple[0] = canonical_value::from_string("source-closure:" + closure.tuple[1].text);
		bundle.tuple[5].tuple[0].tuple[15] = closure.tuple[0];
	}

	[[nodiscard]] canonical_value valid_bundle()
	{
		auto content = source_bytes();
		const auto source_digest = cxxlens::sdk::content_digest(content);
		const auto source_size = static_cast<std::int64_t>(content.size());
		const std::array file_fields{
			canonical_value::from_string("project"),
			canonical_value::from_string("src/main.cpp"),
			canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		auto source_file_id = cxxlens::sdk::canonical_identity_digest("file", file_fields);
		require(source_file_id);
		const std::array snapshot_fields{
			canonical_value::from_string(*source_file_id),
			canonical_value::from_string(source_digest),
			canonical_value::from_string("utf8"),
		};
		auto source_snapshot =
			cxxlens::sdk::canonical_identity_digest("source-snapshot", snapshot_fields);
		require(source_snapshot);
		auto toolchain = canonical_value::from_tuple({
			canonical_value::from_string("gcc"),
			canonical_value::from_string("16.2.0"),
			observed(canonical_value::from_string("/opt/gcc-16.2.0/bin/g++")),
			observed(canonical_value::from_string(digest('1'))),
			canonical_value::from_string("x86_64-linux-gnu"),
			unavailable("no-sysroot", "capture-effective-sysroot"),
			observed(canonical_value::from_string(digest('2'))),
			observed(canonical_value::from_string(digest('3'))),
			observed(canonical_value::from_string(digest('4'))),
			observed(canonical_value::from_string(digest('5'))),
		});
		auto environment = canonical_value::from_tuple({canonical_value::from_tuple({
			canonical_value::from_string("gcc.cpath"),
			derived(canonical_value::from_string("project://include")),
		})});
		auto unit = canonical_value::from_tuple({
			canonical_value::from_string("compile-unit:main"),
			observed(canonical_value::from_string(*source_snapshot)),
			canonical_value::from_string(*source_file_id),
			canonical_value::from_string("project://src/main.cpp"),
			canonical_value::from_string(source_digest),
			canonical_value::from_integer(source_size),
			canonical_value::from_string("project://build"),
			canonical_value::from_string("c++"),
			observed(canonical_value::from_tuple({
				canonical_value::from_string("/opt/gcc-16.2.0/bin/g++"),
				canonical_value::from_string("-std=gnu++23"),
				canonical_value::from_string("/workspace/example/src/main.cpp"),
			})),
			observed(canonical_value::from_tuple({})),
			unavailable("config-files-unobserved", "capture-config-files"),
			observed(std::move(environment)),
			observed(canonical_value::from_string("/workspace/example/build")),
			observed(canonical_value::from_string("gnu++23")),
			observed(canonical_value::from_string("gnu")),
			canonical_value::from_string("source-closure:pending"),
		});
		auto closure = canonical_value::from_tuple({
			canonical_value::from_string("source-closure:pending"),
			canonical_value::from_string(digest('7')),
			canonical_value::from_string(digest('8')),
			canonical_value::from_integer(1),
			canonical_value::from_integer(1),
			canonical_value::from_integer(source_size),
			canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string(*source_file_id),
				canonical_value::from_string("project://src/main.cpp"),
				observed(canonical_value::from_string(source_digest)),
				observed(canonical_value::from_bytes(std::move(content))),
				canonical_value::from_integer(source_size),
				observed(canonical_value::from_string("main")),
				observed(canonical_value::from_string("utf8")),
				canonical_value::from_boolean(true),
			})}),
			observed(canonical_value::from_string("complete")),
		});
		auto gaps = canonical_value::from_tuple({
			gap("compile_units[0].config_files", "config-files-unobserved", "capture-config-files"),
			gap("production_toolchain.sysroot", "no-sysroot", "capture-effective-sysroot"),
		});
		auto bundle = canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
			std::move(toolchain),
			canonical_value::from_string("shell-free-wrapper"),
			canonical_value::from_string("x86_64-linux-gnu"),
			canonical_value::from_string("project:gcc-example"),
			canonical_value::from_tuple({std::move(unit)}),
			canonical_value::from_tuple({std::move(closure)}),
			std::move(gaps),
			canonical_value::from_string("project://"),
			observed(canonical_value::from_tuple({canonical_value::from_tuple({
				canonical_value::from_string("/workspace/example"),
				canonical_value::from_string("project://"),
			})})),
		});
		rebind_source_closure(bundle);
		return bundle;
	}

	[[nodiscard]] canonical_value valid_two_unit_bundle()
	{
		auto bundle = valid_bundle();
		auto second = bundle.tuple[5].tuple.front();
		second.tuple[0] = canonical_value::from_string("compile-unit:secondary");
		second.tuple[8].tuple[1].tuple.push_back(
			canonical_value::from_string("-DSECONDARY_UNIT=1"));
		bundle.tuple[5].tuple.push_back(std::move(second));
		bundle.tuple[7].tuple.insert(bundle.tuple[7].tuple.begin() + 1,
									 gap("compile_units[1].config_files",
										 "config-files-unobserved",
										 "capture-config-files"));
		return bundle;
	}

	[[nodiscard]] canonical_value valid_msvc_bundle()
	{
		auto bundle = valid_bundle();
		bundle.tuple[1].tuple[0] = canonical_value::from_string("msvc");
		bundle.tuple[1].tuple[1] = canonical_value::from_string("19.51.36256");
		bundle.tuple[1].tuple[2] =
			observed(canonical_value::from_string("C:\\VS\\VC\\Tools\\MSVC\\14.51\\bin\\cl.exe"));
		bundle.tuple[1].tuple[4] = canonical_value::from_string("x86_64-pc-windows-msvc");
		bundle.tuple[2] = canonical_value::from_string("msbuild-cltool-proxy");
		bundle.tuple[3] = canonical_value::from_string("x86_64-pc-windows-msvc");
		bundle.tuple[4] = canonical_value::from_string("project:msvc-example");
		auto& unit = bundle.tuple[5].tuple[0];
		unit.tuple[8] = observed(canonical_value::from_tuple({
			canonical_value::from_string("C:\\VS\\VC\\Tools\\MSVC\\14.51\\bin\\cl.exe"),
			canonical_value::from_string("/nologo"),
			canonical_value::from_string("/std:c++latest"),
			canonical_value::from_string("/D"),
			canonical_value::from_string("SEPARATED_DEFINE=1"),
			canonical_value::from_string("/I"),
			canonical_value::from_string("C:\\workspace\\example\\include"),
			canonical_value::from_string("/IC:\\workspace\\example\\include"),
			canonical_value::from_string("C:\\workspace\\example\\src\\main.cpp"),
			canonical_value::from_string("/c"),
		}));
		unit.tuple[11] = observed(canonical_value::from_tuple({}));
		unit.tuple[12] = observed(canonical_value::from_string("C:\\workspace\\example\\build"));
		unit.tuple[13] = observed(canonical_value::from_string("c++latest"));
		unit.tuple[14] = observed(canonical_value::from_string("msvc"));
		bundle.tuple[9] = observed(canonical_value::from_tuple({canonical_value::from_tuple({
			canonical_value::from_string("C:\\workspace\\example"),
			canonical_value::from_string("project://"),
		})}));
		return bundle;
	}

	[[nodiscard]] cxxlens::sdk::relation_descriptor request_descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "test.application_analysis.v1";
		value.name = "test.application_analysis";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "test.application-analysis/1";
		value.owner_namespace = "test";
		value.columns = {{"test.application_analysis.v1.key",
						  "key",
						  {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
						  true,
						  cxxlens::sdk::column_role::claim_key}};
		value.key_columns = {value.columns.front().id};
		value.descriptor_digest =
			*cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										   value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] cxxlens::sdk::provider::provider_candidate
	application_provider_candidate(const cxxlens::sdk::relation_descriptor& descriptor,
								   const std::string& binary_digest,
								   const bool trust_valid = true)
	{
		using namespace cxxlens::sdk::provider;
		const auto policies = builtin_sandbox_policies();
		require(!policies.empty());
		manifest description;
		description.provider_id = "cxxlens.gcc-replay";
		description.provider_version = {1U, 0U, 0U};
		description.package_identity = "cxxlens.gcc-replay.package";
		description.publisher = "cxxlens";
		description.license = "Apache-2.0";
		description.signature = digest('7');
		description.protocol = {protocol_v2_major,
								protocol_v2_minor,
								protocol_v2_minor,
								{"credit-backpressure", "task-input-chunks-v2"},
								{}};
		description.platform_tuples = {"linux-glibc"};
		description.provider_binary_digest = binary_digest;
		description.provider_semantic_contract_digest = digest('b');
		description.offered_relations = {descriptor.id};
		description.interpretation_domains = {"cc.clang23-gcc-replay-1"};
		description.invalidation_contract = digest('f');
		description.determinism_contract = digest('9');
		description.resource_class = "provider.application-analysis";
		description.sandbox_minimum = "enforced";
		description.requested_qualifications = {"canonical-semantic-qualified"};
		return {std::move(description),
				discovery_source::explicit_path,
				{"/opt/cxxlens/bin/cxxlens-clang-gcc-replay-worker-23"},
				true,
				trust_valid,
				true,
				{"canonical-semantic-qualified"},
				{"linux-glibc",
				 policies.front().mechanisms,
				 sandbox_assurance::enforced,
				 policies.front().policy_digest(),
				 digest('8')},
				{}};
	}

	class detached_transcript_sink final : public cxxlens::sdk::provider::frame_sink
	{
	  public:
		cxxlens::sdk::result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	class detached_application_provider final : public cxxlens::sdk::provider::portable_provider
	{
	  public:
		explicit detached_application_provider(cxxlens::sdk::relation_descriptor descriptor)
			: descriptor_{std::move(descriptor)}
		{
		}

		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "cxxlens.gcc-replay";
		}
		[[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return contract_;
		}
		cxxlens::sdk::result<void> run(const cxxlens::sdk::provider::task& task,
									   cxxlens::sdk::provider::context& context) override
		{
			using namespace cxxlens::sdk;
			row_builder builder{descriptor_};
			if (auto set = builder.set({descriptor_.id,
										descriptor_.columns.front().id,
										descriptor_.columns.front().type,
										{}},
									   detached_cell::utf8("observed"));
				!set)
				return set;
			auto row = std::move(builder).finish();
			if (!row)
				return unexpected(std::move(row.error()));
			auto output = context.relation(descriptor_);
			if (auto begun = output.begin("clang23-gcc-replay",
										  "atomic:application-analysis",
										  "batch:application-analysis");
				!begun)
				return begun;
			if (auto pushed = output.push(*row); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("relation", descriptor_.id);
			if (auto covered = context.coverage().classify(
					{"relation", descriptor_.id, "covered", "frontend-observed"});
				!covered)
				return covered;
			context.coverage().request("task", task.task_id);
			if (auto covered = context.coverage().classify(
					{"task", task.task_id, "covered", "translation-unit-executed"});
				!covered)
				return covered;
			context.evidence().add({"application-analysis.replay",
									"compile-unit:main",
									"clang-23.1.0-gcc-mode",
									"detached-test"});
			return {};
		}

	  private:
		cxxlens::sdk::relation_descriptor descriptor_;
		std::string contract_{digest('b')};
	};

	void authenticate(cxxlens::sdk::detail::detached_provider_run_draft& value,
					  const std::string_view signer = "worker:clang23-gcc-replay")
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::detail;
		value.authentication.signer_id = signer;
		value.authentication.key_fingerprint = digest('6');
		for (std::size_t index{}; index < value.authentication.signature.size(); ++index)
			value.authentication.signature[index] = static_cast<std::byte>(index + 1U);
		value.authentication.signature_digest = content_digest(value.authentication.signature);
		auto subject = detached_provider_run_signed_subject_digest(value);
		require(subject);
		value.authentication.signed_subject_digest = std::move(*subject);
	}

	class deterministic_detached_run_signer final : public cxxlens::sdk::detail::detached_run_signer
	{
	  public:
		explicit deterministic_detached_run_signer(const bool fail = false) : fail_{fail} {}

		[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::detail::detached_run_signature>
		sign(const std::string_view scope,
			 const std::string_view signed_subject_digest) const override
		{
			using namespace cxxlens::sdk;
			using namespace cxxlens::sdk::detail;
			if (scope != "detached-provider-run" || !signed_subject_digest.starts_with("sha256:"))
				return unexpected(error{"test.detached-run-signer-invalid", "request", "binding"});
			if (fail_)
				return unexpected(
					error{"test.detached-run-signer-unavailable", "signer", "unavailable"});
			detached_run_signature output;
			output.signer_id = "worker:clang23-gcc-replay";
			output.key_fingerprint = digest('6');
			for (std::size_t index{}; index < output.signature.size(); ++index)
				output.signature[index] = static_cast<std::byte>(index + 1U);
			return output;
		}

	  private:
		bool fail_{};
	};

#if defined(CXXLENS_TEST_CLANG23_WORKER_PATH)
	[[nodiscard]] std::byte test_hex_nibble(const char value)
	{
		if (value >= '0' && value <= '9')
			return static_cast<std::byte>(value - '0');
		if (value >= 'a' && value <= 'f')
			return static_cast<std::byte>(value - 'a' + 10);
		require(false);
		return {};
	}

	template <std::size_t Size>
	[[nodiscard]] std::array<std::byte, Size> test_hex_bytes(const std::string_view hex)
	{
		require(hex.size() == Size * 2U);
		std::array<std::byte, Size> output{};
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] =
				(test_hex_nibble(hex[index * 2U]) << 4U) | test_hex_nibble(hex[index * 2U + 1U]);
		return output;
	}

	class test_ed25519_material_port final
		: public cxxlens::sdk::detail::detached_run_ed25519_signing_material_port
	{
	  public:
		test_ed25519_material_port()
		{
			value_.scope = "detached-provider-run";
			value_.signer_id = "worker:clang23-gcc-replay";
			value_.private_key = test_hex_bytes<32U>("9d61b19deffd5a60ba844af492ec2cc4"
													 "4449c5697b326919703bac031cae7f60");
			value_.public_key = test_hex_bytes<32U>("d75a980182b10ab7d54bfed3c964073a"
													"0ee172f3daa62325af021a68f707511a");
		}

		[[nodiscard]] cxxlens::sdk::result<
			cxxlens::sdk::detail::detached_run_ed25519_signing_material>
		load(const std::string_view, const std::string_view) const override
		{
			return value_;
		}

		[[nodiscard]] const std::array<std::byte, 32U>& public_key() const noexcept
		{
			return value_.public_key;
		}

	  private:
		cxxlens::sdk::detail::detached_run_ed25519_signing_material value_;
	};

	class detached_run_test_directory final
	{
	  public:
		detached_run_test_directory()
		{
			path_ = std::filesystem::temp_directory_path() /
				("cxxlens detached adoption " + std::to_string(std::rand()));
			std::error_code error;
			std::filesystem::create_directories(path_, error);
			require(!error);
		}

		~detached_run_test_directory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path_, ignored);
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	void write_binary(const std::filesystem::path& path, const std::span<const std::byte> bytes)
	{
		std::ofstream output{path, std::ios::binary | std::ios::trunc};
		output.write(reinterpret_cast<const char*>(bytes.data()),
					 static_cast<std::streamsize>(bytes.size()));
		require(static_cast<bool>(output));
	}
#endif

	class exact_detached_signature_verifier final
		: public cxxlens::sdk::detail::trusted_detached_run_signature_verifier
	{
	  public:
		explicit exact_detached_signature_verifier(
			const cxxlens::sdk::detail::validated_detached_provider_run& run,
			const cxxlens::sdk::detail::detached_run_signature_verdict verdict =
				cxxlens::sdk::detail::detached_run_signature_verdict::verified)
			: authentication_{run.value().authentication}, verdict_{verdict}
		{
		}

		[[nodiscard]] cxxlens::sdk::detail::detached_run_signature_verification
		verify(const std::string_view scope,
			   const std::string_view signer_id,
			   const std::string_view key_fingerprint,
			   const std::string_view signed_subject_digest,
			   const std::span<const std::byte> signature,
			   const std::string_view signature_digest) const override
		{
			using namespace cxxlens::sdk::detail;
			if (scope != "detached-provider-run" || signer_id != authentication_.signer_id ||
				key_fingerprint != authentication_.key_fingerprint ||
				signed_subject_digest != authentication_.signed_subject_digest ||
				signature_digest != authentication_.signature_digest ||
				!std::ranges::equal(signature, authentication_.signature))
			{
				detached_run_signature_verification rejected;
				rejected.verdict = detached_run_signature_verdict::rejected;
				return rejected;
			}
			return {verdict_,
					"test:detached-ed25519-verifier",
					digest('5'),
					std::string{signer_id},
					std::string{key_fingerprint},
					std::string{signed_subject_digest},
					std::string{signature_digest}};
		}

	  private:
		cxxlens::sdk::detail::detached_provider_run_authentication authentication_;
		cxxlens::sdk::detail::detached_run_signature_verdict verdict_;
	};

	class deterministic_multi_run_verifier final
		: public cxxlens::sdk::detail::trusted_detached_run_signature_verifier
	{
	  public:
		[[nodiscard]] cxxlens::sdk::detail::detached_run_signature_verification
		verify(const std::string_view scope,
			   const std::string_view signer_id,
			   const std::string_view key_fingerprint,
			   const std::string_view signed_subject_digest,
			   const std::span<const std::byte> signature,
			   const std::string_view signature_digest) const override
		{
			using namespace cxxlens::sdk;
			using namespace cxxlens::sdk::detail;
			std::array<std::byte, detached_provider_run_signature_bytes> expected{};
			for (std::size_t index{}; index < expected.size(); ++index)
				expected[index] = static_cast<std::byte>(index + 1U);
			if (scope != "detached-provider-run" || signer_id != "worker:clang23-gcc-replay" ||
				key_fingerprint != digest('6') || !signed_subject_digest.starts_with("sha256:") ||
				!std::ranges::equal(signature, expected) ||
				signature_digest != content_digest(expected))
			{
				detached_run_signature_verification rejected;
				rejected.verdict = detached_run_signature_verdict::rejected;
				return rejected;
			}
			return {detached_run_signature_verdict::verified,
					"test:detached-ed25519-verifier",
					digest('5'),
					std::string{signer_id},
					std::string{key_fingerprint},
					std::string{signed_subject_digest},
					std::string{signature_digest}};
		}
	};

	[[nodiscard]] bool has_reason(const std::span<const cxxlens::sdk::capture_gap> gaps,
								  const std::string_view reason)
	{
		return std::ranges::any_of(gaps,
								   [&](const auto& gap)
								   {
									   return gap.reason == reason;
								   });
	}

	void positive_decode_and_deterministic_import()
	{
		auto bytes = canonical_binary(valid_bundle());
		require(bytes);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*bytes);
		auto decoded_again = cxxlens::sdk::decode_capture_bundle(*bytes);
		require(decoded);
		require(decoded_again && decoded_again->digest() == decoded->digest());
		require(decoded->production_compiler() == "gcc-16.2.0");
		require(decoded->capture_adapter() == "shell-free-wrapper");
		require(decoded->target_abi() == "x86_64-linux-gnu");
		require(decoded->project_id() == "project:gcc-example");
		require(decoded->logical_project_root() == "project://");
		require(decoded->compile_unit_count() == 1U);
		require(decoded->gaps().size() == 2U);
		require(decoded->digest() == cxxlens::sdk::content_digest(*bytes));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		auto imported_again = cxxlens::sdk::import_capture(*decoded_again);
		require(imported && imported_again);
		require(imported->id() == imported_again->id());
		require(imported->capture_bundle_digest() == decoded->digest());
		require(!imported->catalog_semantic_digest().empty());
		require(imported->catalog_semantic_digest() == imported_again->catalog_semantic_digest());
		require(imported->replay_plans().size() == 1U);
		const auto& plan = imported->replay_plans().front();
		require(plan.digest() == imported_again->replay_plans().front().digest());
		require(plan.capture_bundle_digest() == decoded->digest());
		require(plan.compile_unit_id() == "compile-unit:main");
		require(plan.analysis_frontend() == "clang-23.1.0-gcc-mode");
		require(plan.target_abi() == "x86_64-linux-gnu");
		require(
			has_reason(plan.unresolved(), "analysis-frontend-differs-from-production-compiler"));
		require(
			has_reason(plan.unresolved(), "gcc-extension-fidelity-not-proved-for-clang-replay"));
		require(has_reason(plan.unresolved(), "gcc-environment-effect-not-replayed"));
		require(imported->unresolved().size() == plan.unresolved().size());
	}

	void replay_fidelity_and_import_bounds_fail_closed()
	{
		auto strict = valid_bundle();
		strict.tuple[5].tuple[0].tuple[8].tuple[1].tuple[1] =
			canonical_value::from_string("-std=c++23");
		strict.tuple[5].tuple[0].tuple[13] = observed(canonical_value::from_string("c++23"));
		strict.tuple[5].tuple[0].tuple[14] = observed(canonical_value::from_string("strict"));
		auto strict_bytes = canonical_binary(strict);
		require(strict_bytes);
		auto strict_bundle = cxxlens::sdk::decode_capture_bundle(*strict_bytes);
		require(strict_bundle);
		auto strict_import = cxxlens::sdk::import_capture(*strict_bundle);
		require(strict_import);
		require(!has_reason(strict_import->unresolved(),
							"gcc-extension-fidelity-not-proved-for-clang-replay"));

		auto unknown = valid_bundle();
		unknown.tuple[5].tuple[0].tuple[8].tuple[1].tuple.insert(
			unknown.tuple[5].tuple[0].tuple[8].tuple[1].tuple.begin() + 2,
			canonical_value::from_string("-fvendor-mode"));
		auto unknown_bytes = canonical_binary(unknown);
		require(unknown_bytes);
		auto unknown_bundle = cxxlens::sdk::decode_capture_bundle(*unknown_bytes);
		require(unknown_bundle);
		auto unknown_import = cxxlens::sdk::import_capture(*unknown_bundle);
		require(unknown_import &&
				has_reason(unknown_import->unresolved(), "gcc-option-not-classified"));

		cxxlens::sdk::import_limits limits;
		limits.maximum_arguments_per_unit = 2U;
		auto bounded = cxxlens::sdk::import_capture(*unknown_bundle, limits);
		require(!bounded && bounded.error().code == "application-analysis.import-limit-exceeded");

		auto missing_output = valid_bundle();
		missing_output.tuple[5].tuple[0].tuple[8].tuple[1].tuple.push_back(
			canonical_value::from_string("-o"));
		auto missing_output_bytes = canonical_binary(missing_output);
		require(missing_output_bytes);
		auto missing_output_bundle = cxxlens::sdk::decode_capture_bundle(*missing_output_bytes);
		require(missing_output_bundle);
		auto missing_output_import = cxxlens::sdk::import_capture(*missing_output_bundle);
		require(!missing_output_import &&
				missing_output_import.error().detail == "missing-output-path");

		auto empty_argv = valid_bundle();
		empty_argv.tuple[5].tuple[0].tuple[8] = observed(canonical_value::from_tuple({}));
		auto empty_argv_bytes = canonical_binary(empty_argv);
		require(empty_argv_bytes);
		auto empty_argv_bundle = cxxlens::sdk::decode_capture_bundle(*empty_argv_bytes);
		require(empty_argv_bundle);
		auto empty_argv_import = cxxlens::sdk::import_capture(*empty_argv_bundle);
		require(!empty_argv_import &&
				empty_argv_import.error().code == "application-analysis.target-unavailable");
	}

	void msvc_capture_import_preserves_replay_fidelity()
	{
		auto bytes = canonical_binary(valid_msvc_bundle());
		require(bytes);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*bytes);
		require(decoded);
		require(decoded->production_compiler() == "msvc-19.51.36256");
		require(decoded->capture_adapter() == "msbuild-cltool-proxy");
		require(decoded->target_abi() == "x86_64-pc-windows-msvc");
		auto imported = cxxlens::sdk::import_capture(*decoded);
		require(imported && imported->replay_plans().size() == 1U);
		const auto& plan = imported->replay_plans().front();
		require(plan.analysis_frontend() == "clang-cl-23.1.0");
		require(plan.target_abi() == "x86_64-pc-windows-msvc");
		require(
			has_reason(plan.unresolved(), "analysis-frontend-differs-from-production-compiler"));
		require(!has_reason(plan.unresolved(), "msvc-input-path-not-bound"));

		auto unsupported = valid_msvc_bundle();
		unsupported.tuple[5].tuple[0].tuple[8].tuple[1].tuple.insert(
			unsupported.tuple[5].tuple[0].tuple[8].tuple[1].tuple.begin() + 2,
			canonical_value::from_string("/vendorUnknown"));
		unsupported.tuple[5].tuple[0].tuple[8].tuple[1].tuple.insert(
			unsupported.tuple[5].tuple[0].tuple[8].tuple[1].tuple.begin() + 3,
			canonical_value::from_string("/Yucommon.hpp"));
		auto unsupported_bytes = canonical_binary(unsupported);
		require(unsupported_bytes);
		auto unsupported_bundle = cxxlens::sdk::decode_capture_bundle(*unsupported_bytes);
		require(unsupported_bundle);
		auto unsupported_import = cxxlens::sdk::import_capture(*unsupported_bundle);
		require(unsupported_import);
		require(has_reason(unsupported_import->unresolved(), "msvc-option-not-classified"));
		require(has_reason(unsupported_import->unresolved(),
						   "msvc-pch-or-module-input-not-replayable"));
	}

	void portable_msvc_encoder_round_trips_through_host_authority()
	{
		using namespace cxxlens::application_analysis_worker;
		const auto text_bytes = [](const std::string_view text)
		{
			std::vector<std::byte> output;
			output.reserve(text.size());
			for (const auto byte : text)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		};
		msvc_capture_input input;
		input.project_id = "project:msvc-portable-vector";
		input.canonical_project_root = "C:\\workspace\\unicode-project";
		input.canonical_working_directory = "C:\\workspace\\unicode-project\\build space";
		input.canonical_compiler_path =
			"C:\\VS\\VC\\Tools\\MSVC\\14.51.36231\\bin\\Hostx64\\x64\\cl.exe";
		input.compiler_binary_digest = digest('1');
		input.windows_sdk_root = "C:\\Program Files (x86)\\Windows Kits\\10";
		input.abi_digest = digest('2');
		input.builtin_headers_digest = digest('3');
		input.builtin_macros_digest = digest('4');
		input.include_search_digest = digest('5');
		input.original_arguments = {
			input.canonical_compiler_path,
			"@C:\\workspace\\unicode-project\\build space\\options.rsp",
			"C:\\workspace\\unicode-project\\src\\main.cpp",
			"/c",
		};
		input.main_source = {"C:\\workspace\\unicode-project\\src\\main.cpp",
							 text_bytes("#include <model.hpp>\nint main(){return model();}\n"),
							 "main",
							 "utf8"};
		input.dependency_sources = {{"C:\\workspace\\unicode-project\\include\\model.hpp",
									 text_bytes("inline int model(){return 0;}\n"),
									 "header",
									 "utf8"}};
		input.response_files = {{"C:\\workspace\\unicode-project\\build space\\options.rsp",
								 text_bytes("/std:c++latest /DUNICODE=1"),
								 std::nullopt}};

		auto encoded = encode_msvc_capture_bundle(input);
		auto encoded_again = encode_msvc_capture_bundle(input);
		require(encoded && encoded_again && *encoded == *encoded_again);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*encoded);
		require(decoded && decoded->digest() == cxxlens::sdk::content_digest(*encoded));
		auto imported = cxxlens::sdk::import_capture(*decoded);
		require(imported && imported->replay_plans().size() == 1U);
		require(imported->replay_plans().front().analysis_frontend() == "clang-cl-23.1.0");

		auto partial = input;
		partial.source_closure_membership =
			unavailable_capture_field{"dependency-member-outside-project-root",
									  "recapture-with-a-qualified-logical-read-root"};
		auto partial_encoded = encode_msvc_capture_bundle(partial);
		require(partial_encoded);
		auto partial_decoded = cxxlens::sdk::decode_capture_bundle(*partial_encoded);
		require(partial_decoded &&
				has_reason(partial_decoded->gaps(), "dependency-member-outside-project-root"));
		auto partial_imported = cxxlens::sdk::import_capture(*partial_decoded);
		require(
			partial_imported &&
			has_reason(partial_imported->unresolved(), "dependency-member-outside-project-root"));

		auto outside = input;
		outside.main_source.canonical_path = "D:\\foreign\\main.cpp";
		auto rejected = encode_msvc_capture_bundle(outside);
		require(!rejected && rejected.error().code == "application-analysis.msvc-capture-invalid");

		auto bounded = input;
		msvc_capture_limits limits;
		limits.maximum_arguments = 2U;
		auto limited = encode_msvc_capture_bundle(bounded, limits);
		require(!limited &&
				limited.error().code == "application-analysis.msvc-capture-limit-exceeded");
		auto oversized_token = input;
		oversized_token.original_arguments.push_back(std::string(4097U, 'x'));
		auto token_rejected = encode_msvc_capture_bundle(oversized_token);
		require(!token_rejected && token_rejected.error().field == "original_arguments");
		auto embedded_nul = input;
		embedded_nul.original_arguments.push_back(std::string{"/DVALUE=ok\0hidden", 17U});
		auto nul_rejected = encode_msvc_capture_bundle(embedded_nul);
		require(!nul_rejected && nul_rejected.error().field == "original_arguments");
	}

	void msvc_source_dependencies_are_bounded_and_canonical()
	{
		using cxxlens::application_analysis_worker::decode_msvc_source_dependencies;
		constexpr std::string_view document = R"json({
  "Version": "1.2",
  "Data": {
    "Source": "C:\\workspace\\src\\main.cpp",
    "ProvidedModule": "",
    "ImportedModules": [],
    "ImportedHeaderUnits": [],
    "Includes": [
      "C:\\workspace\\include\\z.hpp",
      "C:\\workspace\\include\\a.hpp"
    ]
  }
})json";
		auto decoded = decode_msvc_source_dependencies(document);
		require(decoded && decoded->source == "C:\\workspace\\src\\main.cpp" &&
				decoded->includes ==
					std::vector<std::string>{"C:\\workspace\\include\\a.hpp",
											 "C:\\workspace\\include\\z.hpp"});

		auto unsupported = decode_msvc_source_dependencies(
			R"json({"Version":"2.0","Data":{"Source":"x","Includes":[]}})json");
		require(!unsupported &&
				unsupported.error().code ==
					"application-analysis.msvc-source-dependencies-invalid");
		auto duplicate = decode_msvc_source_dependencies(
			R"json({"Version":"1.1","Data":{"Source":"x","Includes":["a","a"]}})json");
		require(!duplicate && duplicate.error().detail == "duplicate");
		auto bounded = decode_msvc_source_dependencies(document, 2U, 4096U);
		require(!bounded &&
				bounded.error().code ==
					"application-analysis.msvc-source-dependencies-limit-exceeded");
		auto malformed = decode_msvc_source_dependencies(
			R"json({"Version":"1.2","Data":{"Source":"x","Includes":[1]}})json");
		require(!malformed && malformed.error().field == "Data.Includes[0]");
	}

	void msvc_response_tokenization_is_shared_and_bounded()
	{
		using namespace cxxlens::application_analysis_worker;
		const auto bytes = [](const std::string_view text)
		{
			std::vector<std::byte> output;
			output.reserve(text.size());
			for (const auto byte : text)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		};

		auto parsed = parse_msvc_response_arguments(
			bytes(R"rsp(/DNAME="a b" /I"C:\Program Files\SDK" source.cpp)rsp"), 4U);
		require(
			parsed &&
			*parsed ==
				std::vector<std::string>{"/DNAME=a b", "/IC:\\Program Files\\SDK", "source.cpp"});
		auto bounded = parse_msvc_response_arguments(bytes("one two"), 1U);
		require(!bounded && bounded.error() == msvc_response_parse_failure::argument_count);
		auto unterminated = parse_msvc_response_arguments(bytes("\"open"), 4U);
		require(!unterminated &&
				unterminated.error() == msvc_response_parse_failure::unterminated_quote);
		auto nul = bytes("valid");
		nul.push_back(std::byte{});
		auto embedded_nul = parse_msvc_response_arguments(nul, 4U);
		require(!embedded_nul && embedded_nul.error() == msvc_response_parse_failure::embedded_nul);

		std::vector<std::byte> utf16le{std::byte{0xffU}, std::byte{0xfeU}};
		for (const auto unit : std::u16string_view{u"/DUNICODE=\u65e5\u672c source.cpp"})
		{
			utf16le.push_back(static_cast<std::byte>(unit & 0xffU));
			utf16le.push_back(static_cast<std::byte>(unit >> 8U));
		}
		auto unicode = parse_msvc_response_arguments(utf16le, 4U);
		require(unicode &&
				*unicode == std::vector<std::string>{"/DUNICODE=\u65e5\u672c", "source.cpp"});
		utf16le.pop_back();
		auto malformed_utf16 = parse_msvc_response_arguments(utf16le, 4U);
		require(!malformed_utf16 &&
				malformed_utf16.error() == msvc_response_parse_failure::invalid_encoding);
		const std::vector<std::byte> lone_high_surrogate{
			std::byte{0xffU}, std::byte{0xfeU}, std::byte{0x00U}, std::byte{0xd8U}};
		auto malformed_surrogate = parse_msvc_response_arguments(lone_high_surrogate, 4U);
		require(!malformed_surrogate &&
				malformed_surrogate.error() == msvc_response_parse_failure::invalid_encoding);
	}

	void allocation_failures_are_typed_at_external_boundaries()
	{
#if !defined(CXXLENS_TSAN_ALLOCATION_FAULT_TESTS_DISABLED)
		auto bytes = canonical_binary(valid_bundle());
		require(bytes);
		auto admitted = cxxlens::sdk::decode_capture_bundle(*bytes);
		require(admitted);

		fail_next_allocation = true;
		auto decode_failure = cxxlens::sdk::decode_capture_bundle(*bytes);
		const auto decode_fault_was_injected = !fail_next_allocation;
		fail_next_allocation = false;
		require(decode_fault_was_injected && !decode_failure &&
				decode_failure.error().code == "application-analysis.import-limit-exceeded" &&
				decode_failure.error().field == "bundle" &&
				decode_failure.error().detail == "allocation");

		fail_next_allocation = true;
		auto import_failure = cxxlens::sdk::import_capture(*admitted);
		const auto import_fault_was_injected = !fail_next_allocation;
		fail_next_allocation = false;
		require(import_fault_was_injected && !import_failure &&
				import_failure.error().code == "application-analysis.import-limit-exceeded" &&
				import_failure.error().field == "replay_plan" &&
				import_failure.error().detail == "allocation");
#endif
	}

	void toolchain_observation_gaps_are_preserved()
	{
		auto missing = valid_bundle();
		missing.tuple[1].tuple[7] =
			unavailable("builtin-headers-unobserved", "capture-builtin-header-identity");
		missing.tuple[7].tuple.insert(missing.tuple[7].tuple.end() - 1,
									  gap("production_toolchain.builtin_headers_digest",
										  "builtin-headers-unobserved",
										  "capture-builtin-header-identity"));
		auto missing_bytes = canonical_binary(missing);
		require(missing_bytes);
		auto missing_result = cxxlens::sdk::decode_capture_bundle(*missing_bytes);
		require(missing_result && has_reason(missing_result->gaps(), "builtin-headers-unobserved"));

		auto direct_value = valid_bundle();
		direct_value.tuple[1].tuple[6] = canonical_value::from_string(digest('2'));
		auto direct_bytes = canonical_binary(direct_value);
		require(direct_bytes);
		auto direct_result = cxxlens::sdk::decode_capture_bundle(*direct_bytes);
		require(!direct_result &&
				direct_result.error().field == "production_toolchain.abi_digest" &&
				direct_result.error().detail == "tuple-shape");

		auto malformed = valid_bundle();
		malformed.tuple[1].tuple[9] = observed(canonical_value::from_string("sha256:not-a-digest"));
		auto malformed_bytes = canonical_binary(malformed);
		require(malformed_bytes);
		auto malformed_result = cxxlens::sdk::decode_capture_bundle(*malformed_bytes);
		require(!malformed_result &&
				malformed_result.error().field == "production_toolchain.include_search_digest" &&
				malformed_result.error().detail == "digest");
	}

	void duplicate_source_variants_bind_one_closure()
	{
		auto bundle = valid_bundle();
		auto second = bundle.tuple[5].tuple[0];
		second.tuple[0] = canonical_value::from_string("compile-unit:variant-two");
		bundle.tuple[5].tuple.push_back(std::move(second));
		bundle.tuple[7].tuple.insert(bundle.tuple[7].tuple.begin() + 1,
									 gap("compile_units[1].config_files",
										 "config-files-unobserved",
										 "capture-config-files"));
		auto bytes = canonical_binary(bundle);
		require(bytes);
		auto decoded = cxxlens::sdk::decode_capture_bundle(*bytes);
		require(decoded && decoded->compile_unit_count() == 2U);
		auto imported = cxxlens::sdk::import_capture(*decoded);
		require(imported && imported->replay_plans().size() == 2U);
		require(std::ranges::none_of(imported->replay_plans()[0].unresolved(),
									 [](const auto& value)
									 {
										 return value.field.starts_with("compile_units[1].");
									 }));
		require(std::ranges::none_of(imported->replay_plans()[1].unresolved(),
									 [](const auto& value)
									 {
										 return value.field.starts_with("compile_units[0].");
									 }));

		auto missing_closure = std::move(bundle);
		missing_closure.tuple[5].tuple[1].tuple[15] =
			canonical_value::from_string("source-closure:missing");
		auto missing_bytes = canonical_binary(missing_closure);
		require(missing_bytes);
		auto missing = cxxlens::sdk::decode_capture_bundle(*missing_bytes);
		require(!missing && missing.error().detail == "reference-mismatch");
	}

	void resource_and_shape_fail_closed()
	{
		auto bytes = canonical_binary(valid_bundle());
		require(bytes);
		cxxlens::sdk::import_limits limits;
		limits.maximum_bundle_bytes = bytes->size() - 1U;
		auto oversized = cxxlens::sdk::decode_capture_bundle(*bytes, limits);
		require(!oversized &&
				oversized.error().code == "application-analysis.import-limit-exceeded");

		auto mismatched = valid_bundle();
		mismatched.tuple[2] = canonical_value::from_string("msbuild-cltool-proxy");
		auto mismatch_bytes = canonical_binary(mismatched);
		require(mismatch_bytes);
		auto mismatch = cxxlens::sdk::decode_capture_bundle(*mismatch_bytes);
		require(!mismatch && mismatch.error().detail == "toolchain-mismatch");

		auto missing_gap = valid_bundle();
		missing_gap.tuple[7].tuple.pop_back();
		auto missing_gap_bytes = canonical_binary(missing_gap);
		require(missing_gap_bytes);
		auto gap_result = cxxlens::sdk::decode_capture_bundle(*missing_gap_bytes);
		require(!gap_result && gap_result.error().detail == "census-mismatch");

		auto partial_source = valid_bundle();
		partial_source.tuple[6].tuple[0].tuple[4] = canonical_value::from_integer(0);
		partial_source.tuple[6].tuple[0].tuple[5] = canonical_value::from_integer(0);
		partial_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[3] =
			unavailable("source-bytes-unavailable", "recapture-source-closure");
		partial_source.tuple[7].tuple.push_back(gap("source_closures[0].members[0].content",
													"source-bytes-unavailable",
													"recapture-source-closure"));
		rebind_source_closure(partial_source);
		auto partial_source_bytes = canonical_binary(partial_source);
		require(partial_source_bytes);
		auto partial_source_result = cxxlens::sdk::decode_capture_bundle(*partial_source_bytes);
		require(partial_source_result && partial_source_result->gaps().size() == 3U);

		auto unobserved_membership = valid_bundle();
		unobserved_membership.tuple[6].tuple[0].tuple[7] =
			unavailable("dependency-output-unobserved",
						"recapture-with-shell-free-wrapper-or-run-dependency-probe");
		unobserved_membership.tuple[7].tuple.push_back(
			gap("source_closures[0].membership_coverage",
				"dependency-output-unobserved",
				"recapture-with-shell-free-wrapper-or-run-dependency-probe"));
		rebind_source_closure(unobserved_membership);
		auto unobserved_membership_bytes = canonical_binary(unobserved_membership);
		require(unobserved_membership_bytes);
		auto unobserved_membership_result =
			cxxlens::sdk::decode_capture_bundle(*unobserved_membership_bytes);
		require(unobserved_membership_result &&
				has_reason(unobserved_membership_result->gaps(), "dependency-output-unobserved"));

		auto invalid_membership = valid_bundle();
		invalid_membership.tuple[6].tuple[0].tuple[7] =
			observed(canonical_value::from_string("partial"));
		rebind_source_closure(invalid_membership);
		auto invalid_membership_bytes = canonical_binary(invalid_membership);
		require(invalid_membership_bytes);
		auto invalid_membership_result =
			cxxlens::sdk::decode_capture_bundle(*invalid_membership_bytes);
		require(!invalid_membership_result &&
				invalid_membership_result.error().field ==
					"source_closures[0].membership_coverage" &&
				invalid_membership_result.error().detail == "enum");

		auto direct_membership = valid_bundle();
		direct_membership.tuple[6].tuple[0].tuple[7] = canonical_value::from_string("complete");
		auto direct_membership_bytes = canonical_binary(direct_membership);
		require(direct_membership_bytes);
		auto direct_membership_result =
			cxxlens::sdk::decode_capture_bundle(*direct_membership_bytes);
		require(!direct_membership_result &&
				direct_membership_result.error().field ==
					"source_closures[0].membership_coverage" &&
				direct_membership_result.error().detail == "tuple-shape");

		auto forged_blob_census = valid_bundle();
		forged_blob_census.tuple[6].tuple[0].tuple[4] = canonical_value::from_integer(0);
		auto forged_blob_census_bytes = canonical_binary(forged_blob_census);
		require(forged_blob_census_bytes);
		auto forged_blob_census_result =
			cxxlens::sdk::decode_capture_bundle(*forged_blob_census_bytes);
		require(!forged_blob_census_result &&
				forged_blob_census_result.error().detail == "blob-census-mismatch");

		auto tampered_source = valid_bundle();
		tampered_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[3] =
			observed(canonical_value::from_bytes({std::byte{0x78}}));
		auto tampered_source_bytes = canonical_binary(tampered_source);
		require(tampered_source_bytes);
		auto tampered_source_result = cxxlens::sdk::decode_capture_bundle(*tampered_source_bytes);
		require(!tampered_source_result &&
				tampered_source_result.error().detail == "digest-or-size-mismatch");

		auto missing_source = valid_bundle();
		missing_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[0] =
			canonical_value::from_string("source-file:other");
		rebind_source_closure(missing_source);
		auto missing_source_bytes = canonical_binary(missing_source);
		require(missing_source_bytes);
		auto missing_source_result = cxxlens::sdk::decode_capture_bundle(*missing_source_bytes);
		require(!missing_source_result &&
				missing_source_result.error().detail == "binding-mismatch");

		auto forged_snapshot = valid_bundle();
		forged_snapshot.tuple[5].tuple[0].tuple[1] =
			observed(canonical_value::from_string("source-snapshot:forged"));
		auto forged_snapshot_bytes = canonical_binary(forged_snapshot);
		require(forged_snapshot_bytes);
		auto forged_snapshot_result = cxxlens::sdk::decode_capture_bundle(*forged_snapshot_bytes);
		require(!forged_snapshot_result &&
				forged_snapshot_result.error().detail == "source-snapshot-mismatch");

		auto non_main_source = valid_bundle();
		non_main_source.tuple[6].tuple[0].tuple[6].tuple[0].tuple[5] =
			observed(canonical_value::from_string("header"));
		rebind_source_closure(non_main_source);
		auto non_main_bytes = canonical_binary(non_main_source);
		require(non_main_bytes);
		auto non_main_result = cxxlens::sdk::decode_capture_bundle(*non_main_bytes);
		require(!non_main_result &&
				non_main_result.error().detail == "compile-unit-source-mismatch");

		auto forged_closure_id = valid_bundle();
		forged_closure_id.tuple[6].tuple[0].tuple[0] =
			canonical_value::from_string("source-closure:forged");
		forged_closure_id.tuple[5].tuple[0].tuple[15] =
			canonical_value::from_string("source-closure:forged");
		auto forged_closure_bytes = canonical_binary(forged_closure_id);
		require(forged_closure_bytes);
		auto forged_closure_result = cxxlens::sdk::decode_capture_bundle(*forged_closure_bytes);
		require(!forged_closure_result &&
				forged_closure_result.error().detail == "binding-mismatch");

		auto raw_environment_name = valid_bundle();
		raw_environment_name.tuple[5].tuple[0].tuple[11].tuple[1].tuple[0].tuple[0] =
			canonical_value::from_string("CPATH");
		auto raw_environment_bytes = canonical_binary(raw_environment_name);
		require(raw_environment_bytes);
		auto raw_environment_result = cxxlens::sdk::decode_capture_bundle(*raw_environment_bytes);
		require(!raw_environment_result && raw_environment_result.error().field.ends_with(".name"));

		auto recursive = valid_bundle();
		recursive.tuple[5].tuple[0].tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({
				canonical_value::from_string("project://build/options.rsp"),
				observed(canonical_value::from_string(digest('9'))),
				canonical_value::from_integer(12),
				canonical_value::from_integer(0),
			}),
		}));
		auto recursive_bytes = canonical_binary(recursive);
		require(recursive_bytes);
		auto recursive_result = cxxlens::sdk::decode_capture_bundle(*recursive_bytes);
		require(!recursive_result && recursive_result.error().detail == "recursive-reference");

		auto unpinned = valid_bundle();
		unpinned.tuple[1].tuple[1] = canonical_value::from_string("16.3.0");
		auto unpinned_bytes = canonical_binary(unpinned);
		require(unpinned_bytes);
		auto unpinned_result = cxxlens::sdk::decode_capture_bundle(*unpinned_bytes);
		require(!unpinned_result && unpinned_result.error().detail == "not-pinned");

		auto relative_compiler = valid_bundle();
		relative_compiler.tuple[1].tuple[2] =
			observed(canonical_value::from_string("toolchain/bin/g++"));
		auto relative_compiler_bytes = canonical_binary(relative_compiler);
		require(relative_compiler_bytes);
		auto relative_compiler_result =
			cxxlens::sdk::decode_capture_bundle(*relative_compiler_bytes);
		require(!relative_compiler_result &&
				relative_compiler_result.error().detail == "not-canonical-absolute");

		auto unmapped_working_directory = valid_bundle();
		unmapped_working_directory.tuple[5].tuple[0].tuple[12] =
			observed(canonical_value::from_string("/another/build"));
		auto unmapped_bytes = canonical_binary(unmapped_working_directory);
		require(unmapped_bytes);
		auto unmapped_result = cxxlens::sdk::decode_capture_bundle(*unmapped_bytes);
		require(!unmapped_result && unmapped_result.error().detail == "unmapped-physical-path");

		auto overlapping_mapping = valid_bundle();
		overlapping_mapping.tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({canonical_value::from_string("/workspace"),
										 canonical_value::from_string("project://")}),
			canonical_value::from_tuple({canonical_value::from_string("/workspace/example"),
										 canonical_value::from_string("project://src")}),
		}));
		auto overlapping_bytes = canonical_binary(overlapping_mapping);
		require(overlapping_bytes);
		auto overlapping_result = cxxlens::sdk::decode_capture_bundle(*overlapping_bytes);
		require(!overlapping_result &&
				overlapping_result.error().detail == "overlapping-authority");

		auto excessive_mappings = valid_bundle();
		cxxlens::sdk::import_limits path_limits;
		path_limits.maximum_path_mappings = 1U;
		excessive_mappings.tuple[9] = observed(canonical_value::from_tuple({
			canonical_value::from_tuple({canonical_value::from_string("/external"),
										 canonical_value::from_string("project://external")}),
			canonical_value::from_tuple({canonical_value::from_string("/workspace/example"),
										 canonical_value::from_string("project://")}),
		}));
		auto excessive_mapping_bytes = canonical_binary(excessive_mappings);
		require(excessive_mapping_bytes);
		auto excessive_mapping_result =
			cxxlens::sdk::decode_capture_bundle(*excessive_mapping_bytes, path_limits);
		require(!excessive_mapping_result &&
				excessive_mapping_result.error().code ==
					"application-analysis.import-limit-exceeded");

		auto escaping_logical_path = valid_bundle();
		escaping_logical_path.tuple[5].tuple[0].tuple[3] =
			canonical_value::from_string("external://src/main.cpp");
		auto escaping_bytes = canonical_binary(escaping_logical_path);
		require(escaping_bytes);
		auto escaping_result = cxxlens::sdk::decode_capture_bundle(*escaping_bytes);
		require(!escaping_result &&
				escaping_result.error().detail == "duplicate-or-noncanonical-order");

		canonical_value nested = canonical_value::from_string("leaf");
		for (std::size_t depth{}; depth < 12U; ++depth)
			nested = canonical_value::from_tuple({std::move(nested)});
		auto nested_bytes = canonical_binary(nested);
		require(nested_bytes);
		limits = {};
		limits.maximum_nesting_depth = 8U;
		auto nested_result = cxxlens::sdk::decode_capture_bundle(*nested_bytes, limits);
		require(!nested_result && nested_result.error().detail == "nesting-depth");
	}

	void materialization_request_factory_validates_authority()
	{
		cxxlens::sdk::relation_registry registry;
		auto descriptor = request_descriptor();
		require(registry.add(descriptor));
		auto engine = registry.build("application-analysis-test");
		require(engine);
		const auto policies = cxxlens::sdk::provider::builtin_sandbox_policies();
		require(!policies.empty());
		cxxlens::sdk::provider::provider_selection_request provider;
		provider.provider_id = "cxxlens.gcc-replay";
		provider.provider_version = {1U, 0U, 0U};
		provider.provider_binary_digest = digest('a');
		provider.provider_semantic_contract_digest = digest('b');
		provider.sandbox = {cxxlens::sdk::provider::sandbox_assurance::enforced,
							policies.front().policy_digest()};
		cxxlens::sdk::snapshot_draft publication{{"catalog:test",
												  "experimental",
												  std::string{engine->generation()},
												  "condition:test",
												  std::string{engine->registry_digest()},
												  digest('c'),
												  digest('d')},
												 {1U, 0U, 0U},
												 digest('e'),
												 std::nullopt};
		auto request = cxxlens::sdk::materialization_request::make(
			*engine, publication, {descriptor.id}, "cc.clang23-gcc-replay-1", provider);
		require(request);
		require(request->relation_descriptor_ids().size() == 1U);
		require(request->interpretation() == "cc.clang23-gcc-replay-1");
		auto candidate = application_provider_candidate(descriptor, digest('a'));
		auto configured = cxxlens::sdk::materialization_request::make(*engine,
																	  publication,
																	  {descriptor.id},
																	  "cc.clang23-gcc-replay-1",
																	  provider,
																	  {candidate});
		require(configured);
		auto undiscovered = cxxlens::sdk::materialization_request::make(
			*engine,
			publication,
			{descriptor.id},
			"cc.clang23-gcc-replay-1",
			provider,
			std::vector<cxxlens::sdk::provider::provider_candidate>{});
		require(!undiscovered && undiscovered.error().code == "provider.not-found");

		auto wrong_identity = candidate;
		wrong_identity.description.provider_binary_digest = digest('0');
		auto mismatched = cxxlens::sdk::materialization_request::make(*engine,
																	  publication,
																	  {descriptor.id},
																	  "cc.clang23-gcc-replay-1",
																	  provider,
																	  {wrong_identity});
		require(!mismatched && mismatched.error().code == "provider.not-found");
		auto untrusted_candidate = application_provider_candidate(descriptor, digest('a'), false);
		auto untrusted = cxxlens::sdk::materialization_request::make(*engine,
																	 publication,
																	 {descriptor.id},
																	 "cc.clang23-gcc-replay-1",
																	 provider,
																	 {untrusted_candidate});
		require(!untrusted && untrusted.error().code == "security.downgrade-forbidden");

		auto duplicate = cxxlens::sdk::materialization_request::make(*engine,
																	 publication,
																	 {descriptor.id, descriptor.id},
																	 "cc.clang23-gcc-replay-1",
																	 provider);
		require(!duplicate && duplicate.error().detail == "duplicate");
		auto zero_budget = cxxlens::sdk::provider::execution_budget{};
		zero_budget.output_bytes = 0U;
		auto invalid_budget = cxxlens::sdk::materialization_request::make(*engine,
																		  std::move(publication),
																		  {descriptor.id},
																		  "cc.clang23-gcc-replay-1",
																		  std::move(provider),
																		  zero_budget);
		require(!invalid_budget && invalid_budget.error().field == "budget");
	}

	void execution_plan_binds_capture_task_provider_and_publication_before_effects()
	{
		using namespace cxxlens::sdk;
		auto bytes = canonical_binary(valid_bundle());
		require(bytes);
		auto bundle = decode_capture_bundle(*bytes);
		require(bundle);
		auto imported = import_capture(*bundle);
		require(imported);

		relation_registry registry;
		auto descriptor = request_descriptor();
		require(registry.add(descriptor));
		auto engine = registry.build("application-analysis-execution-test");
		require(engine);
		const auto policies = provider::builtin_sandbox_policies();
		require(!policies.empty());
		auto candidate = application_provider_candidate(descriptor, digest('a'));
		provider::provider_selection_request provider_request;
		provider_request.provider_id = candidate.description.provider_id;
		provider_request.provider_version = candidate.description.provider_version;
		provider_request.provider_binary_digest = candidate.description.provider_binary_digest;
		provider_request.provider_semantic_contract_digest =
			candidate.description.provider_semantic_contract_digest;
		provider_request.sandbox = {provider::sandbox_assurance::enforced,
									policies.front().policy_digest()};
		auto selection = provider::select_provider(provider_request, {&candidate, 1U});
		require(selection);
		const auto& project = application_analysis_imported_value_internal(*imported);
		snapshot_draft publication{{"catalog:test",
									"experimental",
									std::string{engine->generation()},
									"condition:test",
									std::string{engine->registry_digest()},
									digest('c'),
									digest('d')},
								   {1U, 0U, 0U},
								   project.catalog.catalog_digest,
								   std::nullopt};
		auto first =
			detail::make_application_materialization_execution_plan(project,
																	*engine,
																	publication,
																	{&descriptor.id, 1U},
																	"cc.clang23-gcc-replay-1",
																	*selection,
																	{},
																	{});
		auto second =
			detail::make_application_materialization_execution_plan(project,
																	*engine,
																	publication,
																	{&descriptor.id, 1U},
																	"cc.clang23-gcc-replay-1",
																	*selection,
																	{},
																	{});
		require(first && second &&
				first->materialization_request_id == second->materialization_request_id);
		require(first->transport ==
				cxxlens::sdk::detail::application_materialization_execution_transport::process);
		require(first->units.size() == 1U);
		const auto& unit = first->units.front();
		require(unit.process.task_id == unit.task.value().provider_task.task_id);
		require(unit.process.task_input_digest == unit.provider_input.input_digest());
		require(unit.task.value().provider_input_digest == unit.provider_input.input_digest());
		require(unit.task.value().capture.semantic_identity() == unit.capture.semantic_identity());
		require(unit.observation_technique == "clang_gcc_mode_replay");
		require(unit.task.value().provider_task.dependency_groups ==
				std::vector<std::string>{"clang23-gcc-replay"});
		require(unit.task.value().partitions.front().candidate.current.input.precision_profile ==
				"under_approximation");
		require(unit.host_partitions.empty());

		auto unavailable_relation = detail::make_application_materialization_execution_plan(
			project,
			*engine,
			publication,
			std::array<std::string, 1U>{"test.unoffered.v1"},
			"cc.clang23-gcc-replay-1",
			*selection,
			{},
			{});
		require(!unavailable_relation &&
				unavailable_relation.error().detail == "provider-relation-unavailable");

		candidate.executable_argv.front() = "relative-worker";
		auto relative_selection = provider::select_provider(provider_request, {&candidate, 1U});
		require(relative_selection);
		auto relative =
			detail::make_application_materialization_execution_plan(project,
																	*engine,
																	publication,
																	{&descriptor.id, 1U},
																	"cc.clang23-gcc-replay-1",
																	*relative_selection,
																	{},
																	{});
		require(!relative && relative.error().detail == "absolute-path-required");

		auto detached_candidate = candidate;
		detached_candidate.executable_argv.clear();
		auto detached_selection =
			provider::select_provider(provider_request, {&detached_candidate, 1U});
		require(detached_selection);
		auto detached = detail::make_application_materialization_execution_plan(
			project,
			*engine,
			publication,
			{&descriptor.id, 1U},
			"cc.clang23-gcc-replay-1",
			*detached_selection,
			{},
			{},
			{},
			detail::application_materialization_execution_transport::detached);
		require(detached &&
				detached->transport ==
					detail::application_materialization_execution_transport::detached);
		auto invalid_transport = detail::make_application_materialization_execution_plan(
			project,
			*engine,
			publication,
			{&descriptor.id, 1U},
			"cc.clang23-gcc-replay-1",
			*selection,
			{},
			{},
			{},
			static_cast<detail::application_materialization_execution_transport>(255U));
		require(!invalid_transport && invalid_transport.error().field == "transport" &&
				invalid_transport.error().detail == "unsupported");

		auto two_unit_bytes = canonical_binary(valid_two_unit_bundle());
		require(two_unit_bytes);
		auto two_unit_bundle = decode_capture_bundle(*two_unit_bytes);
		require(two_unit_bundle && two_unit_bundle->compile_unit_count() == 2U);
		auto two_unit_project = import_capture(*two_unit_bundle);
		require(two_unit_project);
		const auto& two_unit_value =
			application_analysis_imported_value_internal(*two_unit_project);
		publication.catalog_semantic_digest = two_unit_value.catalog.catalog_digest;
		auto two_unit_plan =
			detail::make_application_materialization_execution_plan(two_unit_value,
																	*engine,
																	publication,
																	{&descriptor.id, 1U},
																	"cc.clang23-gcc-replay-1",
																	*selection,
																	{},
																	{});
		require(two_unit_plan && two_unit_plan->units.size() == 2U);
		require(two_unit_plan->units[0].task.id() != two_unit_plan->units[1].task.id());
	}

	void authenticated_detached_run_is_revalidated_before_publication()
	{
		using namespace cxxlens::sdk;
		using namespace cxxlens::sdk::detail;
		using namespace cxxlens::sdk::provider;
		auto bytes = canonical_binary(valid_bundle());
		require(bytes);
		auto bundle = decode_capture_bundle(*bytes);
		require(bundle);
		auto imported = import_capture(*bundle);
		require(imported);

		relation_registry registry;
		auto descriptor = request_descriptor();
		require(registry.add(descriptor));
		auto engine = registry.build("application-analysis-detached-adoption-test");
		require(engine);
		auto candidate = application_provider_candidate(descriptor, digest('a'));
		provider_selection_request provider_request;
		provider_request.provider_id = candidate.description.provider_id;
		provider_request.provider_version = candidate.description.provider_version;
		provider_request.provider_binary_digest = candidate.description.provider_binary_digest;
		provider_request.provider_semantic_contract_digest =
			candidate.description.provider_semantic_contract_digest;
		provider_request.sandbox = {sandbox_assurance::enforced, candidate.sandbox.policy_digest};
		auto selection = select_provider(provider_request, {&candidate, 1U});
		require(selection);
		const auto& project = application_analysis_imported_value_internal(*imported);
		snapshot_draft publication{{"catalog:detached-test",
									"experimental",
									std::string{engine->generation()},
									"condition:detached-test",
									std::string{engine->registry_digest()},
									digest('c'),
									digest('d')},
								   {1U, 0U, 0U},
								   project.catalog.catalog_digest,
								   std::nullopt};
		candidate.executable_argv.clear();
		auto detached_selection = select_provider(provider_request, {&candidate, 1U});
		require(detached_selection);
		auto plan = make_application_materialization_execution_plan(
			project,
			*engine,
			publication,
			{&descriptor.id, 1U},
			"cc.clang23-gcc-replay-1",
			*detached_selection,
			{},
			{},
			{},
			application_materialization_execution_transport::detached);
		require(plan && plan->units.size() == 1U &&
				plan->transport == application_materialization_execution_transport::detached);
		auto& unit = plan->units.front();

		detached_transcript_sink sink;
		protocol_writer writer{sink, unit.process.limits};
		writer.grant_credit(unit.process.output_credit);
		writer.set_output_budget(unit.process.budget.output_bytes);
		auto hello = encode_control_text(candidate.description.canonical_json());
		auto schema =
			encode_schema_negotiate_metadata({"cxxlens.provider-protocol.v2", protocol_v2_minor});
		require(hello && schema && writer.send(message_type::hello, *hello) &&
				writer.send(message_type::schema_negotiate, *schema));
		detached_application_provider implementation{descriptor};
		require(run_worker(implementation, unit.task.value().provider_task, writer));

		deterministic_detached_run_signer signer;
		auto run = build_detached_provider_run(
			unit.process, unit.replay_plan_digest, sink.transcript, signer);
		require(run && run->value().runtime_receipt_digest);
		auto repeat = build_detached_provider_run(
			unit.process, unit.replay_plan_digest, sink.transcript, signer);
		require(repeat && repeat->digest() == run->digest() &&
				std::ranges::equal(repeat->bytes(), run->bytes()));
		auto validated_transcript =
			provider::detail::validate_detached_provider_transcript(unit.process, sink.transcript);
		require(validated_transcript && candidate.description.signature);
		auto required_features = candidate.description.protocol.required_features;
		std::ranges::sort(required_features);
		auto offered_relations = candidate.description.offered_relations;
		std::ranges::sort(offered_relations);
		provider::detail::detached_provider_transcript_validation_authority
			direct_validation_authority{candidate.description,
										{candidate.description.provider_id,
										 candidate.description.provider_version,
										 candidate.description.provider_binary_digest,
										 candidate.description.provider_semantic_contract_digest,
										 unit.process.limits.protocol_major,
										 unit.process.limits.maximum_minor,
										 required_features,
										 unit.process.sandbox.policy_digest,
										 offered_relations},
										unit.process.output_descriptors,
										unit.process.limits,
										unit.process.budget};
		auto direct_validation =
			provider::detail::validate_detached_provider_transcript_from_sealed_input(
				direct_validation_authority, validated_transcript->input_seal, sink.transcript);
		require(direct_validation);
		auto process_receipt_digest = provider::detail::provider_runtime_receipt_digest(
			validated_transcript->runtime_receipt);
		auto direct_receipt_digest =
			provider::detail::provider_runtime_receipt_digest(direct_validation->runtime_receipt);
		require(process_receipt_digest && direct_receipt_digest &&
				*direct_receipt_digest == *process_receipt_digest);
		auto wrong_identity = direct_validation_authority;
		wrong_identity.provider_identity.provider_binary_digest = digest('f');
		auto identity_mismatch =
			provider::detail::validate_detached_provider_transcript_from_sealed_input(
				std::move(wrong_identity), validated_transcript->input_seal, sink.transcript);
		require(!identity_mismatch &&
				identity_mismatch.error().code == "provider.binary-identity-mismatch");
		auto empty_stream =
			provider::detail::validate_detached_provider_transcript_from_sealed_input(
				direct_validation_authority, validated_transcript->input_seal, {});
		require(!empty_stream && empty_stream.error().code == "provider.output-limit" &&
				empty_stream.error().detail == "detached-transport-bytes");
		auto wrong_protocol = direct_validation_authority;
		wrong_protocol.session_limits.maximum_minor = 1U;
		auto protocol_mismatch =
			provider::detail::validate_detached_provider_transcript_from_sealed_input(
				std::move(wrong_protocol), validated_transcript->input_seal, sink.transcript);
		require(!protocol_mismatch &&
				protocol_mismatch.error().code == "provider.protocol-minor-mismatch");
		detached_provider_run_authority detached_authority{
			unit.process.task_id,
			unit.process.task_input_digest,
			unit.process.normalized_invocation_digest,
			unit.process.toolchain_digest,
			unit.process.environment_digest,
			unit.replay_plan_digest,
			{candidate.description.provider_id,
			 candidate.description.provider_version,
			 candidate.description.provider_binary_digest,
			 candidate.description.provider_semantic_contract_digest,
			 *candidate.description.signature,
			 "not-revoked",
			 unit.process.sandbox.policy_digest}};
		auto direct = build_detached_provider_run_from_validated_transcript(
			detached_authority, sink.transcript, *validated_transcript, signer);
		require(direct && direct->digest() == run->digest() &&
				std::ranges::equal(direct->bytes(), run->bytes()));
		detached_authority.task_input_digest = digest('f');
		auto mismatched = build_detached_provider_run_from_validated_transcript(
			std::move(detached_authority), sink.transcript, *validated_transcript, signer);
		require(!mismatched &&
				mismatched.error().code ==
					"application-analysis.detached-provider-run-build-failed" &&
				mismatched.error().detail == "runtime-binding-mismatch");
		deterministic_detached_run_signer unavailable_signer{true};
		auto unsigned_run = build_detached_provider_run(
			unit.process, unit.replay_plan_digest, sink.transcript, unavailable_signer);
		require(!unsigned_run &&
				unsigned_run.error().code == "test.detached-run-signer-unavailable");
		const auto provider_receipt = *run->value().runtime_receipt_digest;
		exact_detached_signature_verifier verifier{*run};
		auto prepared = prepare_detached_application_materialization(
			*engine, unit.task, unit.provider_input, unit.process, *run, verifier);
		if (!prepared)
			std::cerr << prepared.error().code << ':' << prepared.error().field << ':'
					  << prepared.error().detail << '\n';
		require(prepared && prepared->runtime.runtime_receipt_digest != provider_receipt &&
				prepared->runtime.runtime_receipt_digest.starts_with(
					"detached-provider-run-adoption-receipt:sha256:"));

		auto projection_tamper = run->value();
		++projection_tamper.partitions.front().row_count;
		authenticate(projection_tamper);
		auto projection_run = validate_detached_provider_run(std::move(projection_tamper));
		require(projection_run);
		exact_detached_signature_verifier projection_verifier{*projection_run};
		auto projection_rejected =
			prepare_detached_application_materialization(*engine,
														 unit.task,
														 unit.provider_input,
														 unit.process,
														 *projection_run,
														 projection_verifier);
		require(!projection_rejected && projection_rejected.error().field == "projection");
		auto receipt_tamper = run->value();
		receipt_tamper.runtime_receipt_digest = digest('0');
		authenticate(receipt_tamper);
		auto receipt_run = validate_detached_provider_run(std::move(receipt_tamper));
		require(receipt_run);
		exact_detached_signature_verifier receipt_verifier{*receipt_run};
		auto receipt_rejected = prepare_detached_application_materialization(
			*engine, unit.task, unit.provider_input, unit.process, *receipt_run, receipt_verifier);
		require(!receipt_rejected && receipt_rejected.error().field == "runtime_receipt_digest");

		exact_detached_signature_verifier revoked{*run, detached_run_signature_verdict::revoked};
		auto revoked_result = prepare_detached_application_materialization(
			*engine, unit.task, unit.provider_input, unit.process, *run, revoked);
		require(!revoked_result && revoked_result.error().detail == "signing-key-revoked");
		exact_detached_signature_verifier unavailable{*run,
													  detached_run_signature_verdict::unavailable};
		auto unavailable_result = prepare_detached_application_materialization(
			*engine, unit.task, unit.provider_input, unit.process, *run, unavailable);
		require(!unavailable_result &&
				unavailable_result.error().code ==
					"application-analysis.detached-run-authentication-unavailable");

		auto inflated_process = unit.process;
		++inflated_process.budget.rows;
		auto inflated_result = prepare_detached_application_materialization(
			*engine, unit.task, unit.provider_input, inflated_process, *run, verifier);
		require(!inflated_result && inflated_result.error().field == "provider_identity");

		auto transcript_tamper = run->value();
		transcript_tamper.protocol_transcript.back() ^= std::byte{1U};
		authenticate(transcript_tamper);
		auto transcript_run = validate_detached_provider_run(std::move(transcript_tamper));
		require(transcript_run);
		exact_detached_signature_verifier transcript_verifier{*transcript_run};
		auto transcript_rejected =
			prepare_detached_application_materialization(*engine,
														 unit.task,
														 unit.provider_input,
														 unit.process,
														 *transcript_run,
														 transcript_verifier);
		require(!transcript_rejected &&
				transcript_rejected.error().code == "provider.checksum-mismatch");

		auto store = make_in_memory_snapshot_store(*engine);
		require(store && !store->current(publication.series));
		auto process_plan = *plan;
		process_plan.transport = application_materialization_execution_transport::process;
		auto wrong_transport = publish_detached_application_materializations(
			*engine, *store, process_plan, std::span<const std::vector<std::byte>>{}, verifier);
		require(!wrong_transport && wrong_transport.error().field == "plan" &&
				wrong_transport.error().detail == "detached-transport-required" &&
				!store->current(publication.series));
		std::vector<std::vector<std::byte>> missing;
		auto missing_run = publish_detached_application_materializations(
			*engine, *store, *plan, missing, verifier);
		require(!missing_run && !store->current(publication.series));
		std::vector<std::vector<std::byte>> malformed{
			{run->bytes().begin(), run->bytes().end() - 1}};
		auto malformed_run = publish_detached_application_materializations(
			*engine, *store, *plan, malformed, verifier);
		require(!malformed_run && !store->current(publication.series));
		std::vector<std::vector<std::byte>> detached_runs{
			{run->bytes().begin(), run->bytes().end()}};
		import_limits byte_limited;
		byte_limited.maximum_total_metadata_bytes = detached_runs.front().size() - 1U;
		auto over_limit = publish_detached_application_materializations(
			*engine, *store, *plan, detached_runs, verifier, byte_limited);
		require(!over_limit && over_limit.error().detail == "total-byte-limit" &&
				!store->current(publication.series));
		auto adopted = publish_detached_application_materializations(
			*engine, *store, *plan, detached_runs, verifier);
		require(adopted && adopted->publication.publication_verified &&
				adopted->publication.snapshot.publication().state == publication_state::committed);

#if defined(CXXLENS_TEST_CLANG23_WORKER_PATH)
		test_ed25519_material_port signing_material;
		const openssl_detached_run_signer trusted_signer{signing_material};
		auto trusted_run = build_detached_provider_run(
			unit.process, unit.replay_plan_digest, sink.transcript, trusted_signer);
		require(trusted_run);
		const detached_run_test_directory directory;
		const auto run_path = directory.path() / "compile-unit.run";
		const auto key_path = directory.path() / "trusted-public-key.raw";
		write_binary(run_path, trusted_run->bytes());
		write_binary(key_path, signing_material.public_key());
		cxxlens::runtime::detached_application_materialization_file_request file_request{
			{run_path.string()},
			"worker:clang23-gcc-replay",
			key_path.string(),
			detached_run_public_key_state::trusted,
			{}};
		auto public_file_request = materialization_request::make(*engine,
																 publication,
																 {descriptor.id},
																 "cc.clang23-gcc-replay-1",
																 provider_request,
																 {candidate});
		require(public_file_request);
		auto file_store = make_in_memory_snapshot_store(*engine);
		require(file_store);
		auto file_adopted = cxxlens::runtime::materialize_detached_application_from_files(
			*file_store, *imported, *public_file_request, file_request);
		require(file_adopted &&
				file_adopted->terminal() ==
					cxxlens::sdk::materialization_terminal::published_partial &&
				file_adopted->published_snapshot() && file_adopted->provenance() &&
				file_store->current(publication.series));

		file_request.public_key_state = detached_run_public_key_state::revoked;
		auto revoked_store = make_in_memory_snapshot_store(*engine);
		require(revoked_store);
		auto file_revoked = cxxlens::runtime::materialize_detached_application_from_files(
			*revoked_store, *imported, *public_file_request, file_request);
		require(!file_revoked && file_revoked.error().detail == "signing-key-revoked" &&
				!revoked_store->current(publication.series));
#endif

		auto multi_bytes = canonical_binary(valid_two_unit_bundle());
		require(multi_bytes);
		auto multi_bundle = decode_capture_bundle(*multi_bytes);
		require(multi_bundle);
		auto multi_imported = import_capture(*multi_bundle);
		require(multi_imported);
		const auto& multi_project = application_analysis_imported_value_internal(*multi_imported);
		publication.series.catalog_id = "catalog:detached-multi-test";
		publication.catalog_semantic_digest = multi_project.catalog.catalog_digest;
		auto multi_plan = make_application_materialization_execution_plan(
			multi_project,
			*engine,
			publication,
			{&descriptor.id, 1U},
			"cc.clang23-gcc-replay-1",
			*detached_selection,
			{},
			{},
			{},
			application_materialization_execution_transport::detached);
		require(multi_plan && multi_plan->units.size() == 2U &&
				multi_plan->transport == application_materialization_execution_transport::detached);
		std::vector<validated_detached_provider_run> multi_runs;
		for (const auto& multi_unit : multi_plan->units)
		{
			detached_transcript_sink multi_sink;
			protocol_writer multi_writer{multi_sink, multi_unit.process.limits};
			multi_writer.grant_credit(multi_unit.process.output_credit);
			multi_writer.set_output_budget(multi_unit.process.budget.output_bytes);
			auto multi_hello = encode_control_text(candidate.description.canonical_json());
			auto multi_schema = encode_schema_negotiate_metadata(
				{"cxxlens.provider-protocol.v2", protocol_v2_minor});
			require(multi_hello && multi_schema &&
					multi_writer.send(message_type::hello, *multi_hello) &&
					multi_writer.send(message_type::schema_negotiate, *multi_schema));
			detached_application_provider multi_provider{descriptor};
			require(
				run_worker(multi_provider, multi_unit.task.value().provider_task, multi_writer));
			auto multi_run = build_detached_provider_run(
				multi_unit.process, multi_unit.replay_plan_digest, multi_sink.transcript, signer);
			require(multi_run);
			multi_runs.push_back(std::move(*multi_run));
		}
		std::vector<std::vector<std::byte>> duplicate_runs{
			{multi_runs[0].bytes().begin(), multi_runs[0].bytes().end()},
			{multi_runs[0].bytes().begin(), multi_runs[0].bytes().end()}};
		auto multi_store = make_in_memory_snapshot_store(*engine);
		require(multi_store);
		const deterministic_multi_run_verifier multi_verifier;
		auto duplicate = publish_detached_application_materializations(
			*engine, *multi_store, *multi_plan, duplicate_runs, multi_verifier);
		require(!duplicate && !multi_store->current(publication.series));
		std::vector<std::vector<std::byte>> permuted_runs{
			{multi_runs[1].bytes().begin(), multi_runs[1].bytes().end()},
			{multi_runs[0].bytes().begin(), multi_runs[0].bytes().end()}};
		auto multi_adopted = publish_detached_application_materializations(
			*engine, *multi_store, *multi_plan, permuted_runs, multi_verifier);
		require(multi_adopted && multi_adopted->publication.publication_verified &&
				multi_store->current(publication.series));
	}

#if defined(CXXLENS_TEST_CLANG23_WORKER_PATH)
	[[nodiscard]] std::string executable_digest(const std::string& path)
	{
		std::ifstream input{path, std::ios::binary};
		require(input);
		const std::vector<char> characters{std::istreambuf_iterator<char>{input},
										   std::istreambuf_iterator<char>{}};
		std::vector<std::byte> bytes;
		bytes.reserve(characters.size());
		for (const auto value : characters)
			bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		return cxxlens::sdk::content_digest(bytes);
	}

	void exact_clang23_worker_materializes_through_public_service()
	{
		using namespace cxxlens;
		using namespace cxxlens::sdk;
		const std::array descriptors{
			&build::relations::project::descriptor(),
			&build::relations::compile_unit::descriptor(),
			&build::relations::variant::descriptor(),
			&build::relations::toolchain_context::descriptor(),
			&source::relations::file::descriptor(),
			&source::relations::span::descriptor(),
			&cc::relations::entity::descriptor(),
			&cc::relations::declaration::descriptor(),
			&cc::relations::type::descriptor(),
			&cc::relations::call_site::descriptor(),
			&cc::relations::call_direct_target::descriptor(),
		};
		relation_registry registry;
		std::vector<std::string> relation_ids;
		for (const auto* descriptor : descriptors)
		{
			require(registry.add(*descriptor));
			relation_ids.push_back(descriptor->id);
		}
		std::ranges::sort(relation_ids);
		auto engine = registry.build("application-analysis-clang23-test");
		require(engine);

		auto bundle_bytes = canonical_binary(valid_two_unit_bundle());
		require(bundle_bytes);
		auto bundle = decode_capture_bundle(*bundle_bytes);
		require(bundle);
		auto project = import_capture(*bundle);
		require(project && project->replay_plans().size() == 2U);
		const auto policies = provider::builtin_sandbox_policies();
		require(!policies.empty());
		const std::string worker{CXXLENS_TEST_CLANG23_WORKER_PATH};
		provider::manifest manifest;
		manifest.provider_id = "cxxlens.clang23-gcc-replay";
		manifest.provider_version = {1U, 0U, 0U};
		manifest.package_identity = "cxxlens.clang23-gcc-replay.package";
		manifest.publisher = "cxxlens.project";
		manifest.license = "Apache-2.0 WITH LLVM-exception";
		manifest.protocol = {provider::protocol_v2_major,
							 provider::protocol_v2_minor,
							 provider::protocol_v2_minor,
							 {"credit-backpressure", "task-input-chunks-v2"},
							 {}};
		manifest.platform_tuples = {"linux-x86_64-clang23"};
		manifest.provider_binary_digest = executable_digest(worker);
		manifest.provider_semantic_contract_digest = digest('b');
		manifest.offered_relations = relation_ids;
		manifest.interpretation_domains = {"cc.clang23-gcc-replay-1"};
		manifest.invalidation_contract = digest('c');
		manifest.determinism_contract = digest('d');
		manifest.resource_class = "provider.application-analysis";
		manifest.requested_qualifications = {"experimental"};
		provider::provider_candidate candidate{manifest,
											   provider::discovery_source::explicit_path,
											   {worker},
											   true,
											   true,
											   true,
											   {"experimental"},
											   {"linux-glibc",
												policies.front().mechanisms,
												provider::sandbox_assurance::enforced,
												policies.front().policy_digest(),
												digest('e')},
											   {}};
		provider::provider_selection_request provider_request{
			manifest.provider_id,
			manifest.provider_version,
			manifest.provider_binary_digest,
			manifest.provider_semantic_contract_digest,
			{provider::sandbox_assurance::enforced, policies.front().policy_digest()},
			true,
			std::nullopt};
		provider::execution_budget execution_budget;
#if defined(CXXLENS_SANITIZER_INSTRUMENTED)
		// Sanitizer runtimes reserve a large virtual address range and create helper threads.
		// Keep the production defaults intact while allowing the fully instrumented worker to
		// cross the same process boundary exercised by this test.
		execution_budget.address_space_bytes = std::numeric_limits<std::uint64_t>::max();
		execution_budget.subprocesses = 1024U;
#endif
		snapshot_draft publication{{"catalog:application-analysis",
									"experimental",
									std::string{engine->generation()},
									"condition:application-analysis",
									std::string{engine->registry_digest()},
									digest('f'),
									digest('0')},
								   {1U, 0U, 0U},
								   std::string{project->catalog_semantic_digest()},
								   std::nullopt};
		auto request = materialization_request::make(*engine,
													 publication,
													 relation_ids,
													 "cc.clang23-gcc-replay-1",
													 provider_request,
													 {candidate},
													 execution_budget);
		require(request);
		auto store = make_in_memory_snapshot_store(*engine);
		require(store);
		auto result = materialize(*store, *project, *request);
		if (!result)
			std::cerr << result.error().code << ':' << result.error().field << ':'
					  << result.error().detail << '\n';
		else if (result->terminal() != materialization_terminal::published_partial)
		{
			std::cerr << "unexpected materialization terminal: "
					  << static_cast<int>(result->terminal()) << '\n';
			for (const auto& unresolved : result->unresolved())
				std::cerr << unresolved.code << ':' << unresolved.subject << ':'
						  << unresolved.detail << '\n';
		}
		require(result && result->terminal() == materialization_terminal::published_partial);
		require(result->published_snapshot() && result->provenance());
		require(!result->published_snapshot()->unresolved_items().empty());
		require(store->retained_generation_count() == 1U);
		require(std::ranges::any_of(result->coverage(),
									[](const provider::coverage_unit& unit)
									{
										return unit.kind == "relation" &&
											unit.id == source::relations::file::descriptor().id &&
											unit.state == "covered";
									}));
		require(result->provenance()->provider_id == manifest.provider_id);
		require(result->provenance()->provider_binary_digest == manifest.provider_binary_digest);
		require(!result->provenance()->runtime_receipt_digest.empty());

		const std::string failing_worker{"/bin/false"};
		auto failed_candidate = candidate;
		failed_candidate.description.provider_binary_digest = executable_digest(failing_worker);
		failed_candidate.executable_argv = {failing_worker};
		auto failed_provider_request = provider_request;
		failed_provider_request.provider_binary_digest =
			failed_candidate.description.provider_binary_digest;
		auto failed_request = materialization_request::make(*engine,
															publication,
															relation_ids,
															"cc.clang23-gcc-replay-1",
															failed_provider_request,
															{failed_candidate},
															execution_budget);
		require(failed_request);
		auto empty_store = make_in_memory_snapshot_store(*engine);
		require(empty_store);
		auto failed = materialize(*empty_store, *project, *failed_request);
		require(failed && failed->terminal() == materialization_terminal::failed);
		require(!failed->published_snapshot());
		require(!empty_store->current(publication.series));
	}
#endif
} // namespace

int main(const int argc, const char* const* argv)
{
	if (argc == 2 && std::string_view{argv[1]} == "--detached-adoption-only")
	{
		authenticated_detached_run_is_revalidated_before_publication();
		return EXIT_SUCCESS;
	}
	positive_decode_and_deterministic_import();
	replay_fidelity_and_import_bounds_fail_closed();
	msvc_capture_import_preserves_replay_fidelity();
	portable_msvc_encoder_round_trips_through_host_authority();
	msvc_source_dependencies_are_bounded_and_canonical();
	msvc_response_tokenization_is_shared_and_bounded();
	allocation_failures_are_typed_at_external_boundaries();
	toolchain_observation_gaps_are_preserved();
	duplicate_source_variants_bind_one_closure();
	resource_and_shape_fail_closed();
	materialization_request_factory_validates_authority();
	execution_plan_binds_capture_task_provider_and_publication_before_effects();
	authenticated_detached_run_is_revalidated_before_publication();
#if defined(CXXLENS_TEST_CLANG23_WORKER_PATH)
	exact_clang23_worker_materializes_through_public_service();
#endif
}
