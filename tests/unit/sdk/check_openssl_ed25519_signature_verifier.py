#!/usr/bin/env python3
"""Compile and exercise the source-private OpenSSL Ed25519 verifier boundary."""

from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import textwrap


ROOT = pathlib.Path(__file__).resolve().parents[3]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"check_openssl_ed25519_signature_verifier: {message}")


def main() -> int:
    compiler = os.environ.get("CXX", "") or shutil.which("clang++-22") or shutil.which("clang++") or shutil.which("c++")
    require(bool(compiler), "no C++ compiler found")
    source_text = textwrap.dedent(
        r'''
        #include "openssl_ed25519_signature_verifier.hpp"

        #include <openssl/evp.h>

        #include <array>
        #include <cstddef>
        #include <cstdio>
        #include <cstdlib>
        #include <memory>
        #include <optional>
        #include <span>
        #include <string>
        #include <string_view>

        namespace doctor = cxxlens::sdk::doctor;

        [[noreturn]] void fail(const char* message)
        {
            std::fprintf(stderr, "%s\\n", message);
            std::exit(1);
        }

        void require(const bool condition, const char* message)
        {
            if (!condition)
                fail(message);
        }

        std::string sha256(const std::span<const std::byte> bytes)
        {
            std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
            unsigned int digest_size{};
            using context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
            context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
            require(context && EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) == 1,
                    "sha256 initialization failed");
            require(EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1,
                    "sha256 update failed");
            require(EVP_DigestFinal_ex(context.get(), digest.data(), &digest_size) == 1 &&
                        digest_size == 32U,
                    "sha256 finalization failed");
            static constexpr char hex[] = "0123456789abcdef";
            std::string result{"sha256:"};
            for (unsigned int index = 0U; index < digest_size; ++index)
            {
                result.push_back(hex[(digest[index] >> 4U) & 0x0fU]);
                result.push_back(hex[digest[index] & 0x0fU]);
            }
            return result;
        }

        struct generated_key
        {
            using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
            key_ptr key{nullptr, EVP_PKEY_free};
            std::array<std::byte, doctor::ed25519_public_key_bytes> public_key{};
        };

        generated_key generate_key()
        {
            using context_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
            context_ptr context{EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr),
                                EVP_PKEY_CTX_free};
            require(context && EVP_PKEY_keygen_init(context.get()) == 1,
                    "Ed25519 keygen initialization failed");
            generated_key output;
            EVP_PKEY* raw_key{};
            require(EVP_PKEY_keygen(context.get(), &raw_key) == 1 && raw_key != nullptr,
                    "Ed25519 key generation failed");
            output.key.reset(raw_key);
            std::size_t public_key_size = output.public_key.size();
            require(EVP_PKEY_get_raw_public_key(
                        output.key.get(),
                        reinterpret_cast<unsigned char*>(output.public_key.data()),
                        &public_key_size) == 1 && public_key_size == output.public_key.size(),
                    "Ed25519 public key extraction failed");
            return output;
        }

        std::array<std::byte, doctor::ed25519_signature_bytes>
        sign(EVP_PKEY* key, const std::string_view message)
        {
            using context_ptr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
            context_ptr context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
            require(context && EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1,
                    "Ed25519 signing initialization failed");
            std::array<std::byte, doctor::ed25519_signature_bytes> signature{};
            std::size_t signature_size = signature.size();
            require(EVP_DigestSign(
                        context.get(),
                        reinterpret_cast<unsigned char*>(signature.data()),
                        &signature_size,
                        reinterpret_cast<const unsigned char*>(message.data()),
                        message.size()) == 1 && signature_size == signature.size(),
                    "Ed25519 signing failed");
            return signature;
        }

        class material_port final : public doctor::trusted_ed25519_signature_material_port
        {
          public:
            std::optional<doctor::trusted_ed25519_signature_material> value;

            [[nodiscard]] std::optional<doctor::trusted_ed25519_signature_material> lookup(
                const std::string_view,
                const std::string_view,
                const std::string_view,
                const std::string_view) const override
            {
                return value;
            }
        };

        int main()
        {
            const auto key = generate_key();
            const std::string subject = "sha256:" + std::string(64U, 'a');
            const auto signature = sign(key.key.get(), subject);
            doctor::trusted_ed25519_signature_material material;
            material.scope = "provider-certificate";
            material.signer_id = "issuer.example";
            material.key_fingerprint = sha256(key.public_key);
            material.public_key = key.public_key;
            material.signature = signature;
            const auto signature_digest = sha256(material.signature);

            material_port port;
            port.value = material;
            const doctor::openssl_ed25519_signature_verifier verifier{port};
            const auto verified = verifier.verify(
                material.scope, material.signer_id, subject, signature_digest);
            require(verified.verdict == doctor::authority_verdict::verified,
                    "valid Ed25519 signature was not verified");
            require(verified.verifier_id == "openssl.evp-ed25519.v1",
                    "unexpected verifier identity");
            require(verified.key_fingerprint == material.key_fingerprint,
                    "verified key fingerprint was not retained");
            require(verified.signed_subject_digest == subject &&
                        verified.signature_digest == signature_digest,
                    "verified subject/signature binding was not retained");

            const auto changed_subject = verifier.verify(
                material.scope, material.signer_id, subject + "0", signature_digest);
            require(changed_subject.verdict == doctor::authority_verdict::rejected,
                    "subject mutation escaped Ed25519 verification");

            const auto changed_signature_digest = verifier.verify(
                material.scope,
                material.signer_id,
                subject,
                "sha256:" + std::string(64U, 'b'));
            require(changed_signature_digest.verdict == doctor::authority_verdict::rejected,
                    "signature digest mutation escaped material binding");

            const auto signer_mismatch = verifier.verify(
                material.scope, "other-issuer.example", subject, signature_digest);
            require(signer_mismatch.verdict == doctor::authority_verdict::rejected,
                    "signer identity mutation escaped material binding");

            const auto malformed_digest = verifier.verify(
                material.scope, material.signer_id, subject, "not-a-digest");
            require(malformed_digest.verdict == doctor::authority_verdict::rejected,
                    "malformed signature identity was not rejected");

            auto changed_key_fingerprint = material;
            changed_key_fingerprint.key_fingerprint = "sha256:" + std::string(64U, 'b');
            port.value = changed_key_fingerprint;
            const auto key_mismatch = verifier.verify(
                material.scope, material.signer_id, subject, signature_digest);
            require(key_mismatch.verdict == doctor::authority_verdict::rejected,
                    "public-key fingerprint mismatch was not rejected");

            port.value.reset();
            const auto unavailable = verifier.verify(
                material.scope, material.signer_id, subject, signature_digest);
            require(unavailable.verdict == doctor::authority_verdict::unverified &&
                        unavailable.verifier_id.empty(),
                    "unavailable trust material was promoted or misclassified");

            return 0;
        }
        ''',
    )
    with tempfile.TemporaryDirectory(prefix="cxxlens-ed25519-") as temporary:
        directory = pathlib.Path(temporary)
        source = directory / "verifier.cpp"
        binary = directory / "verifier"
        source.write_text(source_text, encoding="utf-8")
        result = subprocess.run(
            [
                compiler,
                "-std=c++23",
                "-Wall",
                "-Wextra",
                "-Wpedantic",
                "-Werror",
                "-I",
                str(ROOT / "include"),
                "-I",
                str(ROOT / "tools" / "sdk"),
                str(source),
                "-lcrypto",
                "-o",
                str(binary),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        require(result.returncode == 0, f"verifier consumer did not compile: {result.stderr}")
        completed = subprocess.run([str(binary)], capture_output=True, text=True, check=False)
        require(completed.returncode == 0, f"verifier consumer failed: {completed.stderr}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
