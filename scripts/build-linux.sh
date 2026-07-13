#!/usr/bin/env sh
# Configure a Linux build. It builds the pinned rsync submodule by default.
# Set RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC=OFF only to explicitly select the
# system rsync after a private-rsync build failure.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${BUILD_DIR:-"$root/build"}
bundled=${RSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC:-ON}

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
  require_command python3 "install python3 and python3-cmarkgfm or python3-commonmark"
  if command -v python3 >/dev/null 2>&1 &&
     ! python3 -c 'import cmarkgfm' >/dev/null 2>&1 &&
     ! python3 -c 'import commonmark' >/dev/null 2>&1; then
    printf '%s\n' "missing Python Markdown module: install python3-cmarkgfm or python3-commonmark" >&2
    missing=1
  fi
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

printf '%s\n' "rsync-assistant Linux build"
printf '%s\n' "  source: $root"
printf '%s\n' "  build directory: $build_dir"
printf '%s\n' "  bundled rsync: $bundled"
printf '%s\n' "  step 1/2: configure CMake"
cmake -S "$root" -B "$build_dir" \
  -DRSYNC_ASSISTANT_BUILD_BUNDLED_RSYNC="$bundled"
printf '%s\n' "  step 2/2: build targets"
cmake --build "$build_dir" --parallel
