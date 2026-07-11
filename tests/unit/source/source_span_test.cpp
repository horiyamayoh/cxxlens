#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#include <cxxlens/source.hpp>

namespace
{
	auto test_file_id(const char fill) -> cxxlens::file_id
	{
		return cxxlens::file_id{"file_" + std::string(64U, fill)};
	}

	auto check(bool condition, const char* message) -> bool
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	auto point(const char* file,
			   std::uint64_t offset,
			   std::uint32_t line,
			   std::uint32_t column) -> cxxlens::source_point
	{
		const std::string_view key{file};
		char fill = 'a';
		if (key.find("include/") != key.npos)
			fill = 'b';
		else if (key.find("use.cpp") != key.npos)
			fill = 'c';
		else if (key.find("utf8.cpp") != key.npos)
			fill = 'd';
		else if (key.find("eof.cpp") != key.npos)
			fill = 'e';
		return cxxlens::source_point::at(test_file_id(fill), offset, line, column);
	}

	auto range(const char* file,
			   std::uint64_t begin,
			   std::uint64_t end,
			   std::uint32_t begin_column = 1U,
			   std::uint32_t end_column = 1U) -> cxxlens::file_range
	{
		return {point(file, begin, 1U, begin_column),
				point(file, end, 1U, end_column),
				cxxlens::source_range_kind::token};
	}
} // namespace

auto main(const int argc, const char* const* argv) -> int
{
	using namespace cxxlens;
	bool passed = true;
	source_span direct;
	direct.primary = range("file:src/main.cpp", 0U, 3U, 1U, 4U);
	direct.origin = source_origin::directly_spelled;
	direct.digest = {"sha256", 1U, "abc"};
	passed &= check(!direct.validate(), "direct span invalid");
	passed &= check(direct.is_directly_editable(), "direct span not editable");

	auto reversed = direct;
	reversed.primary = range("file:src/main.cpp", 4U, 2U);
	passed &= check(reversed.validate()->code == source_validation_code::reversed_range,
					"reversed range accepted");
	auto cross_file = direct;
	cross_file.primary.end.file = test_file_id('b');
	passed &= check(cross_file.validate()->code == source_validation_code::different_files,
					"cross-file range accepted");
	auto absolute_key = direct;
	absolute_key.primary.begin.file = file_id{"file:/checkout/src/main.cpp"};
	absolute_key.primary.end.file = absolute_key.primary.begin.file;
	passed &=
		check(absolute_key.validate()->code == source_validation_code::invalid_semantic_file_key,
			  "absolute semantic file key accepted");

	// UTF-8 source "αx": alpha occupies bytes 0..2, so x begins at 1-based byte column 3.
	auto utf8 = range("file:src/utf8.cpp", 2U, 3U, 3U, 4U);
	passed &= check(utf8.begin.column == 3U, "UTF-8 byte column changed");
	auto character = direct;
	character.primary.kind = source_range_kind::character;
	passed &=
		check(!character.validate() &&
				  character.to_canonical_json().find(R"("kind":"character")") != std::string::npos,
			  "character range semantics lost");

	source_span nested;
	nested.primary = range("file:src/use.cpp", 10U, 14U);
	nested.spelling = range("file:include/macros.hpp", 4U, 8U);
	nested.expansion = range("file:src/use.cpp", 10U, 20U);
	nested.macro_stack = {{"OUTER", range("file:src/use.cpp", 8U, 22U), std::nullopt, 0U},
						  {"INNER",
						   range("file:src/use.cpp", 10U, 20U),
						   range("file:include/macros.hpp", 0U, 9U),
						   1U}};
	nested.origin = source_origin::macro_argument;
	nested.digest = {"sha256", 1U, "def"};
	if (argc == 2 && std::string_view{argv[1]} == "--emit")
	{
		std::cout << direct.to_canonical_json() << '\n' << nested.to_canonical_json() << '\n';
		return 0;
	}
	passed &=
		check(!nested.validate() && nested.macro_stack.size() == 2U, "nested macro mapping lost");
	passed &= check(!nested.is_directly_editable(), "macro span editable");
	const auto json = nested.to_canonical_json();
	passed &= check(json.find("/home/") == std::string::npos &&
						json.find("file_cccccccc") != std::string::npos,
					"canonical JSON leaked display root");
	std::ifstream golden_input{CXXLENS_NESTED_SOURCE_GOLDEN};
	std::string golden{std::istreambuf_iterator<char>{golden_input},
					   std::istreambuf_iterator<char>{}};
	if (!golden.empty() && golden.back() == '\n')
	{
		golden.pop_back();
	}
	passed &= check(json == golden, "nested macro canonical JSON golden changed");
	const auto direct_json = direct.to_canonical_json();
	passed &= check(direct_json.find(test_file_id('a').value()) != std::string::npos,
					"direct canonical JSON stable file ID changed");
	passed &= check(!file_id{"file:/tmp/root/src/a.cpp"}.valid() &&
						test_file_id('a') == test_file_id('a'),
					"checkout root entered semantic source key");

	for (const auto origin : {source_origin::implicit_compiler_node,
							  source_origin::generated_file,
							  source_origin::system_header,
							  source_origin::builtin,
							  source_origin::unknown})
	{
		auto read_only = direct;
		read_only.origin = origin;
		read_only.read_only = true;
		passed &= check(!read_only.is_directly_editable(), "unsafe origin editable");
	}
	for (const auto origin :
		 {source_origin::macro_argument, source_origin::macro_body, source_origin::macro_expansion})
	{
		auto macro = nested;
		macro.origin = origin;
		passed &= check(!macro.validate() && !macro.is_directly_editable(),
						"macro origin was collapsed or editable");
	}
	auto invalid = direct;
	invalid.primary.begin = source_point::invalid();
	passed &= check(invalid.validate()->code == source_validation_code::invalid_point_coordinates &&
						!invalid.is_directly_editable(),
					"invalid point fabricated as offset zero");
	passed &= check(source_point::invalid().state != source_point::unknown().state,
					"invalid and unknown locations collapsed");
	passed &= check(range("file:src/eof.cpp", 7U, 7U).begin.byte_offset == 7U,
					"empty EOF range rejected");
	return passed ? 0 : 1;
}
