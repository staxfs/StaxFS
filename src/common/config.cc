#include "common/config.h"

#include "fmt/core.h"

namespace dfs {

auto CommonCmdlineOptions() -> cxxopts::Options {
  cxxopts::Options options("DFS Prototype Server",
                           "Server program of DFS Prototype");
  options.add_options()(
      "f,config", "Specify config file path",
      cxxopts::value<std::string>()->default_value("config.toml"))(
      "r,role",
      "Specify role in the cluster, the name should be configured in config "
      "file",
      cxxopts::value<std::string>()->default_value("data"))(
      "i,id",
      "Specify id in the cluster, the id should be configured in config file",
      cxxopts::value<std::string>()->default_value("1"))("h,help",
                                                         "Print usage");

  return options;
}

auto ParseCoreIds(std::string_view core_ids_string) -> std::vector<size_t> {
  std::vector<size_t> core_ids;
  std::string core_spec;
  std::istringstream core_ids_stream(core_ids_string.data());
  while (std::getline(core_ids_stream, core_spec, ',')) {
    if (auto dash_pos = core_spec.find('-'); dash_pos != std::string::npos) {
      // Range
      size_t start = std::stoul(core_spec.substr(0, dash_pos));
      size_t end = std::stoul(core_spec.substr(dash_pos + 1));
      if (start > end) {
        throw std::invalid_argument(
            fmt::format("Invalid core range: {}", core_spec));
      }
      for (size_t i = start; i <= end; i++) {
        core_ids.push_back(i);
      }
    } else {
      // Single core
      core_ids.push_back(std::stoul(core_spec));
    }
  }
  return core_ids;
}

} // namespace dfs