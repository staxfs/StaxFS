// INSERT_DELAY calibration tool.
// Measures the actual INSERT_DELAY macro cost for iteration counts 0..100.
// Output: CSV with columns: iters, cycles, ns, per_iter_ns
//
// Usage: taskset -c 32 ./latency          (pin to a Node 1 CPU)
//    or: sudo ./run.sh                    (disables turbo + c-states first)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <x86intrin.h>

// Must match the INSERT_DELAY in cxl_mem.h exactly.
#define INSERT_DELAY(times)                                                    \
  _mm_mfence();                                                                \
  for (int i = 0; i < (times); i++) {                                          \
    _rdtsc();                                                                  \
    __asm__ volatile("pause" ::: "memory");                                    \
  }                                                                            \
  _mm_mfence();

static uint64_t get_cpu_freq_khz() {
  FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
  if (!f)
    return 2400000; // fallback
  uint64_t khz = 0;
  if (fscanf(f, "%lu", &khz) != 1)
    khz = 2400000;
  fclose(f);
  return khz;
}

// Measure INSERT_DELAY(n) by calling the macro in a tight loop.
// Uses rdtscp (serializing) for accurate start/end timestamps.
static double measure_delay(int n, int repeats) {
  // warmup
  for (int i = 0; i < 1000; i++) {
    INSERT_DELAY(n);
  }

  uint64_t sum = 0;
  unsigned aux;
  for (int j = 0; j < repeats; j++) {
    uint64_t t0 = __rdtscp(&aux);
    INSERT_DELAY(n);
    uint64_t t1 = __rdtscp(&aux);
    sum += t1 - t0;
  }
  return static_cast<double>(sum) / repeats;
}

auto main() -> int {
  uint64_t freq_khz = get_cpu_freq_khz();
  double freq_ghz = static_cast<double>(freq_khz) / 1e6;

  int repeats = 100000;
  int max_iters = 30;

  printf("CPU freq: %.3f GHz\n", freq_ghz);
  printf("Repeats:  %d\n\n", repeats);

  // Measure mfence-only overhead (INSERT_DELAY(0) = 2x mfence + empty loop)
  double mfence_cycles = measure_delay(0, repeats);
  double mfence_ns = mfence_cycles / freq_ghz;
  printf("mfence overhead (INSERT_DELAY(0)): %.1f cycles = %.1f ns\n\n",
         mfence_cycles, mfence_ns);

  printf("%-6s %10s %10s %12s\n", "iters", "cycles", "total_ns", "per_iter_ns");
  printf("%-6s %10s %10s %12s\n", "-----", "------", "--------", "-----------");

  for (int n = 1; n <= max_iters; n++) {
    double cycles = measure_delay(n, repeats);
    double ns = cycles / freq_ghz;
    double loop_ns = (cycles - mfence_cycles) / freq_ghz;
    double per_iter = loop_ns / n;
    printf("%-6d %10.1f %10.1f %12.1f\n", n, cycles, ns, per_iter);
  }

  return 0;
}
