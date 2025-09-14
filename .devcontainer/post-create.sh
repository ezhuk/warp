#!/usr/bin/env bash
set -euo pipefail

curl -LsSf https://astral.sh/uv/install.sh | sh

if [ -n "${VCPKG_ROOT:-}" ]; then
  sudo rm -rf "${VCPKG_ROOT}"
  sudo git clone https://github.com/microsoft/vcpkg "${VCPKG_ROOT}"
  sudo chown -R "$(whoami)":vcpkg "${VCPKG_ROOT}"
  (cd "${VCPKG_ROOT}" && ./bootstrap-vcpkg.sh -disableMetrics)
fi

cmake --preset ninja-multi-vcpkg
