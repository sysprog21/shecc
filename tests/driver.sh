#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

# try - test shecc with given code
# Usage:
# - try exit_code input_code
# compile "input_code" with shecc and expect the compile program exit with
# code "exit_code".
#
# - try exit_code expected_output input_code
# compile "input_code" with shecc and expect the compile program output
# "expected_output" and exit with code "exit_code".
function try() {
    local expected="$1"
    if [ $# -eq 2 ]; then
        local input="$2"
    elif [ $# -eq 3 ]; then
        local expected_output="$2"
        local input="$3"
    fi

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    echo "$input" > "$tmp_in"
    "$SHECC" -o "$tmp_exe" "$tmp_in"
    chmod +x $tmp_exe

    local output=''
    output=$($TARGET_EXEC "$tmp_exe")
    local actual="$?"

    if [ "$actual" != "$expected" ]; then
        echo "$input => $expected expected, but got $actual"
        echo "input: $tmp_in"
        echo "executable: $tmp_exe"
        exit 1
    elif [ "${expected_output+x}" != "" ] && [ "$output" != "$expected_output" ]; then
        echo "$input => $expected_output expected, but got $output"
        echo "input: $tmp_in"
        echo "executable: $tmp_exe"
        exit 2
    else
        echo "$input"
        echo "exit code => $actual"
        echo "output => $output"
    fi
}

function try_() {
    local expected="$1"
    local input="$(cat)"
    try "$expected" "$input"
}

function try_output() {
    local expected="$1"
    local expected_output="$2"
    local input="$(cat)"
    try "$expected" "$expected_output" "$input"
}

function items() {
    local expected="$1"
    local input="$2"
    try "$expected" "int main(int argc, int argv) { $input }"
}

function expr() {
    local expected="$1"
    local input="$2"
    items "$expected" "exit($input);"
}

# just a number
expr 0 0
expr 42 42

# arithmetic
expr 42 "24 + 18"
expr 30 "58 - 28"
expr 10 "5 * 2"
expr 4 "16 >> 2"

expr 20 "8 + 3 * 4"
expr 54 "(11 - 2) * 6"
expr 10 "9 / 3 + 7"
expr 8 "8 / (4 - 3)"
expr 35 "8 + 3 * 5 + 2 * 6"

expr 55 "1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9 + 10"
expr 55 "((((((((1 + 2) + 3) + 4) + 5) + 6) + 7) + 8) + 9) + 10"
expr 55 "1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + (9 + 10))))))))"
expr 210 "1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + (9 + (10 + (11 + (12 + (13 + (14 + (15 + (16 + (17 + (18 + (19 + 20))))))))))))))))))"

# expr 21 "+1+20"
# expr 10 "-15+(+35-10)"

expr 2 "5 % 3"
expr 6 "111 % 7"

expr 1 "10 > 5"
expr 1 "3+3 > 5"
expr 0 "30 == 20"
expr 0 "5 >= 10"
expr 1 "5 >= 5"
expr 1 "30 != 20"

expr 0 "!237"
expr 18 "~237"

expr 0 "0 || 0"
expr 1 "1 || 0"
expr 1 "1 || 1"
expr 0 "0 && 0"
expr 0 "1 && 0"
expr 1 "1 && 1"

expr 16 "2 << 3"
expr 32 "256 >> 3"
expr 239 "237 | 106"
expr 135 "237 ^ 106"
expr 104 "237 & 106"

# return statements
items 1 "return 1;";
items 42 "return 2*21;";

# variables
items 10 "int var; var = 10; return var;"
items 42 "int va; int vb; va = 11; vb = 31; int vc; vc = va + vb; return vc;"
items 50 "int v; v = 30; v = 50; return v;"

# if
items 5 "if (1) return 5; else return 20;"
items 10 "if (0) return 5; else if (0) return 20; else return 10;"
items 10 "int a; a = 0; int b; b = 0; if (a) b = 10; else if (0) return a; else if (a) return b; else return 10;"
items 27 "int a; a = 15; int b; b = 2; if(a - 15) b = 10; else if (b) return a + b + 10; else if (a) return b; else return 10;"

# compound
items 5 "{ return 5; }"
items 10 "{ int a; a = 5; { a = 5 + a; } return a; }"
items 20 "int a; a = 10; if (1) { a = 20; } else { a = 10; } return a;"
items 30 "int a; a = 10; if (a) { if (a - 10) { a = a + 1; } else { a = a + 20; } a = a - 10; } else { a = a + 5; } return a + 10;"

# loop
items 55 "int acc; int p; acc = 0; p = 10; while (p) { acc = acc + p; p = p - 1; } return acc;"
items 60 "int acc; acc = 15; do { acc = acc * -2; } while (acc < 0); return acc;"
items 45 "int i; int acc; acc = 0; for (i = 0; i < 10; ++i) { acc = acc + i; } return acc;"
items 45 "int i; int j; i=0; j=0; while (i<10) { j=j+i; i=i+1; } return j;"
items 1 "int x; x=0; do {x = x + 1; break;} while (1); return x;"
items 2 "int x; x=0; do {x++; continue; abort();} while (x < 2); return x;"
items 2 "int x; x=0; while(x < 2){x++; continue; abort();} return x;"
items 7 "int i; i=0; int j; for (j = 0; j < 10; j++) { if (j < 3) continue; i = i + 1; } return i;"
items 10 "while(0); return 10;"
items 10 "while(1) break; return 10;"
items 10 "for(;;) break; return 10;"
items 0 "int x; for(x = 10; x > 0; x--); return x;"
items 30 "int i; int acc; i = 0; acc = 0; do { i = i + 1; if (i - 1 < 5) continue; acc = acc + i; if (i == 9) break; } while (i < 10); return acc;"
items 26 "int acc; acc = 0; int i; for (i = 0; i < 100; i++) { if (i < 5) continue; if (i == 9) break; acc = acc + i; } return acc;"

# functions
try_ 55 << EOF
int sum(int m, int n) {
    int acc;
    acc = 0;
    int i;
    for (i = m; i <= n; i = i + 1)
        acc = acc + i;
    return acc;
}

int main() {
    return sum(1, 10);
}
EOF

try_ 120 << EOF
int fact(int x) {
    if (x == 0) {
        return 1;
    } else {
        return x * fact(x - 1);
    }
}

int main() {
    return fact(5);
}
EOF

try_ 1 << EOF
int is_odd(int x);

int is_even(int x) {
    if (x == 0) {
        return 1;
    } else {
        return is_odd(x - 1);
    }
}

int is_odd(int x) {
    if (x == 0) {
        return 0;
    } else {
        return is_even(x - 1);
    }
}

int main() {
    return is_even(20);
}
EOF

try_ 253 << EOF
int ack(int m, int n) {
    if (m == 0) {
        return n + 1;
    } else if (n == 0) {
        return ack(m - 1, 1);
    } else {
        return ack(m - 1, ack(m, n - 1));
    }
}

int main() {
    return ack(3, 5);
}
EOF

# pointers
items 3 "int x; int *y; x = 3; y = &x; return y[0];"
items 5 "int b; int *a; b = 10; a = &b; a[0] = 5; return b;"
items 2 "int x[2]; int y; x[1] = 2; y = *(x + 1); return y;"
items 2 "int x; int *y; int z; z = 2; y = &z; x = *y; return x;"
try_ 10 << EOF
void change_it(int *p) {
    if (p[0] == 0) {
        p[0] = 10;
    } else {
        p[0] = p[0] - 1;
    }
}

int main() {
  int v;
  v = 2;
  change_it(&v);
  change_it(&v);
  change_it(&v);
  return v;
}
EOF

# function pointers
try_ 18 << EOF
typedef struct {
    int (*ta)();
    int (*tb)(int);
} fptrs;
int t1() { return 7; }
int t2(int x) { return x + 1; }
int main() {
    fptrs fb;
    fptrs *fs = &fb;
    fs->ta = t1;
    fs->tb = t2;
    return fs->ta() + fs->tb(10);
}
EOF

# arrays
try_ 12 << EOF
int nth_of(int *a, int i) {
    return a[i];
}

int main() {
    int ary[5];
    int i;
    int v0;
    int v1;
    int v2;

    for (i = 0; i < 5; i++) {
        ary[i] = i * 2;
    }

    v0 = nth_of(ary, 0);
    v1 = nth_of(ary, 2);
    v2 = nth_of(ary, 4);
    return v0 + v1 + v2;
}
EOF

# global initialization
try_ 20 << EOF
int a = 5 * 2;
int b = -4 * 3 + 7 + 9 / 3 * 5;
int main()
{
    return a + b;
}
EOF

# conditional operator
expr 10 "1 ? 10 : 5"
expr 25 "0 ? 10 : 25"

# compound assignemnt
items 5 "int a; a = 2; a += 3; return a;"
items 5 "int a; a = 10; a -= 5; return a;"
items 20 "int *p; int a[3]; a[0] = 10; a[1] = 20; a[2] = 30; p = a; p+=1; return p[0];"

# sizeof
expr 4 "sizeof(int)";
expr 1 "sizeof(char)";

# switch-case
items 10 "int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } return a;"
items 10 "int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } return a;"

# enum
try_ 6 << EOF
typedef enum { enum1 = 5, enum2 } enum_t;
int main() { enum_t v = enum2; return v; }
EOF

# malloc and free
try_ 1 << EOF
int main()
{
    /* change test bench if different scheme apply */
    int *a = malloc(sizeof(int) * 5);
    free(a);
    if (a == NULL)
        abort();
    int *b = malloc(sizeof(int) * 3);

    /* "malloc" will reuse memory free'd by "free(a)" */
    return a == b;
}
EOF

try_ 1 << EOF
int main()
{
    char *ptr = "hello";
    return (0 == strcmp(ptr, "hello")) == (!strcmp(ptr, "hello"));
}
EOF

# #ifdef...#else...#endif
try_ 0 << EOF
#define A 0
#define B 200
int main()
{
    int x;
#ifdef A
    x = A;
#else
    x = B;
#endif
    return x;
}
EOF

# #if defined(...) ... #elif defined(...) ... #else ... #endif
try_ 0 << EOF
#define A 0
#define B 0xDEAD
int main()
{
    int x;
#if defined(A)
    x = A;
#elif defined(B)
    x = B;
#else
    x = 0xCAFE;
#endif
    return x;
}
EOF

# #define ... #undef
try_output 0 "1" << EOF
#define A 1
void log()
{
    printf("%d", A);
}
#undef A
#define A 0
int main()
{
    log();
    return A;
}
EOF

# format
try_output 0 "2147483647" << EOF
int main() {
    printf("%d", 2147483647);
    return 0;
}
EOF

try_output 0 "-2147483648" << EOF
int main() {
    printf("%d", -2147483648);
    return 0;
}
EOF

try_output 0 "-2147483647" << EOF
int main() {
    printf("%d", -2147483647);
    return 0;
}
EOF

try_output 0 "-214748364" << EOF
int main() {
    printf("%d", -214748364);
    return 0;
}
EOF

try_output 0 " -214748364" << EOF
int main() {
    printf("%11d", -214748364);
    return 0;
}
EOF

try_output 0 "      -214748364" << EOF
int main() {
    printf("%16d", -214748364);
    return 0;
}
EOF

try_output 0 "$(printf '%97s123')" << EOF
int main() {
    printf("%100d", 123);
    return 0;
}
EOF

try_output 0 "%1" << EOF
int main() {
    printf("%%%d", 1);
    return 0;
}
EOF

try_output 0 "144" << EOF
int main() {
    printf("%o", 100);
    return 0;
}
EOF

try_output 0 "0144" << EOF
int main() {
    printf("%#o", 100);
    return 0;
}
EOF

try_output 0 "7f" << EOF
int main() {
    printf("%x", 127);
    return 0;
}
EOF

try_output 0 "0x7f" << EOF
int main() {
    printf("%#x", 127);
    return 0;
}
EOF

# function-like macro
try_ 1 << EOF
#define MAX(a, b) ((a) > (b) ? (a) : (b))
int main()
{
    int x = 0, y = 1;
    return MAX(x, y);
}
EOF

try_ 7 << EOF
#define M(a, b) a + b
int main()
{
    return M(1, 2) * 3;
}
EOF

# function-like variadic macro
try_ 2 << EOF
#define M(m, n, ...)     \
    do {                 \
        x = __VA_ARGS__; \
    } while (0)
int main()
{
    int x = 0;
    M(0, 1, 2);
    return x;
}
EOF

try_output 0 "Wrapper: Hello World!" << EOF
#define WRAPPER(...)         \
    do {                     \
        printf("Wrapper: "); \
        printf(__VA_ARGS__); \
    } while (0)
int main()
{
    WRAPPER("%s", "Hello World!");
    return 0;
}
EOF

try_ 0 << EOF
#if 1 || 0
#define A 0
#elif 1 && 0
#define A 1
#else
#define A 2
#endif
int main()
{
    return A;
}
EOF

# optimizers

# common subexpression elimination (CSE)
try_ 1 << EOF
int i = 0;
void func()
{
    i = 1;
}
int main()
{
    char arr[2], t;
    arr[0] = 0;
    arr[1] = 1;
    t = arr[i];
    func();
    t = arr[i];
    return t;
}
EOF

# constant folding
try_ 20 << EOF
int main()
{
    int a = 2;            /* constant assingment */
    int b = a;            /* assignment via constant representation */
    int c = a + b;
    int d = c + 8;        /* mixed assigment */
    return a + b + c + d; /* chained assignment */
}
EOF

echo OK
