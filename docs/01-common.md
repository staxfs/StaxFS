# 公共头文件

`include/common/` 与 `include/version.h` 集中放置全工程都要用到的核心类型、RPC 类型枚举、配置/日志接口、profile 工具，以及编译期开关常量。本文档逐个文件梳理当前实现。

---

## include/common/metadata_types.h

整个工程最重要的"配置 + 数据契约"头文件。集中编译期开关、容量常量、CXL/SSD 后端路径，以及 `Inode`、`Dirent`、`DentView`、`DirHandle`、`RemoteInodeChange` 等核心结构。

### 1. 编译开关

| 开关 | 默认 | 说明 |
|------|------|------|
| `USING_CXL_OFFSET` | 启用 | 启用 CXL 内存（基于偏移寻址访问 hashtable / WAL 等结构） |
| `USING_CXL_PERSISTENCE` | 启用 | 启用 CXLPersistence（CompactWAL + SSD region + checkpoint） |
| `USING_LEVEL_HASHTABLE` | 启用 | 使用 TwoLevelHashtable（默认 inode/dirent 容器） |
| `USING_LEVEL_HASHTABLE_BASELINE` | 关闭 | 改用 ConcurrentLevelHashtable 基准实现（4 槽/桶、stop-the-world resize、无 TagHint），仅用于对比基准 |
| `LEVEL_HASHTABLE_TAG_HINT` | 启用 | 本地 DRAM TagHint 加速 TwoLevelHashtable `find()` 路径 |
| `USING_INODE_ARRAY` | 启用 | inode 改用 InodeArray 直接映射存储（dirent 仍走哈希表） |
| `LEVEL_HASHTABLE_STATS` | 关闭 | TwoLevelHashtable find()-path 计数器（配合 `scripts/analyze_hint.py`） |
| `LISTDIR_LATENCY_PROFILE` | 关闭 | listdir 热路径分阶段延迟统计（见下文 listdir_profile.h） |
| `MD_OP_LATENCY_PROFILE` | 关闭 | 8 个路径级元数据 op 的 count/avg/p50/p99/p999/p9999/max 直方图（见下文 metadata_op_profile.h） |
| `HASHTABLE_LATENCY_PROFILE` | 关闭 | 哈希表逐操作延迟（与 `LEVEL_HASHTABLE_STATS` 配套） |
| `CHECKPOINT_STATS_PROFILE` | 关闭 | CXLPersistence 每次 sync 的滚动窗口（每 32 次打印一次） |
| `CACHE_SKIP` | 关闭 | 历史遗留，当前未使用 |
| `HASHTABLE_BUCKET_NUM` | `1ULL << 18` | 哈希表 BL 桶数；总容量 ≈ BUCKET_NUM × 8 × 1.5 |

### 2. 关键常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `kMaxFilenameLen` | 63 | 最大文件名长度（不含 null）|
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
| `CXLSSD_NUMA_NODE` | = `CXL_NUMA_NODE` | CXL-SSD DRAM cache 所在 NUMA node |
| `CXLSSD_CHECKPOINT_INTERVAL_MS` | 1 | checkpoint 线程的空闲休眠时长 |
| `CXLSSD_WAL_ENTRIES` | `1 << 21` (2M, 128 MB) | CompactWAL 槽位数 |
| `CXLSSD_REMOTE_INODE_WAL_ENTRIES` | `1 << 21` (2M, 128 MB) | RemoteInodeWAL 槽位数 |
| `CXL_PATH` | `/dev/hugepages/cxl_memory` | CXL hugepage 后端文件 |
| `GIM_PATH` | `/dev/hugepages/gim_memory` | GIM hugepage 后端文件 |
| `CXLSSD_PATH` | `/sharenvme/usershome/hyx/dfs_ssd` | CXL-SSD 闪存后端目录（`Meta<N>_Inode` / `Meta<N>_Dent`） |

### 3. RemoteInodeChange & RemoteInodeChangeOp

跨 MDS 的 inode 变更条目。被 CompactWAL 主写入端转化生成，再走 RemoteInodeWAL 投递给目标 MDS。

```cpp
enum RemoteInodeChangeOp : uint8_t {
  kRemoteInodeNoop = 0,
  kRemoteInodeUnlink = 1,         // nlink--；可合并：取最新 version
  kRemoteInodeLink   = 2,         // nlink++；可合并：取最新 version
  kRemoteInodeSetattr= 3,         // mode 改写；latest-wins
  kRemoteInodeTouchCtime = 4,     // 仅 touch ctime；latest-wins
  kRemoteParentBlocksDelta = 5,   // parent.extra_ 增量；可加性合并（和值，取最大 version）
};

struct RemoteInodeChange {
  RemoteInodeChangeOp op_ = kRemoteInodeNoop;
  uint64_t inode_id_ = 0;
  int64_t  value_    = 0;     // op 相关：nlink delta / mode / ctime / blocks delta
  uint64_t version_  = 0;     // 来自 CompactWAL 的 HLC version
};
```

> 在 `CXLPersistence::DoCheckpoint` 内部按 `(inode_id << 4) | op_` 做合并，详见 [03-server.md](03-server.md)。

### 4. ObjectLocation / ObjectUuid

```cpp
struct ObjectLocation {
  uint64_t node_id_ : 48;
  uint64_t disk_id_ : 16;
};
union ObjectUuid {
  ObjectLocation location_;
  uint64_t       reserved_;
};
```

数据对象的全局位置标识（48 位 node + 16 位 disk）。当前 ObjectStore 只用 `reserved_` 当 `blkuuid_t`。

### 5. Inode（核心）

CXL 哈希表里存的实际 value。所有"meta 路由信息"已经从结构里清掉，只留 POSIX 必需字段 + `extra_`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id_` | `uint64_t` | inode id（meta_num 通过高位推算） |
| `nlink_` | `nlink_t` | 硬链接数 |
| `mode_` | `mode_t` | 文件类型 + 权限位 |
| `extra_` | `uint32_t` | 显式 padding；目录用作子条目计数（rmdir 检查 `extra_ == 0`） |
| `uid_` | `uint32_t` | 所有者 UID |
| `gid_` | `gid_t` | 所属组 |
| `rdev_` | `dev_t` | POSIX 兼容用，目前置 0 |
| `size_` | `size_t` | 文件大小 |
| `blksize_` | `size_t` | 推荐 IO 块大小 |
| `blocks_` | `size_t` | 已分配块数 |
| `atime_` `mtime_` `ctime_` | `time_t` | POSIX 三时间戳 |

```cpp
struct Inode {
  Inode(uint64_t id, nlink_t nlink, mode_t mode, uint32_t uid, gid_t gid,
        time_t atime, time_t mtime, time_t ctime,
        size_t size = 0, size_t blocks = 0);
  Inode() = default;
  static auto Create(...) -> Inode *;   // 历史 API，保留为 new+设字段
  static void Delete(Inode *);
  void Clear();                          // id_ = 0
};
```

### 6. Dirent

目录项，CXL dirent 哈希表 value。结构经过精简，已不再携带"路由 / 缓存"信息，跨 meta 路由由客户端发起遍历完成：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id_` | `ino_t` | 子节点 inode id（默认 0） |
| `pid_` | `ino_t` | 父目录 inode id |
| `reclen_` | `uint16_t` | 记录长度（`SetRecLen()` 维护） |
| `type_` | `unsigned char` | DT_REG / DT_DIR / ... |
| `name_[64]` | `char` | null-terminated 文件名 |

方法：

- `SetRecLen()`：`reclen_ = offsetof(Dirent, name_) + strlen(name_) + 1`
- `ToLinuxDirent(struct dirent &d)`：转成 POSIX `dirent`
- 静态 `Create / Delete`，以及 `Clear()`（`id_ = 0`）

### 7. DirHandle

服务端 `OpenDir` 给客户端的"目录句柄"，仅两个字段：

```cpp
struct DirHandle {
  uint64_t id_ = 0;
  uint64_t read_cutoff_version_ = 0;  // 后续 GetDentViews 携带，避免读到本次打开之后的新增/删除
};
```

### 8. DentView

`GetDentViews` 返回的目录项视图（含 tombstone + version，便于客户端在多 MDS 数据上按版本合并）：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id_` | `ino_t` | 子节点 inode id |
| `pid_` | `ino_t` | 父目录 inode id |
| `version_` | `uint64_t` | HLC version（合并依据） |
| `reclen_` | `uint16_t` | 记录长度 |
| `type_` | `unsigned char` | DT_* |
| `flags_` | `uint8_t` | bit 0 = `kFlagTombstone`（已删除占位） |
| `name_[64]` | `char` | null-terminated 文件名 |

```cpp
static constexpr uint8_t kFlagTombstone = 0x01;
auto IsDeleted() const -> bool;   // (flags_ & kFlagTombstone) != 0
```

### 9. 模板辅助

```cpp
template <typename DentType>
inline auto OffsetDirent(DentType *dents, int offset) -> DentType *;
```

按字节偏移把 `dents` 强转成新指针，遍历变长 dirent / DentView buffer 用。

---

## include/common/dfs.h

仅定义一个枚举 `RPCType`，作为 eRPC 注册 handler 时的"消息类型 ID"。

```cpp
enum RPCType : int {
  kHelloReq                  = 1,   // 初始连接握手
  kMetaGeneralReq            = 2,   // 通用 inode Get/Put
  kMetaPathCommonReq         = 3,   // 路径级元数据 op (Stat/Mkdir/Create/Unlink/Rmdir/Rename/Link/Chmod/Access/OpenDir)
  kMetaFDCommonReq           = 4,   // FD 级元数据 op (GetDents / GetDentViews)
  kDataReq                   = 5,   // 数据块读写
  kMetaCommunicationReq      = 6,   // meta server 之间同步
  kGuardianCommunicationReq  = 7,   // Guardian 之间通信
  kTestMetaCommunicationReq  = 8,   // 测试用元数据通信
  kGuardianCommonReq         = 9,   // Guardian 心跳/通用请求
  kLoadBalanceReq            = 10,  // 负载均衡触发
  kGetNodeReq                = 11,  // 获取节点信息
  kModifyInodeReq            = 12,  // 修改 inode 属性
  kSpecialReq                = 13,  // 特殊操作
  kMetaPersistenceReq        = 14,  // CXLPersistence 跨 MDS inode 变更
  kMetaPersistenceReqSplit   = 15,  // 同上，按 MTU 分片
};
```

> 实际 handler 在 `src/server_main.cc` 里通过 `rpc.register_req_func(RPCType, handler)` 注册。

---

## include/common/exception.h

Linux 系统调用异常封装，用来把 errno 转成可抛出的 C++ 异常：

```cpp
class LinuxSyscallException : public std::exception {
  int errcode_;
public:
  explicit LinuxSyscallException(int errcode);
  auto what() const noexcept -> const char * override;  // strerror(errcode_)
};
```

---

## include/common/config.h

仅声明两个工具函数：

```cpp
auto CommonCmdlineOptions() -> cxxopts::Options;
auto ParseCoreIds(std::string_view core_ids_string) -> std::vector<size_t>;
```

- `CommonCmdlineOptions()`：返回 `--config` / `--role` / `--id` / `--help` 等通用选项
- `ParseCoreIds()`：解析 `"32-37"` / `"0,2,4"` / `"1,3-5,7"` 三种格式，输出 CPU 核 ID 数组

实现见 `src/common/config.cc`。

---

## include/common/logging.h

只暴露一个 `InitLogger()`，由 `server_main.cc` 与 `preload.cc` 入口处调用：

```cpp
void InitLogger();
```

实现位于 `src/common/logging.cc`，行为：

- 创建带颜色的 stdout sink
- 若 `DFS_LOG_FILENAME` 非空，再加一个 100 MB 旋转文件 sink
- 默认日志格式：`[HH:MM:SS.fff Z] [LEVEL] [pid PID] [tid TID] [file:line] msg`
- 按 `SPDLOG_LEVEL` 环境变量切等级；按 `DFS_LOG_FLUSH_INTERVAL`（秒）周期 flush

---

## include/common/listdir_profile.h

listdir 热路径分阶段延迟 profiler。**默认关闭**（`#ifdef LISTDIR_LATENCY_PROFILE`），关闭时所有宏退化成 `(void)0`，零运行时开销。

### 阶段枚举（`enum ListdirStep`）

| 类别 | 枚举名 | 覆盖范围 |
|------|--------|----------|
| 客户端 | `kCliOpendirTotal` | 整个 `dfs_opendir` 函数体 |
|  | `kCliOpendirRpc` | OpenDir 路径解析 RPC |
|  | `kCliLoadMergedTotal` | 整个 `LoadMergedDirents` 函数体 |
|  | `kCliGetDentViewsRpc` | 单次 `GetDentViews` RPC（每 meta、每页一次） |
|  | `kCliMergeBuffer` | `MergeDentViewsFromBuffer` |
|  | `kCliSortAndPack` | names vector + std::sort + 打包 |
|  | `kCliAllocMergedBuf` | 最终 malloc + memcpy |
| 服务端 | `kSrvFdHandlerTotal` | 整个 `MDFDCommonReqHandler` 函数体 |
|  | `kSrvAcquireBuffer` | `AcquireBuffer` 自旋开销 |
|  | `kSrvGetDentViewsOuter` | `GetDentViews` 外壳（vector + 序列化） |
|  | `kSrvCollectDentViews` | 整个 `CollectPersistentDentViews` |
|  | `kSrvReadDirLatest` | `SSDDentRegion::ReadDirLatest` |
|  | `kSrvWalTailScan` | WAL tail 扫描 + 合并 |
|  | `kSrvNamesBuildSort` | names 构造 + std::sort |
|  | `kSrvEntriesEmplace` | DentView vector 构造 |
|  | `kSrvSerializeDentViews` | `SerializeDentViews` |
|  | `kSrvRpcResponseEncode` | response buffer 申请 + memcpy + enqueue |

### 公共 API

```cpp
struct ListdirStepStats {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> total_ns{0};
  std::atomic<uint64_t> max_ns{0};
};

class ListdirProfiler {
  static ListdirProfiler &Instance();
  void Record(int step, uint64_t ns);
  void Dump(const char *tag);    // 打印 count / avg_us / max_us / total_ms 表
};

class ListdirScopedTimer {       // RAII：构造记起点，析构 Record
  explicit ListdirScopedTimer(int step);
};
```

### 宏

```cpp
LISTDIR_PROFILE_SCOPE(step_enum)             // RAII 计时一个 scope
LISTDIR_PROFILE_DUMP("server-periodic")      // 立即 dump
LISTDIR_PROFILE_MAYBE_DUMP(N, "client")      // closedir 每 N 次 dump
```

服务端在 `Metadata::PrintSpace()` 周期触发 dump；客户端在 `dfs_closedir` 调用 `LISTDIR_PROFILE_MAYBE_DUMP`。

---

## include/common/metadata_op_profile.h

8 个路径级元数据 op 的延迟 profiler。**默认关闭**（`#ifdef MD_OP_LATENCY_PROFILE`）。

### 8 个 op

```cpp
enum MDOpStep : int {
  kMDOpUnlink = 0,
  kMDOpRemoveDir, kMDOpAccess, kMDOpMakeDir,
  kMDOpStat,      kMDOpRename, kMDOpLink, kMDOpCreate,
  kMDOpStepCount,
};
```

### 存储形态

每个 op 一份：

```cpp
constexpr int kMDOpHistBuckets = 64;   // bucket k 计 [2^k, 2^(k+1)) ns

struct MDOpStepStats {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> total_ns{0};
  std::atomic<uint64_t> max_ns{0};
  std::array<std::atomic<uint64_t>, kMDOpHistBuckets> hist{};
};
```

`Record()` 用 `__builtin_clzll` 落桶；`PercentileNs()` 顺序累加桶计数取分位（桶中点 = `1.5 * 2^k`，相对误差 ≤ 50%，足够区分量级）。

### Dump 输出

`Dump(tag)` 输出两块：

1. 人读表：`op | count | avg_us | p50_us | p99_us | p999_us | p9999_us | max_us`
2. 机器可读 raw：每 op 一行，含 `count` / `total_ns` / `max_ns` 与 64 个桶计数，便于 `scripts/analyze_md_op.py` 跨 log 聚合

### 宏

```cpp
MD_OP_PROFILE_SCOPE(step_enum)   // RAII 计时
MD_OP_PROFILE_DUMP(tag)          // dump
```

由 `Metadata::PrintSpace()` 周期触发 dump。

---

## include/version.h

构建期生成的 git 元信息：

```cpp
extern char const *const kGitCommit;
extern char const *const kGitBranch;
extern char const *const kGitTag;
```

实现由 `cmake/GitVersion.cmake` 在编译时生成 `version.cpp`，链接进各可执行文件。`InitPreload()` / `server_main.cc` 启动时打印 `[GitCommit / Branch / Tag]`，便于把日志精确对回某次构建。

---

## 总体关系

```
                    ┌───────────────────────────────────────┐
                    │ metadata_types.h                      │
                    │  · 编译开关 / 容量常量                 │
                    │  · Inode / Dirent / DentView /        │
                    │    DirHandle / RemoteInodeChange       │
                    │  · ObjectLocation / ObjectUuid         │
                    └────────────────┬─────────────────────┘
                                     │
                                     │ 引用
                                     ▼
        ┌───────────────────┐  ┌──────────────────────────┐
        │ dfs.h             │  │ listdir_profile.h        │
        │ · RPCType         │  │ metadata_op_profile.h    │
        │   (15 类)         │  │   (条件编译，默认关闭)    │
        └───────────────────┘  └──────────────────────────┘
                                     ▲
                                     │ 用于
                                     │
             ┌───────────────┐  ┌────┴─────────┐
             │ exception.h   │  │ logging.h    │
             │ LinuxSyscall- │  │ InitLogger() │
             │ Exception     │  │  + spdlog    │
             └───────────────┘  └──────────────┘

                          ┌───────────────────┐
                          │ config.h          │
                          │  cxxopts 选项     │
                          │  ParseCoreIds()   │
                          └───────────────────┘

                          ┌───────────────────┐
                          │ version.h         │
                          │  kGitCommit/Branch│
                          │  /Tag (编译时填)   │
                          └───────────────────┘
```

> profile 类头文件设计成"全部宏 + zero-cost 关闭"，所以即便不启用，也作为 include 出现在客户端/服务端代码里。
