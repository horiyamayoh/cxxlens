#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

namespace
{
	using cxxlens::sdk::canonical_value;

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
} // namespace

int main()
{
	auto content = source_bytes();
	const auto source_digest = cxxlens::sdk::content_digest(content);
	const auto source_size = static_cast<std::int64_t>(content.size());
	auto toolchain = canonical_value::from_tuple({
		canonical_value::from_string("gcc"),
		canonical_value::from_string("16.2.0"),
		observed(canonical_value::from_string("/opt/gcc-16.2.0/bin/g++")),
		observed(canonical_value::from_string(digest('1'))),
		canonical_value::from_string("x86_64-linux-gnu"),
		observed(canonical_value::from_string("/opt/gcc-16.2.0/sysroot")),
		canonical_value::from_string(digest('2')),
		canonical_value::from_string(digest('3')),
		canonical_value::from_string(digest('4')),
		canonical_value::from_string(digest('5')),
	});
	auto unit = canonical_value::from_tuple({
		canonical_value::from_string("compile-unit:main"),
		canonical_value::from_string("source-snapshot:one"),
		canonical_value::from_string("source-file:main"),
		canonical_value::from_string("project://src/main.cpp"),
		canonical_value::from_string(source_digest),
		canonical_value::from_integer(source_size),
		canonical_value::from_string("project://build"),
		canonical_value::from_string("c++"),
		observed(canonical_value::from_tuple({
			canonical_value::from_string("/opt/gcc-16.2.0/bin/g++"),
			canonical_value::from_string("-std=c++23"),
			canonical_value::from_string("project://src/main.cpp"),
		})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_string("/workspace/example/build")),
	});
	auto closure = canonical_value::from_tuple({
		canonical_value::from_string("source-closure:one"),
		canonical_value::from_string(digest('7')),
		canonical_value::from_string(digest('8')),
		canonical_value::from_integer(1),
		canonical_value::from_integer(1),
		canonical_value::from_integer(source_size),
		canonical_value::from_tuple({canonical_value::from_tuple({
			canonical_value::from_string("source-file:main"),
			canonical_value::from_string("project://src/main.cpp"),
			observed(canonical_value::from_string(source_digest)),
			observed(canonical_value::from_bytes(std::move(content))),
			canonical_value::from_integer(source_size),
		})}),
	});
	auto encoded_members = cxxlens::sdk::canonical_binary(closure.tuple[6]);
	if (!encoded_members)
		return 1;
	closure.tuple[2] = canonical_value::from_string(cxxlens::sdk::content_digest(*encoded_members));
	const std::array closure_fields{
		closure.tuple[2], closure.tuple[3], closure.tuple[4], closure.tuple[5]};
	auto closure_digest =
		cxxlens::sdk::canonical_identity_digest("application-source-closure", closure_fields);
	if (!closure_digest)
		return 1;
	closure.tuple[1] = canonical_value::from_string(std::move(*closure_digest));
	auto encoded = cxxlens::sdk::canonical_binary(canonical_value::from_tuple({
		canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
		std::move(toolchain),
		canonical_value::from_string("shell-free-wrapper"),
		canonical_value::from_string("x86_64-linux-gnu"),
		canonical_value::from_string("project:gcc-example"),
		canonical_value::from_tuple({std::move(unit)}),
		std::move(closure),
		canonical_value::from_tuple({}),
		canonical_value::from_string("project://"),
		observed(canonical_value::from_tuple({canonical_value::from_tuple({
			canonical_value::from_string("/workspace/example"),
			canonical_value::from_string("project://"),
		})})),
	}));
	if (!encoded)
		return 1;
	auto capture = cxxlens::sdk::decode_capture_bundle(*encoded);
	if (!capture || capture->compile_unit_count() != 1U || !capture->gaps().empty())
		return 2;
	// Phase 3.0 exposes the bounded capture surface while the target remains explicitly planned.
	auto imported = cxxlens::sdk::import_capture(*capture);
	return !imported && imported.error().code == "application-analysis.target-unavailable" ? 0 : 3;
}
