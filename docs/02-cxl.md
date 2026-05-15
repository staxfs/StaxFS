# CXL 内存子系统

## 1. 架构概览

CXL 内存子系统统一管理三类存储资源，全部由全局单例 `gDevice`（`CXLDevice` 类型）封装；元数据层、持久化层都通过 `gDevice->XXX` 间接访问硬件。

```
gDevice : CXLDevice (include/cxl/device.h)
├── cxl  : CXLMem  — CXL 扩展内存（NUMA 4，跨 meta 共享，~600ns R/W、~2300ns CAS）
├── gim  : GIMMem  — 每 meta 一片 DRAM hugepage
│              ├─ Local  路径：本机 memcpy / __sync_*  (~6ns)
│              └─ Remote 路径：访问别 meta 区段 + 注入 RDMA 延迟 (~2.6us)
├── ssd  : CXLSSD  — CXL-SSD 模拟器
│              ├─ DRAM cache：在 cxl 池里 bump-alloc（共享 CXL 延迟）
│              └─ Flash 后端：NVMe pread/pwrite（无注入）
└── dfs_header_ : DFSHeader — 跨 meta 共享的 allocator/notify/inode_array 偏移表
```

### 文件清单

| 文件 | 说明 |
|------|------|
| `include/cxl/cxl_allocator.h` | NaiveAllocator — 64 B 对齐 bump 分配器，所有模块共用 |
| `include/cxl/cxl_mem.h` | CXLMem — CXL 扩展内存：mmap、Read/Write/Cas + 交换机延迟注入 |
| `include/cxl/gim_mem.h` | GIMMem — 每 meta 本地 DRAM；Local 零延迟 / Remote 注入 RDMA 延迟 |
| `include/cxl/cxl_ssd.h` | CXLSSD — DRAM cache（复用 CXL 池）+ Inode/Dirent 各一份 NVMe 后端文件 |
| `include/cxl/device.h` | CXLDevice — 统一容器；定义 `DFSHeader` 与共享内存布局 |
| `include/cxl/cxl_base.h` | CXLPointer / CXLBuffer — 旧偏移抽象（保留接口兼容） |
| `include/cxl/cxl_context.h` `gim_context.h` | 旧 per-meta 包装类（保留 `GIMGlobalHeader` 类型给 device.h 使用） |
| `src/cxl/*.cc` | 上述各 `.h` 的实现 |
| `test/cxl/cxl_read_write_test.cc` | 直接 `numa_alloc_onnode` 的纯带宽测试（见 [07-cxl-bandwidth-benchmark.md](07-cxl-bandwidth-benchmark.md)） |
| `scripts/insert_ns_delay/` | `INSERT_DELAY(N)` 校准工具 |

---

## 2. 延迟注入

### 2.1 INSERT_DELAY 宏（`include/cxl/cxl_mem.h`）

```cpp
#define INSERT_DELAY(times)                 \
  _mm_mfence();                             \
  for (int i = 0; i < (times); i++) {       \
    _rdtsc();                               \
    __asm__ volatile("pause" ::: "memory"); \
  }                                         \
  _mm_mfence();
```

> 为什么 2 个 mfence：`rdtsc + pause` 不是序列化指令，CPU 乱序执行会把延迟与后续内存操作重叠掉。前后各一个 mfence 强制序列化。

### 2.2 实测校准结果（Xeon Gold 6448H @ 2.4 GHz，Node 1 CPU → Node 4 CXL）

| 路径 | iters 宏 | 注入次数 | 函数体耗时 | 注入耗时 | 合计 | 目标 |
|------|---------|---------|-----------|---------|------|------|
| CXL Read 64 B | `CXL_READ_DELAY_ITERS` | 2 | ~481 ns | ~116 ns | ~597 ns | 600 ns |
| CXL Write 64 B | `CXL_WRITE_DELAY_ITERS` | 5 | ~381 ns | ~209 ns | ~590 ns | 600 ns |
| CXL CAS | `CXL_CAS_DELAY_ITERS` | 72 | ~16 ns | ~2286 ns | ~2302 ns | 2300 ns |
| GIM Remote Read | `GIM_READ_DELAY_ITERS` | 63 | ~584 ns | ~2007 ns | ~2591 ns | 2600 ns |
| GIM Remote Write | `GIM_WRITE_DELAY_ITERS` | 28 | ~358 ns | ~922 ns | ~1280 ns | 1270 ns |
| GIM Remote CAS / FAA | `GIM_ATOMIC_DELAY_ITERS` | 79 | ~99 ns | ~2503 ns | ~2602 ns | 2610 ns |
| GIM Local R/W/CAS | — | 0 | ~6 ns / ~6 ns / ~14 ns | 0 | ~6 ns | — |

校准脚本：`scripts/insert_ns_delay/run.sh`。RDMA 目标值来自实测 `RDMA_latency.log`。

---

## 3. NaiveAllocator —— 共享 bump 分配器

```cpp
class NaiveAllocator {
  uint64_t magic_num_;
  uint8_t *buf_;
  uint64_t capacity_;
  std::atomic_uint64_t offset_;     // 当前已分配位置
  std::atomic_uint64_t free_size_;  // 累计 fake-free 字节（不回收）
  void Init(void *buf, uint64_t capacity);
  auto CXLMalloc(size_t size) -> void *;
  auto CXLMallocOffset(size_t size) -> uint64_t;
  void CXLFree(void *ptr, size_t size);
  void CXLFakeFree(void *ptr, size_t size);
};
```

- 64 B 对齐
- 只支持 **追加**：`free_*` 系列只更新统计计数，不回收空间
- 每个池一份 allocator：CXLMem 的池一份（跨 meta 共享）；GIMMem 的池每 meta 一份（在 `DFSHeader::allocator_[]` 数组里）

---

## 4. CXLMem —— CXL 扩展内存

```cpp
struct CXLMemHeader {                          // 位于 CXL 共享内存内
  std::atomic<uint64_t> magic_num_{0};         // 0x43584C53 ("CXLS") 表示已初始化
  std::atomic<uint64_t> meta_finish_{0};       // 各 meta 完成 init 的位掩码
  NaiveAllocator allocator_;
};

class CXLMem {
  Init(meta_num, capacity_GiB, path, numa_node);   // mmap hugepages on NUMA 4
  Read (offset, size, data);    // INSERT_DELAY(2) + clflushopt + SSE load + stream store
  Write(offset, size, data);    // INSERT_DELAY(5) + 反序 SSE store + clwb
  Cas  (offset, cmp, swap);     // INSERT_DELAY(72) + __sync_val_compare_and_swap
  Alloc/Free/Reset              // 转 NaiveAllocator
};
```

### 初始化

```
meta_num == 0
  ├─ mmap(MAP_FIXED|MAP_SHARED|MAP_HUGETLB) 到 0x600000000000
  ├─ 全区清零（保证 hashtable D-bit 起点为 0）
  ├─ 在 0x1000 偏移 placement-new 一个 CXLMemHeader
  └─ 写入 magic_num_ = "CXLS"

meta_num != 0
  ├─ 同样 mmap 到 0x600000000000（共享同一个 hugepage 文件）
  └─ 自旋等 header_->IsInit() == true
```

### 读路径细节

`CXLMem::Read` 用 `clflushopt` 把 64 B 行从本地 cache 失效，再用非临时 SSE load + stream store 把数据搬到本地 buffer。这模拟"绕过本地 cache、强制走 CXL"的真实开销。`Write` 反序写入 + `clwb` 是为崩溃一致性服务（高地址先落、低地址（含 header）最后落）。

---

## 5. GIMMem —— 每 meta 本地 DRAM + 模拟 RDMA

```cpp
constexpr int kMaxMetaNum = 16;

class GIMMem {
  Init(meta_num, cxl_capacity, per_meta_MiB, hugepage_base_path, numa_node);
  MapOtherMetas(numa_nodes);    // mmap 别人的 hugepage 进自己 VA 空间
  AllocLocal(size); FreeLocal(p, size);  GetOffset(p);
  // 本地路径（零延迟）
  LocalRead/Write/Cas/FAA(offset, ...);
  // 远端路径（注入 RDMA 延迟）
  RemoteRead /Write/Cas/FAA(target_meta, offset, ...);
  // 测试钩子
  SetTestBacking(my_meta, total, bufs[], alloc);
};
```

### VA 布局

```
0x700000000000 + 0 * per_meta_size  →  Meta 0 的 hugepage（其本地 DRAM）
0x700000000000 + 1 * per_meta_size  →  Meta 1 的 hugepage
0x700000000000 + 2 * per_meta_size  →  Meta 2 的 hugepage
...
```

每个 meta 启动时：

```
1. 在自己 NUMA node 上创建 GIM hugepage 文件
2. mmap(MAP_FIXED) 到 GIM_BASE + my_meta * per_meta_size
3. 把 my_alloc_ 的指针写进 DFSHeader::allocator_[my_meta]
4. 调用 MapOtherMetas() —— 依次 mmap 别 meta 的 hugepage 文件
   到 GIM_BASE + i * per_meta_size，并 mbind 到对应 NUMA node
```

> Local 路径只是普通 memcpy/`__sync_*`，约 6 ns；Remote 路径多了一次 `INSERT_DELAY(N)`，模拟跨机 RDMA 的延迟。在实现层，Remote 仍是直接访问 mmap 进来的别人 hugepage（无需真正过网络）。

### 跨 meta 协调用结构

```cpp
struct alignas(64) GIMResizeNotify {
  uint64_t resize_version{0};
  uint8_t _pad[56];
};
```

每个 meta 在自己的 GIM 区段里分配一个 `GIMResizeNotify`，把它的 GIM 偏移写进 `DFSHeader::d_resize_notify_offset_[me]` / `i_resize_notify_offset_[me]`。哈希表渐进 resize 时，发起 meta 通过 `GIMWriteSync(target, off, ...)` 通知其他 meta 推进 cursor。

---

## 6. CXLSSD —— CXL-SSD 模拟器

```cpp
struct CXLSSDDramHeader {
  std::atomic<uint64_t> magic_num_;
  std::atomic<uint64_t> alloc_pos_;   // DRAM 区内的 bump 偏移
  uint64_t capacity_;
  uint64_t base_offset_;              // DRAM 区在 CXLMem 池中的起始偏移
};

class CXLSSD {
  Init(cxl_mem, dram_capacity_MiB, flash_path, header, meta_num);

  // DRAM 路径（走 CXLMem，注入 600ns）
  DramAlloc(size, align);       // 在 DRAM 区内 fetch_add
  DramRead /Write(off, size, buf);
  DramBase();  DramCapacity();

  // Flash 路径（NVMe pread/pwrite，零注入）
  InodeWrite/Read/Sync(...);    // 写到 <flash_path>/Meta<N>_Inode
  DentWrite/Read/Sync(...);     // 写到 <flash_path>/Meta<N>_Dent
};
```

### DRAM cache 区域

```
CXLMem 池
┌────────── pool_base_ ──────────┐
│ ... 各种 allocator 分配 ...    │
│ ┌───── DRAM base_offset_ ────┐ │
│ │ CXL-SSD DRAM cache         │ │  capacity_ = CXLSSD_CAPACITY_MB
│ │  └── alloc_pos_ ↑          │ │  meta 0 在 Init 时一次性预留
│ └────────────────────────────┘ │
└────────────────────────────────┘
```

### Flash 后端

每个 meta 都有自己的 inode/dent 后端文件，互不重叠：

```
<CXLSSD_PATH>/Meta0_Inode    ← SSDInodeRegion 的脏页 pwrite 落在此
<CXLSSD_PATH>/Meta0_Dent     ← SSDDentRegion  的脏页 pwrite 落在此
<CXLSSD_PATH>/Meta1_Inode
<CXLSSD_PATH>/Meta1_Dent
...
```

> 用法上层在 `include/server/cxl_persistence.h` 的 `SSDInodeRegion` / `SSDDentRegion`，详见 [03-server.md](03-server.md)。

---

## 7. CXLDevice —— 统一容器与初始化序列

### 7.1 共享内存布局

`device.h` 声明了 `CXLDeviceLayout`，把所有"共享 header"按固定偏移码进 CXL 内存的前 1 MB：

```
0x600000000000 + 0x000000  reserved (4 KB)
              + 0x001000   CXLMemHeader        ← kCXLMemHeaderOffset
              + ...        GIMGlobalHeader     ← GIMHeaderOffset()
              + ...        CXLSSDDramHeader    ← SSDHeaderOffset()
              + ...        DFSHeader           ← DFSHeaderOffset()
              + 0x100000   ←─ pool start (1 MB)
              │
              │  NaiveAllocator 的 bump 区
              │  （哈希表桶、CompactWAL ring、SSDInodeRegion 等都从这里 alloc）
              ↓
```

### 7.2 DFSHeader

```cpp
constexpr int kMaxMetaNumOfGroup = 8;

struct DFSHeader {
  int            numa_node_  [kMaxMetaNumOfGroup];     // 每 meta 的 NUMA node
  NaiveAllocator allocator_  [kMaxMetaNumOfGroup];     // 每 meta 的 GIM 分配器
  std::atomic<uint64_t> magic_num_{0};                 // 0x43584C53 表示已初始化

  uint64_t d_resize_meta_offset_   = UINT64_MAX;       // dirent 哈希表 ResizeMeta 在 CXL 的偏移
  uint64_t i_resize_meta_offset_   = UINT64_MAX;       // inode  哈希表 ResizeMeta 在 CXL 的偏移

  uint64_t d_resize_notify_offset_[kMaxMetaNumOfGroup]; // 每 meta 一个通知槽（GIM 偏移）
  uint64_t i_resize_notify_offset_[kMaxMetaNumOfGroup];

  uint64_t inode_array_block_offset_[kMaxMetaNumOfGroup]; // USING_INODE_ARRAY 时每 meta 的块头偏移
};
```

### 7.3 8 步初始化序列

```
CXLDevice::Init(meta_num, ...) 入口
    │
    ▼
① CXLMem::Init(meta_num, ...)
    └─ mmap hugepages on NUMA 4，固定到 0x600000000000
②  if meta_num == 0
        清零 + placement-new CXLMemHeader / GIMGlobalHeader / DFSHeader
        magic_num_ = "CXLS"
    else
        spin until CXLMemHeader::IsInit()
③ cxl->AttachHeader(header_)        ← 设置 pool_base_ = 0x100000
④ GIMMem::Init(meta_num, ...)       ← 在自己 NUMA 上建 GIM hugepage
⑤ DFSHeader::allocator_[meta_num] = my GIM allocator
⑥ GIMMem::MapOtherMetas(numa_nodes) ← mmap 别 meta 的 GIM 进 0x700000000000
⑦ CXLSSD::Init(cxl, dram_MiB, flash_path, ssd_header, meta_num)
    └─ meta 0 一次性 reserve DRAM 区；其他人复用 base_offset_
⑧ CAS bit meta_num 进 CXLMemHeader::meta_finish_，等所有 meta 都置位
```

`DestroyDevice()` 反向 munmap + close + free，把 `gDevice` 设回 nullptr。

### 7.4 全局接口

```cpp
extern CXLDevice *gDevice;
#define gDualMem gDevice                    // 老代码兼容
using DualMemDevice = CXLDevice;            // 老类型兼容

// 启停
void InitDevice(meta_num, cxl_capacity_GiB, cxl_path, cxl_numa,
                gim_per_meta_MiB, gim_path, gim_numa,
                ssd_dram_MiB, flash_path);
void DestroyDevice();
```

`gDevice` 的方法清单（全部 inline 转发到底层模块）：

| 类别 | 方法 |
|------|------|
| CXL 内存 | `CXLReadSync` / `CXLWriteSync` / `CXLAtomicCasSync` / `CXLMemMalloc` / `CXLMemMallocPointer` / `CXLMemFree` / `CXLMemFreePointer` / `CXLMemReset` |
| GIM 本地 | `GIMMemMallocPointer` / `GIMMemFreePointer` / `GetGIMMemOffset` |
| GIM 远端 | `GIMReadSync(target, ...)` / `GIMWriteSync` / `GIMAtomicCasSync` / `GIMAtomicFAASync` |
| CXL-SSD | `SSDDramAlloc` / `SSDDramRead` / `SSDDramWrite` / `SSDDramBase` / `SSDDramCapacity` |

> SSD Flash 路径（`InodeWrite/Sync` 等）由 `gDevice->ssd->InodeXxx()` 直接调用，没有顶层包装——这部分调用集中在 `CXLPersistence::SSDInodeRegion` / `SSDDentRegion`。

---

## 8. 测试与基准

CXL 带宽数据见 [07-cxl-bandwidth-benchmark.md](07-cxl-bandwidth-benchmark.md)（用 `test/cxl/cxl_read_write_test.cc`，绕过 `INSERT_DELAY`）。

---

## 9. 编译开关

CXL 子系统本身没有编译开关（延迟通过 `*_DELAY_ITERS` 宏调），影响其行为的开关都在 `include/common/metadata_types.h`：

| 开关 | 默认 | 影响 |
|------|------|------|
| `USING_CXL_OFFSET` | 启用 | 上层用偏移而非裸指针访问 CXL |
| `USING_CXL_PERSISTENCE` | 启用 | 启用 `CXLPersistence`，依赖 CXLSSD |

> CXLDevice / CXLMem / GIMMem / CXLSSD 这一层不区分 `USING_LEVEL_HASHTABLE_BASELINE`、`USING_INODE_ARRAY` 等开关——这些只影响如何**使用**这些原语。
