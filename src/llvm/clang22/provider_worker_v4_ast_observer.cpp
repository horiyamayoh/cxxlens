#include "provider_worker_v4_ast_observer.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <cxxlens/provider/clang22.hpp>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <clang/AST/Attr.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		using namespace std::string_view_literals;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool valid_text(const std::string_view value) noexcept
		{
			return !value.empty() && value.find('\0') == std::string_view::npos &&
				sdk::validate_utf8_text(value);
		}

		[[nodiscard]] bool checked_add(const std::size_t left,
									   const std::size_t right,
									   const std::size_t maximum,
									   std::size_t& output) noexcept
		{
			if (left > maximum || right > maximum - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_multiply(const std::size_t left,
											const std::size_t right,
											const std::size_t maximum,
											std::size_t& output) noexcept
		{
			if (left != 0U && right > maximum / left)
				return false;
			output = left * right;
			return output <= maximum;
		}

		[[nodiscard]] constexpr std::size_t decimal_digits(std::uint64_t value) noexcept
		{
			std::size_t output{1U};
			while (value >= 10U)
			{
				value /= 10U;
				++output;
			}
			return output;
		}

		[[nodiscard]] bool append_framed_size(const std::size_t value_size,
											  const std::size_t maximum,
											  std::size_t& total) noexcept
		{
			std::size_t framed{};
			if (!checked_add(decimal_digits(value_size), 1U, maximum, framed) ||
				!checked_add(framed, value_size, maximum, framed) ||
				!checked_add(total, framed, maximum, total))
				return false;
			return true;
		}

		[[maybe_unused]] [[nodiscard]] std::optional<std::size_t>
		canonical_size(const provider_worker_v4_ast_observation& observation,
					   const std::size_t maximum) noexcept
		{
			std::size_t total{};
			if (!append_framed_size(
					"cxxlens.clang22.task-v4.ast-observation.v1"sv.size(), maximum, total) ||
				!append_framed_size(
					decimal_digits(static_cast<std::uint8_t>(observation.kind)), maximum, total) ||
				!append_framed_size(observation.compile_unit.size(), maximum, total) ||
				!append_framed_size(observation.semantic_key.size(), maximum, total) ||
				!append_framed_size(decimal_digits(observation.payload.size()), maximum, total))
				return std::nullopt;
			for (const auto& [key, value] : observation.payload)
				if (!append_framed_size(key.size(), maximum, total) ||
					!append_framed_size(value.size(), maximum, total))
					return std::nullopt;

			std::size_t span{};
			if (!observation.primary_span)
			{
				if (!append_framed_size("absent"sv.size(), maximum, span))
					return std::nullopt;
			}
			else
			{
				const auto& value = *observation.primary_span;
				if (!append_framed_size("present"sv.size(), maximum, span) ||
					!append_framed_size(value.span_id.size(), maximum, span) ||
					!append_framed_size(value.snapshot.size(), maximum, span) ||
					!append_framed_size(value.file.size(), maximum, span) ||
					!append_framed_size(decimal_digits(value.begin), maximum, span) ||
					!append_framed_size(decimal_digits(value.end), maximum, span) ||
					!append_framed_size(value.role.size(), maximum, span) ||
					!append_framed_size(1U, maximum, span))
					return std::nullopt;
			}
			if (!append_framed_size(span, maximum, total))
				return std::nullopt;

			std::size_t origins{};
			if (!append_framed_size(decimal_digits(observation.origins.size()), maximum, origins))
				return std::nullopt;
			for (const auto& origin : observation.origins)
				if (!append_framed_size(origin.kind.size(), maximum, origins) ||
					!append_framed_size(origin.logical_path.size(), maximum, origins) ||
					!append_framed_size(decimal_digits(static_cast<std::uint64_t>(origin.begin)),
										maximum,
										origins) ||
					!append_framed_size(
						decimal_digits(static_cast<std::uint64_t>(origin.end)), maximum, origins) ||
					!append_framed_size(1U, maximum, origins))
					return std::nullopt;
			if (!append_framed_size(origins, maximum, total) ||
				!append_framed_size(observation.exact_equivalence ? "exact"sv.size()
																  : "limited"sv.size(),
									maximum,
									total) ||
				!append_framed_size(
					observation.limitation ? observation.limitation->size() : 0U, maximum, total))
				return std::nullopt;
			return total;
		}

		[[nodiscard]] std::optional<std::size_t>
		row_logical_reservation(const provider_worker_v4_ast_observation& observation,
								const std::size_t maximum) noexcept
		{
			// Detached-row JSON may escape one input byte to six bytes; byte-valued cells use two.
			// Eight therefore bounds all variable projections.  The fixed allowance covers the
			// descriptor/column/type names, the outer origin-chain tuple, and canonical JSON
			// framing for every observation-v2 cell.  Each origin also contributes at most 113
			// canonical- binary framing bytes excluding kind/path text (five tuple items, two
			// strings, two signed 64-bit integers, and one boolean); hex JSON doubles that.  The
			// 256-byte charge therefore bounds both the intermediate canonical binary and its
			// detached-row spelling before either is allocated.
			constexpr std::size_t fixed_row_bytes{8192U};
			constexpr std::size_t variable_expansion{8U};
			constexpr std::size_t origin_framing_bytes{256U};
			std::size_t variable{};
			auto add = [&](const std::size_t value) noexcept
			{
				return checked_add(variable, value, maximum, variable);
			};
			if (!add(observation.compile_unit.size()) || !add(observation.semantic_key.size()))
				return std::nullopt;
			for (const auto& [key, value] : observation.payload)
				if (!add(key.size()) || !add(value.size()))
					return std::nullopt;
			if (observation.primary_span)
			{
				const auto& span = *observation.primary_span;
				if (!add(span.span_id.size()) || !add(span.snapshot.size()) ||
					!add(span.file.size()) || !add(span.role.size()))
					return std::nullopt;
			}
			for (const auto& origin : observation.origins)
				if (!add(origin.kind.size()) || !add(origin.logical_path.size()))
					return std::nullopt;
			if (observation.limitation && !add(observation.limitation->size()))
				return std::nullopt;
			std::size_t expanded{};
			std::size_t origin_framing{};
			std::size_t output{};
			if (!checked_multiply(variable, variable_expansion, maximum, expanded) ||
				!checked_multiply(
					observation.origins.size(), origin_framing_bytes, maximum, origin_framing) ||
				!checked_add(fixed_row_bytes, expanded, maximum, output) ||
				!checked_add(output, origin_framing, maximum, output))
				return std::nullopt;
			return output;
		}

		class observer_budget final
		{
		  public:
			explicit observer_budget(provider_worker_v4_ast_observer_limits limits) noexcept
				: limits_{limits}
			{
			}

			[[nodiscard]] const provider_worker_v4_ast_observer_limits& limits() const noexcept
			{
				return limits_;
			}

			[[nodiscard]] sdk::result<void>
			preflight_observations(const std::size_t count,
								   const std::size_t compile_unit_bytes) const
			{
				std::size_t next_count{};
				std::size_t copied_bytes{};
				std::size_t next_bytes{};
				if (!checked_add(observations_, count, limits_.maximum_observations, next_count))
					return sdk::unexpected(resource_failure("observations"));
				if (!checked_multiply(
						compile_unit_bytes, count, limits_.maximum_logical_bytes, copied_bytes) ||
					!checked_add(
						logical_bytes_, copied_bytes, limits_.maximum_logical_bytes, next_bytes))
					return sdk::unexpected(resource_failure("bytes", "observation-compile-unit"));
				return {};
			}

			[[nodiscard]] sdk::result<void>
			reserve_observation(const std::size_t compile_unit_bytes)
			{
				std::size_t next_count{};
				std::size_t next_bytes{};
				if (!checked_add(observations_, 1U, limits_.maximum_observations, next_count))
					return sdk::unexpected(resource_failure("observations"));
				if (!checked_add(logical_bytes_,
								 compile_unit_bytes,
								 limits_.maximum_logical_bytes,
								 next_bytes))
					return sdk::unexpected(resource_failure("bytes", "observation-compile-unit"));
				observations_ = next_count;
				logical_bytes_ = next_bytes;
				return {};
			}

			[[nodiscard]] sdk::result<void> reserve_rows(const std::size_t count)
			{
				std::size_t next{};
				if (!checked_add(rows_, count, limits_.maximum_rows, next))
					return sdk::unexpected(resource_failure("rows"));
				rows_ = next;
				return {};
			}

			[[nodiscard]] sdk::result<void> reserve_diagnostic(const std::size_t bytes)
			{
				std::size_t next_count{};
				std::size_t next_bytes{};
				if (!checked_add(diagnostics_, 1U, limits_.maximum_diagnostics, next_count))
					return sdk::unexpected(resource_failure("diagnostics"));
				if (!checked_add(logical_bytes_, bytes, limits_.maximum_logical_bytes, next_bytes))
					return sdk::unexpected(resource_failure("bytes", "diagnostic"));
				diagnostics_ = next_count;
				logical_bytes_ = next_bytes;
				return {};
			}

			[[nodiscard]] sdk::result<void> reserve_origin(std::size_t& observation_origins,
														   const std::size_t bytes)
			{
				std::size_t next_total{};
				std::size_t next_observation{};
				std::size_t next_bytes{};
				if (!checked_add(origins_, 1U, limits_.maximum_origins, next_total))
					return sdk::unexpected(resource_failure("origins", "aggregate"));
				if (!checked_add(observation_origins,
								 1U,
								 limits_.maximum_origins_per_observation,
								 next_observation))
					return sdk::unexpected(resource_failure("origins", "per-observation"));
				if (!checked_add(logical_bytes_, bytes, limits_.maximum_logical_bytes, next_bytes))
					return sdk::unexpected(resource_failure("bytes", "origin"));
				origins_ = next_total;
				observation_origins = next_observation;
				logical_bytes_ = next_bytes;
				return {};
			}

			[[nodiscard]] sdk::result<void> reserve_bytes(const std::size_t bytes,
														  const std::string_view detail)
			{
				std::size_t next{};
				if (!checked_add(logical_bytes_, bytes, limits_.maximum_logical_bytes, next))
					return sdk::unexpected(resource_failure("bytes", std::string{detail}));
				logical_bytes_ = next;
				return {};
			}

			[[nodiscard]] sdk::result<void> preflight_bytes(const std::size_t bytes,
															const std::string_view detail) const
			{
				std::size_t next{};
				if (!checked_add(logical_bytes_, bytes, limits_.maximum_logical_bytes, next))
					return sdk::unexpected(resource_failure("bytes", std::string{detail}));
				return {};
			}

			[[nodiscard]] sdk::result<void> enter_depth()
			{
				std::size_t next_entries{};
				std::size_t next_depth{};
				if (!checked_add(
						traversal_entries_, 1U, limits_.maximum_traversal_entries, next_entries))
					return sdk::unexpected(resource_failure("traversal-count"));
				if (!checked_add(depth_, 1U, limits_.maximum_traversal_depth, next_depth))
					return sdk::unexpected(resource_failure("depth"));
				traversal_entries_ = next_entries;
				depth_ = next_depth;
				return {};
			}

			void leave_depth() noexcept
			{
				if (depth_ != 0U)
					--depth_;
			}

		  private:
			[[nodiscard]] static sdk::error
			resource_failure(std::string field, std::string detail = "maximum-or-overflow")
			{
				return failure(
					"provider-worker-v4.ast-resource-limit", std::move(field), std::move(detail));
			}

			provider_worker_v4_ast_observer_limits limits_;
			std::size_t observations_{};
			std::size_t rows_{};
			std::size_t diagnostics_{};
			std::size_t origins_{};
			std::size_t logical_bytes_{};
			std::size_t traversal_entries_{};
			std::size_t depth_{};
		};

		constexpr std::size_t maximum_clang_text_bytes{64U * 1024U};
		constexpr std::size_t maximum_derived_identity_bytes{128U};

		[[maybe_unused]] [[nodiscard]] sdk::result<void>
		preflight_derived_identity(observer_budget& budget,
								   const std::size_t projection_bytes,
								   const std::string_view detail)
		{
			std::size_t scratch{};
			if (!checked_add(projection_bytes,
							 maximum_derived_identity_bytes,
							 budget.limits().maximum_logical_bytes,
							 scratch))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-resource-limit", "bytes", std::string{detail}));
			return budget.preflight_bytes(scratch, detail);
		}

		class fixed_text_buffer final
		{
		  public:
			[[nodiscard]] bool append(const std::string_view value) noexcept
			{
				const auto available = storage_.size() - used_;
				const auto copied = std::min(available, value.size());
				if (copied != 0U)
					std::memcpy(storage_.data() + used_, value.data(), copied);
				used_ += copied;
				if (copied != value.size())
					overflowed_ = true;
				return !overflowed_;
			}

			[[nodiscard]] bool append_decimal(const std::uint64_t value) noexcept
			{
				std::array<char, 32U> digits{};
				const auto converted =
					std::to_chars(digits.data(), digits.data() + digits.size(), value);
				return converted.ec == std::errc{} &&
					append(std::string_view{
						digits.data(), static_cast<std::size_t>(converted.ptr - digits.data())});
			}

			[[nodiscard]] bool append_framed(const std::string_view value) noexcept
			{
				return append_decimal(value.size()) && append(":"sv) && append(value);
			}

			[[nodiscard]] std::string_view view() const noexcept
			{
				return {storage_.data(), used_};
			}

			[[nodiscard]] bool overflowed() const noexcept
			{
				return overflowed_;
			}

		  private:
			std::array<char, maximum_clang_text_bytes> storage_{};
			std::size_t used_{};
			bool overflowed_{};
		};

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		class fixed_raw_ostream final : public llvm::raw_ostream
		{
		  public:
			explicit fixed_raw_ostream(fixed_text_buffer& output) noexcept : output_{&output}
			{
				SetUnbuffered();
			}

		  private:
			void write_impl(const char* pointer, const std::size_t size) override
			{
				(void)output_->append(std::string_view{pointer, size});
			}

			[[nodiscard]] std::uint64_t current_pos() const override
			{
				return output_->view().size();
			}

			fixed_text_buffer* output_;
		};

		class counting_raw_ostream final : public llvm::raw_ostream
		{
		  public:
			explicit counting_raw_ostream(const std::size_t maximum) noexcept : maximum_{maximum}
			{
				SetUnbuffered();
			}

			[[nodiscard]] std::size_t size() const noexcept
			{
				return size_;
			}

			[[nodiscard]] bool overflowed() const noexcept
			{
				return overflowed_;
			}

		  private:
			void write_impl(const char*, const std::size_t size) override
			{
				if (overflowed_ || size > maximum_ - std::min(size_, maximum_))
				{
					overflowed_ = true;
					return;
				}
				size_ += size;
			}

			[[nodiscard]] std::uint64_t current_pos() const override
			{
				return size_;
			}

			std::size_t maximum_;
			std::size_t size_{};
			bool overflowed_{};
		};

		class preallocated_raw_ostream final : public llvm::raw_ostream
		{
		  public:
			explicit preallocated_raw_ostream(std::string& output) noexcept : output_{&output}
			{
				SetUnbuffered();
			}

			[[nodiscard]] bool complete() const noexcept
			{
				return !overflowed_ && size_ == output_->size();
			}

		  private:
			void write_impl(const char* pointer, const std::size_t size) override
			{
				const auto available = output_->size() - std::min(size_, output_->size());
				if (size > available)
				{
					overflowed_ = true;
					return;
				}
				if (size != 0U)
					std::memcpy(output_->data() + size_, pointer, size);
				size_ += size;
			}

			[[nodiscard]] std::uint64_t current_pos() const override
			{
				return size_;
			}

			std::string* output_;
			std::size_t size_{};
			bool overflowed_{};
		};

		template <class Printer>
		[[nodiscard]] sdk::result<std::string> bounded_clang_text(observer_budget& budget,
																  Printer&& printer,
																  const std::string_view detail)
		{
			counting_raw_ostream count{budget.limits().maximum_logical_bytes};
			printer(count);
			count.flush();
			if (count.overflowed())
				return sdk::unexpected(
					failure("provider-worker-v4.ast-resource-limit", "bytes", std::string{detail}));
			if (auto preflight = budget.preflight_bytes(count.size(), detail); !preflight)
				return sdk::unexpected(std::move(preflight.error()));
			std::string output(count.size(), '\0');
			preallocated_raw_ostream stream{output};
			printer(stream);
			stream.flush();
			if (!stream.complete())
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-bound-invalid", "bytes", "clang-text-size-changed"));
			return output;
		}

		[[nodiscard]] sdk::result<std::string>
		bounded_qualified_name(observer_budget& budget, const clang::NamedDecl& declaration)
		{
			return bounded_clang_text(
				budget,
				[&](llvm::raw_ostream& output)
				{
					declaration.printQualifiedName(output);
				},
				"clang-qualified-name");
		}

		[[nodiscard]] sdk::result<std::string> bounded_canonical_type(observer_budget& budget,
																	  const clang::QualType type)
		{
			return bounded_clang_text(
				budget,
				[&](llvm::raw_ostream& output)
				{
					const clang::PrintingPolicy policy{clang::LangOptions{}};
					type.getCanonicalType().print(output, policy);
				},
				"clang-canonical-type");
		}

		class semantic_digest_stream final
		{
		  public:
			semantic_digest_stream(const std::string_view domain,
								   const std::size_t payload_size) noexcept
				: remaining_payload_{payload_size}
			{
				constexpr auto marker = "cxxlens-semantic-digest-v2"sv;
				update_header_byte(0x05U);
				update_header_u64(3U);
				update_header_u64(9U + marker.size());
				update_header_byte(0x04U);
				update_header_u64(marker.size());
				update_header(marker);
				update_header_u64(9U + domain.size());
				update_header_byte(0x04U);
				update_header_u64(domain.size());
				update_header(domain);
				update_header_u64(9U + payload_size);
				update_header_byte(0x03U);
				update_header_u64(payload_size);
			}

			semantic_digest_stream(const semantic_digest_stream&) = delete;
			semantic_digest_stream& operator=(const semantic_digest_stream&) = delete;

			[[nodiscard]] bool update(const std::string_view value) noexcept
			{
				if (value.size() > remaining_payload_)
					return false;
				hash_.update(llvm::StringRef{value.data(), value.size()});
				remaining_payload_ -= value.size();
				return true;
			}

			[[nodiscard]] sdk::result<std::string> finish()
			{
				if (remaining_payload_ != 0U)
					return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
												   "fallback-identity",
												   "projection-size-changed"));
				const auto bytes = hash_.final();
				constexpr auto digits = "0123456789abcdef"sv;
				std::string output{"semantic-v2:sha256:"};
				output.reserve(output.size() + bytes.size() * 2U);
				for (const auto byte : bytes)
				{
					output.push_back(digits[(byte >> 4U) & 0x0fU]);
					output.push_back(digits[byte & 0x0fU]);
				}
				return output;
			}

		  private:
			void update_header(const std::string_view value) noexcept
			{
				hash_.update(llvm::StringRef{value.data(), value.size()});
			}

			void update_header_byte(const std::uint8_t value) noexcept
			{
				hash_.update(llvm::ArrayRef<std::uint8_t>{&value, 1U});
			}

			void update_header_u64(const std::uint64_t value) noexcept
			{
				std::array<std::uint8_t, 8U> bytes{};
				for (std::size_t index{}; index < bytes.size(); ++index)
					bytes[index] = static_cast<std::uint8_t>(value >> ((7U - index) * 8U));
				hash_.update(bytes);
			}

			llvm::SHA256 hash_;
			std::size_t remaining_payload_;
		};

		class digest_raw_ostream final : public llvm::raw_ostream
		{
		  public:
			digest_raw_ostream(semantic_digest_stream& output,
							   const std::size_t expected_size) noexcept
				: output_{&output}, remaining_{expected_size}
			{
				SetUnbuffered();
			}

			[[nodiscard]] bool complete() const noexcept
			{
				return !overflowed_ && remaining_ == 0U;
			}

		  private:
			void write_impl(const char* pointer, const std::size_t size) override
			{
				if (overflowed_ || size > remaining_ ||
					!output_->update(std::string_view{pointer, size}))
				{
					overflowed_ = true;
					return;
				}
				remaining_ -= size;
				position_ += size;
			}

			[[nodiscard]] std::uint64_t current_pos() const override
			{
				return position_;
			}

			semantic_digest_stream* output_;
			std::size_t remaining_;
			std::size_t position_{};
			bool overflowed_{};
		};
#endif

		void append_text(std::ostringstream& output, const std::string_view value)
		{
			output << value.size() << ':' << value;
		}

		[[nodiscard]] std::string
		span_canonical(const std::optional<materialization::observation_v2_primary_span>& span)
		{
			std::ostringstream output;
			if (!span)
			{
				append_text(output, "absent");
				return output.str();
			}
			append_text(output, "present");
			append_text(output, span->span_id);
			append_text(output, span->snapshot);
			append_text(output, span->file);
			append_text(output, std::to_string(span->begin));
			append_text(output, std::to_string(span->end));
			append_text(output, span->role);
			append_text(output, span->read_only ? "1" : "0");
			return output.str();
		}

		[[nodiscard]] std::string
		origins_canonical(const std::vector<materialization::observation_v2_origin>& origins)
		{
			std::ostringstream output;
			append_text(output, std::to_string(origins.size()));
			for (const auto& origin : origins)
			{
				append_text(output, origin.kind);
				append_text(output, origin.logical_path);
				append_text(output, std::to_string(origin.begin));
				append_text(output, std::to_string(origin.end));
				append_text(output, origin.read_only ? "1" : "0");
			}
			return output.str();
		}

		[[nodiscard]] sdk::result<void>
		validate_span(const materialization::observation_v2_primary_span& span)
		{
			if (!valid_text(span.span_id) || !valid_text(span.snapshot) || !valid_text(span.file) ||
				!valid_text(span.role) || span.end < span.begin)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "primary-span"));
			auto expected = sdk::source_span_identity(
				span.snapshot, span.file, span.begin, span.end, span.role);
			if (!expected || *expected != span.span_id)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "primary-span-identity"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_origin(const materialization::observation_v2_origin& origin)
		{
			if (!valid_text(origin.kind) || !valid_text(origin.logical_path) || origin.begin < 0 ||
				origin.end < origin.begin || !origin.read_only)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "origin"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_task_metadata(const source_closure_task_v4_decoded& metadata,
							   const std::string_view compile_unit,
							   const source_closure_member*& main)
		{
			if (!valid_text(compile_unit))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "compile-unit"));
			if (auto valid = metadata.input.closure.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto identity = derive_source_closure_task_v4_identity(metadata.input);
			if (!identity || *identity != metadata.identity)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "task-identity"));
			if (!valid_text(metadata.identity.task_id) ||
				!valid_text(metadata.identity.task_v4_digest))
				return sdk::unexpected(failure("provider-worker-v4.ast-input-invalid", "identity"));
			main = metadata.input.closure.find_member(metadata.input.main_logical_path);
			if (main == nullptr || main->role != source_closure_role::main ||
				main->logical_path != metadata.input.main_logical_path)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-input-invalid", "main-member"));
			return {};
		}

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
		[[nodiscard]] sdk::result<std::optional<std::string>>
		source_anchor(provider::clang22::borrowed_translation_unit& unit,
					  observer_budget& budget,
					  const clang::SourceLocation location,
					  const std::string_view source_snapshot,
					  const std::string_view file)
		{
			auto& source = unit.source_manager();
			const auto spelling = source.getSpellingLoc(location);
			if (spelling.isInvalid() || !source.isWrittenInMainFile(spelling))
				return std::optional<std::string>{};
			bool invalid{};
			const auto buffer = source.getBufferData(source.getFileID(spelling), &invalid);
			if (invalid)
				return std::optional<std::string>{};
			if (auto preflight =
					budget.preflight_bytes(maximum_derived_identity_bytes, "source-anchor-content");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			const auto content =
				sdk::content_digest(std::as_bytes(std::span{buffer.data(), buffer.size()}));
			fixed_text_buffer projection;
			if (!projection.append_framed(source_snapshot) || !projection.append_framed(file) ||
				!projection.append_framed(content))
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "source-anchor-projection"));
			std::array<char, 32U> offset{};
			const auto converted = std::to_chars(
				offset.data(), offset.data() + offset.size(), source.getFileOffset(spelling));
			if (converted.ec != std::errc{} ||
				!projection.append_framed(std::string_view{
					offset.data(), static_cast<std::size_t>(converted.ptr - offset.data())}))
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "source-anchor-projection"));
			if (auto preflight = preflight_derived_identity(
					budget, projection.view().size(), "source-anchor-identity");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			auto anchor =
				sdk::semantic_digest("clang22.declaration-source-anchor.v1", projection.view());
			return anchor ? std::optional<std::string>{std::move(*anchor)}
						  : std::optional<std::string>{};
		}

		[[nodiscard]] std::string_view declaration_kind(const clang::FunctionDecl& declaration)
		{
			if (llvm::isa<clang::CXXConstructorDecl>(declaration))
				return "constructor";
			if (llvm::isa<clang::CXXDestructorDecl>(declaration))
				return "destructor";
			if (llvm::isa<clang::CXXMethodDecl>(declaration))
				return "method";
			return "function";
		}

		[[nodiscard]] sdk::result<std::string>
		declaration_context_identity(provider::clang22::borrowed_translation_unit& unit,
									 observer_budget& budget,
									 const clang::DeclContext& context,
									 const std::string_view source_snapshot,
									 const std::string_view file)
		{
			fixed_text_buffer output;
			const auto* declaration = clang::Decl::castFromDeclContext(&context);
			if (declaration == nullptr)
				return std::string{};
			if (!output.append_framed(declaration->getDeclKindName()))
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "declaration-context"));
			if (const auto* named = llvm::dyn_cast<clang::NamedDecl>(declaration))
			{
				const auto* canonical = llvm::cast<clang::NamedDecl>(named->getCanonicalDecl());
				auto qualified = bounded_qualified_name(budget, *canonical);
				if (!qualified)
					return sdk::unexpected(std::move(qualified.error()));
				if (!output.append_framed(*qualified))
					return sdk::unexpected(failure(
						"provider-worker-v4.ast-resource-limit", "bytes", "declaration-context"));
				auto anchor =
					source_anchor(unit, budget, canonical->getLocation(), source_snapshot, file);
				if (!anchor)
					return sdk::unexpected(std::move(anchor.error()));
				if (*anchor && !output.append_framed(**anchor))
					return sdk::unexpected(failure(
						"provider-worker-v4.ast-resource-limit", "bytes", "declaration-context"));
			}
			if (auto preflight =
					budget.preflight_bytes(output.view().size(), "declaration-context");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			return std::string{output.view()};
		}

		void print_template_identity(llvm::raw_ostream& output,
									 const clang::FunctionDecl& declaration)
		{
			output << static_cast<unsigned>(declaration.getTemplatedKind());
			const auto* arguments = declaration.getTemplateSpecializationArgs();
			if (arguments == nullptr)
				return;
			clang::PrintingPolicy policy{declaration.getASTContext().getLangOpts()};
			for (const auto& argument : arguments->asArray())
			{
				output << ':';
				argument.print(policy, output, true);
			}
		}

		[[nodiscard]] sdk::result<std::size_t>
		template_identity_size(observer_budget& budget, const clang::FunctionDecl& declaration)
		{
			counting_raw_ostream output{budget.limits().maximum_logical_bytes};
			print_template_identity(output, declaration);
			output.flush();
			if (output.overflowed())
				return sdk::unexpected(
					failure("provider-worker-v4.ast-resource-limit", "bytes", "template-identity"));
			if (auto preflight = budget.preflight_bytes(output.size(), "template-identity");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			return output.size();
		}

		[[nodiscard]] bool append_framed(semantic_digest_stream& output,
										 const std::string_view value) noexcept
		{
			std::array<char, 32U> size{};
			const auto converted =
				std::to_chars(size.data(), size.data() + size.size(), value.size());
			return converted.ec == std::errc{} &&
				output.update(std::string_view{
					size.data(), static_cast<std::size_t>(converted.ptr - size.data())}) &&
				output.update(":"sv) && output.update(value);
		}

		[[nodiscard]] bool append_framed_prefix(semantic_digest_stream& output,
												const std::size_t value_size) noexcept
		{
			std::array<char, 32U> size{};
			const auto converted =
				std::to_chars(size.data(), size.data() + size.size(), value_size);
			return converted.ec == std::errc{} &&
				output.update(std::string_view{
					size.data(), static_cast<std::size_t>(converted.ptr - size.data())}) &&
				output.update(":"sv);
		}

		[[nodiscard]] bool add_framed_size(const std::size_t value_size,
										   const std::size_t maximum,
										   std::size_t& aggregate) noexcept
		{
			std::size_t framing{};
			std::size_t next{};
			return checked_add(decimal_digits(value_size), 1U, maximum, framing) &&
				checked_add(framing, value_size, maximum, framing) &&
				checked_add(aggregate, framing, maximum, next) && ((aggregate = next), true);
		}

		class usr_structural_profiler final
			: public clang::RecursiveASTVisitor<usr_structural_profiler>
		{
			using base = clang::RecursiveASTVisitor<usr_structural_profiler>;

		  public:
			usr_structural_profiler(fixed_text_buffer& output,
									observer_budget& budget,
									const clang::ASTContext& context)
				: output_{&output}, stream_{output}, context_{&context}, budget_{&budget},
				  policy_{context.getLangOpts()}
			{
				policy_.SuppressTemplateArgsInCXXConstructors = true;
			}

			[[nodiscard]] bool profile(const clang::FunctionDecl& declaration)
			{
				return profile_function(declaration) && !output_->overflowed();
			}

			[[nodiscard]] std::optional<sdk::error> take_failure()
			{
				return std::move(failure_);
			}

			bool TraverseType(clang::QualType type, const bool traverse_qualifier = true)
			{
				if (type.isNull())
					return true;
				return with_budget(
					[&]()
					{
						if (!marker())
							return false;
						if (const auto* tag = type->getAs<clang::TagType>())
							if (!profile_tag(*tag->getDecl()))
								return false;
						if (const auto* dependent = type->getAs<clang::DependentNameType>())
						{
							dependent->getQualifier().print(stream_, policy_);
							if (!append(dependent->getIdentifier()->getName()))
								return false;
						}
						if (const auto* array = context_->getAsConstantArrayType(type))
							if (!markers(static_cast<std::size_t>(array->getSize().getBitWidth()) +
										 1U))
								return false;
						return base::TraverseType(type, traverse_qualifier);
					});
			}

			bool TraverseTemplateName(const clang::TemplateName name)
			{
				return with_budget(
					[&]()
					{
						if (!marker())
							return false;
						if (const auto* declaration =
								name.getAsTemplateDecl(/*IgnoreDeduced=*/true))
							if (!profile_named(*declaration))
								return false;
						return base::TraverseTemplateName(name);
					});
			}

			bool TraverseTemplateArgument(const clang::TemplateArgument& argument)
			{
				return with_budget(
					[&]()
					{
						if (!marker())
							return false;
						switch (argument.getKind())
						{
							case clang::TemplateArgument::Declaration:
								return argument.getAsDecl() == nullptr ||
									profile_named(*argument.getAsDecl());
							case clang::TemplateArgument::Integral:
								return markers(static_cast<std::size_t>(
												   argument.getAsIntegral().getBitWidth()) +
											   1U) &&
									TraverseType(argument.getIntegralType());
							case clang::TemplateArgument::StructuralValue:
								return markers(32U) &&
									TraverseType(argument.getStructuralValueType());
							case clang::TemplateArgument::NullPtr:
								return TraverseType(argument.getNullPtrType());
							case clang::TemplateArgument::Expression:
								// Clang 22 emits an ODR hash, never expression prose.
								return markers(32U);
							default:
								return base::TraverseTemplateArgument(argument);
						}
					});
			}

			bool TraverseStmt(clang::Stmt* statement)
			{
				// USRGeneration encodes expression template arguments with a fixed-size ODR hash.
				return statement == nullptr ||
					with_budget(
						   [&]()
						   {
							   return markers(32U);
						   });
			}

		  private:
			template <class Operation>
			[[nodiscard]] bool with_budget(Operation&& operation)
			{
				auto entered = budget_->enter_depth();
				if (!entered)
				{
					failure_ = std::move(entered.error());
					return false;
				}
				const auto result = operation();
				budget_->leave_depth();
				return result;
			}

			class active_declaration final
			{
			  public:
				active_declaration(usr_structural_profiler& owner, const clang::Decl& declaration)
					: owner_{&owner}, entered_{owner.enter(declaration)}
				{
				}

				~active_declaration()
				{
					if (entered_)
						owner_->leave();
				}

				[[nodiscard]] bool entered() const noexcept
				{
					return entered_;
				}

			  private:
				usr_structural_profiler* owner_;
				bool entered_;
			};

			[[nodiscard]] bool enter(const clang::Decl& declaration)
			{
				for (std::size_t index = 0U; index < active_size_; ++index)
					if (active_[index] == &declaration)
						return false;
				if (active_size_ == active_.size())
					return false;
				active_[active_size_++] = &declaration;
				return true;
			}

			void leave() noexcept
			{
				if (active_size_ != 0U)
					--active_size_;
			}

			[[nodiscard]] bool marker()
			{
				return output_->append("x"sv);
			}

			[[nodiscard]] bool markers(std::size_t count)
			{
				constexpr auto values =
					"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"sv;
				if (count > maximum_clang_text_bytes)
					count = maximum_clang_text_bytes + 1U;
				while (count != 0U)
				{
					const auto next = std::min(count, values.size());
					if (!output_->append(values.substr(0U, next)))
						return false;
					count -= next;
				}
				return true;
			}

			[[nodiscard]] bool append(const llvm::StringRef value)
			{
				return output_->append(std::string_view{value.data(), value.size()});
			}

			[[nodiscard]] bool profile_location(const clang::Decl& declaration,
												const bool include_offset)
			{
				if (!marker())
					return false;
				const auto& source = context_->getSourceManager();
				const auto location = source.getExpansionLoc(declaration.getBeginLoc());
				if (location.isInvalid())
					return true;
				const auto decomposed = source.getDecomposedLoc(location);
				const auto entry = source.getFileEntryRefForID(decomposed.first);
				if (entry && !append(llvm::sys::path::filename(entry->getName())))
					return false;
				if (!include_offset)
					return true;
				if (!markers(decimal_digits(source.getFileOffset(location))))
					return false;
				const auto spelling = source.getSpellingLoc(declaration.getBeginLoc());
				return spelling.isInvalid() || spelling == location ||
					markers(decimal_digits(source.getFileOffset(spelling)));
			}

			[[nodiscard]] bool should_profile_location(const clang::NamedDecl& declaration) const
			{
				if (declaration.isExternallyVisible())
					return false;
				if (declaration.getParentFunctionOrMethod() != nullptr)
					return true;
				const auto location = declaration.getLocation();
				return location.isValid() &&
					!context_->getSourceManager().isInSystemHeader(location);
			}

			[[nodiscard]] bool
			profile_template_parameters(const clang::TemplateParameterList* parameters)
			{
				return parameters == nullptr ||
					with_budget(
						   [&]()
						   {
							   return profile_template_parameters_impl(*parameters);
						   });
			}

			[[nodiscard]] bool
			profile_template_parameters_impl(const clang::TemplateParameterList& parameters)
			{
				if (!markers(decimal_digits(parameters.size()) + 1U))
					return false;
				for (const auto* parameter : parameters.asArray())
				{
					if (!with_budget(
							[&]()
							{
								if (!marker())
									return false;
								if (const auto* non_type =
										llvm::dyn_cast<clang::NonTypeTemplateParmDecl>(parameter))
									return TraverseType(non_type->getType());
								if (const auto* nested =
										llvm::dyn_cast<clang::TemplateTemplateParmDecl>(parameter))
									return profile_template_parameters(
										nested->getTemplateParameters());
								return true;
							}))
						return false;
				}
				return true;
			}

			[[nodiscard]] bool profile_context(const clang::DeclContext* context)
			{
				return with_budget(
					[&]()
					{
						return profile_context_impl(context);
					});
			}

			[[nodiscard]] bool profile_context_impl(const clang::DeclContext* context)
			{
				if (context == nullptr || context->isTranslationUnit())
					return true;
				if (const auto* linkage = llvm::dyn_cast<clang::LinkageSpecDecl>(context))
					return marker() && profile_context(linkage->getParent());
				const auto* declaration = clang::Decl::castFromDeclContext(context);
				const auto* named = llvm::dyn_cast_or_null<clang::NamedDecl>(declaration);
				return named == nullptr ? marker() : profile_named(*named);
			}

			[[nodiscard]] bool profile_tag(const clang::TagDecl& original)
			{
				return with_budget(
					[&]()
					{
						return profile_tag_impl(original);
					});
			}

			[[nodiscard]] bool profile_tag_impl(const clang::TagDecl& original)
			{
				const auto& declaration = *original.getCanonicalDecl();
				active_declaration active{*this, declaration};
				if (!active.entered())
					return false;
				if (!marker() || !profile_context(declaration.getDeclContext()))
					return false;
				if (!llvm::isa<clang::EnumDecl>(declaration) &&
					should_profile_location(declaration) &&
					!profile_location(declaration,
									  declaration.getParentFunctionOrMethod() != nullptr))
					return false;
				if (const auto* external = declaration.getExternalSourceSymbolAttr())
					if (!append(external->getDefinedIn()))
						return false;
				if (!append(declaration.getName()))
					return false;
				if (const auto* alias = declaration.getTypedefNameForAnonDecl())
				{
					if (!append(alias->getName()))
						return false;
				}
				else if (declaration.getName().empty() && declaration.isEmbeddedInDeclarator() &&
						 !declaration.isFreeStanding())
				{
					if (!profile_location(declaration, true))
						return false;
				}
				else if (const auto* enumeration = llvm::dyn_cast<clang::EnumDecl>(&declaration))
				{
					if (declaration.getName().empty() && !enumeration->enumerators().empty() &&
						!append((*enumeration->enumerators().begin())->getName()))
						return false;
				}

				const auto* record = llvm::dyn_cast<clang::CXXRecordDecl>(&declaration);
				if (record == nullptr)
					return true;
				if (const auto* described = record->getDescribedClassTemplate())
					if (!profile_template_parameters(described->getTemplateParameters()))
						return false;
				if (const auto* partial =
						llvm::dyn_cast<clang::ClassTemplatePartialSpecializationDecl>(record))
					if (!profile_template_parameters(partial->getTemplateParameters()))
						return false;
				if (const auto* specialization =
						llvm::dyn_cast<clang::ClassTemplateSpecializationDecl>(record))
					for (const auto& argument : specialization->getTemplateArgs().asArray())
						if (!TraverseTemplateArgument(argument))
							return false;
				return true;
			}

			[[nodiscard]] bool profile_function(const clang::FunctionDecl& original)
			{
				return with_budget(
					[&]()
					{
						return profile_function_impl(original);
					});
			}

			[[nodiscard]] bool profile_function_impl(const clang::FunctionDecl& original)
			{
				const auto& declaration = *original.getCanonicalDecl();
				active_declaration active{*this, declaration};
				if (!active.entered())
					return false;
				if (!marker() || !profile_context(declaration.getDeclContext()) ||
					(should_profile_location(declaration) &&
					 !profile_location(declaration,
									   declaration.getParentFunctionOrMethod() != nullptr)))
					return false;
				if (const auto* external = declaration.getExternalSourceSymbolAttr())
					if (!append(external->getDefinedIn()))
						return false;
				declaration.getDeclName().print(stream_, policy_);
				if (output_->overflowed())
					return false;
				if (const auto* described = declaration.getDescribedFunctionTemplate())
					if (!profile_template_parameters(described->getTemplateParameters()))
						return false;
				if (const auto* arguments = declaration.getTemplateSpecializationArgs())
				{
					for (const auto& argument : arguments->asArray())
						if (!TraverseTemplateArgument(argument))
							return false;
				}
				else if (const auto* written = declaration.getTemplateSpecializationArgsAsWritten())
				{
					for (const auto& argument : written->arguments())
						if (!TraverseTemplateArgument(argument.getArgument()))
							return false;
				}
				const auto canonical_type = declaration.getType().getCanonicalType();
				if (const auto* prototype = canonical_type->getAs<clang::FunctionProtoType>())
					for (const auto parameter : prototype->param_types())
						if (!TraverseType(parameter))
							return false;
				if (declaration.getDescribedFunctionTemplate() != nullptr &&
					!TraverseType(declaration.getReturnType()))
					return false;
				return true;
			}

			[[nodiscard]] bool profile_named(const clang::NamedDecl& declaration)
			{
				return with_budget(
					[&]()
					{
						return profile_named_impl(declaration);
					});
			}

			[[nodiscard]] bool profile_named_impl(const clang::NamedDecl& declaration)
			{
				if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(&declaration))
					return profile_function(*function);
				if (const auto* function_template =
						llvm::dyn_cast<clang::FunctionTemplateDecl>(&declaration))
					return profile_function(*function_template->getTemplatedDecl());
				if (const auto* tag = llvm::dyn_cast<clang::TagDecl>(&declaration))
					return profile_tag(*tag);
				if (const auto* class_template =
						llvm::dyn_cast<clang::ClassTemplateDecl>(&declaration))
					return profile_tag(*class_template->getTemplatedDecl());

				active_declaration active{*this, declaration};
				if (!active.entered())
					return false;
				const auto location_free_kind =
					llvm::isa<clang::NamespaceDecl, clang::NamespaceAliasDecl, clang::FieldDecl>(
						declaration);
				const auto template_parameter_kind =
					llvm::isa<clang::TemplateTypeParmDecl,
							  clang::NonTypeTemplateParmDecl,
							  clang::TemplateTemplateParmDecl>(declaration);
				if (!marker() || !profile_context(declaration.getDeclContext()) ||
					(template_parameter_kind && !profile_location(declaration, true)) ||
					(!location_free_kind && !template_parameter_kind &&
					 should_profile_location(declaration) &&
					 !profile_location(declaration,
									   declaration.getParentFunctionOrMethod() != nullptr)))
					return false;
				if (const auto* external = declaration.getExternalSourceSymbolAttr())
					if (!append(external->getDefinedIn()))
						return false;
				if (const auto* unresolved_value =
						llvm::dyn_cast<clang::UnresolvedUsingValueDecl>(&declaration))
					unresolved_value->getQualifier().print(stream_, policy_);
				if (const auto* unresolved_type =
						llvm::dyn_cast<clang::UnresolvedUsingTypenameDecl>(&declaration))
					unresolved_type->getQualifier().print(stream_, policy_);
				declaration.getDeclName().print(stream_, policy_);
				if (const auto* variable_template =
						llvm::dyn_cast<clang::VarTemplateDecl>(&declaration))
					if (!profile_template_parameters(variable_template->getTemplateParameters()))
						return false;
				if (const auto* partial =
						llvm::dyn_cast<clang::VarTemplatePartialSpecializationDecl>(&declaration))
					if (!profile_template_parameters(partial->getTemplateParameters()))
						return false;
				if (const auto* specialization =
						llvm::dyn_cast<clang::VarTemplateSpecializationDecl>(&declaration))
					for (const auto& argument : specialization->getTemplateArgs().asArray())
						if (!TraverseTemplateArgument(argument))
							return false;
				return !output_->overflowed();
			}

			fixed_text_buffer* output_;
			fixed_raw_ostream stream_;
			const clang::ASTContext* context_;
			observer_budget* budget_;
			clang::PrintingPolicy policy_;
			std::array<const clang::Decl*, provider_worker_v4_ast_product_maximum_traversal_depth>
				active_{};
			std::size_t active_size_{};
			std::optional<sdk::error> failure_;
		};

		[[nodiscard]] sdk::result<std::size_t>
		bounded_usr_structural_profile(observer_budget& budget,
									   const clang::FunctionDecl& declaration)
		{
			fixed_text_buffer output;
			usr_structural_profiler profiler{output, budget, declaration.getASTContext()};
			if (!profiler.profile(declaration) || output.overflowed())
			{
				if (auto traversal = profiler.take_failure())
					return sdk::unexpected(std::move(*traversal));
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "clang-usr-structure"));
			}
			if (auto preflight =
					budget.preflight_bytes(output.view().size(), "clang-usr-structure");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			return output.view().size();
		}

		struct bounded_usr_inputs final
		{
			std::size_t maximum_encoded_bytes{};
		};

		[[nodiscard]] sdk::result<bounded_usr_inputs>
		preflight_usr_generation(observer_budget& budget, const clang::FunctionDecl& declaration)
		{
			bounded_usr_inputs output;
			auto structural_profile = bounded_usr_structural_profile(budget, declaration);
			if (!structural_profile)
				return sdk::unexpected(std::move(structural_profile.error()));

			// The structural profiler walks exactly the declaration/context/type/template inputs
			// consumed by Clang's USR generator.  It retains declaration and dependent names,
			// locations and integer widths, while representing defaults, constraints, expressions,
			// and structural values by fixed markers because Clang does not emit their prose.
			// Thirty-two encoded bytes per structural byte plus fixed headroom bounds exact 22.1
			// without permitting the SmallString below to leave its inline storage.
			constexpr std::size_t fixed_usr_bytes{8192U};
			constexpr std::size_t maximum_usr_bytes_per_profile_byte{32U};
			std::size_t expanded_profile{};
			if (!checked_multiply(*structural_profile,
								  maximum_usr_bytes_per_profile_byte,
								  maximum_clang_text_bytes,
								  expanded_profile) ||
				!checked_add(fixed_usr_bytes,
							 expanded_profile,
							 maximum_clang_text_bytes,
							 output.maximum_encoded_bytes))
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "clang-usr-envelope"));
			if (auto preflight =
					budget.preflight_bytes(output.maximum_encoded_bytes, "clang-usr-envelope");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			return output;
		}

		[[nodiscard]] sdk::result<std::string>
		constraint_identity(provider::clang22::borrowed_translation_unit& unit,
							observer_budget& budget,
							const clang::FunctionDecl& declaration)
		{
			const auto& requires_clause = declaration.getTrailingRequiresClause();
			const auto* constraint = requires_clause.ConstraintExpr;
			if (constraint == nullptr)
				return std::string{};
			const auto source = clang::Lexer::getSourceText(
				clang::CharSourceRange::getTokenRange(constraint->getSourceRange()),
				unit.source_manager(),
				unit.ast().getLangOpts());
			if (source.size() > maximum_clang_text_bytes)
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "constraint-identity"));
			if (auto preflight = budget.preflight_bytes(source.size(), "constraint-identity");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			return source.str();
		}

		[[nodiscard]] sdk::result<std::pair<std::string, std::string>>
		declaration_identity_for(provider::clang22::borrowed_translation_unit& unit,
								 observer_budget& budget,
								 const clang::FunctionDecl& declaration,
								 const std::string_view toolchain_digest,
								 const std::string_view source_snapshot,
								 const std::string_view file)
		{
			const auto* canonical = declaration.getCanonicalDecl();
			const auto* anchor_declaration = canonical;
			if (!unit.source_manager().isWrittenInMainFile(canonical->getLocation()))
				if (const auto* definition = declaration.getDefinition(); definition != nullptr &&
					unit.source_manager().isWrittenInMainFile(definition->getLocation()))
					anchor_declaration = definition;
			if (const auto* external = canonical->getAttr<clang::ExternalSourceSymbolAttr>();
				external != nullptr && !external->getUSR().empty())
			{
				const auto usr = external->getUSR();
				std::size_t identity_size{};
				if (usr.size() > maximum_clang_text_bytes ||
					!checked_add("clang-usr:"sv.size(),
								 usr.size(),
								 budget.limits().maximum_logical_bytes,
								 identity_size))
					return sdk::unexpected(
						failure("provider-worker-v4.ast-resource-limit", "bytes", "clang-usr"));
				if (auto preflight = budget.preflight_bytes(identity_size, "clang-usr"); !preflight)
					return sdk::unexpected(std::move(preflight.error()));
				std::string identity{"clang-usr:"};
				identity.append(usr.data(), usr.size());
				return std::pair<std::string, std::string>{std::move(identity), "exact-usr"};
			}
			auto usr_inputs = preflight_usr_generation(budget, *canonical);
			if (!usr_inputs)
				return sdk::unexpected(std::move(usr_inputs.error()));
			llvm::SmallString<maximum_clang_text_bytes> storage;
			if (!clang::index::generateUSRForDecl(canonical, storage) && !storage.empty())
			{
				std::size_t identity_size{};
				if (storage.size() > usr_inputs->maximum_encoded_bytes)
					return sdk::unexpected(failure(
						"provider-worker-v4.ast-bound-invalid", "bytes", "clang-usr-envelope"));
				if (storage.size() > maximum_clang_text_bytes ||
					!checked_add("clang-usr:"sv.size(),
								 storage.size(),
								 budget.limits().maximum_logical_bytes,
								 identity_size))
					return sdk::unexpected(
						failure("provider-worker-v4.ast-resource-limit", "bytes", "clang-usr"));
				if (auto preflight = budget.preflight_bytes(identity_size, "clang-usr"); !preflight)
					return sdk::unexpected(std::move(preflight.error()));
				std::string identity{"clang-usr:"};
				identity.append(storage.data(), storage.size());
				return std::pair<std::string, std::string>{std::move(identity), "exact-usr"};
			}
			auto anchor = source_anchor(
				unit, budget, anchor_declaration->getLocation(), source_snapshot, file);
			if (!anchor)
				return sdk::unexpected(std::move(anchor.error()));
			if (toolchain_digest.empty() || !*anchor)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-identity-unresolved", "fallback"));
			auto qualified_name = bounded_qualified_name(budget, *canonical);
			if (!qualified_name)
				return sdk::unexpected(std::move(qualified_name.error()));
			auto canonical_type = bounded_canonical_type(budget, canonical->getType());
			if (!canonical_type)
				return sdk::unexpected(std::move(canonical_type.error()));
			auto template_value_size = template_identity_size(budget, *canonical);
			if (!template_value_size)
				return sdk::unexpected(std::move(template_value_size.error()));
			auto constraint = constraint_identity(unit, budget, *canonical);
			if (!constraint)
				return sdk::unexpected(std::move(constraint.error()));
			auto context = declaration_context_identity(
				unit, budget, *canonical->getDeclContext(), source_snapshot, file);
			if (!context)
				return sdk::unexpected(std::move(context.error()));
			constexpr auto fallback_domain = "clang22.declaration-fallback.v2"sv;
			std::size_t projection_size{};
			for (const auto text : {fallback_domain,
									toolchain_digest,
									declaration_kind(*canonical),
									std::string_view{*qualified_name},
									std::string_view{*canonical_type}})
				if (!add_framed_size(
						text.size(), budget.limits().maximum_logical_bytes, projection_size))
					return sdk::unexpected(failure(
						"provider-worker-v4.ast-resource-limit", "bytes", "fallback-projection"));
			if (!add_framed_size(
					*template_value_size, budget.limits().maximum_logical_bytes, projection_size))
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-resource-limit", "bytes", "fallback-projection"));
			for (const auto text : {std::string_view{*constraint},
									std::string_view{*context},
									std::string_view{**anchor}})
				if (!add_framed_size(
						text.size(), budget.limits().maximum_logical_bytes, projection_size))
					return sdk::unexpected(failure(
						"provider-worker-v4.ast-resource-limit", "bytes", "fallback-projection"));
			if (auto preflight =
					preflight_derived_identity(budget, projection_size, "fallback-identity");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));

			semantic_digest_stream projection{fallback_domain, projection_size};
			for (const auto text : {fallback_domain,
									toolchain_digest,
									declaration_kind(*canonical),
									std::string_view{*qualified_name},
									std::string_view{*canonical_type}})
				if (!append_framed(projection, text))
					return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
												   "fallback-identity",
												   "projection-size-changed"));
			if (!append_framed_prefix(projection, *template_value_size))
				return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
											   "fallback-identity",
											   "projection-size-changed"));
			digest_raw_ostream template_stream{projection, *template_value_size};
			print_template_identity(template_stream, *canonical);
			template_stream.flush();
			if (!template_stream.complete())
				return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
											   "fallback-identity",
											   "template-identity-size-changed"));
			for (const auto text : {std::string_view{*constraint},
									std::string_view{*context},
									std::string_view{**anchor}})
				if (!append_framed(projection, text))
					return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
												   "fallback-identity",
												   "projection-size-changed"));
			auto digest = projection.finish();
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			std::size_t identity_size{};
			if (!checked_add("clang-fallback:"sv.size(),
							 digest->size(),
							 budget.limits().maximum_logical_bytes,
							 identity_size))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-resource-limit", "bytes", "fallback-identity"));
			if (auto preflight = budget.preflight_bytes(identity_size, "fallback-identity");
				!preflight)
				return sdk::unexpected(std::move(preflight.error()));
			std::string identity{"clang-fallback:"};
			identity.append(*digest);
			return std::pair<std::string, std::string>{std::move(identity), "structural-fallback"};
		}

		[[nodiscard]] std::string_view call_kind(const clang::CallExpr& expression)
		{
			const auto* direct = expression.getDirectCallee();
			if (direct == nullptr)
			{
				if (expression.isTypeDependent() || expression.isValueDependent())
					return "dependent";
				if (llvm::isa<clang::CXXMemberCallExpr>(expression))
					return "indirect_member_pointer";
				return "indirect_function";
			}
			const auto* method = llvm::dyn_cast<clang::CXXMethodDecl>(direct);
			if (method == nullptr)
				return direct->isOverloadedOperator() ? "operator" : "direct_function";
			const auto* member =
				llvm::dyn_cast<clang::MemberExpr>(expression.getCallee()->IgnoreParenImpCasts());
			if (method->isVirtual() && (member == nullptr || !member->hasQualifier()))
				return "virtual_member";
			return direct->isOverloadedOperator() ? "operator" : "direct_member";
		}

		class visitor final : public clang::RecursiveASTVisitor<visitor>
		{
			using base = clang::RecursiveASTVisitor<visitor>;

			struct source_attachment
			{
				materialization::observation_v2_primary_span primary_span;
				std::vector<materialization::observation_v2_origin> origins;
			};

		  public:
			visitor(provider::clang22::borrowed_translation_unit& unit,
					provider_worker_v4_ast_observation_batch& output,
					observer_budget& budget,
					std::string_view source_snapshot,
					std::string_view source_file,
					std::string_view toolchain_digest)
				: unit_{&unit}, output_{&output}, budget_{&budget},
				  source_snapshot_{source_snapshot}, source_file_{source_file},
				  toolchain_digest_{toolchain_digest}
			{
			}

			bool TraverseDecl(clang::Decl* declaration)
			{
				if (declaration == nullptr)
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseDecl(declaration);
					});
			}

			bool TraverseType(clang::QualType type, const bool traverse_qualifier = true)
			{
				if (type.isNull())
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseType(type, traverse_qualifier);
					});
			}

			bool TraverseTypeLoc(clang::TypeLoc type, const bool traverse_qualifier = true)
			{
				if (type.isNull())
					return true;
				return with_depth(
					[&]()
					{
						return base::TraverseTypeLoc(type, traverse_qualifier);
					});
			}

			bool TraverseNestedNameSpecifier(clang::NestedNameSpecifier value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseNestedNameSpecifier(value);
					});
			}

			bool TraverseNestedNameSpecifierLoc(clang::NestedNameSpecifierLoc value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseNestedNameSpecifierLoc(value);
					});
			}

			bool TraverseDeclarationNameInfo(clang::DeclarationNameInfo value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseDeclarationNameInfo(value);
					});
			}

			bool TraverseTemplateName(clang::TemplateName value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseTemplateName(value);
					});
			}

			bool TraverseTemplateArgument(const clang::TemplateArgument& value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseTemplateArgument(value);
					});
			}

			bool TraverseTemplateArgumentLoc(const clang::TemplateArgumentLoc& value)
			{
				return with_depth(
					[&]()
					{
						return base::TraverseTemplateArgumentLoc(value);
					});
			}

			bool dataTraverseStmtPre(clang::Stmt* statement)
			{
				if (statement == nullptr || failure_)
					return statement == nullptr;
				auto entered = budget_->enter_depth();
				if (!entered)
				{
					set_failure(std::move(entered.error()));
					return false;
				}
				return true;
			}

			bool dataTraverseStmtPost(clang::Stmt* statement)
			{
				if (statement != nullptr)
					budget_->leave_depth();
				return !failure_;
			}

			bool TraverseFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr)
					return true;
				if (!declaration->isImplicit() &&
					written_in_main_file(declaration->getLocation()) &&
					!accept(budget_->preflight_observations(2U, output_->compile_unit.size())))
					return false;
				auto previous = std::move(current_function_);
				auto identity = declaration_identity_for(*unit_,
														 *budget_,
														 *declaration,
														 toolchain_digest_,
														 source_snapshot_,
														 source_file_);
				if (identity)
				{
					if (!accept(
							budget_->preflight_bytes(identity->first.size(), "current-function")))
					{
						current_function_ = std::move(previous);
						return false;
					}
					current_function_ = identity->first;
				}
				else if (identity.error().code == "provider-worker-v4.ast-resource-limit")
				{
					set_failure(std::move(identity.error()));
					current_function_ = std::move(previous);
					return false;
				}
				else
					current_function_.clear();
				const auto traversed = base::TraverseFunctionDecl(declaration);
				current_function_ = std::move(previous);
				return traversed;
			}

			bool VisitFunctionDecl(clang::FunctionDecl* declaration)
			{
				if (declaration == nullptr || declaration->isImplicit() ||
					!written_in_main_file(declaration->getLocation()))
					return true;
				provider_worker_v4_ast_observation entity;
				entity.kind = provider_worker_v4_ast_observation_kind::entity;
				provider_worker_v4_ast_observation type;
				type.kind = provider_worker_v4_ast_observation_kind::type;
				if (!begin_observation(entity) || !begin_observation(type))
					return false;
				auto identity = declaration_identity_for(*unit_,
														 *budget_,
														 *declaration,
														 toolchain_digest_,
														 source_snapshot_,
														 source_file_);
				if (!identity)
				{
					if (identity.error().code == "provider-worker-v4.ast-resource-limit")
					{
						set_failure(std::move(identity.error()));
						return false;
					}
					return record_diagnostic(identity.error().code);
				}
				auto qualified_name = bounded_qualified_name(*budget_, *declaration);
				if (!qualified_name)
				{
					set_failure(std::move(qualified_name.error()));
					return false;
				}
				auto signature = bounded_canonical_type(*budget_, declaration->getType());
				if (!signature)
				{
					set_failure(std::move(signature.error()));
					return false;
				}

				const bool exact_identity = identity->second == "exact-usr";
				if (!set_semantic_key(entity, std::move(identity->first)) ||
					!put_payload_preflighted(
						entity, "symbol.identity_confidence", std::move(identity->second)) ||
					!put_payload(entity, "symbol.kind", declaration_kind(*declaration)) ||
					!put_payload_preflighted(
						entity, "symbol.qualified_name", std::move(*qualified_name)) ||
					!put_payload_preflighted(entity, "symbol.signature", std::move(*signature)) ||
					!put_payload(entity,
								 "symbol.is_definition",
								 declaration->isThisDeclarationADefinition() ? "true"sv
																			 : "false"sv) ||
					!put_payload(entity,
								 "symbol.is_canonical_declaration",
								 declaration == declaration->getCanonicalDecl() ? "true"sv
																				: "false"sv))
					return false;
				entity.exact_equivalence = exact_identity;
				if (!entity.exact_equivalence &&
					!set_limitation(entity, "identity-confidence:structural-fallback"))
					return false;
				if (!attach_source(entity, declaration->getSourceRange(), "declaration") ||
					!insert(std::move(entity)))
					return false;

				auto canonical_type = bounded_canonical_type(*budget_, declaration->getType());
				if (!canonical_type)
				{
					set_failure(std::move(canonical_type.error()));
					return false;
				}
				if (!accept(preflight_derived_identity(
						*budget_, canonical_type->size(), "type-semantic-identity")))
					return false;
				auto type_identity = sdk::semantic_digest("clang22.type.v1", *canonical_type);
				if (!type_identity)
				{
					set_failure(std::move(type_identity.error()));
					return false;
				}
				if (!put_payload_preflighted(type, "type.canonical", std::move(*canonical_type)) ||
					!set_semantic_key(type, std::move(*type_identity)) || !insert(std::move(type)))
					return false;
				return true;
			}

			bool VisitCallExpr(clang::CallExpr* expression)
			{
				if (expression == nullptr || !written_in_main_file(expression->getExprLoc()))
					return true;
				provider_worker_v4_ast_observation call;
				call.kind = provider_worker_v4_ast_observation_kind::call;
				if (!begin_observation(call) ||
					!put_payload(call, "call.kind", call_kind(*expression)))
					return false;
				if (!current_function_.empty() &&
					!put_payload(call, "call.caller", std::string_view{current_function_}))
					return false;
				if (const auto* callee = expression->getDirectCallee(); callee != nullptr)
				{
					const auto* declaration = callee->getDefinition();
					if (declaration == nullptr)
						declaration = callee->getCanonicalDecl();
					auto identity = declaration_identity_for(*unit_,
															 *budget_,
															 *declaration,
															 toolchain_digest_,
															 source_snapshot_,
															 source_file_);
					if (identity)
					{
						const bool exact_identity = identity->second == "exact-usr";
						if (!put_payload_preflighted(
								call, "call.direct_callee", std::move(identity->first)) ||
							!put_payload_preflighted(call,
													 "call.direct_callee_identity_confidence",
													 std::move(identity->second)))
							return false;
						call.exact_equivalence = exact_identity;
						if (!call.exact_equivalence &&
							!set_limitation(call, "identity-confidence:structural-fallback"))
							return false;
					}
					else if (identity.error().code == "provider-worker-v4.ast-resource-limit")
					{
						set_failure(std::move(identity.error()));
						return false;
					}
					else if (!record_diagnostic(identity.error().code) ||
							 !put_payload(
								 call, "call.unresolved_reason", "callee-identity-unavailable"sv))
						return false;
					auto signature = bounded_canonical_type(*budget_, declaration->getType());
					if (!signature)
					{
						set_failure(std::move(signature.error()));
						return false;
					}
					auto qualified_name = bounded_qualified_name(*budget_, *declaration);
					if (!qualified_name)
					{
						set_failure(std::move(qualified_name.error()));
						return false;
					}
					if (!put_payload(
							call, "call.direct_callee_kind", declaration_kind(*declaration)) ||
						!put_payload_preflighted(
							call, "call.direct_callee_signature", std::move(*signature)) ||
						!put_payload_preflighted(
							call, "call.direct_callee_qualified_name", std::move(*qualified_name)))
						return false;
				}
				else if (call.payload.at("call.kind") == "dependent")
				{
					if (!put_payload(call, "call.unresolved_reason", "dependent-callee"sv))
						return false;
				}
				else if (call.payload.at("call.kind") == "indirect_member_pointer")
				{
					if (!put_payload(
							call, "call.unresolved_reason", "member-pointer-target-not-modeled"sv))
						return false;
				}
				else if (!put_payload(call,
									  "call.unresolved_reason",
									  "function-pointer-target-not-modeled"sv))
					return false;

				if (!attach_source(call, expression->getSourceRange(), "expression"))
					return false;
				fixed_text_buffer identity_projection;
				if (!identity_projection.append_framed(current_function_))
				{
					set_failure(failure("provider-worker-v4.ast-resource-limit",
										"bytes",
										"call-identity-projection"));
					return false;
				}
				if (call.primary_span)
				{
					if (!identity_projection.append_framed(call.primary_span->span_id) ||
						!identity_projection.append_framed(
							call.payload.contains("call.direct_callee")
								? std::string_view{call.payload.at("call.direct_callee")}
								: std::string_view{}))
					{
						set_failure(failure("provider-worker-v4.ast-resource-limit",
											"bytes",
											"call-identity-projection"));
						return false;
					}
				}
				else
				{
					if (unavailable_call_index_ == std::numeric_limits<std::uint64_t>::max())
					{
						set_failure(failure("provider-worker-v4.ast-resource-limit",
											"observations",
											"call-index-overflow"));
						return false;
					}
					std::array<char, 32U> index{};
					const auto converted = std::to_chars(
						index.data(), index.data() + index.size(), unavailable_call_index_++);
					if (converted.ec != std::errc{} ||
						!identity_projection.append_framed(std::string_view{
							index.data(), static_cast<std::size_t>(converted.ptr - index.data())}))
					{
						set_failure(failure("provider-worker-v4.ast-resource-limit",
											"bytes",
											"call-identity-projection"));
						return false;
					}
					for (const auto& [key, value] : call.payload)
					{
						if (!identity_projection.append_framed(key) ||
							!identity_projection.append_framed(value))
						{
							set_failure(failure("provider-worker-v4.ast-resource-limit",
												"bytes",
												"call-identity-projection"));
							return false;
						}
					}
				}
				if (!accept(preflight_derived_identity(
						*budget_, identity_projection.view().size(), "call-semantic-identity")))
					return false;
				auto semantic_key = sdk::semantic_digest(
					call.primary_span ? "clang22.call.v1" : "clang22.call-source-unavailable.v1",
					identity_projection.view());
				if (!semantic_key)
				{
					set_failure(std::move(semantic_key.error()));
					return false;
				}
				return set_semantic_key(call, std::move(*semantic_key)) && insert(std::move(call));
			}

			[[nodiscard]] const std::optional<sdk::error>& error() const noexcept
			{
				return failure_;
			}

			[[nodiscard]] sdk::result<void> release_observations()
			{
				if (failure_)
					return sdk::unexpected(*failure_);
				output_->observations.reserve(observations_.size());
				for (auto& [key, observation] : observations_)
				{
					(void)key;
					output_->observations.push_back(std::move(observation));
				}
				return {};
			}

		  private:
			[[nodiscard]] bool written_in_main_file(const clang::SourceLocation location) const
			{
				return location.isValid() && unit_->source_manager().isWrittenInMainFile(location);
			}

			template <class Function>
			bool with_depth(Function&& function)
			{
				if (failure_)
					return false;
				auto entered = budget_->enter_depth();
				if (!entered)
				{
					set_failure(std::move(entered.error()));
					return false;
				}
				const bool traversed = function();
				budget_->leave_depth();
				return traversed;
			}

			void set_failure(sdk::error value)
			{
				if (!failure_)
					failure_.emplace(std::move(value));
			}

			[[nodiscard]] bool accept(sdk::result<void> value)
			{
				if (value)
					return true;
				set_failure(std::move(value.error()));
				return false;
			}

			[[nodiscard]] bool begin_observation(provider_worker_v4_ast_observation& observation)
			{
				if (!accept(budget_->reserve_observation(output_->compile_unit.size())))
					return false;
				observation.compile_unit = output_->compile_unit;
				return true;
			}

			[[nodiscard]] bool set_semantic_key(provider_worker_v4_ast_observation& observation,
												std::string&& value)
			{
				if (!accept(budget_->reserve_bytes(value.size(), "semantic-key")))
					return false;
				observation.semantic_key = std::move(value);
				return true;
			}

			[[nodiscard]] bool set_limitation(provider_worker_v4_ast_observation& observation,
											  const std::string_view value)
			{
				if (!accept(budget_->reserve_bytes(value.size(), "limitation")))
					return false;
				observation.limitation.emplace(value);
				return true;
			}

			[[nodiscard]] bool set_limitation(provider_worker_v4_ast_observation& observation,
											  const std::string_view prefix,
											  const std::string_view suffix)
			{
				std::size_t size{};
				if (!checked_add(prefix.size(),
								 suffix.size(),
								 budget_->limits().maximum_logical_bytes,
								 size) ||
					!accept(budget_->reserve_bytes(size, "limitation")))
					return false;
				std::string value;
				value.reserve(size);
				value.append(prefix);
				value.append(suffix);
				observation.limitation.emplace(std::move(value));
				return true;
			}

			[[nodiscard]] bool put_payload(provider_worker_v4_ast_observation& observation,
										   const std::string_view key,
										   const std::string_view value)
			{
				if (!accept(budget_->reserve_bytes(key.size(), "payload-key")) ||
					!accept(budget_->reserve_bytes(value.size(), "payload-value")))
					return false;
				observation.payload.emplace(std::string{key}, std::string{value});
				return true;
			}

			// The rvalue must have been produced by one of the preflighted bounded text/identity
			// helpers above.  This overload performs the independent retained-output charge before
			// moving it into the observation.
			[[nodiscard]] bool
			put_payload_preflighted(provider_worker_v4_ast_observation& observation,
									const std::string_view key,
									std::string&& value)
			{
				if (!accept(budget_->reserve_bytes(key.size(), "payload-key")) ||
					!accept(budget_->reserve_bytes(value.size(), "payload-value")))
					return false;
				observation.payload.emplace(std::string{key}, std::move(value));
				return true;
			}

			[[nodiscard]] bool record_diagnostic(const std::string_view code)
			{
				if (output_->failed_count == std::numeric_limits<std::uint64_t>::max())
				{
					set_failure(failure("provider-worker-v4.ast-resource-limit",
										"diagnostics",
										"failed-count-overflow"));
					return false;
				}
				if (!accept(budget_->reserve_diagnostic(code.size())))
					return false;
				++output_->failed_count;
				output_->diagnostics.emplace_back(code);
				return true;
			}

			[[nodiscard]] sdk::result<source_attachment>
			bounded_source(const clang::SourceRange& range, const std::string_view role)
			{
				auto& source_manager = unit_->source_manager();
				const auto expansion = source_manager.getExpansionRange(range);
				const auto begin_location = source_manager.getExpansionLoc(expansion.getBegin());
				auto end_location = source_manager.getExpansionLoc(expansion.getEnd());
				if (expansion.isTokenRange())
					end_location = clang::Lexer::getLocForEndOfToken(
						end_location, 0U, source_manager, unit_->ast().getLangOpts());
				if (begin_location.isInvalid() || end_location.isInvalid() ||
					!source_manager.isWrittenInSameFile(begin_location, end_location))
					return sdk::unexpected(failure("native.source-span-invalid", "range"));
				const auto filename = source_manager.getFilename(begin_location);
				if (filename.empty())
					return sdk::unexpected(failure("native.source-span-invalid", "file"));
				const auto begin = source_manager.getFileOffset(begin_location);
				const auto end = source_manager.getFileOffset(end_location);
				if (end < begin)
					return sdk::unexpected(failure("native.source-span-invalid", "offset"));
				std::size_t identity_projection_bytes{256U};
				for (const auto text : {source_snapshot_, source_file_, role})
					if (!checked_add(identity_projection_bytes,
									 text.size(),
									 budget_->limits().maximum_logical_bytes,
									 identity_projection_bytes))
						return sdk::unexpected(failure("provider-worker-v4.ast-resource-limit",
													   "bytes",
													   "source-span-identity"));
				if (auto preflight =
						budget_->preflight_bytes(identity_projection_bytes, "source-span-identity");
					!preflight)
					return sdk::unexpected(std::move(preflight.error()));
				auto id =
					sdk::source_span_identity(source_snapshot_, source_file_, begin, end, role);
				if (!id)
					return sdk::unexpected(std::move(id.error()));

				for (const auto text :
					 {std::string_view{*id}, source_snapshot_, source_file_, role})
					if (auto reserved = budget_->reserve_bytes(text.size(), "primary-span");
						!reserved)
						return sdk::unexpected(std::move(reserved.error()));

				source_attachment output{
					{std::move(*id),
					 std::string{source_snapshot_},
					 std::string{source_file_},
					 begin,
					 end,
					 std::string{role},
					 range.getBegin().isMacroID() || range.getEnd().isMacroID()},
					{},
				};
				std::size_t observation_origins{};
				auto append_origin = [&](const std::string_view kind,
										 const llvm::StringRef logical_path,
										 const std::uint64_t origin_begin,
										 const std::uint64_t origin_end) -> sdk::result<void>
				{
					if (origin_begin >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
						origin_end >
							static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
						return sdk::unexpected(failure("native.source-origin-invalid", "offset"));
					std::size_t bytes{};
					if (!checked_add(kind.size(),
									 logical_path.size(),
									 budget_->limits().maximum_logical_bytes,
									 bytes))
						return sdk::unexpected(failure("provider-worker-v4.ast-resource-limit",
													   "bytes",
													   "origin-size-overflow"));
					if (auto reserved = budget_->reserve_origin(observation_origins, bytes);
						!reserved)
						return reserved;
					output.origins.push_back({std::string{kind},
											  logical_path.str(),
											  static_cast<std::int64_t>(origin_begin),
											  static_cast<std::int64_t>(origin_end),
											  true});
					return {};
				};

				auto origin_begin = range.getBegin();
				auto origin_end = range.getEnd();
				for (std::size_t depth = 0U;
					 depth < 128U && (origin_begin.isMacroID() || origin_end.isMacroID());
					 ++depth)
				{
					const auto spelling_begin = source_manager.getSpellingLoc(origin_begin);
					const auto spelling_end_token = source_manager.getSpellingLoc(origin_end);
					const auto spelling_end = clang::Lexer::getLocForEndOfToken(
						spelling_end_token, 0U, source_manager, unit_->ast().getLangOpts());
					if (spelling_begin.isInvalid() || spelling_end.isInvalid())
						return sdk::unexpected(failure("native.source-origin-invalid", "range"));
					if (source_manager.isWrittenInSameFile(spelling_begin, spelling_end))
					{
						const auto origin_filename = source_manager.getFilename(spelling_begin);
						if (origin_filename.empty())
							return sdk::unexpected(failure("native.source-origin-invalid", "file"));
						if (auto appended =
								append_origin("macro-spelling",
											  origin_filename,
											  source_manager.getFileOffset(spelling_begin),
											  source_manager.getFileOffset(spelling_end));
							!appended)
							return sdk::unexpected(std::move(appended.error()));
					}
					else
					{
						const auto begin_filename = source_manager.getFilename(spelling_begin);
						const auto end_filename = source_manager.getFilename(spelling_end_token);
						const auto begin_token_end = clang::Lexer::getLocForEndOfToken(
							spelling_begin, 0U, source_manager, unit_->ast().getLangOpts());
						if (begin_filename.empty() || end_filename.empty() ||
							begin_token_end.isInvalid())
							return sdk::unexpected(failure("native.source-origin-invalid", "file"));
						if (auto appended =
								append_origin("macro-spelling-begin",
											  begin_filename,
											  source_manager.getFileOffset(spelling_begin),
											  source_manager.getFileOffset(begin_token_end));
							!appended)
							return sdk::unexpected(std::move(appended.error()));
						if (auto appended =
								append_origin("macro-spelling-end",
											  end_filename,
											  source_manager.getFileOffset(spelling_end_token),
											  source_manager.getFileOffset(spelling_end));
							!appended)
							return sdk::unexpected(std::move(appended.error()));
					}
					const auto next_begin = origin_begin.isMacroID()
						? source_manager.getImmediateExpansionRange(origin_begin).getBegin()
						: origin_begin;
					const auto next_end = origin_end.isMacroID()
						? source_manager.getImmediateExpansionRange(origin_end).getEnd()
						: origin_end;
					if (next_begin == origin_begin && next_end == origin_end)
						break;
					origin_begin = next_begin;
					origin_end = next_end;
				}
				if (origin_begin.isMacroID() || origin_end.isMacroID())
					return sdk::unexpected(failure("native.source-origin-invalid", "depth"));
				return output;
			}

			[[nodiscard]] bool attach_source(provider_worker_v4_ast_observation& observation,
											 const clang::SourceRange& range,
											 const std::string_view role)
			{
				auto source = bounded_source(range, role);
				if (!source)
				{
					if (source.error().code == "provider-worker-v4.ast-resource-limit")
					{
						set_failure(std::move(source.error()));
						return false;
					}
					if (!record_diagnostic(source.error().code))
						return false;
					observation.exact_equivalence = false;
					return set_limitation(
						observation, "source-span-unavailable:", source.error().code);
				}
				observation.primary_span.emplace(std::move(source->primary_span));
				observation.origins = std::move(source->origins);
				return true;
			}

			[[nodiscard]] bool insert(provider_worker_v4_ast_observation observation)
			{
				const auto expected_size =
					canonical_size(observation, budget_->limits().maximum_logical_bytes);
				if (!expected_size)
				{
					set_failure(failure("provider-worker-v4.ast-resource-limit",
										"bytes",
										"observation-canonical-key-overflow"));
					return false;
				}
				if (!accept(budget_->reserve_bytes(*expected_size, "observation-canonical-key")))
					return false;
				auto key = observation.canonical_form();
				if (key.size() != *expected_size)
				{
					set_failure(failure("provider-worker-v4.ast-bound-invalid",
										"canonical-key",
										"size-estimator-mismatch"));
					return false;
				}
				observations_.try_emplace(std::move(key), std::move(observation));
				return true;
			}

			provider::clang22::borrowed_translation_unit* unit_;
			provider_worker_v4_ast_observation_batch* output_;
			observer_budget* budget_;
			std::string_view source_snapshot_;
			std::string_view source_file_;
			std::string_view toolchain_digest_;
			std::string current_function_;
			std::map<std::string, provider_worker_v4_ast_observation, std::less<>> observations_;
			std::optional<sdk::error> failure_;
			std::uint64_t unavailable_call_index_{};
		};
#endif
	} // namespace

	sdk::result<void> provider_worker_v4_ast_observer_limits::validate() const
	{
		for (const auto [value, maximum, field] : {
				 std::tuple{maximum_observations,
							provider_worker_v4_ast_product_maximum_observations,
							std::string_view{"observations"}},
				 std::tuple{maximum_rows,
							provider_worker_v4_ast_product_maximum_rows,
							std::string_view{"rows"}},
				 std::tuple{maximum_diagnostics,
							provider_worker_v4_ast_product_maximum_diagnostics,
							std::string_view{"diagnostics"}},
				 std::tuple{maximum_origins,
							provider_worker_v4_ast_product_maximum_origins,
							std::string_view{"origins"}},
				 std::tuple{maximum_origins_per_observation,
							provider_worker_v4_ast_product_maximum_origins_per_observation,
							std::string_view{"origins-per-observation"}},
				 std::tuple{maximum_logical_bytes,
							provider_worker_v4_ast_product_maximum_logical_bytes,
							std::string_view{"bytes"}},
				 std::tuple{maximum_traversal_entries,
							provider_worker_v4_ast_product_maximum_traversal_entries,
							std::string_view{"traversal-count"}},
				 std::tuple{maximum_traversal_depth,
							provider_worker_v4_ast_product_maximum_traversal_depth,
							std::string_view{"depth"}},
			 })
		{
			if (value == 0U)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-limit-invalid", std::string{field}, "nonzero"));
			if (value > maximum)
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-limit-invalid", std::string{field}, "product-maximum"));
		}
		if (maximum_origins_per_observation > maximum_origins)
			return sdk::unexpected(failure("provider-worker-v4.ast-limit-invalid",
										   "origins-per-observation",
										   "aggregate-maximum"));
		return {};
	}

	sdk::result<void> provider_worker_v4_ast_observation::validate() const
	{
		if (!valid_text(compile_unit) || !valid_text(semantic_key))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "identity"));
		if (kind == provider_worker_v4_ast_observation_kind::type &&
			(primary_span || !origins.empty()))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "type-source-authority"));
		if (primary_span)
		{
			if (auto valid = validate_span(*primary_span); !valid)
				return valid;
		}
		for (const auto& origin : origins)
			if (auto valid = validate_origin(origin); !valid)
				return valid;
		if (limitation && (!valid_text(*limitation) || exact_equivalence))
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "limitation"));
		if (!limitation && !exact_equivalence)
			return sdk::unexpected(
				failure("provider-worker-v4.ast-observation-invalid", "exact-equivalence"));
		for (const auto& [key, value] : payload)
			if (!valid_text(key) || value.find('\0') != std::string::npos ||
				!sdk::validate_utf8_text(value))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-observation-invalid", "payload"));
		return {};
	}

	std::string provider_worker_v4_ast_observation::canonical_form() const
	{
		std::ostringstream output;
		append_text(output, "cxxlens.clang22.task-v4.ast-observation.v1");
		append_text(output, std::to_string(static_cast<unsigned>(kind)));
		append_text(output, compile_unit);
		append_text(output, semantic_key);
		append_text(output, std::to_string(payload.size()));
		for (const auto& [key, value] : payload)
		{
			append_text(output, key);
			append_text(output, value);
		}
		append_text(output, span_canonical(primary_span));
		append_text(output, origins_canonical(origins));
		append_text(output, exact_equivalence ? "exact" : "limited");
		append_text(output, limitation ? std::string_view{*limitation} : std::string_view{});
		return output.str();
	}

	sdk::result<void> provider_worker_v4_ast_observation_batch::validate() const
	{
		if (!valid_text(task_id) || !valid_text(task_v4_digest) || !valid_text(compile_unit) ||
			!valid_text(source_snapshot) || !valid_text(source_file))
			return sdk::unexpected(failure("provider-worker-v4.ast-batch-invalid", "identity"));
		if (rows.size() != observations.size())
			return sdk::unexpected(
				failure("provider-worker-v4.ast-batch-invalid", "rows", "observation-cardinality"));
		std::string previous;
		for (std::size_t index{}; index < observations.size(); ++index)
		{
			const auto& observation = observations[index];
			if (auto valid = observation.validate(); !valid)
				return valid;
			if (observation.compile_unit != compile_unit)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "compile-unit"));
			const auto expected_descriptor = [&]() -> std::string_view
			{
				switch (observation.kind)
				{
					case provider_worker_v4_ast_observation_kind::entity:
						return materialization::entity_observation_v2_descriptor().id;
					case provider_worker_v4_ast_observation_kind::type:
						return materialization::type_observation_v2_descriptor().id;
					case provider_worker_v4_ast_observation_kind::call:
						return materialization::call_observation_v2_descriptor().id;
				}
				return {};
			}();
			if (rows[index].descriptor_id != expected_descriptor)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "rows", "descriptor-kind"));
			auto descriptor = materialization::observation_v2_descriptor(
				observation.kind == provider_worker_v4_ast_observation_kind::entity
					? materialization::observation_v2_kind::entity
					: observation.kind == provider_worker_v4_ast_observation_kind::type
					? materialization::observation_v2_kind::type
					: materialization::observation_v2_kind::call);
			if (!descriptor)
				return sdk::unexpected(std::move(descriptor.error()));
			if (auto valid = sdk::validate_row(**descriptor, rows[index]); !valid)
				return valid;
			if (auto valid = sdk::validate_domain_identity(**descriptor, rows[index]); !valid)
				return valid;
			const materialization::observation_v2_task_authority authority{
				compile_unit, source_snapshot, source_file, source_size_bytes};
			auto decoded = materialization::decode_observation_v2_row(rows[index], authority);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			const auto expected_kind =
				observation.kind == provider_worker_v4_ast_observation_kind::entity
				? materialization::observation_v2_kind::entity
				: observation.kind == provider_worker_v4_ast_observation_kind::type
				? materialization::observation_v2_kind::type
				: materialization::observation_v2_kind::call;
			if (decoded->kind != expected_kind ||
				decoded->final_relation_compile_unit_id != observation.compile_unit ||
				decoded->semantic_key != observation.semantic_key ||
				decoded->primary_span != observation.primary_span ||
				decoded->origin_chain != observation.origins ||
				decoded->exact_equivalence != observation.exact_equivalence ||
				decoded->limitation != observation.limitation)
				return sdk::unexpected(failure(
					"provider-worker-v4.ast-batch-invalid", "rows", "typed-observation-mismatch"));
			const auto canonical = observation.canonical_form();
			if (!previous.empty() && previous >= canonical)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "observation-order"));
			previous = canonical;
		}
		for (const auto& diagnostic : diagnostics)
			if (!valid_text(diagnostic))
				return sdk::unexpected(
					failure("provider-worker-v4.ast-batch-invalid", "diagnostic"));
		return {};
	}

	sdk::result<provider_worker_v4_ast_observation_batch>
	observe_provider_worker_v4_ast(provider::clang22::borrowed_translation_unit& unit,
								   const source_closure_task_v4_decoded& metadata,
								   std::string compile_unit,
								   provider_worker_v4_ast_observer_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			const source_closure_member* main{};
			if (auto valid = validate_task_metadata(metadata, compile_unit, main); !valid)
				return sdk::unexpected(std::move(valid.error()));

			observer_budget budget{limits};
			for (const auto text : {std::string_view{metadata.identity.task_id},
									std::string_view{metadata.identity.task_v4_digest},
									std::string_view{compile_unit},
									std::string_view{metadata.input.closure.snapshot_id},
									std::string_view{main->file_id}})
				if (auto reserved = budget.reserve_bytes(text.size(), "batch-identity"); !reserved)
					return sdk::unexpected(std::move(reserved.error()));

			provider_worker_v4_ast_observation_batch output{
				metadata.identity.task_id,
				metadata.identity.task_v4_digest,
				std::move(compile_unit),
				metadata.input.closure.snapshot_id,
				main->file_id,
				main->size_bytes,
				{},
				{},
				0U,
				{},
			};

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
			visitor extractor{unit,
							  output,
							  budget,
							  output.source_snapshot,
							  output.source_file,
							  metadata.input.toolchain_digest};
			const bool traversed = extractor.TraverseDecl(unit.ast().getTranslationUnitDecl());
			if (extractor.error())
				return sdk::unexpected(*extractor.error());
			if (!traversed)
				return sdk::unexpected(
					failure("provider-worker-v4.ast-traversal-failed", "translation-unit"));
			if (auto released = extractor.release_observations(); !released)
				return sdk::unexpected(std::move(released.error()));
#else
			(void)unit;
			return sdk::unexpected(
				failure("native.unsupported-clang-major", "translation-unit", "clang-major-22"));
#endif

			if (auto reserved = budget.reserve_rows(output.observations.size()); !reserved)
				return sdk::unexpected(std::move(reserved.error()));
			std::size_t total_row_reservation{};
			for (const auto& observation : output.observations)
			{
				const auto row_reservation =
					row_logical_reservation(observation, limits.maximum_logical_bytes);
				if (!row_reservation ||
					!checked_add(total_row_reservation,
								 *row_reservation,
								 limits.maximum_logical_bytes,
								 total_row_reservation))
					return sdk::unexpected(failure("provider-worker-v4.ast-resource-limit",
												   "bytes",
												   "row-reservation-overflow"));
			}
			if (auto reserved = budget.reserve_bytes(total_row_reservation, "row-reservations");
				!reserved)
				return sdk::unexpected(std::move(reserved.error()));
			const materialization::observation_v2_task_authority authority{
				output.compile_unit, output.source_snapshot, output.source_file, main->size_bytes};
			output.rows.reserve(output.observations.size());
			for (const auto& observation : output.observations)
			{
				const auto row_reservation =
					row_logical_reservation(observation, limits.maximum_logical_bytes);
				if (!row_reservation)
					return sdk::unexpected(failure("provider-worker-v4.ast-resource-limit",
												   "bytes",
												   "row-reservation-overflow"));
				materialization::native_observation_v2 native;
				switch (observation.kind)
				{
					case provider_worker_v4_ast_observation_kind::entity:
						native.kind = materialization::observation_v2_kind::entity;
						break;
					case provider_worker_v4_ast_observation_kind::type:
						native.kind = materialization::observation_v2_kind::type;
						break;
					case provider_worker_v4_ast_observation_kind::call:
						native.kind = materialization::observation_v2_kind::call;
						break;
				}
				native.final_relation_compile_unit_id = observation.compile_unit;
				native.semantic_key = observation.semantic_key;
				for (const auto& [key, value] : observation.payload)
					native.payload.push_back({key, value});
				native.primary_span = observation.primary_span;
				native.origin_chain = observation.origins;
				native.exact_equivalence = observation.exact_equivalence;
				native.limitation = observation.limitation;
				auto row = materialization::make_observation_v2_row(native, authority);
				if (!row)
					return sdk::unexpected(std::move(row.error()));
				const auto canonical_row = row->canonical_form();
				if (canonical_row.size() > *row_reservation)
					return sdk::unexpected(failure("provider-worker-v4.ast-bound-invalid",
												   "row-reservation",
												   "upper-bound-too-small"));
				output.rows.push_back(std::move(*row));
			}
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				failure("provider-worker-v4.ast-allocation", "observer", "bad-alloc"));
		}
	}
} // namespace cxxlens::detail::clang22
