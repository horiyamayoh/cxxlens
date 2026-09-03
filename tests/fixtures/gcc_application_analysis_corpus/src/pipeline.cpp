#include "corpus/pipeline.hpp"

namespace cxxlens_corpus
{
	int run_pipeline(const int value)
	{
		return selected_transform().apply({value}).value;
	}
} // namespace cxxlens_corpus
