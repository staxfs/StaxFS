// cxl_lock_test.cc
// Compile: g++ -O3 -march=native -std=gnu++17 cxl_lock_test.cc -o cxl_lock
// -lnuma -lpthread
// Run: sudo ./cxl_lock

#include <bits/stdc++.h>
#include <fcntl.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

static constexpr int MAX_PROCS = 32;
static constexpr int TEST_SECONDS = 10;
static constexpr int CXL_NODE = 4;
static constexpr const char *SHM_NAME = "/cxl_lock_shm_v1";

#define MAX_PROCS 32
double result_table[5][MAX_PROCS]; // result_table[nodes_used][procs]

// Shared layout in the shm region:
// offset 0: uint8_t lock_byte;
// offset 8: uint64_t ready_count;
// offset 16: uint64_t start_flag; (0/1)
// offset 24: uint64_t stop_flag;  (0/1)
// offset 32: uint64_t child_counts[MAX_PROCS];
// total size: let's allocate one page.

static size_t round_up_page(size_t x) {
  long p = sysconf(_SC_PAGESIZE);
  return ((x + p - 1) / p) * p;
}

struct ShmLayout {
  volatile uint8_t lock; // 1 byte
  uint8_t padding0[7];
  volatile uint64_t ready_count; // processes increment when ready
  volatile uint64_t start_flag;  // parent sets to 1 to start
  volatile uint64_t stop_flag;   // parent sets to 1 to stop
  uint64_t counts[MAX_PROCS];    // per-process counters
};

// Simple spinlock on a uint8_t in shared memory (process-shared)
// Uses GCC atomic builtins on shared memory.
static inline void spin_lock(volatile uint8_t *lockptr) {
  // try to set from 0->1 atomically
  uint8_t expected = 0;
  while (!__atomic_compare_exchange_n(lockptr, &expected, (uint8_t)1, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    // busy-wait fallback: wait until lock becomes 0 to reduce CAS
    while (__atomic_load_n(lockptr, __ATOMIC_RELAXED)) {
      // give scheduler a hint
      sched_yield();
    }
    expected = 0;
  }
}

static inline void spin_unlock(volatile uint8_t *lockptr) {
  __atomic_store_n(lockptr, (uint8_t)0, __ATOMIC_RELEASE);
}

// create and mmap shm, return pointer to mapped memory and fd
void *create_and_map_shm(const char *name, size_t size, int &out_fd) {
  int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
  if (fd == -1) {
    perror("shm_open");
    exit(1);
  }
  if (ftruncate(fd, size) == -1) {
    perror("ftruncate");
    exit(1);
  }
  void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  out_fd = fd;
  return addr;
}

// bind region [addr, addr+len) to a single node using mbind + nodemask
void bind_region_to_node(void *addr, size_t len, int node) {
  // nodemask as unsigned long array; node must be < (sizeof(unsigned long)*8)
  unsigned long nodemask = 0UL;
  if (node >= (int)(sizeof(unsigned long) * 8)) {
    // fallback: use libnuma bitmask approach (rare for node indices)
    cerr << "node index too large for simple mask, abort\n";
    exit(1);
  }
  nodemask = (1UL << node);
  int ret =
      mbind(addr, len, MPOL_BIND, &nodemask, sizeof(unsigned long) * 8, 0);
  if (ret != 0) {
    perror("mbind");
    // Not fatal necessarily; continue with a warning
    cerr << "Warning: mbind failed (errno=" << errno
         << "), test may not place pages on requested node\n";
  }
}

// helper: gather cpus of a node into vector<int>
vector<int> cpus_of_node(int node) {
  vector<int> cpus;
  struct bitmask *bm = numa_allocate_cpumask();
  if (numa_node_to_cpus(node, bm) != 0) {
    cerr << "numa_node_to_cpus failed for node " << node << endl;
    numa_free_cpumask(bm);
    return cpus;
  }
  for (int c = 0; c < bm->size; ++c) {
    if (numa_bitmask_isbitset(bm, c))
      cpus.push_back(c);
  }
  numa_free_cpumask(bm);
  return cpus;
}

// bind current process to cpu
void bind_this_process_to_cpu(int cpu) {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  CPU_SET(cpu, &mask);
  if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
    perror("sched_setaffinity");
    // not fatal, but warn
  }
}

// run worker in child process
// idx: index of process (0..p-1)
// proc_cpu: cpu core number to bind to
void child_worker(int idx, int proc_cpu, ShmLayout *layout) {
  // bind process to cpu
  bind_this_process_to_cpu(proc_cpu);

  // mark ready
  __atomic_add_fetch(&layout->ready_count, 1, __ATOMIC_SEQ_CST);

  // wait for start
  while (!__atomic_load_n(&layout->start_flag, __ATOMIC_SEQ_CST)) {
    sched_yield();
  }

  // clear local counter
  layout->counts[idx] = 0;

  // run until stop_flag set
  while (!__atomic_load_n(&layout->stop_flag, __ATOMIC_SEQ_CST)) {
    // lock
    spin_lock(&layout->lock);
    // critical: increment own slot
    layout->counts[idx]++;
    // unlock
    spin_unlock(&layout->lock);
  }

  // exit
  _exit(0);
}

// run a single test with `proc_count` processes distributed evenly across
// `num_nodes_to_use` among nodes 0..3 nodes_to_use: vector of node ids (e.g.,
// {0} or {0,1} ...) cores_per_node: precomputed cpu lists for nodes
void run_one_configuration(const vector<int> &nodes_to_use,
                           const vector<vector<int>> &cores_per_node,
                           int proc_count) {
  cout << "=== Test procs=" << proc_count
       << " nodes_used=" << nodes_to_use.size() << " ===\n";

  // Create shared shm and bind to CXL node
  size_t shm_size = round_up_page(sizeof(ShmLayout));
  int shm_fd;
  void *shmaddr = create_and_map_shm(SHM_NAME, shm_size, shm_fd);
  // bind pages to CXL node
  bind_region_to_node(shmaddr, shm_size, CXL_NODE);

  // zero initialize
  memset(shmaddr, 0, shm_size);
  ShmLayout *layout = reinterpret_cast<ShmLayout *>(shmaddr);

  // parent will fork children; assign nodes -> cpus round-robin
  vector<int> assigned_cpu(proc_count, -1);
  // track next cpu index per node
  unordered_map<int, int> next_cpu_idx;
  for (int n : nodes_to_use)
    next_cpu_idx[n] = 0;

  for (int i = 0; i < proc_count; ++i) {
    int node = nodes_to_use[i % nodes_to_use.size()];
    const vector<int> &cpus = cores_per_node[node];
    if (cpus.empty()) {
      cerr << "No cpus for node " << node << ", abort\n";
      exit(1);
    }
    int pos = next_cpu_idx[node] % cpus.size();
    assigned_cpu[i] = cpus[pos];
    next_cpu_idx[node] += 1;
  }

  // fork children
  vector<pid_t> children;
  children.reserve(proc_count);

  for (int i = 0; i < proc_count; ++i) {
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
      exit(1);
    } else if (pid == 0) {
      // child
      child_worker(i, assigned_cpu[i], layout);
      // never returns
    } else {
      children.push_back(pid);
    }
  }

  // wait until all children are ready
  const int timeout_ms = 5000;
  int waited = 0;
  while ((int)__atomic_load_n(&layout->ready_count, __ATOMIC_SEQ_CST) <
         proc_count) {
    usleep(1000);
    if ((waited += 1) > timeout_ms) {
      cerr << "Timeout waiting for children ready (have " << layout->ready_count
           << " of " << proc_count << ")\n";
      break;
    }
  }

  // set start_flag
  __atomic_store_n(&layout->start_flag, 1, __ATOMIC_SEQ_CST);

  // sleep test duration
  this_thread::sleep_for(chrono::seconds(TEST_SECONDS));

  // stop children
  __atomic_store_n(&layout->stop_flag, 1, __ATOMIC_SEQ_CST);

  // wait for children exit
  for (pid_t c : children) {
    int status = 0;
    waitpid(c, &status, 0);
  }

  // collect results
  uint64_t total_ops = 0;
  for (int i = 0; i < proc_count; ++i) {
    uint64_t v = __atomic_load_n(&layout->counts[i], __ATOMIC_SEQ_CST);
    total_ops += v;
    cout << "  proc " << i << " cpu " << assigned_cpu[i] << " ops=" << v
         << "\n";
  }

  double ops_per_sec = (double)total_ops / (double)TEST_SECONDS;
  cout << "  => total_ops=" << total_ops << " ops/sec=" << ops_per_sec << "\n";

  result_table[nodes_to_use.size()][proc_count] = ops_per_sec;

  // cleanup shm
  munmap(shmaddr, shm_size);
  close(shm_fd);
  shm_unlink(SHM_NAME);

  // small pause between tests
  this_thread::sleep_for(chrono::milliseconds(200));
}

int main() {
  if (numa_available() == -1) {
    cerr << "NUMA not available on this system. Exiting.\n";
    return 1;
  }

  setvbuf(stdout, NULL, _IOLBF, 0); // line buffered

  // prepare cores per node for nodes 0..3
  vector<vector<int>> cores_per_node;
  cores_per_node.resize(5); // nodes 0..4 (4 is CXL possibly with no CPUs)
  for (int n = 0; n <= 4; ++n) {
    cores_per_node[n] = cpus_of_node(n);
    // debug
    cerr << "node " << n << " cpus: ";
    for (int c : cores_per_node[n])
      cerr << c << " ";
    cerr << "\n";
  }

  // nodes available for process distribution: 0..3 (sockets)
  vector<int> candidate_nodes = {0, 1, 2, 3};

  // // For k = 1..4 nodes used, test p = 1..30 processes
  // for (int nodes_used = 1; nodes_used <= 4; ++nodes_used) {
  //   vector<int> nodes_to_use(candidate_nodes.begin(),
  //                            candidate_nodes.begin() + nodes_used);
  //   cout << "\n*** Running tests with nodes_used = " << nodes_used
  //        << " (nodes:";
  //   for (int x : nodes_to_use)
  //     cout << x << " ";
  //   cout << ") ***\n";

  //   // for p = 1 .. 30
  //   for (int p = 1; p <= 30; ++p) {
  //     run_one_configuration(nodes_to_use, cores_per_node, p);
  //   }
  // }

  vector<vector<int>> test_node_groups = {
      {0},       // Case 1: only Node 0
      {0, 2},    // Case 2: Node 0 + Node 2
      {0, 2, 3}, // Case 3: Node 0 + Node 2 + Node 3
                 // {0, 1, 2, 3}
  };

  // Run tests
  for (auto &nodes_to_use : test_node_groups) {
    cout << "\n*** Running tests using nodes: ";
    for (int x : nodes_to_use)
      cout << x << " ";
    cout << "***\n";

    // p = 1..30 processes
    for (int p = 1; p <= 30; ++p) {
      run_one_configuration(nodes_to_use, cores_per_node, p);
    }
  }

  cout << "All tests done.\n";

  printf("\n========= Spinlock OPS (CXL Memory) =========\n");
  printf("procs | 1-socket | 2-socket | 3-socket \n");
  for (int p = 1; p <= MAX_PROCS; p++) {
    printf("%5d |", p);
    for (int n = 1; n <= 3; n++) {
      if (result_table[n][p] > 0)
        printf(" %.0f |", result_table[n][p]); // 👈 输出完整数字
      else
        printf(" 0 |");
    }
    printf("\n");
  }
  printf("==============================================\n");

  return 0;
}
