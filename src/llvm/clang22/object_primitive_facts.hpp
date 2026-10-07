#pragma once
#include <optional>
#include <string>

namespace cxxlens::detail::clang22::object_semantics
{
	// Native-only compiler classifications. Consumer rule verdicts and Finding IDs
	// never cross into these primitive producer types.
	enum class state
	{
		complete,
		unavailable,
		conflicting
	};
	struct integer_range
	{
		std::string lower, upper;
	};
	struct missing_return
	{
		enum class return_kind
		{
			value,
			reference,
			void_value,
			constructor,
			destructor,
			unknown
		};
		enum class exit_kind
		{
			fallthrough,
			return_value,
			exceptional,
			no_return,
			unreachable,
			unknown
		};
		return_kind returns{return_kind::unknown};
		exit_kind exits{exit_kind::unknown};
		std::optional<bool> is_main, is_coroutine, coroutine_has_return_void;
		state exit_binding{state::unavailable};
	};
	struct bool_representation
	{
		enum class kind
		{
			representation_load,
			bit_cast,
			integral_conversion,
			unknown
		};
		kind operation{kind::unknown};
		std::optional<bool> canonical_bool;
		std::optional<integer_range> original_representation;
		state representation_binding{state::unavailable};
	};
	struct enum_value
	{
		std::optional<bool> fixed_underlying;
		std::optional<integer_range> compiler_value_domain, original_value;
		state enumeration_binding{state::unavailable};
	};
} // namespace cxxlens::detail::clang22::object_semantics
