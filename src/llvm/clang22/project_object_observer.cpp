#include "project_object_observer.hpp"

#include "project_object_recorder.hpp"

namespace cxxlens::detail::clang22::object_semantics
{
	class project_object_observer::implementation
	{
	  public:
		project_object_observations output;
		project_object_event_scope scope{output,
										 {},
										 original_object_hooks_available(),
										 original_atomic_hooks_available(),
										 original_fence_hooks_available()};
	};
	project_object_observer::project_object_observer() : state_{std::make_unique<implementation>()}
	{
	}
	project_object_observer::~project_object_observer() = default;
	void project_object_observer::freeze(bool parser_completed, bool fatal_error) noexcept
	{
		state_->scope.freeze();
		if (!parser_completed || fatal_error)
		{
			state_->output.sequence_partial = true;
			state_->output.object_partial = true;
			state_->output.atomic_partial = true;
			state_->output.fence_partial = true;
		}
	}
	const project_object_observations& project_object_observer::observations() const noexcept
	{
		return state_->output;
	}
} // namespace cxxlens::detail::clang22::object_semantics
