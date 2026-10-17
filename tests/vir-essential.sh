#!/usr/bin/env bash
set -euo pipefail
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
# The helpers follow main, so they keep their own graphs for inspection.
"${compiler[@]}" --dump-ir -o "$task_dir/test" tests/vir-essential.c > "$task_dir/graph"
python3 - "$task_dir/graph" << 'PY'
import re
import sys

text = open(sys.argv[1]).read()
functions = dict(re.findall(r'function (\w+)\n(.*?)(?=\nfunction |\Z)', text, re.S))
for name in ('jump32', 'jump64', 'signed_jump'):
    assert 'branch ' not in functions[name], name
for name in ('reference32', 'reference64', 'signed_reference'):
    assert 'branch ' in functions[name] and 'volatile.store' in functions[name], name
for name in ('triangle', 'rotate'):
    assert '@' + name + '(' not in functions[name], name
assert functions['small_fib'].count('@small_fib(') == 1
# An even-count pointer reduction gains a second load and accumulator; an odd
# count would need a remainder iteration, so it stays as written.
for name, loads in (('reduce_even', 2), ('reduce_wide', 2), ('reduce_store', 2),
                    ('reduce_odd', 1)):
    assert functions[name].count(' = load.') == loads, name
assert '@effectful(' in functions['effectful']
assert '@narrow(' in functions['narrow']
# Narrow parameters need ABI conversion, so the recursion stays a call.
assert '@narrow_tail(' in functions['narrow_tail']
for name in ('signed_divide', 'signed_remainder', 'unsigned_divide',
             'unsigned_remainder', 'wide_divide', 'wide_remainder'):
    assert not re.search(r' = [su](?:div|rem)\.', functions[name]), name
PY

# The leaves defined above main are inlined, narrow arguments included: their
# calls and their unreachable definitions both disappear.
for leaf in mix_leaf narrow_leaf short_leaf; do
    if grep -q "call.* @$leaf(\|^function $leaf\$" "$task_dir/graph"; then
        echo "Leaf $leaf was not inlined" >&2
        exit 1
    fi
done
echo 'Essential VIR optimization graph checks passed'
