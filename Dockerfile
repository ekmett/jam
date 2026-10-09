# syntax=docker/dockerfile:1
# SPDX-License-Identifier: BSD-2-Clause OR Apache-2.0
FROM ghcr.io/ekmett/native:latest AS build
WORKDIR /src/jam
COPY . .
RUN cmake -S . -B /tmp/jam-build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/jam \
      -DJAM_BUILD_TESTS=ON \
 && cmake --build /tmp/jam-build --parallel 4 \
 && ctest --test-dir /tmp/jam-build --output-on-failure --no-tests=error \
 && cmake --install /tmp/jam-build

FROM ghcr.io/ekmett/native:latest
COPY --from=build /opt/jam /opt/jam
ENV CMAKE_PREFIX_PATH="/opt/jam:/opt/native"
# This stage has only installed libraries; CMake regenerates consumer BMIs.
RUN --mount=type=bind,source=t/installed,target=/tmp/jam-consumer \
    cmake -S /tmp/jam-consumer -B /tmp/jam-check -G Ninja -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /tmp/jam-check --parallel 4 \
 && ctest --test-dir /tmp/jam-check --output-on-failure --no-tests=error \
 && rm -rf /tmp/jam-check
WORKDIR /workspace
LABEL org.opencontainers.image.source="https://github.com/ekmett/jam" \
      org.opencontainers.image.title="jam" \
      org.opencontainers.image.description="Jam and Work on Native's LLVM 23 development image" \
      org.opencontainers.image.licenses="BSD-2-Clause OR Apache-2.0"
