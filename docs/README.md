# Docs

1. [项目总览](./00-overview.md)

2. 设计文档

| 文档 | 内容 |
|------|------|
| [01-common.md](01-common.md) | 公共头文件：核心类型（Inode/Dirent/DentView/RemoteInodeChange）、RPC 枚举、配置/日志、profile 工具 |
| [02-cxl.md](02-cxl.md) | CXL 内存子系统：CXLMem、GIMMem、CXLSSD、CXLDevice、DFSHeader |
| [03-server.md](03-server.md) | 服务端：Metadata 类、哈希表三选一、CXLPersistence、CompactWAL、跨 MDS 同步 |
| [04-client.md](04-client.md) | 客户端：POSIX 拦截、HLC 跟踪、BatchGetDentViews 多 MDS 并发 |
| [05-proto-build-test.md](05-proto-build-test.md) | Proto / 构建 / 线程亲和 / 测试 / 脚本 |
| [06-config.md](06-config.md) | 配置文件格式、目录命名、部署方式 |
| [07-cxl-bandwidth-benchmark.md](07-cxl-bandwidth-benchmark.md) | CXL 带宽实测数据 |

3. Development Tips
    1. [Performance Analysis](./perf.md)
    2. [How to use GDB to debug this project](./debug.md)
    3. ...
