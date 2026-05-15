#include "threading/affinity.h"

#include "spdlog/spdlog.h"
#include <cstddef>

namespace dfs {

auto GetCpuAffinity() -> std::vector<std::size_t> {
  std::vector<size_t> cores;
  cpu_set_t cpuset;
  if (auto rc =
          ::pthread_getaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
      rc != 0) {
    SPDLOG_ERROR("failed to get affinity {}\n", ::strerror(errno));
    return cores;
  }
  for (size_t i = 0; i < CPU_SETSIZE; ++i) {
    if (CPU_ISSET(i, &cpuset)) {
      cores.push_back(i);
    }
  }
  return cores;
}

void EnableOnCores(std::vector<std::size_t> const &core_ids) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  for (auto core_id : core_ids) {
    CPU_SET(core_id, &cpuset);
  }
  if (auto rc =
          ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
      rc != 0) {
    SPDLOG_ERROR("failed to set affinity {}\n", ::strerror(errno));
  }
}

void BindToCore(std::size_t core_id) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);
  if (auto rc =
          ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuset), &cpuset);
      rc != 0) {
    SPDLOG_ERROR("failed to set affinity {}\n", ::strerror(errno));
  }
}

} // namespace dfs