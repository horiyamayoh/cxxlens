#pragma once

/** @file testing.hpp @brief Production-path fixture、assertion、property testing substrate。 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/core/evidence.hpp>
#include <cxxlens/core/schema.hpp>
#include <cxxlens/source.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::testing
{
	namespace detail
	{
		struct workspace_fixture_data;
	} // namespace detail

	/** @brief Fixture source role。 */
	enum class fixture_file_kind : std::uint8_t
	{
		/** @brief Translation-unit/source role。 */
		source,
		/** @brief Header role。 */
		header,
	};

	/** @brief One value-owned fixture file。 */
	struct fixture_file
	{
		/** @brief Root-relative normalized path。 */
		path name;
		/** @brief Exact file bytes。 */
		std::string content;
		/** @brief Source/header role。 */
		fixture_file_kind kind{fixture_file_kind::source};
		/** @brief Generated-code classification。 */
		bool generated{};
		/** @brief System-header classification。 */
		bool system_header{};
	};

	/** @brief One deterministic build variant input。 */
	struct fixture_variant
	{
		/** @brief Stable unique fixture-local name。 */
		std::string name;
		/** @brief Ordered literal argv suffix。 */
		std::vector<std::string> arguments;
		/** @brief Canonically ordered explicit environment metadata。 */
		std::map<std::string, std::string, std::less<>> environment;
	};

	/** @brief Production workspace loader が消費する immutable fixture materialization。 */
	struct fixture_bundle
	{
		/** @brief Operational synthetic root, excluded from semantic JSON。 */
		path root;
		/** @brief Root-relative main file。 */
		path main_file;
		/** @brief `c` or `c++` language。 */
		std::string language;
		/** @brief Language-standard spelling。 */
		std::string standard;
		/** @brief Target triple。 */
		std::string target;
		/** @brief Common ordered literal argv suffix。 */
		std::vector<std::string> arguments;
		/** @brief Canonically ordered materialized files。 */
		std::vector<fixture_file> files;
		/** @brief Canonically ordered variants。 */
		std::vector<fixture_variant> variants;
		/** @brief Canonical arguments-based compilation database JSON。 */
		std::string compilation_database_json;

		/** @brief Bundle invariant を検証する。
		 * @retval value Valid なら success。 @pre なし。 @post Object を変更しない。
		 * @note Semantic facts や AST は bundle に含めない。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return cxxlens::testing::workspace_fixture::cpp("int main(){}")
		 * .materialize().value().validate()?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> validate() const;

		/** @brief Versioned canonical fixture JSON を返す。
		 * @retval value Root-relocatable canonical projection。 @pre `validate()` succeeds。
		 * @post Object を変更しない。 @note File/variant insertion order is excluded。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto b=cxxlens::testing::workspace_fixture::c("int main(void){}")
		 * .materialize();return b&&b.value().to_json().empty()?1:0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;
	};

	/** @brief Immutable fixture input builder。 */
	class workspace_fixture
	{
	  public:
		/** @brief C++ main source fixture を作る。
		 * @param[in] main_source Main file bytes。 @retval value Immutable C++ builder。
		 * @pre Bytes are UTF-8/text fixture input。 @post Default main file is `main.cpp`。
		 * @note Parsing is deferred to the production frontend。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto f=cxxlens::testing::workspace_fixture::cpp("int main(){}");return
		 * f.materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] static workspace_fixture cpp(std::string main_source);
		/** @brief C main source fixture を作る。
		 * @param[in] main_source Main file bytes。 @retval value Immutable C builder。
		 * @pre Bytes are fixture input。 @post Default main file is `main.c`。
		 * @note No semantic facts are fabricated。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return cxxlens::testing::workspace_fixture::c("").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] static workspace_fixture c(std::string main_source);
		/** @brief Main file path を置換した copy を返す。
		 * @param[in] value Relative path。 @retval value Updated immutable builder。
		 * @pre Path safety is checked by `materialize()`。 @post Source builder is unchanged。
		 * @note Traversal/absolute paths are rejected at materialization。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").main_file("x.cpp").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture main_file(path value) const;
		/** @brief Source file を追加した copy を返す。
		 * @param[in] name Relative path。 @param[in] content File bytes。
		 * @retval value Updated copy。 @pre Inputs are untrusted。 @post Original is unchanged。
		 * @note Duplicate paths are rejected by `materialize()`。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").add_file("x.cpp","").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture add_file(path name, std::string content) const;
		/** @brief Header file を追加した copy を返す。
		 * @param[in] name Relative path。 @param[in] content File bytes。
		 * @retval value Updated copy。 @pre Inputs are untrusted。 @post Original is unchanged。
		 * @note Header role is metadata, not a fabricated semantic fact。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").add_header("x.hpp","").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture add_header(path name, std::string content) const;
		/** @brief Build variant を追加した copy を返す。
		 * @param[in] value Name/argv/environment metadata。 @retval value Updated copy。
		 * @pre Variant may be unvalidated。 @post Original is unchanged。
		 * @note Duplicate names are rejected by `materialize()`。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return cxxlens::testing::workspace_fixture::cpp("").add_variant({"v",{},
		 * {}}).materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture add_variant(fixture_variant value) const;
		/** @brief Language standard を置換した copy を返す。
		 * @param[in] value Standard spelling。 @retval value Updated copy。
		 * @pre Non-empty validation is deferred。 @post Original is unchanged。
		 * @note Value becomes a literal `-std=` argument。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").standard("c++20").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture standard(std::string value) const;
		/** @brief Target triple を置換した copy を返す。
		 * @param[in] triple Target spelling。 @retval value Updated copy。
		 * @pre Non-empty validation is deferred。 @post Original is unchanged。
		 * @note Value becomes a literal `--target=` argument。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").target("x86_64-unknown-linux-gnu").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture target(std::string triple) const;
		/** @brief Literal compiler argument を追加した copy を返す。
		 * @param[in] value One argv element。 @retval value Updated copy。
		 * @pre Argument is never a shell string。 @post Original is unchanged。
		 * @note Metacharacters remain literal argv bytes。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").argument("-Wall").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture argument(std::string value) const;
		/** @brief File を generated として分類した copy を返す。
		 * @param[in] value Existing fixture relative path。 @retval value Updated copy。
		 * @pre Path is validated on materialization。 @post Original is unchanged。
		 * @note Classification remains visible in bundle JSON。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").add_file("g.cpp","").generated("g.cpp").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture generated(path value) const;
		/** @brief File を system header として分類した copy を返す。
		 * @param[in] value Existing header relative path。 @retval value Updated copy。
		 * @pre Path is validated on materialization。 @post Original is unchanged。
		 * @note Classification remains visible in bundle JSON。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return
		 * cxxlens::testing::workspace_fixture::cpp("").add_header("s.h","").system_header("s.h").materialize()?0:1;}
		 * @endcode */
		[[nodiscard]] workspace_fixture system_header(path value) const;

		/** @brief Fixture を production workspace path で開く。
		 * @param[in] context Cancellation、deadline、parallelism controls。
		 * @retval value Memory filesystem 上の immutable workspace、または structured error。
		 * @pre Builder inputs must satisfy `fixture_bundle::validate()`。
		 * @post Production catalog/frontend/reducer/store components are used; fake facts are not
		 * created。
		 * @note Source/compile database bytes remain value-owned by the workspace and no temporary
		 * host files are created。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto ws=cxxlens::testing::workspace_fixture::cpp("int main(){}")
		 * .open();return ws && ws.value().compile_units().size()==1U?0:1;}
		 * @endcode */
		[[nodiscard]] result<workspace> open(execution_context context = {}) const;

		/** @brief Production workspace seam 用 bundle を materialize する。
		 * @param[in] root Synthetic workspace root。 @retval value Canonical bundle or error。
		 * @pre Root は absolute lexical path。 @post Builder を変更しない。
		 * @note 実 frontend 接続は M1 `workspace::open` が担い、fake facts は生成しない。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto b=cxxlens::testing::workspace_fixture::cpp("int main(){}")
		 * .add_header("a.hpp", "#pragma once").materialize();return b?0:1;}
		 * @endcode */
		[[nodiscard]] result<fixture_bundle> materialize(const path& root = path{
															 "/workspace"}) const;

	  private:
		explicit workspace_fixture(std::shared_ptr<const detail::workspace_fixture_data> data);
		std::shared_ptr<const detail::workspace_fixture_data> data_;
	};

	/** @brief Runtime port fault target category。 */
	enum class fault_target : std::uint8_t
	{
		/** @brief Filesystem port。 */
		filesystem,
		/** @brief Process port。 */
		process,
		/** @brief Hash port。 */
		hash,
		/** @brief Time/deadline port。 */
		time,
	};

	/** @brief One exact deterministic fault schedule row。 */
	struct fault_injection
	{
		/** @brief Target port category。 */
		fault_target target{};
		/** @brief Stable operation name。 */
		std::string operation;
		/** @brief Caller-owned deterministic call index。 */
		std::uint64_t call_index{};
		/** @brief Structured failure returned on exact match。 */
		error failure;
	};

	/** @brief Immutable exact-key runtime fault plan。 */
	class fault_plan
	{
	  public:
		/** @brief Fault rows を検証・canonicalize する。
		 * @param[in] injections Target/operation/index keyed rows。 @retval value Plan or conflict
		 * error。
		 * @pre Error codes must be registered。 @post Rows are sorted and duplicate keys rejected。
		 * @note Worker completion order is not an input。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){cxxlens::error e;e.code.value="io.retryable";return
		 * cxxlens::testing::fault_plan::make({{cxxlens::testing::fault_target::hash,"hash",1,e}})?0:1;}
		 * @endcode */
		[[nodiscard]] static result<fault_plan> make(std::vector<fault_injection> injections);

		/** @brief Exact target/operation/index fault を probe する。
		 * @param[in] target Port category。 @param[in] operation Stable operation name。
		 * @param[in] call_index Caller-owned index。 @retval value Success if no injection;
		 * configured error on match。
		 * @pre Plan is factory-created。 @post Plan is unchanged。 @note No global counter is
		 * used。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto p=cxxlens::testing::fault_plan::make({});return p&&p.value().probe(
		 * cxxlens::testing::fault_target::filesystem,"read",0)?0:1;}
		 * @endcode */
		[[nodiscard]] result<void>
		probe(fault_target target, std::string_view operation, std::uint64_t call_index) const;

	  private:
		std::vector<fault_injection> injections_;
	};

	/** @brief Generic operation outcome presented to assertions without losing evidence。 */
	struct result_observation
	{
		/** @brief Authoritative result row count。 */
		std::size_t result_count{};
		/** @brief Operation-level structured failure。 */
		std::optional<error> failure;
		/** @brief Original coverage accounting。 */
		coverage_report coverage;
		/** @brief Original partial uncertainty rows。 */
		std::vector<unresolved> unresolved_items;
		/** @brief Original structured evidence。 */
		evidence supporting_evidence;
		/** @brief Producer canonical JSON when golden comparison is requested。 */
		std::string canonical_json;
	};

	/** @brief Immutable typed expectation builder。 */
	class result_assertion
	{
	  public:
		/** @brief Exact row count expectation を追加する。
		 * @param[in] value Expected count。 @retval value Updated immutable assertion。
		 * @pre なし。 @post Source assertion is unchanged。 @note Zero means success-empty, not
		 * failure。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){cxxlens::testing::result_observation o;return
		 * cxxlens::testing::result_assertion{}.has_exactly(0).check(o)?0:1;}
		 * @endcode */
		[[nodiscard]] result_assertion has_exactly(std::size_t value) const;
		/** @brief Failed-error absence expectation を追加する。
		 * @retval value Updated assertion。 @pre なし。 @post Source is unchanged。
		 * @note Partial unresolved success remains distinct and may pass this expectation。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){cxxlens::testing::result_observation o;return
		 * cxxlens::testing::result_assertion{}.has_no_errors().check(o)?0:1;}
		 * @endcode */
		[[nodiscard]] result_assertion has_no_errors() const;
		/** @brief Complete coverage expectation を追加する。
		 * @retval value Updated assertion。 @pre なし。 @post Source is unchanged。
		 * @note Coverage rows remain authoritative。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){cxxlens::testing::result_observation o;return
		 * cxxlens::testing::result_assertion{}.is_complete().check(o)?0:1;}
		 * @endcode */
		[[nodiscard]] result_assertion is_complete() const;
		/** @brief Partial unresolved kind expectation を追加する。
		 * @param[in] kind Required typed kind。 @retval value Updated assertion。
		 * @pre なし。 @post Source is unchanged。 @note Prose is not inspected。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto
		 * a=cxxlens::testing::result_assertion{}.is_partial_with(cxxlens::unresolved_kind::custom);(void)a;return
		 * 0;}
		 * @endcode */
		[[nodiscard]] result_assertion is_partial_with(unresolved_kind kind) const;
		/** @brief Exact canonical golden expectation を追加する。
		 * @param[in] golden Golden file path。 @retval value Updated assertion。
		 * @pre Read is performed through filesystem port。 @post Source is unchanged。
		 * @note Normalization must be explicit before this exact comparison。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){auto
		 * a=cxxlens::testing::result_assertion{}.json_matches("golden.json");(void)a;return 0;}
		 * @endcode */
		[[nodiscard]] result_assertion json_matches(path golden) const;

		/** @brief Observation を全 expectation に照合する。
		 * @param[in] observation Original error/evidence/coverage を含む outcome。
		 * @retval value Success or `testing.assertion-failed` with stable attributes。
		 * @pre Observation rows are producer-owned。 @post Observation を変更しない。
		 * @note Empty success、partial unresolved、failed error を区別する。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){cxxlens::testing::result_observation o;return
		 * cxxlens::testing::result_assertion{}.has_exactly(0).has_no_errors().check(o)?0:1;}
		 * @endcode */
		[[nodiscard]] result<void> check(const result_observation& observation) const;

	  private:
		std::optional<std::size_t> exact_count_;
		bool require_no_errors_{};
		bool require_complete_{};
		std::optional<unresolved_kind> partial_kind_;
		std::optional<path> golden_;
	};

	/** @brief Allowed golden normalization inputs。 */
	struct golden_context
	{
		/** @brief Allowed workspace root prefix。 */
		path workspace_root;
		/** @brief Allowed build root prefix。 */
		path build_root;
		/** @brief Allowed resource root prefix。 */
		path resource_root;
	};

	/** @brief Root/runtime metadata だけを canonical JSON 内で normalize する。
	 * @param[in] canonical_json Versioned JSON。 @param[in] context Allowed roots。
	 * @retval value Normalized canonical JSON or structured parse error。
	 * @pre Input is untrusted。 @post IDs/ranges/variants/reason codes remain unchanged。
	 * @note Runtime fields are replaced, never used for semantic comparison。
	 * @code{.cpp}
	 * #include <cxxlens/testing.hpp>
	 * int main(){return cxxlens::testing::normalize_golden("{}",{} )?0:1;}
	 * @endcode */
	[[nodiscard]] result<std::string> normalize_golden(std::string_view canonical_json,
													   const golden_context& context);

	/** @brief Registered schema の required field と schema ID を検証する。
	 * @param[in] schema_id Exact registered ID。 @param[in] canonical_json JSON bytes。
	 * @retval value Success or deterministic schema path/field failure。
	 * @pre Input is untrusted。 @post Input を変更しない。 @note Unknown version never fallback。
	 * @code{.cpp}
	 * #include <cxxlens/testing.hpp>
	 * int main(){return cxxlens::testing::assert_schema_conforms(
	 * "cxxlens.evidence.v1",cxxlens::evidence{}.to_json())?0:1;}
	 * @endcode */
	[[nodiscard]] result<void> assert_schema_conforms(std::string_view schema_id,
													  std::string_view canonical_json);

	/** @brief Property execution/replay options。 */
	struct property_options
	{
		/** @brief Deterministic generator seed。 */
		std::uint64_t seed{0xC771EULL};
		/** @brief Generated case count outside replay mode。 */
		std::size_t cases{100U};
		/** @brief Exact case index for replay mode。 */
		std::optional<std::size_t> replay_case;
	};

	/** @brief Replayable property failure。 */
	struct property_failure
	{
		/** @brief Generator seed。 */
		std::uint64_t seed{};
		/** @brief Original generated failing case index。 */
		std::size_t case_index{};
		/** @brief Canonically rendered locally minimal failing input。 */
		std::string minimal_input;
		/** @brief Stable `seed=N;case=N` replay token。 */
		std::string replay;
	};

	/** @brief Property outcome。 */
	struct property_report
	{
		/** @brief Number of generated cases evaluated。 */
		std::size_t executed{};
		/** @brief First canonical failing case, if any。 */
		std::optional<property_failure> failure;
		/** @brief Property success state を返す。
		 * @retval value Failure がなければ true。 @pre なし。 @post Report is unchanged。
		 * @note Executed zero cases is successful only when requested by options。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return cxxlens::testing::property_report{}.passed()?0:1;}
		 * @endcode */
		[[nodiscard]] bool passed() const noexcept
		{
			return !failure.has_value();
		}
	};

	template <class Generator, class Predicate, class Shrinker, class Renderer>
	[[nodiscard]] property_report check_property(const property_options& options,
												 Generator&& generator,
												 Predicate&& predicate,
												 Shrinker&& shrinker,
												 Renderer&& renderer);

	/** @brief Deterministic generic property cases を実行する。
	 * @tparam Generator `(seed,index)->case`。 @tparam Predicate `(case)->bool`。
	 * @tparam Renderer `(case)->string`。 @param[in] options Seed/case/replay settings。
	 * @param[in] generator Deterministic generator。 @param[in] predicate Property predicate。
	 * @param[in] renderer Canonical input renderer。 @retval value Replayable report。
	 * @pre Generator is deterministic for seed/index。 @post Failure includes seed/case/input。
	 * @note `replay_case` executes exactly that generated case。
	 * @code{.cpp}
	 * #include <cxxlens/testing.hpp>
	 * int main(){auto
	 * r=cxxlens::testing::check_property(cxxlens::testing::property_options{1,2,std::nullopt},
	 * [](auto s,auto i){return s+i;},[](auto){return true;},[](auto v){return std::to_string(v);});
	 * return r.passed()?0:1;}
	 * @endcode */
	template <class Generator, class Predicate, class Renderer>
	[[nodiscard]] property_report check_property(const property_options& options,
												 Generator&& generator,
												 Predicate&& predicate,
												 Renderer&& renderer)
	{
		using case_type =
			std::decay_t<std::invoke_result_t<Generator&, std::uint64_t, std::size_t>>;
		return check_property(
			options,
			std::forward<Generator>(generator),
			std::forward<Predicate>(predicate),
			[](const auto&)
			{
				return std::vector<case_type>{};
			},
			std::forward<Renderer>(renderer));
	}

	/** @brief Shrinker 付き property cases を実行し canonical minimal failure を返す。
	 * @tparam Generator Generator type。 @tparam Predicate Predicate type。
	 * @tparam Shrinker `(case)->vector<case>`。 @tparam Renderer Renderer type。
	 * @param[in] options Seed/case/replay settings。 @param[in] generator Case generator。
	 * @param[in] predicate Property predicate。 @param[in] shrinker Candidate generator。
	 * @param[in] renderer Canonical input renderer。
	 * @retval value Seed/case と locally minimal rendered input を含む report。
	 * @pre Generator/shrinker/renderer are deterministic。 @post Callables are not retained。
	 * @note Shrink candidates are sorted by rendered bytes and cycles are rejected。
	 * @code{.cpp}
	 * #include <cxxlens/testing.hpp>
	 * int main(){auto
	 * r=cxxlens::testing::check_property(cxxlens::testing::property_options{1,1,std::nullopt},
	 * [](auto,auto){return 2;},[](auto v){return v<1;},[](auto v){return std::vector<int>{v/2};},
	 * [](auto v){return std::to_string(v);});return r.failure?0:1;}
	 * @endcode
	 */
	template <class Generator, class Predicate, class Shrinker, class Renderer>
	[[nodiscard]] property_report check_property(const property_options& options,
												 Generator&& generator,
												 Predicate&& predicate,
												 Shrinker&& shrinker,
												 Renderer&& renderer)
	{
		property_report report;
		const auto first = options.replay_case.value_or(0U);
		const auto last = options.replay_case ? first + 1U : options.cases;
		for (std::size_t index = first; index < last; ++index)
		{
			auto value = generator(options.seed, index);
			++report.executed;
			if (!predicate(value))
			{
				std::set<std::string, std::less<>> visited;
				visited.insert(renderer(value));
				while (true)
				{
					auto candidates = shrinker(value);
					std::ranges::sort(candidates,
									  {},
									  [&](const auto& candidate)
									  {
										  return renderer(candidate);
									  });
					const auto smaller = std::ranges::find_if(
						candidates,
						[&](const auto& candidate)
						{
							const auto bytes = renderer(candidate);
							return !predicate(candidate) && !visited.contains(bytes);
						});
					if (smaller == candidates.end())
						break;
					value = *smaller;
					visited.insert(renderer(value));
				}
				const auto rendered = renderer(value);
				report.failure = property_failure{
					options.seed,
					index,
					rendered,
					"seed=" + std::to_string(options.seed) + ";case=" + std::to_string(index),
				};
				break;
			}
		}
		return report;
	}

	/** @brief Determinism matrix axes。 */
	struct determinism_options
	{
		/** @brief Parallelism values。 */
		std::vector<std::size_t> parallelism{1U, 2U, 8U};
		/** @brief Scheduler seeds。 */
		std::vector<std::uint64_t> scheduler_seeds{0U, 1U, 0xC771EULL};
		/** @brief Whether callers should exercise reversed input ordering。 */
		bool reverse_input_order{true};
		/** @brief Whether matrix alternates synthetic workspace roots。 */
		bool relocate_workspace_root{true};
	};

	/** @brief Semantic-byte comparison outcome。 */
	struct determinism_report
	{
		/** @brief Completed operation executions。 */
		std::size_t executions{};
		/** @brief Stable axis descriptions whose bytes differed。 */
		std::vector<std::string> differences;
		/** @brief Matrix stability を返す。
		 * @retval value Differences が空なら true。 @pre なし。 @post Report is unchanged。
		 * @note Operational differences must remain outside compared semantic bytes。
		 * @code{.cpp}
		 * #include <cxxlens/testing.hpp>
		 * int main(){return cxxlens::testing::determinism_report{}.stable()?0:1;}
		 * @endcode */
		[[nodiscard]] bool stable() const noexcept
		{
			return differences.empty();
		}
	};

	/** @brief Fixture operation を jobs/seed/root/order matrix で比較する。
	 * @tparam Operation `(bundle,jobs,seed)->result<string>`。 @param[in] fixture Input fixture。
	 * @param[in] operation Production-path consumer。 @param[in] options Matrix axes。
	 * @retval value Stable differences report or fixture/operation error。
	 * @pre Operation output excludes operational metadata。 @post Fixture remains immutable。
	 * @note Semantic backend is supplied by production code, never fabricated here。
	 * @code{.cpp}
	 * #include <cxxlens/testing.hpp>
	 * int main(){auto f=cxxlens::testing::workspace_fixture::cpp("");auto r=
	 * cxxlens::testing::check_determinism(f,[](const auto&
	 * b,auto,auto)->cxxlens::result<std::string>{return b.to_json();});return
	 * r&&r.value().stable()?0:1;}
	 * @endcode
	 */
	template <class Operation>
	[[nodiscard]] result<determinism_report>
	check_determinism(const workspace_fixture& fixture,
					  Operation&& operation,
					  const determinism_options& options = {})
	{
		determinism_report report;
		std::optional<std::string> baseline;
		std::size_t root_index = 0U;
		for (const auto jobs : options.parallelism)
			for (const auto seed : options.scheduler_seeds)
				for (std::size_t order = 0U; order < (options.reverse_input_order ? 2U : 1U);
					 ++order)
				{
					const path root = options.relocate_workspace_root && root_index++ % 2U != 0U
						? path{"/relocated/workspace"}
						: path{"/workspace"};
					auto bundle = fixture.materialize(root);
					if (!bundle)
						return std::move(bundle.error());
					if (order != 0U)
					{
						std::ranges::reverse(bundle.value().files);
						std::ranges::reverse(bundle.value().variants);
					}
					auto output = operation(bundle.value(), jobs, seed);
					if (!output)
						return std::move(output.error());
					++report.executions;
					if (!baseline)
						baseline = output.value();
					else if (*baseline != output.value())
						report.differences.push_back("jobs=" + std::to_string(jobs) +
													 ";seed=" + std::to_string(seed) +
													 ";order=" + std::to_string(order));
				}
		return report;
	}
} // namespace cxxlens::testing
