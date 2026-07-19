#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only

set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "usage: expect_exit.sh EXPECTED COMMAND [ARGS...]" >&2
    exit 2
fi

expected="$1"
shift

set +e
"$@"
observed=$?
set -e

if [[ "$observed" -ne "$expected" ]]; then
    echo "expected exit ${expected}, observed ${observed}: $*" >&2
    exit 1
fi

echo "expect_exit: PASS expected=${expected} command=$1"
