#include "sdk/source_identity_internal.hpp"

#include <array>
#include <cstdlib>
#include <string>

namespace
{
	template <class value_type>
	void require(const value_type& condition)
	{
		if (!static_cast<bool>(condition))
			std::abort();
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	void file_identity_has_one_projection()
	{
		using namespace cxxlens::sdk;
		auto identity = detail::derive_source_file_id("src/main.cpp");
		const std::array fields{
			canonical_value::from_string("project"),
			canonical_value::from_string("src/main.cpp"),
			canonical_value::from_string("cxxlens.logical-path.v1"),
		};
		auto expected = canonical_identity_digest("file", fields);
		require(identity && expected && *identity == *expected && identity->starts_with("file:"));
		for (const auto invalid : {"",
								   "/src/main.cpp",
								   "project://src/main.cpp",
								   "src//main.cpp",
								   "src/../main.cpp",
								   "src/./main.cpp",
								   "src/"})
		{
			auto rejected = detail::derive_source_file_id(invalid);
			require(!rejected && rejected.error().field == "logical_path");
		}
	}

	void snapshot_identity_has_one_projection()
	{
		using namespace cxxlens::sdk;
		auto file = detail::derive_source_file_id("include/answer.hpp");
		require(file);
		auto identity = detail::derive_source_snapshot_id(*file, digest('1'), "utf8");
		const std::array fields{
			canonical_value::from_string(*file),
			canonical_value::from_string(digest('1')),
			canonical_value::from_string("utf8"),
		};
		auto expected = canonical_identity_digest("source-snapshot", fields);
		require(identity && expected && *identity == *expected &&
				identity->starts_with("source-snapshot:"));

		auto bad_file = detail::derive_source_snapshot_id("source:wrong", digest('1'), "utf8");
		auto bad_digest = detail::derive_source_snapshot_id(*file, "sha256:bad", "utf8");
		auto bad_encoding = detail::derive_source_snapshot_id(*file, digest('1'), "guessed");
		require(!bad_file && bad_file.error().field == "file_id");
		require(!bad_digest && bad_digest.error().field == "content_digest");
		require(!bad_encoding && bad_encoding.error().field == "encoding");
	}
} // namespace

int main()
{
	file_identity_has_one_projection();
	snapshot_identity_has_one_projection();
}
