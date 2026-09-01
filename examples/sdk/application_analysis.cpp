#include <cstddef>
#include <string>
#include <utility>

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
} // namespace

int main()
{
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
		canonical_value::from_string(digest('6')),
		canonical_value::from_integer(42),
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
	});
	auto closure = canonical_value::from_tuple({
		canonical_value::from_string("source-closure:one"),
		canonical_value::from_string(digest('7')),
		canonical_value::from_string(digest('8')),
		canonical_value::from_integer(1),
		canonical_value::from_integer(1),
		canonical_value::from_integer(42),
	});
	auto encoded = cxxlens::sdk::canonical_binary(canonical_value::from_tuple({
		canonical_value::from_string("cxxlens.build-capture-bundle.v1"),
		std::move(toolchain),
		canonical_value::from_string("shell-free-wrapper"),
		canonical_value::from_string("x86_64-linux-gnu"),
		canonical_value::from_string("project:gcc-example"),
		canonical_value::from_tuple({std::move(unit)}),
		std::move(closure),
		canonical_value::from_tuple({}),
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
