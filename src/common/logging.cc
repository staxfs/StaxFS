#include "spdlog/cfg/env.h"
#include "spdlog/common.h"
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace dfs {

void InitLogger() {
  spdlog::cfg::load_env_levels();

  auto main_logger = spdlog::stdout_color_mt("main_logger");
  spdlog::set_default_logger(main_logger);

  // Only add file log if log isn't turned off.
  if (main_logger->level() != spdlog::level::off) {
    auto const log_filename = std::getenv("DFS_LOG_FILENAME");
    if (log_filename != nullptr) {
      main_logger->sinks().emplace_back(
          std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
              log_filename, 1024 * 1024 * 100, 0, true));
    }
    auto const flush_interval = std::getenv("DFS_LOG_FLUSH_INTERVAL");
    if (flush_interval != nullptr) {
      spdlog::flush_every(std::chrono::seconds(std::stoi(flush_interval)));
    }
  }
  spdlog::flush_on(spdlog::level::err);
  spdlog::set_pattern("[%H:%M:%S.%f %z] [%^%L%$] [pid %P] [tid %t] [%@] %v");

  // SPDLOG_INFO("SPDLOG Active Logger initialized with log level {}.",
  //             spdlog::level::to_string_view(
  //                 (spdlog::level::level_enum)SPDLOG_ACTIVE_LEVEL));

  SPDLOG_INFO("SPDLOG Global Logger initialized with log level {}.",
              spdlog::level::to_string_view(spdlog::get_level()));
  SPDLOG_INFO("main_logger  initialized with log level {}.",
              spdlog::level::to_string_view(main_logger->level()));
}

} // namespace dfs