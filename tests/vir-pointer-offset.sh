#!/usr/bin/env bash
set -euo pipefail
compiler=${1:-out/shecc}
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"
"$compiler" -o "$task_dir/test" tests/vir-pointer-offset.c
chmod +x "$task_dir/test"
"${runner[@]}" "$task_dir/test"
echo 'Native VIR wide pointer offset test passed'
