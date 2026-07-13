#include "interop_extractors.hpp"

#include <memory>
#include <type_traits>

int main()
{
	using extractor = cxxlens::interop::clang_fact_extractor;
	static_assert(std::has_virtual_destructor_v<extractor>);
	static_assert(std::is_abstract_v<extractor>);

	cxxlens::interop::custom_fact value{
		.provider_namespace = "dev.example.analysis",
		.schema_id = "dev.example.analysis.escape-summary",
		.schema_version = {1U, 0U, 0U, {}},
		.semantic_key = "symbol:symbol_aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		.payload_json = R"({"escapes":false})",
		.source = std::nullopt,
	};
	return value.provider_namespace.empty() ? 1 : 0;
}
