#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "store_port.hpp"

namespace cxxlens::detail::store
{
	[[nodiscard]] result<std::vector<std::byte>> encode_snapshot(const snapshot_data& snapshot);
	[[nodiscard]] result<std::shared_ptr<snapshot_data>>
	decode_snapshot(std::span<const std::byte> bytes);
} // namespace cxxlens::detail::store
