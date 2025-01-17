#!/usr/bin/env bash

set -u

if [ "$#" != 1 ]; then
    echo "Usage: $0 <stage>"
    exit 1
fi

case "$1" in
    "0")
        readonly SHECC="$PWD/out/shecc" ;;
    "1")
        readonly SHECC="$PWD/out/shecc-stage1.elf" ;;
    "2")
        readonly SHECC="$PWD/out/shecc-stage2.elf" ;;
    *)
        echo "$1 is not a valid stage"
        exit 1 ;;
esac

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

# try_compile_error - test shecc with invalid C program
# Usage:
# - try_compile_error invalid_input_code
# compile "invalid_input_code" with shecc so that shecc generates a
# compilation error message.
#
# This function uses shecc to compile invalid code and obtains the exit
# code returned by shecc. The exit code must be a non-zero value to
# indicate that shecc has the ability to parse the invalid code and
# output an error message.
function try_compile_error() {
    local input=$(cat)

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    echo "$input" > "$tmp_in"
    "$SHECC" -o "$tmp_exe" "$tmp_in"
    local exit_code=$?

    if [ 0 == $exit_code ]; then
        echo "Error: compilation is passed."
        exit 1
    fi
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

# octal constant (satisfying re(0[0-7]+))
expr 10 012
expr 65 0101

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

expr 11 "1 + 012"   # oct(012) = dec(10)
expr 25 "017 + 012" # oct(017) = dec(15), oct(012) = dec(10)

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

# value satisfying re(0[0-7]+) should be parsed as octal
expr 1 "010 == 8"
expr 1 "011 < 11"
expr 0 "021 >= 21"
expr 1 "(012 - 5) == 5"
expr 16 "0100 >> 2"

# oct(355) = bin(1110_1101), oct(~355) = bin(0001_0010) = dec(18)
expr 18 "~0355"

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
try_output 0 "128 59926 -6 -4 -500283"  << EOF
int main() {
  printf("%d %d %d %d %d", 32768 >> 8, 245458999 >> 12, -11 >> 1, -16 >> 2, -1000565 >> 1);
  return 0;
}
EOF
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

# variable with octal literals
items 10 "int var; var = 012; return var;"
items 100 "int var; var = 10 * 012; return var;"
items 32 "int var; var = 0100 / 2; return var;"
items 65 "int var; var = 010 << 3; var += 1; return var;"

# if
items 5 "if (1) return 5; else return 20;"
items 10 "if (0) return 5; else if (0) return 20; else return 10;"
items 10 "int a; a = 0; int b; b = 0; if (a) b = 10; else if (0) return a; else if (a) return b; else return 10;"
items 27 "int a; a = 15; int b; b = 2; if(a - 15) b = 10; else if (b) return a + b + 10; else if (a) return b; else return 10;"

items 8 "if (1) return 010; else return 11;"
items 10 "int a; a = 012 - 10; int b; b = 0100 - 64; if (a) b = 10; else if (0) return a; else if (a) return b; else return 10;"

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

# C-style comments / C++-style comments
# Start
try_ 0 << EOF
/* This is a test C-style comments */
int main() { return 0; }
EOF
try_ 0 << EOF
// This is a test C++-style comments
int main() { return 0; }
EOF
# Middle
try_ 0 << EOF
int main() {
    /* This is a test C-style comments */
    return 0;
}
EOF
try_ 0 << EOF
int main() {
    // This is a test C++-style comments
    return 0;
}
EOF
# End
try_ 0 << EOF
int main() { return 0; }
/* This is a test C-style comments */
EOF
try_ 0 << EOF
int main() { return 0; }
// This is a test C++-style comments
EOF

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

try_ 55 << EOF
int fib(int n, int a, int b)
{
    if (n == 0)
        return a;
    else if (n == 1)
        return b;
    return fib(n - 1, b, a + b);
}

int main() {
    return fib(012, 0, 1); /* octal(12) = dec(10) */
}
EOF

try_compile_error << EOF
int main() {
    int a = 03, b = 01118, c = 091;
    printf("%d %d %d\n", a, b, c);
    return 0;
}
EOF

# Unreachable declaration should not cause prog seg-falut (prog should leave normally with exit code 0)
try_ 0 << EOF
int main()
{
    return 0;
    int a = 5;
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
items 4 "int a; a = 2; a *= 2; return a;"
items 33 "int a; a = 100; a /= 3; return a;"
items 1 "int a; a = 100; a %= 3; return a;"
items 4 "int a; a = 2; a <<= 1; return a;"
items 2 "int a; a = 4; a >>= 1; return a;"
items 1 "int a; a = 1; a ^= 0; return a;"
items 20 "int *p; int a[3]; a[0] = 10; a[1] = 20; a[2] = 30; p = a; p+=1; return p[0];"

# sizeof
expr 0 "sizeof(void)";
expr 1 "sizeof(_Bool)";
expr 1 "sizeof(char)";
expr 4 "sizeof(int)";
# sizeof pointers
expr 4 "sizeof(void*)";
expr 4 "sizeof(_Bool*)";
expr 4 "sizeof(char*)";
expr 4 "sizeof(int*)";
# sizeof multi-level pointer
expr 4 "sizeof(void**)";
expr 4 "sizeof(_Bool**)";
expr 4 "sizeof(char**)";
expr 4 "sizeof(int**)";
# sizeof struct
try_ 4 << EOF
typedef struct {
    int a;
    int b;
} struct_t;
int main() { return sizeof(struct_t*); }
EOF
# sizeof enum
try_ 4 << EOF
typedef enum {
    A,
    B
} enum_t;
int main() { return sizeof(enum_t*); }
EOF

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

# #ifndef...#else...#endif
try_ 0 << EOF
#ifndef A
#define A 0
#else
#define A 1
#endif

#ifndef A
#define B 1
#else
#define B 0
#endif
int main()
{
    return A + B;
}
EOF

# include guard test, simulates inclusion of a file named defs.h and global.c
try_ 0 << EOF
/* #include "defs.h" */
#ifndef DEFS_H
#define DEFS_H

#define A 1

#endif
/* end if "defs.h" inclusion */

/* #include "global.c" */
#ifndef GLOBAL_C
#define GLOBAL_C

#define B 1

/* [global.c] #include "defs.h" */
#ifndef DEFS_H
#define DEFS_H

#define A 2

#endif
/* end if "defs.h" inclusion */
#endif
/* end if "global.c" inclusion */

int main()
{
    return A - B;
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

fmt_ans="0x0000000000000000000000ff00cde1
                      0xff00cde1
000000000000000000000000ff00cde1
                        ff00cde1
0xff00cde1
ff00cde1
0x00ff00cde1
  0xff00cde1
0000ff00cde1
    ff00cde1
00000000000000000000037700146741
                    037700146741
00000000000000000000037700146741
                     37700146741
037700146741
37700146741
037700146741
037700146741
037700146741
 37700146741
-0000000000000000000000016724511
                       -16724511
-16724511
-00016724511
   -16724511
0x0000000000000000000000fffff204
                      0xfffff204
000000000000000000000000fffff204
                        fffff204
0xfffff204
fffff204
0x00fffff204
  0xfffff204
0000fffff204
    fffff204
00000000000000000000037777771004
                    037777771004
00000000000000000000037777771004
                     37777771004
037777771004
37777771004
037777771004
037777771004
037777771004
 37777771004
-0000000000000000000000000003580
                           -3580
-3580
-00000003580
       -3580
0x00000000000000000000000001000c
                         0x1000c
0000000000000000000000000001000c
                           1000c
0x1000c
1000c
0x000001000c
     0x1000c
00000001000c
       1000c
00000000000000000000000000200014
                         0200014
00000000000000000000000000200014
                          200014
0200014
200014
000000200014
     0200014
000000200014
      200014
00000000000000000000000000065548
                           65548
65548
000000065548
       65548
00000000000000000000000000000000
                               0
00000000000000000000000000000000
                               0
0
0
000000000000
           0
000000000000
           0
00000000000000000000000000000000
                               0
00000000000000000000000000000000
                               0
0
0
000000000000
           0
000000000000
           0
00000000000000000000000000000000
                               0
0
000000000000
           0"

try_output 0 "$fmt_ans" << EOF
void printf_conversion(int num) {
    printf("%#032x\n%#32x\n%032x\n%32x\n%#x\n%x\n", num, num, num, num, num, num);
    printf("%#012x\n%#12x\n%012x\n%12x\n", num, num, num, num);
    printf("%#032o\n%#32o\n%032o\n%32o\n%#o\n%o\n", num, num, num, num, num, num);
    printf("%#012o\n%#12o\n%012o\n%12o\n", num, num, num, num);
    printf("%032d\n%32d\n%d\n", num, num, num);
    printf("%012d\n%12d\n", num, num);
}

int main() {
    int a = 0xFF00CDE1, b = 0xFFFFF204, c = 65548, d = 0;
    printf_conversion(a);
    printf_conversion(b);
    printf_conversion(c);
    printf_conversion(d);
    return 0;
}
EOF

try_ 0 << EOF
int main() {
    return '\0';
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

# Variables can be declared within a for-loop iteration
try_ 120 << EOF
int main()
{
    int fac = 1;
    for (int i = 1; i <= 5; i++) {
        fac = fac * i;
    }
    return fac;
}
EOF

# Multiplication for signed integers
try_output 0 "35 -35 -35 35" << EOF
int main()
{
    printf("%d %d %d %d\n", 5 * 7, 5 * (-7), (-5) * 7, (-5) * (-7));
    return 0;
}
EOF

try_output 0 "-212121 -535050 336105 666666666" << EOF
int main()
{
    printf("%d %d %d %d\n", (-333) * 637, 1450 * (-369), 37345 * 9, (-111111111) * (-6));
    return 0;
}
EOF

try_output 0 "1073676289 -131071 30" << EOF
int main()
{
    printf("%d %d %d\n", 32767 * 32767, 65535 * 65535, 54 * 5 * 954437177);
    return 0;
}
EOF

try_output 0 "-2 6 24" << EOF
int main()
{
    printf("%d %d %d\n", (-1) * 2, (-1) * 2 * (-3), (-1) * 2 * (-3) * 4);
    return 0;
}
EOF

# Division and modulo for signed integers
try_output 0 "-1 -2" << EOF
int main()
{
    printf("%d %d", -6 / 4, -6 % 4);
    return 0;
}
EOF

try_output 0 "-3 1" << EOF
int main()
{
    printf("%d %d", 7 / -2, 7 % -2);
    return 0;
}
EOF

try_output 0 "12 -1" << EOF
int main()
{
    printf("%d %d", -109 / -9, -109 % -9);
    return 0;
}
EOF

# octal(155) = dec(109), expect same output with above test suite
try_output 0 "12 -1" << EOF
int main()
{
    printf("%d %d", -0155 / -9, -0155 % -9);
    return 0;
}
EOF

try_output 0 "1365 0" << EOF
int main()
{
    printf("%d %d", 1365 / 1, 1365 % 1);
    return 0;
}
EOF

try_output 0 "-126322567 -8" << EOF
int main()
{
    printf("%d %d", -2147483647 / 17, -2147483647 % 17);
    return 0;
}
EOF

try_output 0 "-1 -1" << EOF
int main()
{
    printf("%d %d", -2147483648 / 2147483647, -2147483648 % 2147483647);
    return 0;
}
EOF

try_output 0 "-2147483648 0" << EOF
int main()
{
    printf("%d %d", -2147483648 / 1, -2147483648 % 1);
    return 0;
}
EOF

try_output 0 "-134217728 0" << EOF
int main()
{
    printf("%d %d", -2147483648 / 16, -2147483648 % 16);
    return 0;
}
EOF

try_output 0 "134217728 0" << EOF
int main()
{
    printf("%d %d", -2147483648 / -16, -2147483648 % -16);
    return 0;
}
EOF

try_output 0 "1 0" << EOF
int main()
{
    printf("%d %d", -2147483648 / -2147483648, -2147483648 % -2147483648);
    return 0;
}
EOF

try_output 0 "-8910720 -128" << EOF
int main()
{
    printf("%d %d", -2147483648 / 241, -2147483648 % 241);
    return 0;
}
EOF

try_output 0 "1" << EOF
int main()
{
    printf("%d", 6 / -2 / -3);
    return 0;
}
EOF

try_output 0 "0" << EOF
int main()
{
    printf("%d", 477 / 37 % -3);
    return 0;
}
EOF

try_output 0 "12" << EOF
int main()
{
    printf("%d", 477 / (37 + 1 / -3));
    return 0;
}
EOF

try_output 0 "-39" << EOF
int main()
{
    printf("%d", 477 / (37 / -3));
    return 0;
}
EOF

try_output 0 "2 3" << EOF
int div(int a, int b)
{
    return a / b;
}

int mod(int a, int b)
{
    return a % b;
}

int main()
{
    int a = div(4 + 5 + 6, 1 + 2 + 3);
    int b = mod(4 + 5 + 6, 1 + 2 + 3);
    printf("%d %d", a, b);
    return 0;
}
EOF

try_output 0 "-1422 -3094" << EOF
int div(int a, int b)
{
    return a / b;
}

int mod(int a, int b)
{
    return a % b;
}

int main()
{
    int a = div(-4449688, 3127);
    int b = mod(-4449688, 3127);
    printf("%d %d", a, b);
    return 0;
}
EOF

try_output 0 "-2267573 102" << EOF
int div(int a, int b)
{
    return a / b;
}

int mod(int a, int b)
{
    return a % b;
}

int main()
{
    int a = div(333333333, -147);
    int b = mod(333333333, -147);
    printf("%d %d", a, b);
    return 0;
}
EOF

try_output 0 "104643 -134" << EOF
int div(int a, int b)
{
    return a / b;
}

int mod(int a, int b)
{
    return a % b;
}

int main()
{
    int a = div(-104747777, -1001);
    int b = mod(-104747777, -1001);
    printf("%d %d", a, b);
    return 0;
}
EOF

# _Bool size should be equivalent to char, which is 1 byte
try_output 0 "1" << EOF
int main()
{
    printf("%d", sizeof(bool));
    return 0;
}
EOF

# Logical-and
try_output 0 "1 0 0 0" << EOF
int main()
{
    int a = 7, b = -15;
    int res = a && b;
    printf("%d ", res);
    a = 0;
    res = a && b;
    printf("%d ", res);
    a = -79;
    b = 0;
    res = a && b;
    printf("%d ", res);
    a = 0;
    b = 0;
    res = a && b;
    printf("%d", res);
    return 0;
}
EOF

# Logical-and, if statement
try_output 0 "6" << EOF
int main()
{
    int a = 4, b = 10;
    if (a && b)
        printf("%d", b - a);
    return 0;
}
EOF

# Logical-and, for loop condition 
try_output 0 "10" << EOF
int main()
{
    int a = 0;
    for (int i = 0; i < 10 && a < 10; i++) {
        a += 2;
    }
    printf("%d", a);
    return 0;
}
EOF

# Logical-and, while loop condition expression
try_output 0 "5" << EOF
int main()
{
    int a = 10, b = 1;
    while (a > 5 && b){
        a--;
    }
    printf("%d", a);
    return 0;
}
EOF

# Logical-and, do-while loop condition expression
try_output 0 "10" << EOF
int main()
{
    int a = 1, b = 5;
    do {
        a++;
    } while(a < 10 && b == 5);
    printf("%d", a);
    return 0;
}
EOF

# Logical-and, Left-to-right evaluation
try_output 0 "x > 0, 1" << EOF
int func(int x)
{
    if (x > 0) {
        printf("x > 0, ");
    }
    return x;
}
int main()
{
    int ret = 0;
    ret = 1 && func(5);
    if (ret)
        printf("%d", ret);
    ret = 0 && func(5);
    if (ret)
        printf("%d", ret);
    return 0;
}
EOF

# global character initialization
try_ 198 << EOF
char ch1 = 'A';
char ch2 = ('B');
char ch3 = (('C'));
int main()
{
    return ch1 + ch2 + ch3;
}
EOF

# global initialization with logical and equality operation
try_ 4 << EOF
int b1 = 1 && 1;
int b2 = 1 || 0;
int b3 = 1 == 1;
int b4 = 1 != 2;
int main()
{
    return b1 + b2 + b3 + b4;
}
EOF

# Logical-or: simplest case
expr 1 "41 || 20"

# Logical-or: control flow

ans="0 20
0"

try_output 0 "$ans" << EOF
int main()
{
    int a = 0;
    int b = 20;

    if (a || b)
        printf("%d %d\n", a, b);

    b = 0;

    if (a || b)
        printf("%d %d\n", a, b);
    else
        printf("0\n");

    return 0;
}
EOF

# Logical-or: for loop
ans="a--
a--
b--
b--
b--
b--
b--
b--
b--
b--
0 0 45"

try_output 0 "$ans" << EOF
int main()
{
    int a = 2, b = 8, c = 0;
    for (int i = 0; a || b; i++) {
        if (a) {
            c += i;
            a--;
            printf("a--\n");
            continue;
        }
        if (b) {
            c += i;
            b--;
            printf("b--\n");
            continue;
        }
    }
    printf("%d %d %d\n", a, b, c);

    return 0;
}
EOF

# Logical-or: while loop
ans="a -= 2
a -= 2
b -= 3
b -= 3
b -= 3
-1 0 13"

try_output 0 "$ans" << EOF
int main()
{
    int a = 3, b = 9, c = 0;
    while (a > 0 || b > 0) {
        if (a > 0) {
            c += 2;
            a -= 2;
            printf("a -= 2\n");
            continue;
        }
        if (b > 0) {
            c += 3;
            b -= 3;
            printf("b -= 3\n");
            continue;
        }
    }
    printf("%d %d %d\n", a, b, c);

    return 0;
}
EOF

# Logical-or: do-while loop
ans="do: a -= 2
do: a -= 2
do: a -= 2
do: b -= 5
do: b -= 5
do: b -= 5
do: b -= 5
-1 -4 -26"

try_output 0 "$ans" << EOF
int main()
{
    int a = 5, b = 16, c = 0;
    do {
        printf("do: ");
        if (a > 0) {
            c -= 2;
            a -= 2;
            printf("a -= 2\n");
        } else if (b > 0) {
            c -= 5;
            b -= 5;
            printf("b -= 5\n");
        }
    } while (a > 0 || b > 0);
    printf("%d %d %d\n", a, b, c);

    return 0;
}
EOF

# Logical-or: test the short-circuit principle
ans="10 > 0
10 0
20 > 0
0 20
get 0"

try_output 0 "$ans" << EOF
int func(int x)
{
    if (x > 0)
        printf("%d > 0\n", x);
    return x;
}

int main()
{
    int a = 10, b = 0, c = 20, d = -100;
    if (func(a) || func(b))
        printf("%d %d\n", a, b);

    if (func(b) || func(c))
        printf("%d %d\n", b, c);

    if (func(d + 100) || func(b))
        printf("%d %d\n", b, c);
    else
        printf("get 0\n");


    return 0;
}
EOF

# Logical-or and logical-and: More complex use cases
ans="0
1
1
1
1
1
1
1
0
1
1
1
1
1
0
1
0
func(10): 10 > 0
func(20): 20 > 0
func(0): 0 <= 0
0
func(10): 10 > 0
0
func(10): 10 > 0
0
func(0): 0 <= 0
func(10): 10 > 0
func(-100): -100 <= 0
1
func(0): 0 <= 0
func(0): 0 <= 0
0
func(0): 0 <= 0
func(0): 0 <= 0
0
func(0): 0 <= 0
func(10): 10 > 0
func(-100): -100 <= 0
1
func(10): 10 > 0
func(-100): -100 <= 0
1
func(10): 10 > 0
func(-100): -100 <= 0
1"

try_output 0 "$ans" << EOF
int func(int x)
{
    if (x > 0)
        printf("func(%d): %d > 0\n", x, x);
    else
	printf("func(%d): %d <= 0\n", x, x);
    return x;
}

int main()
{
    int a = 10, b = 20, c = 0, d = -100;
    printf("%d\n", a && b && c && d);
    printf("%d\n", a || b && c && d);
    printf("%d\n", a && b || c && d);
    printf("%d\n", a && b && c || d);
    printf("%d\n", a || b || c && d);
    printf("%d\n", a || b && c || d);
    printf("%d\n", a && b || c || d);
    printf("%d\n", a || b || c || d);

    printf("%d\n", (a || b) && c && d);
    printf("%d\n", a && (b || c) && d);
    printf("%d\n", a && b && (c || d));
    printf("%d\n", (a || b || c) && d);
    printf("%d\n", (a || b) && (c || d));
    printf("%d\n", a && (b || c || d));
    printf("%d\n", a * 0 && (b || c || d));
    printf("%d\n", a * 2 && (b || c || d));
    printf("%d\n", a && (b * 0 || c || d * 0));

    printf("%d\n", func(a) && func(b) && func(c));
    printf("%d\n", func(a) - a && func(b) && func(c));
    printf("%d\n", func(a) - a && func(b) && func(c) + 1);
    printf("%d\n", func(c) || func(a) && func(d));
    printf("%d\n", func(c) || func(c) && func(d));
    printf("%d\n", (func(c) || func(c)) && func(d + 100));
    printf("%d\n", func(c) || func(a) && func(d));
    printf("%d\n", func(a) && (func(d) || func(c)));
    printf("%d\n", func(a) * 2 && (func(d) || func(c)));

    return 0;
}
EOF

echo OK
