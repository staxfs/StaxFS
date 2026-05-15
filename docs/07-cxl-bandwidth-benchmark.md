# CXL Memory Bandwidth Benchmark

The results obtained from ./test/cxl/cxl_read_write_test.cc

## Test Configuration

- **Target**: CXL Extended Memory (NUMA Node 4)
- **Duration**: 10 seconds per test point
- **Grain sizes**: 64B, 128B, 256B, 512B, 1024B
- **Thread counts**: 1, 2, 4, 8, 16

## Results

### Read Bandwidth (GB/s)

| Threads \ Grain | 64B | 128B | 256B | 512B | 1024B |
|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 1.66 | 2.98 | 3.66 | 4.02 | 4.04 |
| **2** | 3.54 | 5.83 | 7.14 | 7.80 | 7.81 |
| **4** | 6.94 | 10.78 | 12.14 | 12.90 | 12.36 |
| **8** | 11.59 | 13.09 | 12.93 | 13.21 | 12.48 |
| **16** | 12.56 | 13.18 | 12.93 | 13.27 | 12.67 |

### Write Bandwidth (GB/s)

| Threads \ Grain | 64B | 128B | 256B | 512B | 1024B |
|:---:|:---:|:---:|:---:|:---:|:---:|
| **1** | 2.51 | 2.58 | 2.98 | 3.07 | 2.81 |
| **2** | 4.83 | 4.85 | 5.48 | 5.64 | 5.17 |
| **4** | 8.59 | 8.24 | 9.04 | 9.18 | 8.57 |
| **8** | 11.53 | 10.79 | 11.06 | 10.94 | 10.89 |
| **16** | 11.60 | 11.09 | 11.18 | 11.03 | 11.13 |

## Key Observations

| Metric | Value |
|:---|:---|
| **Peak Read BW** | ~13.3 GB/s (8-16 threads, 256-512B grain) |
| **Peak Write BW** | ~11.6 GB/s (16 threads, 64B grain) |
| **Read/Write Ratio** | Read ~1.2x faster than Write |
| **Saturation Point** | ~8 threads (read), ~8 threads (write) |
| **Single-thread Read** | 1.66 - 4.04 GB/s (grain dependent) |
| **Single-thread Write** | 2.51 - 3.07 GB/s (relatively flat) |

### Scaling Analysis

- **Read scales well with grain size** (1T: 64B→1024B = 2.4x), saturates at 256-512B
- **Write is relatively grain-insensitive** (1T: 64B→512B = 1.2x), bottlenecked by clwb/sfence
- **Thread scaling is near-linear up to 4 threads**, then diminishing returns
- **Bandwidth ceiling ~13 GB/s read / ~11.5 GB/s write** regardless of thread/grain combo

