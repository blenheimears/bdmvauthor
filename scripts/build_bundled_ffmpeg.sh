#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <make-program>" >&2
  exit 2
fi

make_program="$1"
cores="${NIX_BUILD_CORES:-${CMAKE_BUILD_PARALLEL_LEVEL:-2}}"
case "$cores" in
  ''|*[!0-9]*) cores=2 ;;
  0) cores=2 ;;
esac
exec "$make_program" -j"$cores"
