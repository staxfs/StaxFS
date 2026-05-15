#include "client/preload.h"

#include "client/file_descriptor.h"
#include "client/rpc_client.h"
#include "common/logging.h"
#include "nexus.h"
#include "spdlog/spdlog.h"
#include "version.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numa.h>
#include <rpc.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <toml++/toml.h>

namespace dfs {

// nexus is shared between threads of a same process
std::shared_ptr<SharedContext> gSharedCtx;
// Initialization will be automatically triggered upon first visit
thread_local std::unique_ptr<PreloadContext> gPreloadCtx =
    std::make_unique<PreloadContext>(*gSharedCtx);
std::atomic_int *gShmBuf = nullptr;
int gShmIndex = -1;
int gMaxClinet = 128;

SharedContext::SharedContext(std::string const &config_file_path) {
  auto config = toml::parse_file(config_file_path);
  auto env_client_uri = std::getenv("DFS_CLIENT_URI");
  std::string client_uri;
  if (env_client_uri != nullptr) {
    client_uri = env_client_uri;
  } else {
    SPDLOG_DEBUG("ENV: DFS_CLIENT_URI is not set");
    auto const client_host = config["client"]["host"].value<std::string_view>();
    uint32_t base_port = config["client"]["port"].value_or(0);
    if (!client_host.has_value()) {
      SPDLOG_ERROR("DFS Client Host is not set");
      std::exit(1);
    }
    // Check if MPI ranks are available, if yes, use them
    // const char *env_node_rank = std::getenv("OMPI_COMM_WORLD_NODE_RANK");
    // if (env_node_rank != nullptr) {
    //   node_rank_ = std::atoi(env_node_rank);
    // } else {
    int shmlen = gMaxClinet * sizeof(int);
    int shmfd =
        open("/dev/shm/.dfs-client.ranks", O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    if (shmfd < 0) {
      SPDLOG_ERROR("SHM open failed");
      SPDLOG_ERROR("errno : {}", std::strerror(errno));
      exit(-1);
    }

    if (ftruncate(shmfd, shmlen) != 0) {
      SPDLOG_ERROR("SHM ftruncate failed");
      SPDLOG_ERROR("errno : {}", std::strerror(errno));
      exit(-1);
    }

    // https://man7.org/linux/man-pages/man2/mmap.2.html
    const int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    // const int flags = MAP_SHARED_VALIDATE | MAP_POPULATE;
    const int flags = MAP_SHARED | MAP_POPULATE;

    gShmBuf = static_cast<std::atomic_int *>(
        mmap(nullptr, shmlen, prot, flags, shmfd, 0));
    if (gShmBuf == nullptr || gShmBuf == MAP_FAILED) {
      SPDLOG_ERROR("SHM mmap failed");
      SPDLOG_ERROR("errno : {}", std::strerror(errno));
      exit(-1);
    }
    // Get an available node_rank from shared memory array

    int pid = getpid();
    int expected = 0;
    for (int i = 0; i < gMaxClinet; i++) {
      expected = 0;
      if (gShmBuf[i].compare_exchange_strong(expected, pid)) {
        gShmIndex = i;
        break;
      }
      if (kill(expected, 0) != 0 && // dead process, can be overwritten
          gShmBuf[i].compare_exchange_strong(expected, pid)) {
        gShmIndex = i;
        break;
      }
    }
    if (gShmIndex == -1) {
      SPDLOG_ERROR("Couldn't find an available port. Up to {} clients can run "
                   "on a single machine!",
                   gMaxClinet);
      exit(-1);
    }
    SPDLOG_INFO("Got node_rank = {}", gShmIndex);
    node_rank_ = gShmIndex;
    // }
    client_uri =
        fmt::format("{}:{}", client_host.value(), base_port + node_rank_);
  }
  {
    int nexus_node = -1;
    // check if local node has hugepages:
    int node = numa_node_of_cpu(sched_getcpu());
    const char *node_dir = "/sys/devices/system/node";
    const char *nr_path = "hugepages/hugepages-2048kB/nr_hugepages";
    auto path = fmt::format("{}/node{}/{}", node_dir, node, nr_path);
    auto read_nr_pages = [](const char *path) -> int {
      auto fstream = fopen(path, "r");
      int nr_hugepages = 0;
      if (fstream != nullptr) {
        int ret = fscanf(fstream, "%d", &nr_hugepages);
      }
      fclose(fstream);
      return nr_hugepages;
    };
    if (read_nr_pages(path.c_str()) > 0) {
      nexus_node = node;
    } else { // check if any node has hugepages:
      for (node = 0; node < numa_num_configured_nodes(); node++) {
        auto path = fmt::format("{}/node{}/{}", node_dir, node, nr_path);
        if (read_nr_pages(path.c_str()) > 0) {
          nexus_node = node;
          break;
        }
      }
    }
    if (nexus_node == -1) {
      SPDLOG_ERROR("!! CANNOT FIND A NODE WITH HUGEPAGES !!");
      abort();
    }
    SPDLOG_INFO("Client URI: {}, nexus on node #{}", client_uri, nexus_node);
    nexus_ = std::make_shared<erpc::Nexus>(client_uri, nexus_node);
  }

  // FIXME: for compatibility
  auto cluster_config = config;
  auto cluster_config_path =
      config["client"]["cluster_config_path"].value<std::string_view>();
  if (cluster_config_path.has_value()) {
    SPDLOG_INFO("Cluster config path is set: {}", cluster_config_path.value());
    cluster_config = toml::parse_file(cluster_config_path.value());
  }

  // read data server url list and setup connection
  int nr_dataserver = cluster_config["data"]["num"].value_or(0);
  // SPDLOG_INFO("Number of Data Server: {}", nr_dataserver);
  for (int i = 0; i < nr_dataserver; i++) {
    std::string const &id = std::to_string(i);
    auto const host =
        cluster_config["data"][id]["host"].value<std::string_view>();
    uint16_t port = cluster_config["data"][id]["port"].value_or(0);
    if (!host.has_value()) {
      SPDLOG_ERROR("Data Server {} Host is not set", id);
      std::exit(1);
    }
    if (port == 0) {
      SPDLOG_ERROR("Data Server {} Port is not set", id);
      std::exit(1);
    }
    std::string const &data_server_uri =
        fmt::format("{}:{}", host.value(), port);
    uint32_t data_threads = cluster_config["data"][id]["threads"].value_or(1);
    // SPDLOG_INFO("Data Server {} URI: {}, has {} threads", id,
    // data_server_uri,
    //             data_threads);
    AddDataServer(data_server_uri, data_threads);
  }

  // read meta server url list and setup connection
  int nr_metaserver = cluster_config["meta"]["num"].value_or(0);
  // SPDLOG_INFO("Number of Metadata Server: {}", nr_metaserver);
  for (int i = 0; i < nr_metaserver; i++) {
    std::string id = std::to_string(i);
    auto const host =
        cluster_config["meta"][id]["host"].value<std::string_view>();
    uint16_t port = cluster_config["meta"][id]["port"].value_or(0);
    if (!host.has_value()) {
      SPDLOG_ERROR("Meta Server {} Host is not set", id);
      std::exit(1);
    }
    if (port == 0) {
      SPDLOG_ERROR("Meta Server {} Port is not set", id);
      std::exit(1);
    }
    std::string const &meta_server_uri =
        fmt::format("{}:{}", host.value(), port);
    uint32_t meta_threads =
        cluster_config["meta"][id]["threads"].value_or(1);
    // SPDLOG_INFO("Meta Server {} URI: {}, has {} threads", id, meta_server_uri,
    //             meta_threads);
    AddMetaServer(meta_server_uri, meta_threads);
  }

  // If  bind() still complains about port already in use, use this again
  // https://stackoverflow.com/questions/3275015/ld-preload-affects-new-child-even-after-unsetenvld-preload
  // unsetenv("LD_PRELOAD");

  SPDLOG_INFO("PID : {}. You can attach to this process by using gdb or perf",
              getpid());
  // sleep for 5 seconds to attach (such as gdb or perf)
  // get from environment variable
  int wait_attach_sec =
      std::getenv("DFS_CLIENT_WAIT_ATTACH_SEC") != nullptr
          ? std::atoi(std::getenv("DFS_CLIENT_WAIT_ATTACH_SEC"))
          : 0;
  SPDLOG_INFO("Wait for {} seconds to attach", wait_attach_sec);
  std::this_thread::sleep_for(std::chrono::seconds(wait_attach_sec));
  SPDLOG_INFO("DFS Client Hook is ready!");
}

PreloadContext::PreloadContext(SharedContext &shared_ctx) {
  rpc_client_ = std::make_unique<ClientRpcWrapper>(shared_ctx);
  fd_manager_ = std::make_unique<FdManager>();
}

PreloadContext::~PreloadContext() {
  rpc_client_.reset();
  SPDLOG_INFO("DFS Client Hook is close!");
}

extern "C" {

void InitPreload() {
  InitLogger();
  SPDLOG_INFO("DFS Client Hook [commit={}] [branch={}] [tag={}]",
              dfs::kGitCommit, dfs::kGitBranch, dfs::kGitTag);
  auto config_file_path = "";
  auto const env_config_file_path = std::getenv("DFS_CLIENT_CONFIG_PATH");
  if (env_config_file_path == nullptr) {
    SPDLOG_WARN("DFS_CLIENT_CONFIG_PATH is not set");
    // try default path /tmp/dfs-prototype/client.toml
    auto const default_config_file_path = "/tmp/dfs-prototype/client.toml";
    // if there is a file, continue, esle exit
    if (std::filesystem::exists(default_config_file_path)) {
      SPDLOG_INFO("Use default config file: {}", default_config_file_path);
      config_file_path = default_config_file_path;
    } else {
      SPDLOG_ERROR("Default config file does not exist: {}",
                   default_config_file_path);
      std::exit(1);
    }
  } else {
    config_file_path = env_config_file_path;
  }
  SPDLOG_INFO("Config file: {}", config_file_path);
  gSharedCtx = std::make_shared<SharedContext>(config_file_path);
}

void DestroyPreload() {
  // close connection pool
  if (gShmIndex != -1) {
    gShmBuf[gShmIndex] = 0;
  }
  if (gShmBuf != nullptr) {
    munmap(gShmBuf, std::thread::hardware_concurrency() * sizeof(int));
  }
}
}

} // namespace dfs
