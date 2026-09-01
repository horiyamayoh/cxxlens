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
	const std::array file_fields{
		canonical_value::from_string("project"),
		canonical_value::from_string("src/main.cpp"),
		canonical_value::from_string("cxxlens.logical-path.v1"),
	};
	auto source_file_id = cxxlens::sdk::canonical_identity_digest("file", file_fields);
	if (!source_file_id)
		return 1;
	const std::array snapshot_fields{
		canonical_value::from_string(*source_file_id),
		canonical_value::from_string(source_digest),
		canonical_value::from_string("utf8"),
	};
	auto source_snapshot =
		cxxlens::sdk::canonical_identity_digest("source-snapshot", snapshot_fields);
	if (!source_snapshot)
		return 1;
	auto toolchain = canonical_value::from_tuple({
		canonical_value::from_string("gcc"),
		canonical_value::from_string("16.2.0"),
		observed(canonical_value::from_string("/opt/gcc-16.2.0/bin/g++")),
		observed(canonical_value::from_string(digest('1'))),
		canonical_value::from_string("x86_64-linux-gnu"),
		observed(canonical_value::from_string("/opt/gcc-16.2.0/sysroot")),
		observed(canonical_value::from_string(digest('2'))),
		observed(canonical_value::from_string(digest('3'))),
		observed(canonical_value::from_string(digest('4'))),
		observed(canonical_value::from_string(digest('5'))),
	});
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
			canonical_value::from_string("-std=c++23"),
			canonical_value::from_string("/workspace/example/src/main.cpp"),
		})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_tuple({})),
		observed(canonical_value::from_string("/workspace/example/build")),
		observed(canonical_value::from_string("c++23")),
		observed(canonical_value::from_string("strict")),
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
	closure.tuple[0] = canonical_value::from_string("source-closure:" + closure.tuple[1].text);
	unit.tuple[15] = closure.tuple[0];
	auto encoded = cxxlens::sdk::canonical_binary(canonical_value::from_tuple({
		canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
		std::move(toolchain),
		canonical_value::from_string("shell-free-wrapper"),
		canonical_value::from_string("x86_64-linux-gnu"),
		canonical_value::from_string("project:gcc-example"),
		canonical_value::from_tuple({std::move(unit)}),
		canonical_value::from_tuple({std::move(closure)}),
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
	auto imported = cxxlens::sdk::import_capture(*capture);
	if (!imported || imported->replay_plans().size() != 1U ||
		imported->replay_plans().front().analysis_frontend() != "clang-23.1.0-gcc-mode")
		return 3;
	return 0;
}
