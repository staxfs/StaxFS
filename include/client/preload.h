#pragma once

#include "client/file_descriptor.h"
#include "client/rpc_client.h"
#include <fcntl.h>
#include <memory>

namespace dfs {

class PreloadContext {
public:
  std::unique_ptr<dfs::ClientRpcWrapper> rpc_client_;
  std::unique_ptr<dfs::FdManager> fd_manager_;

  explicit PreloadContext(SharedContext &shared_ctx);
  ~PreloadContext();
};

} // namespace dfs