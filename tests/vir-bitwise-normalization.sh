#!/usr/bin/env bash
set -euo pipefail
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
"${compiler[@]}" --no-inline --dump-ir -o "$task_dir/test" tests/vir-bitwise-normalization.c > "$task_dir/graph"
python3 - "$task_dir/graph" << 'PY'
import re
import sys

text = open(sys.argv[1]).read()
functions = dict(re.findall(r'function (\w+)\n(.*?)(?=\nfunction |\Z)', text, re.S))
for name in ('absorb_and', 'absorb_or', 'complement'):
    assert not re.search(r' = bit(?:and|or|xor|not)\.', functions[name]), name
for name, opcode in (('repeat_and', 'bitand'), ('repeat_or', 'bitor')):
    assert len(re.findall(r' = ' + opcode + r'\.', functions[name])) == 1, name
assert not re.search(r' = add\.', functions['complement'])
assert 'const.i32 4294967294' in functions['complement']
assert functions['main'].count('volatile.load.') == 2
assert 'call.i32 @read_value()' in functions['main']
PY
echo 'Native VIR bitwise normalization graph checks passed'
