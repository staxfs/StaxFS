#pragma once

#include <cxxopts.hpp>

namespace dfs {

auto CommonCmdlineOptions() -> cxxopts::Options;

auto ParseCoreIds(std::string_view core_ids_string) -> std::vector<size_t>;

} // namespace dfs