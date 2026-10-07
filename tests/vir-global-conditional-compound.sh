#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
for declaration in \
    'struct s *p = 1 ? &(struct t){1} : 0;' \
    'struct s *p = 0 ? 0 : &(struct t){1};' \
    'struct s **p = 1 ? &(struct s){1} : 0;' \
    'int p = 1 ? &(struct s){1} : 0;' \
    'struct s *p = 0 ? 0 : (1 ? &(struct t){1} : 0);' \
    'wide_callback *p = 1 ? &(callback){plus} : 0;'; do
    cat > "$task_dir/source.c" << SOURCE
struct s { int x; };
struct t { int x; };
int plus(int x) { return x + 1; }
typedef int (*callback)(int);
typedef long long (*wide_callback)(int);
$declaration
int main(void) { return 0; }
SOURCE
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/source.c" > "$task_dir/diagnostic" 2>&1; then
        echo "Invalid selected compound address accepted: $declaration" >&2
        exit 1
    fi
    grep -q 'Incompatible compound literal address' "$task_dir/diagnostic"
done
echo 'Global conditional compound destination diagnostics passed'

for declaration in 'int x = a[1];' 'int x = 1 ? a[1] : 0;'; do
    printf '%s\n' "int a[2]; $declaration int main(void) { return 0; }" > "$task_dir/source.c"
    status=0
    "${compiler[@]}" -o "$task_dir/test" "$task_dir/source.c" > "$task_dir/diagnostic" 2>&1 || status=$?
    if [[ $status != 1 ]]; then
        echo "Nonconstant array initializer rejection failed: $status" >&2
        exit 1
    fi
    grep -q 'Global initializer requires a constant value' "$task_dir/diagnostic"
done
