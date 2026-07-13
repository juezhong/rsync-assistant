#!/usr/bin/env sh
# Configure a Linux build. It builds the pinned rsync submodule by default.
# Set RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF only to explicitly select the
# system rsync after a private-rsync build failure.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build"}
bundled=${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC:-ON}

detect_build_jobs() {
  detected=
  if command -v getconf >/dev/null 2>&1; then
    detected=$(getconf _NPROCESSORS_ONLN 2>/dev/null || true)
  fi
  case "$detected" in ''|*[!0-9]*)
    if command -v nproc >/dev/null 2>&1; then
      detected=$(nproc 2>/dev/null || true)
    fi
    ;;
  esac
  case "$detected" in ''|*[!0-9]*)
    detected=1
    ;;
  esac
  if [ "$detected" -gt 16 ]; then detected=16; fi
  printf '%s\n' "$detected"
}

build_jobs=$(detect_build_jobs)

missing=0
require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf '%s\n' "missing required command: $1 ($2)" >&2
    missing=1
  fi
}

printf '%s\n' "rsync-assistant Linux dependency check"
require_command cmake "install cmake"
require_command make "install make"
require_command cc "install gcc or another C compiler"
require_command c++ "install g++ or another C++ compiler"
require_command git "install git; CMake fetches FTXUI with it"
require_command ssh "install openssh-client"
require_command scp "install openssh-client"
if [ "$bundled" = "ON" ]; then
  require_command aclocal "install automake"
  require_command autoconf "install autoconf"
  require_command autoheader "install autoconf"
  require_command gawk "install gawk"
  if [ ! -x "$root/third_party/rsync/configure" ]; then
    printf '%s\n' "missing rsync submodule: run git submodule update --init --recursive" >&2
    missing=1
  fi
fi
if [ "$missing" -ne 0 ]; then
  printf '%s\n' "Install the missing prerequisites, then run this script again. See README: Linux bundled-rsync prerequisites." >&2
  exit 1
fi

# CMake's FindSQLite3 error is otherwise terse on minimal Linux installs.
sqlite_probe=$(mktemp "${TMPDIR:-/tmp}/rsync-assistant-sqlite.XXXXXX")
trap 'rm -f "$sqlite_probe"' EXIT HUP INT TERM
if ! printf '%s\n' '#include <sqlite3.h>' 'int main(void) { return sqlite3_libversion_number() == 0; }' |
     cc -x c - -lsqlite3 -o "$sqlite_probe" >/dev/null 2>&1; then
  printf '%s\n' "missing SQLite development files: install libsqlite3-dev (Debian/Ubuntu) or sqlite-devel (Fedora/RHEL)" >&2
  exit 1
fi

require_bundled_rsync_library() {
  feature=$1
  header=$2
  library=$3
  packages=$4
  if ! printf '#include <%s>\nint main(void) { return 0; }\n' "$header" |
       cc -x c - -l"$library" -o "$sqlite_probe" >/dev/null 2>&1; then
    printf '%s\n' "missing bundled-rsync feature: $feature (header <$header> or -l$library)" >&2
    printf '%s\n' "  install: $packages" >&2
    missing=1
  fi
}

if [ "$bundled" = "ON" ]; then
  # Upstream rsync's Git build enables these features by default and aborts
  # configure when their development headers or libraries are absent.
  require_bundled_rsync_library xxhash xxhash.h xxhash \
    "libxxhash-dev (Debian/Ubuntu) | xxhash-devel (Fedora/RHEL) | xxhash (Arch)"
  require_bundled_rsync_library lz4 lz4.h lz4 \
    "liblz4-dev (Debian/Ubuntu) | lz4-devel (Fedora/RHEL) | lz4 (Arch)"
  require_bundled_rsync_library zstd zstd.h zstd \
    "libzstd-dev (Debian/Ubuntu) | libzstd-devel (Fedora/RHEL) | zstd (Arch)"
  require_bundled_rsync_library openssl openssl/md4.h crypto \
    "libssl-dev (Debian/Ubuntu) | openssl-devel (Fedora/RHEL) | openssl (Arch)"
fi
if [ "$missing" -ne 0 ]; then
  printf '%s\n' "Install the listed bundled-rsync development packages, then run this script again." >&2
  exit 1
fi

printf '%s\n' "rsync-assistant Linux build"
printf '%s\n' "  source: $root"
printf '%s\n' "  build directory: $build_dir"
printf '%s\n' "  bundled rsync: $bundled"
printf '%s\n' "  parallel compile jobs: $build_jobs (min(logical CPUs, 16))"
printf '%s\n' "  step 1/3: configure CMake"
cmake -S "$root" -B "$build_dir" \
  -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC="$bundled" \
  -DRSYNC_ASSISTANT_BUILD_JOBS="$build_jobs"
if [ "$bundled" = "ON" ]; then
  # Keep rsync's configure output in its own phase.  Otherwise CMake may
  # schedule it alongside FTXUI compilation and interleave both logs.
  printf '%s\n' "  step 2/3: build bundled rsync"
  cmake --build "$build_dir" --target private-rsync --parallel "$build_jobs"
else
  printf '%s\n' "  step 2/3: bundled rsync disabled (system rsync selected)"
fi
printf '%s\n' "  step 3/3: build rsync-assistant and FTXUI"
cmake --build "$build_dir" --parallel "$build_jobs"
