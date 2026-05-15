#pragma once

#include "client/preload.h"

namespace dfs {
extern thread_local std::unique_ptr<dfs::PreloadContext> gPreloadCtx;
extern std::shared_ptr<SharedContext> gSharedCtx;
} // namespace dfs

extern "C" {
void InitPreload();
void DestroyPreload();
}
