#pragma once

/** @file schema.hpp @brief M0 schema registry and compatibility policy。 */

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core.hpp>
#include <cxxlens/core/failure.hpp>

namespace cxxlens
{
	/** @brief Schema evolution change category。 */
	enum class schema_change_kind : std::uint8_t
	{
		optional_field_added,
		optional_field_removed,
		required_field_added,
		required_field_removed,
		field_type_changed,
		field_meaning_changed,
	};

	/** @brief One declared schema change。 */
	struct schema_change
	{
		/** @brief Change category。 */
		schema_change_kind kind{};
		/** @brief Canonical field path。 */
		std::string field;
	};

	/** @brief Registered schema identity and field policy。 */
	struct schema_descriptor
	{
		/** @brief Stable schema ID without checkout/runtime metadata。 */
		std::string id;
		/** @brief Independent schema version axis。 */
		semantic_version version;
		/** @brief Semantics version interpreted by this schema。 */
		semantic_version semantics_version;
		/** @brief Sorted unique required field names。 */
		std::vector<std::string> required_fields;
		/** @brief Sorted unique optional field names。 */
		std::vector<std::string> optional_fields;
	};

	/** @brief Schema compatibility outcome。 */
	enum class schema_compatibility : std::uint8_t
	{
		compatible,
		incompatible_rebuild_required,
	};

	/** @brief Compatibility decision and machine reasons。 */
	struct schema_compatibility_report
	{
		/** @brief Decision category。 */
		schema_compatibility status{schema_compatibility::compatible};
		/** @brief Sorted machine reason codes。 */
		std::vector<std::string> reasons;
	};

	/** @brief Immutable built-in M0 schema registry。 */
	class schema_registry
	{
	  public:
		/** @brief Built-in registryを作る。
		 * @pre なし。 @post Entries are canonical sorted unique。 @note Registry is deterministic。
		 * @code{.cpp}
		 * #include <cxxlens/core/schema.hpp>
		 * int main(){return cxxlens::schema_registry{}.all().empty()?1:0;}
		 * @endcode */
		schema_registry();

		/** @brief Exact schema ID/versionを解決する。
		 * @param[in] id Stable schema ID。 @param[in] version Required exact version。
		 * @retval value Descriptor or structured version/schema error。
		 * @pre なし。 @post Registryを変更しない。 @note Unknown required versions never fallback。
		 * @code{.cpp}
		 * #include <cxxlens/core/schema.hpp>
		 * int main(){cxxlens::schema_registry r;return
		 * r.find("cxxlens.finding.v1",{1,0,0,{}})?0:1;}
		 * @endcode */
		[[nodiscard]] result<schema_descriptor> find(std::string_view id,
													 const semantic_version& version) const;

		/** @brief Canonical registered schemasを返す。
		 * @retval value Immutable descriptor span。 @pre なし。 @post Registryを変更しない。
		 * @note Consumers must not infer schema compatibility from library version。
		 * @code{.cpp}
		 * #include <cxxlens/core/schema.hpp>
		 * int main(){return cxxlens::schema_registry{}.all().size()>=7?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const schema_descriptor> all() const noexcept;

		/** @brief Declared schema evolution compatibilityを判定する。
		 * @param[in] current Current descriptor。 @param[in] proposed Proposed descriptor。
		 * @param[in] changes Explicit change list。 @retval value Decision and stable reasons。
		 * @pre Descriptors/change paths are independently supplied。 @post Inputsを変更しない。
		 * @note Only compatible optional additions are accepted within a major version。
		 * @code{.cpp}
		 * #include <cxxlens/core/schema.hpp>
		 * int main(){cxxlens::schema_registry r;auto d=r.all().front();return
		 * r.check_compatibility(d,d,{}).status==cxxlens::schema_compatibility::compatible?0:1;}
		 * @endcode */
		[[nodiscard]] schema_compatibility_report
		check_compatibility(const schema_descriptor& current,
							const schema_descriptor& proposed,
							std::span<const schema_change> changes) const;

	  private:
		std::vector<schema_descriptor> entries_;
	};
} // namespace cxxlens
