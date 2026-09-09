#!/usr/bin/env bash
set -euo pipefail

idf.py fullclean
idf.py set-target esp32s3
idf.py build 2>&1 | tee idf-build.log

if grep -Ei 'warning:|error:' idf-build.log; then
    echo "ESP-IDF build emitted a warning or error" >&2
    exit 1
fi
