#include "corpus/pipeline.hpp"

namespace cxxlens_corpus
{
	namespace
	{
		class offset_transform final : public transform
		{
		  public:
			[[nodiscard]] output apply(const input value) const override
			{
				return {clamp(value.value + 3, 0, 100)};
			}
		};
	} // namespace

	const transform& selected_transform()
	{
		static const offset_transform instance;
		return instance;
	}
} // namespace cxxlens_corpus
