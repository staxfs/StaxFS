// cxl_read_write_test.cc
/*
Compile (Sapphire Rapids 及以上，需要 AVX-512F/BW 支持 _mm512_stream_*):

g++ -O3 -mavx512f -mavx512bw -std=gnu++17 -pthread cxl_read_write_test.cc \
    -lnuma -o cxl_read_write
# 或者直接 -march=native

Run:

sudo sh -c 'echo 5200 > \
/sys/devices/system/node/node4/hugepages/hugepages-2048kB/nr_hugepages'
echo 0 | sudo tee /proc/sys/kernel/numa_balancing
sudo ./cxl_read_write > cxl_read_write.log 2>&1
echo 1 | sudo tee /proc/sys/kernel/numa_balancing

读写都走 non-temporal (NT) 路径：
  - 写：_mm512_stream_si512 (VMOVNTDQ)
        真·绕过 cache，无 RFO，走 WCB → 内存控制器；
        worker 退出前 _mm_sfence() flush WCB。
  - 读：_mm512_stream_load_si512 (VMOVNTDQA)
        NT load 提示。注意：对 WB (write-back) memory 类型（普通 hugepage 即
        WB），这条指令在 Intel 文档里仍会进 cache，只是带 NTA hint 减少污染；
        真正"绕过 cache 的读"需要 WC/UC 映射或显式 clflush，那个开销比较大。
        这里靠 10 GiB 工作集（≫ L3 ≈ 60 MB on 6448H）让 cache 命中率接近 0，
        加 NT load hint 进一步减少 prefetch 进 LLC，足以反映 CXL 真实读带宽。
每次 inner-loop 单位 = 64 B（1 条 cache line / 1 条 ZMM 指令）。
*/

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <numa.h>
#include <numaif.h>
#include <pthread.h>
#include <sched.h>
#include <set>
#include <string>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

// glibc 默认的 sys/mman.h 不一定导出 MAP_HUGE_2MB，但 Linux kernel 支持 (21 <<
// MAP_HUGE_SHIFT)，其中 MAP_HUGE_SHIFT=26 表示 hugepage size 编码在
// flags[31:26] = log2(page_size_bytes)。
#ifndef MAP_HUGE_SHIFT
  #define MAP_HUGE_SHIFT 26
#endif
#ifndef MAP_HUGE_2MB
  #define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif

// 总测试内存大小：10 GB
static const size_t MEM_SIZE = 10ULL * 1024ULL * 1024ULL * 1024ULL;
// 2 MB hugepage 大小
static const size_t HUGE_PAGE_SIZE = 2ULL * 1024ULL * 1024ULL;
// 64 B 缓存行对齐
static const size_t ALIGN = 64;
// 每个 (threads, grain, op) 配置的测试时长（秒）
static const double TEST_DURATION_SEC = 5.0;
// CXL 内存所在 NUMA 节点
static const int CXL_NODE = 4;
// 发起访问的 CPU 所在 NUMA 节点（CXL 内存挂载到的本地节点）
static const int LOCAL_NODE = 1;

// 访问粒度列表：64/128/256/512/1024 B
static const size_t GRAIN_LIST[] = {64, 128, 256, 512, 1024};
static const int NUM_GRAINS = sizeof(GRAIN_LIST) / sizeof(GRAIN_LIST[0]);
// 测试的线程数列表
static const int THREAD_COUNTS[] = {1, 2, 4, 8, 16, 32};
static const int NUM_THREAD_COUNTS =
    sizeof(THREAD_COUNTS) / sizeof(THREAD_COUNTS[0]);

// 全局 sink，用 volatile 接收 reduce 结果以防整个内层循环被优化掉。
// 不在 hot path 里写，只在每个 worker 退出时写一次。
static volatile uint64_t g_sink = 0;

static double now_sec() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec + tv.tv_usec * 1e-6;
}

// set_mempolicy / get_mempolicy / mbind 的 nodemask 第二参数 maxnode 是
// "**位数**"，且 kernel 要求大于 max valid node 编号；这里给 64 位足够。
static const unsigned long MAXNODE_BITS = 64;

// 关键修复：set_mempolicy(MPOL_BIND) 必须在 mmap(MAP_HUGETLB) 之**前**调用。
// MAP_HUGETLB 是急切分配（mmap 时就从 hugepage 池抢页），kernel 选哪个 node
// 的池取决于调用线程当前的 memory policy；如果先 mmap 后 mbind，hugepage 已经
// 从错误的池里拿走了，且 hugetlb 在大多数内核上不支持 MPOL_MF_MOVE，导致
// mbind 静默无效。set_mempolicy 之后 hugepage 会从指定 node 的池里分配。
//
// 失败时回退到 numa_alloc_onnode + MADV_HUGEPAGE。
static char *alloc_on_node(size_t size, int node) {
  size_t aligned = (size + HUGE_PAGE_SIZE - 1) & ~(HUGE_PAGE_SIZE - 1);
  unsigned long nodemask = 1UL << node;

  // 1) 先把当前线程的 mempolicy 限制到目标 node
  if (set_mempolicy(MPOL_BIND, &nodemask, MAXNODE_BITS) != 0) {
    fprintf(stderr, "set_mempolicy(MPOL_BIND, node=%d) failed: %s\n", node,
            strerror(errno));
  }

  // 2) mmap 此时分配的 hugepage 会从 node 的池里取
  void *p =
      mmap(nullptr, aligned, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_2MB, -1, 0);

  // 3) 恢复默认策略，避免影响后续 std::vector / new 等本地分配
  set_mempolicy(MPOL_DEFAULT, nullptr, 0);

  if (p != MAP_FAILED) {
    // 冗余保险：把 VMA 也 mbind 上，hugepage 已经分配但策略持久化在 vma 上，
    // 后续如果有任何 fault-in（理论上不会，因为 MAP_HUGETLB 已经全部预分配），
    // 也会被绑到目标 node。
    if (mbind(p, aligned, MPOL_BIND, &nodemask, MAXNODE_BITS, MPOL_MF_STRICT) !=
        0) {
      fprintf(stderr,
              "mbind(strict) on hugepage region failed: %s "
              "(non-fatal if pages already on node %d)\n",
              strerror(errno), node);
    }
    fprintf(stderr, "Allocated %zu MB with 2 MB hugepages on node %d\n",
            aligned >> 20, node);
    return reinterpret_cast<char *>(p);
  }

  fprintf(stderr,
          "MAP_HUGETLB failed (%s); falling back to numa_alloc_onnode + "
          "MADV_HUGEPAGE\n",
          strerror(errno));
  char *buf = reinterpret_cast<char *>(numa_alloc_onnode(size, node));
  if (!buf) {
    fprintf(stderr, "numa_alloc_onnode failed\n");
    return nullptr;
  }
  if (madvise(buf, size, MADV_HUGEPAGE) != 0) {
    perror("madvise(MADV_HUGEPAGE)");
  }
  return buf;
}

// 用 move_pages(pid=0, nodes=NULL) 抽样验证 buf 区间的实际所在 node。返回
// 不在 expected_node 上的采样数；首次发现错位会打印诊断。
static int verify_pages_on_node(void *buf, size_t size, int expected_node) {
  const size_t N = 16;
  void *pages[N];
  int statuses[N];
  for (size_t i = 0; i < N; ++i) {
    statuses[i] = -1;
    pages[i] = static_cast<char *>(buf) + (i * (size / N));
  }
  // 显式触摸一下，避免 status = -ENOENT (不存在的页)；first-touch 后这里其实
  // 已经在 main 的 memset 里完成了，但保险起见
  for (size_t i = 0; i < N; ++i) {
    *static_cast<volatile char *>(pages[i]) = 0;
  }
  if (move_pages(0, N, pages, nullptr, statuses, 0) != 0) {
    perror("move_pages");
    return -1;
  }
  int wrong = 0;
  for (size_t i = 0; i < N; ++i) {
    if (statuses[i] < 0) {
      fprintf(stderr, "verify: page %zu at %p has status %d (errno-like)\n", i,
              pages[i], statuses[i]);
      ++wrong;
    } else if (statuses[i] != expected_node) {
      fprintf(stderr, "verify: page %zu at %p is on node %d, expected %d\n", i,
              pages[i], statuses[i], expected_node);
      ++wrong;
    }
  }
  if (wrong == 0) {
    fprintf(stderr, "verify: all %zu sampled pages are on node %d\n", N,
            expected_node);
  }
  return wrong;
}

// 在 NUMA 节点 node 上挑出 max_count 个**不同物理核**的逻辑 CPU 编号。
// 通过 /sys/devices/system/cpu/cpu*/topology/thread_siblings_list 对 SMT
// 兄弟核去重。 失败/挑不够时返回已有的部分。
static std::vector<int> pick_physical_cores_on_node(int node, int max_count) {
  std::vector<int> result;
  std::set<std::string> seen_siblings;

  int max_cpu = numa_num_configured_cpus();
  for (int cpu = 0; cpu < max_cpu && (int)result.size() < max_count; ++cpu) {
    if (numa_node_of_cpu(cpu) != node)
      continue;

    char path[256];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list",
             cpu);
    FILE *f = fopen(path, "r");
    if (!f)
      continue;
    char line[256] = {0};
    if (!fgets(line, sizeof(line), f)) {
      fclose(f);
      continue;
    }
    fclose(f);

    std::string siblings(line);
    if (!siblings.empty() && siblings.back() == '\n')
      siblings.pop_back();
    if (seen_siblings.count(siblings))
      continue; // 已经从这个物理核挑过 1 个 SMT thread 了
    seen_siblings.insert(siblings);
    result.push_back(cpu);
  }
  return result;
}

struct ThreadArg {
  char *buf;
  size_t mem_size;
  size_t grain;
  int cpu_id;
  double *bytes_processed;
  uint64_t seed;
  std::atomic<bool> *stop_flag;
};

// 快速 RNG：xorshift64，单次 ~1 ns。比 mt19937_64 (~3-5 ns) 显著轻，
// 避免在 64 B grain 这种"内层非常短"的循环里被 RNG 拖慢。
static inline uint64_t xorshift64(uint64_t *s) {
  uint64_t x = *s;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *s = x;
  return x;
}

// Lemire nearly-divisionless bounded random：避免取模。
static inline uint64_t bounded_rand(uint64_t *state, uint64_t range) {
  uint64_t x = xorshift64(state);
  return (uint64_t)(((__uint128_t)x * range) >> 64);
}

// 读 worker：每次外层迭代 = 一次随机跳 + 顺序访问 grain 字节（每次 64 B = 1
// 条 cache line = 1 条 ZMM stream-load）。
// 用 _mm512_stream_load_si512 给 CPU NT load 提示，减少 prefetch / 缓存污染；
// 配合 10 GiB 工作集（远大于 L3）让命中率接近 0，达到"等效绕开 cache"。
//
// 小 grain (=64) 是纯 random small access：每次 iter 只摸 1 条 cache line，
// prefetcher 完全失效。大 grain 是 random base + 顺序 burst，burst 内仍能被
// L2 stream prefetcher 部分流水化，对应应用读"一个 N 字节对象"的真实模式。
static void *worker_read(void *arg) {
  ThreadArg *t = reinterpret_cast<ThreadArg *>(arg);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t->cpu_id, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
    perror("sched_setaffinity");
  }

  const size_t grain = t->grain;
  // 每个 grain 内 64-byte cache line 数（也就是 ZMM 寄存器数）
  const size_t lines_per_grain = grain / 64;
  // 起点必须保证 grain 字节都落在 buf 内
  const uint64_t range = (t->mem_size - grain) / ALIGN + 1;
  uint64_t rng = t->seed | 1; // xorshift64 需要非零 state

  __m512i acc = _mm512_setzero_si512();
  double bytes = 0.0;

  while (!t->stop_flag->load(std::memory_order_relaxed)) {
    uint64_t idx = bounded_rand(&rng, range);
    const __m512i *src =
        reinterpret_cast<const __m512i *>(t->buf + idx * ALIGN);
    for (size_t k = 0; k < lines_per_grain; ++k) {
      // VMOVNTDQA：NT load。对 WB 内存仍会进 cache，但带 NTA hint，硬件
      // 实现上会减少 LLC 污染；真正绕开靠 working-set 远大于 L3。
      __m512i v = _mm512_stream_load_si512(
          const_cast<void *>(reinterpret_cast<const void *>(src + k)));
      acc = _mm512_xor_si512(acc, v);
    }
    bytes += grain;
  }

  *(t->bytes_processed) = bytes;

  // reduce acc 到全局 sink，防止整个内层循环被优化掉
  alignas(64) uint64_t parts[8];
  _mm512_store_si512(reinterpret_cast<__m512i *>(parts), acc);
  uint64_t reduced = 0;
  for (int i = 0; i < 8; ++i)
    reduced ^= parts[i];
  g_sink ^= reduced;

  return nullptr;
}

// 写 worker：每次外层迭代 = 一次随机跳 + 顺序写 grain 字节（每次 64 B = 1
// 条 cache line = 1 条 ZMM stream-store）。
// _mm512_stream_si512 (VMOVNTDQ) 是真·NT store：
//   - 不进 cache（不污染 L1/L2/L3）
//   - 不触发 RFO（不需要先把 cache line 读进来）
//   - 数据走 WCB → memory controller，链路上跑 1 份 64 B = 报告字节数
// 与 Intel MLC `-W6` 的写语义一致，可直接对比。
// 出循环前 _mm_sfence() 把 WCB 里残留的写刷下去。
static void *worker_write(void *arg) {
  ThreadArg *t = reinterpret_cast<ThreadArg *>(arg);

  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(t->cpu_id, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
    perror("sched_setaffinity");
  }

  const size_t grain = t->grain;
  const size_t lines_per_grain = grain / 64;
  const uint64_t range = (t->mem_size - grain) / ALIGN + 1;
  uint64_t rng = t->seed | 1;

  const __m512i pattern = _mm512_set1_epi8(0x5a);
  double bytes = 0.0;

  while (!t->stop_flag->load(std::memory_order_relaxed)) {
    uint64_t idx = bounded_rand(&rng, range);
    __m512i *dst = reinterpret_cast<__m512i *>(t->buf + idx * ALIGN);
    for (size_t k = 0; k < lines_per_grain; ++k) {
      _mm512_stream_si512(dst + k, pattern);
    }
    bytes += grain;
  }

  // flush WCB 中尚未提交到内存控制器的 NT store
  _mm_sfence();

  *(t->bytes_processed) = bytes;
  return nullptr;
}

struct ResultEntry {
  int threads;
  size_t grain;
  bool is_write;
  double seconds;
  double bandwidth_gbps;
};

int main() {
  if (numa_available() < 0) {
    fprintf(stderr, "libnuma not available\n");
    return 1;
  }

  // 1. 在 CXL_NODE 上分配 hugepage 内存
  printf("Allocating %zu GB on NUMA node %d (CXL)...\n", MEM_SIZE >> 30,
         CXL_NODE);
  char *buf = alloc_on_node(MEM_SIZE, CXL_NODE);
  if (!buf) {
    return 1;
  }

  // 2. 完整 first-touch，确保所有 hugepage 真正映射到 CXL_NODE
  printf("Warming up memory by memset %zu GB...\n", MEM_SIZE >> 30);
  {
    double t0 = now_sec();
    memset(buf, 0, MEM_SIZE);
    double t1 = now_sec();
    printf("Warmup done in %.2f s\n", t1 - t0);
  }

  // 2.5 验证内存确实分配在 CXL_NODE 上；如果不是，结果会失真。
  if (verify_pages_on_node(buf, MEM_SIZE, CXL_NODE) > 0) {
    fprintf(stderr,
            "ERROR: memory is NOT actually on node %d. The bandwidth numbers "
            "below will be measuring whatever node the pages landed on, not "
            "CXL. Aborting.\n",
            CXL_NODE);
    munmap(buf, MEM_SIZE);
    return 2;
  }

  // 2.6 防止 AutoNUMA / numa_balancing 在测试期间把 hugepage 迁回 LOCAL_NODE。
  // mlock 拦不住 NUMA 迁移，主要靠系统级 echo 0 >
  // /proc/sys/kernel/numa_balancing； 这里再做两个补丁：
  //   - mlock 一下，至少阻止 swap 路径
  //   - 主线程也 set_mempolicy(MPOL_BIND, node 4)，让 worker
  //   线程继承"分配仍要走
  //     node 4"的策略，万一有 fault-in 也不会落到本地
  if (mlock(buf, MEM_SIZE) != 0) {
    fprintf(stderr, "mlock failed: %s (continuing)\n", strerror(errno));
  }
  {
    unsigned long nodemask = 1UL << CXL_NODE;
    if (set_mempolicy(MPOL_BIND, &nodemask, MAXNODE_BITS) != 0) {
      fprintf(stderr, "set_mempolicy(MPOL_BIND) on main thread failed: %s\n",
              strerror(errno));
    }
  }

  // 3. 在 LOCAL_NODE 上挑出至多 max_threads 个**不同物理核**的逻辑 CPU
  int max_threads = THREAD_COUNTS[NUM_THREAD_COUNTS - 1];
  std::vector<int> cpus = pick_physical_cores_on_node(LOCAL_NODE, max_threads);
  if ((int)cpus.size() < max_threads) {
    fprintf(stderr,
            "Warning: only found %zu distinct physical cores on node %d "
            "(needed %d); some threads may share a physical core via SMT\n",
            cpus.size(), LOCAL_NODE, max_threads);
  }
  if (cpus.empty()) {
    fprintf(stderr,
            "No CPUs found on node %d; falling back to default range 32-47\n",
            LOCAL_NODE);
    for (int i = 32; i < 48; ++i)
      cpus.push_back(i);
  }
  printf("Using CPUs on node %d:", LOCAL_NODE);
  for (int c : cpus)
    printf(" %d", c);
  printf("\n");

  printf("Start tests...\n");

  std::vector<ResultEntry> results;
  results.reserve(NUM_THREAD_COUNTS * NUM_GRAINS * 2);

  // 跟踪是否检测到 NUMA 迁移；若有，直接停测以免后续数据无意义。
  bool migration_detected = false;

  for (int tc_idx = 0; tc_idx < NUM_THREAD_COUNTS; ++tc_idx) {
    int nthreads = THREAD_COUNTS[tc_idx];

    // 每换一个 thread 数前快速抽查一下是否还在 CXL_NODE 上
    if (verify_pages_on_node(buf, MEM_SIZE, CXL_NODE) > 0) {
      fprintf(stderr,
              "ABORT: pages migrated off node %d before threads=%d tests "
              "(numa_balancing?)\n",
              CXL_NODE, nthreads);
      migration_detected = true;
      break;
    }

    for (int g_idx = 0; g_idx < NUM_GRAINS; ++g_idx) {
      size_t grain = GRAIN_LIST[g_idx];

      for (int op = 0; op < 2; ++op) {
        bool is_write = (op == 1);

        std::vector<pthread_t> threads(nthreads);
        std::vector<ThreadArg> targs(nthreads);
        std::vector<double> bytes(nthreads, 0.0);

        std::atomic<bool> stop_flag(false);

        double start = now_sec();

        for (int i = 0; i < nthreads; ++i) {
          ThreadArg &ta = targs[i];
          ta.buf = buf;
          ta.mem_size = MEM_SIZE;
          ta.grain = grain;
          ta.bytes_processed = &bytes[i];
          ta.cpu_id = cpus[i % cpus.size()];
          ta.stop_flag = &stop_flag;
          ta.seed = 1234ULL + static_cast<uint64_t>(i) +
                    static_cast<uint64_t>(nthreads) * 100ULL +
                    static_cast<uint64_t>(grain) * 1000ULL +
                    static_cast<uint64_t>(op) * 10000ULL;

          int ret = pthread_create(&threads[i], nullptr,
                                   is_write ? worker_write : worker_read, &ta);
          if (ret != 0) {
            fprintf(stderr, "pthread_create failed: %d\n", ret);
            return 1;
          }
        }

        unsigned int sleep_sec = static_cast<unsigned int>(TEST_DURATION_SEC);
        sleep(sleep_sec);

        stop_flag.store(true, std::memory_order_relaxed);

        for (int i = 0; i < nthreads; ++i) {
          pthread_join(threads[i], nullptr);
        }

        double end = now_sec();
        double elapsed = end - start;

        double total_bytes = 0.0;
        for (int i = 0; i < nthreads; ++i) {
          total_bytes += bytes[i];
        }

        double gb = total_bytes / 1e9;
        double bw = gb / elapsed;

        ResultEntry re;
        re.threads = nthreads;
        re.grain = grain;
        re.is_write = is_write;
        re.seconds = elapsed;
        re.bandwidth_gbps = bw;
        results.push_back(re);

        printf("[threads=%2d, grain=%4zuB, %s] time=%.2fs, BW=%.2f GB/s\n",
               nthreads, grain, is_write ? "write" : "read", elapsed, bw);
        fflush(stdout);
      }
    }
  }

  // 测试结束后再 verify 一次：如果跑出了大于 MLC 链路峰的"反常高带宽"，
  // 这里十有八九能看到部分页已经被迁回 LOCAL_NODE。
  fprintf(stderr, "\nPost-test verify (16 samples):\n");
  int wrong_after = verify_pages_on_node(buf, MEM_SIZE, CXL_NODE);
  if (wrong_after > 0) {
    fprintf(stderr,
            "Pages migrated off node %d during the test run "
            "(probably AutoNUMA). Disable numa_balancing and rerun:\n"
            "    echo 0 | sudo tee /proc/sys/kernel/numa_balancing\n",
            CXL_NODE);
  }
  if (migration_detected) {
    fprintf(stderr,
            "Test was aborted mid-run due to detected migration; the SUMMARY "
            "below is partial.\n");
  }

  printf("\n================== SUMMARY ==================\n");
  printf("%8s %8s %8s %12s %14s\n", "threads", "grain", "op", "seconds",
         "BW(GB/s)");
  for (const auto &r : results) {
    printf("%8d %8zu %8s %12.2f %14.2f\n", r.threads, r.grain,
           r.is_write ? "write" : "read", r.seconds, r.bandwidth_gbps);
  }
  printf("============================================\n");
  printf("(sink=%lx; ignore)\n", (unsigned long)g_sink);

  // 释放：mmap 和 numa_alloc_onnode 释放方式不同；我们这里简单用 munmap，
  // 失败时再 numa_free
  if (munmap(buf, MEM_SIZE) != 0) {
    numa_free(buf, MEM_SIZE);
  }
  return 0;
}
