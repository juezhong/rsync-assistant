#!/usr/bin/env sh
# Configure a macOS build. The normal macOS path uses the system rsync for the
# first build, while runtime still prefers build/private-rsync/rsync if present.
# Pass RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=ON only after installing the rsync
# build prerequisites (autoconf/automake, a C compiler, and make).
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build"}
bundled=${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC:-OFF}

printf '%s\n' "rsync-assistant macOS build"
printf '%s\n' "  source: $root"
printf '%s\n' "  build directory: $build_dir"
printf '%s\n' "  bundled rsync: $bundled"
printf '%s\n' "  step 1/2: configure CMake"
cmake -S "$root" -B "$build_dir" \
  -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC="$bundled"
printf '%s\n' "  step 2/2: build targets"
cmake --build "$build_dir" --parallel
