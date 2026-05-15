# debug

```
sudo gdb --pid=$(ps aux | grep meta-0 | grep -v grep | awk '{print $2}')

sudo gdb --pid=$(ps aux | grep data-0 | grep -v grep | awk '{print $2}')

sudo gdb --pid=$(ps aux | grep filebench | grep -v grep | awk '{print $2}' | tail -n 1)

for pid in $(pgrep -x mdtest); do     sudo gdb -batch -p $pid -ex "thread 1" -ex "bt 8" 2>/dev/null       | grep -q "PMPI_Barrier" || echo "STUCK: $pid";   done
```

# 查看CXL内存使用情况

```
sudo numastat -p pid
numactl --hardware

hugeadm --list-all-mounts
echo 0 | sudo tee /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
grep . /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages
grep . /sys/devices/system/node/node*/hugepages/hugepages-2048kB/free_hugepages
```

# ib驱动报错

```
ibstat
# 若没有输出，或不处于 Active 状态，可能是相关驱动没启动
sudo modprobe ib_uverbs
sudo modprobe rdma_ucm
sudo modprobe mlx5_ib
sudo modprobe ib_ipoib
sudo systemctl start opensm

# 确保处于 Active 状态，例如
$ ibstat
CA 'ibp231s0'
        CA type: MT4123
        Number of ports: 1
        Firmware version: 20.33.1048
        Hardware version: 0
        Node GUID: 0xb83fd20300b1df4c
        System image GUID: 0xb83fd20300b1df4c
        Port 1:
                State: Active
                Physical state: LinkUp
                Rate: 200
                Base lid: 4
                LMC: 0
                SM lid: 4
                Capability mask: 0xa651e84a
                Port GUID: 0xb83fd20300b1df4c
                Link layer: InfiniBand
```

# filebench运行报错

```
# for example: Unexpected Process termination Code 3, Errno 0 around line 11
echo 0 | sudo tee /proc/sys/kernel/randomize_va_space

# for example: Out of shared memory (1)!
# When the number of files exceeds 1024 * 1024, it is necessary to modify 
# filebench/ipc.h: #define FILEBENCH_NFILESETENTRIES (1024 * 1024).
# Afterwards, recompile Filebench

# Testing has found that compiling with gcc-13 will result in errors, 
# while using gcc-11 will work normally
```