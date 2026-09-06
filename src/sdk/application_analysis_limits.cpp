#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk
{
	result<void> import_limits::validate() const
	{
		if (maximum_bundle_bytes == 0U || maximum_nesting_depth == 0U ||
			maximum_nesting_depth > 64U || maximum_compile_units == 0U ||
			maximum_arguments_per_unit == 0U || maximum_auxiliary_files_per_unit == 0U ||
			maximum_environment_effects_per_unit == 0U || maximum_path_mappings == 0U ||
			maximum_string_bytes == 0U || maximum_total_metadata_bytes == 0U ||
			maximum_source_closure_members == 0U || maximum_source_closures == 0U ||
			maximum_source_closure_blobs == 0U || maximum_source_closure_bytes == 0U)
			return unexpected(error{"application-analysis.import-limits-invalid", "limits", {}});
		return {};
	}
} // namespace cxxlens::sdk
