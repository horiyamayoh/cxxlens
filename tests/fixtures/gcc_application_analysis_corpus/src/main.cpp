#include "corpus/pipeline.hpp"

int main()
{
	return cxxlens_corpus::run_pipeline(39) == 42 ? 0 : 1;
}
