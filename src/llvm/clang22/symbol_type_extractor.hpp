#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/core/failure.hpp>
#include <cxxlens/workspace.hpp>

#include "../common/frontend_port.hpp"

namespace clang
{
	class ASTContext;
	class CompilerInstance;
	class NamedDecl;
	class QualType;
	class SourceRange;
} // namespace clang

namespace cxxlens::detail::clang22
{
	struct detached_symbol_identity
	{
		symbol_id id;
		facts::name_identity name;
		std::string kind;
		std::string linkage;
	};

	struct detached_type_identity
	{
		type_id id;
		facts::type_identity type;
		std::string kind;
		bool is_const{};
		bool is_volatile{};
		bool is_pointer{};
		bool is_reference{};
		bool dependent{};
	};

	class semantic_identity_adapter
	{
	  public:
		semantic_identity_adapter(clang::CompilerInstance& compiler,
								  const compile_unit& unit,
								  const std::vector<frontend::virtual_source_file>& virtual_files);
		~semantic_identity_adapter();
		semantic_identity_adapter(const semantic_identity_adapter&) = delete;
		semantic_identity_adapter& operator=(const semantic_identity_adapter&) = delete;
		semantic_identity_adapter(semantic_identity_adapter&&) noexcept;
		semantic_identity_adapter& operator=(semantic_identity_adapter&&) noexcept;

		[[nodiscard]] result<detached_symbol_identity> symbol(const clang::NamedDecl& declaration);
		[[nodiscard]] result<detached_type_identity> type(const clang::QualType& value);
		[[nodiscard]] result<source_span> source(const clang::SourceRange& range) const;
		[[nodiscard]] facts::observation_record
		observation(fact_kind kind,
					std::string semantic_key,
					std::optional<source_span> source = std::nullopt) const;
		void mark(facts::observation_record& observation,
				  coverage_state state = coverage_state::covered,
				  std::optional<std::string> reason = std::nullopt) const;
		[[nodiscard]] clang::CompilerInstance& compiler() const noexcept;
		[[nodiscard]] const compile_unit& unit() const noexcept;

	  private:
		struct impl;
		std::unique_ptr<impl> impl_;
	};

	class semantic_extraction_session
	{
	  public:
		virtual ~semantic_extraction_session() = default;
		[[nodiscard]] virtual result<void> consume(clang::ASTContext& context) = 0;
		[[nodiscard]] virtual result<std::vector<facts::observation_record>> take() = 0;
	};

	[[nodiscard]] result<std::unique_ptr<semantic_extraction_session>>
	make_semantic_extractor(clang::CompilerInstance& compiler,
							const compile_unit& unit,
							const std::vector<frontend::virtual_source_file>& virtual_files);
} // namespace cxxlens::detail::clang22
