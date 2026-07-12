#pragma once

/** @file facts.hpp @brief LLVM-free immutable fact and observation-facing value contracts. */

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/core/evidence.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens
{
	namespace detail
	{
		struct fact_store_access;
	} // namespace detail
	/** @brief Persistable semantic fact kinds fixed by the v1 fact schema. */
	enum class fact_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		file,
		compile_command,
		symbol,
		type,
		declaration,
		definition,
		reference,
		call,
		construction,
		conversion,
		inheritance,
		override_relation,
		include_relation,
		macro_definition,
		macro_expansion,
		cfg_summary,
		flow_summary,
		effect_summary,
		dynamic_observation,
		coverage_region,
		custom,
	};

	/** @brief Immutable concrete fact-kind and precision request. */
	class fact_profile
	{
	  public:
		/** @brief Minimal build-context profile. @retval value Canonical profile. @pre None.
		 * @post Includes file and compile command. @note Expansion contains concrete kinds only.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::minimal().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile minimal();
		/** @brief Semantic search profile. @retval value Canonical profile. @pre None.
		 * @post Includes symbol/type/reference/call/relation facts. @note No placeholder reaches
		 * scheduling.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::semantic_search().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile semantic_search();
		/** @brief Refactor profile. @retval value Canonical profile. @pre None. @post Includes
		 * source relations.
		 * @note Mutation still requires independent safety validation.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::refactor().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile refactor();
		/** @brief Generation profile. @retval value Canonical profile. @pre None. @post Includes
		 * declarations.
		 * @note Unsupported surfaces remain explicit.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::generation().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile generation();
		/** @brief Flow profile. @retval value Canonical profile. @pre None. @post Includes
		 * CFG/flow/effect.
		 * @note Path-sensitive capability remains separately negotiated.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::flow().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile flow();
		/** @brief All built-in facts profile. @retval value Canonical profile. @pre None.
		 * @post Contains every non-custom v1 kind. @note Custom kinds require explicit
		 * registration.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::full().to_json().empty();}
		 * @endcode */
		[[nodiscard]] static fact_profile full();
		/** @brief Include a concrete kind. @param[in] kind Kind. @retval value New profile. @pre
		 * Valid enum.
		 * @post Original unchanged and kinds sorted unique. @note Idempotent.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){auto
		 * p=cxxlens::fact_profile::minimal().include(cxxlens::fact_kind::symbol);return
		 * p.to_json().empty();}
		 * @endcode */
		[[nodiscard]] fact_profile include(fact_kind kind) const;
		/** @brief Exclude a concrete kind. @param[in] kind Kind. @retval value New profile. @pre
		 * Valid enum.
		 * @post Original unchanged. @note Excluding an absent kind is idempotent.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){auto
		 * p=cxxlens::fact_profile::full().exclude(cxxlens::fact_kind::custom);return
		 * p.to_json().empty();}
		 * @endcode */
		[[nodiscard]] fact_profile exclude(fact_kind kind) const;
		/** @brief Set required precision. @param[in] value Precision. @retval value New profile.
		 * @pre Valid enum. @post Original unchanged. @note No implicit downgrade occurs.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){auto
		 * p=cxxlens::fact_profile::minimal().precision(cxxlens::precision_level::workspace_semantic);return
		 * p.to_json().empty();}
		 * @endcode */
		[[nodiscard]] fact_profile precision(precision_level value) const;
		/** @brief Serialize concrete expansion. @retval value Canonical JSON. @pre Factory-built
		 * profile.
		 * @post No state changes. @note Order is the fact-kind registry order.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact_profile::minimal().to_json().starts_with("{")?0:1;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		std::vector<fact_kind> kinds_;
		precision_level precision_{precision_level::ast_structural};
	};

	/** @brief Canonical contributors for a detached fact. */
	struct provenance
	{
		/** @brief Sorted unique contributing compile units. */
		std::vector<compile_unit_id> compile_units;
		/** @brief Sorted unique contributing variants. */
		std::vector<build_variant_id> variants;
		/** @brief Namespaced extractor identity. */
		std::string extractor_id;
		/** @brief Versioned extractor semantics. */
		std::string extractor_version;
	};

	/** @brief Generic immutable fact handle detached from all AST lifetimes. */
	class fact
	{
	  public:
		fact() = default;
		/** @brief Stable fact ID. @retval value Full ID. @pre Resolved handle. @post Unchanged.
		 * @note Adapter metadata is excluded. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return cxxlens::fact{}.id().empty()?0:1;}
		 * @endcode */
		[[nodiscard]] fact_id id() const;
		/** @brief Fact kind. @retval value Concrete kind. @pre Resolved handle. @post Unchanged.
		 * @note Opaque custom payloads remain versioned. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_kind kind() const noexcept;
		/** @brief Stable semantic key. @retval value Borrowed key. @pre Handle lives. @post
		 * Unchanged.
		 * @note Qualified name or pretty type alone is invalid. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string_view stable_key() const noexcept;
		/** @brief Optional normalized source. @retval value Detached span. @pre Resolved handle.
		 * @post Unchanged.
		 * @note Invalid source is not fabricated. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<source_span> source() const;
		/** @brief Canonical provenance. @retval value Borrowed provenance. @pre Handle lives. @post
		 * Unchanged.
		 * @note Contributor order is canonical. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const provenance& origin() const noexcept;
		/** @brief Canonical fact projection. @retval value Schema-valid JSON. @pre Valid fact.
		 * @post Unchanged.
		 * @note Operational metadata is absent. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json() const;

	  private:
		struct data;
		explicit fact(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
	};

	/** @brief Semantic declaration category. */
	enum class symbol_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		namespace_,
		record,
		class_,
		struct_,
		union_,
		function,
		method,
		constructor,
		destructor,
		variable,
		field,
		enum_type,
		enum_constant,
		typedef_,
		type_alias,
		template_,
		concept_,
		macro,
		module_,
		parameter,
		unknown
	};
	/** @brief Semantic linkage category. */
	enum class linkage_kind : std::uint8_t
	{
		none,
		internal,
		unique_external,
		external,
		module,
		unknown
	};
	/** @brief C++ access category. */
	enum class access_kind : std::uint8_t
	{
		none,
		public_,
		protected_,
		private_
	};
	/** @brief Structural TypeIR category. */
	enum class type_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		builtin,
		pointer,
		lvalue_reference,
		rvalue_reference,
		array,
		function,
		record,
		enum_,
		typedef_,
		auto_,
		decltype_,
		template_parameter,
		template_specialization,
		dependent,
		unknown
	};
	/** @brief Semantic reference role. */
	enum class reference_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		read,
		write,
		read_write,
		call,
		address_taken,
		type_use,
		template_argument,
		using_exposure,
		override,
		unknown
	};
	/** @brief Semantic call expression category. */
	enum class call_kind : std::uint16_t // NOLINT(performance-enum-size)
	{
		direct_function,
		member,
		virtual_member,
		constructor,
		destructor,
		overloaded_operator,
		builtin_operator,
		function_pointer,
		callback,
		modeled,
		unknown
	};
	/** @brief Call-target resolution category. */
	enum class dispatch_kind : std::uint8_t
	{
		direct_exact,
		static_member_target,
		virtual_candidate_set,
		indirect_candidate_set,
		modeled_candidate_set,
		unresolved
	};

	/** @brief Detached symbol value. */
	class symbol
	{
	  public:
		symbol() = default;
		/** @brief Return semantic symbol ID. @retval value Full stable ID. @pre Resolved value.
		 * @post Unchanged. @note Names alone never form identity. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_id id() const;
		/** @brief Return symbol category. @retval value Typed category. @pre Resolved value.
		 * @post Unchanged. @note Unknown remains explicit. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_kind kind() const noexcept;
		/** @brief Return unqualified display name. @retval value Borrowed spelling. @pre Value
		 * lives.
		 * @post Unchanged. @note Display spelling is non-authoritative. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string_view name() const noexcept;
		/** @brief Return qualified display name. @retval value Borrowed spelling. @pre Value lives.
		 * @post Unchanged. @note It is insufficient for identity. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string_view qualified_name() const noexcept;
		/** @brief Return Clang-independent USR bytes when available. @retval value Optional
		 * borrowed USR.
		 * @pre Value lives. @post Unchanged. @note Missing USR requires structural identity.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<std::string_view> usr() const noexcept;
		/** @brief Return semantic linkage. @retval value Typed linkage. @pre Resolved value.
		 * @post Unchanged. @note Unknown is not treated as none. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] linkage_kind linkage() const noexcept;
		/** @brief Return canonical declaration observation. @retval value Optional detached span.
		 * @pre Resolved value. @post Unchanged. @note Absence remains distinct from invalid source.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<source_span> declaration() const;
		/** @brief Return canonical definition observation. @retval value Optional detached span.
		 * @pre Resolved value. @post Unchanged. @note Multiple conflicting definitions are not
		 * first-wins.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<source_span> definition() const;
		/** @brief Return contributing variants. @retval value Borrowed sorted unique IDs. @pre
		 * Value lives.
		 * @post Unchanged. @note Variant differences remain visible. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::span<const build_variant_id> variants() const noexcept;
		/** @brief Render a human-facing symbol name. @retval value Display string. @pre Resolved
		 * value.
		 * @post Unchanged. @note Never use this projection for equality. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string display_name() const;

	  private:
		struct data;
		explicit symbol(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
		friend class fact_store;
	};

	/** @brief Detached structural type value. */
	class type_ref
	{
	  public:
		type_ref() = default;
		/** @brief Return structural type ID. @retval value Full stable ID. @pre Resolved value.
		 * @post Unchanged. @note Pretty spelling alone is forbidden identity. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_id id() const;
		/** @brief Return type category. @retval value Typed category. @pre Resolved value.
		 * @post Unchanged. @note Dependent and unknown remain distinct. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] type_kind kind() const noexcept;
		/** @brief Return observed display spelling. @retval value Borrowed spelling. @pre Value
		 * lives.
		 * @post Unchanged. @note Non-authoritative display data. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string_view spelling() const noexcept;
		/** @brief Return canonical display spelling. @retval value Borrowed spelling. @pre Value
		 * lives.
		 * @post Unchanged. @note Structural TypeIR remains authoritative. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string_view canonical_spelling() const noexcept;
		/** @brief Return declaring semantic symbol. @retval value Optional declaration ID.
		 * @pre Resolved value. @post Unchanged. @note Non-builtin types require structural
		 * components.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<symbol_id> declaration() const;
		/** @brief Report top-level const qualification. @retval value Qualification bit.
		 * @pre Resolved value. @post Unchanged. @note Pointee qualification is structural TypeIR
		 * data.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] bool is_const() const noexcept;
		/** @brief Report top-level volatile qualification. @retval value Qualification bit.
		 * @pre Resolved value. @post Unchanged. @note No pretty-string parsing occurs. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] bool is_volatile() const noexcept;
		/** @brief Report pointer structure. @retval value True for pointer kind. @pre Resolved
		 * value.
		 * @post Unchanged. @note Derived from TypeIR, not spelling. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] bool is_pointer() const noexcept;
		/** @brief Report reference structure. @retval value True for reference kinds. @pre Resolved
		 * value.
		 * @post Unchanged. @note Lvalue/rvalue remain distinct in kind. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] bool is_reference() const noexcept;
		/** @brief Return structural template arguments. @retval value Detached argument values.
		 * @pre Resolved value. @post Unchanged. @note Declaration order is semantic and preserved.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::vector<type_ref> template_arguments() const;

	  private:
		struct data;
		explicit type_ref(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
		friend class fact_store;
	};

	/** @brief Detached reference fact. */
	class reference
	{
	  public:
		reference() = default;
		/** @brief Return reference fact ID. @retval value Full stable ID. @pre Resolved value.
		 * @post Unchanged. @note Source and target semantics form identity. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_id id() const;
		/** @brief Return referenced symbol. @retval value Semantic target ID. @pre Resolved value.
		 * @post Unchanged. @note Ambiguity is represented before fact creation. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] symbol_id target() const;
		/** @brief Return reference role. @retval value Typed role. @pre Resolved value.
		 * @post Unchanged. @note Unknown is explicit. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] reference_kind kind() const noexcept;
		/** @brief Return normalized occurrence span. @retval value Borrowed detached span. @pre
		 * Value lives.
		 * @post Unchanged. @note Macro origin remains in the span. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const source_span& location() const noexcept;
		/** @brief Report macro origin. @retval value True for macro-derived occurrence. @pre
		 * Resolved value.
		 * @post Unchanged. @note Does not make a macro range directly editable. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] bool from_macro() const noexcept;
		/** @brief Return structured reference evidence. @retval value Borrowed evidence. @pre Value
		 * lives.
		 * @post Unchanged. @note Prose alone is non-authoritative. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const evidence& why() const noexcept;

	  private:
		struct data;
		explicit reference(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
		friend class fact_store;
	};

	/** @brief Detached call fact preserving resolution dimensions separately. */
	class call_site
	{
	  public:
		call_site() = default;
		/** @brief Return call fact ID. @retval value Full stable ID. @pre Resolved value.
		 * @post Unchanged. @note Resolution dimensions remain separate. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_id id() const;
		/** @brief Return call category. @retval value Typed category. @pre Resolved value.
		 * @post Unchanged. @note Unknown is explicit. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] call_kind kind() const noexcept;
		/** @brief Return normalized call span. @retval value Borrowed detached span. @pre Value
		 * lives.
		 * @post Unchanged. @note Macro mapping is retained. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const source_span& location() const noexcept;
		/** @brief Return semantic caller when known. @retval value Optional caller ID. @pre
		 * Resolved value.
		 * @post Unchanged. @note Missing is not fabricated. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<symbol_id> caller() const;
		/** @brief Return statically direct callee when known. @retval value Optional direct ID.
		 * @pre Resolved value. @post Unchanged. @note Virtual candidates are not collapsed into
		 * this field.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<symbol_id> direct_callee() const;
		/** @brief Return possible runtime callees. @retval value Borrowed sorted unique IDs. @pre
		 * Value lives.
		 * @post Unchanged. @note Candidate set and direct target are distinct. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::span<const symbol_id> possible_callees() const noexcept;
		/** @brief Return receiver static type. @retval value Optional structural type. @pre
		 * Resolved value.
		 * @post Unchanged. @note Required for member-call facts. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::optional<type_ref> receiver_static_type() const;
		/** @brief Return dispatch semantics. @retval value Typed dispatch. @pre Resolved value.
		 * @post Unchanged. @note Unresolved dispatch is not an empty direct call. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] dispatch_kind dispatch() const noexcept;
		/** @brief Return resolution confidence. @retval value Confidence category. @pre Resolved
		 * value.
		 * @post Unchanged. @note Independent from guarantee. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] confidence certainty() const noexcept;
		/** @brief Return achieved guarantee. @retval value Guarantee category. @pre Resolved value.
		 * @post Unchanged. @note Independent from confidence. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		/** @brief Return call-resolution evidence. @retval value Borrowed evidence. @pre Value
		 * lives.
		 * @post Unchanged. @note Required for every call fact. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] const evidence& why() const noexcept;

	  private:
		struct data;
		explicit call_site(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
		friend class fact_store;
	};

	/** @brief Direct inheritance edge; never a transitive closure row. */
	struct inheritance_edge
	{
		/** @brief Derived record ID. */
		symbol_id derived;
		/** @brief Direct base record ID. */
		symbol_id base;
		/** @brief Declared inheritance access. */
		access_kind access{access_kind::public_};
		/** @brief Whether this direct edge is virtual. */
		bool is_virtual{};
		/** @brief Normalized base-specifier source. */
		source_span source;
		/** @brief Structured direct-edge evidence. */
		evidence why;
	};
	/** @brief Direct override edge; never an inferred transitive closure row. */
	struct override_edge
	{
		/** @brief Overriding method ID. */
		symbol_id overriding_method;
		/** @brief Directly overridden method ID. */
		symbol_id overridden_method;
		/** @brief Normalized override source. */
		source_span source;
		/** @brief Structured direct-edge evidence. */
		evidence why;
	};
	/** @brief Variant-sensitive direct include relation. */
	struct include_relation
	{
		/** @brief Including file ID. */
		file_id includer;
		/** @brief Literal include spelling. */
		std::string spelling;
		/** @brief Resolved provider file, when known. */
		std::optional<file_id> resolved;
		/** @brief Directive source span. */
		source_span source;
		/** @brief Whether angle brackets were used. */
		bool angled{};
		/** @brief Whether provider is a system header. */
		bool system{};
		/** @brief Whether directive is conditionally active. */
		bool conditional{};
		/** @brief Whether semantic use was observed. */
		bool used{};
		/** @brief Canonically sorted symbols requiring this include. */
		std::vector<symbol_id> symbols_using_it;
	};
	/** @brief Detached macro expansion relation. */
	struct macro_expansion
	{
		/** @brief Macro identifier. */
		std::string name;
		/** @brief Expansion source span. */
		source_span expansion;
		/** @brief Definition span, when available. */
		std::optional<source_span> definition;
		/** @brief Lexical argument spellings in parameter order. */
		std::vector<std::string> arguments;
		/** @brief Whether the macro is function-like. */
		bool function_like{};
	};

	/** @brief Immutable fact snapshot filter expression. */
	class fact_query
	{
	  public:
		fact_query() = default;
		/** @brief Select every fact. @retval value Immutable query. @pre None. @post No filters.
		 * @note Coverage remains separately queryable. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] static fact_query all();
		/** @brief Add kind filter. @param[in] value Concrete kind. @retval value New query.
		 * @pre Valid kind. @post Original unchanged. @note Filters never broaden implicitly.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query kind(fact_kind value) const;
		/** @brief Add file filter. @param[in] value Semantic file ID. @retval value New query.
		 * @pre Valid ID. @post Original unchanged. @note Display paths are not compared.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query file(file_id value) const;
		/** @brief Add semantic owner filter. @param[in] value Owner ID. @retval value New query.
		 * @pre Valid ID. @post Original unchanged. @note Name-only ownership is forbidden.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query owner(symbol_id value) const;
		/** @brief Add source intersection filter. @param[in] value Normalized span. @retval value
		 * New query.
		 * @pre Span validates. @post Original unchanged. @note Half-open byte ranges are used.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query range_intersects(source_span value) const;
		/** @brief Add build variant filter. @param[in] value Variant ID. @retval value New query.
		 * @pre Valid ID. @post Original unchanged. @note Variant disagreement stays visible.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query variant(build_variant_id value) const;
		/** @brief Add versioned custom filter. @param[in] key Namespaced key. @param[in] value
		 * Value.
		 * @retval value New query. @pre Key is registered. @post Original unchanged.
		 * @note Unknown custom keys are errors at execution.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] fact_query custom(std::string key, std::string value) const;

	  private:
		struct data;
		explicit fact_query(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
		friend class fact_store;
	};

	/** @brief Immutable detached query handle over one atomically published fact snapshot. */
	class fact_store
	{
	  public:
		fact_store() = default;
		/** @brief Query one immutable snapshot. @param[in] query Validated query.
		 * @retval value Canonically ordered detached facts or structured error. @pre Store is
		 * bound.
		 * @post Snapshot unchanged. @note Ordering is kind, stable key, then full fact ID.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<fact>> find(fact_query query) const;
		/** @brief Return all symbols. @retval value Canonical detached symbols or error. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note No AST objects escape. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<symbol>> symbols() const;
		/** @brief Return references to a target. @param[in] target Semantic symbol ID.
		 * @retval value Canonical references or error. @pre Valid ID. @post Snapshot unchanged.
		 * @note Empty and unresolved coverage remain distinct.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<reference>> references(symbol_id target) const;
		/** @brief Return call facts. @retval value Canonical detached calls or error. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note Resolution dimensions remain separate. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<call_site>> calls() const;
		/** @brief Return direct inheritance edges. @retval value Canonical edges or error. @pre
		 * Store bound.
		 * @post Snapshot unchanged. @note Transitive closure is not stored as direct rows.
		 * @code{.cpp} #include <cxxlens/facts.hpp> int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<inheritance_edge>> inheritance() const;
		/** @brief Return direct override edges. @retval value Canonical edges or error. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note Transitive closure is derived separately. @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<override_edge>> overrides() const;
		/** @brief Return include relations. @retval value Canonical relations or error. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note Conditional and variant fields remain explicit.
		 * @code{.cpp} #include <cxxlens/facts.hpp> int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<include_relation>> includes() const;
		/** @brief Return macro expansions. @retval value Canonical expansions or error. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note Definition and expansion locations remain distinct.
		 * @code{.cpp} #include <cxxlens/facts.hpp> int main(){return 0;}
		 * @endcode */
		[[nodiscard]] result<std::vector<macro_expansion>> macros() const;
		/** @brief Return coverage for a profile and scope. @param[in] profile Concrete profile.
		 * @param[in] scope Immutable scope. @retval value Authoritative accounting. @pre Store
		 * bound.
		 * @post Snapshot unchanged. @note Partial cache rows never become complete implicitly.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] coverage_report coverage(fact_profile profile,
											   analysis_scope scope = analysis_scope::all()) const;
		/** @brief Serialize matching facts. @param[in] query Validated query. @retval value
		 * Canonical JSON.
		 * @pre Store bound. @post Snapshot unchanged. @note Operational metadata is excluded.
		 * @code{.cpp}
		 * #include <cxxlens/facts.hpp>
		 * int main(){return 0;}
		 * @endcode */
		[[nodiscard]] std::string to_json(fact_query query = fact_query::all()) const;

	  private:
		struct data;
		explicit fact_store(std::shared_ptr<const data> value);
		std::shared_ptr<const data> data_;
		friend struct detail::fact_store_access;
	};
} // namespace cxxlens
