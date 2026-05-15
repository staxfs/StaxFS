# Proto / 构建 / 线程亲和 / 测试 / 脚本

本文档汇总 dfs-prototype 的"非业务但必备"内容：FlatBuffers 协议定义、CMake 构建系统、线程亲和封装、测试与基准、各类启动/运行脚本。

---

## 一、Proto 定义（FlatBuffers）

`proto/*.fbs` 全部使用 FlatBuffers，编译时由 `flatbuffers_proto` interface library 触发 `flatc` 生成 `*_generated.h`，被服务端、客户端、测试共同 include。

### 1.1 数据平面

#### `proto/data.fbs`

```fbs
namespace dfs.data;

enum RequestType : byte { None = 0, Read = 1, Write = 2 }

table DataRequest {
  io_type : RequestType;
  objuuid : uint64;       // ObjectUuid::reserved_
  offset  : uint64;
  size    : uint64;
  buffer  : [ubyte];      // Write 时是请求体；Read 时是响应体
}
root_type DataRequest;
```

#### `proto/metadata.fbs`

历史最小桩；仅一个 `Inode { id : uint64 }`，工程内未真正使用。

### 1.2 元数据 op 枚举

#### `proto/mdrequest.fbs`

```fbs
namespace dfs.mdrequest;

enum MDOpType : byte {
  None = 0,

  /* Path Common */
  Unlink, RemoveDir,
  Access, MakeDir,
  Stat,
  Rename, Link,
  Create,                  // path, mode → inode_id | 0
  OpenDir,

  /* File Descriptor */
  GetDents,                // id, offset, size → buffer
  GetDentViews,            // id, offset, size → 最新 DentView 视图

  /* General Inode Get/Put */
  Chmod,
  Get,                     // inode_id → inode
  Put                      // inode_id, inode → 0 | -1
}

table MDRequest {                  // kMetaGeneralReq 的请求体
  uid           : uint32;
  gid           : uint32;
  op            : MDOpType;        // 仅 Get / Put / Chmod
  inode_id      : uint64;
  last_seen_hlc : uint64;
  inode         : [ubyte];         // 当 op = Put 时，序列化后的 dfs::Inode
}
root_type MDRequest;
```

> 共 14 个 op（不含 `None`）。`MDOpType` 同时被 path-common、fd-common 两个请求 table 复用，handler 内部通过 `op` 字段二级分发。

### 1.3 路径级请求 / 响应

#### `proto/mdpathcommonrequest.fbs`

```fbs
table MDPathCommonRequest {
  uid           : uint32;
  gid           : uint32;
  op            : MDOpType;
  path          : string;
  mode          : uint32;          // mkdir / access 用
  newpath       : string;          // rename 用
  meta_num      : int;             // 客户端选定的目标 meta；-1 由服务端 LocateInode 决定
  last_seen_hlc : uint64;          // 客户端 HLC（弱一致跟踪）
}
```

#### `proto/mdpathcommonresponse.fbs`

```fbs
table MDPathCommonResponse {
  flag             : short;        // 0 = ok；-1 = 跨 meta（按 next_meta_server 重试）；其它为 -errno
  reqs             : long;         // 服务端累计请求计数（负载均衡用）
  next_meta_server : short;        // flag=-1 时给客户端的"去这台"指引
  old_path         : string;       // rename 中间态使用
  new_path         : string;
  length           : short;        // path-walk 走到第几段
  server_hlc       : uint64;       // 服务端响应时刻的 HLC（客户端 ObserveServerHLC）
}
```

### 1.4 FD 级请求

#### `proto/mdfdrequest.fbs`

```fbs
table MDFDCommonRequest {
  uid           : uint32;
  gid           : uint32;
  op            : MDOpType;        // GetDents / GetDentViews / renameat
  id            : uint64;          // 对应 inode id
  offset        : int64;           // GetDents / GetDentViews 的页偏移
  u32arg        : uint32;          // size (getdents) / mode (mkdirat / accessat)
  u64arg        : uint64;          // GetDentViews 的 read_cutoff_version
  newpath       : string;          // renameat 用
  last_seen_hlc : uint64;
}
```

### 1.5 跨 MDS 通信

#### `proto/mdguardiancommonrequest.fbs`

```fbs
table MDGuardianCommonRequest {     // Guardian → Guardian 心跳/请求计数
  meta_num : int;
  reqs     : [int64];               // 各 meta 处理的请求量
}
```

#### `proto/mdguardianheartrequest.fbs`

```fbs
table MDGuardianHeartRequest {      // 简单心跳
  tv_sec  : int64;
  tv_nsec : int64;
}
```

#### `proto/mdmovenoderequest.fbs`

负载均衡：把一棵子树从一个 meta 迁到另一个 meta。Inode/Dirent 是序列化版（与 C++ 结构对齐）：

```fbs
table Inode {
  id : ulong;  nlink : ushort;  mode : uint;  extra : uint;
  uid : uint;  gid : uint;      rdev : uint;
  size : ulong;  blksize : ulong;  blocks : ulong;
  atime : long;  mtime : long;    ctime : long;
}

table Dirent {
  id : ulong;  pid : ulong;  off : long;  reclen : ushort;
  last_meta_server : short;  next_meta_server : short;
  type : ubyte;             name : string;
}

table MDMoveNodeRequest {
  mark             : short;          // 占位，保留
  last_meta_server : short;
  move_back        : short;
  id               : string;         // 子树根路径
  inodes           : [Inode];
  dirents          : [Dirent];
}
```

> Dirent 这里仍含 `last_meta_server / next_meta_server`，是为了在迁移时保留路由轨迹，**不**说明运行期 Dirent 还有这两个字段（C++ 端已精简掉）。

### 1.6 持久化跨 MDS 通道

#### `proto/mdpersistencerequest.fbs`

```fbs
table MDPersistenceRequest {
  inode_change_op      : [ubyte];     // RemoteInodeChangeOp
  inode_change_id      : [uint64];    // 被改 inode id
  inode_change_value   : [long];      // op 相关的值（nlink delta / mode / blocks delta / ctime）
  inode_change_version : [uint64];    // CompactWAL 的 HLC version
}
```

由 CXLPersistence 的 outbound forwarder 打包，对应 `kMetaPersistenceReq` / `kMetaPersistenceReqSplit`（payload 超过 MTU 时分片）。

---

## 二、构建系统

`CMakeLists.txt`（顶层）+ `cmake/` 助手模块。无业务相关 `option()` 开关——所有第三方依赖通过 `add_subdirectory(third_party/...)` 直接编进来；编译期开关都集中在 `include/common/metadata_types.h` 的 `#define` 里。

### 2.1 主要编译产物

| 产物 | 类型 | 来源 |
|------|------|------|
| `dfs-prototype-server` | executable | `src/server_main.cc` + server-lib |
| `dfs-prototype-client` | executable | `src/client_main.cc`（RPC 延迟基准） |
| `dfs-client-cli` | executable | `src/dfs_client_cli.cc`（不走 hook 的 demo） |
| `dfs-hook` | shared library | `src/hook.c` + client-lib（即 `libdfs-hook.so`） |
| `dfs-prototype-common` | static library | 公共代码（types、config、logging、cxl、threading） |
| `dfs-prototype-client-lib` | static library | 客户端实现（preload/posix_wrapper/rpc_client/file_descriptor） |
| `dfs-prototype-server-lib` | static library | 服务端实现（metadata/data/rpc_server/cxl_persistence） |
| `flatbuffers_proto` | INTERFACE library | 触发 `flatc` 生成 `*_generated.h` |
| `git-version` | static library | `cmake/GitVersion.cmake` 生成 `version.cpp` |

### 2.2 编译选项

```cmake
target_compile_definitions(dfs-prototype-common PUBLIC
  -DERPC_INFINIBAND
  -DBACKWARD_HAS_BFD=1
  -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)
```

- C++20，CMake ≥ 3.18
- `-march=core-avx2`、`-mclflushopt`、`-mclwb`（条件检测可用后开启）
- Release 增加 `-flto`
- Clang 路径上额外 `-fcoroutines-ts -stdlib=libc++`
- `-fmacro-prefix-map`（reproducible build）

### 2.3 第三方依赖

```
liburing             ExternalProject (静态)
spdlog + fmt          third_party
erpc                  third_party (transport=infiniband)
libcuckoo             third_party
concurrentqueue       third_party
backward-cpp + BFD    第 3 方调用栈
tomlplusplus          TOML 解析
cxxopts               CLI 参数
unordered_dense       高性能 hash map
opentelemetry-cpp     ↑ WITH_OLTP_GRPC / WITH_PROMETHEUS
flatbuffers           proto 生成
googletest            unit test
benchmark             microbench
zstd / lz4 / zlib     压缩
NUMA                  benchmark / cxl 模块
```

各 ON/OFF 选项（CMake `option()`）：

| 选项 | 默认 |
|------|------|
| `BUILD_TESTING` | OFF |
| `WITH_OLTP_GRPC` | ON |
| `WITH_PROMETHEUS` | ON |
| `WITH_EXAMPLES` | OFF |
| `FLATBUFFERS_BUILD_TESTS` | OFF |

### 2.4 Cmake 助手

| 文件 | 用途 |
|------|------|
| `cmake/GitVersion.cmake` | 抓 git commit / branch / tag / dirty，生成 `version.cpp` 给 `version.h` 链接 |
| `cmake/BuildDPDK.cmake` | DPDK 集成（当前主链路未启用） |
| `cmake/BuildSPDK.cmake` | SPDK 集成（同上） |
| `cmake/FindMake.cmake` | 找 `make` 给 ExternalProject 用 |

### 2.5 一键构建

```bash
./scripts/build.sh
# 等价于：
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
cmake --build build
```

---

## 三、线程亲和（`include/threading/affinity.h`）

```cpp
auto GetCpuAffinity() -> std::vector<std::size_t>;
void EnableOnCores(std::vector<std::size_t> const &core_ids);
void BindToCore(std::size_t core_id);
```

`src/threading/affinity.cc` 用 POSIX `pthread_get/setaffinity_np` + `CPU_*` 宏实现：

- `GetCpuAffinity()`：返回当前线程允许运行的 CPU 列表
- `EnableOnCores()`：放宽到给定核集合
- `BindToCore(c)`：清空 + 设置单核 c

服务端在 `EventLoopThread / GuardianThread / Checkpoint thread` 启动后第一句就调 `BindToCore`。

---

## 四、通用源码

### `src/common/config.cc`

实现两个工具：

- `CommonCmdlineOptions()`：构建 `cxxopts::Options`，通用选项 `--config / --role / --id / --help`
- `ParseCoreIds("32-37" / "0,2,4" / "1,3-5,7")`：返回 `std::vector<size_t>`

### `src/common/logging.cc`

实现 `InitLogger()`：
- stdout 彩色 sink + （可选）100 MB 旋转文件 sink
- pattern `[%Y-%m-%d %H:%M:%S.%e] [%l] [pid %P] [tid %t] [%s:%#] %v`
- 读取 `SPDLOG_LEVEL` / `DFS_LOG_FILENAME` / `DFS_LOG_FLUSH_INTERVAL`

---

## 五、测试与基准

### 5.1 单元测试（GoogleTest）

CMake：
```cmake
function(add_test_executable name) ... gtest_discover_tests(...) endfunction()

add_test_executable(metadata_test            test/metadata.cc)
add_test_executable(persistence_metadata_test test/persistence_metadata_test.cc)
add_test_executable(serdes_test              test/serdes.cc)
add_test_executable(data_test                test/data.cc)
add_test_executable(common_test              test/common.cc)
add_test_executable(level_hashtable_test     test/level_hashtable_test.cc)
```

| 测试 | 覆盖范围 |
|------|----------|
| `metadata_test` | Metadata 类：Stat/Mkdir/Create/Unlink/Rename/Link/OpenDir/ReadDir/PutInode/GetDents 等；含 `LocateInode` 路径解析 |
| `persistence_metadata_test` | CXLPersistence 端到端：CompactWAL 写 + checkpoint flush + 重启后 ReadDirLatest 验证 |
| `serdes_test` | FlatBuffers 各 schema 的 roundtrip（Inode、DataRequest、MDPathCommonRequest、MDRequest 等） |
| `data_test` | ObjectStore Read/Write 边界条件 |
| `common_test` | `ParseCoreIds` 各格式分支 |
| `level_hashtable_test` | TwoLevelHashtable 与 ConcurrentLevelHashtable 的 insert/find/update/erase + 并发 + resize 路径，多进程压测 |

### 5.2 子目录测试

`test/cxl/`：
- `cxl_lock_test.cc` —— mbind 到 NUMA 4 上分配自旋锁，fork 1–30 个进程压测 ops/sec
- `cxl_read_write_test.cc` —— 直接 `numa_alloc_onnode(10 GB, node=4)` 跑带宽（[07-cxl-bandwidth-benchmark.md](07-cxl-bandwidth-benchmark.md) 来源）

`test/syscall/`：
- `concurrent.cc` / `data.cc` / `meta.cc` —— 走 dfs-hook，跑 POSIX 系统调用层的并发/边界场景


### 5.3 代码格式 / 静态分析

- `.clang-format`：基于 Google style，2-space 缩进，80-col；模板对齐；定义块隔行
- `.clang-tidy`：开启 bugprone / clang-analyzer / google / modernize / performance / portability / readability；命名约定 CamelCase（类、enum、函数）+ lower_case（成员、变量）+ k 前缀（常量）+ g 前缀（全局）
- `.pre-commit-config.yaml`：`clang-format v14.0.6`
- 安装：
  ```bash
  pip install pre-commit
  pre-commit install
  pre-commit run --all
  ```

---

## 六、脚本

### 6.1 顶层脚本 `scripts/`

| 脚本 | 用途 |
|------|------|
| `build.sh` | `git submodule update --init --recursive` + Debug 构建 |
| `mount-hugepage.sh` | mount hugetlbfs，给所有 NUMA node 各加 4096 张 2 MB 页 |
| `clear-hugepage.sh` | 清空 hugepage 数 |
| `start-servers.sh` | 调 `functions.sh::start_servers ROLE COUNT BIN CONF`，nohup 后台拉起，pid 存到 `/tmp/dfs-prototype-{ROLE}-pids` |
| `kill-servers.sh` | 调 `functions.sh::kill_servers ROLE SIGNAL`，按 pid 文件 kill |
| `functions.sh` | 上述 start/kill 的 shell 函数实现，统一日志命名 `dfs-prototype-{ROLE}-{i}.log` |
| `disk-partition.sh` | 给后端块设备分 4 个分区（meta server "device-mode" 用） |
| `local-test-syscall.sh` | 跑 `test/syscall/*` 的便捷入口 |
| `analyze_hint.py` | 解析 `LEVEL_HASHTABLE_STATS` 输出 |
| `analyze_md_op.py` | 解析 `MD_OP_LATENCY_PROFILE` 输出（聚合多 log 直方图） |
| `verify_persistence.py` | 重启前/后比对 SSD region 内容 |
| `insert_ns_delay/run.sh` | 校准 `INSERT_DELAY(N)` 实际开销 |

### 6.2 本地测试 `scripts/local/`

| 脚本 | 用途 |
|------|------|
| `setup-test-servers.sh` / `kill-test-servers.sh` | 拉起/拆除 3 meta + 3 data |
| `dev-run-{cp,filebench,mdtest,ior,ycsb,THUCTC}.sh` | 各 workload 的 1-shot 入口（启 server → 跑 → 拆）|
| `{cp,filebench,ior,mdtest,ycsb,THUCTC}-runner.sh` | runner，设置 `LD_PRELOAD=libdfs-hook.so`、`DFS_CLIENT_CONFIG_PATH`、`SPDLOG_LEVEL` |

### 6.3 分布式测试 `scripts/distributed/`

| 脚本 | 用途 |
|------|------|
| `setup-test-servers.sh` | 检查 hugepage、按 conf 拉 meta+data，等 `data_root_path` 创建后再起下一组，最多重试 3 次 |
| `kill-test-servers.sh` | 拆除 + 清 `/tmp` + 清 hugepage |
| `dev-run-{cp,filebench,mdtest,mdtest-billion,mdtest-billion-add,THUCTC}.sh` | 完整跑一遍：稳频 → 构建 → 拉服务 → mpirun runner → 收尾 |
| `mdtest-runner.sh` `filebench-runner.sh` `THUCTC-runner.sh` `cp-runner.sh` | 实际 workload runner（含 LD_PRELOAD 设置） |
| `data_load.py` | 数据加载工具（递归 cp 替代品） |
| `THUCTC-data-load.sh` | THUCNews 数据集加载 |
| `organize-{filebench,mdtest}-result.py` | 跨节点结果聚合 |

---

## 七、关系总览

```
                  proto/*.fbs
                       │ flatc 生成
                       ▼
              *_generated.h  ←─────────────  flatbuffers_proto (INTERFACE)
                  │           │
        ┌─────────┴─┐         │
        ▼           ▼         │
  dfs-prototype-     dfs-prototype-
  client-lib         server-lib
        │                   │
        ▼                   ▼
  dfs-hook (so)     dfs-prototype-server
  dfs-prototype-    + 线程亲和 (threading/affinity)
  client (基准)      + 持久化 (CXLPersistence)
        │                   │
        └─── eRPC + IB ─────┘
                 │
                 ▼
       gtest 单元 (test/*.cc, test/cxl/*, test/syscall/*)
       Google Benchmark (benchmark/cxl_device_bench, hashmap_bench)
       脚本 (scripts/{,local,distributed}/*)
```

各部分通过 CMake 顶层装配，最终产物只有 4 个：`dfs-prototype-server`、`dfs-prototype-client`、`dfs-client-cli`、`libdfs-hook.so`。
