#include "llvm/clang22/compiler_vfs.hpp"

#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace source_closure = cxxlens::detail::clang22::source_closure;

namespace
{
	[[nodiscard]] std::vector<std::byte> bytes(const std::string& value)
	{
		return {reinterpret_cast<const std::byte*>(value.data()),
			reinterpret_cast<const std::byte*>(value.data() + value.size())};
	}

	[[nodiscard]] source_closure::blob make_blob(const std::string& value)
	{
		auto content = bytes(value);
		return {source_closure::sha256_digest(content), std::move(content)};
	}

	[[nodiscard]] source_closure::file make_file(std::string path,
		const source_closure::blob& content,
		const source_closure::file_role role)
	{
		const auto provenance_bytes = bytes("vfs-test");
		return {std::move(path), content.content_digest, content.content.size(), role,
			source_closure::sha256_digest(provenance_bytes)};
	}

	[[nodiscard]] std::shared_ptr<const source_closure::validated_snapshot> snapshot()
	{
		auto main = make_blob("#include \"shared.hpp\"\n");
		auto shared = make_blob("inline int answer(){return 42;}\n");
		auto generated = make_blob("#define GENERATED 1\n");
		source_closure::snapshot input;
		input.files = {
			make_file("project://src/main.cpp", main, source_closure::file_role::main_source),
			make_file("project://include/shared.hpp", shared,
				source_closure::file_role::project_header),
			make_file("generated://build/config.hpp", generated,
				source_closure::file_role::generated_header),
		};
		input.blobs = {main, shared, generated};
		auto validated = source_closure::validate(std::move(input));
		assert(validated);
		return std::make_shared<const source_closure::validated_snapshot>(std::move(*validated));
	}
} // namespace

int main()
{
	auto filesystem = source_closure::read_only_compiler_vfs::create(snapshot());
	assert(filesystem);
	assert(!filesystem->ambient_project_fallback_allowed());

	const auto header = filesystem->open_logical("project://include/shared.hpp");
	assert(header);
	assert(header->compiler_path == "/__cxxlens/project/include/shared.hpp");
	assert(header->role == source_closure::file_role::project_header);
	assert(!header->content.empty());

	const auto generated = filesystem->open_compiler("/__cxxlens/generated/build/config.hpp");
	assert(generated);
	assert(generated->logical_path == "generated://build/config.hpp");

	const std::vector<std::string> arguments{
		"clang++", "-Iproject://include", "-include", "generated://build/config.hpp",
		"project://src/main.cpp"};
	const auto rewritten = filesystem->rewrite_invocation(arguments);
	assert(rewritten);
	assert((*rewritten)[1U] == "-I/__cxxlens/project/include");
	assert((*rewritten)[3U] == "/__cxxlens/generated/build/config.hpp");
	assert((*rewritten)[4U] == "/__cxxlens/project/src/main.cpp");

	const auto missing = filesystem->open_logical("project://include/ambient-only.hpp");
	assert(!missing);
	assert(missing.error().code == "source-closure.vfs-missing-input");
	assert(missing.error().path == "project://include/ambient-only.hpp");
	assert(missing.error().role == "project-or-generated");

	const auto ambient = filesystem->open_compiler("/etc/passwd");
	assert(!ambient);
	assert(ambient.error().code == "source-closure.vfs-missing-input");
	return 0;
}
