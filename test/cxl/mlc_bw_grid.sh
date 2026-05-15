#!/bin/bash
# mlc_bw_grid.sh
#
# 用 Intel MLC 测 node LOCAL_NODE 的 CPU -> node CXL_NODE 的 CXL 内存带宽，
# **全部走随机访问模式 (-U)**，扫两个维度：
#   threads in {1, 2, 4, 8, 16}   (load threads，不含 latency thread)
#   grain   in {64, 128, 256, 512, 1024} B   (-l 在 -U 模式下基本不影响访问粒度，
#                                              MLC 单次访问始终是 1 条 64B cache line；
#                                              这里保留只是为了和 C++ 测试网格对齐)
# 每个组合分别测全读 (默认) 和全写 (-W6, NT store)。
#
# MLC v3.11 关键 flag 说明（从 mlc --help 解析得到）：
#   --loaded_latency       # 唯一同时支持 -W -l -d -k -c -j -U 的模式
#   -U                     # **随机访问 buffer**（默认是顺序，prefetcher 友好）
#                          # 加上之后才符合 metadata / small-object 真实场景
#   -d0                    # 0 cycle 注入延迟 = 最大压力
#   -W0                    # ALL Reads
#   -W6                    # ALL Writes (NT store)
#   -k<csv>                # load 线程的 core 列表
#   -c<core>               # latency 线程的 core（默认 core 0，需要显式挪到 node 1 上）
#   -j<node>               # 从指定 NUMA node 分配内存（替代 numactl --membind）
#   -l<n>                  # stride length in bytes（默认 64；-U 下作用很弱）
#   -L                     # 用 2MB 大页
#   -e                     # 不修改 HW prefetcher 状态
#   -tn                    # 测试时长（秒）
#
# 注意：v3.11 的 --peak_injection_bandwidth **不支持 -W**，所以无法限定到全
# 读/全写，只能拿默认的 5 种 R:W 比例输出；这里整个脚本统一用 --loaded_latency
# -d0 模式来取带宽数。
#
# 用法：
#   sudo MLC=~/mlc/Linux/mlc bash mlc_bw_grid.sh
#   sudo MLC=~/mlc/Linux/mlc DURATION=20 bash mlc_bw_grid.sh
#
# 跑前建议：
#   echo 0 | sudo tee /proc/sys/kernel/numa_balancing
#   sudo sh -c 'echo 5200 > /sys/devices/system/node/node4/hugepages/hugepages-2048kB/nr_hugepages'

set -uo pipefail

MLC="${MLC:-${HOME}/mlc/Linux/mlc}"
LOCAL_NODE="${LOCAL_NODE:-1}"
CXL_NODE="${CXL_NODE:-4}"
DURATION="${DURATION:-5}"
LOG="${LOG:-mlc_bw_grid.log}"
DUMP_DIR="${DUMP_DIR:-mlc_bw_grid.out}"
# 每线程 buffer 大小（KiB）。MLC 默认只有 100 MiB/thread → random 模式下大量
# 命中 L3，且 buffer 跨的 hugepage 太少，CXL 多设备 interleave 用不上。
# 调到 1 GiB/线程，和 C++ 测试 10 GiB 的工作集量级靠拢。
# 注意：-L 用 2MB 大页，1 GiB = 512 hugepages，16 线程 = 8192 个 hugepage 在 node 4 上。
# 跑前确保 node 4 hugepages 足够。
BUF_KIB="${BUF_KIB:-1048576}"

THREADS=(1 2 4 8 16)
GRAINS=(64)
# GRAINS=(64 128 256 512 1024)

# 这个 v3.11b 评估版二进制实测**完全不支持 -W**（连 --loaded_latency 都报
# "Unsupported -W option!"），尽管 help 里写着 [-Wn]。所以：
#   - 读：不传 -W（--loaded_latency 默认就是全读）
#   - 写：smoke 阶段探测哪个 -W 码能用；探不到则跳过写测试
# 这两个值会在 main() 的 smoke 阶段动态确认/覆盖。
declare -A OP_FLAG=(
  ["read"]=""
  ["write"]="-W6"
)
# 写探测顺序：-W6 (NT-write) 最常见 → -W11 (regular write) → -W2 (3:1 mix) → 放弃
WRITE_PROBES=("-W6" "-W11" "-W2")
OP_ORDER=("read" "write")
# 写测试是否可用（smoke 阶段会写）
WRITE_AVAILABLE=1

# ------------------------------------------------------------------
# 前置检查
# ------------------------------------------------------------------
if [[ ! -x "$MLC" ]]; then
  echo "ERROR: MLC binary not found or not executable: $MLC" >&2
  exit 1
fi
if ! command -v numactl >/dev/null 2>&1; then
  echo "ERROR: numactl not found (only used for topology query)" >&2
  exit 1
fi
if [[ $EUID -ne 0 ]]; then
  echo "ERROR: must run as root (MLC needs hugepages / msr access)" >&2
  exit 1
fi

# ------------------------------------------------------------------
# pick_cpus_with_extra <node> <load_count>
#   挑 (load_count + 1) 个**不同物理核**的逻辑 CPU，去重 SMT 兄弟。
#   stdout 输出："<load_csv> <lat_core>"
#   load_csv 是 load_count 个 core 的 csv，lat_core 是多余的那一个。
# ------------------------------------------------------------------
pick_cpus_with_extra() {
  local node="$1"
  local need=$(( $2 + 1 ))
  local cpus
  cpus=$(numactl -H | awk -v n="$node" \
    '$1=="node" && $2==n && $3=="cpus:" { for(i=4;i<=NF;i++) print $i }')

  declare -A seen
  local result=()
  for cpu in $cpus; do
    local sib
    sib=$(cat "/sys/devices/system/cpu/cpu${cpu}/topology/thread_siblings_list" \
      2>/dev/null) || continue
    if [[ -z "${seen[$sib]:-}" ]]; then
      seen[$sib]=1
      result+=("$cpu")
      (( ${#result[@]} >= need )) && break
    fi
  done

  if (( ${#result[@]} < need )); then
    echo "ERROR: only ${#result[@]} physical cores on node $node, need $need" >&2
    return 1
  fi

  local lat="${result[0]}"
  local load=("${result[@]:1}")
  local IFS=','
  echo "${load[*]} $lat"
}

# ------------------------------------------------------------------
# extract_bw <out_file>
#   从 mlc --loaded_latency 的输出里提取 Bandwidth (MB/s)
#     Inject  Latency Bandwidth
#     Delay   (ns)    MB/sec
#     ==========================
#      00000  20502.11        23459.2
# ------------------------------------------------------------------
extract_bw() {
  awk '/^[[:space:]]*0+[[:space:]]+[0-9.]+[[:space:]]+[0-9.]+/ \
    { print $NF; exit }' "$1"
}

# ------------------------------------------------------------------
# run_mlc <load_csv> <lat_core> <op_flag> <stride> <tag>
# ------------------------------------------------------------------
run_mlc() {
  local load_csv="$1"
  local lat_core="$2"
  local op_flag="$3"
  local stride="$4"
  local tag="$5"
  local out_file="$DUMP_DIR/${tag}.txt"

  # op_flag 可能是空字符串（read 默认无 -W），需要避免传一个空 arg
  # -U: 随机访问模式（默认顺序）
  # -b<KiB>: 每线程 buffer 大小，覆盖 MLC 默认的 100 MiB（太小，会命中 L3）
  local cmd_args=( --loaded_latency -U -d0 -b"$BUF_KIB" )
  if [[ -n "$op_flag" ]]; then
    cmd_args+=( "$op_flag" )
  fi
  cmd_args+=( -k"$load_csv" -c"$lat_core" -j"$CXL_NODE" -L -e
              -l"$stride" -t"$DURATION" )

  {
    echo "+ $MLC ${cmd_args[*]}"
    echo "----"
  } > "$out_file"

  "$MLC" "${cmd_args[@]}" >> "$out_file" 2>&1 || true

  extract_bw "$out_file"
}

mbps_to_gbps() {
  local x="$1"
  if [[ -z "$x" ]]; then echo "ERR"; return; fi
  awk -v b="$x" 'BEGIN { printf "%.2f", b/1000 }'
}

# ------------------------------------------------------------------
# Smoke test (read)：用最小配置 + 默认 -W 跑一次，确认参数 + 解析都正常
# ------------------------------------------------------------------
smoke_test_read() {
  local pair load lat
  pair=$(pick_cpus_with_extra "$LOCAL_NODE" 1) || return 1
  load="${pair% *}"
  lat="${pair##* }"

  local out="$DUMP_DIR/smoke_read.txt"
  {
    echo "+ $MLC --loaded_latency -U -d0 -b$BUF_KIB -k$load -c$lat -j$CXL_NODE -L -e -l64 -t3"
    echo "----"
  } > "$out"
  "$MLC" --loaded_latency -U -d0 -b"$BUF_KIB" -k"$load" -c"$lat" -j"$CXL_NODE" \
         -L -e -l64 -t3 >> "$out" 2>&1 || true

  local bw_mbps
  bw_mbps=$(extract_bw "$out")
  if [[ -z "$bw_mbps" ]]; then
    echo "Smoke (read) FAILED: cannot extract BW from $out" | tee -a "$LOG"
    echo "----- first 60 lines of $out: -----" | tee -a "$LOG"
    head -60 "$out" | tee -a "$LOG"
    echo "------------------------------------" | tee -a "$LOG"
    return 1
  fi
  echo "Smoke (read) OK: 1T 64B read = $(mbps_to_gbps "$bw_mbps") GB/s" \
    | tee -a "$LOG"
  return 0
}

# ------------------------------------------------------------------
# Probe write -W code：依次试 WRITE_PROBES 里的每个 -W 码，找第一个不报
# "Unsupported -W option" 又能解析到 BW 的。改写 OP_FLAG[write] / WRITE_AVAILABLE。
# ------------------------------------------------------------------
probe_write_W() {
  local pair load lat
  pair=$(pick_cpus_with_extra "$LOCAL_NODE" 1) || return 1
  load="${pair% *}"
  lat="${pair##* }"

  for W in "${WRITE_PROBES[@]}"; do
    local out="$DUMP_DIR/smoke_write_${W#-}.txt"
    {
      echo "+ $MLC --loaded_latency -U -d0 -b$BUF_KIB $W -k$load -c$lat -j$CXL_NODE -L -e -l64 -t3"
      echo "----"
    } > "$out"
    "$MLC" --loaded_latency -U -d0 -b"$BUF_KIB" "$W" -k"$load" -c"$lat" -j"$CXL_NODE" \
           -L -e -l64 -t3 >> "$out" 2>&1 || true

    if grep -q "Unsupported -W option" "$out"; then
      echo "Probe write $W: not supported by this MLC binary" | tee -a "$LOG"
      continue
    fi
    local bw_mbps
    bw_mbps=$(extract_bw "$out")
    if [[ -n "$bw_mbps" ]]; then
      OP_FLAG["write"]="$W"
      WRITE_AVAILABLE=1
      echo "Probe write OK: using $W (1T 64B = $(mbps_to_gbps "$bw_mbps") GB/s)" \
        | tee -a "$LOG"
      return 0
    fi
    echo "Probe write $W: ran but couldn't extract BW; see $out" | tee -a "$LOG"
  done

  WRITE_AVAILABLE=0
  echo "WARNING: no -W code worked for writes; grid will only have read column" \
    | tee -a "$LOG"
  echo "         If you know the right -W code for this MLC build, edit" \
    | tee -a "$LOG"
  echo "         WRITE_PROBES in mlc_bw_grid.sh and rerun." | tee -a "$LOG"
  return 1
}

# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
main() {
  : > "$LOG"
  mkdir -p "$DUMP_DIR"
  rm -f "$DUMP_DIR"/*.txt 2>/dev/null

  {
    echo "============================================================"
    echo " MLC bandwidth grid (random access, -U)"
    echo "   MLC binary:  $MLC"
    echo "   local node:  $LOCAL_NODE"
    echo "   CXL node:    $CXL_NODE"
    echo "   duration:    $DURATION sec/test"
    echo "   threads:     ${THREADS[*]} (load) + 1 (latency)"
    echo "   grains:      ${GRAINS[*]}  (-l, weak effect under -U)"
    echo "   buf/thread:  $BUF_KIB KiB ($((BUF_KIB/1024)) MiB)"
    echo "   ops:         ${OP_ORDER[*]}  (default = read, -W6 = write-NT)"
    echo "   dump dir:    $DUMP_DIR"
    echo "============================================================"
  } | tee -a "$LOG"

  echo "" | tee -a "$LOG"
  echo "Smoke (read)..." | tee -a "$LOG"
  if ! smoke_test_read; then
    echo "Aborting: read smoke test failed; see $DUMP_DIR/smoke_read.txt" \
      | tee -a "$LOG"
    exit 2
  fi
  echo "" | tee -a "$LOG"
  echo "Probing write -W code..." | tee -a "$LOG"
  probe_write_W || true

  {
    echo ""
    echo "=== Grid: --loaded_latency -U -d0 -W<n> -l<grain> (random) ==="
    printf "%-10s %-10s %-10s %s\n" "threads" "grain" "op" "BW(GB/s)"
  } | tee -a "$LOG"

  declare -A grid_bw
  for T in "${THREADS[@]}"; do
    local pair load lat
    if ! pair=$(pick_cpus_with_extra "$LOCAL_NODE" "$T"); then
      echo "Skipping T=$T: insufficient cores" | tee -a "$LOG"
      continue
    fi
    load="${pair% *}"
    lat="${pair##* }"

    for G in "${GRAINS[@]}"; do
      for op_label in "${OP_ORDER[@]}"; do
        if [[ "$op_label" == "write" && "$WRITE_AVAILABLE" -eq 0 ]]; then
          grid_bw["$T,$G,$op_label"]="N/A"
          printf "%-10d %-10d %-10s %s\n" "$T" "$G" "$op_label" "N/A (no -W)" \
            | tee -a "$LOG"
          continue
        fi
        op_flag="${OP_FLAG[$op_label]}"
        tag="t${T}_g${G}_${op_label}"
        bw_mbps=$(run_mlc "$load" "$lat" "$op_flag" "$G" "$tag")
        if [[ -z "$bw_mbps" ]]; then
          bw="ERR(see $DUMP_DIR/${tag}.txt)"
        else
          bw=$(mbps_to_gbps "$bw_mbps")
        fi
        grid_bw["$T,$G,$op_label"]="$bw"
        printf "%-10d %-10d %-10s %s\n" "$T" "$G" "$op_label" "$bw" \
          | tee -a "$LOG"
      done
    done
  done

  # ----------------------------------------------------------------
  # 汇总表：BW (GB/s) 和 IOPS (Mops/s)
  # IOPS_Mops = BW_GBps * 1000 / grain_B   （Mops 即 millions of ops per sec）
  # ----------------------------------------------------------------
  # short_cell <value>: 把 grid_bw 里的值格式化成定宽显示用
  short_cell() {
    local v="$1"
    if [[ "$v" =~ ^[0-9.]+$ ]]; then
      echo "$v"
    elif [[ "$v" == "N/A" ]]; then
      echo "N/A"
    else
      echo "ERR"   # 把 "ERR(see ...)" 缩到 "ERR"，保持表格对齐
    fi
  }

  # iops_cell <bw_str> <grain>: BW 数值时返回 IOPS (Mops/s, 1 位小数)
  iops_cell() {
    local bw="$1"
    local g="$2"
    if [[ "$bw" =~ ^[0-9.]+$ ]]; then
      awk -v b="$bw" -v g="$g" 'BEGIN { printf "%.1f", b*1000/g }'
    elif [[ "$bw" == "N/A" ]]; then
      echo "N/A"
    else
      echo "ERR"
    fi
  }

  {
    echo ""
    echo "=== Summary: bandwidth (GB/s) ==="
    for op_label in "${OP_ORDER[@]}"; do
      echo ""
      echo "op = $op_label"
      printf "%-12s" "thr\\grain"
      for G in "${GRAINS[@]}"; do
        printf "%10s" "${G}B"
      done
      echo ""
      for T in "${THREADS[@]}"; do
        printf "%-12d" "$T"
        for G in "${GRAINS[@]}"; do
          v="${grid_bw["$T,$G,$op_label"]:-ERR}"
          printf "%10s" "$(short_cell "$v")"
        done
        echo ""
      done
    done

    echo ""
    echo "=== Summary: IOPS (M ops/s) ==="
    echo "(IOPS_Mops = BW_GBps * 1000 / grain_B)"
    for op_label in "${OP_ORDER[@]}"; do
      echo ""
      echo "op = $op_label"
      printf "%-12s" "thr\\grain"
      for G in "${GRAINS[@]}"; do
        printf "%10s" "${G}B"
      done
      echo ""
      for T in "${THREADS[@]}"; do
        printf "%-12d" "$T"
        for G in "${GRAINS[@]}"; do
          v="${grid_bw["$T,$G,$op_label"]:-ERR}"
          printf "%10s" "$(iops_cell "$v" "$G")"
        done
        echo ""
      done
    done
  } | tee -a "$LOG"

  echo "" | tee -a "$LOG"
  echo "Done. Log: $LOG, raw dumps: $DUMP_DIR/" | tee -a "$LOG"
}

main "$@"
