#include "provider_worker.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <ostream>
#include <ranges>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/sdk/provider.hpp>

#if CXXLENS_HAS_CLANG22
#include <clang/AST/DeclCXX.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <llvm/ADT/SmallString.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		using sdk::provider::message_type;

		constexpr std::string_view provider_id = "cxxlens.clang22.reference";
		constexpr std::string_view task_magic = "cxxlens.clang22.task.v1";
		const sdk::semantic_version provider_version{1U, 0U, 0U};
		constexpr std::uint32_t maximum_string_bytes = 64U * 1024U * 1024U;
		constexpr std::uint32_t maximum_arguments = 1024U;

		[[nodiscard]] sdk::error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
		{
			const auto input = std::as_bytes(std::span{value});
			return {input.begin(), input.end()};
		}

		[[nodiscard]] sdk::detached_cell symbol_cell(const sdk::scalar_kind kind,
													 std::string parameter,
													 std::string value,
													 const bool optional = false)
		{
			return {{kind, std::move(parameter), optional},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::detached_cell optional_typed(std::string parameter, std::string value)
		{
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_utf8(std::string value)
		{
			auto output = sdk::detached_cell::utf8(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell optional_bytes(std::vector<std::byte> value)
		{
			auto output = sdk::detached_cell::bytes(std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] std::string limitation_text(const std::vector<std::string>& limitations)
		{
			std::ostringstream output;
			for (std::size_t index = 0U; index < limitations.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << limitations[index];
			}
			return output.str();
		}

		[[nodiscard]] sdk::relation_descriptor observation_descriptor(const std::string& name)
		{
			sdk::relation_descriptor descriptor;
			descriptor.id = name + ".v1";
			descriptor.name = name;
			descriptor.version = {1U, 0U, 0U};
			descriptor.semantic_major = 1U;
			descriptor.semantics = name + "/1";
			descriptor.owner_namespace = "cxxlens.provider.clang22";
			const auto prefix = descriptor.id + ".";
			descriptor.columns = {
				{prefix + "observation",
				 "observation",
				 {sdk::scalar_kind::typed_id, "clang22_observation_id", false},
				 true,
				 sdk::column_role::claim_key},
				{prefix + "compile_unit",
				 "compile_unit",
				 {sdk::scalar_kind::typed_id, "compile_unit_id", false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "semantic_key",
				 "semantic_key",
				 {sdk::scalar_kind::bytes, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "payload_digest",
				 "payload_digest",
				 {sdk::scalar_kind::digest, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "source",
				 "source",
				 {sdk::scalar_kind::typed_id, "source_span_id", true},
				 false,
				 sdk::column_role::authoritative_payload},
				{prefix + "exact_equivalence",
				 "exact_equivalence",
				 {sdk::scalar_kind::boolean, {}, false},
				 true,
				 sdk::column_role::authoritative_payload},
				{prefix + "limitation",
				 "limitation",
				 {sdk::scalar_kind::utf8_string, {}, true},
				 false,
				 sdk::column_role::authoritative_payload},
			};
			descriptor.key_columns = {prefix + "observation"};
			descriptor.merge = sdk::merge_mode::functional_assertion;
			descriptor.conflict_columns = {
				prefix + "compile_unit",
				prefix + "semantic_key",
				prefix + "payload_digest",
				prefix + "source",
				prefix + "exact_equivalence",
				prefix + "limitation",
			};
			descriptor.descriptor_digest = *sdk::semantic_digest("cxxlens.relation-descriptor.v1",
																 descriptor.canonical_form());
			return descriptor;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		observation_row(const sdk::relation_descriptor& descriptor,
						const detached_observation& observation,
						const bool exact,
						const std::string_view limitation)
		{
			sdk::row_builder builder{descriptor};
			const auto projection = observation.canonical_form();
			const auto prefix = descriptor.id + ".";
			for (auto result : {
					 builder.set({descriptor.id,
								  prefix + "observation",
								  {sdk::scalar_kind::typed_id, "clang22_observation_id", false}},
								 sdk::detached_cell::typed(
									 "clang22_observation_id",
									 *sdk::semantic_digest("clang22.observation.v1", projection))),
					 builder.set(
						 {descriptor.id,
						  prefix + "compile_unit",
						  {sdk::scalar_kind::typed_id, "compile_unit_id", false}},
						 sdk::detached_cell::typed("compile_unit_id", observation.compile_unit)),
					 builder.set({descriptor.id,
								  prefix + "semantic_key",
								  {sdk::scalar_kind::bytes, {}, false}},
								 sdk::detached_cell::bytes(bytes(observation.semantic_key))),
					 builder.set({descriptor.id,
								  prefix + "payload_digest",
								  {sdk::scalar_kind::digest, {}, false}},
								 symbol_cell(sdk::scalar_kind::digest,
											 {},
											 *sdk::semantic_digest("clang22.observation-payload.v1",
																   projection))),
					 builder.set({descriptor.id,
								  prefix + "exact_equivalence",
								  {sdk::scalar_kind::boolean, {}, false}},
								 sdk::detached_cell::boolean(exact)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (observation.source_span_id)
			{
				auto result =
					builder.set({descriptor.id,
								 prefix + "source",
								 {sdk::scalar_kind::typed_id, "source_span_id", true}},
								optional_typed("source_span_id", *observation.source_span_id));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			if (!limitation.empty())
			{
				auto result = builder.set({descriptor.id,
										   prefix + "limitation",
										   {sdk::scalar_kind::utf8_string, {}, true}},
										  optional_utf8(std::string{limitation}));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		entity_row(const detached_observation& observation,
				   const std::string& entity,
				   const std::string& toolchain,
				   const bool exact)
		{
			using relation = cc::relations::entity;
			relation::builder builder;
			const auto projection = observation.canonical_form();
			const auto kind = observation.payload.contains("symbol.kind")
				? observation.payload.at("symbol.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::entity_column>(
						 sdk::detached_cell::typed("cc_entity_id", entity)),
					 builder.set<relation::canonicalization>(
						 symbol_cell(sdk::scalar_kind::closed_symbol,
									 "cc.canonicalization-state/1",
									 exact ? "canonical" : "provider_local")),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.entity-kind/1", kind)),
					 builder.set<relation::structural_signature_digest>(symbol_cell(
						 sdk::scalar_kind::digest,
						 {},
						 *sdk::semantic_digest("cc.entity.structural-signature.v1", projection))),
					 builder.set<relation::toolchain>(
						 optional_typed("toolchain_context_id", toolchain)),
					 builder.set<relation::provider_local_key>(
						 optional_bytes(bytes(observation.semantic_key))),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (observation.source_span_id)
			{
				auto result = builder.set<relation::anchor>(
					optional_typed("source_span_id", *observation.source_span_id));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			const auto qualified_name = observation.payload.find("symbol.qualified_name");
			if (qualified_name != observation.payload.end() && !qualified_name->second.empty())
			{
				auto result =
					builder.set<relation::qualified_name>(optional_utf8(qualified_name->second));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		call_site_row(const detached_observation& observation,
					  const std::string& call,
					  const std::map<std::string, std::string, std::less<>>& entities,
					  const std::uint64_t ordinal)
		{
			using relation = cc::relations::call_site;
			relation::builder builder;
			const auto caller = observation.payload.find("call.caller");
			const auto kind = observation.payload.contains("call.kind")
				? observation.payload.at("call.kind")
				: "unknown";
			for (auto result : {
					 builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", call)),
					 builder.set<relation::compile_unit>(
						 sdk::detached_cell::typed("compile_unit_id", observation.compile_unit)),
					 builder.set<relation::kind>(
						 symbol_cell(sdk::scalar_kind::open_symbol, "cc.call-kind/1", kind)),
					 builder.set<relation::source>(
						 sdk::detached_cell::typed("source_span_id", *observation.source_span_id)),
					 builder.set<relation::ordinal>(sdk::detached_cell::unsigned_integer(ordinal)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (caller != observation.payload.end())
			{
				const auto found = entities.find(caller->second);
				if (found != entities.end())
				{
					auto result = builder.set<relation::caller>(
						optional_typed("cc_entity_id", found->second));
					if (!result)
						return sdk::unexpected(std::move(result.error()));
				}
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row> direct_target_row(const std::string& call,
																	   const std::string& target)
		{
			using relation = cc::relations::call_direct_target;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", call)),
					 builder.set<relation::target>(
						 sdk::detached_cell::typed("cc_entity_id", target)),
					 builder.set<relation::resolution>(symbol_cell(sdk::scalar_kind::open_symbol,
																   "cc.direct-target-resolution/1",
																   "syntactic_direct")),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		class binary_writer
		{
		  public:
			void string(const std::string_view value)
			{
				integer(static_cast<std::uint32_t>(value.size()));
				const auto data = std::as_bytes(std::span{value});
				output_.insert(output_.end(), data.begin(), data.end());
			}

			void integer(const std::uint32_t value)
			{
				for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
					output_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
			}

			[[nodiscard]] std::vector<std::byte> finish() &&
			{
				return std::move(output_);
			}

		  private:
			std::vector<std::byte> output_;
		};

		class binary_reader
		{
		  public:
			explicit binary_reader(const std::span<const std::byte> input) : input_{input} {}

			[[nodiscard]] sdk::result<std::uint32_t> integer()
			{
				if (remaining() < 4U)
					return sdk::unexpected(provider_error("provider.task-input-invalid", "length"));
				std::uint32_t output{};
				for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
					output |= std::to_integer<std::uint32_t>(input_[offset_++]) << shift;
				return output;
			}

			[[nodiscard]] sdk::result<std::string> string()
			{
				auto length = integer();
				if (!length)
					return sdk::unexpected(std::move(length.error()));
				if (*length > maximum_string_bytes || remaining() < *length)
					return sdk::unexpected(provider_error("provider.task-input-invalid", "string"));
				const auto* data = reinterpret_cast<const char*>(input_.data() + offset_);
				std::string output{data, *length};
				offset_ += *length;
				return output;
			}

			[[nodiscard]] bool empty() const noexcept
			{
				return offset_ == input_.size();
			}

		  private:
			[[nodiscard]] std::size_t remaining() const noexcept
			{
				return input_.size() - offset_;
			}
			std::span<const std::byte> input_;
			std::size_t offset_{};
		};

		class stream_sink final : public sdk::provider::frame_sink
		{
		  public:
			explicit stream_sink(std::ostream& output) : output_{&output} {}

			sdk::result<void> write(const std::span<const std::byte> data) override
			{
				output_->write(reinterpret_cast<const char*>(data.data()),
							   static_cast<std::streamsize>(data.size()));
				return output_->good()
					? sdk::result<void>{}
					: sdk::unexpected(provider_error("provider.worker-write", "stdout"));
			}

		  private:
			std::ostream* output_;
		};

		[[nodiscard]] std::optional<std::uint64_t> unsigned_field(const std::string_view text,
																  const std::size_t separator)
		{
			if (separator == std::string_view::npos || separator + 1U >= text.size())
				return std::nullopt;
			std::uint64_t output{};
			for (const auto byte : text.substr(separator + 1U))
			{
				if (byte < '0' || byte > '9')
					return std::nullopt;
				const auto digit = static_cast<std::uint64_t>(byte - '0');
				if (output > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
					return std::nullopt;
				output = output * 10U + digit;
			}
			return output;
		}

#if CXXLENS_HAS_CLANG22
		[[nodiscard]] std::string declaration_key(const clang::NamedDecl& declaration)
		{
			llvm::SmallString<256> storage;
			const auto* canonical = llvm::cast<clang::NamedDecl>(declaration.getCanonicalDecl());
			if (!clang::index::generateUSRForDecl(canonical, storage) && !storage.empty())
				return "clang-usr:" + storage.str().str();
			return *sdk::semantic_digest("clang22.declaration-fallback.v1",
										 canonical->getQualifiedNameAsString() + "\n" +
											 canonical->getDeclKindName());
		}

		[[nodiscard]] std::string declaration_kind(const clang::FunctionDecl& declaration)
		{
			if (llvm::isa<clang::CXXConstructorDecl>(declaration))
				return "constructor";
			if (llvm::isa<clang::CXXDestructorDecl>(declaration))
				return "destructor";
			if (llvm::isa<clang::CXXMethodDecl>(declaration))
				return "method";
			return "function";
		}

		class observation_visitor final : public clang::RecursiveASTVisitor<observation_visitor>
		{
		  public:
			observation_visitor(provider::clang22::borrowed_translation_unit& unit,
								observation_batch& output)
				: unit_{&unit}, output_{&output}
			{
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr)
					return true;
				auto previous = current_function_;
				current_function_ = declaration_key(*declaration);
				const auto traversed =
					clang::RecursiveASTVisitor<observation_visitor>::TraverseFunctionDecl(
						declaration);
				current_function_ = std::move(previous);
				return traversed;
			}

			bool VisitFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr || declaration->isImplicit() ||
					!unit_->source_manager().isWrittenInMainFile(declaration->getLocation()))
					return true;
				detached_observation entity;
				entity.kind = observation_kind::entity;
				entity.compile_unit = output_->unit;
				entity.semantic_key = declaration_key(*declaration);
				entity.payload.emplace("symbol.kind", declaration_kind(*declaration));
				entity.payload.emplace("symbol.qualified_name",
									   declaration->getQualifiedNameAsString());
				entity.payload.emplace("symbol.signature",
									   declaration->getType().getCanonicalType().getAsString());
				auto source =
					provider::clang22::normalize_source(*unit_, declaration->getSourceRange());
				if (source)
					entity.source_span_id = std::move(source->id);
				insert(std::move(entity));

				detached_observation type;
				type.kind = observation_kind::type;
				type.compile_unit = output_->unit;
				type.payload.emplace("type.canonical",
									 declaration->getType().getCanonicalType().getAsString());
				type.semantic_key =
					*sdk::semantic_digest("clang22.type.v1", type.payload.at("type.canonical"));
				insert(std::move(type));
				return true;
			}

			bool VisitCallExpr(clang::CallExpr* expression)
			{
				if (expression == nullptr ||
					!unit_->source_manager().isWrittenInMainFile(expression->getExprLoc()))
					return true;
				detached_observation call;
				call.kind = observation_kind::call;
				call.compile_unit = output_->unit;
				call.payload.emplace("call.kind", "direct_function");
				if (!current_function_.empty())
					call.payload.emplace("call.caller", current_function_);
				if (const auto* callee = expression->getDirectCallee(); callee != nullptr)
					call.payload.emplace("call.direct_callee", declaration_key(*callee));
				else
					call.payload.emplace("call.unresolved_reason", "no-direct-callee");
				auto source =
					provider::clang22::normalize_source(*unit_, expression->getSourceRange());
				if (!source)
				{
					++output_->failed_count;
					output_->diagnostics.push_back(source.error().code);
					return true;
				}
				call.source_span_id = std::move(source->id);
				call.semantic_key =
					*sdk::semantic_digest("clang22.call.v1",
										  current_function_ + "\n" + *call.source_span_id + "\n" +
											  call.payload["call.direct_callee"]);
				insert(std::move(call));
				return true;
			}

		  private:
			void insert(detached_observation observation)
			{
				const auto key = std::to_string(static_cast<unsigned>(observation.kind)) + "\n" +
					observation.semantic_key + "\n" +
					observation.source_span_id.value_or(std::string{});
				if (seen_.emplace(key, output_->observations.size()).second)
					output_->observations.push_back(std::move(observation));
			}

			provider::clang22::borrowed_translation_unit* unit_;
			observation_batch* output_;
			std::string current_function_;
			std::map<std::string, std::size_t, std::less<>> seen_;
		};
#endif

		[[nodiscard]] sdk::result<observation_batch> extract(const clang22_task_input& input)
		{
			observation_batch output;
			output.unit = input.compile_unit;
			output.variant = input.variant;
			provider::clang22::translation_unit_input native_input{
				input.logical_path, input.source, input.arguments};
			auto outcome = provider::clang22::with_translation_unit(
				native_input,
				[&output](provider::clang22::borrowed_translation_unit& unit) -> sdk::result<void>
				{
#if CXXLENS_HAS_CLANG22
					observation_visitor visitor{unit, output};
					if (!visitor.TraverseDecl(unit.ast().getTranslationUnitDecl()))
						return sdk::unexpected(
							provider_error("provider.native-traversal-failed", output.unit));
					return {};
#else
					(void)unit;
					return sdk::unexpected(provider_error(
						"native.unsupported-clang-major", output.unit, "clang-major-22"));
#endif
				});
			if (!outcome)
				return sdk::unexpected(std::move(outcome.error()));
			std::ranges::sort(output.observations,
							  {},
							  [](const detached_observation& observation)
							  {
								  return observation.canonical_form();
							  });
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		class canonical_provider final : public sdk::provider::portable_provider
		{
		  public:
			canonical_provider(clang22_task_input request, std::string toolchain_digest)
				: request_{std::move(request)}, toolchain_digest_{std::move(toolchain_digest)}
			{
			}

			[[nodiscard]] std::string_view id() const noexcept override
			{
				return provider_id;
			}

			[[nodiscard]] sdk::semantic_version version() const noexcept override
			{
				return provider_version;
			}

			sdk::result<void> run(const sdk::provider::task& task,
								  sdk::provider::context& context) override
			{
				std::vector<std::string> limitations;
				const auto invocation_exact =
					invocation_has_exact_equivalence(request_.arguments, limitations);
				auto batch = extract(request_);
				if (!batch)
					return sdk::unexpected(std::move(batch.error()));
				auto normalized = canonicalize_provider_batch(
					*batch, toolchain_digest_, invocation_exact, std::move(limitations));
				if (!normalized)
					return sdk::unexpected(std::move(normalized.error()));

				const auto emit = [&](const sdk::relation_descriptor& descriptor,
									  const std::vector<sdk::detached_row>& rows,
									  const std::string_view group) -> sdk::result<void>
				{
					auto sink = context.relation(descriptor);
					if (auto opened = sink.begin(
							std::string{group}, "clang22-atomic", descriptor.id + "-batch");
						!opened)
						return opened;
					for (const auto& row : rows)
						if (auto pushed = sink.push(row); !pushed)
							return pushed;
					return sink.end();
				};

				for (const auto& [descriptor, rows, group] : {
						 std::tuple{entity_observation_descriptor(),
									&normalized->entity_observations,
									std::string_view{"observation"}},
						 std::tuple{type_observation_descriptor(),
									&normalized->type_observations,
									std::string_view{"observation"}},
						 std::tuple{call_observation_descriptor(),
									&normalized->call_observations,
									std::string_view{"observation"}},
						 std::tuple{cc::relations::entity::descriptor(),
									&normalized->entities,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_site::descriptor(),
									&normalized->call_sites,
									std::string_view{"canonical"}},
						 std::tuple{cc::relations::call_direct_target::descriptor(),
									&normalized->direct_targets,
									std::string_view{"canonical"}},
					 })
					if (auto emitted = emit(descriptor, *rows, group); !emitted)
						return emitted;

				for (auto item : normalized->unresolved)
					context.unresolved().add(std::move(item));
				const auto coverage_state =
					normalized->exact_equivalence ? "covered" : "unresolved";
				const auto reason = limitation_text(normalized->equivalence_limitations);
				for (const auto kind : {
						 "frontend.clang22.observation",
						 "cc.entity",
						 "cc.call-extraction",
					 })
				{
					context.coverage().request(kind, task.task_id);
					if (auto classified = context.coverage().classify(
							{kind, task.task_id, coverage_state, reason});
						!classified)
						return classified;
				}
				context.evidence().add(
					{"provider.clang22.execution",
					 task.task_id,
					 std::string{provider_id},
					 normalized->exact_equivalence ? "exact" : "provider-local"});
				return {};
			}

		  private:
			clang22_task_input request_;
			std::string toolchain_digest_;
		};
	} // namespace

	sdk::result<void> detached_observation::validate() const
	{
		if (compile_unit.empty() || semantic_key.empty())
			return sdk::unexpected(provider_error("provider.observation-invalid", "identity"));
		if (source_span_id && source_span_id->empty())
			return sdk::unexpected(
				provider_error("provider.observation-invalid", "source_span_id"));
		for (const auto& [key, value] : payload)
			if (key.empty() || key.find('\0') != std::string::npos ||
				value.find('\0') != std::string::npos)
				return sdk::unexpected(provider_error("provider.observation-invalid", "payload"));
		return {};
	}

	std::string detached_observation::canonical_form() const
	{
		std::ostringstream output;
		output << static_cast<unsigned>(kind) << '\n'
			   << compile_unit << '\n'
			   << semantic_key << '\n';
		for (const auto& [key, value] : payload)
			output << key.size() << ':' << key << value.size() << ':' << value;
		output << '\n' << source_span_id.value_or(std::string{});
		return output.str();
	}

	sdk::result<void> observation_batch::validate() const
	{
		if (unit.empty() || variant.empty())
			return sdk::unexpected(provider_error("provider.batch-invalid", "identity"));
		for (const auto& observation : observations)
		{
			if (auto valid = observation.validate(); !valid)
				return valid;
			if (observation.compile_unit != unit)
				return sdk::unexpected(provider_error("provider.batch-invalid", "compile_unit"));
		}
		for (const auto& diagnostic : diagnostics)
			if (diagnostic.empty())
				return sdk::unexpected(provider_error("provider.batch-invalid", "diagnostic"));
		return {};
	}

	sdk::result<void> clang22_task_input::validate() const
	{
		if (compile_unit.empty() || variant.empty())
			return sdk::unexpected(provider_error("provider.task-input-invalid", "identity"));
		return provider::clang22::translation_unit_input{logical_path, source, arguments}
			.validate();
	}

	sdk::result<std::vector<std::byte>> encode_task_input(const clang22_task_input& input)
	{
		if (auto valid = input.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (input.source.size() > maximum_string_bytes)
			return sdk::unexpected(provider_error("provider.task-input-invalid", "source-size"));
		binary_writer writer;
		writer.string(task_magic);
		writer.string(input.compile_unit);
		writer.string(input.variant);
		writer.string(input.logical_path);
		writer.string(input.source);
		writer.integer(static_cast<std::uint32_t>(input.arguments.size()));
		for (const auto& argument : input.arguments)
			writer.string(argument);
		return std::move(writer).finish();
	}

	sdk::result<clang22_task_input> decode_task_input(const std::span<const std::byte> input)
	{
		binary_reader reader{input};
		auto magic = reader.string();
		auto compile_unit = reader.string();
		auto variant = reader.string();
		auto logical_path = reader.string();
		auto source = reader.string();
		auto count = reader.integer();
		if (!magic || !compile_unit || !variant || !logical_path || !source || !count ||
			*magic != task_magic || *count > maximum_arguments)
			return sdk::unexpected(provider_error("provider.task-input-invalid", "payload"));
		clang22_task_input output{
			std::move(*compile_unit),
			std::move(*variant),
			std::move(*logical_path),
			std::move(*source),
			{},
		};
		output.arguments.reserve(*count);
		for (std::uint32_t index = 0U; index < *count; ++index)
		{
			auto argument = reader.string();
			if (!argument)
				return sdk::unexpected(std::move(argument.error()));
			output.arguments.push_back(std::move(*argument));
		}
		if (!reader.empty())
			return sdk::unexpected(provider_error("provider.task-input-invalid", "trailing-bytes"));
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}

	sdk::relation_descriptor entity_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.entity_observation");
	}

	sdk::relation_descriptor type_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.type_observation");
	}

	sdk::relation_descriptor call_observation_descriptor()
	{
		return observation_descriptor("frontend.clang22.call_observation");
	}

	bool invocation_has_exact_equivalence(const std::span<const std::string> arguments,
										  std::vector<std::string>& limitations)
	{
		static constexpr std::array<std::string_view, 5U> unsupported_semantic_prefixes{
			"-fabi-version",
			"-fno-gnu-unique",
			"-fconcepts-diagnostics-depth",
			"-Wno-psabi",
			"-mabi=",
		};
		for (const auto& argument : arguments)
			for (const auto prefix : unsupported_semantic_prefixes)
				if (argument.starts_with(prefix))
					limitations.push_back("ignored-or-gcc-specific-option:" + argument);
		std::ranges::sort(limitations);
		limitations.erase(std::ranges::unique(limitations).begin(), limitations.end());
		return limitations.empty();
	}

	sdk::result<canonicalized_provider_batch>
	canonicalize_provider_batch(const observation_batch& batch,
								const std::string& toolchain_digest,
								const bool invocation_exact,
								std::vector<std::string> invocation_limitations)
	{
		if (auto valid = batch.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!toolchain_digest.starts_with("sha256:"))
			return sdk::unexpected(
				provider_error("provider.toolchain-digest-invalid", "toolchain"));
		for (const auto& diagnostic : batch.diagnostics)
			invocation_limitations.push_back("frontend-diagnostic:" + diagnostic);
		std::ranges::sort(invocation_limitations);
		invocation_limitations.erase(std::ranges::unique(invocation_limitations).begin(),
									 invocation_limitations.end());

		canonicalized_provider_batch output;
		output.exact_equivalence =
			invocation_exact && invocation_limitations.empty() && batch.failed_count == 0U;
		output.equivalence_limitations = std::move(invocation_limitations);
		const auto limitation = limitation_text(output.equivalence_limitations);
		const auto toolchain = *sdk::semantic_digest("toolchain-context", toolchain_digest);

		std::map<std::string, std::string, std::less<>> entity_ids;
		for (const auto& observation : batch.observations)
			if (observation.kind == observation_kind::entity)
			{
				const auto entity = *sdk::semantic_digest("cc-entity",
														  observation.semantic_key + "\n" +
															  toolchain + "\n" + batch.variant);
				entity_ids.emplace(observation.semantic_key, entity);
				auto local = observation_row(entity_observation_descriptor(),
											 observation,
											 output.exact_equivalence,
											 limitation);
				auto canonical =
					entity_row(observation, entity, toolchain, output.exact_equivalence);
				if (!local)
					return sdk::unexpected(std::move(local.error()));
				if (!canonical)
					return sdk::unexpected(std::move(canonical.error()));
				output.entity_observations.push_back(std::move(*local));
				output.entities.push_back(std::move(*canonical));
			}

		std::uint64_t call_ordinal{};
		for (const auto& observation : batch.observations)
		{
			if (observation.kind == observation_kind::type)
			{
				auto local = observation_row(type_observation_descriptor(),
											 observation,
											 output.exact_equivalence,
											 limitation);
				if (!local)
					return sdk::unexpected(std::move(local.error()));
				output.type_observations.push_back(std::move(*local));
				continue;
			}
			if (observation.kind != observation_kind::call)
				continue;
			auto local = observation_row(
				call_observation_descriptor(), observation, output.exact_equivalence, limitation);
			if (!local)
				return sdk::unexpected(std::move(local.error()));
			output.call_observations.push_back(std::move(*local));
			if (!observation.source_span_id)
			{
				output.unresolved.push_back(
					{"provider.source-unavailable", observation.semantic_key, "cc.call_site"});
				continue;
			}
			const auto call = *sdk::semantic_digest(
				"cc-call",
				batch.unit + "\n" + batch.variant + "\n" + *observation.source_span_id + "\n" +
					observation.semantic_key + "\n" + std::to_string(call_ordinal));
			auto site = call_site_row(observation, call, entity_ids, call_ordinal++);
			if (!site)
				return sdk::unexpected(std::move(site.error()));
			output.call_sites.push_back(std::move(*site));
			const auto target = observation.payload.find("call.direct_callee");
			const auto mapped = target == observation.payload.end()
				? entity_ids.end()
				: entity_ids.find(target->second);
			if (target == observation.payload.end() || mapped == entity_ids.end())
			{
				const auto reason = observation.payload.contains("call.unresolved_reason")
					? observation.payload.at("call.unresolved_reason")
					: "direct-target-unresolved";
				output.unresolved.push_back({"provider.direct-target-unresolved", call, reason});
				continue;
			}
			auto direct = direct_target_row(call, mapped->second);
			if (!direct)
				return sdk::unexpected(std::move(direct.error()));
			output.direct_targets.push_back(std::move(*direct));
		}
		return output;
	}

	int run_provider_worker(const std::span<const std::byte> input, std::ostream& output)
	{
		auto frames = sdk::provider::decode_frame_stream(input);
		if (!frames || frames->size() != 5U || frames->at(0U).type != message_type::hello_ack ||
			frames->at(2U).type != message_type::open_task ||
			frames->at(3U).type != message_type::credit)
			return EXIT_FAILURE;
		auto hello = sdk::provider::decode_control_text(frames->at(0U).control);
		auto task_control = sdk::provider::decode_control_text(frames->at(2U).control);
		auto credit = sdk::provider::decode_control_text(frames->at(3U).control);
		if (!hello || !task_control || !credit)
			return EXIT_FAILURE;
		const auto first_credit_separator = credit->find('|');
		auto byte_credit = unsigned_field(*credit, std::string_view::npos);
		if (first_credit_separator != std::string::npos)
		{
			std::uint64_t parsed{};
			for (const auto byte : std::string_view{*credit}.substr(0U, first_credit_separator))
			{
				if (byte < '0' || byte > '9')
					return EXIT_FAILURE;
				parsed = parsed * 10U + static_cast<std::uint64_t>(byte - '0');
			}
			byte_credit = parsed;
		}
		const auto frame_credit = unsigned_field(*credit, first_credit_separator);
		if (!byte_credit || !frame_credit || *byte_credit == 0U || *frame_credit == 0U)
			return EXIT_FAILURE;
		if (!std::string_view{*hello}.starts_with(std::string{provider_id} + "|1.0.0|") ||
			std::ranges::count(std::string_view{*hello}, '|') != 3)
			return EXIT_FAILURE;

		stream_sink sink{output};
		sdk::provider::protocol_writer writer{sink};
		writer.grant_credit({*byte_credit, *frame_credit});
		if (!writer.send(message_type::hello, frames->at(0U).control))
			return EXIT_FAILURE;

		auto request = decode_task_input(frames->at(2U).payload);
		if (!request)
		{
			const auto failed = bytes("provider.task-input-invalid|payload");
			std::vector<std::byte> control{static_cast<std::byte>(0x78U),
										   static_cast<std::byte>(failed.size())};
			control.insert(control.end(), failed.begin(), failed.end());
			(void)writer.send(message_type::task_failed, control);
			return EXIT_SUCCESS;
		}
		const auto control_parts = std::string_view{*task_control};
		const auto separator = control_parts.find('|');
		if (separator == std::string_view::npos)
			return EXIT_FAILURE;
		const std::string task_id{control_parts.substr(0U, separator)};
		sdk::provider::task task{
			task_id,
			{request->compile_unit,
			 *sdk::semantic_digest("cxxlens.provider-project.v1", request->compile_unit),
			 ".",
			 {request->compile_unit}},
			{entity_observation_descriptor(),
			 type_observation_descriptor(),
			 call_observation_descriptor(),
			 cc::relations::entity::descriptor(),
			 cc::relations::call_site::descriptor(),
			 cc::relations::call_direct_target::descriptor()},
			"all",
			"cc.clang22-canonical-1",
		};
		const auto toolchain_separator = control_parts.rfind('|');
		const auto environment_separator = toolchain_separator == std::string_view::npos
			? std::string_view::npos
			: control_parts.rfind('|', toolchain_separator - 1U);
		const std::string toolchain_digest{
			environment_separator == std::string_view::npos
				? std::string_view{}
				: control_parts.substr(environment_separator + 1U,
									   toolchain_separator - environment_separator - 1U)};
		canonical_provider provider{std::move(*request), toolchain_digest};
		(void)sdk::provider::run_worker(provider, task, writer);
		return EXIT_SUCCESS;
	}
} // namespace cxxlens::detail::clang22
