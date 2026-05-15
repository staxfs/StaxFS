# DFS-Prototype 代码文档

## 项目概述

DFS-Prototype 是一个面向 CXL 内存语义的分布式文件系统原型，采用 C++20 编写。系统使用 eRPC（InfiniBand/RoCE）做高性能 RPC，FlatBuffers 做请求/响应序列化，spdlog 做日志记录；元数据持久化层基于自实现的 CXL 内存子系统 + CXL-SSD 模拟器，并附带紧凑 WAL（CompactWAL）和跨 MDS 的远程 inode 变更通道。

### 系统架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        应用程序 (POSIX API)                          │
├─────────────────────────────────────────────────────────────────────┤
│                  hook.c (LD_PRELOAD 拦截层)                          │
│          路径判断: /dfs/* → DFS | 其他 → libc                        │
├─────────────────────────────────────────────────────────────────────┤
│              posix_wrapper.cc (POSIX → RPC 转换)                     │
│       FdManager | gLockTable | DentView 合并 | DIR* LSB 编码          │
├──────────────────────┬──────────────────────────────────────────────┤
│   rpc_client.cc      │         eRPC (InfiniBand/RoCE)               │
│   会话池             │         FlatBuffers 序列化                    │
│   path_cache         │         RDMA 传输                             │
│   HLC 跟踪           │                                              │
├──────────────────────┴──────────────────────────────────────────────┤
│   ┌────────────────────────────┐   ┌────────────────────────────┐   │
│   │ 元数据服务器 (meta)         │   │ 数据服务器 (data)           │   │
│   │ src/server/metadata.cc     │   │ src/server/data.cc         │   │
│   │ ┌─ ihashtable_ ─┐          │   │ ObjectStore                │   │
│   │ │ TwoLevelHash /│          │   │ libcuckoo::cuckoohash_map  │   │
│   │ │ InodeArray    │          │   │                            │   │
│   │ └───────────────┘          │   └────────────────────────────┘   │
│   │ ┌─ dhashtable_ ─┐          │                                    │
│   │ │ TwoLevelHash  │          │                                    │
│   │ └───────────────┘          │                                    │
│   │ pending_listings_          │                                    │
│   │                            │                                    │
│   │ ┌─ CXLPersistence ───────┐ │                                    │
│   │ │ CompactWAL (主 WAL)    │ │                                    │
│   │ │ RemoteInodeWAL         │ │                                    │
│   │ │ SSDInodeRegion (页缓存)│ │                                    │
│   │ │ SSDDentRegion (追加页) │ │                                    │
│   │ │ Checkpoint 线程        │ │                                    │
│   │ └────────────────────────┘ │                                    │
│   └──────────────┬─────────────┘                                    │
│                  │                                                   │
│   ┌──────────────▼─────────────┐                                    │
│   │ CXL 内存子系统 (gDevice)    │                                    │
│   │ CXLMem  (NUMA 4, 600ns)    │                                    │
│   │ GIMMem  (本地 / 远程 RDMA) │                                    │
│   │ CXLSSD  (DRAM cache + 闪存)│                                    │
│   │ DFSHeader (跨 meta 协调)    │                                    │
│   └────────────────────────────┘                                    │
└─────────────────────────────────────────────────────────────────────┘
```

### 请求处理流程（读路径）

```
应用程序 POSIX 调用：open("/dfs/file.txt")
    │
    ▼
hook.c: is_mount_path("/dfs/...") → dfs_open("/file.txt")
    │
    ▼
posix_wrapper.cc: dfs_open()
    │  ├─ 无 O_CREAT → ClientRpcWrapper::Stat()
    │  └─ 有 O_CREAT → ClientRpcWrapper::Create()
    │
    ▼
rpc_client.cc: 构建 MDPathCommonRequest (FlatBuffers)
    │  携带 last_seen_hlc → eRPC enqueue_request → spin event_loop
    │
    ▼
server_main.cc: MDPathCommonReqHandler 分发
    │
    ▼
src/server/metadata.cc: Stat() → LocateInode() → ihashtable_->find()
    │
    ▼
响应填充 server_hlc → 客户端 ObserveServerHLC()
    │
    ▼
posix_wrapper.cc: AllocateFd(inode) → DFS fd (≥ 0x0fff0000) → 返回应用
```

### 请求处理流程（写路径，含持久化与跨 MDS 同步）

```
Create("/file.txt", mode)
    │
    ▼
CXLPersistence::LogCreate(...)
    │  └─ CompactWAL::Append(64B 紧凑 entry, 含 HLC)
    │
    ▼
metadata.cc: AllocateInodeId() → ihashtable_->insert(inode)
    │       └─ AddDirectoryEntry → dhashtable_->insert(dirent)
    │
    ▼ ── 立即返回客户端 (ack 不等持久化) ──
    │
    │   ┌──────────── checkpoint 线程 (异步) ────────────┐
    │   │ DoCheckpoint:                                  │
    │   │  1. 扫主 WAL [ckpt_pos, head)                  │
    │   │  2. apply 到 SSDInodeRegion / SSDDentRegion    │
    │   │  3. 跨 MDS 的变更 → RemoteInodeWAL              │
    │   │  4. 到达批阈值或周期到 → FlushDirty + fdatasync │
    │   │  5. AdvanceCheckpoint()                        │
    │   │  6. ServicePendingRemoteInodeChanges          │
    │   │      （转发到目标 MDS, kMetaPersistenceReq）    │
    │   └────────────────────────────────────────────────┘
```

### 源码统计

- 头文件: 39 (include/)
- 源文件: 23 (src/, 含 hook.c)
- 测试与基准: 11 (test/) + 2 (benchmark/)
- Proto schema: 10 (proto/, FlatBuffers)
- 主要可执行文件: `dfs-prototype-server`, `dfs-prototype-client`, `dfs-client-cli`, `dfs-hook` (so)

### 文档索引

| 文档 | 内容 |
|------|------|
| [01-common.md](01-common.md) | 公共头文件：核心类型、RPC 枚举、配置/日志、profile 工具 |
| [02-cxl.md](02-cxl.md) | CXL 内存子系统：CXLMem、GIMMem、CXLSSD、CXLDevice |
| [03-server.md](03-server.md) | 服务端：元数据管理、哈希表三选一、CXLPersistence、跨 MDS 同步 |
| [04-client.md](04-client.md) | 客户端：POSIX 拦截、RPC 客户端、HLC、DentView 合并 |
| [05-proto-build-test.md](05-proto-build-test.md) | Proto 定义、构建系统、线程亲和、测试与脚本 |
| [06-config.md](06-config.md) | 配置文件格式、目录命名、部署方式 |
| [07-cxl-bandwidth-benchmark.md](07-cxl-bandwidth-benchmark.md) | CXL 带宽实测数据 |

### 编译开关总览（`include/common/metadata_types.h`）

| 开关 | 默认 | 说明 |
|------|------|------|
| `USING_CXL_OFFSET` | 启用 | 启用 CXL 内存（基于偏移寻址） |
| `USING_CXL_PERSISTENCE` | 启用 | 启用 CXLPersistence（CompactWAL + SSD region + checkpoint） |
| `USING_LEVEL_HASHTABLE` | 启用 | 使用 TwoLevelHashtable（默认 inode/dirent 容器） |
| `USING_LEVEL_HASHTABLE_BASELINE` | 关闭 | 改用 paper-faithful ConcurrentLevelHashtable（stop-the-world resize, 4 槽/桶, 仅基准对比用） |
| `LEVEL_HASHTABLE_TAG_HINT` | 启用 | 本地 DRAM TagHint 加速 TwoLevelHashtable find() |
| `USING_INODE_ARRAY` | 启用 | inode 改用 InodeArray 直接映射存储（dirent 仍走哈希表） |
| `LEVEL_HASHTABLE_STATS` | 关闭 | TwoLevelHashtable find()-path 计数器（配合 `scripts/analyze_hint.py`） |
| `LISTDIR_LATENCY_PROFILE` | 关闭 | listdir 热路径分阶段延迟统计 |
| `MD_OP_LATENCY_PROFILE` | 关闭 | 8 个路径级元数据 op 的 count/avg/p50/p99/p999/p9999/max 直方图 |
| `HASHTABLE_LATENCY_PROFILE` | 关闭 | 哈希表逐操作延迟 |
| `CHECKPOINT_STATS_PROFILE` | 关闭 | CXLPersistence 每次 sync 的滚动窗口（每 32 次打印一次） |
| `CACHE_SKIP` | 关闭 | 历史遗留，当前未使用 |
| `HASHTABLE_BUCKET_NUM` | `1ULL << 18` | 哈希表 BL 桶数；总容量 ≈ BUCKET_NUM × 8 × 1.5 |

### 关键常量（`include/common/metadata_types.h`）

| 常量 | 值 | 说明 |
|------|-----|------|
| `kMaxFilenameLen` | 63 | 最大文件名长度（不含 null） |
| `kRootId` | 2 | 根目录 inode id |
| `DIR_MAX_ALLOC` | `1 MiB` | 单次目录分配上限 |
| `HUGEPAGES` | 1024 | eRPC 大页池预留页数 |
| `INODE_ID_RANGE` | 50 | 每个 meta 拥有 `[i << 50, (i+1) << 50)` 的 inode id 域 |
| `CXL_CAPACITY` | 20 GB | CXL 扩展内存容量 |
| `CXL_NUMA_NODE` | 4 | CXL 内存所在 NUMA node |
| `GIM_CAPACITY` | 128 MB | 每 meta 的 GIM 本地 hugepage 容量 |
| `CXLSSD_CAPACITY_MB` | 1024 | CXL-SSD DRAM 缓存总容量 |
| `CXLSSD_INODE_REGION_GB` | 16 | 每 meta 的 inode 闪存区大小 |
| `CXLSSD_DENT_REGION_GB` | 16 | 每 meta 的 dirent 闪存区大小 |
| `CXLSSD_CHECKPOINT_INTERVAL_MS` | 1 | checkpoint 线程的空闲休眠时长 |
| `CXLSSD_WAL_ENTRIES` | `1 << 21` (= 2M, 128 MB) | CompactWAL 槽位数 |
| `CXLSSD_REMOTE_INODE_WAL_ENTRIES` | `1 << 21` (= 2M, 128 MB) | RemoteInodeWAL 槽位数 |
| `CXL_PATH` | `/dev/hugepages/cxl_memory` | CXL hugepage 后端文件 |
| `GIM_PATH` | `/dev/hugepages/gim_memory` | GIM hugepage 后端文件 |
| `CXLSSD_PATH` | `/sharenvme/usershome/hyx/dfs_ssd` | CXL-SSD 闪存后端目录 |

### RPC 类型与元数据 op

完整 RPCType 与 MDOpType 见 [01-common.md](01-common.md)。简表：

- 客户端→服务端核心 op：`kHelloReq` / `kMetaGeneralReq` (Get/Put inode) / `kMetaPathCommonReq` (POSIX 路径 op) / `kMetaFDCommonReq` (GetDents/GetDentViews) / `kDataReq` (块读写)
- 服务端内部：`kMetaCommunicationReq` / `kGuardianCommunicationReq` / `kGuardianCommonReq` / `kLoadBalanceReq`
- CXL 持久化跨 MDS 转发：`kMetaPersistenceReq` (=14) / `kMetaPersistenceReqSplit` (=15)
