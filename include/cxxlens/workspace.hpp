#pragma once

/** @file workspace.hpp @brief LLVM-free compilation database and immutable workspace catalog API.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/core/finding.hpp>

namespace cxxlens
{
	/** @brief Missing command handling policy. */
	enum class compile_command_policy : std::uint8_t
	{
		require_exact,
		allow_header_inference,
		allow_fallback_for_snippets,
	};
	/** @brief Build variant selection policy. */
	enum class variant_selection : std::uint8_t
	{
		all,
		representative,
		explicitly_selected,
	};
	/** @brief Generated source handling policy. */
	enum class generated_code_policy : std::uint8_t
	{
		include,
		exclude,
		read_only,
	};
	/** @brief System header handling policy. */
	enum class system_header_policy : std::uint8_t
	{
		include,
		exclude,
	};

	/** @brief Cancellation, budget and progress inputs shared by workspace operations. */
	struct execution_context
	{
		/** @brief Cooperative cancellation token. */
		std::stop_token cancellation;
		/** @brief Optional steady-clock deadline. */
		std::optional<std::chrono::steady_clock::time_point> deadline;
		/** @brief Requested parallelism; zero means automatic. */
		std::size_t parallelism{};
		/** @brief Memory budget in MiB; zero means configured default. */
		std::size_t memory_budget_mb{};
		/** @brief Progress callback invoked without internal locks. */
		std::function<void(double, std::string_view)> progress;
	};

	/** @brief Immutable workspace construction options. */
	struct workspace_options
	{
		/** @brief Project root used to derive semantic relative paths. */
		path project_root;
		/** @brief Build directory or JSON Compilation Database file. */
		path compilation_database;
		/** @brief Optional persistent cache directory. */
		std::optional<path> cache_directory;
		/** @brief Missing-command policy. */
		compile_command_policy commands{compile_command_policy::require_exact};
		/** @brief Variant selection policy. */
		variant_selection variants{variant_selection::all};
		/** @brief Generated-code policy. */
		generated_code_policy generated{generated_code_policy::exclude};
		/** @brief System-header policy. */
		system_header_policy system_headers{system_header_policy::exclude};
		/** @brief Optional configuration file captured by the snapshot. */
		std::optional<path> configuration_file;

		/** @brief Construct options from a build directory or compilation database file.
		 * @param[in] build_or_json Directory or `compile_commands.json` path. @retval value
		 * Options.
		 * @pre Path is lexical input. @post No filesystem access occurs.
		 * @note A directory is resolved to `compile_commands.json` by `workspace::open`.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){auto o=cxxlens::workspace_options::from_compilation_database("build");return
		 * o.compilation_database.empty();}
		 * @endcode */
		[[nodiscard]] static workspace_options from_compilation_database(path build_or_json);
	};

	/** @brief Shell-free normalized compile command. */
	struct compile_command
	{
		/** @brief Normalized working directory. */
		path directory;
		/** @brief Normalized main source path. */
		path file;
		/** @brief Tokenized, response-expanded argv; never a shell string. */
		std::vector<std::string> arguments;
		/** @brief Optional compiler output path. */
		std::optional<path> output;
	};

	/** @brief Canonical target and language projection. */
	struct target_context
	{
		/** @brief Explicit target triple, or empty when absent. */
		std::string triple;
		/** @brief Explicit ABI selection, or empty when absent. */
		std::string abi;
		/** @brief Explicit language standard, or empty when absent. */
		std::string language_standard;
		/** @brief Explicit compiler resource directory. */
		std::optional<std::string> resource_directory;
	};

	/** @brief One source/build-variant pair in an immutable workspace catalog. */
	class compile_unit
	{
	  public:
		/** @brief Construct an unresolved empty handle. @pre None. @post All IDs are empty.
		 * @note Catalog operations return resolved handles; empty is not a compile command.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::compile_unit{}.id().empty()?0:1;}
		 * @endcode */
		compile_unit() = default;
		/** @brief Return the root-independent compile-unit ID.
		 * @retval value Versioned full-digest ID. @pre Object came from a workspace.
		 * @post No state changes. @note Source semantic key and variant ID form identity.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::compile_unit{}.id().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] compile_unit_id id() const;
		/** @brief Return the semantic build-variant ID.
		 * @retval value Versioned full-digest ID. @pre Object came from a workspace.
		 * @post No state changes. @note Absolute checkout root is excluded.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::compile_unit{}.variant_id().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] build_variant_id variant_id() const;
		/** @brief Return the main-file semantic ID.
		 * @retval value Project-relative file ID. @pre Object came from a workspace.
		 * @post No state changes. @note Display paths are not identity.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::compile_unit{}.main_file().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] file_id main_file() const;
		/** @brief Return the normalized command.
		 * @retval value Borrowed command reference. @pre Object outlives the reference.
		 * @post No state changes. @note Response files are already expanded.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){cxxlens::compile_unit u;return u.command().arguments.empty()?0:1;}
		 * @endcode */
		[[nodiscard]] const compile_command& command() const;
		/** @brief Return canonical target context.
		 * @retval value Borrowed target reference. @pre Object outlives the reference.
		 * @post No state changes. @note Empty fields mean explicitly unresolved inputs.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){cxxlens::compile_unit u;return u.target().triple.empty()?0:1;}
		 * @endcode */
		[[nodiscard]] const target_context& target() const;
		/** @brief Return the canonical invocation digest.
		 * @retval value 64 lower-case hexadecimal characters. @pre Object came from a workspace.
		 * @post No state changes. @note Root and database enumeration order are excluded.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::compile_unit{}.command_digest().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::string command_digest() const;

	  private:
		struct data;
		explicit compile_unit(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend class workspace;
	};

	/** @brief Immutable, serializable analysis universe selection. */
	class analysis_scope
	{
	  public:
		/** @brief Select every compile unit. @retval value All scope. @pre None. @post Immutable
		 * value.
		 * @note Headers remain excluded unless requested.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::all().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static analysis_scope all();
		/** @brief Select exact files. @param[in] files Project paths. @retval value Canonically
		 * sorted scope.
		 * @pre Paths are lexical. @post Duplicates removed. @note Scope never broadens silently.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::files({"a.cpp"}).to_json().empty();}
		 * @endcode */
		[[nodiscard]] static analysis_scope files(std::vector<path> files);
		/** @brief Select exact compile units. @param[in] units IDs. @retval value Canonically
		 * sorted scope.
		 * @pre IDs should be valid. @post Duplicates removed. @note Missing IDs are not replaced.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::compile_units({}).to_json().empty();}
		 * @endcode */
		[[nodiscard]] static analysis_scope compile_units(std::vector<compile_unit_id> units);
		/** @brief Select caller-declared changed files. @param[in] files Project paths.
		 * @retval value Canonically sorted changed scope. @pre Paths are lexical. @post Duplicates
		 * removed.
		 * @note No VCS command is executed.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::changed_files({"a.cpp"}).to_json().empty();}
		 * @endcode */
		[[nodiscard]] static analysis_scope changed_files(std::vector<path> files);
		/** @brief Set header inclusion policy. @param[in] enabled Whether headers are included.
		 * @retval value New scope. @pre None. @post Original is unchanged. @note Copy-returning
		 * builder.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::all().include_headers().to_json().empty();}
		 * @endcode */
		[[nodiscard]] analysis_scope include_headers(bool enabled = true) const;
		/** @brief Restrict variants. @param[in] values Exact variant IDs. @retval value New scope.
		 * @pre IDs should be valid. @post Original unchanged and duplicates removed.
		 * @note Empty means an explicitly empty variant set, not all variants.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::all().variants({}).to_json().empty();}
		 * @endcode */
		[[nodiscard]] analysis_scope variants(std::vector<build_variant_id> values) const;
		/** @brief Serialize the authoritative scope.
		 * @retval value Canonical schema-valid JSON. @pre Scope factories were used. @post No state
		 * changes.
		 * @note Operational metadata is absent.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::analysis_scope::all().to_json().starts_with("{")?0:1;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		enum class kind : std::uint8_t
		{
			all,
			files,
			compile_units,
			changed_files,
		};
		kind kind_{kind::all};
		std::vector<path> files_;
		std::vector<compile_unit_id> units_;
		std::optional<std::vector<build_variant_id>> variants_;
		bool include_headers_{};
	};

	class fact_profile;
	class fact_store;
	class capability_set;
	/** @brief Structured workspace health projection. */
	class doctor_report
	{
	  public:
		/** @brief Return whether no error diagnostic exists. @retval value Health state. @pre None.
		 * @post No state changes. @note Diagnostic codes, not prose, drive health.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::doctor_report{}.healthy()?0:1;}
		 * @endcode */
		[[nodiscard]] bool healthy() const noexcept;
		/** @brief Return structured diagnostics. @retval value Borrowed canonical rows. @pre Object
		 * lives.
		 * @post No state changes. @note Empty healthy is distinct from an unavailable doctor
		 * operation.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::doctor_report{}.diagnostics().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] std::span<const diagnostic> diagnostics() const noexcept;
		/** @brief Serialize the report. @retval value Canonical JSON. @pre Diagnostics are valid.
		 * @post No state changes. @note Prose remains display data.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::doctor_report{}.to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
		/** @brief Render the report. @retval value Markdown projection. @pre None. @post No state
		 * changes.
		 * @note JSON diagnostics remain authoritative.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return cxxlens::doctor_report{}.to_markdown().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_markdown() const;

	  private:
		std::vector<diagnostic> diagnostics_;
		friend class workspace;
	};

	/** @brief Immutable compilation database catalog and captured input snapshot. */
	class workspace
	{
	  public:
		/** @brief Open and validate a compilation database without invoking a shell.
		 * @param[in] options Root/database/security policies. @param[in] context Cancellation and
		 * budget.
		 * @retval value Immutable catalog or structured workspace error.
		 * @pre Database and response files are untrusted. @post All distinct commands are retained.
		 * @note Plugin/load flags are rejected by default; no compiler is executed.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){auto
		 * r=cxxlens::workspace::open(cxxlens::workspace_options::from_compilation_database("missing"));return
		 * r?1:0;}
		 * @endcode */
		[[nodiscard]] static result<workspace> open(workspace_options options,
													execution_context context = {});
		/** @brief Return canonical project root. @retval value Display root. @pre Workspace is
		 * open.
		 * @post No state changes. @note Root is excluded from semantic IDs.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] path root() const;
		/** @brief Return every compile unit in canonical order.
		 * @retval value Value-owned handles. @pre Workspace is open. @post No state changes.
		 * @note Multiple variants for one file are retained.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::vector<compile_unit> compile_units() const;
		/** @brief Return zero, one or many matching commands deterministically.
		 * @param[in] file Absolute or root-relative file. @retval value Canonically ordered
		 * matches.
		 * @pre Workspace is open. @post Ambiguity remains visible. @note Header inference obeys
		 * policy.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::vector<compile_unit> command_for(path file) const;
		/** @brief Build missing facts within an exact scope. @param[in] profile Requested fact
		 * profile.
		 * @param[in] scope Immutable universe. @param[in] context Execution controls. @retval value
		 * Status.
		 * @pre Facts package is available. @post Partial failures remain represented.
		 * @note Implementation is owned by the M1 provisioning issue.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<void> ensure(fact_profile profile,
										  analysis_scope scope = analysis_scope::all(),
										  execution_context context = {}) const;
		/** @brief Return the immutable fact-store handle. @retval value Snapshot handle. @pre
		 * Workspace open.
		 * @post No state changes. @note Implementation is owned by the fact-store issue.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_store facts() const;
		/** @brief Return workspace capabilities. @retval value Explicit availability states. @pre
		 * Workspace open.
		 * @post No state changes. @note Unavailable is never an empty success.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] capability_set capabilities() const;
		/** @brief Diagnose build, resource and snapshot inputs. @param[in] context Execution
		 * controls.
		 * @retval value Structured report or failure. @pre Workspace open. @post No state changes.
		 * @note Diagnostics do not control behavior via prose.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<doctor_report> doctor(execution_context context = {}) const;
		/** @brief Explain normalization, snapshot compatibility and inference evidence.
		 * @param[in] file Empty for catalog summary or a query path. @retval value Canonical JSON.
		 * @pre Workspace is open. @post Current input digests are compared without mutation.
		 * @note Changed inputs are reported as stale; they are never silently accepted.
		 * @code{.cpp}
		 * #include <cxxlens/workspace.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string explain_build_context(path file = {}) const;

	  private:
		struct data;
		explicit workspace(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
	};
} // namespace cxxlens
