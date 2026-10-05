#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace clang
{
	class ASTContext;
	class Preprocessor;
	class CodeGenOptions;
	class RecordDecl;
	class FunctionDecl;
} // namespace clang
namespace cxxlens::detail::clang22
{
	struct project_abi_limits
	{
		std::size_t maximum_operations{8'000'000U};
		std::size_t maximum_depth{128U};
		std::size_t maximum_signature_bytes{8U * 1024U * 1024U};
		std::size_t maximum_extents{100'000U};
	};
	struct project_abi_observation
	{
		std::string abi_state{"unknown"}, layout_state{"unknown"};
		std::string abi_context, abi_signature, reason;
		std::optional<std::uint64_t> byte_size, byte_alignment;
		std::vector<std::pair<std::uint64_t, std::uint64_t>> occupied_ranges;
	};
	class project_abi_observer
	{
	  public:
		project_abi_observer(clang::ASTContext&,
							 clang::Preprocessor&,
							 const clang::CodeGenOptions&,
							 project_abi_limits = {});
		~project_abi_observer();
		project_abi_observer(const project_abi_observer&) = delete;
		project_abi_observer& operator=(const project_abi_observer&) = delete;
		[[nodiscard]] sdk::result<project_abi_observation> record(const clang::RecordDecl&);
		[[nodiscard]] sdk::result<project_abi_observation> function(const clang::FunctionDecl&);

	  private:
		class implementation;
		std::unique_ptr<implementation> impl_;
	};
} // namespace cxxlens::detail::clang22
