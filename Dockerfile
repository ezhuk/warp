FROM ubuntu:24.04 AS build

ARG BAZELISK_VERSION=1.29.0
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends build-essential ca-certificates curl git libssl-dev pkg-config python3 unzip zip && \
    rm -rf /var/lib/apt/lists/*

RUN curl -fsSL "https://github.com/bazelbuild/bazelisk/releases/download/v${BAZELISK_VERSION}/bazelisk-linux-amd64" -o /usr/local/bin/bazelisk && \
    chmod +x /usr/local/bin/bazelisk && \
    ln -s /usr/local/bin/bazelisk /usr/local/bin/bazel

WORKDIR /src

COPY . .

RUN bazel build -c opt //:warp_bin && \
    install -Dm755 bazel-bin/warp_bin /usr/local/bin/warp

RUN mkdir -p /opt/warp/lib && \
    ldd /usr/local/bin/warp | awk '/=> \// {print $3}' | xargs -r -I '{}' cp -L '{}' /opt/warp/lib/ && \
    ! ldd /usr/local/bin/warp | grep -q "not found"

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates libstdc++6 && \
    rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/bin/warp /usr/local/bin/warp
COPY --from=build /opt/warp/lib/ /opt/warp/lib/

ENV LD_LIBRARY_PATH=/opt/warp/lib

EXPOSE 1883

ENTRYPOINT ["/usr/local/bin/warp"]
