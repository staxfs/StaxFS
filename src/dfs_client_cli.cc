#include "client/g_preload.h"
#include "spdlog/spdlog.h"

auto main() -> int {
  InitPreload();
  SPDLOG_INFO("Hello, DFS Client Test!");
  auto rpc_client = dfs::gPreloadCtx->rpc_client_.get();
  int nr_dataserver = rpc_client->GetDataserverCount();
  int nr_metaserver = 1;
  SPDLOG_INFO("nr_dataserver: {}", nr_dataserver);
  SPDLOG_INFO("nr_metaserver: {}", nr_metaserver);

  // Note: can use for benchmark the rpc latency
  bool need_rpc_bench = true;
  if (need_rpc_bench) {
    for (int i = 0; i < nr_dataserver; i++) {
      for (int j = 0; j < 1; j++) {
        // for (int j = 0; j < rpc_client_->GetDataserverThreadCount(i); j++){
        int warm_count = 1000;
        for (int k = 0; k < warm_count; k++) {
          rpc_client->RpcHelloDataServer(i, j);
        }
        int test_count = 5000;
        auto start_time = std::chrono::high_resolution_clock::now();
        for (int k = 0; k < test_count; k++) {
          rpc_client->RpcHelloDataServer(i, j);
        }
        auto end_time = std::chrono::high_resolution_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time);
        SPDLOG_INFO("Data Server {} Thread {} RPC Latency: {} ns", i, j,
                    duration.count() / (double)test_count);
      }
    }
    {
      // meta
      int warm_count = 1000;
      for (int k = 0; k < warm_count; k++) {
        rpc_client->RpcHelloMetaServer(0);
      }
      int test_count = 5000;
      auto start_time = std::chrono::high_resolution_clock::now();
      for (int k = 0; k < test_count; k++) {
        rpc_client->RpcHelloMetaServer(0);
      }
      auto end_time = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
          end_time - start_time);
      SPDLOG_INFO("Meta Server {} Thread {} RPC Latency: {} ns", 0, 0,
                  duration.count() / (double)test_count);
    }
  }

  DestroyPreload();
  return 0;
}