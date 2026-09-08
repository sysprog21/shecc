#!/usr/bin/env bash
# Focused AAPCS64 integer ABI checks. These deliberately avoid host headers:
# shecc supplies its inlined libc and the same file can run for stage 0 or 2.
set -eu

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <stage> [<dynlink>]" >&2
    exit 2
fi

# TARGET_EXEC is a command with its own arguments, so both it and the compiler
# invocation are kept as argv arrays. A string passed through eval would lose a
# checkout path containing a space.
read -r -a runner <<< "${TARGET_EXEC:-}"

case "$1" in
    0) shecc=("$PWD/out/shecc") ;;
    2) shecc=("${runner[@]}" "$PWD/out/shecc-stage2.elf") ;;
    *)
        echo "arm64 ABI tests support stage 0 or 2" >&2
        exit 2
        ;;
esac

if [ "${2:-0}" = 1 ]; then
    shecc+=(--dynlink)
    link_mode=dynamic
else
    link_mode=static
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT
count=0

run_case()
{
    name=$1 expected=$2 source=$3
    src="$tmpdir/test.c" exe="$tmpdir/test.elf"
    printf '%s\n' "$source" > "$src"
    if ! output=$("${shecc[@]}" -o "$exe" "$src" 2>&1); then
        echo "FAIL: $name (compile)" >&2
        printf '%s\n' "$output" >&2
        exit 1
    fi
    # A generated ELF must be runnable as produced, whichever libc opened it.
    if [ ! -x "$exe" ]; then
        echo "FAIL: $name (output is not executable)" >&2
        exit 1
    fi
    set +e
    "${runner[@]}" "$exe" > /dev/null 2>&1
    got=$?
    set -e
    if [ "$got" -ne "$expected" ]; then
        echo "FAIL: $name (expected $expected, got $got)" >&2
        exit 1
    fi
    count=$((count + 1))
}

# x0-x7 are the eight AAPCS64 integer/pointer argument registers.
run_case 'eight register arguments' 36 '
int sum8(int a,int b,int c,int d,int e,int f,int g,int h) {
 return a+b+c+d+e+f+g+h;
}
int main(void) { return sum8(1,2,3,4,5,6,7,8); }'

# Arguments nine and ten must be read from the caller stack with 16-byte SP.
run_case 'overflow stack arguments' 55 '
int sum10(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j) {
 return a+b+c+d+e+f+g+h+i+j;
}
int main(void) { return sum10(1,2,3,4,5,6,7,8,9,10); }'

# The callee reads stack arguments at a fixed offset above its own frame, so
# that offset has to be rounded exactly as the prologue rounds the frame. A
# frame whose size is 8 modulo 16 is what catches a disagreement: an
# address-taken local forces the rounding that flips the parity.
run_case 'stack arguments with an odd-parity frame' 42 '
int pick(int a,int b,int c,int d,int e,int f,int g,int h,int i) {
 int v; int *p = &v; *p = 0;
 return i;
}
int main(void) { return pick(1,2,3,4,5,6,7,8,42); }'

run_case 'stack arguments read past live locals' 38 '
int sum10(int a,int b,int c,int d,int e,int f,int g,int h,int i,int j) {
 int t[3]; t[0]=a; t[1]=i; t[2]=j;
 return t[0]+t[1]+t[2]+i+j;
}
int main(void) {
 int keep = 5; int *q = &keep;
 return sum10(1,2,3,4,5,6,7,8,9,10) - *q + 4;
}'

# SP must stay 16-byte aligned: AArch64 Linux enables SP alignment checking, so
# a frame rounded to 8 faults the moment the next prologue touches [sp].
run_case 'stack stays 16-byte aligned through nesting' 42 '
int d3(int x) { int a; int *p = &a; *p = x; return *p + 1; }
int d2(int x) { int a,b,c; a=x; b=a+1; c=b+1; return d3(c) + 1; }
int d1(int x) { int a; int *p = &a; *p = x; return d2(*p) + 1; }
int main(void) { return d1(37); }'

# Narrow array elements must load sign-extended. Kept here rather than in
# tests/driver.sh because the Arm backend zero-extends them (see TODO.md).
run_case 'narrow array load and store' 42 '
int main(void) {
 char b[4]; short h[4]; int i;
 for (i = 0; i < 4; i++) { b[i] = -1 - i; h[i] = -1000 - i; }
 for (i = 0; i < 4; i++) {
  if (b[i] != -1 - i) return 1;
  if (h[i] != -1000 - i) return 2;
 }
 return 42;
}'

# x19-x28 are callee-saved; the backend currently allocates x20-x22.
run_case 'callee-saved allocation survives call' 42 '
int leaf(int x) { int a=7,b=8,c=9; return x+a+b+c; }
int caller(int x) { int keep=18; return leaf(x)+keep; }
int main(void) { return caller(0); }'

run_case 'struct member offset layout' 12 '
struct P { char tag; int number; };
int main(void) { struct P p; p.tag=1; p.number=11; return p.tag+p.number; }'

run_case 'defined function pointer' 42 '
int add1(int n) { return n + 1; }
int main(void) {
    int (*p)(int);
    p = add1;
    return p(41);
}'

run_case 'negative pointer index' 7 '
int main(void) {
    int a[3];
    int *p = &a[1];
    a[0] = 7;
    return p[-1];
}'

run_case 'negative runtime pointer subtraction' 42 '
int get(int i) {
    int a[3];
    int *p = &a[1];
    a[2] = 42;
    return *(p - i);
}
int main(void) { return get(-1); }'

if [ "$link_mode" = static ]; then

    # Linux AArch64 getpid is syscall 172. This verifies the backend moves the
    # syscall number to x8 and six arguments to x0..x5.
    run_case 'syscall register ABI' 0 '
int main(void) {
    return __syscall(172, 0, 0, 0, 0, 0, 0) > 0 ? 0 : 1;
}'
fi

if [ "$link_mode" = dynamic ]; then
    run_case 'external function pointer through PLT' 0 '
int puts(char *s);
int main(void) {
    int (*p)(char *);
    p = puts;
    p("external function pointer");
    return 0;
}'
fi

echo "AAPCS64 ABI: $count/$count passed (stage $1, $link_mode)"
