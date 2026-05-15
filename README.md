# DFS Prototype

This is a prototype of a distributed file system. It is not intended for production use.

[Docs](./docs/README.md)

## Build

Toolchain: C++ Compiler and CMake that support **C++ 17** (GCC>=8, CMake>=3.8)

```
## Ubuntu 22.04
sudo apt install autoconf pkg-config ninja-build binutils-dev libnuma-dev librocksdb-dev bzip2 libbz2-dev libsnappy-dev liburing-dev liblz4-dev libzstd-dev libcurl4-openssl-dev libpfm4-dev zlib1g-dev systemtap-sdt-dev python3-toml python-is-python3 flex msr-tools

git clone --recurse-submodules https://github.com/staxfs/StaxFS.git
cmake -B build -GNinja -DCMAKE_BUILD_TYPE=RelWithDebInfo  .
# cmake -DCMAKE_C_COMPILER=/usr/bin/gcc-11 -DCMAKE_CXX_COMPILER=/usr/bin/g++-11 -B build -GNinja -DCMAKE_BUILD_TYPE=Debug .
cmake --build build
```

## Running

If you want to run the metadata server on a block device file backend, make sure you make 4 PARTITIONS `<device_name>p1~p4` first (each with abundant size!).
Then, modify the config file for meta server, change `data_root_path` to the DEVICE NAME (e.g. `/dev/nvme0n1`). Program should do the rest for you.

### mdtest + ior
```
## Suppose you are in ~ and you have done building.
## Get ior and compile it
git clone https://github.com/hpc/ior.git
cd ior && ./bootstrap
./configure
make
```

### filebench
```
## Suppose you are in ~ and you have done building.
## Get filebench and compile it
git clone https://github.com/filebench/filebench.git
cd filebench
libtoolize
aclocal
autoheader
automake --add-missing
autoupdate
autoconf
./configure
make
# sudo make install # Used for using cvar
```


### THUCTC(NLP)
```
git clone https://github.com/hhyx/THUCTC.git

# download dataset THUCNews at http://thuctc.thunlp.org/ or https://aistudio.baidu.com/datasetdetail/8164/0
# Including 14 directories, 836k files, with an average size of 2.6KB

# compile
cd THUCTC
mkdir -p bin
find src -name "*.java" > sources.txt
javac -cp "lib/*" -d bin @sources.txt

```

## running test
```
## Suppose you are in ~ and you have done building, ior, filebench.

## Mount hugepage, eRPC requires this. It has been added to the running script and can be modified as needed.
# sudo scripts/mount-hugepage.sh

## In case you haven't installed openmpi:
# sudo apt install libopenmpi-dev

# local
## First parameter is the path to binary, second is SPDLOG_LEVEL
## To tweak other settings like # of threads, # of files, change scripts itself, because there could be too many of them.

# mdtest
sudo ./scripts/local/dev-run-mdtest.sh ../ior/src/mdtest off
# filebench
sudo ./scripts/local/dev-run-filebench.sh ../filebench/filebench workloads/filebench/fileserver.f off
# THUCTC
sudo ./scripts/local/dev-run-THUCTC.sh ../THUCTC path/to/THUCNews off
# cp
sudo ./scripts/local/dev-run-cp.sh path/to/cp off

# distribute
# mdtest
./scripts/distributed/dev-run-mdtest.sh ./conf/distributed_cxl_3_9_3_10 info '~/ior/src/mdtest' 16
# filebench
./scripts/distributed/dev-run-filebench.sh ./conf/distributed_cxl_3_9_3_10 info '~/filebench/filebench' workloads/filebench/fileserver.f 1
# cp
./scripts/distributed/dev-run-cp.sh ./conf/distributed_cxl_3_9_3_10 info ~/data/THUCNews 8
# THUCTC
./scripts/distributed/dev-run-THUCTC.sh ./conf/distributed_cxl_3_9_3_10 info ~/THUCTC ~/data/THUCNews 16
```

## For Developer

Code formatter:

```
pip install pre-commit
pre-commit run --all
```

Build `clangd` from source [link](https://github.com/llvm/llvm-project/tree/main/clang-tools-extra/clangd) or install it from package manager (`apt install clangd`).

Configuration for VSCode ([link](https://clangd.llvm.org/installation), [ref](https://zhuanlan.zhihu.com/p/566506467)).
(change the `clangd.path` if you need)
```
{
  "clangd.arguments": [
    "-log=error",
    "--clang-tidy",                 // 开启clang-tidy
    "--all-scopes-completion",      // 全代码库补全
    "--completion-style=detailed",  // 详细补全
    "--header-insertion=iwyu",      
    "--pch-storage=disk",           // 如果内存够大可以关闭这个选项
    "--j=5",                        // 后台线程数，可根据机器配置自行调整
    "--background-index",
  ],
  "clangd.path": "/home/wyf/nfs/software/llvm-project/build/bin/clangd"
}
```
