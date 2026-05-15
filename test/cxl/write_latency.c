// Measure remote-NUMA write latency from `src` node to `dst` node.
//
// Build: gcc -O2 -march=native -mavx512f -o write_latency write_latency.c
// -lnuma Run:   sudo ./write_latency           # defaults: src=1, dst=4
//        sudo ./write_latency 1 4       # explicit
//
// Method:
//   1. Pin this thread to a CPU on `src`.
//   2. Allocate MEMORY_SIZE on `dst` and touch every page so the kernel
//      actually backs it with physical memory on `dst`.
//   3. clflushopt every cache line so the measurement starts cold.
//   4. Inner loop: random cache-line-aligned offset → one AVX-512
//      non-temporal store (writes exactly 64 B = one cache line in a
//      single instruction, bypasses cache) → sfence (drains write-
//      combining buffer to memory). Per-iteration latency = one 64 B
//      cache-line write to remote-NUMA memory.
//
// The original prefetch + regular store + mfence loop measured store-
// buffer-to-L1 latency, not memory-write latency.

#define _GNU_SOURCE
#include <numa.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <x86intrin.h>

#define ITERATIONS 10000000UL                  // 10 M measured ops
#define WARMUP 1000000UL                       //  1 M warmup ops
#define STRIDE 64                              // cache-line size
#define MEMORY_SIZE (4UL * 1024 * 1024 * 1024) // 4 GiB (power of 2)
#define DEFAULT_SRC 1
#define DEFAULT_DST 4

// xorshift64* — fast PRNG so the address stream is not prefetcher-friendly
static inline uint64_t prng_next(uint64_t *s) {
  uint64_t x = *s;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *s = x;
  return x * 0x2545F4914F6CDD1DULL;
}

int main(int argc, char **argv) {
  int src = (argc > 1) ? atoi(argv[1]) : DEFAULT_SRC;
  int dst = (argc > 2) ? atoi(argv[2]) : DEFAULT_DST;

  if (numa_available() == -1) {
    fprintf(stderr, "NUMA support unavailable\n");
    return 1;
  }

  // 1. Pin this thread to a CPU on the source node.
  if (numa_run_on_node(src) != 0) {
    perror("numa_run_on_node");
    return 1;
  }
  printf("Source node = %d, destination node = %d\n", src, dst);

  // 2. Allocate memory on the destination node.
  char *memory = (char *)numa_alloc_onnode(MEMORY_SIZE, dst);
  if (!memory) {
    fprintf(stderr, "numa_alloc_onnode(%lu, %d) failed\n",
            (unsigned long)MEMORY_SIZE, dst);
    return 1;
  }

  // 3. Touch every page so the kernel actually allocates physical
  //    pages on `dst` (numa_alloc_onnode by itself only reserves).
  for (size_t i = 0; i < MEMORY_SIZE; i += 4096)
    memory[i] = 0;

  // 4. Flush every cache line from this CPU's caches so each later
  //    NT store starts cold and reaches the remote node.
  for (size_t i = 0; i < MEMORY_SIZE; i += STRIDE)
    _mm_clflushopt(memory + i);
  _mm_sfence();

  const size_t mask = MEMORY_SIZE - 1; // requires power of 2
  uint64_t state = 0xdeadbeefULL;
  __m512i val = _mm512_set1_epi64(1); // 64 B payload

  // 5. Warmup so TLB / page-table-walk costs aren't billed to the
  //    first measured iterations.
  for (size_t i = 0; i < WARMUP; i++) {
    size_t off = prng_next(&state) & mask & ~(uint64_t)(STRIDE - 1);
    _mm512_stream_si512((__m512i *)(memory + off), val); // 64 B NT store
  }
  _mm_sfence();

  // 6. Measurement: random 64 B-aligned offset, one 64 B NT store per
  //    iteration, sfence to drain the WC buffer to dst memory.
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  for (size_t i = 0; i < ITERATIONS; i++) {
    size_t off = prng_next(&state) & mask & ~(uint64_t)(STRIDE - 1);
    _mm512_stream_si512((__m512i *)(memory + off), val); // 64 B NT store
    _mm_sfence();                                        // drain WC buffer
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);

  long long ns = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL +
                 (t1.tv_nsec - t0.tv_nsec);
  double avg_ns = (double)ns / (double)ITERATIONS;

  printf("Write latency from node %d to node %d: %.2f ns (%.2f Mops/s)\n", src,
         dst, avg_ns, 1000.0 / avg_ns);

  numa_free(memory, MEMORY_SIZE);
  return 0;
}
