#pragma once

#include <memory>

namespace cxxlens::detail::clang22::object_semantics
{
	struct project_object_observations;
	/** Source-private parser facade. It exposes no LLVM/Clang headers to the
	 * application analyzer; the native implementation owns its borrowed views. */
	class project_object_observer
	{
		class implementation;
		std::unique_ptr<implementation> state_;

	  public:
		project_object_observer();
		~project_object_observer();
		project_object_observer(const project_object_observer&) = delete;
		project_object_observer& operator=(const project_object_observer&) = delete;
		void freeze(bool parser_completed, bool fatal_error) noexcept;
		[[nodiscard]] const project_object_observations& observations() const noexcept;
	};
} // namespace cxxlens::detail::clang22::object_semantics
