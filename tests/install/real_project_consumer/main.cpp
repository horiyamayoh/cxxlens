#include "model.hpp"

int main()
{
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(real_project::descriptor()))
		return 1;
	auto engine = registry.build("installed-real-project");
	if (!engine)
		return 2;
	auto memory = cxxlens::sdk::make_in_memory_snapshot_store(*engine);
	auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(":memory:", *engine);
	if (!memory || !sqlite)
		return 3;
	if (const auto code = real_project::qualify(*memory, *engine); code != 0)
		return 10 + code;
	if (const auto code = real_project::qualify(*sqlite, *engine); code != 0)
		return 30 + code;
	return 0;
}
