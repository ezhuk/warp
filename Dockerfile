FROM ubuntu:latest AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends autoconf automake build-essential ca-certificates curl git libtool ninja-build pkg-config unzip wget zip && \
    rm -rf /var/lib/apt/lists/*

RUN wget -q https://cmake.org/files/v4.1/cmake-4.1.0-linux-x86_64.sh && \
    bash cmake-4.1.0-linux-x86_64.sh --skip-license --prefix=/usr/local && \
    rm cmake-4.1.0-linux-x86_64.sh

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone https://github.com/microsoft/vcpkg.git ${VCPKG_ROOT} && \
    ${VCPKG_ROOT}/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake/ ./cmake/
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/
COPY vcpkg.json ./

RUN cmake --preset ninja-multi-vcpkg && \
    cmake --build --preset prod

FROM ubuntu:latest

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && \
    apt-get install -y --no-install-recommends libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/ninja-multi-vcpkg/Release/warp /usr/local/bin/warp
COPY --from=build /src/build/ninja-multi-vcpkg/vcpkg_installed /opt/vcpkg/vcpkg_installed

ENV LD_LIBRARY_PATH=/opt/vcpkg/installed/x64-linux/lib:${LD_LIBRARY_PATH}

EXPOSE 1883

ENTRYPOINT ["/usr/local/bin/warp"]
