#!/usr/bin/env sh
# Configure a Linux build. It builds the pinned rsync submodule by default.
# Set RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF only to explicitly select the
# system rsync after a private-rsync build failure.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build"}
bundled=${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC:-ON}

printf '%s\n' "rsync-assistant Linux build"
printf '%s\n' "  source: $root"
printf '%s\n' "  build directory: $build_dir"
printf '%s\n' "  bundled rsync: $bundled"
printf '%s\n' "  step 1/2: configure CMake"
cmake -S "$root" -B "$build_dir" \
  -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC="$bundled"
printf '%s\n' "  step 2/2: build targets"
cmake --build "$build_dir" --parallel
