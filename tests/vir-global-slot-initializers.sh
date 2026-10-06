#!/usr/bin/env bash
set -euo pipefail
# The compiler command, an emulator prefix included, one word per argument.
compiler=("$@")
[ "${#compiler[@]}" -gt 0 ] || compiler=(out/shecc)
task_dir=$(mktemp -d)
trap 'rm -rf "$task_dir"' EXIT
cases=(
    'int x; int (**s)(void)=(char *)(void *)&x;'
    'int (**s)(void)=(char *)(void *)0;'
    'int f(void){return 1;} int (*cb)(void)=f; int (**s)(void)=(char *)&cb;'
    'int x; int (**s)(void)=&x;'
    'typedef int (*fn_t)(void); int x; fn_t *s=&x;'
    'int x; int main(void){static int (**s)(void)=&x;return 0;}'
    'int x; int (**s)(void)=(char *)&x;'
    'int inc(int x){return x+1;} int (*cb)(int)=inc; int (*t[1])(int)={&cb};'
    'int inc(int x){return x+1;} int (*raw[1])(int)={inc}; int (*t[1])(int)={raw};'
    'typedef int (*cb_t)(int); int inc(int x){return x+1;} cb_t cb=inc; cb_t t[1]={&cb};'
    'int inc(int x){return x+1;} int (*cb)(int)=inc; int main(void){static int (*t[1])(int)={&cb};return 0;}'
)
for source in "${cases[@]}"; do
    printf '%s
' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Incompatible object address accepted as callback slot' >&2
        exit 1
    fi
    grep -q 'incompatible callback slot types in initializer' "$task_dir/diagnostic"
done
echo 'Global callback slot initializer rejection checks passed'
# A cast cannot turn a function symbol into an object pointer or callback slot.
for source in \
    'int five(void){return 5;} int (**s)(void)=(int (**)(void))five;' \
    'int five(void){return 5;} void *t[1]={(void *)five};' \
    'int five(void){return 5;} char *p=(char *)(&five);' \
    'int five(void){return 5;} void *t[1]={(void *)((five))};' \
    'int five(void){return 5;} struct{char *p;} s={(char *)five};' \
    'int five(void){return 5;} struct{int (**s)(void);int n;} g={(int (**)(void))five,1};'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Function address accepted as object pointer' >&2
        exit 1
    fi
    grep -q 'incompatible function pointer types' "$task_dir/diagnostic"
done

# An operand cast does not convert the enclosing logical or conditional result.
for source in \
    'int *p=(int *)"a" && 1;' \
    'int *p=(int *)"a" || 0;' \
    'int *p=(int *)(char *)"a" && 1;' \
    'int *p=(int *)"a" ? 1 : 0;' \
    'int main(void){static int *p=(int *)"a" && 1;return 0;}' \
    'char *p=(char *)"ab"[1] && 1;' \
    'char *p=(char *)"ab"[1] || 0;' \
    'char *p=(char *)"ab"[1] ? 1 : 0;' \
    'void (*p)(void)=(void (*)(void))"a" && 1;'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Operand cast converted an enclosing scalar result' >&2
        exit 1
    fi
    grep -q 'integer converted to pointer without a cast' "$task_dir/diagnostic"
done

# Global shifts use the promoted left operand width, regardless of RHS rank.
for source in \
    'int shifted = 1 << 32;' \
    'int shifted = 1 >> 32;' \
    'int shifted = 1 << 32ULL;' \
    'int shifted = 1 << 64ULL;' \
    'int shifted = 1 << 0x100000000ULL;' \
    'int shifted = 1 << -1;' \
    'int shifted = (signed char)1 << 32;' \
    'int shifted = (unsigned short)1 >> 32;' \
    'unsigned shifted = (unsigned int)1 << 32;'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Out-of-range narrow global shift accepted' >&2
        exit 1
    fi
    grep -q 'Shift count out of range in constant expression' "$task_dir/diagnostic"
done
echo 'Global initializer shift width checks passed'

for source in \
    'char *p=(char *)"ab"[1] == 98;' \
    'int compared = (char *)-1 == -1;' \
    'int compared = (char *)1 != 1;' \
    'int compared = 2 < (char *)3;'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Global pointer comparison with a nonzero integer accepted' >&2
        exit 1
    fi
    grep -q 'Global pointer comparison requires a null constant' "$task_dir/diagnostic"
done
echo 'Global pointer comparison operand checks passed'

# Selected nonconstant operands still cannot initialize static storage.
for source in \
    'int object; unsigned long long value = 1 ? object : 1U;' \
    'int object; unsigned long long value = 1 && object ? 0x100000000ULL : 1U;'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Selected nonconstant global operand accepted' >&2
        exit 1
    fi
    grep -q 'Identifier is not an integer constant' "$task_dir/diagnostic"
done

# A converted nonzero integer arm does not become a null pointer constant.
for source in \
    'char *p = 1 ? "a" : (1 ? 1 : 0U);' \
    'char *p = 1 ? "a" : (1 ? -1 : 0LL);' \
    'char *p = 1 ? "a" : (1 ? -1 : 0ULL);' \
    'char *p = 0 ? "a" : (1 ? -1 : 0ULL);'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Nonzero conditional integer accepted as null' >&2
        exit 1
    fi
    grep -q 'Conditional pointer operands must be pointers or null' "$task_dir/diagnostic"
done

# Folding a discarded constant expression must preserve its nonzero value.
for source in \
    'char *p = 1 ? "a" : 1ULL + 1;' \
    'char *p = 1 ? "a" : 3U | 0;' \
    'char *p = 1 ? "a" : (1ULL != 1 ? 0 : 5);'; do
    printf '%s\n' "$source" > "$task_dir/invalid.c"
    if "${compiler[@]}" -o "$task_dir/test" "$task_dir/invalid.c" > "$task_dir/diagnostic" 2>&1; then
        echo 'Folded nonzero conditional integer accepted as null' >&2
        exit 1
    fi
    grep -q 'Conditional pointer operands must be pointers or null' "$task_dir/diagnostic"
done
