# 客户端

客户端通过 `LD_PRELOAD` 注入共享库 `libdfs-hook.so`，把对 `/dfs/*` 路径或 DFS 文件描述符的 POSIX 调用劫持到 DFS 自己的 RPC 路径，其余仍交还给 libc。整条客户端栈：

```
应用程序                       (POSIX 调用)
   │
   ▼
hook.c                       (LD_PRELOAD 入口；is_mount_path / is_dfs_fd)
   │
   ▼
posix_wrapper.cc             (dfs_*：FdManager 管 fd / gLockTable 管锁 / DentView 合并)
   │
   ▼
rpc_client.cc                (ClientRpcWrapper：会话池 / FlatBuffers 序列化 / HLC 跟踪)
   │
   ▼
eRPC + InfiniBand            → meta server / data server
```

代码组织：

| 文件 | 内容 |
|------|------|
| `src/hook.c` | LD_PRELOAD 拦截层；C 接口 |
| `src/client/posix_wrapper.cc` | POSIX → RPC 适配 |
| `src/client/rpc_client.cc` | eRPC 包装、会话管理、负载均衡、路径缓存、HLC |
| `src/client/preload.cc` | 全局上下文初始化、TOML 解析、客户端 rank 抢占 |
| `src/client/file_descriptor.cc` | DFS fd 分配/路由实现 |
| `src/client_main.cc` | 独立 RPC 延迟基准 |
| `src/dfs_client_cli.cc` | 不走 hook 的纯 eRPC demo |
| `include/client/*.h` | 上述各 `.cc` 的接口 |

---

## 一、全局常量与 fd 命名空间

`include/client/g_constants.h`：

```cpp
#define DFS_MOUNT_POINT  "/dfs"
#define DFS_MNTPT_LEN    4
#define DIR_MAX_ALLOC    1048576       // 单次 GetDentViews 缓冲上限
enum { kDfsMagicFdPrefix = 0x0fff0000 };
```

- 任何路径以 `/dfs` 开头视为 DFS 路径，hook 把前缀剥掉后交给 `dfs_*`
- 任何 `fd >= 0x0fff0000` 视为 DFS fd，否则交回 libc

---

## 二、PreloadContext / SharedContext —— 客户端启动

```cpp
// include/client/preload.h
class PreloadContext {
  std::unique_ptr<dfs::ClientRpcWrapper> rpc_client_;
  std::unique_ptr<dfs::FdManager>        fd_manager_;
  explicit PreloadContext(SharedContext &shared_ctx);
};

// include/client/g_preload.h
extern thread_local std::unique_ptr<dfs::PreloadContext> gPreloadCtx;
extern std::shared_ptr<SharedContext>                    gSharedCtx;

extern "C" {
  void InitPreload();
  void DestroyPreload();
}
```

`gPreloadCtx` 是 `thread_local`：每个调用 DFS 的线程独立持有 RPC client 与 fd manager；首次访问触发 lazy 初始化。`gSharedCtx` 跨线程共享 eRPC Nexus、服务端 URI 列表与每个 server 的 RPC 线程数。

### 2.1 `InitPreload()` 流程（`src/client/preload.cc`）

```
hook.c::setup() — __attribute__((constructor))
   │
   ▼
InitPreload()
   │
   ├─ InitLogger()                           （读 SPDLOG_LEVEL / DFS_LOG_FILENAME）
   ├─ 打印 git commit / branch / tag
   ├─ 等待 DFS_CLIENT_WAIT_ATTACH_SEC 秒（gdb attach 用）
   │
   ├─ 构造 gSharedCtx = make_shared<SharedContext>(config_path)
   │     ├─ 解析 client.toml
   │     │     · [client] host / port
   │     │     · [meta]  num + meta.{i} (host/port/threads)
   │     │     · [data]  num + data.{i} (host/port/threads)
   │     ├─ 通过 /dev/shm/.dfs-client.ranks 抢占客户端 rank（CAS 原子）
   │     │     · 最多 128 个并发客户端进程（gMaxClient = 128）
   │     │     · 监听端口 = [client].port + rank
   │     │     · 也可由 DFS_CLIENT_URI 显式覆盖
   │     ├─ 检测 NUMA 各 node 的 hugepage 数，挑有页的那一台建 Nexus
   │     ├─ 调 AddDataServer / AddMetaServer 把所有 server URI 入表
   │     └─ 创建 erpc::Nexus
   │
   └─ 留给业务线程首次访问 gPreloadCtx 时 lazy-construct PreloadContext
         · ClientRpcWrapper 构造（创建 erpc::Rpc 实例）
         · 对每个 server 起一组 session（每个 RPC thread 一条）
         · 调 WaitServerSessionConnected() 自旋直到全部连上
```

### 2.2 环境变量

| 变量 | 作用 |
|------|------|
| `DFS_CLIENT_CONFIG_PATH` | client.toml 路径，缺省 `/tmp/dfs-prototype/client.toml` |
| `DFS_CLIENT_URI` | 显式指定客户端 eRPC URI，覆盖 rank 计算 |
| `DFS_CLIENT_WAIT_ATTACH_SEC` | 启动时阻塞 N 秒供 gdb attach |
| `SPDLOG_LEVEL` | 日志等级 |
| `DFS_LOG_FILENAME` | 日志输出文件（不设则只 stdout） |
| `DFS_LOG_FLUSH_INTERVAL` | 强制 flush 间隔（秒） |

---

## 三、FdManager / DFS fd

`include/client/file_descriptor.h`：

```cpp
struct FileDescriptor {
  int fd_;                       // DFS fd（≥ kDfsMagicFdPrefix）
  int flags_ = 0;                // open flags
  bool modify_ = false;          // 写过；close/fsync 时触发 PutInodeRequest
  uint64_t seek_offset_ = 0;     // 顺序 read/write 的当前偏移
  Inode inode_;                  // 缓存的 inode（只在打开时拉取）
};

class FdManager {
  std::queue<int>                       fd_pool_;     // 释放过的 fd 优先复用
  int                                   min_free_fd_; // 起点 = kDfsMagicFdPrefix
  std::unordered_map<int, FileDescriptor> fd_map_;
public:
  auto AllocateFd(const Inode &)             -> int;
  auto ReleaseFd(int)                        -> int;
  auto GetFd(int, FileDescriptor &)          -> int;
  auto SetFd(int, FileDescriptor const &)    -> int;
};

auto LocateNodeId  (const Inode &)          -> int;   // 哈希 inode.id_ % data 数
auto LocateThreadId(const Inode &, int node) -> int;  // 哈希到该 data 的某条 RPC 线程
```

每个线程一份 `FdManager`，所以"线程持有的 DFS fd 不可跨线程共享"——这与 libc 的 fd 共享语义不同，需要应用层注意。

---

## 四、posix_wrapper.cc —— POSIX 适配

`include/client/posix_wrapper.h` 列出全部对外的 `dfs_*` 函数（C 接口）。下面按用途分组介绍语义。

### 4.1 文件 I/O

| 函数 | 行为 |
|------|------|
| `dfs_open(path, flags, mode)` | `O_CREAT` → `dfs_creat()`（即 `Create()` RPC）；否则 `Stat()` 拉 inode + `AllocateFd()` |
| `dfs_creat(path, mode)` | `Create()` RPC + `AllocateFd()` |
| `dfs_close(fd)` | 若 `modify_` 为真则先 `PutInodeRequest()` 把脏 inode 同步回 meta，再 `ReleaseFd()` |
| `dfs_read / dfs_write` | `LocateNodeId(inode)` → `RpcPread / RpcPwrite`；维护 `seek_offset_` |
| `dfs_pread / dfs_pwrite` | 同上但带显式 offset，**不修改** `seek_offset_` |
| `dfs_lseek` | 仅修改 `FileDescriptor::seek_offset_`，不发 RPC |
| `dfs_fsync / dfs_fdatasync` | 若 `modify_` → `PutInodeRequest()` |

### 4.2 路径级元数据

| 函数 | 对应 RPC | 备注 |
|------|----------|------|
| `dfs_stat / dfs_fstat / dfs_lstat / dfs_fstatat / dfs_statx` | `Stat()` | `fstat` 直接读 fd 上缓存的 `Inode`，不再请求服务端 |
| `dfs_access` | `Access()` | |
| `dfs_chmod` | `Chmod()` | |
| `dfs_mkdir / dfs_rmdir` | `Mkdir()` / `Rmdir()` | |
| `dfs_unlink` | `Unlink()` | |
| `dfs_rename` | `Rename()` | |
| `dfs_link / dfs_symlink` | `Link()` / `Symlink()`（目前 symlink 半实现） | |

### 4.3 目录读

```
dfs_opendir(path, alloc)
   │
   ├─ ClientRpcWrapper::OpenDir(path, &handle)   ← 路径解析 + 拿 DirHandle
   │     · handle.id_ = inode_id
   │     · handle.read_cutoff_version_ = 服务端当前 HLC，
   │       后续 GetDentViews 不会读到此后新增/删除
   ├─ malloc ClientDirstream
   ├─ LoadMergedDirents()  (见下)
   └─ 把 DIR* 的 LSB 置 1 后返回
```

```
dfs_readdir(sptr) / dfs_closedir(sptr)
   │  is_dfs_dirstream() 看 LSB
   ├─ 是 DFS：从已打包的 ClientDirstream 中按 reclen_ 顺序返回 dirent
   └─ 否：转 libc readdir / closedir
```

#### LoadMergedDirents — 跨 MDS 合并目录

由于 dirent 哈希表分散在所有 meta 上（按 `(parent_id, name)` 分片），读目录必须并行问每一台 meta。`LoadMergedDirents` 的流程：

```
LoadMergedDirents(handle, dirstream)
   │
   ▼
分页循环（每页 chunk_size = DIR_MAX_ALLOC）
   ├─ 调 BatchGetDentViews(id, cutoff_ver, n=meta_num,
   │                       meta_nums=[0..n-1], offsets=[页偏移]×n,
   │                       out_bufs[i], chunk_size, out_sizes[i])
   │      （所有 meta 并行 enqueue → 同一个 event_loop 等齐 → 解包）
   ├─ 对每条 DentView：丢弃 IsDeleted() == true 的 tombstone
   ├─ 同名按 version_ 取最大；不同 meta 上的同名只可能因为 rename 中间态
   ├─ 把保留下来的 DentView 转成 Linux dirent 推进 names 向量
   └─ 任一 meta 该页未结束就继续下一轮分页
   │
   ▼
std::sort(names)
打包到 ClientDirstream 的连续区，记下 reclen 链
```

> `BatchGetDentViews()` 使用一次 `enqueue_request` 发 N 条请求，再用同一个 `run_event_loop_once` 循环直到所有 continuation 都把 `out_sizes[i]` 填好。该函数返回 0 当且仅当所有 meta 都成功；任一 meta 失败时 `out_sizes[i] = -1`。

### 4.4 fcntl 与本地锁

```cpp
struct LockEntry { pid_t pid_; struct flock lock_; };
bool LocksConflict(const struct flock *a, const struct flock *b);
extern std::map<uint64_t, std::list<LockEntry>> gLockTable;
extern std::mutex                                gLockTableMutex;
```

`F_GETLK / F_SETLK / F_SETLKW` 由客户端进程内的 `gLockTable` 处理（按 inode id 索引），**未走服务端**。同一进程跨线程共享；多客户端目前不互斥。

### 4.5 mmap / fopen 等其他

- `dfs_mmap`：仅支持 `MAP_PRIVATE && !PROT_WRITE` 的只读映射，且通过 `malloc + dfs_pread` 模拟（**不是真实 mmap**）
- `dfs_munmap`：对应 free
- `dfs_fopen / dfs_fdopen`：用 `fopencookie()` 把 DFS fd 包装成 `FILE *`；维护 `FILE * → fd` 的映射
- `dfs_fileno`：从映射里反查 fd
- `dfs_dup`：分配新 DFS fd，复用 inode；`dfs_dup3` 暂未实现
- `dfs_mkstemp`：生成模板文件；走 `Create`
- `dfs_statfs / dfs_statvfs`：占位

---

## 五、ClientRpcWrapper —— eRPC 包装

`include/client/rpc_client.h`：

### 5.1 构造与会话

```cpp
class ClientRpcWrapper {
  std::vector<std::vector<int>> data_sessions_;   // [node_id][thread_idx]
  std::vector<std::vector<int>> meta_sessions_;   // [meta_num][thread_idx]
  cuckoohash_map<std::string, MapInfo> path_cache_;
  erpc::FastRand fastrand_;
  std::atomic<uint64_t> last_seen_hlc_{0};
public:
  std::unique_ptr<erpc::Rpc<erpc::CTransport>> rpc_;
  uint32_t uid_, gid_;
  void AddDataServer(uri, dataserver, n_threads);   // 每个 RPC thread 一条 session
  void AddMetaServer(uri, n_threads);
  auto WaitServerSessionConnected() -> void;        // 自旋直到 gConnectedSessionNum 满
  ...
};
```

`MapInfo` 是路径缓存的 value：

```cpp
struct MapInfo { int16_t meta_num_; std::string new_path_; };
```

`path_cache_` 用 cuckoohash_map 实现"路径 → 已知最近一次落点 meta_num"的命中加速；只在路径缓存模式下使用。

### 5.2 数据 RPC

```cpp
auto RpcPwrite(node_id, objuuid, offset, io_size, io_buffer) -> int;
auto RpcPread (node_id, objuuid, offset, io_size, io_buffer) -> int;
```

序列化成 `data.fbs::DataRequest`，用 `FastRandSessionNum(data_sessions_[node_id])` 选会话（Lemire 取模技巧），enqueue 后 `run_event_loop` 等结果。

### 5.3 元数据 RPC

```cpp
// 通用入口
auto MetaCall(fbb, req_type, meta_num, resp_size = 1) -> erpc::MsgBuffer;

// 路径级
auto PathCommonRequest(path, op, mode = 0, buf = nullptr) -> int;

// FD 级（GetDents / GetDentViews）
auto FDCommonRequest(id, op, offset = 0, u32arg = 0, buf = nullptr,
                     meta_num = -1, u64arg = 0) -> int;

// 多 meta 并发拉 DentView
auto BatchGetDentViews(id, read_cutoff_version, n, meta_nums, offsets,
                       out_bufs, chunk_size, out_sizes) -> int;

// 直接 Inode CRUD（fsync / close 走这个）
auto GetInodeRequest(inode_id, i_buf) -> int;
auto PutInodeRequest(inode_id, i_buf) -> int;
```

每个 op 同时也有薄包装：`Stat / Unlink / Rmdir / Access / Mkdir / Rename / Link / Create / Chmod / OpenDir / GetDents / GetDentViews`，它们都是对应 `MDOpType` 的一行内联。

### 5.4 路径级 RPC 的两种模式

`PathCommonRequest` 内部根据编译开关分两种行为：

- **CXL 模式（`USING_CXL_OFFSET` 启用）**：`meta_num = FastRandSessionNum(meta_count)` 随机选 meta，由服务端的 LocateInode 在哈希表上自行回弹到正确 meta（跨 meta 时返回 `flag = -1` + `next_meta_server`，客户端按指引重发）
- **路径缓存 + 负载均衡模式**：先在 `path_cache_` 上做最长前缀匹配；命中则按 `meta_num_` 直发；定期（每 5000 次请求）做一次负载均衡决策；`flag = -1` 时按 response 里的 `next_meta_server` 更新缓存

### 5.5 HLC 跟踪

每条请求都携带 `last_seen_hlc`，由：

```cpp
uint64_t LoadLastSeenHLC() const;          // 取本地最高观察值
void     ObserveServerHLC(uint64_t hlc);   // CAS-max 更新
```

服务端响应里附带 `server_hlc`（在 `mdpathcommonresponse.fbs`），客户端在解包后立刻 `ObserveServerHLC()`。这套"客户端只观察、不主动 tick"的弱协议保证了：

1. 同一客户端串行发出的请求在服务端按因果顺序执行
2. 跨客户端时不需要 lock-step；服务端会自己 merge-and-tick

---

## 六、hook.c —— LD_PRELOAD 入口

`src/hook.c` 是唯一的 C 文件，纯 C 接口便于 ELF 注入与 libc 互调。

```c
__attribute__((constructor)) static void setup(void)    { InitPreload(); … }
__attribute__((destructor))  static void teardown(void) { DestroyPreload(); }

static int  is_dfs_fd(int fd)        { return fd >= kDfsMagicFdPrefix; }
static int  is_mount_path(const char *p) { return strncmp(p,"/dfs",4)==0; }
static const char *get_dfs_path(const char *p) { return p + DFS_MNTPT_LEN; } // 剥前缀
```

每个被劫持的 libc 符号都按下列模板转发：

```c
int open(const char *pathname, int flags, ...) {
  ASSIGN_FN(open);                              // dlsym(RTLD_NEXT, "open") 缓存
  if (is_mount_path(pathname)) {
    mode_t mode = ...;                          // va_arg
    mode &= ~gUmask;                            // 应用 umask
    return dfs_open(get_dfs_path(pathname), flags, mode);
  }
  return libc_open(pathname, flags, mode);
}
```

被拦截的全集（节选）：`open / open64 / openat / openat64 / creat / read / write / pread / pwrite / lseek / lseek64 / fsync / fdatasync / close / stat / stat64 / fstat / fstat64 / lstat / lstat64 / fstatat / fstatat64 / statx / access / chmod / mkdir / rmdir / unlink / rename / link / symlink / opendir / readdir / readdir64 / closedir / fcntl / fopen / fopen64 / fdopen / fileno / mmap / mmap64 / munmap / mkstemp / mkstemp64`。

`F_SETLKW` 在锁冲突时由 hook.c 自旋 sleep 10 ms 后重试（不进入内核 wait），保持完全用户态。

---

## 七、`client_main.cc` 与 `dfs_client_cli.cc`

二者都是辅助二进制，不是日常路径：

- `client_main.cc`：纯 RPC 延迟基准。`InitPreload()` → 对每个 data/meta 服务的每条线程 1000 次 `RpcHelloMetaServer/RpcHelloDataServer` 预热 + 5000 次计时 → 打印平均延迟（ns）→ `DestroyPreload()`
- `dfs_client_cli.cc`：**不走 hook，不走 preload**，直接构造 eRPC Nexus + Rpc + session 到硬编码地址，发若干 `Hello` / `DataRequest`，只用来快速验证服务端连通

---

## 八、整体调用流程图

### 读流程（`dfs_read(fd, buf, n)`）

```
应用程序 read(fd, buf, n)
   │
   ▼
hook.c::read()                  is_dfs_fd → dfs_read
   │
   ▼
posix_wrapper.cc::dfs_read
   │  · GetFd(fd) → FileDescriptor & inode
   │  · LocateNodeId(inode) → data 服务器编号
   │
   ▼
ClientRpcWrapper::RpcPread
   │  · FastRandSessionNum(data_sessions_[node])
   │  · build DataRequest{ Read, objuuid, offset, size, buf }
   │  · enqueue_request → run_event_loop_once until done
   │
   ▼
data 服务器 IoReqHandler → ObjectStore::Read → 回包
   │
   ▼
posix_wrapper 把 buffer payload memcpy 给应用，
更新 FileDescriptor::seek_offset_，返回字节数
```

### 目录流程（`dfs_opendir + dfs_readdir`）

```
opendir("/dfs/myDir/")
   │
   ▼
ClientRpcWrapper::OpenDir(path, &handle)
   │  → 路径解析 RPC，得到 (inode_id, read_cutoff_version)
   │
   ▼
LoadMergedDirents(handle, ds)
   │
   ▼
for each meta i in [0..M):
    enqueue_request GetDentViews(i, id, cutoff_ver, offset[i])
run_event_loop_once until 所有 i 的 out_sizes[i] 写好
   │
   ▼
合并：
  · 丢 IsDeleted() == true
  · 同名按 version_ 取最大
  · 累入 names 向量
std::sort(names)；打包成 ClientDirstream

readdir(ds) → 按 reclen_ 顺序输出 dirent，直至 names 跑完
closedir(ds) → free 缓冲；可触发 LISTDIR_PROFILE_MAYBE_DUMP
```

### 写流程（`dfs_creat + dfs_write + dfs_close`）

```
creat("/dfs/foo")
   │
   ▼
PathCommonRequest(MDOpType_Create, mode, buf=&inode_out)
   │  服务端：LocateInode → AllocateInodeId → ihashtable_->insert
   │           AddDirectoryEntry → CompactWAL::Append
   │
   ▼
AllocateFd(inode_out) → DFS fd

write(fd, buf, n) ×K
   │
   ▼
RpcPwrite (data 服务器)
   · 同时本地把 fd_map_[fd].modify_ = true
   · 维护 seek_offset_

close(fd)
   │
   ▼
modify_ ? PutInodeRequest(inode.id_, inode) → MDPathCommonReqHandler / MDGeneralReqHandler
ReleaseFd(fd)
```
