#!/usr/bin/env python3
"""Focused product tests for the SDK doctor resolver and its projections."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
from typing import Any

import jsonschema
import yaml


USE_CASE = "cxxlens.clang22.materialize-and-query.v1"
ROOT = pathlib.Path(__file__).resolve().parents[3]


def validate_product_schema(name: str, value: Any) -> None:
    schema = yaml.safe_load((ROOT / "schemas" / name).read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(value)


def digest(hex_digit: str) -> str:
    return "sha256:" + hex_digit * 64


def semantic_digest(hex_digit: str) -> str:
    return "semantic-v2:sha256:" + hex_digit * 64


def run(executable: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [executable, *arguments], capture_output=True, text=True, check=False
    )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"check_sdk_doctor_product: {message}")


OPERATIONAL_FIELDS = {
    "git",
    "issue",
    "work_unit",
    "work-unit",
    "checkpoint",
    "source_revision",
    "source_tree",
    "qualification_state",
    "evidence_refs",
}


def require_product_only_projection(value: Any) -> None:
    """Doctor documents must not expose repository-operation metadata."""
    if isinstance(value, dict):
        forbidden = OPERATIONAL_FIELDS.intersection(value)
        require(not forbidden, f"operational metadata leaked into doctor output: {sorted(forbidden)}")
        for child in value.values():
            require_product_only_projection(child)
    elif isinstance(value, list):
        for child in value:
            require_product_only_projection(child)


def valid_candidate(*, candidate_digit: str = "1", binary_digit: str = "b") -> dict[str, Any]:
    return {
        "candidate_id": semantic_digest(candidate_digit),
        "provider_id": "provider.example",
        "provider_version": "2.0.0",
        "package_identity": "package.example",
        "provider_manifest_digest": digest("a"),
        "provider_binary_digest": digest(binary_digit),
        "provider_semantic_contract_digest": digest("c"),
        "protocol": {"major": 2, "minor": 0},
        "features": ["task-input-chunks-v2", "task-source-closure-v2"],
        "relations": ["cc.call_site.v1", "cc.entity.v1"],
        "interpretations": ["cc.clang22-canonical-1"],
        "sandbox": {"minimum": "enforced", "policy_digest": digest("d")},
        "trust": {
            "state": "verified",
            "registry_sequence": 7,
            "certificate_id": "certificate.example",
            "trust_anchor_id": "trust-anchor.example",
            "signature_digest": digest("e"),
            "revocation": {
                "state": "not-revoked",
                "effective_sequence": None,
                "reason": None,
            },
        },
    }


def valid_project(*, providers: int = 1, source: bool = True) -> dict[str, Any]:
    catalog_digest = semantic_digest("f")
    project: dict[str, Any] = {
        "project_id": "project.example",
        "catalog_id": "catalog:" + catalog_digest,
        "catalog_digest": catalog_digest,
        "logical_root": "project://example",
        "environment_digest": digest("9"),
        "environment": {
            "release_version": "1.0.0",
            "surface": "provider-sdk",
            "os": "linux",
            "architecture": "x86_64",
            "compiler_provider_major": "clang22",
            "linkage": "static",
        },
        "provider_candidates": [valid_candidate()] if providers else [],
        "store": {"backend": "memory", "format": "cxxlens.snapshot.v3"},
    }
    if source:
        project["source_input"] = {
            "source_snapshot_id": "snapshot.example",
            "compilation_database_id": "compdb.example",
        }
    return {
        "schema": "cxxlens.sdk-doctor-project.v2",
        "document_version": "2.0.0",
        "project": project,
    }


def write_project(directory: pathlib.Path, name: str, document: Any) -> pathlib.Path:
    path = directory / name
    path.write_text(json.dumps(document, ensure_ascii=True, separators=(",", ":")), encoding="utf-8")
    return path


def installed_static_relation_ids() -> list[str]:
    registry = yaml.safe_load(
        (ROOT / "schemas" / "cxxlens_ng_relation_registry.yaml").read_text(encoding="utf-8")
    )
    return sorted(
        f"{relation['name']}.v{relation['semantic_major']}"
        for relation in registry["relations"]
        if relation.get("cpp_projection") == "installed-static"
    )


def check_direct_authenticated_resolver(executable: str, directory: pathlib.Path) -> None:
    """Compile one internal consumer to exercise the injected authority ports."""
    root = ROOT
    build = pathlib.Path(executable).resolve().parent
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(r"^CMAKE_CXX_COMPILER(?::[^=]*)?=(.+)$", cache, re.MULTILINE)
    compiler = match.group(1).strip() if match else ""
    if not compiler:
        compiler = os.environ.get("CXX", "") or shutil.which("clang++-22") or shutil.which("c++") or ""
    require(bool(compiler), "cannot locate the configured C++ compiler for direct resolver test")

    source = directory / "doctor-authority-consumer.cpp"
    binary = directory / "doctor-authority-consumer"
    expected_catalog_projection = directory / "doctor-catalog-projection.json"
    state_projection = directory / "doctor-state-projections.json"
    catalog_document = yaml.safe_load(
        (root / "schemas" / "cxxlens_ng_sdk_doctor_catalog.yaml").read_text(
            encoding="utf-8"
        )
    )
    validate_product_schema("cxxlens_ng_sdk_doctor_catalog.schema.yaml", catalog_document)
    expected_catalog_projection.write_text(
        json.dumps(catalog_document, ensure_ascii=True, separators=(",", ":"), sort_keys=True),
        encoding="utf-8",
    )
    source.write_text(
        textwrap.dedent(
            r'''
            #include <algorithm>
            #include <cstdlib>
            #include <fstream>
            #include <iostream>
            #include <iterator>
			#include <optional>
            #include <string>
            #include <type_traits>
			#include <utility>
            #include <variant>
			#include <vector>

            #include "doctor_product.hpp"

            namespace doctor = cxxlens::sdk::doctor;

            static_assert(!std::is_aggregate_v<doctor::authenticated_provider_execution>);
            static_assert(!std::is_default_constructible_v<
                          doctor::authenticated_provider_execution>);
			static_assert(!std::is_aggregate_v<doctor::provider_installation_artifact>);
			static_assert(!std::is_default_constructible_v<
						  doctor::provider_installation_artifact>);
			static_assert(!std::is_constructible_v<
						  doctor::provider_installation_artifact,
						  cxxlens::sdk::provider::provider_selection,
						  doctor::authenticated_provider_execution>);

			template <class Loader>
			concept accepts_untrusted_execution_report = requires(
				const Loader& loader,
				cxxlens::sdk::provider::process_execution_report report) {
				loader.load(std::move(report));
			};

			static_assert(!accepts_untrusted_execution_report<
						  doctor::provider_execution_authority_loader>);

            [[noreturn]] void fail(const char* message)
            {
                std::cerr << message << '\n';
                std::exit(1);
            }

            void require(const bool condition, const char* message)
            {
                if (!condition)
                    fail(message);
            }

            std::string digest(const char digit)
            {
                return "sha256:" + std::string(64U, digit);
            }

            std::string semantic_digest(const char digit)
            {
                return "semantic-v2:sha256:" + std::string(64U, digit);
            }

            std::string provider_executable_path;

            std::string executable_digest(const std::string_view path)
            {
                std::ifstream input{std::string{path}, std::ios::binary};
                require(input.good(), "provider executable fixture could not be opened");
                const std::string bytes{std::istreambuf_iterator<char>{input},
                                        std::istreambuf_iterator<char>{}};
                require(!input.bad() && !bytes.empty(), "provider executable fixture unreadable");
                return cxxlens::sdk::content_digest(std::as_bytes(std::span{bytes}));
            }

            class exact_signature_verifier final : public doctor::trusted_signature_verifier
            {
              public:
                doctor::authority_verdict registry_verdict{doctor::authority_verdict::verified};
                doctor::authority_verdict certificate_verdict{doctor::authority_verdict::verified};

                [[nodiscard]] doctor::signature_verification_result verify(
                    const std::string_view scope,
                    const std::string_view signer_id,
                    const std::string_view subject,
                    const std::string_view signature) const override
                {
                    if (signer_id != "trust-anchor.example" && signer_id != "issuer.example")
                        return {};
                    return {scope == "certification-registry" ? registry_verdict : certificate_verdict,
                            "trusted.ed25519.test-verifier",
                            signer_id == "trust-anchor.example" ? digest('a') : digest('b'),
                            std::string{subject},
                            std::string{signature}};
                }
            };

            class exact_authority_state final : public doctor::trusted_authority_state_port
            {
              public:
                std::optional<std::uint64_t> epoch{150U};
                std::optional<doctor::support_tuple> environment{doctor::support_tuple{
                    "1.0.0", "provider-sdk", "linux", "x86_64", "clang22", "static"}};
                bool available{true};
                mutable std::map<std::string, std::pair<std::uint64_t, std::string>, std::less<>>
                    accepted;

                [[nodiscard]] std::optional<std::uint64_t> trusted_epoch() const override
                {
                    return epoch;
                }

                [[nodiscard]] std::optional<doctor::support_tuple> installed_environment()
                    const override
                {
                    return environment;
                }

                [[nodiscard]] doctor::registry_sequence_acceptance accept_registry_update(
                    const std::string_view registry_id,
                    const std::uint64_t sequence,
                    const std::string_view identity) const override
                {
                    if (!available)
                        return doctor::registry_sequence_acceptance::unavailable;
                    const auto found = accepted.find(registry_id);
                    if (found != accepted.end() &&
                        (sequence < found->second.first ||
                         (sequence == found->second.first && identity != found->second.second)))
                        return doctor::registry_sequence_acceptance::rollback;
                    accepted[std::string{registry_id}] = {sequence, std::string{identity}};
                    return doctor::registry_sequence_acceptance::accepted;
                }
            };

            cxxlens::sdk::project_catalog project_catalog()
            {
                auto catalog = cxxlens::sdk::project_catalog::make(
                    "project://example",
                    digest('9'),
                    {{"compile-unit.example", digest('6'), digest('7'), digest('8')}});
                require(catalog.has_value(), "project catalog fixture invalid");
                return std::move(*catalog);
            }

            class runtime_fixture_provider final
                : public cxxlens::sdk::provider::portable_provider
            {
              public:
				explicit runtime_fixture_provider(std::string provider_id)
					: provider_id_{std::move(provider_id)}
                {
                }

                [[nodiscard]] std::string_view id() const noexcept override
                {
					return provider_id_;
                }

                [[nodiscard]] cxxlens::sdk::semantic_version version() const noexcept override
                {
					return {2U, 0U, 0U};
                }

                [[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
                {
					return semantic_contract_digest_;
                }

                cxxlens::sdk::result<void> run(
                    const cxxlens::sdk::provider::task& task,
                    cxxlens::sdk::provider::context& context) override
                {
                    context.coverage().request("task", task.task_id);
                    return context.coverage().classify(
                        {"task", task.task_id, "covered", {}});
                }

              private:
				std::string provider_id_;
				std::string semantic_contract_digest_{digest('c')};
            };

			class stdout_frame_sink final : public cxxlens::sdk::provider::frame_sink
            {
              public:
                cxxlens::sdk::result<void> write(
                    const std::span<const std::byte> bytes) override
                {
					std::cout.write(reinterpret_cast<const char*>(bytes.data()),
									static_cast<std::streamsize>(bytes.size()));
					std::cout.flush();
					if (!std::cout)
						return cxxlens::sdk::unexpected(cxxlens::sdk::error{
							"sdk.provider-write-failed", "stdout", {}});
                    return {};
                }
            };

			std::optional<std::string> process_environment(const char* name)
            {
				const auto* value = std::getenv(name);
				return value == nullptr ? std::nullopt : std::optional<std::string>{value};
			}

			cxxlens::sdk::result<cxxlens::sdk::provider::task> runtime_task(
				const std::string_view provider_id)
			{
				using namespace cxxlens::sdk::provider;
				const auto descriptor = cxxlens::cc::relations::call_site::descriptor();
				provider_session session{std::string{provider_id},
									 {2U, 0U, 0U},
									 digest('c'),
									 {descriptor},
									 {},
									 {"cc.clang22-canonical-1"},
									 "observation",
									 "observation"};
				return task::make(std::move(session),
								  project_catalog(),
								  {descriptor},
								  "condition.all",
								  "cc.clang22-canonical-1");
			}

			int run_provider_child()
			{
				using namespace cxxlens::sdk::provider;
				const auto manifest = process_environment("CXXLENS_PROVIDER_MANIFEST");
				const auto provider_id = process_environment("CXXLENS_PROVIDER_ID");
				const auto task_id = process_environment("CXXLENS_PROVIDER_TASK_ID");
				if (!manifest || !provider_id || !task_id)
					return 1;
				auto provider_task = runtime_task(*provider_id);
				if (!provider_task || provider_task->task_id != *task_id)
					return 1;
				stdout_frame_sink sink;
				protocol_writer writer{sink};
				writer.grant_credit({64U * 1024U * 1024U, 65536U});
				auto hello = encode_control_text(*manifest);
				auto schema = encode_schema_negotiate_metadata(
					{"cxxlens.provider-protocol.v2", protocol_v2_minor});
				if (!hello || !schema || !writer.send(message_type::hello, *hello) ||
					!writer.send(message_type::schema_negotiate, *schema))
					return 1;
				runtime_fixture_provider implementation{*provider_id};
				return run_worker(implementation, *provider_task, writer).has_value() ? 0 : 1;
			}

			cxxlens::sdk::provider::process_task_request execution_request(
				cxxlens::sdk::provider::provider_selection selection)
            {
                using namespace cxxlens::sdk::provider;
				const auto& manifest = selection.selected_candidate().description;
				const auto descriptor = cxxlens::cc::relations::call_site::descriptor();
				auto provider_task = runtime_task(manifest.provider_id);
                require(provider_task.has_value(), "provider runtime task fixture invalid");
                process_task_request request;
				request.selection = std::move(selection);
                request.output_descriptors = {descriptor};
                request.task_id = provider_task->task_id;
                request.task_input_digest = cxxlens::sdk::content_digest(
                    std::span<const std::byte>{request.payload});
                request.normalized_invocation_digest = digest('1');
                request.toolchain_digest = digest('2');
                request.environment_digest = digest('3');
				request.sandbox = request.selection.authority_request().sandbox;
				return request;
			}

			doctor::provider_installation_artifact execution_authority(
				cxxlens::sdk::provider::provider_selection selection)
			{
				auto request = execution_request(std::move(selection));
                const doctor::provider_execution_authority_loader loader;
				auto loaded = loader.load(std::move(request));
				if (std::holds_alternative<doctor::product_error>(loaded))
				{
					const auto& error = std::get<doctor::product_error>(loaded);
					std::cerr << error.code << ':' << error.field << ':' << error.detail << '\n';
				}
				require(std::holds_alternative<doctor::provider_installation_artifact>(loaded),
						"production provider runtime did not mint authority");
				return std::get<doctor::provider_installation_artifact>(std::move(loaded));
            }

            doctor::provider_installation_artifact installation(
                const char identity = '1',
                const cxxlens::sdk::provider::sandbox_assurance assurance =
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                const std::string_view executable = {},
                const bool include_execution_authority = true,
                const std::string_view publisher = "cxxlens.project",
                const bool include_provider_owned_relations = false)
            {
                const auto selected_executable =
                    executable.empty() ? provider_executable_path : std::string{executable};
                cxxlens::sdk::provider::provider_candidate discovered;
                auto& manifest = discovered.description;
                manifest.provider_id =
                    identity == '1' ? "cxxlens.clang22.reference" : "cxxlens.provider.other";
                manifest.provider_version = {2U, 0U, 0U};
                manifest.package_identity = identity == '1'
                    ? "cxxlens.clang22.reference.package"
                    : "cxxlens.provider.other.package";
                manifest.publisher = std::string{publisher};
                manifest.license = "Apache-2.0";
                manifest.protocol.major = 2U;
                manifest.protocol.minimum_minor = 0U;
                manifest.protocol.maximum_minor = 0U;
                manifest.protocol.required_features = {"task-input-chunks-v2"};
                manifest.protocol.optional_features = {"task-source-closure-v2"};
                manifest.platform_tuples = {"linux-glibc"};
                manifest.provider_binary_digest = executable_digest(selected_executable);
                manifest.provider_semantic_contract_digest = digest('c');
                manifest.offered_relations = {"cc.call_site@1", "cc.entity@1"};
                if (identity == '1' || include_provider_owned_relations)
                {
                    manifest.offered_relations.push_back("cc.call_direct_target@1");
                    manifest.offered_relations.push_back("frontend.clang22.call_observation@2");
                    manifest.offered_relations.push_back("frontend.clang22.entity_observation@2");
                    manifest.offered_relations.push_back("frontend.clang22.type_observation@2");
                }
                std::ranges::sort(manifest.offered_relations);
                manifest.interpretation_domains = {"cc.clang22-canonical-1"};
                manifest.invalidation_contract = digest('4');
                manifest.determinism_contract = digest('5');
                manifest.resource_class = "bounded";
                manifest.sandbox_minimum =
                    std::string{doctor::sandbox_assurance_name(assurance)};
                manifest.requested_qualifications = {
                    "canonical-semantic-qualified", "schema-conformant"};
                const auto policies = cxxlens::sdk::provider::builtin_sandbox_policies();
                require(!policies.empty(), "built-in sandbox policies missing");
                discovered.source = cxxlens::sdk::provider::discovery_source::explicit_path;
                discovered.executable_argv = {selected_executable};
                discovered.authoritative_path = true;
                discovered.trust_valid = true;
                discovered.certification_valid = true;
                discovered.certified_qualifications = {
                    "canonical-semantic-qualified", "schema-conformant"};
                discovered.sandbox.platform = "linux-glibc";
                discovered.sandbox.mechanisms = policies.front().mechanisms;
                discovered.sandbox.achieved = assurance;
                discovered.sandbox.policy_digest = policies.front().policy_digest();
                cxxlens::sdk::provider::execution_budget execution_budget;
                auto evidence = cxxlens::sdk::provider::sandbox_evidence_digest(
                    policies.front(),
                    execution_budget,
                    assurance,
                    discovered.sandbox.mechanisms,
                    manifest.provider_binary_digest);
                require(evidence.has_value(), "sandbox evidence fixture invalid");
                discovered.sandbox.evidence_digest = std::move(*evidence);
                cxxlens::sdk::provider::provider_selection_request request;
                request.provider_id = manifest.provider_id;
                request.provider_version = manifest.provider_version;
                request.provider_binary_digest = manifest.provider_binary_digest;
                request.provider_semantic_contract_digest =
                    manifest.provider_semantic_contract_digest;
                request.sandbox.minimum = assurance;
                request.sandbox.policy_digest = discovered.sandbox.policy_digest;
                auto selected = cxxlens::sdk::provider::select_provider(request, {&discovered, 1U});
                require(selected.has_value(), "provider discovery token fixture invalid");
                if (include_execution_authority)
					return execution_authority(std::move(*selected));
				const doctor::provider_execution_authority_loader loader;
				auto observed = loader.observe(std::move(*selected));
				require(std::holds_alternative<doctor::provider_installation_artifact>(observed),
						"validated provider selection was not observed");
				return std::get<doctor::provider_installation_artifact>(std::move(observed));
            }

            const cxxlens::sdk::provider::provider_candidate& discovered(
                const doctor::provider_installation_artifact& artifact)
            {
				return artifact.discovery().selected_candidate();
            }

            std::string candidate_id(const doctor::provider_installation_artifact& artifact)
            {
                const auto found = std::ranges::find_if(
					artifact.discovery().decisions(),
                    [](const cxxlens::sdk::provider::provider_candidate_decision& value)
                    {
                        return value.selected;
                    });
				require(found != artifact.discovery().decisions().end(),
						"selected decision missing");
                return found->candidate_digest;
            }

            doctor::provider_certificate certificate(
                const doctor::provider_installation_artifact& artifact)
            {
                const auto& selected = discovered(artifact);
                const auto& manifest = selected.description;
                doctor::provider_certificate output;
                output.id = "certificate." + manifest.provider_id;
                output.issuer_id = "issuer.example";
                output.serial = "serial." + manifest.provider_id;
                output.subject = {
                    manifest.provider_id,
                    manifest.provider_version.string(),
                    manifest.package_identity,
                    manifest.publisher,
                    doctor::content_digest_text(manifest.canonical_json()),
                    manifest.provider_binary_digest,
                    manifest.provider_semantic_contract_digest};
                for (const auto& relation : manifest.offered_relations)
                    for (const auto& interpretation : manifest.interpretation_domains)
                        output.qualifications.push_back(
                            {relation.starts_with("frontend.clang22.")
                                 ? "schema-conformant"
                                 : "canonical-semantic-qualified",
                             relation,
                             interpretation,
                             {"clang-22"},
                             {"linux"}});
                output.not_before_epoch = 100U;
                output.not_after_epoch = 200U;
                output.registry_sequence = 7U;
                output.certificate_signature_digest = digest('e');
                return output;
            }

            doctor::certification_registry_document registry(
                const std::vector<doctor::provider_installation_artifact>& artifacts)
            {
                doctor::certification_registry_document output;
                output.update_sequence = 7U;
                output.trust_anchors = {
                    {"trust-anchor.example", digest('a'), "production", true}};
                output.issuers = {{"issuer.example",
                                   "trust-anchor.example",
                                   digest('b'),
                                   true,
                                   {"canonical-semantic-qualified", "schema-conformant"},
                                   {"cc.", "cxxlens.", "frontend.clang22."}}};
                for (const auto& artifact : artifacts)
                    output.certificates.push_back(certificate(artifact));
                auto identity = doctor::certification_registry_semantic_identity(output);
                require(std::holds_alternative<std::string>(identity), "registry identity failed");
                output.declared_semantic_identity = std::get<std::string>(std::move(identity));
                output.registry_signature_digest = digest('f');
                return output;
            }

            void reseal_registry(doctor::certification_registry_document& value)
            {
                auto identity = doctor::certification_registry_semantic_identity(value);
                require(std::holds_alternative<std::string>(identity), "registry reseal failed");
                value.declared_semantic_identity = std::get<std::string>(std::move(identity));
            }

            doctor::installed_product_authority_verifier authority(
                const std::optional<cxxlens::sdk::project_catalog>& catalog,
                const std::vector<doctor::provider_installation_artifact>& artifacts,
                doctor::certification_registry_document registry_value,
                const doctor::trusted_signature_verifier& verifier,
                const doctor::trusted_authority_state_port& authority_state)
            {
                doctor::installed_authority_source source;
                source.project_catalog = catalog;
                source.certification_registry = std::move(registry_value);
                source.provider_installations = artifacts;
                const doctor::installed_product_authority_loader authority_loader;
                auto loaded = authority_loader.load(source, verifier, authority_state);
				if (std::holds_alternative<doctor::product_error>(loaded))
				{
					const auto& error = std::get<doctor::product_error>(loaded);
					std::cerr << error.code << ':' << error.field << ':' << error.detail << '\n';
				}
                require(std::holds_alternative<doctor::installed_product_authority_verifier>(loaded),
                        "authority loader failed");
                return std::get<doctor::installed_product_authority_verifier>(std::move(loaded));
            }

            std::variant<doctor::installed_product_authority_verifier, doctor::product_error>
            load_authority(
                const std::optional<cxxlens::sdk::project_catalog>& catalog,
                const std::vector<doctor::provider_installation_artifact>& artifacts,
                doctor::certification_registry_document registry_value,
                const doctor::trusted_signature_verifier& verifier,
                const doctor::trusted_authority_state_port& authority_state)
            {
                doctor::installed_authority_source source;
                source.project_catalog = catalog;
                source.certification_registry = std::move(registry_value);
                source.provider_installations = artifacts;
                const doctor::installed_product_authority_loader authority_loader;
                return authority_loader.load(source, verifier, authority_state);
            }

            doctor::installed_product_authority_verifier authority(
                const std::optional<cxxlens::sdk::project_catalog>& catalog,
                const std::vector<doctor::provider_installation_artifact>& artifacts,
                doctor::certification_registry_document registry_value,
                const doctor::trusted_signature_verifier& verifier)
            {
                const exact_authority_state authority_state;
                return authority(catalog, artifacts, std::move(registry_value), verifier, authority_state);
            }

            doctor::project_context project(
                const cxxlens::sdk::project_catalog& catalog,
                const std::vector<doctor::provider_installation_artifact>& artifacts)
            {
                doctor::project_context output;
                output.project_id = "project.example";
                output.catalog_digest = catalog.catalog_digest;
                output.catalog_id = catalog.catalog_id;
                output.logical_root = catalog.logical_root;
                output.environment_digest = catalog.environment_digest;
                output.environment = {"1.0.0", "provider-sdk", "linux", "x86_64", "clang22", "static"};
                output.source_input = true;
                output.source_snapshot_id = "snapshot.example";
                output.compilation_database_id = "compdb.example";
                output.store_input = true;
                output.store_backend = "memory";
                output.store_format = "cxxlens.snapshot.v3";
                for (const auto& artifact : artifacts)
                {
                    const auto& selected = discovered(artifact);
                    const auto& manifest = selected.description;
                    doctor::provider_candidate candidate;
                    candidate.candidate_id = candidate_id(artifact);
                    candidate.provider_id = manifest.provider_id;
                    candidate.provider_version = manifest.provider_version.string();
                    candidate.package_identity = manifest.package_identity;
                    candidate.provider_manifest_digest =
                        doctor::content_digest_text(manifest.canonical_json());
                    candidate.provider_binary_digest = manifest.provider_binary_digest;
                    candidate.provider_semantic_contract_digest =
                        manifest.provider_semantic_contract_digest;
                    candidate.protocol_major = manifest.protocol.major;
                    candidate.protocol_minor = manifest.protocol.maximum_minor;
                    candidate.features = manifest.protocol.required_features;
                    candidate.features.insert(candidate.features.end(),
                                              manifest.protocol.optional_features.begin(),
                                              manifest.protocol.optional_features.end());
                    for (const auto& relation : manifest.offered_relations)
                    {
                        auto normalized = doctor::provider_relation_offer_to_descriptor(relation);
                        require(normalized.has_value(), "provider relation fixture did not normalize");
                        candidate.relations.push_back(std::move(*normalized));
                    }
                    candidate.interpretations = manifest.interpretation_domains;
                    candidate.sandbox_minimum =
                        std::string{doctor::sandbox_assurance_name(selected.sandbox.achieved)};
                    const auto policy = cxxlens::sdk::provider::resolve_sandbox_policy(
                        selected.sandbox.policy_digest);
                    require(policy.has_value(), "selected sandbox policy missing");
                    candidate.sandbox_policy_digest =
                        doctor::content_digest_text(policy->canonical_form());
                    candidate.trust.state = doctor::trust_state::verified;
                    candidate.trust.registry_sequence = 7U;
                    candidate.trust.certificate_id = "certificate." + manifest.provider_id;
                    candidate.trust.trust_anchor_id = "trust-anchor.example";
                    candidate.trust.signature_digest = digest('e');
                    candidate.trust.revocation.state = doctor::revocation_state::not_revoked;
                    output.provider_candidates.push_back(std::move(candidate));
                }
                return output;
            }

            doctor::resolution resolved(
                const doctor::project_context& project_value,
                const doctor::installed_product_authority_verifier& verifier,
                const doctor::authenticated_capability_catalog& catalog)
            {
                auto result = doctor::resolve(
                    "cxxlens.clang22.materialize-and-query.v1", project_value, catalog, verifier);
                if (std::holds_alternative<doctor::product_error>(result))
                {
                    const auto& error = std::get<doctor::product_error>(result);
                    std::cerr << error.code << ':' << error.field << ':' << error.detail << '\n';
                    fail("resolver returned product error");
                }
                return std::get<doctor::resolution>(std::move(result));
            }

            const doctor::capability_result& capability(
                const doctor::resolution& result, const std::string_view id)
            {
                const auto found = std::ranges::find(result.capability_path, id, &doctor::capability_result::id);
                require(found != result.capability_path.end(), "capability missing from result");
                return *found;
            }

            void write_state_projections(const doctor::resolution& baseline,
                                         const std::string_view path)
            {
                const auto make = [&](const doctor::resolution_state state,
                                      const doctor::diagnosis_reason reason)
                    -> doctor::resolution {
                    auto sample = baseline;
                    sample.state = state;
                    sample.reason = reason;
                    sample.completion_plan.clear();
                    sample.conflicts.clear();
                    for (auto& item : sample.capability_path)
                    {
                        item.state = state;
                        item.reason = reason;
                        item.candidate_ids.clear();
                    }
                    if (state == doctor::resolution_state::proved)
                    {
                        sample.missing.clear();
                        sample.unresolved.clear();
                    }
                    if (state == doctor::resolution_state::conflicting)
                        sample.conflicts.push_back(
                            {"provider.protocol.v2", {semantic_digest('1'), semantic_digest('2')}});
                    return sample;
                };
                std::vector<doctor::resolution> samples;
                samples.push_back(make(doctor::resolution_state::proved,
                                       doctor::diagnosis_reason::none));
                samples.push_back(make(doctor::resolution_state::disproved,
                                       doctor::diagnosis_reason::unsupported_tuple));
                samples.push_back(make(doctor::resolution_state::unknown,
                                       doctor::diagnosis_reason::catalog_unavailable));
                samples.push_back(baseline);
                samples.push_back(make(doctor::resolution_state::conflicting,
                                       doctor::diagnosis_reason::conflicting_capability));
                std::ofstream output{std::string{path}, std::ios::binary};
                require(output.good(), "state projection output could not be opened");
                output << '[';
                for (std::size_t index = 0U; index < samples.size(); ++index)
                {
                    if (index != 0U)
                        output << ',';
                    output << doctor::canonical_json(doctor::to_json(samples[index]));
                }
                output << ']';
                require(output.good(), "state projection output could not be written");
            }

            int main(const int argc, char** argv)
            {
				if (std::getenv("CXXLENS_PROVIDER_MANIFEST") != nullptr)
					return run_provider_child();
                require(argc == 4, "doctor executable/catalog/state projection path missing");
				provider_executable_path = std::filesystem::absolute(argv[0]).string();
                const doctor::installed_product_catalog_loader loader;
                auto loaded_catalog = loader.load();
                require(std::holds_alternative<doctor::authenticated_capability_catalog>(
                            loaded_catalog),
                        "compiled product catalog did not authenticate");
                const auto& catalog_token =
                    std::get<doctor::authenticated_capability_catalog>(loaded_catalog);
                require(catalog_token.semantic_identity() ==
                            doctor::sdk_doctor_catalog_semantic_identity,
                        "catalog token did not cross-bind the compiled semantic identity");
                auto catalog_projection = doctor::catalog_semantic_projection(
                    doctor::sdk_doctor_catalog_value());
                require(std::holds_alternative<std::string>(catalog_projection),
                        "typed catalog projection failed");
                std::ifstream expected_projection_stream{argv[2], std::ios::binary};
                const std::string expected_projection{
                    std::istreambuf_iterator<char>{expected_projection_stream},
                    std::istreambuf_iterator<char>{}};
                if (expected_projection_stream.bad() ||
                    std::get<std::string>(catalog_projection) != expected_projection)
                {
                    std::cerr << "observed-catalog=" << std::get<std::string>(catalog_projection)
                              << "\nexpected-catalog=" << expected_projection << '\n';
                    fail("compiled catalog diverged from installed YAML product value");
                }
                exact_signature_verifier crypto;
                const auto catalog = project_catalog();
                const auto baseline_installation = installation();
                auto baseline_registry = registry({baseline_installation});
                auto baseline = project(catalog, {baseline_installation});
                auto verified = authority(
                    catalog, {baseline_installation}, baseline_registry, crypto);
                const auto proved = resolved(baseline, verified, catalog_token);
                require(proved.state == doctor::resolution_state::partial,
                        "unbound source/store artifacts did not remain partial");
                require(capability(proved, "input.project-catalog.v1").state ==
                            doctor::resolution_state::proved &&
                            capability(proved, "provider.protocol.v2").state ==
                                doctor::resolution_state::proved,
                        "authenticated catalog/provider subset was not proved");
                require(capability(proved, "input.source-closure.v1").reason ==
                            doctor::diagnosis_reason::source_closure_unavailable &&
                            capability(proved, "store.snapshot.v3").reason ==
                                doctor::diagnosis_reason::store_authority_unavailable,
                        "unavailable source/store authority reason changed");
                const std::vector<std::string> expected_plan{
                    "input.source-closure.v1", "store.snapshot.v3"};
                require(
                    std::ranges::equal(
                        proved.completion_plan,
                        expected_plan,
                        [](const doctor::plan_step& step, const std::string& capability_id) {
                            return step.unlocks == capability_id;
                        }),
                    "completion plan did not include every actionable capability in order");
                for (const auto& step : proved.completion_plan)
                    require(step.reason != doctor::diagnosis_reason::none,
                            "completion plan omitted its typed reason");
                write_state_projections(proved, argv[3]);
                const auto& baseline_manifest = discovered(baseline_installation).description;
                const std::vector<std::string> expected_provider_provenance{
                    "provider.id=" + baseline_manifest.provider_id,
                    "provider.version=" + baseline_manifest.provider_version.string(),
                    "provider.package-identity=" + baseline_manifest.package_identity,
                    "provider.manifest-digest=" +
                        doctor::content_digest_text(baseline_manifest.canonical_json()),
                    "provider.binary-digest=" + baseline_manifest.provider_binary_digest,
					"provider.semantic-contract-digest=" +
						baseline_manifest.provider_semantic_contract_digest,
					"provider.execution-semantic-identity=" +
						std::string{
							baseline_installation.execution()->execution_semantic_identity()},
					"provider.selection-semantic-identity=" +
						std::string{
							baseline_installation.execution()->selection_semantic_identity()},
                    std::string{"provider.trust-anchor-id=trust-anchor.example"},
                    "provider.signature-digest=" + digest('e')};
                for (const auto& binding : expected_provider_provenance)
                    require(std::ranges::find(proved.provenance, binding) !=
                                proved.provenance.end(),
                            "provider authority provenance binding was dropped");

                auto root_tampered = baseline;
                root_tampered.logical_root = "project://different";
                const auto root_rejected = resolved(root_tampered, verified, catalog_token);
                require(root_rejected.state == doctor::resolution_state::disproved &&
                            root_rejected.reason == doctor::diagnosis_reason::catalog_binding_invalid,
                        "project logical root escaped authenticated catalog binding");

                auto environment_digest_tampered = baseline;
                environment_digest_tampered.environment_digest = digest('0');
                require(resolved(environment_digest_tampered, verified, catalog_token).reason ==
                            doctor::diagnosis_reason::catalog_binding_invalid,
                        "project environment digest escaped authenticated catalog binding");

                auto environment_tuple_tampered = baseline;
                environment_tuple_tampered.environment.linkage = "shared";
                require(resolved(environment_tuple_tampered, verified, catalog_token).reason ==
                            doctor::diagnosis_reason::catalog_binding_invalid,
                        "self-claimed support tuple escaped installed environment binding");

                exact_authority_state environment_unavailable;
                environment_unavailable.environment = std::nullopt;
                auto no_environment = authority(
                    catalog,
                    {baseline_installation},
                    baseline_registry,
                    crypto,
                    environment_unavailable);
                require(capability(resolved(baseline, no_environment, catalog_token),
                                   "input.project-catalog.v1").state ==
                            doctor::resolution_state::unknown,
                        "missing installed environment minted catalog authority");

                auto permuted = baseline;
                std::ranges::reverse(permuted.provider_candidates.front().features);
                std::ranges::reverse(permuted.provider_candidates.front().relations);
                require(
                    doctor::canonical_json(doctor::to_json(proved)) ==
                        doctor::canonical_json(
                            doctor::to_json(resolved(permuted, verified, catalog_token))),
                    "direct input order changed canonical result");

                auto missing_provider = project(catalog, {});
                auto catalog_only = authority(catalog, {}, registry({}), crypto);
                const auto partial = resolved(missing_provider, catalog_only, catalog_token);
                require(partial.state == doctor::resolution_state::partial, "missing provider was not partial");
                require(capability(partial, "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::missing_provider,
                        "missing provider reason changed");
                const std::vector<std::string> expected_missing_provider_plan{
                    "input.source-closure.v1", "provider.protocol.v2", "store.snapshot.v3"};
                require(
                    std::ranges::equal(
                        partial.completion_plan,
                        expected_missing_provider_plan,
                        [](const doctor::plan_step& step, const std::string& capability_id) {
                            return step.unlocks == capability_id;
                        }),
                    "completion plan omitted an actionable capability");
                require(partial.completion_plan[0].reason ==
                                doctor::diagnosis_reason::source_closure_unavailable &&
                            partial.completion_plan[1].reason ==
                                doctor::diagnosis_reason::missing_provider &&
                            partial.completion_plan[2].reason ==
                                doctor::diagnosis_reason::store_authority_unavailable,
                        "completion plan reason codes were not typed");

                auto provider_absent = authority(catalog, {}, registry({}), crypto);
                const auto unverified = resolved(baseline, provider_absent, catalog_token);
                require(unverified.state == doctor::resolution_state::partial,
                        "self-claimed verified provider was not held partial");
                require(capability(unverified, "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unavailable,
                        "missing certification reason changed");

                auto mismatched_registry = baseline_registry;
                mismatched_registry.certificates.front().subject.binary_digest = digest('0');
                reseal_registry(mismatched_registry);
                auto mismatched = authority(
                    catalog, {baseline_installation}, mismatched_registry, crypto);
                const auto mismatch = resolved(baseline, mismatched, catalog_token);
                require(capability(mismatch, "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "certification subject mismatch was not disproved");

                auto unverified_crypto = crypto;
                unverified_crypto.certificate_verdict = doctor::authority_verdict::unverified;
                auto signature_unverified = authority(
                    catalog, {baseline_installation}, baseline_registry, unverified_crypto);
                require(capability(resolved(baseline, signature_unverified, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unverified,
                        "unverified registry signature became authoritative");

                const doctor::unavailable_signature_verifier unavailable_crypto;
                auto verifier_unavailable = authority(
                    catalog, {baseline_installation}, baseline_registry, unavailable_crypto);
                require(capability(resolved(baseline, verifier_unavailable, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unverified,
                        "unavailable cryptographic verifier became a rejection or authority");

                exact_authority_state time_unavailable;
                time_unavailable.epoch = std::nullopt;
                auto no_time = authority(
                    catalog, {baseline_installation}, baseline_registry, crypto, time_unavailable);
                require(capability(resolved(baseline, no_time, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unverified,
                        "missing trusted time minted provider authority");

                exact_authority_state expired_time;
                expired_time.epoch = 201U;
                auto expired = authority(
                    catalog, {baseline_installation}, baseline_registry, crypto, expired_time);
                require(capability(resolved(baseline, expired, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "expired provider certificate was accepted");

                exact_authority_state rollback_state;
                auto newer_registry = baseline_registry;
                newer_registry.update_sequence = 8U;
                auto newer = authority(
                    catalog, {baseline_installation}, newer_registry, crypto, rollback_state);
                require(capability(resolved(baseline, newer, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::proved,
                        "newer signed registry was not accepted");
                auto rolled_back = authority(
                    catalog, {baseline_installation}, baseline_registry, crypto, rollback_state);
                require(capability(resolved(baseline, rolled_back, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "older signed registry bypassed persistent sequence floor");

                exact_authority_state sequence_conflict_state;
                auto first_sequence = authority(
                    catalog,
                    {baseline_installation},
                    baseline_registry,
                    crypto,
                    sequence_conflict_state);
                require(capability(resolved(baseline, first_sequence, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::proved,
                        "initial sequence was not accepted");
                auto conflicting_sequence = baseline_registry;
                conflicting_sequence.certificates.front().serial = "serial.changed";
                reseal_registry(conflicting_sequence);
                auto sequence_conflict = authority(
                    catalog,
                    {baseline_installation},
                    conflicting_sequence,
                    crypto,
                    sequence_conflict_state);
                require(capability(resolved(baseline, sequence_conflict, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "same sequence accepted a different registry identity");

                exact_authority_state state_unavailable;
                state_unavailable.available = false;
                auto no_rollback_state = authority(
                    catalog,
                    {baseline_installation},
                    baseline_registry,
                    crypto,
                    state_unavailable);
                require(capability(resolved(baseline, no_rollback_state, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unverified,
                        "missing rollback state minted provider authority");

                auto invalid_verdict_crypto = crypto;
                invalid_verdict_crypto.certificate_verdict =
                    static_cast<doctor::authority_verdict>(255U);
                auto invalid_verdict = authority(
                    catalog, {baseline_installation}, baseline_registry, invalid_verdict_crypto);
                require(capability(resolved(baseline, invalid_verdict, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "unknown cryptographic verdict became verified");

                auto wrong_toolchain_registry = baseline_registry;
                for (auto& qualification :
                     wrong_toolchain_registry.certificates.front().qualifications)
                    if (qualification.relation == "cc.call_site@1" ||
                        qualification.relation == "cc.entity@1")
                        qualification.toolchains = {"gcc15"};
                reseal_registry(wrong_toolchain_registry);
                auto wrong_toolchain = authority(
                    catalog, {baseline_installation}, wrong_toolchain_registry, crypto);
                require(capability(resolved(baseline, wrong_toolchain, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "certificate for another toolchain was accepted");

                auto wrong_platform_registry = baseline_registry;
                for (auto& qualification :
                     wrong_platform_registry.certificates.front().qualifications)
                    if (qualification.relation == "cc.call_site@1" ||
                        qualification.relation == "cc.entity@1")
                        qualification.platforms = {"windows-x86_64"};
                reseal_registry(wrong_platform_registry);
                auto wrong_platform = authority(
                    catalog, {baseline_installation}, wrong_platform_registry, crypto);
                require(capability(resolved(baseline, wrong_platform, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "certificate for another platform was accepted");

                auto wrong_namespace_registry = baseline_registry;
                wrong_namespace_registry.issuers.front().namespace_prefixes = {"evil."};
                reseal_registry(wrong_namespace_registry);
                auto wrong_namespace = authority(
                    catalog, {baseline_installation}, wrong_namespace_registry, crypto);
                require(capability(resolved(baseline, wrong_namespace, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "issuer without namespace grants was accepted");

                const auto wrong_publisher_installation = installation(
                    '1',
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                    {},
                    true,
                    "cxxlens");
                auto wrong_publisher_project = project(catalog, {wrong_publisher_installation});
                auto wrong_publisher_authority = authority(
                    catalog,
                    {wrong_publisher_installation},
                    registry({wrong_publisher_installation}),
                    crypto);
                require(capability(resolved(wrong_publisher_project,
                                            wrong_publisher_authority,
                                            catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "standard namespace owner/publisher mismatch was accepted");

                const auto wrong_provider_owner_installation = installation(
                    '2',
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                    {},
                    true,
                    "cxxlens.project",
                    true);
                auto wrong_provider_owner_project =
                    project(catalog, {wrong_provider_owner_installation});
                auto wrong_provider_owner_authority = authority(
                    catalog,
                    {wrong_provider_owner_installation},
                    registry({wrong_provider_owner_installation}),
                    crypto);
                require(capability(resolved(wrong_provider_owner_project,
                                            wrong_provider_owner_authority,
                                            catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "registered provider-owned namespace accepted another provider ID");

                auto candidate_claim_tampered = baseline;
                candidate_claim_tampered.provider_candidates.front().candidate_id =
                    semantic_digest('0');
                require(capability(resolved(candidate_claim_tampered, verified, catalog_token),
                                   "provider.protocol.v2").state !=
                            doctor::resolution_state::proved,
                        "project candidate ID self-claim replaced discovery identity");

				const auto unexecuted_path =
					std::filesystem::path{argv[0]}.parent_path() / "provider-unexecuted-image";
				std::filesystem::copy_file(provider_executable_path,
									   unexecuted_path,
									   std::filesystem::copy_options::overwrite_existing);
				const auto unexecuted_installation = installation(
					'1',
					cxxlens::sdk::provider::sandbox_assurance::enforced,
					unexecuted_path.string(),
					false);
				require(candidate_id(unexecuted_installation) != candidate_id(baseline_installation),
						"ordered executable argv was absent from candidate identity");
				auto baseline_selection_identity = doctor::provider_selection_authority_identity(
					baseline_installation.discovery());
				auto unexecuted_selection_identity = doctor::provider_selection_authority_identity(
					unexecuted_installation.discovery());
				require(std::holds_alternative<std::string>(baseline_selection_identity) &&
							std::holds_alternative<std::string>(unexecuted_selection_identity),
						"selection authority identity fixture failed");
				require(baseline_installation.execution()->candidate_identity() ==
							candidate_id(baseline_installation) &&
							baseline_installation.execution()->selection_semantic_identity() ==
								std::get<std::string>(baseline_selection_identity) &&
							baseline_installation.execution()->candidate_identity() !=
								candidate_id(unexecuted_installation) &&
							baseline_installation.execution()->selection_semantic_identity() !=
								std::get<std::string>(unexecuted_selection_identity),
						"executed A token did not retain exact selection identity");
				std::filesystem::remove(unexecuted_path);
				const doctor::provider_execution_authority_loader execution_loader;
				auto stale_execution = execution_loader.load(
					execution_request(unexecuted_installation.discovery()));
				require(std::holds_alternative<doctor::product_error>(stale_execution),
						"removed B selection minted execution authority");
				auto substitution_project = project(catalog, {unexecuted_installation});
				auto substitution_authority = authority(
					catalog,
					{baseline_installation},
					registry({unexecuted_installation}),
					crypto);
				require(capability(resolved(substitution_project,
										 substitution_authority,
										 catalog_token),
								   "provider.protocol.v2").state ==
							doctor::resolution_state::unknown,
						"executed A authority was substituted for unexecuted B selection");

                const auto replacement_path =
                    std::filesystem::path{argv[0]}.parent_path() / "provider-replaced-image";
                std::filesystem::copy_file(provider_executable_path,
                                           replacement_path,
                                           std::filesystem::copy_options::overwrite_existing);
                const auto binary_tampered_installation = installation(
                    '1',
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                    replacement_path.string());
                auto binary_tampered_registry = registry({binary_tampered_installation});
                {
                    std::ofstream replacement{replacement_path,
                                              std::ios::binary | std::ios::app};
                    replacement.put('\0');
                    require(replacement.good(), "provider replacement fixture failed");
                }
                auto binary_tampered_authority = authority(
                    catalog,
                    {binary_tampered_installation},
                    binary_tampered_registry,
                    crypto);
                auto binary_tampered_project = project(catalog, {binary_tampered_installation});
                require(capability(resolved(binary_tampered_project,
                                            binary_tampered_authority,
                                            catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::proved,
                        "post-launch path replacement changed sealed execution authority");
                std::filesystem::remove(replacement_path);

                const auto missing_path =
                    std::filesystem::path{argv[0]}.parent_path() / "provider-missing-image";
                std::filesystem::copy_file(provider_executable_path,
                                           missing_path,
                                           std::filesystem::copy_options::overwrite_existing);
                const auto argv_changed_installation = installation(
                    '1',
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                    missing_path.string());
                auto argv_changed_registry = registry({argv_changed_installation});
                auto argv_changed_project = project(catalog, {argv_changed_installation});
                std::filesystem::remove(missing_path);
                auto argv_changed_authority = authority(
                    catalog,
                    {argv_changed_installation},
                    argv_changed_registry,
                    crypto);
                require(capability(resolved(argv_changed_project,
                                            argv_changed_authority,
                                            catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::proved,
                        "post-launch path removal changed sealed execution authority");

                const auto invalid_sandbox_installation = installation(
                    '1',
                    cxxlens::sdk::provider::sandbox_assurance::enforced,
                    {},
                    false);
                auto invalid_sandbox_project = project(catalog, {invalid_sandbox_installation});
                auto invalid_sandbox_authority = authority(
                    catalog,
                    {invalid_sandbox_installation},
                    registry({invalid_sandbox_installation}),
                    crypto);
                require(capability(resolved(invalid_sandbox_project,
                                            invalid_sandbox_authority,
                                            catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::unknown,
                        "missing runtime sandbox authority did not fail closed");

				const auto none_rank = doctor::sandbox_assurance_rank("none");
				const auto best_effort_rank = doctor::sandbox_assurance_rank("best_effort");
				const auto enforced_rank = doctor::sandbox_assurance_rank("enforced");
				const auto certified_rank = doctor::sandbox_assurance_rank("certified");
				require(none_rank && best_effort_rank && enforced_rank && certified_rank &&
							*none_rank < *best_effort_rank && *best_effort_rank < *enforced_rank &&
							*enforced_rank < *certified_rank,
						"sandbox assurance lattice is not ordered");

                const auto weak_installation = installation(
                    '1', cxxlens::sdk::provider::sandbox_assurance::best_effort);
                auto weak = project(catalog, {weak_installation});
                auto weak_verifier = authority(
                    catalog, {weak_installation}, registry({weak_installation}), crypto);
                require(capability(resolved(weak, weak_verifier, catalog_token),
                                   "provider.protocol.v2").state ==
                            doctor::resolution_state::disproved,
                        "weak sandbox assurance was accepted");

                auto rejected = baseline;
                rejected.provider_candidates.front().trust.state = doctor::trust_state::rejected;
                rejected.provider_candidates.front().trust.revocation.state = doctor::revocation_state::unknown;
                const auto rejected_result = resolved(rejected, provider_absent, catalog_token);
                require(capability(rejected_result, "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unavailable,
                        "project trust rejection self-claim became authority");

                auto revoked_registry = baseline_registry;
                revoked_registry.revocations.push_back(
                    {revoked_registry.certificates.front().id, 7U, "compromised"});
                reseal_registry(revoked_registry);
                auto revoked = authority(
                    catalog, {baseline_installation}, revoked_registry, crypto);
                require(capability(resolved(baseline, revoked, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_revoked,
                        "installed revocation did not take precedence");

                auto unverified_registry_crypto = crypto;
                unverified_registry_crypto.registry_verdict =
                    doctor::authority_verdict::unverified;
                auto unverified_registry_revocation = authority(
                    catalog,
                    {baseline_installation},
                    revoked_registry,
                    unverified_registry_crypto);
                require(capability(resolved(baseline,
                                            unverified_registry_revocation,
                                            catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_certification_unverified,
                        "unverified registry JSON minted an authoritative revocation");

                auto rejected_registry_crypto = crypto;
                rejected_registry_crypto.registry_verdict = doctor::authority_verdict::rejected;
                auto rejected_registry_revocation = authority(
                    catalog,
                    {baseline_installation},
                    revoked_registry,
                    rejected_registry_crypto);
                const auto rejected_registry_resolution =
                    resolved(baseline, rejected_registry_revocation, catalog_token);
                const auto rejected_registry_result = capability(
                    rejected_registry_resolution, "provider.protocol.v2");
                require(rejected_registry_result.state == doctor::resolution_state::disproved &&
                            rejected_registry_result.reason ==
                                doctor::diagnosis_reason::provider_untrusted,
                        "rejected registry JSON promoted its inner revocation claim");
                require(std::ranges::find(rejected_registry_resolution.provenance,
                                          "provider.registry-sequence=0") ==
                            rejected_registry_resolution.provenance.end(),
                        "rejected registry emitted a synthetic zero sequence");

                auto revoked_crypto = crypto;
                revoked_crypto.certificate_verdict = doctor::authority_verdict::revoked;
                auto signature_revoked = authority(
                    catalog, {baseline_installation}, baseline_registry, revoked_crypto);
                require(capability(resolved(baseline, signature_revoked, catalog_token),
                                   "provider.protocol.v2").reason ==
                            doctor::diagnosis_reason::provider_revoked,
                        "revoked signature component lost revoked precedence");

                auto canonical_registry_a = baseline_registry;
                canonical_registry_a.certificates.front().qualifications.front().toolchains =
                    {"clang22", "clang23"};
                canonical_registry_a.certificates.front().qualifications.front().platforms =
                    {"linux-aarch64", "linux-x86_64"};
                auto canonical_registry_b = canonical_registry_a;
                std::ranges::reverse(
                    canonical_registry_b.certificates.front().qualifications.front().toolchains);
                std::ranges::reverse(
                    canonical_registry_b.certificates.front().qualifications.front().platforms);
                const auto canonical_identity_a =
                    doctor::certification_registry_semantic_identity(canonical_registry_a);
                const auto canonical_identity_b =
                    doctor::certification_registry_semantic_identity(canonical_registry_b);
                require(std::holds_alternative<std::string>(canonical_identity_a) &&
                            std::holds_alternative<std::string>(canonical_identity_b) &&
                            std::get<std::string>(canonical_identity_a) ==
                                std::get<std::string>(canonical_identity_b),
                        "registry semantic identity depended on qualification set order");

                exact_authority_state invalid_registry_state;
                auto duplicate_anchor = baseline_registry;
                duplicate_anchor.trust_anchors.push_back(duplicate_anchor.trust_anchors.front());
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            duplicate_anchor,
                            crypto,
                            invalid_registry_state)),
                        "duplicate trust-anchor ID was accepted");

                auto duplicate_issuer = baseline_registry;
                duplicate_issuer.issuers.push_back(duplicate_issuer.issuers.front());
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            duplicate_issuer,
                            crypto,
                            invalid_registry_state)),
                        "duplicate issuer ID was accepted");

                auto duplicate_certificate = baseline_registry;
                duplicate_certificate.certificates.push_back(
                    duplicate_certificate.certificates.front());
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            duplicate_certificate,
                            crypto,
                            invalid_registry_state)),
                        "duplicate certificate ID was accepted");

                auto duplicate_qualification = baseline_registry;
                duplicate_qualification.certificates.front().qualifications.push_back(
                    duplicate_qualification.certificates.front().qualifications.front());
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            duplicate_qualification,
                            crypto,
                            invalid_registry_state)),
                        "duplicate certificate qualification was accepted");

                auto duplicate_revocation = baseline_registry;
                duplicate_revocation.revocations = {
                    {duplicate_revocation.certificates.front().id, 7U, "compromised"},
                    {duplicate_revocation.certificates.front().id, 7U, "duplicate"}};
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            duplicate_revocation,
                            crypto,
                            invalid_registry_state)),
                        "duplicate revocation subject was accepted");

                auto unknown_issuer = baseline_registry;
                unknown_issuer.certificates.front().issuer_id = "issuer.unknown";
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            unknown_issuer,
                            crypto,
                            invalid_registry_state)),
                        "certificate with unknown issuer was accepted");

                auto unknown_anchor = baseline_registry;
                unknown_anchor.issuers.front().trust_anchor_id = "anchor.unknown";
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            unknown_anchor,
                            crypto,
                            invalid_registry_state)),
                        "issuer with unknown trust anchor was accepted");

                auto unknown_revocation = baseline_registry;
                unknown_revocation.revocations = {{"certificate.unknown", 7U, "compromised"}};
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            unknown_revocation,
                            crypto,
                            invalid_registry_state)),
                        "revocation for unknown certificate was accepted");

                std::vector<doctor::provider_installation_artifact> too_many_installations(
                    doctor::maximum_json_collection_count + 1U, baseline_installation);
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            too_many_installations,
                            baseline_registry,
                            crypto,
                            invalid_registry_state)),
                        "installation collection overflow was accepted");

                auto nested_registry_overflow = baseline_registry;
                auto& large_qualification =
                    nested_registry_overflow.certificates.front().qualifications.front();
                large_qualification.toolchains.clear();
                large_qualification.platforms.clear();
                for (std::size_t index = 0U;
                     index < doctor::maximum_json_collection_count;
                     ++index)
                {
                    large_qualification.toolchains.push_back(
                        "toolchain." + std::to_string(index));
                    large_qualification.platforms.push_back(
                        "platform." + std::to_string(index));
                }
                auto large_qualification_value = large_qualification;
                for (std::size_t index = 1U;
                     index < doctor::maximum_json_collection_count;
                     ++index)
                    nested_registry_overflow.certificates.front().qualifications.push_back(
                        large_qualification_value);
                require(std::holds_alternative<doctor::product_error>(load_authority(
                            catalog,
                            {baseline_installation},
                            nested_registry_overflow,
                            crypto,
                            invalid_registry_state)),
                        "nested authority node overflow was accepted");

                const auto second_installation = installation('2');
                auto conflict_project = project(
                    catalog, {baseline_installation, second_installation});
                auto conflict_verifier = authority(
                    catalog,
                    {baseline_installation, second_installation},
                    registry({baseline_installation, second_installation}),
                    crypto);
                const auto conflict = resolved(conflict_project, conflict_verifier, catalog_token);
                require(conflict.state == doctor::resolution_state::conflicting,
                        "multiple authenticated candidates were not conflicting");
                require(conflict.completion_plan.empty(),
                        "conflict emitted a speculative completion action");
                std::vector<std::string> expected_conflict_ids{
                    candidate_id(baseline_installation), candidate_id(second_installation)};
				std::ranges::sort(expected_conflict_ids);
                for (const auto& item : conflict.conflicts)
                    require(item.candidate_ids == expected_conflict_ids,
                            "dependency conflict candidate IDs were not unioned and sorted");
                auto conflict_permuted = conflict_project;
                std::ranges::reverse(conflict_permuted.provider_candidates);
                require(
                    doctor::canonical_json(doctor::to_json(conflict)) ==
                        doctor::canonical_json(
                            doctor::to_json(resolved(
                                conflict_permuted, conflict_verifier, catalog_token))),
                    "authenticated conflict depended on direct candidate order");

                auto duplicate = baseline;
                duplicate.provider_candidates.push_back(baseline.provider_candidates.front());
                const auto duplicate_result = doctor::resolve(
                    "cxxlens.clang22.materialize-and-query.v1",
                    duplicate,
                    catalog_token,
                    verified);
                require(std::holds_alternative<doctor::product_error>(duplicate_result),
                        "duplicate direct candidate identity was accepted");

                auto tampered_catalog = doctor::sdk_doctor_catalog_value();
                tampered_catalog.store_support.format = "tampered.snapshot";
                const auto tampered_result = loader.load(tampered_catalog);
                require(std::holds_alternative<doctor::product_error>(tampered_result),
                        "catalog semantic identity mismatch was accepted");

                auto tampered_command = doctor::sdk_doctor_catalog_value();
                tampered_command.commands.front().output_schema = "tampered.output.v1";
                require(std::holds_alternative<doctor::product_error>(loader.load(tampered_command)),
                        "catalog command binding mismatch was accepted");
                auto tampered_consumers = doctor::sdk_doctor_catalog_value();
                tampered_consumers.capabilities.front().consumers = {"tampered.consumer"};
                require(std::holds_alternative<doctor::product_error>(
                            loader.load(tampered_consumers)),
                        "catalog consumer binding mismatch was accepted");
				auto tampered_capability_kind = doctor::sdk_doctor_catalog_value();
				tampered_capability_kind.capabilities.front().kind =
					doctor::capability_kind::relation;
				require(std::holds_alternative<doctor::product_error>(
							loader.load(tampered_capability_kind)),
						"catalog kind changed derived resolver behavior without rejection");
				for (const auto& capability_value : doctor::sdk_doctor_catalog_value().capabilities)
					require(doctor::derived_capability_probe(capability_value).has_value(),
							"authenticated catalog capability has no deterministic probe");
                auto tampered_protocol = doctor::sdk_doctor_catalog_value();
                tampered_protocol.provider_support.protocol_downgrade = "allowed";
                require(std::holds_alternative<doctor::product_error>(loader.load(tampered_protocol)),
                        "catalog downgrade policy mismatch was accepted");
                auto tampered_trust = doctor::sdk_doctor_catalog_value();
                tampered_trust.provider_support.trust.revocation = "ignored";
                require(std::holds_alternative<doctor::product_error>(loader.load(tampered_trust)),
                        "catalog trust policy mismatch was accepted");
                auto tampered_candidate_identity = doctor::sdk_doctor_catalog_value();
                tampered_candidate_identity.provider_support.candidate_identity.producer =
                    "project-json";
                require(std::holds_alternative<doctor::product_error>(
                            loader.load(tampered_candidate_identity)),
                        "catalog candidate authority mismatch was accepted");
                auto tampered_tuple_fields = doctor::sdk_doctor_catalog_value();
                tampered_tuple_fields.provider_support.support_tuple_fields.pop_back();
                require(std::holds_alternative<doctor::product_error>(
                            loader.load(tampered_tuple_fields)),
                        "catalog support tuple projection mismatch was accepted");
                auto tampered_conflict = doctor::sdk_doctor_catalog_value();
                tampered_conflict.provider_support.conflict_policy.fallback = "first-wins";
                require(std::holds_alternative<doctor::product_error>(loader.load(tampered_conflict)),
                        "catalog conflict policy mismatch was accepted");

                const doctor::installed_product_authority_verifier unavailable;
                const auto authority_missing = resolved(baseline, unavailable, catalog_token);
                require(authority_missing.state == doctor::resolution_state::unknown,
                        "unverified project catalog minted an established subset");

                auto invalid_catalog = catalog;
                invalid_catalog.catalog_digest = semantic_digest('0');
                invalid_catalog.catalog_id = "catalog:" + invalid_catalog.catalog_digest;
                auto catalog_rejected = authority(
                    invalid_catalog, {baseline_installation}, baseline_registry, crypto);
                auto rejected_project = baseline;
                rejected_project.catalog_id = invalid_catalog.catalog_id;
                rejected_project.catalog_digest = invalid_catalog.catalog_digest;
                const auto disproved =
                    resolved(rejected_project, catalog_rejected, catalog_token);
                require(disproved.state == doctor::resolution_state::disproved &&
                            disproved.reason == doctor::diagnosis_reason::catalog_rejected,
                        "rejected catalog did not make the no-established-subset result disproved");

                const auto installed_source =
                    doctor::installed_authority_source_from_paths(argv[1]);
                require(!installed_source.certification_registry.has_value(),
                        "caller-provided argv path overrode /proc/self/exe authority");
                const auto executable_path = std::filesystem::path{argv[1]};
                const auto existing_path = std::getenv("PATH");
                const auto search_path = executable_path.parent_path().string() + ":" +
                    (existing_path == nullptr ? std::string{} : std::string{existing_path});
                require(setenv("PATH", search_path.c_str(), 1) == 0, "cannot set PATH fixture");
                const auto path_resolved_source =
                    doctor::installed_authority_source_from_paths(executable_path.filename().string());
                require(!path_resolved_source.certification_registry.has_value(),
                        "PATH search overrode /proc/self/exe authority");
                return 0;
            }
            '''
        ),
        encoding="utf-8",
    )
    compile_result = subprocess.run(
        [
            compiler,
            "-std=c++23",
            "-Wall",
            "-Wextra",
            "-Wpedantic",
            "-Werror",
            "-I",
            str(root / "include"),
            "-I",
            str(root / "tools" / "sdk"),
            str(source),
            "-L",
            str(build),
            f"-Wl,-rpath,{build}",
            "-lcxxlens_provider_sdk",
            "-lcxxlens_recipes",
            "-lcxxlens_query",
            "-lcxxlens_kernel",
            "-ldl",
            "-lcxxlens_protocol_v2",
            "-lcxxlens_base",
            "-pthread",
            "-o",
            str(binary),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    require(
        compile_result.returncode == 0,
        f"direct resolver consumer did not compile: {compile_result.stderr}",
    )
    completed = subprocess.run(
        [
            str(binary),
            str(pathlib.Path(executable).resolve()),
            str(expected_catalog_projection),
            str(state_projection),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    require(completed.returncode == 0, f"direct resolver consumer failed: {completed.stderr}")
    require(state_projection.exists(), "direct resolver did not emit state projections")
    projections = json.loads(state_projection.read_text(encoding="utf-8"))
    expected_states = ["proved", "disproved", "unknown", "partial", "conflicting"]
    require(
        [item["result"]["state"] for item in projections] == expected_states,
        "resolver projection did not preserve all five product states",
    )
    expected_coverage = [item["id"] for item in projections[0]["capability_path"]]
    for item in projections:
        validate_product_schema("cxxlens_ng_sdk_doctor_resolution.schema.yaml", item)
        require_product_only_projection(item)
        semantics = item["preserved_semantics"]
        require(semantics["coverage"] == expected_coverage, "coverage was not projected from the capability path")
        require(semantics["provenance"], "resolver provenance was dropped from the projection")
        require(
            all(step.get("reason_code", "").startswith("doctor.") for step in item["completion_plan"]),
            "completion plan projection omitted typed reason_code",
        )
    require(
        projections[0]["preserved_semantics"]["closure"] == ["dependency-graph-closed"]
        and not projections[0]["preserved_semantics"]["unresolved"],
        "proved projection did not preserve closed semantics",
    )
    require(
        projections[2]["preserved_semantics"]["unresolved"]
        and projections[2]["preserved_semantics"]["closure"] == ["dependency-graph-open"],
        "unknown projection collapsed unresolved semantics",
    )
    require(
        projections[4]["preserved_semantics"]["conflict"]
        and "conflict-preserved-no-fallback"
        in projections[4]["preserved_semantics"]["guarantee"],
        "conflicting projection dropped conflict semantics",
    )


def check_unverified_and_deterministic(executable: str, directory: pathlib.Path) -> None:
    first = write_project(directory, "first.json", valid_project())
    reordered = valid_project()
    reordered["project"]["provider_candidates"][0]["features"].reverse()
    reordered["project"]["provider_candidates"][0]["relations"].reverse()
    reordered["project"] = {
        "store": reordered["project"]["store"],
        "provider_candidates": reordered["project"]["provider_candidates"],
        "source_input": reordered["project"]["source_input"],
        "environment": reordered["project"]["environment"],
        "environment_digest": reordered["project"]["environment_digest"],
        "logical_root": reordered["project"]["logical_root"],
        "catalog_digest": reordered["project"]["catalog_digest"],
        "catalog_id": reordered["project"]["catalog_id"],
        "project_id": reordered["project"]["project_id"],
    }
    second = write_project(directory, "second.json", reordered)
    first_run = run(executable, "missing", "--project", str(first), "--use-case", USE_CASE)
    second_run = run(executable, "missing", "--project", str(second), "--use-case", USE_CASE)
    require(first_run.returncode == 1, f"unverified resolver exited {first_run.returncode}: {first_run.stderr}")
    require(second_run.returncode == 1, f"reordered resolver exited {second_run.returncode}: {second_run.stderr}")
    require(first_run.stdout == second_run.stdout, "JSON projection depends on input object order")
    report = json.loads(first_run.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_resolution.schema.yaml", report)
    require_product_only_projection(report)
    require(report["schema"] == "cxxlens.sdk-doctor-resolution.v2", "resolution schema mismatch")
    require(
        report["catalog_binding"]
        == {"id": "cxxlens.sdk-doctor-catalog.v1", "document_version": "1.0.0"},
        "resolution did not bind the typed catalog",
    )
    require(report["result"]["state"] == "unknown", "JSON self-claims minted a proved result")
    require(
        report["result"]["reason_code"] == "doctor.catalog-unavailable",
        "missing installed project catalog authority reason changed",
    )
    require(report["missing"], "unverified context lost its missing capabilities")
    require(
        [item["unlocks"] for item in report["completion_plan"]]
        == ["input.project-catalog.v1"],
        "completion plan did not stop at the first actionable authority frontier",
    )
    require(
        all(item["reason_code"].startswith("doctor.") for item in report["completion_plan"]),
        "completion plan omitted typed reason codes",
    )
    require(report["preserved_semantics"]["unresolved"], "unknown capability was collapsed")
    markdown_one = run(
        executable,
        "missing",
        "--project",
        str(first),
        "--use-case",
        USE_CASE,
        "--format",
        "markdown",
    )
    markdown_two = run(
        executable,
        "missing",
        "--project",
        str(first),
        "--use-case",
        USE_CASE,
        "--format",
        "markdown",
    )
    require(markdown_one.returncode == 1 and markdown_two.returncode == 1, "markdown projection failed")
    require(markdown_one.stdout == markdown_two.stdout, "markdown projection is not deterministic")
    require(markdown_one.stdout.startswith("# cxxlens SDK capability diagnosis\n"), "markdown heading missing")
    require(
        markdown_one.stdout
        == "# cxxlens SDK capability diagnosis\n\n```json\n" + first_run.stdout.rstrip("\n") + "\n```\n",
        "JSON and Markdown projections are not semantically identical",
    )


def check_missing_and_completion_plan(executable: str, directory: pathlib.Path) -> None:
    missing_provider = write_project(directory, "missing-provider.json", valid_project(providers=0))
    completed = run(executable, "missing", "--project", str(missing_provider), "--use-case", USE_CASE)
    require(completed.returncode == 1, f"missing provider exit code was {completed.returncode}")
    report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_resolution.schema.yaml", report)
    require_product_only_projection(report)
    require(report["result"]["state"] == "unknown", "unverified catalog was not unknown")
    require(
        report["result"]["reason_code"] == "doctor.catalog-unavailable",
        "catalog authority reason changed",
    )
    require(
        [item["unlocks"] for item in report["completion_plan"]]
        == ["input.project-catalog.v1"],
        "completion plan escaped the first authority frontier",
    )
    require(
        all(item["reason_code"].startswith("doctor.") for item in report["completion_plan"]),
        "completion plan omitted typed reason codes",
    )
    require(report["preserved_semantics"]["unresolved"], "unknown capability was collapsed")
    markdown = run(
        executable,
        "missing",
        "--project",
        str(missing_provider),
        "--use-case",
        USE_CASE,
        "--format",
        "markdown",
    )
    require(markdown.returncode == 1, "unknown Markdown diagnosis changed the exit code")
    require(
        markdown.stdout
        == "# cxxlens SDK capability diagnosis\n\n```json\n" + completed.stdout.rstrip("\n") + "\n```\n",
        "non-proved JSON and Markdown projections differ",
    )

    # Every emitted completion step is dependency ordered. Downstream partial
    # capabilities are intentionally omitted until the authority frontier is closed.
    path_positions = {
        item["id"]: position for position, item in enumerate(report["capability_path"])
    }
    plan_positions = [path_positions[item["unlocks"]] for item in report["completion_plan"]]
    require(plan_positions == sorted(plan_positions), "completion plan is not dependency ordered")
    for item in report["completion_plan"]:
        unlock_position = path_positions[item["unlocks"]]
        require(
            all(path_positions[dependency] < unlock_position for dependency in item["requires"]),
            f"completion step {item['unlocks']} depends on a later capability",
        )

    # Unsupported tuple claims are not authoritative without the installed
    # project catalog/certification binding; the CLI remains fail-closed unknown.
    unsupported_environment = valid_project()
    unsupported_environment["project"]["environment"]["os"] = "windows"
    unsupported = write_project(directory, "unsupported-environment.json", unsupported_environment)
    completed = run(executable, "missing", "--project", str(unsupported), "--use-case", USE_CASE)
    require(completed.returncode == 1, "unsupported environment should not be successful")
    report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_resolution.schema.yaml", report)
    require_product_only_projection(report)
    require(
        report["result"]["state"] == "unknown"
        and report["result"]["reason_code"] == "doctor.catalog-unavailable",
        "unsupported self-claim bypassed missing catalog authority",
    )


def check_provider_trust_and_conflict(executable: str, directory: pathlib.Path) -> None:
    untrusted = valid_project()
    untrusted["project"]["provider_candidates"][0]["trust"]["state"] = "rejected"
    untrusted["project"]["provider_candidates"][0]["trust"]["revocation"] = {
        "state": "unknown",
        "effective_sequence": None,
        "reason": None,
    }
    path = write_project(directory, "untrusted.json", untrusted)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 1, "rejected trust did not return not-proved")
    report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_resolution.schema.yaml", report)
    require(report["result"]["state"] != "proved", "project trust self-claim minted authority")

    revoked = valid_project()
    revoked_trust = revoked["project"]["provider_candidates"][0]["trust"]
    revoked_trust["state"] = "rejected"
    revoked_trust["revocation"] = {
        "state": "revoked",
        "effective_sequence": 8,
        "reason": "compromised",
    }
    path = write_project(directory, "revoked.json", revoked)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 1, "revoked provider did not return not-proved")
    report = json.loads(completed.stdout)
    require(report["result"]["state"] != "proved", "project revocation self-claim minted authority")

    conflict = valid_project()
    second = valid_candidate(candidate_digit="2", binary_digit="8")
    conflict["project"]["provider_candidates"].append(second)
    path = write_project(directory, "conflict.json", conflict)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 1, "conflicting providers did not return not-proved")
    report = json.loads(completed.stdout)
    require(report["result"]["state"] == "unknown", "unverified candidates minted conflict authority")

    permuted = valid_project()
    permuted["project"]["provider_candidates"] = [second, valid_candidate()]
    permuted_path = write_project(directory, "conflict-permuted.json", permuted)
    permuted_run = run(
        executable, "missing", "--project", str(permuted_path), "--use-case", USE_CASE
    )
    require(permuted_run.returncode == 1, "permuted conflict did not return not-proved")
    require(completed.stdout == permuted_run.stdout, "conflict resolution depends on candidate order")


def check_operational_input_is_rejected(executable: str, directory: pathlib.Path) -> None:
    """Repository metadata is not a doctor product-input compatibility surface."""
    for field in sorted(OPERATIONAL_FIELDS):
        project = valid_project()
        project["project"][field] = "forbidden"
        path = write_project(directory, f"forbidden-{field.replace('-', '_')}.json", project)
        completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
        require(completed.returncode == 2, f"forbidden project field {field} was accepted")
        require("unknown-field" in completed.stderr, f"forbidden project field {field} was not typed")


def check_strict_and_fault_inputs(executable: str, directory: pathlib.Path) -> None:
    duplicate = (
        '{"schema":"cxxlens.sdk-doctor-project.v2","document_version":"2.0.0",'
        '"project":{"project_id":"one","project_id":"two"}}'
    )
    duplicate_path = directory / "duplicate.json"
    duplicate_path.write_text(duplicate, encoding="utf-8")
    completed = run(executable, "missing", "--project", str(duplicate_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "duplicate project field did not fail closed")
    require("doctor.project-invalid" in completed.stderr and "duplicate-key" in completed.stderr, "duplicate reason missing")

    unknown = valid_project()
    unknown["project"]["unexpected"] = True
    unknown_path = write_project(directory, "unknown.json", unknown)
    completed = run(executable, "missing", "--project", str(unknown_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "unknown project field did not fail closed")
    require("unknown-field" in completed.stderr, "unknown-field reason missing")

    invalid_utf8 = directory / "invalid-utf8.json"
    invalid_utf8.write_bytes(
        b'{"schema":"cxxlens.sdk-doctor-project.v2","document_version":"2.0.0",'
        b'"project":{"project_id":"\xff"}}'
    )
    completed_bytes = subprocess.run(
        [executable, "missing", "--project", str(invalid_utf8), "--use-case", USE_CASE],
        capture_output=True,
        check=False,
    )
    require(completed_bytes.returncode == 2, "invalid UTF-8 did not fail closed")
    require(b"invalid-utf8" in completed_bytes.stderr, "invalid UTF-8 reason missing")

    completed = run(executable, "missing", "--use-case", USE_CASE)
    require(completed.returncode == 2 and "doctor.project-required" in completed.stderr, "missing project option was not typed")

    oversized = directory / "oversized.json"
    oversized.write_bytes(b" " * (1024 * 1024 + 1))
    completed = run(executable, "missing", "--project", str(oversized), "--use-case", USE_CASE)
    require(completed.returncode == 2, "oversized project was not rejected")
    require("doctor.project-invalid" in completed.stderr and "byte-limit" in completed.stderr,
            "oversized project did not report the resource bound")

    hundred_k_string = directory / "hundred-k-string.json"
    hundred_k_string.write_text('{"x":"' + "x" * 100_000 + '"}', encoding="utf-8")
    completed = run(
        executable, "missing", "--project", str(hundred_k_string), "--use-case", USE_CASE
    )
    require(completed.returncode == 2, "100k string was not bounded")
    require("string-byte-limit" in completed.stderr, "100k string did not hit the text bound")

    too_deep = directory / "too-deep.json"
    too_deep.write_text("[" * 65 + "0" + "]" * 65, encoding="utf-8")
    completed = run(executable, "missing", "--project", str(too_deep), "--use-case", USE_CASE)
    require(completed.returncode == 2, "deep project was not rejected")
    require("doctor.project-invalid" in completed.stderr and "depth-limit" in completed.stderr,
            "deep project did not report the parser resource bound")

    too_many = valid_project()
    too_many["project"]["provider_candidates"][0]["features"] = [
        f"feature-{index}" for index in range(129)
    ]
    too_many_path = write_project(directory, "too-many-features.json", too_many)
    completed = run(executable, "missing", "--project", str(too_many_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "oversized feature set was not rejected")
    require("array-count-limit" in completed.stderr, "feature bound reason missing")

    long_id = valid_project()
    long_id["project"]["project_id"] = "x" * 513
    long_id_path = write_project(directory, "long-id.json", long_id)
    completed = run(executable, "missing", "--project", str(long_id_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "oversized identifier was not rejected")
    require("string-byte-limit" in completed.stderr, "identifier byte bound reason missing")

    leading_zero = json.dumps(valid_project(), ensure_ascii=True, separators=(",", ":"))
    leading_zero = leading_zero.replace('"registry_sequence":7', '"registry_sequence":01')
    leading_zero_path = directory / "leading-zero.json"
    leading_zero_path.write_text(leading_zero, encoding="utf-8")
    completed = run(
        executable, "missing", "--project", str(leading_zero_path), "--use-case", USE_CASE
    )
    require(completed.returncode == 2, "leading-zero JSON number was accepted")
    require("leading-zero-number" in completed.stderr, "leading-zero number reason missing")

    empty_logical_root = valid_project()
    empty_logical_root["project"]["logical_root"] = "project://"
    path = write_project(directory, "empty-logical-root.json", empty_logical_root)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "empty project URI suffix was accepted")
    require("project-uri-required" in completed.stderr, "logical root reason missing")

    unknown_sandbox = valid_project()
    unknown_sandbox["project"]["provider_candidates"][0]["sandbox"]["minimum"] = "magic"
    path = write_project(directory, "unknown-sandbox.json", unknown_sandbox)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "unknown sandbox assurance was ranked")
    require("invalid-direct-candidate" in completed.stderr, "sandbox assurance reason missing")

    duplicate_candidate = valid_project()
    duplicate_candidate["project"]["provider_candidates"].append(
        valid_candidate(binary_digit="8")
    )
    duplicate_path = write_project(directory, "duplicate-candidate.json", duplicate_candidate)
    completed = run(executable, "missing", "--project", str(duplicate_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "duplicate canonical candidate identity was accepted")
    require("duplicate-candidate-id" in completed.stderr, "duplicate candidate reason missing")

    opaque_candidate = valid_project()
    opaque_candidate["project"]["provider_candidates"][0]["candidate_id"] = "provider.one"
    path = write_project(directory, "opaque-candidate.json", opaque_candidate)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "noncanonical candidate identity was accepted")
    require("invalid-provider-identity" in completed.stderr, "candidate identity reason missing")

    mismatched_catalog = valid_project()
    mismatched_catalog["project"]["catalog_id"] = "catalog:" + semantic_digest("0")
    mismatch_path = write_project(directory, "catalog-mismatch.json", mismatched_catalog)
    completed = run(executable, "missing", "--project", str(mismatch_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "catalog ID/digest mismatch was accepted")
    require("catalog-id-digest-mismatch" in completed.stderr, "catalog binding reason missing")

    inconsistent_trust = valid_project()
    inconsistent_trust["project"]["provider_candidates"][0]["trust"]["signature_digest"] = None
    inconsistent_path = write_project(directory, "inconsistent-trust.json", inconsistent_trust)
    completed = run(executable, "missing", "--project", str(inconsistent_path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "inconsistent verified trust was accepted")
    require("inconsistent-verified-trust" in completed.stderr, "trust consistency reason missing")

    inconsistent_revocation = valid_project()
    revocation = inconsistent_revocation["project"]["provider_candidates"][0]["trust"]["revocation"]
    revocation["effective_sequence"] = 9
    path = write_project(directory, "inconsistent-revocation.json", inconsistent_revocation)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "partial revocation binding was accepted")
    require("inconsistent-revocation" in completed.stderr, "revocation consistency reason missing")

    invalid_store = valid_project()
    invalid_store["project"]["store"] = {"backend": "filesystem", "format": "old.snapshot"}
    path = write_project(directory, "invalid-store.json", invalid_store)
    completed = run(executable, "missing", "--project", str(path), "--use-case", USE_CASE)
    require(completed.returncode == 2, "out-of-schema store tuple became a diagnosis")
    require("unsupported-schema-value" in completed.stderr, "store schema reason missing")

    unknown_use_case = run(
        executable,
        "missing",
        "--project",
        str(write_project(directory, "unknown-use-case.json", valid_project())),
        "--use-case",
        "cxxlens.unknown.v1",
    )
    require(unknown_use_case.returncode == 2, "unknown use case did not return invalid-request")
    require("doctor.unknown-use-case" in unknown_use_case.stderr, "unknown use-case reason missing")

    valid_path = write_project(directory, "duplicate-options.json", valid_project())
    duplicate_options = (
        ("--project", str(valid_path), "--project", str(valid_path), "--use-case", USE_CASE),
        ("--project", str(valid_path), "--use-case", USE_CASE, "--use-case=" + USE_CASE),
        ("--project", str(valid_path), "--use-case", USE_CASE, "--format", "json", "--format=json"),
    )
    for arguments in duplicate_options:
        completed = run(executable, "missing", *arguments)
        require(completed.returncode == 2, f"duplicate option was accepted: {arguments}")
        require("duplicate-option" in completed.stderr, "duplicate option reason missing")


def check_relation_presence(executable: str) -> None:
    for legacy_command in ("inspect", "doctor", "query-ir", "provider-manifest"):
        completed = run(executable, legacy_command)
        require(
            completed.returncode == 2 and completed.stdout == "",
            f"legacy command {legacy_command} was still accepted",
        )

    completed = run(
        executable,
        "relation-presence",
        "build.project.v1",
        "cc.call_site.v1",
        "source.span.v1",
    )
    require(completed.returncode == 0, f"relation presence exited {completed.returncode}: {completed.stderr}")
    report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_relation_presence.schema.yaml", report)
    require_product_only_projection(report)
    require(report["schema"] == "cxxlens.sdk-doctor-relation-presence.v2", "relation schema mismatch")
    require(report["state"] == "proved" and report["missing"] == 0, "known relations were not proved")

    expected_ids = installed_static_relation_ids()
    completed = run(executable, "relation-presence", *expected_ids)
    require(completed.returncode == 0, "YAML installed-static relation registry was not fully proved")
    yaml_report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_relation_presence.schema.yaml", yaml_report)
    require_product_only_projection(yaml_report)
    require(
        [component["id"] for component in yaml_report["components"]] == expected_ids
        and all(component["state"] == "proved" for component in yaml_report["components"]),
        "known relation registry diverged from the YAML installed-static projection",
    )
    markdown = run(executable, "relation-presence", *expected_ids, "--format", "markdown")
    require(markdown.returncode == 0, "known relation Markdown projection failed")
    require(
        markdown.stdout.startswith("# cxxlens SDK capability diagnosis\n\n```json\n")
        and markdown.stdout.endswith("\n```\n"),
        "relation Markdown envelope changed",
    )
    markdown_report = json.loads(
        markdown.stdout[len("# cxxlens SDK capability diagnosis\n\n```json\n") : -len("\n```\n")]
    )
    require(markdown_report == yaml_report, "relation JSON and Markdown projections diverged")

    registry = yaml.safe_load(
        (ROOT / "schemas" / "cxxlens_ng_relation_registry.yaml").read_text(encoding="utf-8")
    )
    dynamic_only = next(
        relation
        for relation in registry["relations"]
        if relation.get("cpp_projection") != "installed-static"
    )
    dynamic_id = f"{dynamic_only['name']}.v{dynamic_only['semantic_major']}"
    completed = run(executable, "relation-presence", dynamic_id)
    require(completed.returncode == 1, "dynamic-only relation leaked into installed static registry")
    dynamic_report = json.loads(completed.stdout)
    require(
        dynamic_report["components"][0]["state"] == "unknown"
        and dynamic_report["components"][0]["reason_code"] == "sdk.relation-not-found",
        "dynamic-only relation did not remain an unknown product capability",
    )

    completed = run(executable, "relation-presence", "cc.call_site.v1", "cc.does_not_exist.v1")
    require(completed.returncode == 1, "unknown relation should return incomplete")
    report = json.loads(completed.stdout)
    validate_product_schema("cxxlens_ng_sdk_doctor_relation_presence.schema.yaml", report)
    require_product_only_projection(report)
    absent = next(item for item in report["components"] if item["id"] == "cc.does_not_exist.v1")
    require(absent["state"] == "unknown" and absent["reason_code"] == "sdk.relation-not-found", "unknown relation reason changed")

    completed = run(executable, "relation-presence", "cc.call_site.v2")
    require(completed.returncode == 1, "relation major mismatch should be incomplete")
    mismatch_report = json.loads(completed.stdout)
    require_product_only_projection(mismatch_report)
    require(mismatch_report["components"][0]["reason_code"] == "sdk.relation-major-mismatch", "major mismatch reason changed")

    completed = run(executable, "relation-presence", "cc.call_site.v1", "cc.call_site.v1")
    duplicate_report = json.loads(completed.stdout)
    require_product_only_projection(duplicate_report)
    require(completed.returncode == 0 and duplicate_report["requested"] == 2, "duplicate relation requests were not projected independently")

    completed = run(executable, "relation-presence", "cc.call_site.vX")
    require(completed.returncode == 2 and completed.stdout == "", "malformed relation ID did not fail closed")
    require("doctor.relation-request-invalid" in completed.stderr, "malformed relation reason missing")

    completed = run(executable, "relation-presence", "foo.v1")
    require(completed.returncode == 2, "one-segment relation name escaped schema grammar")

    completed = run(executable, "relation-presence", "cc.call_site.v" + "9" * 100)
    require(completed.returncode == 1, "schema-valid large relation major was rejected as malformed")
    require(
        json.loads(completed.stdout)["components"][0]["reason_code"]
        == "sdk.relation-major-mismatch",
        "large relation major mismatch reason changed",
    )

    exact_boundary_id = "a." + "b" * 507 + ".v1"
    require(len(exact_boundary_id) == 512, "relation boundary fixture is not exact")
    completed = run(executable, "relation-presence", exact_boundary_id)
    require(completed.returncode == 1, "512-byte relation ID was rejected as invalid")

    completed = run(executable, "relation-presence", exact_boundary_id + "x")
    require(completed.returncode == 2, "513-byte relation ID was accepted")
    require("byte-limit" in completed.stderr, "relation byte bound reason missing")

    completed = run(executable, "relation-presence", *(["cc.call_site.v1"] * 128))
    require(completed.returncode == 0, "exact 128-relation boundary was rejected")
    require(json.loads(completed.stdout)["requested"] == 128, "128 relation requests were truncated")

    completed = run(executable, "relation-presence", *(["cc.call_site.v1"] * 129))
    require(completed.returncode == 2, "129 relation requests were accepted")
    require("count-limit" in completed.stderr, "relation count bound reason missing")

    completed = run(
        executable,
        "relation-presence",
        "cc.call_site.v1",
        "--format",
        "json",
        "--format=markdown",
    )
    require(completed.returncode == 2, "duplicate relation format option was accepted")
    require("duplicate-option" in completed.stderr, "duplicate relation option reason missing")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="cxxlens-sdk-doctor-product-") as raw_directory:
        directory = pathlib.Path(raw_directory)
        check_direct_authenticated_resolver(args.executable, directory)
        check_unverified_and_deterministic(args.executable, directory)
        check_missing_and_completion_plan(args.executable, directory)
        check_provider_trust_and_conflict(args.executable, directory)
        check_operational_input_is_rejected(args.executable, directory)
        check_strict_and_fault_inputs(args.executable, directory)
    check_relation_presence(args.executable)
    return 0


if __name__ == "__main__":
    sys.exit(main())
