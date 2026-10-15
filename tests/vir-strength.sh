#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
read -r -a runner <<< "${TARGET_EXEC:-}"
"${compiler[@]}" --dump-ir -o "$task_dir/test" tests/vir-strength.c > "$task_dir/graph"
chmod +x "$task_dir/test"
"${runner[@]}" "$task_dir/test"
python3 - "$task_dir/graph" << 'PY'
import re, sys
text = open(sys.argv[1]).read()
functions = dict(re.findall(r'function (\w+)\n(.*?)(?=\nfunction |\Z)', text, re.S))
def strides(name):
    body = functions[name]
    params = set(re.findall(r'(%d\d+):ptr', body))
    constants = dict(re.findall(r'(%d\d+) = const\.i\d+ (\d+)\n', body))
    return [int(constants[offset]) for base, offset in
            re.findall(r'= ptradd (%d\d+), (%d\d+)\n', body)
            if base in params and offset in constants]
assert not re.search(r' = const\.i(?:32|64) 1\n', functions['dot'])
assert strides('bump') == [4]
assert strides('stride') == [16]
# The LP64 sign-extension discontinuity prohibits a constant pointer step.
expected = [] if 'sext.i32.i64' in functions['dynamic_stride'] else [16]
assert strides('dynamic_stride') == expected
PY
echo 'Native VIR strength reduction execution and graph checks passed'
