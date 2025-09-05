FROM mcr.microsoft.com/devcontainers/cpp:ubuntu-24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends autoconf automake clang-format clang-tidy git libtool ninja-build && \
    rm -rf /var/lib/apt/lists/*

RUN wget -q https://cmake.org/files/v4.1/cmake-4.1.0-linux-x86_64.sh && \
    bash cmake-4.1.0-linux-x86_64.sh --skip-license --prefix=/usr/local && \
    rm cmake-4.1.0-linux-x86_64.sh
