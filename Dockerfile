FROM ubuntu:22.04
RUN apt update && apt install -y git curl libssl-dev g++ cmake ninja-build \
binutils-dev libnuma-dev libaio-dev libibverbs-dev \
liblz4-dev libzstd-dev libcurl4-openssl-dev libpfm4-dev zlib1g-dev