#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <map>
#include <source_location>
#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#include "llvm/clang22/project_abi_observer.hpp"

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclCXX.h>
#include <clang/Basic/CodeGenOptions.h>

namespace
{
	template <class T>
	void require(const T& condition,
				 std::source_location location = std::source_location::current())
	{
		if (!condition)
		{
			std::cerr << location.file_name() << ':' << location.line() << ": ABI check failed\n";
			std::exit(EXIT_FAILURE);
		}
	}
	using observation = cxxlens::detail::clang22::project_abi_observation;
	using limits = cxxlens::detail::clang22::project_abi_limits;
	using ranges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
	const std::string source = R"cpp(
struct Plain { char c; int i; };
struct __attribute__((packed)) Packed { char c; int i; };
struct Empty {};
struct Ebo : Empty { char c; };
struct Overlap { [[no_unique_address]] Empty e; int i; };
struct EmptyMarkers { int i; [[no_unique_address]] Empty e, e2; };
struct Bits { unsigned a:3; unsigned:0; unsigned b:4; };
union Union { char c; int i; };
struct Reference { long double& r; };
struct VirtualBase { virtual int f(int); virtual ~VirtualBase(); int i; };
struct VirtualDerived : virtual VirtualBase { VirtualDerived(); ~VirtualDerived(); char c; };
struct First { int i; };
struct Second { char c; };
struct Multiple : First, Second { short s; };
struct Tail { int i; char c; ~Tail(); };
struct Reused : Tail { char c; };
struct AtomicMember { Tail t; char c; };
struct Zero { char c[0]; };
struct Forward;
template<class T> struct Dependent { T t; };
struct Deleted { Deleted() = delete; ~Deleted() = default; };
struct Abstract { Abstract(); virtual int f() = 0; virtual ~Abstract(); };
Plain call(Plain, int);
int scalar(int);
int __attribute__((ms_abi)) alternate(int);
)cpp";
	struct host_plain
	{
		char c;
		int i;
	};
	struct host_empty
	{
	};
	struct host_overlap
	{
		[[no_unique_address]] host_empty e;
		int i;
	};
	struct host_markers
	{
		int i;
		[[no_unique_address]] host_empty e, e2;
	};
	struct host_bits
	{
		unsigned a : 3;
		unsigned : 0;
		unsigned b : 4;
	};
	union host_union
	{
		char c;
		int i;
	};
	struct host_reference
	{
		long double& r;
	};
	std::map<std::string, observation> collect(std::vector<std::string> extra = {},
											   limits bound = {})
	{
		cxxlens::provider::clang22::translation_unit_input input{
			"snapshot",
			"file",
			"abi.cpp",
			source,
			{"clang++", "-std=c++23", "-Wno-zero-length-array", "abi.cpp"}};
		input.arguments.insert(input.arguments.end() - 1, extra.begin(), extra.end());
		std::map<std::string, observation> output;
		auto parsed = cxxlens::provider::clang22::with_translation_unit(
			input,
			[&](cxxlens::provider::clang22::borrowed_translation_unit& unit)
				-> cxxlens::sdk::result<void>
			{
				if (std::ranges::find(extra, "-O2") != extra.end())
					require(unit.code_generation_options().OptimizationLevel == 2U);
				cxxlens::detail::clang22::project_abi_observer observer{
					unit.ast(), unit.preprocessor(), unit.code_generation_options(), bound};
				for (auto* declaration : unit.ast().getTranslationUnitDecl()->decls())
				{
					if (auto* record = llvm::dyn_cast<clang::RecordDecl>(declaration))
					{
						auto value = observer.record(*record);
						if (!value)
							return value.error();
						output.emplace(record->getNameAsString(), std::move(*value));
						if (auto* cxx = llvm::dyn_cast<clang::CXXRecordDecl>(record))
							for (auto* method : cxx->methods())
								if (!method->isImplicit())
								{
									auto member = observer.function(*method);
									if (!member)
										return member.error();
									output.emplace(method->getQualifiedNameAsString(),
												   std::move(*member));
								}
					}
					if (auto* function = llvm::dyn_cast<clang::FunctionDecl>(declaration))
					{
						auto value = observer.function(*function);
						if (!value)
							return value.error();
						output.emplace(function->getNameAsString(), std::move(*value));
					}
				}
				return {};
			});
		if (!parsed)
			std::cerr << parsed.error().code << ":" << parsed.error().field << ":"
					  << parsed.error().detail << '\n';
		require(parsed);
		return output;
	}
	void complete(const observation& value, std::uint64_t size, ranges expected)
	{
		require(value.abi_state == "complete" && value.layout_state == "complete");
		require(value.byte_size == size && value.byte_alignment && !value.abi_signature.empty());
		require(value.occupied_ranges == expected);
	}
} // namespace
#endif

int main()
{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
	const auto actual = collect();
	complete(
		actual.at("Plain"),
		sizeof(host_plain),
		{{0U, sizeof(char)}, {offsetof(host_plain, i), offsetof(host_plain, i) + sizeof(int)}});
	complete(actual.at("Packed"), sizeof(char) + sizeof(int), {{0U, sizeof(char) + sizeof(int)}});
	complete(actual.at("Empty"), sizeof(host_empty), {{0U, 1U}});
	complete(actual.at("Ebo"), 1U, {{0U, 1U}});
	complete(actual.at("Overlap"), sizeof(host_overlap), {{0U, sizeof(int)}});
	complete(
		actual.at("EmptyMarkers"), sizeof(host_markers), {{0U, offsetof(host_markers, e2) + 1U}});
	complete(actual.at("Bits"),
			 sizeof(host_bits),
			 {{0U, 1U}, {sizeof(unsigned), sizeof(unsigned) + 1U}});
	complete(actual.at("Union"), sizeof(host_union), {{0U, sizeof(host_union)}});
	complete(actual.at("Reference"), sizeof(host_reference), {{0U, sizeof(host_reference)}});
	complete(actual.at("Multiple"), 8U, {{0U, 5U}, {6U, 8U}});
	complete(actual.at("Reused"), 8U, {{0U, 6U}});
	complete(actual.at("AtomicMember"), 12U, {{0U, 9U}});
	complete(actual.at("VirtualBase"), 16U, {{0U, 12U}});
	complete(actual.at("VirtualDerived"), 32U, {{0U, 9U}, {16U, 28U}});
	complete(actual.at("Zero"), 0U, {});
	require(actual.at("Forward").abi_state == "unknown");
	for (const auto* name : {"call",
							 "scalar",
							 "alternate",
							 "VirtualDerived::VirtualDerived",
							 "VirtualDerived::~VirtualDerived",
							 "Abstract::Abstract",
							 "Abstract::~Abstract"})
	{
		const auto& value = actual.at(name);
		require(value.abi_state == "complete" && value.layout_state == "unknown" &&
				!value.abi_signature.empty());
	}
	require(actual.at("scalar").abi_signature != actual.at("alternate").abi_signature);
	const auto unrelated = collect({"-DUNRELATED=1", "-O2"});
	for (const auto& [name, value] : actual)
	{
		const auto& other = unrelated.at(name);
		require(value.abi_context == other.abi_context &&
				value.abi_signature == other.abi_signature &&
				value.occupied_ranges == other.occupied_ranges);
	}
	const auto packed = collect({"-fpack-struct=1"});
	require(actual.at("Plain").abi_signature != packed.at("Plain").abi_signature);
	require(actual.at("call").abi_signature != packed.at("call").abi_signature);
	for (const auto field : {0U, 1U, 2U, 3U})
	{
		cxxlens::provider::clang22::translation_unit_input input{
			"snapshot",
			"file",
			"bound.cpp",
			"struct Plain { char c; int i; };",
			{"clang++", "-std=c++23", "bound.cpp"}};
		auto result = cxxlens::provider::clang22::with_translation_unit(
			input,
			[&](cxxlens::provider::clang22::borrowed_translation_unit& unit)
				-> cxxlens::sdk::result<void>
			{
				limits bound;
				if (field == 0U)
					bound.maximum_operations = 1U;
				if (field == 1U)
					bound.maximum_signature_bytes = 1U;
				if (field == 2U)
					bound.maximum_extents = 1U;
				if (field == 3U)
					bound.maximum_depth = 1U;
				cxxlens::detail::clang22::project_abi_observer observer{
					unit.ast(), unit.preprocessor(), unit.code_generation_options(), bound};
				for (auto* declaration : unit.ast().getTranslationUnitDecl()->decls())
					if (auto* record = llvm::dyn_cast<clang::RecordDecl>(declaration))
					{
						auto value = observer.record(*record);
						require(!value && value.error().code == "application-analysis.abi-budget");
					}
				return {};
			});
		require(result);
	}
#endif
}
