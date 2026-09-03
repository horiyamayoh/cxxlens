#pragma once

#define CXXLENS_CORPUS_NODISCARD [[nodiscard]]

namespace cxxlens_corpus
{
	struct input
	{
		int value{};
	};

	struct output
	{
		int value{};
	};

	template <class value_type>
	[[nodiscard]] constexpr value_type
	clamp(const value_type value, const value_type lower, const value_type upper)
	{
		return value < lower ? lower : (upper < value ? upper : value);
	}

	class transform
	{
	  public:
		virtual ~transform() = default;
		[[nodiscard]] virtual output apply(input value) const = 0;
	};

	CXXLENS_CORPUS_NODISCARD const transform& selected_transform();
	CXXLENS_CORPUS_NODISCARD int run_pipeline(int value);
} // namespace cxxlens_corpus
