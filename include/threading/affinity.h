#pragma once

#include <cstddef>
#include <vector>

namespace dfs {

auto GetCpuAffinity() -> std::vector<std::size_t>;

void EnableOnCores(std::vector<std::size_t> const &core_ids);

void BindToCore(std::size_t core_id);

} // namespace dfs