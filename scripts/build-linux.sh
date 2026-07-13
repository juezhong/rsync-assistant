#!/usr/bin/env sh
# Configure a Linux build. It builds the pinned rsync submodule by default.
# Set RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF only to explicitly select the
# system rsync after a private-rsync build failure.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build"}
bundled=${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC:-ON}

cmake -S "$root" -B "$build_dir" \
  -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC="$bundled"
cmake --build "$build_dir" --parallel
