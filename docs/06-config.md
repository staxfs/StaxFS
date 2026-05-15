# DFS-Prototype 配置与部署

## 一、配置文件总览

DFS 使用 TOML 文件描述每个进程的角色与拓扑。一个完整集群包含 **N 个 meta server + M 个 data server + K 个 client**，三类配置文件分别独立放置，统一收纳在 `conf/<场景>/` 子目录下。

```
conf/
├─ local/                         # 单机本地测试 (3 meta + 3 data + 1 client)
│  ├─ meta-0.toml  meta-1.toml  meta-2.toml
│  ├─ data-0.toml  data-1.toml  data-2.toml
│  └─ client.toml
└─ distributed_cxl_<M>_<P>_<D>_<Q>/
   ├─ meta-{0..M-1}.toml          # 每个 meta 一份
   ├─ data-{0..D-1}.toml          # 每个 data 一份
   ├─ client.toml                 # 客户端引用集群拓扑
   ├─ client_list                 # 用于 mpirun -hostfile 的客户端节点列表
   └─ client_list2                # 备用节点列表（部分实验用）
```

### 命名规范

`distributed_cxl_<M>_<P>_<D>_<Q>` 中的字段含义：

| 占位符 | 含义 |
|--------|------|
| `M` | meta server 数 |
| `P` | 单 meta 工作线程占用 CPU 核数（EventLoop）|
| `D` | data server 数 |
| `Q` | 单 data 占用 CPU 核数 |

当前已存在的拓扑：

```
distributed_cxl_2_4_3_10
distributed_cxl_3_1_3_10
distributed_cxl_3_2_3_10
distributed_cxl_3_4_3_10
distributed_cxl_3_8_3_10
distributed_cxl_4_4_3_10
distributed_cxl_5_4_3_10
```

## 二、TOML 字段说明

### `meta-i.toml`

```toml
[meta]
host           = "172.16.33.35"               # eRPC 监听 IP
port           = 31850                        # eRPC 监听端口（每个 meta 唯一）
data_root_path = "/tmp/dfs-prototype/meta_0/" # 落地目录
core_ids       = "32-37"                      # 绑定 CPU 核（支持 "1,3-5,7" 格式）
meta_id        = 0                            # 唯一 meta 编号 (0..M-1)
```

`core_ids` 个数即该 meta 的总线程数，分配方式：

```
core_ids = "32-37" (6 个核)
       │
       ├─ core[0]     → Guardian 线程 (rpc_id = 0)
       ├─ core[1..n-2] → EventLoop 线程 (rpc_id = 1..n-2)
       └─ core[n-1]   → Checkpoint 线程 (无 RPC 实例)
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `host` | string | 监听 IP，用于 eRPC 会话建立 |
| `port` | int | 监听端口；同机器上的不同 meta 必须使用不同端口 |
| `data_root_path` | string | 启动时若不存在会被创建；setup 脚本以此判断 server 是否就绪 |
| `core_ids` | string | 支持 `"32-37"` / `"0,2,4,6"` / `"1,3-5,7"` 三种格式 |
| `meta_id` | int | 唯一编号；`meta_id=0` 负责 CXL 共享内存初始化、根目录 `"/"` 创建 |

### `data-i.toml`

```toml
[data]
host           = "172.16.33.35"
port           = 31853
data_root_path = "/tmp/dfs-prototype/data_0/"
core_ids       = "0-9"                        # 全部用作 EventLoop 工作线程
```

data server 上每个 `core_ids` 都对应一个 EventLoop 线程，无 Guardian / Checkpoint 之分。

### `client.toml`

```toml
[client]
host = "localhost"        # 客户端 eRPC 端点 IP（端口动态分配，见下）
port = 31850              # 起始端口

[meta]
num = 3
  [meta.0]
  host    = "172.16.33.35"
  port    = 31850
  threads = 4             # 该 meta 接受连接的 RPC 线程数
                          # 与 meta-0.toml 的 core_ids 个数 - 2 对齐
                          # （扣掉 Guardian 与 Checkpoint）

  [meta.1] ...
  [meta.2] ...

[data]
num = 3
  [data.0]
  host    = "172.16.33.35"
  port    = 31853
  threads = 10            # 与 data-0.toml 的 core_ids 个数对齐
  [data.1] ...
  [data.2] ...
```

> **客户端端口分配**：每个客户端进程启动时，从 `/dev/shm/.dfs-client.ranks` CAS 抢占一个 rank（最多 128 个并发客户端），实际监听端口 = `[client].port + rank`。详见 `src/client/preload.cc::SharedContext` 构造函数。

### `client_list` / `client_list2`

每行一个客户端节点的 IP，用于分布式启动脚本（如 `mpirun -hostfile`）：

```
172.16.33.30
172.16.33.31
172.16.33.32
```

## 三、部署流程

### 1. 系统准备（一次性）

```bash
# 系统包
sudo apt install autoconf pkg-config ninja-build binutils-dev libnuma-dev \
    librocksdb-dev bzip2 libbz2-dev libsnappy-dev liburing-dev liblz4-dev \
    libzstd-dev libcurl4-openssl-dev libpfm4-dev zlib1g-dev systemtap-sdt-dev \
    python3-toml python-is-python3 flex msr-tools libopenmpi-dev

# 大页（eRPC 必需）—— 默认 2048×2 MB/NUMA node
sudo ./scripts/mount-hugepage.sh
```

### 2. 编译

```bash
./scripts/build.sh
# 等价于
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .
cmake --build build
```

### 3. 启动服务端

**本地（单机）**：

```bash
sudo ./scripts/local/setup-test-servers.sh ./conf/local off
# 或手动：
./scripts/start-servers.sh meta 3 ./build/dfs-prototype-server ./conf/local
./scripts/start-servers.sh data 3 ./build/dfs-prototype-server ./conf/local
```

**分布式**：

```bash
./scripts/distributed/setup-test-servers.sh \
    ./conf/distributed_cxl_3_4_3_10 info 3 3
# 参数：<conf 路径> <log level> <meta 数> <data 数>
```

`setup-test-servers.sh` 的逻辑：

```
1. kill 旧进程 + 清旧日志
2. 启动 meta server，等待每个 meta 的 data_root_path 目录被创建
3. 检查 *stderr* 文件是否非空（启动失败信号），失败则重试，最多 3 次
4. 启动 data server，同样等待目录创建
5. 打印 "Startup OK!"
```

### 4. 启动客户端（LD_PRELOAD 方式）

```bash
export LD_PRELOAD=$(pwd)/build/libdfs-hook.so
export DFS_CLIENT_CONFIG_PATH=$(pwd)/conf/local/client.toml
export SPDLOG_LEVEL=info
export DFS_LOG_FILENAME=/tmp/dfs-prototype/dfs-client.log

# /dfs/* 路径会被 hook 截走，其余仍走 libc
./your-app
```

或直接用现成 runner（自动设置好上述变量）：

```bash
sudo ./scripts/local/dev-run-mdtest.sh ../ior/src/mdtest off
sudo ./scripts/local/dev-run-filebench.sh ../filebench/filebench \
     workloads/filebench/fileserver.f off
```

### 5. 关闭

```bash
./scripts/kill-servers.sh meta 15
./scripts/kill-servers.sh data 15
# 或一次清干净（含 /tmp 残留 + hugepage）
./scripts/distributed/kill-test-servers.sh
```

## 四、环境变量

| 变量 | 作用方 | 说明 |
|------|--------|------|
| `DFS_CLIENT_CONFIG_PATH` | 客户端 | client.toml 路径，缺省 `/tmp/dfs-prototype/client.toml` |
| `DFS_CLIENT_URI` | 客户端 | 覆盖自动分配的 `host:port`（手动指定客户端 eRPC URI） |
| `DFS_CLIENT_WAIT_ATTACH_SEC` | 客户端 | 启动时阻塞 N 秒，给 gdb attach 留时间 |
| `SPDLOG_LEVEL` | 通用 | spdlog 日志等级 (`trace`/`debug`/`info`/`warn`/`error`/`off`) |
| `DFS_LOG_FILENAME` | 通用 | 旋转文件 sink 的输出路径；不设则只走 stdout |
| `DFS_LOG_FLUSH_INTERVAL` | 通用 | 日志强制 flush 间隔（秒） |
| `LOG_LEVEL` | 启动脚本 | 透传给 `start-servers.sh` 再设进 `SPDLOG_LEVEL` |

## 五、CPU 核心分配示例（双路 NUMA，64 物理核）

```
NUMA Node 0 (CPU 0-31)                NUMA Node 1 (CPU 32-63)
┌─────────────────────────────────┐   ┌─────────────────────────────────┐
│ data 0  : core 0-9              │   │  meta 0 : core 32-37            │
│ data 1  : core 10-19            │   │   ├─ 32  Guardian               │
│ data 2  : core 20-29            │   │   ├─ 33-36 EventLoop ×4         │
│                                 │   │   └─ 37  Checkpoint             │
│                                 │   │  meta 1 : core 38-43 (同上)     │
│                                 │   │  meta 2 : core 44-49 (同上)     │
└─────────────────────────────────┘   └─────────────────────────────────┘
```

`CXL_NUMA_NODE = 4`：CXL 扩展内存（mmap 在 `0x600000000000`，文件 `/dev/hugepages/cxl_memory`）位于一个独立的 CXL NUMA node，所有 meta 共享读写。

## 六、虚拟地址布局

`gDevice` 在所有 meta 的相同虚拟地址上 mmap，使得 inode/dirent 哈希表的"指针"在跨 meta 时直接生效：

```
0x500000000000  CXL-SSD 闪存映射文件（DRAM cache pool 头）
0x600000000000  CXLMem (CXL_CAPACITY = 20 GB) — 共享 hugepage
                ├─ CXLMemHeader / GIMGlobalHeader / DFSHeader
                └─ NaiveAllocator pool（哈希表桶 / WAL ring / SSD region 头）
0x700000000000  GIM 区段（每 meta 一片）
                ├─ meta 0 : per_meta_size 大小
                ├─ meta 1 : 紧邻其后
                └─ ...
```

各 meta 通过 `GIMMem::MapOtherMetas()` 把"别人的 GIM 段"也 mmap 进自己的虚拟地址，模拟"远程 RDMA 访问 + 延迟注入"。详细布局见 [02-cxl.md](02-cxl.md)。

## 七、常见排错

| 现象 | 可能原因 |
|------|----------|
| 启动后 `*stderr*` 文件非空、`setup-test-servers.sh` 重试 | 未挂大页 / hugepage 不足 / NUMA 4 没有 CXL hugepage / `data_root_path` 上次未清 |
| 客户端 `WaitServerSessionConnected` 卡死 | 服务端没起齐；或 client.toml 的 `threads` 数与 server 端 `core_ids` 个数对不上（两侧 RPC 实例数不匹配） |
| `client.toml` 加载失败 | `DFS_CLIENT_CONFIG_PATH` 未设置且默认路径 `/tmp/dfs-prototype/client.toml` 不存在 |
| meta 日志出现 `extra_ > 0` 导致 rmdir 失败 | 上一轮 unlink 还没等到 checkpoint flush；改大 `CXLSSD_CHECKPOINT_INTERVAL_MS` 或减压后重试 |
| 客户端端口冲突 | 已运行 ≥128 个客户端；`/dev/shm/.dfs-client.ranks` 满；删除该文件后重启 |
