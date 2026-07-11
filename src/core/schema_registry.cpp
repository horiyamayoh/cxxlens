#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/core/schema.hpp>

namespace cxxlens
{
	namespace
	{
		[[nodiscard]] schema_descriptor descriptor(std::string id,
												   std::vector<std::string> required,
												   std::vector<std::string> optional = {})
		{
			std::ranges::sort(required);
			std::ranges::sort(optional);
			return {std::move(id), {1U, 0U, 0U, {}}, {1U, 0U, 0U, {}}, required, optional};
		}

		[[nodiscard]] error registry_error(std::string code, std::string id)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "required schema could not be resolved";
			failure.attributes.emplace("schema.id", std::move(id));
			return failure;
		}
	} // namespace

	schema_registry::schema_registry()
		: entries_{
			  descriptor("cxxlens.analysis-scope.v1",
						 {"compile_units",
						  "files",
						  "include_headers",
						  "kind",
						  "library_version",
						  "schema",
						  "semantics_version",
						  "variants"}),
			  descriptor(
				  "cxxlens.config.explain.v1",
				  {"key", "library_version", "schema", "semantics_version", "shadowed", "winner"}),
			  descriptor(
				  "cxxlens.config.resolved.v1",
				  {"library_version", "provenance", "schema", "semantics_version", "values"}),
			  descriptor("cxxlens.config.v1", {"schema"}),
			  descriptor("cxxlens.coverage.v1",
						 {"complete",
						  "library_version",
						  "requested",
						  "schema",
						  "semantics_version",
						  "summary",
						  "units"}),
			  descriptor(
				  "cxxlens.diagnostic.v1",
				  {"id", "library_version", "message", "schema", "semantics_version", "severity"},
				  {"compiler_option", "primary", "related"}),
			  descriptor("cxxlens.error.v1",
						 {"attributes",
						  "causes",
						  "code",
						  "library_version",
						  "locations",
						  "message",
						  "retryable",
						  "schema",
						  "scope",
						  "semantics_version",
						  "suggested_actions"}),
			  descriptor("cxxlens.evidence.v1",
						 {"items", "library_version", "schema", "semantics_version"}),
			  descriptor("cxxlens.finding.v1",
						 {"confidence",
						  "coverage",
						  "evidence",
						  "guarantee",
						  "id",
						  "library_version",
						  "message",
						  "primary",
						  "rule_or_recipe",
						  "schema",
						  "semantics_version",
						  "severity",
						  "unresolved"},
						 {"variant_signature"}),
			  descriptor("cxxlens.finding-set.v1",
						 {"findings", "library_version", "schema", "semantics_version"}),
			  descriptor("cxxlens.source-span.v1",
						 {"digest",
						  "expansion",
						  "library_version",
						  "macro_stack",
						  "origin",
						  "primary",
						  "read_only",
						  "schema",
						  "semantics_version",
						  "spelling"}),
			  descriptor("cxxlens.testing.fixture.v1",
						 {"arguments",
						  "compilation_database",
						  "files",
						  "language",
						  "library_version",
						  "main_file",
						  "schema",
						  "semantics_version",
						  "standard",
						  "target",
						  "variants"}),
			  descriptor("cxxlens.unresolved.v1",
						 {"attributes",
						  "kind",
						  "library_version",
						  "missing_inputs",
						  "related",
						  "schema",
						  "scope",
						  "semantics_version",
						  "stable_code",
						  "suggested_actions",
						  "summary"},
						 {"required_capability", "required_precision"}),
			  descriptor("cxxlens.workspace-context.v1",
						 {"compatible",
						  "header_inference",
						  "library_version",
						  "query",
						  "schema",
						  "semantics_version",
						  "snapshot_key",
						  "stale_inputs",
						  "units"}),
		  }
	{
		std::ranges::sort(entries_, {}, &schema_descriptor::id);
	}

	result<schema_descriptor> schema_registry::find(const std::string_view id,
													const semantic_version& version) const
	{
		const auto entry = std::ranges::lower_bound(entries_, id, {}, &schema_descriptor::id);
		if (entry == entries_.end() || entry->id != id)
			return registry_error("core.schema-validation-failed", std::string{id});
		if (entry->version != version)
			return registry_error("core.version-mismatch", std::string{id});
		return *entry;
	}

	std::span<const schema_descriptor> schema_registry::all() const noexcept
	{
		return entries_;
	}

	schema_compatibility_report
	schema_registry::check_compatibility(const schema_descriptor& current,
										 const schema_descriptor& proposed,
										 const std::span<const schema_change> changes) const
	{
		schema_compatibility_report report;
		if (current.id != proposed.id)
			report.reasons.emplace_back("schema.id-changed");
		if (current.version.major != proposed.version.major)
			report.reasons.emplace_back("schema.major-version-changed");
		for (const auto& change : changes)
		{
			if (change.field.empty())
				report.reasons.emplace_back("schema.empty-field-path");
			if (change.kind != schema_change_kind::optional_field_added)
				report.reasons.emplace_back("schema.incompatible-change");
		}
		std::ranges::sort(report.reasons);
		report.reasons.erase(std::unique(report.reasons.begin(), report.reasons.end()),
							 report.reasons.end());
		if (!report.reasons.empty())
			report.status = schema_compatibility::incompatible_rebuild_required;
		return report;
	}
} // namespace cxxlens
