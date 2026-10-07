#include <algorithm>
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
	if (argc != 2 && argc != 3)
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
	if (argc == 3 && std::string_view{argv[2]} == "--exceptional-exits")
	{
		query::projection_resource_usage usage;
		auto exits = query::project_exceptional_exits(*queries, {}, {}, usage);
		if (!exits)
		{
			std::cerr << exits.error().code << ':' << exits.error().field << ':'
					  << exits.error().detail << '\n';
			return 6;
		}
		require(exits->source_queries.has_value() && usage.operations > 0 &&
					usage.retained_bytes_bound > 0,
				"exceptional projection lost original queries or measured bounds");
		auto cleanup = query::project_exception_cleanup_facets(*queries, {}, {}, usage);
		if (!cleanup)
		{
			std::cerr << cleanup.error().code << ':' << cleanup.error().field << ':'
					  << cleanup.error().detail << '\n';
			return 7;
		}
		const auto complete = query::finite_population_state::complete;
		std::size_t known_true{}, known_false{}, partial{}, normal{}, exceptional{}, unbound{};
		for (const auto& specification : cleanup->specifications)
		{
			if (specification.specification_state == complete && specification.nonthrowing)
				(*specification.nonthrowing ? known_true : known_false)++;
			partial += specification.specification_state == query::finite_population_state::partial;
		}
		for (const auto& emission : cleanup->cleanups)
			if (emission.emission_state == complete)
			{
				normal += emission.route == "normal";
				exceptional += emission.route == "exceptional";
				unbound += emission.declaration_state != complete;
				require(emission.scope_state == complete,
						"original cleanup lost its physical compiler scope");
			}
		require(cleanup->source_queries.has_value() && usage.operations > 0 &&
					usage.retained_bytes_bound > 0 && known_true > 0 && known_false > 0 &&
					partial > 0 && normal > 0 && exceptional > 0 && unbound > 0,
				"original stored specifications and bound/unbound cleanup facets unavailable");
		for (const auto& population : exits->populations)
		{
			std::cout << "population " << population.function << ' '
					  << static_cast<unsigned>(population.enumeration_state) << ' '
					  << static_cast<unsigned>(population.scope_state) << ' '
					  << population.variants.size() << '\n';
			for (const auto& variant : population.variants)
			{
				std::size_t eligible{};
				for (const auto& occurrence : variant.occurrences)
					eligible += occurrence.eligibility == "eligible" ? 1U : 0U;
				std::cout << "variant " << population.function << ' '
						  << static_cast<unsigned>(variant.state) << ' ' << eligible << '\n';
			}
		}
		return 0;
	}
	const bool stock_events = argc == 3 && std::string_view{argv[2]} == "--stock-template-events";
	if (argc == 3 && !stock_events)
	{
		require(std::string_view{argv[2]} == "--complete-template-events",
				"unsupported original projection fixture");
		auto events = query::project_template_events(*queries);
		if (!events)
		{
			std::cerr << events.error().code << ':' << events.error().field << ':'
					  << events.error().detail << '\n';
			return 5;
		}
		const auto complete = query::finite_population_state::complete;
		require(events->source_queries.has_value() && events->populations.size() == 1,
				"complete event fixture lost original queries or unit scope");
		const auto& population = events->populations.front();
		require(population.candidate_state == complete &&
					population.evaluation_root_state == complete &&
					population.invocation_state == complete && population.calls.size() == 1,
				"actual original template event population did not close through SDK");
		return 0;
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
	auto events = query::project_template_events(*queries);
	if (!events)
	{
		std::cerr << events.error().code << ':' << events.error().field << ':'
				  << events.error().detail << '\n';
		return 5;
	}
	require(events->source_queries.has_value() && events->populations.size() == 1,
			"original template event query ownership or unit scope is missing");
	const auto& event_population = events->populations.front();
	if (stock_events)
	{
		require(event_population.candidate_state != complete &&
					event_population.evaluation_root_state != complete &&
					event_population.invocation_state != complete &&
					event_population.candidates.empty() && event_population.calls.empty(),
				"stock compiler falsely closed uninstrumented template events");
		return 0;
	}
	require(event_population.candidate_state == complete,
			"actual original candidate census did not close through SDK");
	const auto partial = query::finite_population_state::partial;
	require(event_population.evaluation_root_state == partial &&
				event_population.invocation_state == partial &&
				std::ranges::any_of(event_population.roots,
									[](const auto& root)
									{
										return root.requested_constant_context == true &&
											root.fold_failure == true &&
											root.completion == "complete" &&
											root.invocation_state ==
											query::finite_population_state::partial;
									}),
			"actual failed constant evaluation lost its independent invocation frontier");
	require(std::ranges::any_of(event_population.candidates,
								[](const auto& candidate)
								{
									return candidate.deduction_result == "substitution_failure" &&
										candidate.exclusion_disposition ==
										"substitution_exclusion" &&
										candidate.completed == true;
								}),
			"original final substitution-failure exclusion is missing");
	require(event_population.calls.size() == 2 &&
				std::ranges::any_of(event_population.roots,
									[complete](const auto& root)
									{
										return root.requested_constant_context == true &&
											root.fold_failure == false && root.call_count == 2 &&
											root.invocation_state == complete;
									}),
			"actual selected constant branch/root calls are missing or runtime calls leaked");
	for (const auto& call : event_population.calls)
		require(call.root_state == complete && !call.root_ids.empty() &&
					!call.callee_usr_hexes.empty(),
				"actual reached call lost its original root or target association");
}
