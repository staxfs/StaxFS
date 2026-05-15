#include "backward.hpp"
#include <cerrno>
#include "common/config.h"
#include "common/dfs.h"
#include "common/listdir_profile.h"
#include "common/logging.h"
#include "common/metadata_op_profile.h"
#include "common/metadata_types.h"
#include "cxl/cxl_mem.h"
#include "data_generated.h"
#include "mdfdrequest_generated.h"
#include "mdguardiancommonrequest_generated.h"
#include "mdguardianheartrequest_generated.h"
#include "mdmovenoderequest_generated.h"
#include "mdpathcommonrequest_generated.h"
#include "mdpathcommonresponse_generated.h"
#include "mdpersistencerequest_generated.h"
#include "mdrequest_generated.h"
#include "msg_buffer.h"
#include "nexus.h"
#include "req_handle.h"
#include "server/cxl_persistence.h"
#include "server/data.h"
#include "server/metadata.h"
#include "server/rpc_server.h"
#include "spdlog/spdlog.h"
#include "threading/affinity.h"
#include "version.h"
#include <bits/ranges_util.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numa.h>
#include <numaif.h>
#include <rpc.h>
#include <string>
#include <string_view>
#include <sys/file.h>
#include <sys/sdt.h>
#include <toml++/toml.h>
#include <unistd.h>
#include <unordered_map>
#include <util/numautils.h>
#include <utility>
#include <vector>

extern int gHeartbeatCycle; // (10s) at server/rpc_server.cc
extern std::atomic<bool> gLoadBalance;

std::unique_ptr<dfs::ObjectStore> gStoreManager = nullptr;
std::unique_ptr<dfs::Metadata> gMetaManager = nullptr;
std::atomic<bool> gShouldExit = false;

extern struct timespec gStartTime, gEndTime;
std::mutex gMtx;
int gReqsNum = 0;
std::atomic<uint64_t> gConnectedSessionNum(0);
std::atomic<int> gLocalServerSetupReady(0);
std::vector<int64_t> gReqs;

namespace {

class MetadataOperationClockGuard {
public:
  explicit MetadataOperationClockGuard(uint64_t request_hlc) {
    if (dfs::gCXLPersistence != nullptr) {
      wal_ = dfs::gCXLPersistence->WAL();
      wal_->BeginOperation(request_hlc);
    }
  }

  ~MetadataOperationClockGuard() {
    if (wal_ != nullptr) {
      wal_->EndOperation();
    }
  }

  MetadataOperationClockGuard(const MetadataOperationClockGuard &) = delete;
  auto operator=(const MetadataOperationClockGuard &)
      -> MetadataOperationClockGuard & = delete;

private:
  dfs::CompactWAL *wal_ = nullptr;
};

auto CurrentMetadataServerHLC() -> uint64_t {
  if (dfs::gCXLPersistence != nullptr) {
    return dfs::gCXLPersistence->WAL()->CurrentVersion();
  }
  return 0;
}

} // namespace

void SessionHandler(int /*unused*/, erpc::SmEventType ev,
                    erpc::SmErrType /*unused*/, void * /*unused*/) {
  if (ev == erpc::SmEventType::kConnected) {
    gConnectedSessionNum.fetch_add(1);
  }
  SPDLOG_TRACE("DFS Server Session up");
}

struct ContContextT {
  int finished_;
  char *buf_;
};

void ContFunc(void *context, void *tag) {
  auto *ret = reinterpret_cast<ContContextT *>(tag);
  SPDLOG_TRACE("Receive response");
  ret->finished_ = 1;
}

void HelloReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDGeneralReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDPathCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDFDCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void IoReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDMetaCommunicationReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDGuardianCommunicationReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDTestCommunicationReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDGuardianCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDPersistenceReqHandler(erpc::ReqHandle *req_handle, void *_ctx);
void MDPersistenceReqSplitHandler(erpc::ReqHandle *req_handle, void *_ctx);

void ConnectionBetweenMetaServers(dfs::ServerContext &meta_context) {
  for (int i = 0; i < meta_context.meta_uri_list_.size(); i++) {
    if (i != meta_context.meta_num_) {
      // Establish connections in sequence
      int sum = 0;
      for (int j = 0; j < i; j++) {
        sum += meta_context.meta_rpc_num_[j] * 2 - 1;
      }
      sum += meta_context.meta_rpc_num_[i];
      while (gConnectedSessionNum.load() != meta_context.rpc_id_ + sum - 1) {
      }

      int remote_rpc_id =
          (meta_context.rpc_id_ - 1) % (meta_context.meta_rpc_num_[i] - 1) + 1;
      int session_num = meta_context.rpc_->create_session(
          meta_context.meta_uri_list_[i], remote_rpc_id);
      erpc::rt_assert(session_num >= 0, "Failed to create session");
      while (!meta_context.rpc_->is_connected(session_num)) {
        meta_context.rpc_->run_event_loop_once();
      }
      meta_context.meta_to_meta_sessions_[i] = session_num;

      SPDLOG_INFO("This is meta {}, Connecting from {} to {}, "
                  "rpc_id {}, session_num {}",
                  meta_context.meta_num_,
                  meta_context.meta_uri_list_[meta_context.meta_num_],
                  meta_context.meta_uri_list_[i], remote_rpc_id, session_num);

      std::ostringstream oss;
      oss << meta_context.meta_uri_list_.size() << '\n';
      for (auto uri : meta_context.meta_uri_list_) {
        oss << uri << '\n';
      }
      for (auto rpc_num : meta_context.meta_rpc_num_) {
        oss << rpc_num << '\n';
      }
      std::string serialized_data = oss.str();

      auto req =
          meta_context.rpc_->alloc_msg_buffer_or_die(serialized_data.size());
      auto resp = meta_context.rpc_->alloc_msg_buffer_or_die(1);
      memcpy(req.buf_, serialized_data.data(), serialized_data.size());
      ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
      meta_context.rpc_->enqueue_request(meta_context.meta_to_meta_sessions_[i],
                                         dfs::RPCType::kMetaCommunicationReq,
                                         &req, &resp, ContFunc, &cc);

      while (cc.finished_ == 0) {
        meta_context.rpc_->run_event_loop_once();
      }
      meta_context.rpc_->free_msg_buffer(req);
      meta_context.rpc_->free_msg_buffer(resp);
    } else {
      meta_context.meta_to_meta_sessions_[i] = -1;
    }
  }
}

void ConnectionBetweenGuardianServers(dfs::GuardianContext &meta_context) {
  for (int i = 0; i < meta_context.meta_uri_list_.size(); i++) {
    if (i != meta_context.meta_num_) {
      for (int j = 0; j < meta_context.meta_rpc_num_[i]; j++) {
        // Establish connections in sequence
        int sum = 0;
        for (int k = 0; k < i; k++) {
          sum += meta_context.meta_rpc_num_[k] * 2 - 1;
        }
        while (gConnectedSessionNum.load() != meta_context.rpc_id_ + sum + j) {
        }
        int session_num = meta_context.rpc_->create_session(
            meta_context.meta_uri_list_[i], j);
        erpc::rt_assert(session_num >= 0, "Failed to create session");
        while (!meta_context.rpc_->is_connected(session_num)) {
          meta_context.rpc_->run_event_loop_once();
        }
        meta_context.meta_to_meta_sessions_[i][j] = session_num;

        SPDLOG_INFO("This is guardian {}, Connecting from {} to {}, "
                    "rpc_id = {}, session_num {}",
                    meta_context.meta_num_,
                    meta_context.meta_uri_list_[meta_context.meta_num_],
                    meta_context.meta_uri_list_[i], j, session_num);
      }

      const int local_server_threads =
          meta_context.meta_rpc_num_[meta_context.meta_num_] - 1;
      while (gLocalServerSetupReady.load(std::memory_order_acquire) <
             local_server_threads) {
        meta_context.rpc_->run_event_loop_once();
      }

      std::ostringstream oss;
      oss << meta_context.meta_uri_list_.size() << '\n';
      for (auto uri : meta_context.meta_uri_list_) {
        oss << uri << '\n';
      }
      for (auto rpc_num : meta_context.meta_rpc_num_) {
        oss << rpc_num << '\n';
      }
      std::string serialized_data = oss.str();

      auto req =
          meta_context.rpc_->alloc_msg_buffer_or_die(serialized_data.size());
      auto resp = meta_context.rpc_->alloc_msg_buffer_or_die(1);
      memcpy(req.buf_, serialized_data.data(), serialized_data.size());
      ContContextT cc = {0, reinterpret_cast<char *>(resp.buf_)};
      meta_context.rpc_->enqueue_request(
          meta_context.meta_to_meta_sessions_[i][0],
          dfs::RPCType::kGuardianCommunicationReq, &req, &resp, ContFunc, &cc);

      while (cc.finished_ == 0) {
        meta_context.rpc_->run_event_loop_once();
      }
      meta_context.rpc_->free_msg_buffer(req);
      meta_context.rpc_->free_msg_buffer(resp);
    } else {
      for (auto &session : meta_context.meta_to_meta_sessions_[i]) {
        session = -1;
      }
    }
  }
}

void EventLoopThread(erpc::Nexus *nexus, size_t rpc_id, size_t core_id,
                     int meta_num,
                     const std::vector<std::string> &meta_uri_list,
                     const std::vector<int> &meta_rpc_num) {
  SPDLOG_INFO("Starting {} server thread on core {}, rpc_id = {}",
              meta_num >= 0 ? "meta" : "data", core_id, rpc_id);
  if (meta_num < 0) {
  }
  dfs::BindToCore(core_id);

  dfs::ServerContext server_context;
  server_context.rpc_id_ = rpc_id;
  server_context.rpc_ = new erpc::Rpc<erpc::CTransport>(nexus, &server_context,
                                                        rpc_id, SessionHandler);

  if (meta_num >= 0) {
    server_context.meta_num_ = meta_num;
    server_context.meta_uri_list_.assign(meta_uri_list.begin(),
                                         meta_uri_list.end());
    server_context.meta_rpc_num_.assign(meta_rpc_num.begin(),
                                        meta_rpc_num.end());
    server_context.meta_to_meta_sessions_.resize(meta_uri_list.size());
    server_context.nr_reqs_.resize(meta_uri_list.size());
    for (auto &nr_req : server_context.nr_reqs_) {
      nr_req = 0;
    }
    gLocalServerSetupReady.fetch_add(1, std::memory_order_release);
    ConnectionBetweenMetaServers(server_context);
  }
  clock_gettime(CLOCK_MONOTONIC, &gStartTime);

  while (true) {
    server_context.rpc_->run_event_loop(10);
    if (gShouldExit.load()) {
      break;
    }
    if (meta_num >= 0) {
      clock_gettime(CLOCK_MONOTONIC, &gEndTime);
      if ((gEndTime.tv_sec - gStartTime.tv_sec) >= gHeartbeatCycle &&
          (gReqsNum & (1 << (server_context.rpc_id_ - 1))) == 0) {
        gMtx.lock();
        gReqsNum |= 1 << (server_context.rpc_id_ - 1);
        for (int i = 0; i < gReqs.size(); i++) {
          gReqs[i] += server_context.nr_reqs_[i];
        }
        gMtx.unlock();

        for (auto &nr_req : server_context.nr_reqs_) {
          nr_req = 0;
        }
      }
    }
  }
  delete server_context.rpc_;
  SPDLOG_INFO("Server thread on core {} exiting", core_id);
}

void CollectReqs(dfs::GuardianContext &guardian_context) {
  int mask =
      (1 << (guardian_context.meta_rpc_num_[guardian_context.meta_num_] - 1)) -
      1;
  while ((gReqsNum & mask) != mask) {
    struct timespec tmp;
    clock_gettime(CLOCK_MONOTONIC, &tmp);
    if ((tmp.tv_sec - gStartTime.tv_sec) >= gHeartbeatCycle + 1) {
      break;
    }
  }

  gMtx.lock();
  gStartTime = gEndTime;
  gReqsNum = 0;
  gMtx.unlock();
  for (int i = 0; i < gReqs.size(); i++) {
    guardian_context.reqs_[i] = gReqs[i];
    gReqs[i] = 0;
  }
  for (int i = 0; i < gReqs.size(); i++) {
    if (i != guardian_context.meta_num_) {
      guardian_context.reqs_[guardian_context.meta_num_] -=
          guardian_context.reqs_[i];
    }
  }
  guardian_context.heart_num_ |= 1 << (guardian_context.meta_num_);

  SPDLOG_INFO("All request: meta_num = {}, reqs count = {}",
              guardian_context.meta_num_,
              guardian_context.reqs_[guardian_context.meta_num_]);

  gMetaManager->PrintSpace();
  if (guardian_context.meta_num_ != 0) {
    struct timespec time = dfs::Heartbeat(guardian_context);
    gMtx.lock();
    gStartTime = time;
    gMtx.unlock();
  }
}

void GuardianThread(erpc::Nexus *nexus, size_t rpc_id, size_t core_id,
                    int meta_num, const std::vector<std::string> &meta_uri_list,
                    const std::vector<int> &meta_rpc_num) {
  SPDLOG_INFO("Starting Guardian thread on core {}, rpc_id = {}", core_id,
              rpc_id);
  dfs::BindToCore(core_id);

  dfs::GuardianContext guardian_context;
  guardian_context.rpc_id_ = rpc_id;
  guardian_context.rpc_ = new erpc::Rpc<erpc::CTransport>(
      nexus, &guardian_context, rpc_id, SessionHandler);

  if (meta_num != -1) {
    guardian_context.meta_num_ = meta_num;
    guardian_context.meta_uri_list_.assign(meta_uri_list.begin(),
                                           meta_uri_list.end());
    guardian_context.meta_rpc_num_.assign(meta_rpc_num.begin(),
                                          meta_rpc_num.end());
    guardian_context.meta_to_meta_sessions_.resize(meta_uri_list.size());
    for (int i = 0; i < meta_uri_list.size(); i++) {
      guardian_context.meta_to_meta_sessions_[i].resize(
          guardian_context.meta_rpc_num_[i]);
    }

    guardian_context.reqs_.resize(meta_uri_list.size());
    gReqs.resize(meta_uri_list.size());
    // guardian_context.req_msgbufs_.resize(meta_uri_list.size());
    // guardian_context.resp_msgbufs_.resize(meta_uri_list.size());
    ConnectionBetweenGuardianServers(guardian_context);
  }
  clock_gettime(CLOCK_MONOTONIC, &gStartTime);

  while (true) {
    guardian_context.rpc_->run_event_loop(10);
    if (gShouldExit.load()) {
      break;
    }
    clock_gettime(CLOCK_MONOTONIC, &gEndTime);
    if (((gEndTime.tv_sec - gStartTime.tv_sec) +
         (gEndTime.tv_nsec - gStartTime.tv_nsec) / 1e9) >= gHeartbeatCycle &&
        (guardian_context.heart_num_ & (1 << guardian_context.meta_num_)) ==
            0) {
      CollectReqs(guardian_context);
    }

    dfs::ServicePendingRemoteInodeChanges(guardian_context);
  }
  delete guardian_context.rpc_;
  SPDLOG_INFO("Guardian thread on core {} exiting", core_id);
}

void ExitSignalHandler(int signum) {
  SPDLOG_INFO("Received signal {}, exiting...", signum);
  gShouldExit.store(true);
}

void SigsegvSignalHandler(int signum) {
  // This should flush the logger before exiting.
  SPDLOG_ERROR("Signal {} received, exiting...", signum);

  backward::SignalHandling sh;

  ::raise(signum);
}

void SetupSignalHandler() {
  {
    int signals[] = {SIGINT, SIGTERM, SIGHUP};
    for (auto const sig : signals) {
      struct sigaction exit_action;
      ::memset(&exit_action, 0, sizeof(exit_action));
      exit_action.sa_handler = ExitSignalHandler;
      ::sigemptyset(&exit_action.sa_mask);
      exit_action.sa_flags = 0;
      ::sigaction(sig, &exit_action, nullptr);
    }
  }
  {
    int signals[] = {
        SIGABRT, // Abort signal from abort(3)
        SIGBUS,  // Bus error (bad memory access)
        SIGFPE,  // Floating point exception
        SIGILL,  // Illegal Instruction
        SIGIOT,  // IOT trap. A synonym for SIGABRT
        SIGQUIT, // Quit from keyboard
        SIGSEGV, // Invalid memory reference
        SIGSYS,  // Bad argument to routine (SVr4)
        SIGTRAP, // Trace/breakpoint trap
        SIGXCPU, // CPU time limit exceeded (4.2BSD)
        SIGXFSZ,
    };
    for (auto const sig : signals) {
      struct sigaction coredump_action;
      ::memset(&coredump_action, 0, sizeof(coredump_action));
      coredump_action.sa_handler = SigsegvSignalHandler;
      ::sigemptyset(&coredump_action.sa_mask);
      coredump_action.sa_flags = SA_NODEFER;
      ::sigaction(sig, &coredump_action, nullptr);
    }
  }
}

void AllocateHugepages(int numa_node) {
  std::string path = "/sys/devices/system/node/node" +
                     std::to_string(numa_node) +
                     "/hugepages/hugepages-2048kB/nr_hugepages";
  size_t increase_pages = HUGEPAGES;

  std::string lock_path =
      "/var/lock/hugepage_lock_" + std::to_string(numa_node);
  int fd_lock = open(lock_path.c_str(), O_CREAT | O_RDWR, 0666);
  if (fd_lock < 0) {
    SPDLOG_ERROR("Initialize Hugepage: Failed to create lock for NUMA node {}",
                 numa_node);
    exit(-1);
  }

  if (flock(fd_lock, LOCK_EX) != 0) {
    SPDLOG_ERROR("Initialize Hugepage: flock lock failed on NUMA node {}",
                 numa_node);
    close(fd_lock);
    exit(-1);
  }

  size_t current = 0;
  {
    std::ifstream in(path);
    if (!in.is_open()) {
      SPDLOG_ERROR(
          "Initialize Hugepage: Failed to read hugepages on NUMA node {}",
          numa_node);
      flock(fd_lock, LOCK_UN);
      close(fd_lock);
      exit(-1);
    }
    in >> current;
  }

  size_t target = current + increase_pages;
  {
    std::ofstream out(path);
    if (!out.is_open()) {
      SPDLOG_ERROR(
          "Initialize Hugepage: Failed to write hugepages on NUMA node {}",
          numa_node);
      flock(fd_lock, LOCK_UN);
      close(fd_lock);
      exit(-1);
    }
    out << target;
  }

  flock(fd_lock, LOCK_UN);
  close(fd_lock);

  SPDLOG_INFO(
      "Initialize Hugepage: Change numa_node {} huge page from {} to {}",
      numa_node, current, target);
}

auto main(int argc, char *argv[]) -> int {
  dfs::InitLogger();

  SetupSignalHandler();

  SPDLOG_INFO("DFS Server [commit={}] [branch={}] [tag={}]", dfs::kGitCommit,
              dfs::kGitBranch, dfs::kGitTag);
  auto options = dfs::CommonCmdlineOptions();
  auto args = options.parse(argc, argv);

  if (args.count("help") != 0UL) {
    std::cout << options.help() << std::endl;
    return 0;
  }

  if (args.count("config") == 0UL) {
    SPDLOG_ERROR("Config file not specified");
    std::cout << options.help() << std::endl;
    return 1;
  }

  auto const &config_file_path = args["config"].as<std::string>();
  auto const &role_name = args["role"].as<std::string>();
  auto const &id = args["id"].as<std::string>();
  SPDLOG_INFO("Config file: {}", config_file_path);
  SPDLOG_INFO("Role: {}", role_name);
  SPDLOG_INFO("ID: {}", id);

  auto config = toml::parse_file(config_file_path);
  if (!config.contains(role_name)) {
    SPDLOG_ERROR(
        "Specified role \"{}\" but section not found in config file {}",
        role_name, config_file_path);
    return 1;
  }
  auto host = config[role_name]["host"].value<std::string_view>();
  uint16_t port = config[role_name]["port"].value_or(0);
  auto data_root_path =
      config[role_name]["data_root_path"].value<std::string_view>();
  auto const &available_core_ids = dfs::GetCpuAffinity();
  auto core_ids_string =
      config[role_name]["core_ids"].value<std::string_view>();
  int meta_id = config[role_name]["meta_id"].value_or(-1);

  if (!core_ids_string.has_value()) {
    SPDLOG_ERROR("Missing core_ids in config file");
    return 1;
  }
  auto const &core_ids = dfs::ParseCoreIds(core_ids_string.value());
  {
    std::set<size_t> available_core_ids_set(available_core_ids.begin(),
                                            available_core_ids.end());
    for (auto const &core_id : core_ids) {
      if (!available_core_ids_set.contains(core_id)) {
        SPDLOG_ERROR("Invalid core id: {}", core_id);
        return 1;
      }
    }
  }

  if (!host.has_value()) {
    SPDLOG_ERROR("Missing host in config file");
    return 1;
  }

  if (port == 0) {
    SPDLOG_ERROR("Missing port in config file");
    return 1;
  }

  if (!data_root_path.has_value()) {
    SPDLOG_ERROR("Missing data_root_path in config file");
    return 1;
  }

  if (role_name == "meta" && meta_id == -1) {
    SPDLOG_ERROR("Missing meta_id in config file");
    return 1;
  }

  std::string server_uri = fmt::format("{}:{}", host.value(), port);
  SPDLOG_INFO("server_uri: {}", server_uri);
  SPDLOG_INFO("Data root path: {}", data_root_path.value());

  // Default restricted to 31850-31882, change
  // dfs-prototype/third_party/erpc/src/rpc_constants.h
  // kMaxNumERpcProcesses = 32
  int numa_node = numa_node_of_cpu(core_ids[0]);
  auto nexus = new erpc::Nexus(server_uri, numa_node);
  int this_meta_num = meta_id;
  std::vector<std::string> meta_uri_list;
  std::vector<int> meta_rpc_num;

  if (role_name == "data") {
    AllocateHugepages(numa_node);
    gStoreManager = dfs::ObjectStore::Init(data_root_path.value());

    nexus->register_req_func(dfs::RPCType::kHelloReq, HelloReqHandler);
    nexus->register_req_func(dfs::RPCType::kDataReq, IoReqHandler);
  } else if (role_name == "meta") {
    int pos = config_file_path.find_last_of('-');
    if (pos != std::string::npos) {
      std::string sub_path = config_file_path.substr(pos + 1);
      int meta_num = std::stoi(sub_path.erase(sub_path.size() - 5));
      while (meta_num > 0) {
        meta_num--;
        std::string meta_config_file_path =
            config_file_path.substr(0, pos + 1) + std::to_string(meta_num) +
            ".toml";
        auto meta_config = toml::parse_file(meta_config_file_path);
        auto meta_host =
            meta_config[role_name]["host"].value<std::string_view>();
        uint16_t meta_port = meta_config[role_name]["port"].value_or(0);
        std::string meta_uri =
            fmt::format("{}:{}", meta_host.value(), meta_port);

        auto meta_core_ids_string =
            meta_config[role_name]["core_ids"].value<std::string_view>();

        auto const &meta_core_ids =
            dfs::ParseCoreIds(meta_core_ids_string.value());

        meta_uri_list.insert(meta_uri_list.begin(), meta_uri);
        // Each meta reserves the last core in core_ids for the CXL
        // persistence checkpoint thread — it does not run an eRPC Rpc
        // instance — so the number of registered RPC ids per meta is
        // core_ids.size() - 1 (Guardian + EventLoopThreads). See the
        // thread-spawn loop below that uses `i < core_ids.size() - 1`.
        meta_rpc_num.insert(meta_rpc_num.begin(), meta_core_ids.size() - 1);
      }
      meta_uri_list.push_back(server_uri);
      meta_rpc_num.push_back(core_ids.size() - 1);
    }
    AllocateHugepages(numa_node);

    dfs::InitDevice(this_meta_num, CXL_CAPACITY, CXL_PATH, CXL_NUMA_NODE,
                    GIM_CAPACITY, GIM_PATH, numa_node, CXLSSD_CAPACITY_MB,
                    CXLSSD_PATH);

    dfs::InitCXLPersistence(this_meta_num, core_ids[core_ids.size() - 1],
                            CXLSSD_CHECKPOINT_INTERVAL_MS);

    gMetaManager = dfs::Metadata::Init(data_root_path.value(), this_meta_num);
    gMetaManager->SetMetaNum(meta_uri_list.size());
    if (dfs::gCXLPersistence != nullptr) {
      dfs::gCXLPersistence->ConfigureClusterMetaCount(meta_uri_list.size());
    }

    nexus->register_req_func(dfs::RPCType::kHelloReq, HelloReqHandler);
    nexus->register_req_func(dfs::RPCType::kMetaGeneralReq,
                             MDGeneralReqHandler);
    nexus->register_req_func(dfs::RPCType::kMetaPathCommonReq,
                             MDPathCommonReqHandler);
    nexus->register_req_func(dfs::RPCType::kMetaFDCommonReq,
                             MDFDCommonReqHandler);

    nexus->register_req_func(dfs::RPCType::kMetaCommunicationReq,
                             MDMetaCommunicationReqHandler);
    nexus->register_req_func(dfs::RPCType::kGuardianCommunicationReq,
                             MDGuardianCommunicationReqHandler);
    nexus->register_req_func(dfs::RPCType::kTestMetaCommunicationReq,
                             MDTestCommunicationReqHandler);
    nexus->register_req_func(dfs::RPCType::kGuardianCommonReq,
                             MDGuardianCommonReqHandler);
    nexus->register_req_func(dfs::RPCType::kMetaPersistenceReq,
                             MDPersistenceReqHandler);
    nexus->register_req_func(dfs::RPCType::kMetaPersistenceReqSplit,
                             MDPersistenceReqSplitHandler);

    std::this_thread::sleep_for(std::chrono::milliseconds(300 * this_meta_num));
  } else {
    SPDLOG_ERROR("Unknown role: {}", role_name);
    return 1;
  }

  /* Each thread on the server side initializes an RPC (Remote Procedure Call)
     object and has a corresponding RPC ID. The client specifies the RPC ID,
     clearly indicating which RPC object (that is, which thread) on the server
     side this session is sending requests to. */
  std::vector<std::thread> threads;
  if (role_name == "data") {
    for (size_t i = 0; i < core_ids.size(); ++i) {
      threads.emplace_back(EventLoopThread, nexus, i, core_ids[i],
                           this_meta_num, meta_uri_list, meta_rpc_num);
    }
  } else if (role_name == "meta") {
    threads.emplace_back(GuardianThread, nexus, 0, core_ids[0], this_meta_num,
                         meta_uri_list, meta_rpc_num);
    for (size_t i = 1; i < core_ids.size() - 1; ++i) {
      threads.emplace_back(EventLoopThread, nexus, i, core_ids[i],
                           this_meta_num, meta_uri_list, meta_rpc_num);
    }
  }

  for (auto &thread : threads) {
    thread.join();
  }

  if (role_name == "meta") {
    dfs::DestroyCXLPersistence();
    dfs::DestroyDevice();
  }

  delete nexus;

  SPDLOG_INFO("Server exiting");
  return 0;
}

void HelloReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  DTRACE_PROBE(DFS_SERVER, HelloReqHandler_enter);
  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 16);
  memcpy(resp.buf_, "hello to client", 15);
  SPDLOG_TRACE("Got hello request from client");
  static_cast<dfs::ServerContext *>(_ctx)->rpc_->enqueue_response(req_handle,
                                                                  &resp);
  DTRACE_PROBE(DFS_SERVER, HelloReqHandler_ret);
}

void MDGeneralReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  SPDLOG_TRACE("Got GeneralReq");
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  using dfs::mdrequest::GetMDRequest;
  using dfs::mdrequest::MDOpType_Get;
  using dfs::mdrequest::MDOpType_Put;

  auto req = GetMDRequest(req_handle->get_req_msgbuf()->buf_);
  uint8_t op = req->op();
  uint64_t id = req->inode_id();
  MetadataOperationClockGuard clock_guard(req->last_seen_hlc());
  uint64_t server_hlc = 0;

  // Determine response size.
  auto &resp = req_handle->pre_resp_msgbuf_;
  uint32_t resp_size = sizeof(uint64_t) + 1;
  if (op == MDOpType_Get) {
    resp_size = sizeof(uint64_t) + 1 + sizeof(dfs::Inode);
  }
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, resp_size);

  // Execute metadata actions according to op and prepare response
  switch (op) {
  case MDOpType_Put:
    gMetaManager->UpdateInode(
        id, const_cast<dfs::Inode *>(
                reinterpret_cast<dfs::Inode const *>(req->inode()->data())));
    // gMetaManager->PutInode(
    //     id, const_cast<dfs::Inode *>(
    //             reinterpret_cast<dfs::Inode const *>(req->inode()->data())));
    resp.buf_[sizeof(uint64_t)] = 0;
    break;
  case MDOpType_Get:
    resp.buf_[sizeof(uint64_t)] =
        static_cast<int>(gMetaManager->GetInode(
            id,
            reinterpret_cast<dfs::Inode *>(resp.buf_ + sizeof(uint64_t) + 1))) -
        1;
    break;
  default:
    SPDLOG_ERROR("Invalid Operation: {}", static_cast<int8_t>(req->op()));
  }
  server_hlc = CurrentMetadataServerHLC();
  *reinterpret_cast<uint64_t *>(resp.buf_) = server_hlc;
  static_cast<dfs::ServerContext *>(_ctx)->rpc_->enqueue_response(req_handle,
                                                                  &resp);
  SPDLOG_TRACE("Sent Meta response");
}

void MDPathCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  using dfs::mdrequest::CreateDirent;
  using dfs::mdrequest::CreateInode;
  using dfs::mdrequest::CreateMDPathCommonResponse;
  using dfs::mdrequest::EnumNameMDOpType;
  using dfs::mdrequest::GetMDMoveNodeRequest;
  using dfs::mdrequest::GetMDPathCommonRequest;
  using dfs::mdrequest::MDOpType_Access;
  using dfs::mdrequest::MDOpType_Chmod;
  using dfs::mdrequest::MDOpType_Create;
  using dfs::mdrequest::MDOpType_Link;
  using dfs::mdrequest::MDOpType_MakeDir;
  using dfs::mdrequest::MDOpType_OpenDir;
  using dfs::mdrequest::MDOpType_RemoveDir;
  using dfs::mdrequest::MDOpType_Rename;
  using dfs::mdrequest::MDOpType_Stat;
  using dfs::mdrequest::MDOpType_Unlink;

  auto req = GetMDPathCommonRequest(req_handle->get_req_msgbuf()->buf_);
  MetadataOperationClockGuard clock_guard(req->last_seen_hlc());
  uint32_t uid = req->uid();
  uint32_t gid = req->gid();
  uint32_t mode = req->mode();
  dfs::mdrequest::MDOpType op = req->op();
  DTRACE_PROBE1(DFS_SERVER, MDPathCommonReqHandler_enter, op);
  const char *path = req->path()->c_str();
  int meta_num = req->meta_num();
  auto ctx = static_cast<dfs::ServerContext *>(_ctx);
  int rpc_id = ctx->rpc_id_;
  SPDLOG_TRACE("<{}:{}> {} for {}", ctx->rpc_id_,
               ctx->nr_reqs_[gMetaManager->meta_num_],
               EnumNameMDOpType(req->op()), path);

  int16_t flag;
  int16_t next_meta_server = -1;
  std::string old_path;
  std::string new_path;
  dfs::Inode inode;

  // For testing latency, mds early return.
  // flatbuffers::FlatBufferBuilder fbb2;
  // auto md_resp2 = CreateMDPathCommonResponse(
  //     fbb2, 0, ctx->nr_reqs_[gMetaManager->meta_num_], 0,
  //     fbb2.CreateString(old_path), fbb2.CreateString(new_path), -1);
  // fbb2.Finish(md_resp2);

  // auto &resp2 = req_handle->pre_resp_msgbuf_;
  // erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp2, fbb2.GetSize());
  // memcpy(resp2.buf_, fbb2.GetBufferPointer(), fbb2.GetSize());
  // ctx->rpc_->enqueue_response(req_handle, &resp2);
  // return;

  std::string tem_path;
  std::string tem_new_path;
  dfs::Result res;
  dfs::OpenDirResult dirres;

  // Execute metadata actions according to op and prepare response
  switch (op) {
  case MDOpType_Unlink: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpUnlink);
    res = gMetaManager->Unlink(path);
    flag = res.mark_;
    break;
  }
  case MDOpType_RemoveDir: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpRemoveDir);
    res = gMetaManager->Rmdir(path);
    flag = res.mark_;
    break;
  }
  case MDOpType_Access: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpAccess);
    res = gMetaManager->Access(path, mode, uid, gid);
    flag = res.mark_;
    break;
  }
  case MDOpType_MakeDir: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpMakeDir);
    res = gMetaManager->Mkdir(path, mode, uid, gid);
    flag = res.mark_;
    break;
  }
  case MDOpType_Stat: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpStat);
    res = gMetaManager->Stat(path, &inode);
    flag = res.mark_;
    break;
  }
  case MDOpType_Rename: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpRename);
    flag = gMetaManager->Rename(path, req->newpath()->c_str());
    break;
  }
  case MDOpType_Link: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpLink);
    flag = gMetaManager->Link(path, req->newpath()->c_str());
    break;
  }
  case MDOpType_Create: {
    MD_OP_PROFILE_SCOPE(dfs::kMDOpCreate);
    res = gMetaManager->Create(path, mode, uid, gid, &inode);
    flag = res.mark_;
    break;
  }
  case MDOpType_OpenDir:
    dirres = gMetaManager->OpenDir(path, mode);
    flag = dirres.res_.mark_;
    break;
  case MDOpType_Chmod:
    res = gMetaManager->Chmod(path, mode, uid, gid);
    flag = res.mark_;
    break;
  default:
    SPDLOG_ERROR("Invalid Operation: {}", static_cast<int8_t>(req->op()));
  }

  // -EAGAIN is WAL-full backpressure, not an error — let the client retry.
  if (flag != 0 && flag != -EAGAIN) {
    if (op == MDOpType_Rename && req->newpath() != nullptr) {
      SPDLOG_ERROR("<{}:{} flag = {}> FAILED {} for ({}) to ({})", ctx->rpc_id_,
                   ctx->nr_reqs_[gMetaManager->meta_num_], flag,
                   EnumNameMDOpType(req->op()), path, req->newpath()->c_str());
    } else {
      SPDLOG_ERROR("<{}:{} flag = {}> FAILED {} for ({})", ctx->rpc_id_,
                   ctx->nr_reqs_[gMetaManager->meta_num_], flag,
                   EnumNameMDOpType(req->op()), path);
    }
  }

  ctx->nr_reqs_[gMetaManager->meta_num_]++;
  uint64_t server_hlc = CurrentMetadataServerHLC();

  flatbuffers::FlatBufferBuilder fbb;
  auto md_resp = CreateMDPathCommonResponse(
      fbb, flag, ctx->nr_reqs_[gMetaManager->meta_num_], next_meta_server,
      fbb.CreateString(old_path), fbb.CreateString(new_path), -1, server_hlc);
  fbb.Finish(md_resp);

  int length = fbb.GetSize();
  auto fbb_ptr =
      flatbuffers::GetMutableRoot<dfs::mdrequest::MDPathCommonResponse>(
          fbb.GetBufferPointer());
  fbb_ptr->mutate_length(length);

  if (op == MDOpType_OpenDir) {
    auto &resp = req_handle->pre_resp_msgbuf_;
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp,
                                                   length + sizeof(DirHandle));
    DirHandle handle = dirres.handle_;
    handle.read_cutoff_version_ = server_hlc;
    memcpy(resp.buf_, fbb.GetBufferPointer(), length);
    memcpy(resp.buf_ + length, &handle, sizeof(DirHandle));
    ctx->rpc_->enqueue_response(req_handle, &resp);
  } else if (op == MDOpType_Stat || op == MDOpType_Create) {
    auto &resp = req_handle->pre_resp_msgbuf_;
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp,
                                                   length + sizeof(dfs::Inode));
    memcpy(resp.buf_, fbb.GetBufferPointer(), length);
    memcpy(resp.buf_ + length, &inode, sizeof(dfs::Inode));
    ctx->rpc_->enqueue_response(req_handle, &resp);
  } else {
    auto &resp = req_handle->pre_resp_msgbuf_;
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, length);
    memcpy(resp.buf_, fbb.GetBufferPointer(), length);
    ctx->rpc_->enqueue_response(req_handle, &resp);
  }

  DTRACE_PROBE(DFS_SERVER, MDPathCommonReqHandler_ret);
}

void MDFDCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  LISTDIR_PROFILE_SCOPE(dfs::kSrvFdHandlerTotal);
  SPDLOG_TRACE("Got FDCommonReq");
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  using dfs::mdrequest::GetMDFDCommonRequest;
  using dfs::mdrequest::MDOpType_GetDents;
  using dfs::mdrequest::MDOpType_GetDentViews;

  auto req = GetMDFDCommonRequest(req_handle->get_req_msgbuf()->buf_);
  MetadataOperationClockGuard clock_guard(req->last_seen_hlc());
  uint32_t uid = req->uid();
  uint32_t gid = req->gid();
  uint64_t id = req->id();
  uint8_t op = req->op();
  int64_t offset = req->offset();
  uint32_t u32arg = req->u32arg();
  uint64_t u64arg = req->u64arg();
  auto ctx = static_cast<dfs::ServerContext *>(_ctx);

  // Execute metadata actions according to op and prepare response
  char *buffer = nullptr;
  erpc::MsgBuffer *resp_ptr = nullptr;
  uint32_t read_size = 0;
  switch (op) {
  case dfs::mdrequest::MDOpType_GetDents: {
    {
      LISTDIR_PROFILE_SCOPE(dfs::kSrvAcquireBuffer);
      buffer = gMetaManager->AcquireBuffer();
    }
    read_size = gMetaManager->GetDents(id, reinterpret_cast<Dirent *>(buffer),
                                       u32arg, offset);
    break;
  }
  case dfs::mdrequest::MDOpType_GetDentViews: {
    {
      LISTDIR_PROFILE_SCOPE(dfs::kSrvAcquireBuffer);
      buffer = gMetaManager->AcquireBuffer();
    }
    read_size = gMetaManager->GetDentViews(
        id, u64arg, reinterpret_cast<DentView *>(buffer), u32arg, offset);
    break;
  }
  default:
    SPDLOG_ERROR("Invalid Operation: {}", static_cast<int8_t>(req->op()));
  }
  {
    LISTDIR_PROFILE_SCOPE(dfs::kSrvRpcResponseEncode);
    if (sizeof(uint64_t) + 1 + sizeof(uint32_t) + read_size <= 3824) {
      resp_ptr = &req_handle->pre_resp_msgbuf_;
      erpc::Rpc<erpc::CTransport>::resize_msg_buffer(
          resp_ptr, sizeof(uint64_t) + 1 + sizeof(uint32_t) + read_size);
    } else {
      resp_ptr = &req_handle->dyn_resp_msgbuf_;
      *resp_ptr = ctx->rpc_->alloc_msg_buffer_or_die(
          sizeof(uint64_t) + 1 + sizeof(uint32_t) + read_size);
    }
    *reinterpret_cast<uint64_t *>(resp_ptr->buf_) = CurrentMetadataServerHLC();
    resp_ptr->buf_[sizeof(uint64_t)] = 0;
    *reinterpret_cast<uint32_t *>(resp_ptr->buf_ + sizeof(uint64_t) + 1) =
        read_size;
    if (buffer != nullptr && read_size != 0) {
      memcpy(resp_ptr->buf_ + sizeof(uint64_t) + 1 + sizeof(uint32_t), buffer,
             read_size);
      gMetaManager->ReleaseBuffer(reinterpret_cast<char *>(buffer));
    } else if (buffer != nullptr) {
      gMetaManager->ReleaseBuffer(reinterpret_cast<char *>(buffer));
    }
    ctx->nr_reqs_[gMetaManager->meta_num_]++;
    ctx->rpc_->enqueue_response(req_handle, resp_ptr);
  }
  SPDLOG_TRACE("Sent Meta response");
}

void IoReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  if (gStoreManager == nullptr) {
    return;
  }
  auto *c = static_cast<dfs::ServerContext *>(_ctx);
  SPDLOG_TRACE("Got IO request");
  using dfs::data::GetDataRequest;
  auto request = GetDataRequest(req_handle->get_req_msgbuf()->buf_);
  const unsigned char *req_buffer = request->buffer()->data();
  const char *io_buffer = reinterpret_cast<const char *>(req_buffer);
  SPDLOG_TRACE("io_type: {}, objuuid: {}, offset: {}, io_size: {}",
               dfs::data::EnumNameRequestType(request->io_type()),
               request->objuuid(), request->offset(), request->size());
  DTRACE_PROBE1(DFS_SERVER, IoReqHandler_enter, request->io_type());
  switch (request->io_type()) {
  case dfs::data::RequestType::RequestType_Read: {
    uint64_t resp_buf_size = sizeof(ssize_t) + request->size();
    char *read_buffer = nullptr;
    erpc::MsgBuffer *resp_ptr = nullptr;
    if (resp_buf_size <= 3824) {
      // small io can avoid extra memory allocation
      resp_ptr = &req_handle->pre_resp_msgbuf_;
      erpc::Rpc<erpc::CTransport>::resize_msg_buffer(resp_ptr, resp_buf_size);
    } else {
      // large then 3824 will cause segfault, so we need to allocate new
      // buffer
      resp_ptr = &req_handle->dyn_resp_msgbuf_;
      *resp_ptr = c->rpc_->alloc_msg_buffer_or_die(resp_buf_size);
    }
    erpc::MsgBuffer &resp = *resp_ptr;
    read_buffer = reinterpret_cast<char *>(resp.buf_ + sizeof(ssize_t));
    ssize_t ret = gStoreManager->Read(request->objuuid(), request->offset(),
                                      request->size(), read_buffer);
    *reinterpret_cast<ssize_t *>(resp.buf_) = ret;
    SPDLOG_TRACE("Read ret = {}", ret);
    c->rpc_->enqueue_response(req_handle, &resp);
  } break;
  case dfs::data::RequestType::RequestType_Write: {
    erpc::MsgBuffer &resp = req_handle->pre_resp_msgbuf_;
    ssize_t ret = gStoreManager->Write(request->objuuid(), request->offset(),
                                       request->size(), io_buffer);
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, sizeof(ssize_t));
    *reinterpret_cast<ssize_t *>(resp.buf_) = ret;
    SPDLOG_TRACE("Write ret = {}", ret);
    c->rpc_->enqueue_response(req_handle, &resp);
  } break;
  default: {
    erpc::MsgBuffer &resp = req_handle->pre_resp_msgbuf_;
    erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 1);
    resp.buf_[0] = -1;
    SPDLOG_TRACE("ret = {}", -1);
    c->rpc_->enqueue_response(req_handle, &resp);
    SPDLOG_ERROR("Invalid IO type {}", static_cast<int8_t>(request->io_type()));
    break;
  }
  }
  SPDLOG_TRACE("Sent Data response");
  DTRACE_PROBE(DFS_SERVER, IoReqHandler_ret);
}

void MDMetaCommunicationReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  DTRACE_PROBE(DFS_SERVER, MDCommunicationReqHandler_enter);
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  const char *data =
      reinterpret_cast<const char *>(req_handle->get_req_msgbuf()->buf_);
  int size = req_handle->get_req_msgbuf()->get_data_size();
  auto ctx = static_cast<dfs::ServerContext *>(_ctx);

  std::istringstream iss(std::string(data, size));
  std::string line;
  std::getline(iss, line);
  int meta_nums = std::stoi(line);
  if (meta_nums == 1) {
    SPDLOG_ERROR("Connection error! The meta that is started later should "
                 "establish a connection with the meta that is started first");
    return;
  }
  if (meta_nums > ctx->meta_uri_list_.size()) {
    ctx->meta_rpc_num_.resize(meta_nums);
    ctx->meta_uri_list_.resize(meta_nums);
    ctx->meta_to_meta_sessions_.resize(meta_nums);
    ctx->nr_reqs_.resize(meta_nums);
    for (auto &nr_req : ctx->nr_reqs_) {
      nr_req = 0;
    }

    for (int i = 0; i < meta_nums; i++) {
      std::getline(iss, line);
      ctx->meta_uri_list_[i] = line;
    }
    for (int i = 0; i < meta_nums; i++) {
      std::getline(iss, line);
      ctx->meta_rpc_num_[i] = std::stoi(line);
    }
  }

  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 1);
  resp.buf_[0] = 0;
  ctx->rpc_->enqueue_response(req_handle, &resp);
  ctx->rpc_->run_event_loop_once();

  // Establish connections in sequence
  int sum = 0;
  for (int j = 0; j < meta_nums - 1; j++) {
    if (j != ctx->meta_num_) {
      sum += ctx->meta_rpc_num_[j] * 2 - 1;
    }
  }
  sum += ctx->meta_rpc_num_[meta_nums - 1];
  while (gConnectedSessionNum.load() != ctx->rpc_id_ + sum - 1) {
  }
  int remote_rpc_id =
      (ctx->rpc_id_ - 1) % (ctx->meta_rpc_num_[meta_nums - 1] - 1) + 1;
  int session_num = ctx->rpc_->create_session(
      ctx->meta_uri_list_[meta_nums - 1], remote_rpc_id);
  erpc::rt_assert(session_num >= 0, "Failed to create session");
  while (!ctx->rpc_->is_connected(session_num)) {
    ctx->rpc_->run_event_loop_once();
  }
  ctx->meta_to_meta_sessions_[meta_nums - 1] = session_num;

  SPDLOG_INFO("This is meta {}, Connecting from {} to {}, "
              "rpc_id {}, session_num {}",
              ctx->meta_num_, ctx->meta_uri_list_[ctx->meta_num_],
              ctx->meta_uri_list_[meta_nums - 1], remote_rpc_id, session_num);

  DTRACE_PROBE(DFS_SERVER, MDCommunicationReqHandler_ret);
}

void MDGuardianCommunicationReqHandler(erpc::ReqHandle *req_handle,
                                       void *_ctx) {
  DTRACE_PROBE(DFS_SERVER, MDGuardianCommunicationReqHandler_enter);
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  const char *data =
      reinterpret_cast<const char *>(req_handle->get_req_msgbuf()->buf_);
  int size = req_handle->get_req_msgbuf()->get_data_size();
  auto ctx = static_cast<dfs::GuardianContext *>(_ctx);

  std::istringstream iss(std::string(data, size));
  std::string line;
  std::getline(iss, line);
  int meta_nums = std::stoi(line);
  if (meta_nums == 1) {
    SPDLOG_ERROR("Connection error! The guardian that is started later "
                 "should establish "
                 "a connection with the guardian that is started first");
    return;
  }
  if (meta_nums > ctx->meta_uri_list_.size()) {
    ctx->meta_rpc_num_.resize(meta_nums);
    ctx->meta_uri_list_.resize(meta_nums);
    ctx->meta_to_meta_sessions_.resize(meta_nums);
    ctx->reqs_.resize(meta_nums);
    gReqs.resize(meta_nums);
    gMetaManager->SetMetaNum(meta_nums);
    if (dfs::gCXLPersistence != nullptr) {
      dfs::gCXLPersistence->ConfigureClusterMetaCount(meta_nums);
    }
    for (auto &nr_req : ctx->reqs_) {
      nr_req = 0;
    }
    for (auto &nr_req : gReqs) {
      nr_req = 0;
    }

    // ctx->req_msgbufs_.resize(meta_nums);
    // ctx->resp_msgbufs_.resize(meta_nums);

    for (int i = 0; i < meta_nums; i++) {
      std::getline(iss, line);
      ctx->meta_uri_list_[i] = line;
    }
    for (int i = 0; i < meta_nums; i++) {
      std::getline(iss, line);
      ctx->meta_rpc_num_[i] = std::stoi(line);
      ctx->meta_to_meta_sessions_[i].resize(ctx->meta_rpc_num_[i]);
    }
  }

  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 1);
  resp.buf_[0] = 0;
  ctx->rpc_->enqueue_response(req_handle, &resp);
  ctx->rpc_->run_event_loop_once();

  for (int j = 0; j < ctx->meta_rpc_num_[meta_nums - 1]; j++) {
    // Establish connections in sequence
    int sum = 0;
    for (int k = 0; k < meta_nums - 1; k++) {
      if (k != ctx->meta_num_) {
        sum += ctx->meta_rpc_num_[k] * 2 - 1;
      }
    }
    while (gConnectedSessionNum.load() != ctx->rpc_id_ + sum + j) {
    }

    int session_num =
        ctx->rpc_->create_session(ctx->meta_uri_list_[meta_nums - 1], j);
    erpc::rt_assert(session_num >= 0, "Failed to create session");
    while (!ctx->rpc_->is_connected(session_num)) {
      ctx->rpc_->run_event_loop_once();
    }
    ctx->meta_to_meta_sessions_[meta_nums - 1][j] = session_num;

    SPDLOG_INFO("This is guardian {}, Connecting from {} to {}, "
                "rpc_id = {}, session_num {}",
                ctx->meta_num_, ctx->meta_uri_list_[ctx->meta_num_],
                ctx->meta_uri_list_[meta_nums - 1], j, session_num);
  }

  DTRACE_PROBE(DFS_SERVER, MDCommunicationReqHandler_ret);
}

void MDTestCommunicationReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  DTRACE_PROBE(DFS_SERVER, MDTestCommunicationReqHandler_enter);
  auto ctx = static_cast<dfs::ServerContext *>(_ctx);

  for (int i = 0; i < ctx->meta_uri_list_.size(); i++) {
    if (i != ctx->meta_num_) {
      SPDLOG_INFO("Test connecting from {} to meta server {}, rpc_id {}, "
                  "session_num {}",
                  ctx->meta_uri_list_[ctx->meta_num_], ctx->meta_uri_list_[i],
                  ctx->rpc_id_, ctx->meta_to_meta_sessions_[i]);
      auto hello_req = ctx->rpc_->alloc_msg_buffer_or_die(16);
      auto hello_resp = ctx->rpc_->alloc_msg_buffer_or_die(16);
      memcpy(hello_req.buf_, "hello to server", 15);
      ContContextT cc = {0, reinterpret_cast<char *>(hello_resp.buf_)};
      ctx->rpc_->enqueue_request(ctx->meta_to_meta_sessions_[i],
                                 dfs::RPCType::kHelloReq, &hello_req,
                                 &hello_resp, ContFunc, &cc);
      while (cc.finished_ == 0) {
        ctx->rpc_->run_event_loop_once();
      }
      ctx->rpc_->free_msg_buffer(hello_req);
      ctx->rpc_->free_msg_buffer(hello_resp);
    }
  }

  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 1);
  memcpy(resp.buf_, "0", 1);
  ctx->rpc_->enqueue_response(req_handle, &resp);

  DTRACE_PROBE(DFS_SERVER, MDTestCommunicationReqHandler_ret);
}

void MDGuardianCommonReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  DTRACE_PROBE(DFS_SERVER, MDGuardianCommonReqHandler_enter);
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  using dfs::mdrequest::CreateMDGuardianHeartRequest;
  using dfs::mdrequest::GetMDGuardianCommonRequest;
  auto ctx = static_cast<dfs::GuardianContext *>(_ctx);

  auto req = GetMDGuardianCommonRequest(req_handle->get_req_msgbuf()->buf_);
  ctx->heart_num_ |= 1 << (req->meta_num());
  SPDLOG_INFO("Get heart from guardian {}, reqs = {}", req->meta_num(),
              req->reqs()->Get(req->meta_num()));

  flatbuffers::FlatBufferBuilder builder;
  clock_gettime(CLOCK_MONOTONIC, &gEndTime);
  if ((ctx->heart_num_ & (1 << ctx->meta_num_)) == 0) {
    CollectReqs(*ctx);
  }
  for (int i = 0; i < req->reqs()->size(); i++) {
    ctx->reqs_[i] += req->reqs()->Get(i);
  }

  auto resp_time = CreateMDGuardianHeartRequest(builder, gStartTime.tv_sec,
                                                gStartTime.tv_nsec);
  builder.Finish(resp_time);

  // Determine response size.
  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, 1 + builder.GetSize());
  resp.buf_[0] = 0;
  memcpy(resp.buf_ + 1, builder.GetBufferPointer(), builder.GetSize());
  ctx->rpc_->enqueue_response(req_handle, &resp);

  int mask = (1 << ctx->meta_uri_list_.size()) - 1;
  if ((ctx->heart_num_ & mask) == mask) {
    ctx->heart_num_.store(0);
  }
  DTRACE_PROBE(DFS_SERVER, MDGuardianCommonReqHandler_ret);
}

static constexpr uint32_t kMDPersistenceFatal = 0xFFFFFFFFU;

void MDPersistenceReqHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  using dfs::mdrequest::GetMDPersistenceRequest;

  auto req = GetMDPersistenceRequest(req_handle->get_req_msgbuf()->buf_);
  auto inode_change_op = req->inode_change_op();
  auto inode_change_id = req->inode_change_id();
  auto inode_change_value = req->inode_change_value();
  auto inode_change_version = req->inode_change_version();

  uint32_t inserted = 0;
  if (inode_change_id != nullptr && inode_change_id->size() != 0 &&
      dfs::gCXLPersistence != nullptr) {
    std::vector<dfs::RemoteInodeChange> changes;
    changes.reserve(inode_change_id->size());
    for (size_t i = 0; i < inode_change_id->size(); ++i) {
      dfs::RemoteInodeChange change;
      change.op_ =
          static_cast<dfs::RemoteInodeChangeOp>(inode_change_op->Get(i));
      change.inode_id_ = inode_change_id->Get(i);
      change.value_ = inode_change_value->Get(i);
      change.version_ = inode_change_version->Get(i);
      changes.push_back(change);
    }
    inserted = static_cast<uint32_t>(
        dfs::gCXLPersistence->TryAppendReceivedRemoteInodeChanges(changes));
  }

  auto ctx = static_cast<dfs::ServerContext *>(_ctx);
  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, sizeof(uint32_t));
  std::memcpy(resp.buf_, &inserted, sizeof(uint32_t));
  ctx->rpc_->enqueue_response(req_handle, &resp);
}

void MDPersistenceReqSplitHandler(erpc::ReqHandle *req_handle, void *_ctx) {
  if (gMetaManager == nullptr) {
    SPDLOG_ERROR("Meta manager is not initialized");
    return;
  }

  auto ctx = static_cast<dfs::ServerContext *>(_ctx);
  auto &resp = req_handle->pre_resp_msgbuf_;
  erpc::Rpc<erpc::CTransport>::resize_msg_buffer(&resp, sizeof(uint32_t));

  auto write_resp = [&](uint32_t v) {
    std::memcpy(resp.buf_, &v, sizeof(uint32_t));
    ctx->rpc_->enqueue_response(req_handle, &resp);
  };

  static thread_local std::unordered_map<int, std::vector<uint8_t>> split_accum;

  const uint8_t *buf = req_handle->get_req_msgbuf()->buf_;
  size_t total = req_handle->get_req_msgbuf()->get_data_size();
  if (total < 5) {
    SPDLOG_ERROR("MDPersistenceReqSplitHandler: short chunk ({} bytes)", total);
    write_resp(kMDPersistenceFatal);
    return;
  }

  uint8_t terminator = buf[0];
  int sender_meta = 0;
  std::memcpy(&sender_meta, buf + 1, sizeof(int));
  const uint8_t *payload = buf + 5;
  size_t payload_len = total - 5;

  auto &sbuf = split_accum[sender_meta];
  sbuf.insert(sbuf.end(), payload, payload + payload_len);

  if (terminator != 1) {
    write_resp(0);
    return;
  }

  uint32_t inserted = 0;
  using dfs::mdrequest::GetMDPersistenceRequest;
  auto req = GetMDPersistenceRequest(sbuf.data());
  auto inode_change_op = req->inode_change_op();
  auto inode_change_id = req->inode_change_id();
  auto inode_change_value = req->inode_change_value();
  auto inode_change_version = req->inode_change_version();

  if (inode_change_id != nullptr && inode_change_id->size() != 0 &&
      dfs::gCXLPersistence != nullptr) {
    std::vector<dfs::RemoteInodeChange> changes;
    changes.reserve(inode_change_id->size());
    for (size_t i = 0; i < inode_change_id->size(); ++i) {
      dfs::RemoteInodeChange change;
      change.op_ =
          static_cast<dfs::RemoteInodeChangeOp>(inode_change_op->Get(i));
      change.inode_id_ = inode_change_id->Get(i);
      change.value_ = inode_change_value->Get(i);
      change.version_ = inode_change_version->Get(i);
      changes.push_back(change);
    }
    inserted = static_cast<uint32_t>(
        dfs::gCXLPersistence->TryAppendReceivedRemoteInodeChanges(changes));
  }
  sbuf.clear();
  write_resp(inserted);
}
