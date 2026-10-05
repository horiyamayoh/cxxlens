#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

#include <cxxlens/sdk.hpp>

namespace
{
	void require(bool value, const char* reason)
	{
		if (!value)
		{
			std::cerr << reason << '\n';
			std::exit(EXIT_FAILURE);
		}
	}
} // namespace

int main(int argc, char** argv)
{
	using namespace cxxlens::sdk;
	if (argc != 2)
		return 2;
	relation_registry registry;
	for (const auto& descriptor : standard_relation_descriptors())
		require(registry.add(descriptor).has_value(), "original registry admission failed");
	auto engine = registry.build("original-input-projection");
	require(engine.has_value(), "original registry build failed");
	std::ifstream file(argv[1]);
	require(file.good(), "original query bundle is missing");
	std::string input{std::istreambuf_iterator<char>{file}, {}};
	auto queries = query::decode_application_queries(*engine, input);
	if (!queries)
	{
		std::cerr << queries.error().code << ':' << queries.error().field << ':'
				  << queries.error().detail << '\n';
		return 3;
	}
	auto templates = query::project_template_domains(*queries);
	if (!templates)
	{
		std::cerr << templates.error().code << ':' << templates.error().field << ':'
				  << templates.error().detail << '\n';
		return 4;
	}
	require(templates->source_queries.has_value(), "projection lost original query ownership");
	require(templates->populations.size() == 1, "wrong actual template unit population");
	const auto& population = templates->populations.front();
	const auto complete = query::finite_population_state::complete;
	require(population.subject_state == complete && population.constraint_state == complete &&
				population.capture_state == complete && population.frame_state == complete,
			"original template inventory did not close through the SDK");
	require(!population.subjects.empty() && !population.constraints.empty() &&
				!population.captures.empty() && !population.frames.empty(),
			"actual template fixture families are missing");
	bool named_subject{}, enclosing_owner{}, captured_object{};
	for (const auto& subject : population.subjects)
	{
		if (!subject.entity.empty() && !subject.semantic_usr.empty())
		{
			named_subject = true;
			require(subject.entity_state == complete,
					"actual compiler template entity identity did not bind");
		}
		if (!subject.owner_entity.empty())
		{
			enclosing_owner = true;
			require(subject.owner_entity_state == complete,
					"actual compiler enclosing template owner did not bind");
		}
	}
	for (const auto& capture : population.captures)
		if (!capture.captured_entity.empty() && !capture.captured_usr.empty())
		{
			captured_object = true;
			require(capture.entity_state == complete,
					"actual compiler captured object identity did not bind");
		}
	require(named_subject && enclosing_owner && captured_object,
			"actual template entity, owner and capture bindings were not exercised");
	bool root_synthesis{}, root_instantiation{}, nested_frame{};
	for (const auto& frame : population.frames)
	{
		if (frame.stack_state != complete)
		{
			std::cerr << "frame=" << frame.frame << " kind=" << frame.kind << '\n';
			for (const auto& gap : frame.gaps)
				std::cerr << gap.code << ':' << gap.subject << ':' << gap.detail << '\n';
		}
		require(frame.stack_state == complete && frame.completion_state == "ended",
				"actual mixed compiler frame stack did not close");
		require(frame.depth.has_value() && frame.is_instantiation.has_value(),
				"original compiler frame classification is missing");
		root_synthesis |= frame.parent.empty() && !*frame.is_instantiation && *frame.depth == 0;
		root_instantiation |= frame.parent.empty() && *frame.is_instantiation && *frame.depth == 1;
		nested_frame |= !frame.parent.empty();
	}
	require(root_synthesis && root_instantiation && nested_frame,
			"mixed original root and nested frame policy was not exercised");
}
