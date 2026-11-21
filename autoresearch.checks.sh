#!/usr/bin/env sh
set -eu
project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ctest --test-dir "$project/build" --output-on-failure
