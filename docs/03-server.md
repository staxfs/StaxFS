# 服务端

服务端单一二进制 `dfs-prototype-server` 同时承担两种角色：

- `data` — 块对象存储（`ObjectStore`），无 CXL 依赖
- `meta` — 元数据存储 + CXL 持久化 + 跨 MDS 同步

哪种角色由配置文件中的 `[meta]` / `[data]` 段决定。本文档以 meta 端为主，data 端只在涉及 RPC handler 时简述。

```
src/server_main.cc                ← 启停 / 配置 / handler 注册 / 线程模型
   ├─ src/server/metadata.cc      ← Metadata 类（路径解析、CRUD、buffer 池）
   ├─ src/server/data.cc          ← ObjectStore（cuckoohash_map 块存储）
   ├─ src/server/rpc_server.cc    ← Guardian / EventLoop 辅助、跨 MDS 通信
   └─ src/server/cxl_persistence.cc ← CompactWAL / RemoteInodeWAL / SSDInodeRegion / SSDDentRegion / 检查点循环

include/server/                   ← 上述各模块的接口与编译期开关
   ├─ abstract_md.h / abstract_data.h     —— 抽象基类（接口占位）
   ├─ metadata.h  / data.h
   ├─ rpc_server.h
   ├─ cxl_persistence.h
   ├─ compact_wal.h
   ├─ dent_region.h
   ├─ inode_array.h
   ├─ level_hashtable.h           —— TwoLevelHashtable（默认）
   ├─ concurrent_level_hashtable.h —— Level-Hashing 基准
   ├─ level_hashtable_traits.h    —— 编译期选型 + KeyTraits
   ├─ tag_hint.h                  —— TagHint 加速 find()
   ├─ hashtable_stats.h           —— LEVEL_HASHTABLE_STATS 计数
   └─ 
```

---

## 一、抽象层

### `abstract_md.h`

定义 `AbstractInodeStorage`（GetInode/PutInode/DeleteInode）和 `AbstractPosixMetadata`（Stat/Mkdir/Create/Unlink/Rename/Link/OpenDir/MetaSync）两个抽象类。当前实现只有 `Metadata` 一份；保留这两个基类是为了 metadata 与持久化层之间的解耦演进。

### `abstract_data.h`

定义 `AbstractDataStorage`，含 `AsyncReadChunk` / `AsyncWriteChunk`（基于 `ChunkIOCallback`）。当前 `ObjectStore` 只暴露同步 `Read/Write`，异步接口为占位。

返回码统一约定：

| 值 | 含义 |
|----|------|
| `0` | OK |
| `-1` | 通用失败 |
| `-EAGAIN` | WAL 满，应重试（CXLPersistence 反压） |
| `1`（仅 Rename） | 整个 rename 应当在 `next_meta_server_` 上重做 |

---

## 二、Metadata 类

`include/server/metadata.h`、`src/server/metadata.cc`。

### 2.1 数据成员

```cpp
class Metadata {
  std::atomic<uint64_t> min_unused_id_;     // 该 meta 待分配的下一个 inode id
  uint64_t              base_id_, max_id_;  // [base_id_, max_id_) 是该 meta 持有的区间

  DirentHashtable      *dhashtable_;        // (parent_id, name) → Dirent
  uint64_t              d_resize_meta_off_; // 共享 ResizeMeta 在 CXL 的偏移

#ifdef USING_INODE_ARRAY
  InodeArray           *ihashtable_;        // 直接映射 inode 槽
#else
  InodeHashtable       *ihashtable_;        // inode_id → Inode
  uint64_t              i_resize_meta_off_;
#endif

  int                   buffer_size_ = DIR_MAX_ALLOC; // 1 MB
  int                   pool_size_   = 10;
  std::vector<std::unique_ptr<char[]>> buffers_;
  std::atomic_bool      in_use_[10];

  PendingListings       pending_listings_;  // listdir 分页 staging
public:
  int meta_num_;
  int all_meta_num_;
};
```

`DirentHashtable` / `InodeHashtable` 的具体类型由 `level_hashtable_traits.h` 在编译期选定（见下文）。

### 2.2 三层方法

```cpp
// L1 —— 直接对哈希表做 CRUD
auto GetInode    (id, *result)          -> bool;
void PutInode    (id, *src);
void UpdateInode (id, *src);
void DeleteInode (id);
auto GetDents     (id, *buf, size, off) -> uint64_t;
auto GetDentViews (id, cutoff_ver, *buf, size, off) -> uint64_t;

// L2 —— 路径解析与 dirent 维护
auto LocateInode      (path, *parent_id_buf=nullptr,
                       *ent_ptr=nullptr, path_len=-1)   -> Result;
void AddDirectoryEntry(*entry, parent_id, update_parent=true);
void DeleteDirectoryEntry(*entry, parent_id, update_parent=true);
auto AllocateInodeId()                                  -> uint64_t;

// L3 —— POSIX 包装
auto Stat   (path, *buf)                                -> Result;
auto Unlink (path, is_dir=false)                        -> Result;
auto Rmdir  (path)                                      -> Result;   // = Unlink(path, true)
auto Access (path, mode, uid, gid)                      -> Result;
auto Chmod  (path, mode, uid, gid)                      -> Result;
auto Create (path, mode, uid, gid, *buf)                -> Result;
auto Mkdir  (path, mode, uid, gid)                      -> Result;   // = Create(buf=nullptr)
auto Rename (oldpath, newpath, link=false)              -> int;
auto Link   (oldpath, newpath)                          -> int;      // = Rename(link=true)
auto OpenDir(path, alloc=DIR_MAX_ALLOC)                 -> OpenDirResult;
auto MetaSync(id)                                       -> int;
```

`Result` 返回值：

```cpp
struct Result {
  int         mark_ = 0;     // 0 OK | -1 fail | -EAGAIN WAL 满 | -ECrossMeta(rename 用)
  uint64_t    id_   = 0;     // 找到/创建的 inode id
  std::string new_path_;     // 跨 meta 时给客户端的提示
};
struct OpenDirResult { Result res_; DirHandle handle_; bool has_handle_; };
struct RenameResult  { int mark_; std::string old_path_, new_path_; };
```

### 2.3 LocateInode（路径行走）

```
LocateInode("/dfs/a/b/c")
   │
   ├─ 从 kRootId(=2) 开始
   ├─ 逐段在 dhashtable_->find((parent_id, name), Dirent) 上查
   ├─ 命中且 dirent 在本 meta → 继续下一段
   ├─ 命中但 dirent 指向另一 meta（meta_num 由 inode_id 高位决定） →
   │      返回 Result{ mark_ = -1, id_ = 0, new_path_ = "剩余路径" }
   │      告诉客户端"再去 next_meta_server 试一次"
   └─ 末段命中 → Result{ mark_ = 0, id_ = 找到的 inode_id }
```

### 2.4 AcquireBuffer / ReleaseBuffer

10 个 1 MB 的预分配缓冲，供 `GetDents / GetDentViews / Listing 序列化` 复用。`AcquireBuffer` 自旋扫 `in_use_` 数组直到拿到空闲；`ReleaseBuffer` 释放回去。

### 2.5 PendingListings —— listdir 分页 staging

`opendir` 之后，客户端通常需要多次 `GetDentViews` 才能拉完一个大目录的全部 dirent。如果每次 RPC 都重新扫一遍 SSDDentRegion + WAL tail + 排序，开销极大。`PendingListings` 把首次为某 `(dir_id, cutoff_version)` 构造的"完整序列化字节流 + 记录边界数组"存下来，后续分页直接从同一份字节流中切片：

```cpp
class PendingListings {
public:
  struct Listing {
    uint64_t dir_id, cutoff;
    std::vector<char>   bytes;            // 已序列化的 DentView 流
    std::vector<size_t> record_offsets;   // 每条记录起点（O(log n) 切片对齐）
    std::chrono::steady_clock::time_point last_touch;
  };
  using ListingPtr = std::shared_ptr<Listing>;

  template <class Builder>
  ListingPtr GetOrStage(uint64_t dir_id, uint64_t cutoff, Builder &&builder);
  void       Release  (uint64_t dir_id, uint64_t cutoff);
  void       SweepExpired(std::chrono::milliseconds ttl);
};
```

终止 RPC（payload 为 0 字节）调用 `Release()` 释放；`SweepExpired()` 兜底回收挂掉客户端的残留。

---

## 三、ObjectStore（data 端）

`include/server/data.h` / `src/server/data.cc`：

```cpp
class ObjectStore {
public:
  static auto Init(string_view data_root_path) -> std::unique_ptr<ObjectStore>;
  ssize_t Read (blkid, offset, size, buf);
  ssize_t Write(blkid, offset, size, buf);   // upsert + auto-extend
  // 异步 API 占位，未实际使用
};
```

后端是 `libcuckoo::cuckoohash_map<uint64_t, std::vector<char>>`，整块对象在内存里——目前 data server 未做 io_uring 落盘。`data_root_path` 只用作"目录占位"，让 `setup-test-servers.sh` 知道服务已起好。

---

## 四、哈希表三选一

`level_hashtable_traits.h` 在编译期把 `DirentHashtable` 与 `InodeHashtable` 绑到下列实现之一：

| 开关 | DirentHashtable | InodeHashtable |
|------|-----------------|----------------|
| 默认（仅 `USING_LEVEL_HASHTABLE`） | `TwoLevelHashtable<DKeyPair, Dirent>` | `TwoLevelHashtable<uint64_t, Inode>` |
| 加 `USING_INODE_ARRAY` | 同上 | `InodeArray`（直接映射） |
| `USING_LEVEL_HASHTABLE_BASELINE` | `ConcurrentLevelHashtable<DKeyPair, Dirent>` | `ConcurrentLevelHashtable<uint64_t, Inode>` |

`KeyTraits` 与 `CLHKeyTraits` 是 `<K, V>` 上的特化，把"value 里能否还原 key"显式声明出来：

```cpp
// 例：(parent_id, filename) 与 Dirent 的双向映射
template <> struct KeyTraits<DKeyPair, Dirent> {
  static bool       match  (const Dirent &d, const DKeyPair &key);
  static DKeyPair   extract(const Dirent &d);
};
```

### 4.1 TwoLevelHashtable（`level_hashtable.h`，默认）

桶布局 1088 B：

```
┌──── 64 B Hash Region ─────┬──────────────── 8 × 128 B Value Region ────────────┐
│ meta[0..7]：                │  slot[0..7]：每槽容纳一个 Inode / Dirent (≤120 B) │
│ ┌─meta[0]: D-bit | ver(4b) │                                                    │
│ │         | fp(60b)        │                                                    │
│ │         (作为整桶的 CAS  │                                                    │
│ │         锁，写入路径要先  │                                                    │
│ │         CAS-lock 此 8B)  │                                                    │
│ └─meta[1..7]: 仅 fp(60b)   │                                                    │
└────────────────────────────┴────────────────────────────────────────────────────┘
```

关键常量：

| 常量 | 值 |
|------|-----|
| `kSlots` | 8 |
| `kValueSize` | 128 B |
| `kHashRegionSize` | 64 B |
| `kBucketSize` | 1088 B |
| `kDirtyBit` | `1ULL << 63` |
| `kVersionShift` | 60 |
| `kFpMask` | `(1ULL << 60) - 1` |
| `kResizeThreshod` | 0.8 |

读路径有三阶段：

```
TwoLevelHashtable::find(key, *out)
  ├─ Phase 1（DRAM 命中）
  │     · TagHint::test(bucket, fp32)  // AVX2 一次比 8 个 32-bit fp
  │     · 命中 → 跳到 Phase 2 校验完整 60-bit fp + key
  │     · 未命中 → 跳到 Phase 2 全桶扫描
  ├─ Phase 2（CXL 读 64 B Hash Region）
  │     · 比较 8 个 60-bit fp
  │     · fp 命中 → 读对应 128 B value，比较 key
  │     └─ 否则继续 Phase 3
  └─ Phase 3（脏位/版本号未稳定时持续重试，直到发起方完成 RMW）
```

写路径：

```
TwoLevelHashtable::insert / update / erase
  └─ CAS slot[0] meta 把 D-bit 置 1（占整桶的写锁）
     · 失败 → 自旋等 D-bit 清掉
     · 成功 → CXL 写 8 byte hash region + 128 B value（按需）
              → CAS unlock：D-bit 清 + ver++
```

渐进 resize：

```cpp
struct alignas(64) ResizeMeta {
  uint64_t ctrl, version;
  uint64_t tl_offset, tl_size;        // top level
  uint64_t bl_offset, bl_size;        // bottom level
  uint64_t old_bl_offset, old_bl_size;
};
```

发起 meta 把 ctrl 由 NORMAL CAS 到 MIGRATING，按 cursor 推进 [old_bl] 上的桶到新 TL；其他 meta 通过 GIM 上的 `GIMResizeNotify` 同步 cursor。新进 op 的 pre_op 在 ctrl != NORMAL 时阻塞，避免在 cursor 已扫过的范围上写丢。

### 4.2 ConcurrentLevelHashtable（`concurrent_level_hashtable.h`，基准）

paper-faithful Level-Hashing 实现。桶布局 512 B：

```
┌─ slot 0 ──────────────────────────────┐
│ meta(8 B) = D-bit | ver(3b) | fp(60b) │  ← CAS 锁与全桶共用
│ value(120 B)                          │
├─ slot 1 ──────────────────────────────┤
│ meta(8 B) = fp(60b)                   │
│ value(120 B)                          │
├─ slot 2, slot 3 同上                  │
└───────────────────────────────────────┘
```

关键常量：

| 常量 | 值 |
|------|-----|
| `kAssocNum` | 4 槽/桶 |
| `kSlotSize` | 128 B |
| `kValueSize` | 120 B |
| `kBucketSize` | 512 B |
| `kVersionShift` | 60 |
| `kVersionMask` | `0x7ULL << 60`（3 bit） |
| `kResizeThreshold` | 0.8 |

resize 是 stop-the-world 的：发起方 CAS ctrl 到 RESIZING，扫整张 old_BL 把元素重哈希到 new_TL，再 CAS 回 NORMAL（同时 version++）。pre_op 在 ctrl != NORMAL 时一律 catch_up 阻塞。

### 4.3 InodeArray（`inode_array.h`）

inode id 在每 meta 上单调分配，没有必要走哈希。`InodeArray` 用三维数组直接映射：

```
InodeArray[meta_id][segment_idx][slot_offset]
```

| 常量 | 值 |
|------|-----|
| `kSlotBits` | 20（每段 1 048 576 槽） |
| `kSlotSize` | 128 B（恰好 2 个 CXL cache line） |
| `kSegmentSize` | 128 MB |
| `kMaxSegments` | 1024（→ 每 meta 最大 128 GB inode 空间） |
| `kIAHeaderSize` | 64 B |
| `kBlockSize` | ≈ 8.1 KB（header + 1024 段偏移） |

`InodeArrayBlock` 头：

```cpp
struct alignas(64) InodeArrayHeader {
  uint64_t ctrl;          // [bit63] EXPANDING
  uint64_t num_segments;  // 已分配段数
  uint64_t base_id;       // 该 meta 的起始 inode id
  uint64_t meta_id;
  uint64_t version;       // 段分配时 ++
  char     reserved[24];
};
```

`InodeSlot`（128 B = 2 cache line，要求 `sizeof(Inode) == 88`）：

```
CL0 [0..63]:  control_(8 B: D-bit | ver7b | reserved | valid) + inode 前 56 B
CL1 [64..127]: inode 后 32 B + reserved_(32 B)
```

写入语义与 TwoLevelHashtable 类似：CAS control_ 的 D-bit 上锁，写完后清锁 + 7-bit 循环 version++（128 次回绕，避开 ABA）。

每个 meta 的段偏移登记在 `gDevice->dfs_header_->inode_array_block_offset_[meta_id]`，启动时各自填，其他 meta 通过 `SetMds()` 读取后即可访问别 meta 的 inode 槽。

---

## 五、TagHint（`tag_hint.h`）

每个 meta 自己 DRAM 里维护 `8 × 4 B / 桶` 的小阵列（约 1.5 MB），存 60-bit fp 的低 32 位。`find()` 第一阶段用 AVX2 一次比 8 个 tag，命中再去 CXL 读 hash region 校验完整 fp + key。**仅本 meta 持有，无 GIM 流量**。

```cpp
class TagHint {
public:
  static constexpr size_t kSlotsPerBucket = 8;
  explicit TagHint(size_t total_buckets);
  void set  (size_t bucket_id, int slot_idx, uint64_t fp);
  void clear(size_t bucket_id, int slot_idx);
  bool test (size_t bucket_id, uint64_t fp) const;        // AVX2 / scalar 兼容
  void copy_from(const TagHint &src, src_start, dst_start, count);
  void reset();
};
```

由 `LEVEL_HASHTABLE_TAG_HINT` 控制是否启用；只对 TwoLevelHashtable 生效。

---

## 六、hashtable_stats.h —— 计数器

`LEVEL_HASHTABLE_STATS` 启用时，把 TwoLevelHashtable 的 find/insert/update/erase 各自细分为：

```
calls / hits / misses
cand_probes
phase1_hint_neg / phase1_hint_pos / phase1_found / phase1_false_pos / phase1_reads
phase2_reads / phase2_found
phase3_retries
cxl_hr_reads / cxl_hr_writes / cxl_val_reads / cxl_val_writes
```

每组操作各 16 个原子计数器；关闭时所有宏退化为空。配套分析脚本 `scripts/analyze_hint.py` 把日志聚成表。

---

## 七、CompactWAL（`compact_wal.h`）

64 B 紧凑 WAL，覆盖 5 类元数据 op：CREATE / UNLINK / RENAME / SETATTR / LINK。

### 7.1 WALOp

```cpp
enum WALOp : uint8_t {
  kWALNoop    = 0,    // padding 或 overflow continuation
  kWALCreate  = 1,
  kWALUnlink  = 2,
  kWALRename  = 3,
  kWALSetattr = 4,
  kWALLink    = 5,
};
```

### 7.2 CompactLogEntry（64 B）

```
[0:4)   seq_        ready_bit(1) | cont_count(2) | local_seq(29)
[4]     op_         WALOp
[5]     flags_      reserved
[6]     name_len_   primary name length（RENAME: old_name_len）
[7]     aux_len_    RENAME: new_name_len；CREATE: dirent type
[8:16)  version_    HLC（physical_ms 46b | logical 14b | meta_id 8b）
[16:24) inode_id_
[24:32) parent_id_  RENAME: old_parent；LINK: new_parent
[32:40) aux_        CREATE: uid<<32|gid；RENAME: new_parent_id；其余 0
[40:44) mode_       CREATE/SETATTR
[44:64) name_[20]   inline name；超过 20 字节会续到 overflow 槽
```

### 7.3 CompactLogOverflow（64 B 续行）

```cpp
struct alignas(64) CompactLogOverflow {
  uint32_t magic_;         // kOverflowMagic = 0x4F564652  ("OVFR")
  uint32_t parent_seq_;    // 低 29 位 = primary 的 local_seq
  char     data_[56];      // 续行 name 字节
};
```

`name` 超长会被切成多片，每片占一个 64 B 槽，连续放在主 entry 之后。`seq_` 的 cont_count 字段（2 bit）记录最多 3 个 continuation 槽。

### 7.4 CompactWAL API

```cpp
class CompactWAL {
public:
  int       Init(CXLSSD *ssd, size_t capacity_entries, int mds_id);

  uint64_t  TryReserveSlots(uint64_t slots_needed);   // 返回槽起点；满了返回 sentinel
  uint64_t  Append(CompactLogEntry &e);               // 设 seq_ + version_ + clwb，返回 local_seq
  uint64_t  AppendWithOverflow(e, ovf_data, ovf_len);

  const CompactLogEntry    *At         (uint64_t pos);
  const CompactLogOverflow *OverflowAt (uint64_t pos);
  void                      ClearRange (begin, end);
  void                      AdvanceCheckpoint(uint64_t new_pos);

  // HLC
  uint64_t  BeginOperation(uint64_t observed);   // 进 op 时 merge-and-tick
  void      EndOperation();
  uint64_t  CurrentVersion() const;
  void      ObserveVersion(uint64_t version);
};
```

write path：

```
1. TryReserveSlots(n)            ← 原子 fetch_add，预占 n 个槽
2. 写 continuation 槽（如有）    ← magic_ + parent_seq_ + data
3. 写 primary entry 字段         ← op/version/inode_id/...
4. 设 seq_ = ready_bit | cont_count | local_seq    （这一刻才发布）
5. clwb 64 B → sfence            ← CXL-SSD 持久化
```

`seq_ == 0` 视为未发布，checkpoint 与回放都跳过。

---

## 八、RemoteInodeWAL（`cxl_persistence.h`）

跨 MDS 的 inode 变更 ring。`RemoteInodeWalEntry` 64 B：

```cpp
struct alignas(64) RemoteInodeWalEntry {
  uint32_t seq_;           // 0 = 未就绪
  uint8_t  op_;            // RemoteInodeChangeOp（见 01-common.md）
  uint8_t  reserved0_;
  uint16_t reserved1_;
  uint64_t version_;       // CompactWAL HLC version
  uint64_t inode_id_;
  int64_t  value_;
  char     padding_[32];
};
```

API：

```cpp
class RemoteInodeWAL {
public:
  int     Init(CXLSSD *ssd, size_t capacity_entries);    // capacity 必须 2^k
  size_t  TryAppendPartial(const std::vector<RemoteInodeChange> &changes);  // 部分入队
  const RemoteInodeWalEntry *At(uint64_t pos) const;
  uint64_t Head() const;
  uint64_t CheckpointPos() const;
  void     AdvanceCheckpoint(uint64_t new_pos);
  void     ClearRange(uint64_t begin, uint64_t end);
  size_t   Capacity() const;
};
```

`TryAppendPartial`：CAS 推 head_，剩余空间不足时只插入能容纳的前缀，剩下交给上层重试，避免阻塞。

---

## 九、CXLPersistence —— 主控

`include/server/cxl_persistence.h`：

```cpp
class CXLPersistence {
public:
  int         Init(CXLSSD *ssd, int server_id);

  CompactWAL     *WAL();
  RemoteInodeWAL *RemoteWAL();

  // 高层日志接口（Metadata 调）
  uint64_t LogCreate (inode_id, parent_id, mode, uid, gid, type, name, name_len);
  uint64_t LogUnlink (inode_id, parent_id, name, name_len);
  uint64_t LogRename (inode_id, old_parent, new_parent, old_name, old_len,
                      new_name, new_len, type);
  uint64_t LogSetattr(inode_id, mode);
  uint64_t LogLink   (inode_id, new_parent, new_name, new_len, type);

  void StartCheckpointThread(int idle_sleep_ms, int core_id = -1);
  void StopCheckpointThread();

  SSDInodeRegion *InodeRegion();
  SSDDentRegion  *DentRegion();

  void ConfigureClusterMetaCount(int count);
  size_t TryAppendReceivedRemoteInodeChanges(const vector<RemoteInodeChange> &);

  // 出站转发队列（由 Guardian 排空）
  std::mutex                                    &PendingForwardMu();
  std::vector<std::vector<RemoteInodeChange>>   &PendingForward();
  std::vector<size_t>                           &ForwardBatchCap();
  int                                            ServerId() const;
};

extern CXLPersistence *gCXLPersistence;
void InitCXLPersistence(int meta_num, int core_id = -1,
                        int idle_sleep_ms = CXLSSD_CHECKPOINT_INTERVAL_MS);
void DestroyCXLPersistence();
```

### 9.1 DoCheckpoint —— 五阶段循环

`src/server/cxl_persistence.cc`：

```
DoCheckpoint(force_sync = false)
   │
   ├─ ① 扫主 WAL [pending_main_pos_, head_)
   │     · 解码 CompactLogEntry（含 overflow 拼名）
   │     · CREATE / UNLINK / RENAME / SETATTR / LINK 重放
   │       → SSDInodeRegion::WriteInode / MutateInode
   │       → SSDDentRegion::AppendEntry / DeleteEntry（按 4 KB 页桶聚合）
   │     · 跨 MDS 影响（如 parent owner 不在本 meta）
   │       → queue_remote_change(target_meta, op, inode_id, value, version)
   │
   ├─ ② 扫 RemoteInodeWAL [pending_remote_pos_, head_)
   │     · 进 deferred_remote_ops_ 临时 vector
   │
   ├─ ③ 合并 deferred_remote_ops_
   │     · 按 (inode_id << 4) | op_ 哈希到 coalesced
   │     · kRemoteParentBlocksDelta：value_ 累加；version 取大
   │     · kRemoteInodeSetattr / kRemoteInodeTouchCtime：latest-wins
   │     · kRemoteInodeUnlink / kRemoteInodeLink：保留每条（顺序敏感）
   │
   ├─ ④ 按 4 KB 页桶 apply
   │     · 同一 PageOffsetFor(inode_id) 的变更聚到一起，避免反复 LoadPage
   │     · 目标 inode 还没在 SSD 上 → 推回 deferred_remote_ops_，下次再试
   │     · deferred_remote_ops_ 容量过 kDeferredHardLimit 时按 version 截断（带 WARN 日志）
   │
   └─ ⑤ sync 时机判定（drained ≥ kSyncBatchThreshold || 时间窗到 || force_sync）
        · SSDInodeRegion::FlushDirty() + ssd->InodeSync()
        · SSDDentRegion ::FlushDirty() + ssd->DentSync()
        · CompactWAL::AdvanceCheckpoint(pending_main_pos_)
        · RemoteInodeWAL::AdvanceCheckpoint(pending_remote_pos_)
        · last_sync_ts_ = now()
```

关键阈值与位置：

| 名字 | 值 | 文件 |
|------|-----|------|
| `kSyncBatchThreshold` | `32 * 1024` | `cxl_persistence.cc` |
| `CXLSSD_CHECKPOINT_INTERVAL_MS` | 1 ms | `metadata_types.h` |
| `kDeferredHardLimit` | （硬上限） | `cxl_persistence.cc` |

不到阈值时 `DoCheckpoint` 只推进 `pending_*_pos_` 与 `pending_remote_batches_`，不真做 flush，下个 cycle 接力。

### 9.2 出站转发

`PendingForward()` 是按 target meta 编号下标的 `std::vector<RemoteInodeChange>` 二维结构。Guardian 线程按节奏调 `ServicePendingRemoteInodeChanges(ctx)`：

```cpp
// src/server/rpc_server.cc
constexpr size_t kForwardBatchFloor   = 512;
constexpr size_t kForwardBatchCeil    = 65536;
constexpr size_t kForwardBatchInitial = 4096;

ServicePendingRemoteInodeChanges(ctx)
  ├─ 加锁取出 PendingForward + ForwardBatchCap
  ├─ 对每个 target meta t：
  │     · 取 caps[t] 条出来打包成 MDPersistenceRequest
  │     · payload < MTU → kMetaPersistenceReq
  │     · payload ≥ MTU → kMetaPersistenceReqSplit（按 5 byte 头分片）
  │     · 调 GuardianToServerMetaCall / ...Split 同步发送
  │     · 成功 → caps[t] = min(caps[t]*2, kForwardBatchCeil)
  │             不成功（部分入队）→ caps[t] = max(inserted, kForwardBatchFloor)
  └─ 没全发完则下次再来
```

### 9.3 入站

```cpp
// 由 MDPersistenceReqHandler / MDPersistenceReqSplitHandler 调用
size_t TryAppendReceivedRemoteInodeChanges(const std::vector<RemoteInodeChange> &);
```

非阻塞：内部走 `RemoteInodeWAL::TryAppendPartial`，能塞多少塞多少，剩下让对端重传。

---

## 十、SSD region —— 落盘

### 10.1 SSDInodeRegion

固定槽分配：`inode_id → file_offset = (inode_id % max_inodes) * 128 B`。4 KB 页内 32 个槽。

主要数据结构（`include/server/cxl_persistence.h`）：

```cpp
class SSDInodeRegion {
public:
  int Init(CXLSSD *ssd, uint64_t capacity_bytes);

  void WriteInode (uint64_t id, const void *data, size_t len);
  void ReadInode  (uint64_t id, void *buf, size_t len);
  template <class Fn>
  bool MutateInode(uint64_t id, Fn &&fn);             // 命中 + dirty 一次完成
  uint64_t PageOffsetFor(uint64_t id) const;          // checkpoint 桶聚用

  PageFlushStats FlushDirty();   // 把 LRU 链上 dirty 页 pwrite 到 ssd
  uint64_t       Capacity() const;
};
```

关键常量：

| 常量 | 值 |
|------|-----|
| `kSlotSize` | 128 B |
| `kPageSize` | 4 KB |
| `kSlotsPerPage` | 32 |
| `kPageCacheMaxPages` | 64K（→ 256 MB DRAM cache） |

页缓存：

- `pool_[kPageCacheMaxPages]` 一段连续内存，每个 `PageCacheEntry` 64 B + 4 KB 页
- `page_slots_[num_page_slots_]` 稠密下标（page_off / kPageSize）→ pool 索引；空 = `UINT32_MAX`
- LRU 链通过 `prev / next` 字段做 intrusive doubly-linked，splice = 4 次索引写
- 命中 → 移动到 LRU 头；未命中 → 从 LRU 尾驱逐（脏则就地 pwrite，并把 evict_pages_ / evict_bytes_ 累计到下次 `FlushDirty` 的返回值里）

### 10.2 SSDDentRegion（`dent_region.h`）

每目录一条 4 KB DirPage 链，append-only：

```cpp
struct __attribute__((packed)) DirPageHeader {
  uint64_t dir_id_;          // 8 B
  uint64_t next_page_off_;   // 8 B  下一页偏移；0 表示链尾
  uint16_t used_bytes_;      // 2 B  data_ 已用字节
  uint16_t entry_count_;     // 2 B  PUT + DEL 总条数
  uint16_t put_count_;       // 2 B  PUT 数（不维护 live set，只用作启发）
};
```

每条记录前 20 B 是 header：

```cpp
struct __attribute__((packed)) DentRecordHeader {
  uint64_t inode_id_;
  uint64_t version_;
  uint16_t reclen_;
  uint8_t  type_;
  uint8_t  flags_;           // bit0 = tombstone（DEL 事件）
};
static_assert(sizeof(DentRecordHeader) == 20);
```

写路径：

- WAL CREATE → `AppendEntry(dir_id, DentEntry, version)` 追加 PUT
- WAL UNLINK → `DeleteEntry(dir_id, inode_id, name, version)` 追加 DEL
- WAL RENAME → 旧目录 DELETE + 新目录 APPEND

读路径：

```cpp
int ReadDirLatest(uint64_t dir_id, std::vector<DentEntry> &entries);
```

按链遍历所有 DirPage，把同 name 的事件按 `version_` 取最大；如果最大那条是 tombstone 则丢掉，否则计入。WAL tail 那段还没 checkpoint 的事件由调用方 (`Metadata::CollectPersistentDentViews`) 在 `ReadDirLatest` 之后再 merge 一遍，最后再排序。

`PageFlushStats` 同时被 inode/dent 两个 region 复用，给 `CHECKPOINT_STATS_PROFILE` 上报真实磁盘字节。

---

## 十一、s2fifo_cache.h（旧缓存）

```cpp
template <typename Key, typename Value, typename Hash, typename KeyEq>
class S2FIFOCache {
  S2FIFOCache(int meta_num, size_t main_cache_size);
  bool find  (const Key &k, Value &v);
  bool insert(const Key &k, const Value &v);
  bool update(const Key &k, const Value &v);
  bool erase (const Key &k);
  // 各种原子计数器：hit_count_, miss_count_, hit_time_, ...
};
```

设计上是"小队列 + 主队列 + CascadeHashtable 索引"的两段 FIFO，配合 `DirentCache / InodeCache` 子类做 metadata 缓存。当前 `CACHE_SKIP` 默认关闭——所有 op 直接打哈希表，缓存是 no-op；这套 API 主要保留做对比基线。

---

## 十二、rpc_server.h —— 跨 MDS 通信

```cpp
struct ServerContext {
  erpc::Rpc<erpc::CTransport> *rpc_;
  size_t                       rpc_id_;
  std::vector<int64_t>         nr_reqs_;     // 每 meta 的请求量（心跳上报用）
  int                          meta_num_;
  std::vector<std::string>     meta_uri_list_;
  std::vector<int>             meta_rpc_num_;
  std::vector<int>             meta_to_meta_sessions_;
};

struct GuardianContext {
  erpc::Rpc<erpc::CTransport>  *rpc_;
  size_t                        rpc_id_;
  int                           meta_num_;
  std::vector<std::string>      meta_uri_list_;
  std::vector<int>              meta_rpc_num_;
  std::vector<std::vector<int>> meta_to_meta_sessions_;   // [meta][thread]
  std::vector<int64_t>          reqs_;                    // 汇总各 EventLoop 的累计量
  std::atomic<uint32_t>         heart_num_{0};
};

// 同步 RPC 包装
auto GuardianToGuardianMetaCall      (fbb, req_type, meta_num, ctx, resp_size = 1) -> erpc::MsgBuffer;
auto GuardianToGuardianMetaCallSplit (fbb, req_type, meta_num, ctx, resp_size = 1) -> erpc::MsgBuffer;
auto GuardianToServerMetaCall        (fbb, req_type, meta_num, ctx, resp_size = 1) -> erpc::MsgBuffer;
auto GuardianToServerMetaCallSplit   (fbb, req_type, meta_num, ctx, resp_size = 1) -> erpc::MsgBuffer;
auto ServerMetaCall                  (fbb, req_type, meta_num, sctx, resp_size = 300) -> erpc::MsgBuffer;

// Guardian 周期性活动
auto Heartbeat(GuardianContext &ctx) -> struct timespec;
void ServicePendingRemoteInodeChanges(GuardianContext &ctx);
```

`Split` 版本为 payload 超过 eRPC MTU 时分片发送，前 5 byte 头编码 `[last_flag][meta_num]`。

---

## 十三、`server_main.cc` —— 启动与请求分发

### 13.1 全局状态

```cpp
std::unique_ptr<ObjectStore>  gStoreManager;     // data 角色
std::unique_ptr<Metadata>     gMetaManager;      // meta 角色
std::atomic<bool>             gShouldExit{false};

std::vector<int64_t>          gReqs;             // 各 meta 的请求量快照
std::atomic<uint64_t>         gReqsNum;          // bitmask: 第 i 位表示 meta i 已上报本周期
std::atomic<int>              gConnectedSessionNum;

extern int                    gHeartbeatCycle;   // 10 s（rpc_server.cc 中定义）
```

### 13.2 启动序列

```
main(argc, argv)
   │
   ├─ ParseConfig / signal handler / numa 检测
   │
   ├─ data 角色：
   │   · gStoreManager = ObjectStore::Init(data_root_path)
   │   · register_req_func(kHelloReq, HelloReqHandler)
   │   · register_req_func(kDataReq , IoReqHandler)
   │   · 每个 core_ids[i] 起一个 EventLoopThread
   │
   └─ meta 角色：
       · 扫描 conf 找出所有 meta server，构造 meta_uri_list / meta_rpc_num
       · InitDevice(meta_id, CXL_CAPACITY, ...)        ← 02-cxl.md
       · InitCXLPersistence(meta_id, core_ids[last], CXLSSD_CHECKPOINT_INTERVAL_MS)
       · gMetaManager = Metadata::Init(data_root_path, meta_id)
       · gMetaManager->SetMetaNum(all_meta_num)
       · ConfigureClusterMetaCount(all_meta_num)
       │
       · register_req_func 11 个 handler（见下表）
       │
       · core_ids[0]    : GuardianThread (rpc_id = 0)
       · core_ids[1..n-2]: EventLoopThread × (n-2) (rpc_id = 1..n-2)
       · core_ids[n-1]  : Checkpoint 线程（CXLPersistence::CheckpointLoop，无 rpc 实例）
```

### 13.3 Handler 表

| RPCType | handler | 角色 |
|---------|---------|------|
| `kHelloReq` (1) | `HelloReqHandler` | data + meta |
| `kMetaGeneralReq` (2) | `MDGeneralReqHandler` | meta（Inode Get/Put）|
| `kMetaPathCommonReq` (3) | `MDPathCommonReqHandler` | meta（Stat/Mkdir/Create/Unlink/Rmdir/Rename/Link/Chmod/Access/OpenDir）|
| `kMetaFDCommonReq` (4) | `MDFDCommonReqHandler` | meta（GetDents/GetDentViews）|
| `kDataReq` (5) | `IoReqHandler` | data（ObjectStore Read/Write）|
| `kMetaCommunicationReq` (6) | `MDMetaCommunicationReqHandler` | meta server 间协调 |
| `kGuardianCommunicationReq` (7) | `MDGuardianCommunicationReqHandler` | Guardian 间通信 |
| `kTestMetaCommunicationReq` (8) | `MDTestCommunicationReqHandler` | 测试用 |
| `kGuardianCommonReq` (9) | `MDGuardianCommonReqHandler` | Heartbeat + 触发负载均衡 |
| `kMetaPersistenceReq` (14) | `MDPersistenceReqHandler` | 接收远端 inode 变更 |
| `kMetaPersistenceReqSplit` (15) | `MDPersistenceReqSplitHandler` | 分片版 |

### 13.4 线程职责

```
GuardianThread (core_ids[0], rpc_id = 0)
   │
   ├─ run_event_loop_once 处理本端到达的请求
   ├─ 每 gHeartbeatCycle (10 s) 把累计 reqs_ 通过 GuardianCommonReq 上报到 meta 0
   ├─ 周期 ServicePendingRemoteInodeChanges(ctx)（从 CXLPersistence::PendingForward
   │   排空，按 batch cap 自适应增减、按需走 split RPC）
   └─ 把 PendingListings::SweepExpired() 这类家政活也放在 Guardian

EventLoopThread (core_ids[i], i ∈ [1..n-2])
   │
   ├─ run_event_loop_once 处理客户端 RPC
   ├─ 每条 op 进入时构造 MetadataOperationClockGuard（CompactWAL::Begin/EndOperation）
   ├─ 失败路径：WAL 满 → 返回 -EAGAIN，触发客户端重试
   └─ 把本线程处理的请求量累加进 nr_reqs_，等 Guardian 收

Checkpoint Thread (core_ids[n-1])
   │
   └─ CXLPersistence::CheckpointLoop(idle_sleep_ms, core_id)
        · 调 DoCheckpoint() 直到 head_ == checkpoint_pos_
        · 没事干 → 睡 CXLSSD_CHECKPOINT_INTERVAL_MS = 1 ms
```

---

## 十四、并发原语速查

| 组件 | 锁/原子原语 | 用途 |
|------|-------------|------|
| `TwoLevelHashtable` 桶 | meta[0] 的 D-bit + 4-bit version（CAS） | 桶级 RMW 锁 |
| `ConcurrentLevelHashtable` 桶 | slot[0] 的 D-bit + 3-bit version（CAS） | 同上 |
| `InodeArray` 槽 | control_ 的 D-bit + 7-bit version | 槽级 RMW |
| TwoLevelHashtable resize | `ResizeMeta::ctrl` CAS + GIM `GIMResizeNotify` | 渐进 resize 的发起/感知 |
| ConcurrentLevelHashtable resize | `CLHResizeMeta::ctrl` CAS | stop-the-world resize |
| `CompactWAL` ring | `head_` 原子 fetch_add | 无锁追加 |
| `RemoteInodeWAL` ring | `head_` CAS | 无锁追加（partial-insert） |
| `SSDInodeRegion` 页缓存 | 无锁（仅 checkpoint 线程写） | 单写者，readdir 走 SSDDentRegion |
| `SSDDentRegion` DirPage 链 | append-only + 单写 | 无锁 |
| `PendingListings` | `std::mutex` + map | listdir staging |
| `CXLPersistence::pending_forward_` | `std::mutex` | 出站转发缓冲 |
| `Metadata::buffers_[]` | `std::atomic_bool in_use_[10]` | 1 MB 缓冲池 |

---

## 十五、跨 MDS 同步流程图

```
Meta A 上：客户端 Create("/foo"，foo 的 inode 落 Meta B)
   │
   ▼
Metadata::Create
   ├─ ihashtable_->insert(foo_inode)            （仅 Meta A 视角的临时写）
   ├─ AddDirectoryEntry(parent, "foo")          （dirent 落到拥有 parent 的 meta）
   └─ CXLPersistence::LogCreate(... parent ...)
         · CompactWAL::Append → ready
         · 若 parent_owner != A (= B) :
              queue_remote_change(B, kRemoteParentBlocksDelta, parent_id, +1, version)

           ──── 本端 ack 客户端 ────

   ┌──── checkpoint 线程（异步） ────┐
   │ DoCheckpoint:                    │
   │  · 主 WAL 重放：apply 到 SSD     │
   │  · 出站：把上面的 +1 增量塞进     │
   │    PendingForward[B] 队列        │
   └──────────────────────────────────┘
                 │
                 ▼
       Guardian 线程 ServicePendingRemoteInodeChanges
                 │
                 │ kMetaPersistenceReq / kMetaPersistenceReqSplit
                 ▼
Meta B 上：MDPersistenceReqHandler
   │
   ▼
CXLPersistence::TryAppendReceivedRemoteInodeChanges
   │  (RemoteInodeWAL::TryAppendPartial)
   │
   ▼
   B 的 checkpoint 线程
   ├─ 扫 RemoteInodeWAL 进 deferred_remote_ops_
   ├─ 合并：同 inode 同 op 的多条 → 一条（参见 §九 ④）
   └─ 应用到 SSDInodeRegion（若 inode 还没出现 → 推回 deferred 等下次）
```

---

## 十六、淘汰组件

下列结构在源码里仍可见，但已不再用于元数据存储路径，留下只为兼容和对比基线：

- `CascadeHashtable / CXLCascadeHashtable`（`hashtable.h`）—— 旧链式扩张哈希；新代码不引用，但 `S2FIFOCache` 还把它当索引

新代码直接选用 `TwoLevelHashtable` / `InodeArray` / `CompactWAL` 这条主线。
