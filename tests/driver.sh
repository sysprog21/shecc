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
        readonly SHECC="$TARGET_EXEC $PWD/out/shecc-stage1.elf" ;;
    "2")
        readonly SHECC="$TARGET_EXEC $PWD/out/shecc-stage2.elf" ;;
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
    $SHECC -o "$tmp_exe" "$tmp_in"
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
    $SHECC -o "$tmp_exe" "$tmp_in"
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

# try_large - test shecc with large return values (> 255)
# Usage:
# - try_large expected_value input_code
# compile "input_code" with shecc and verify the return value by printing it
# instead of using exit code (which is limited to 0-255).
function try_large() {
    local expected="$1"
    local input="$(cat)"

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"

    # Wrap the input to print the return value
    cat > "$tmp_in" << EOF
int printf(char *format, ...);
$input
int main() {
    int result = test_function();
    printf("%d", result);
    return 0;
}
EOF

    $SHECC -o "$tmp_exe" "$tmp_in"
    chmod +x $tmp_exe

    local output=$($TARGET_EXEC "$tmp_exe")

    if [ "$output" != "$expected" ]; then
        echo "$input => $expected expected, but got $output"
        echo "input: $tmp_in"
        echo "executable: $tmp_exe"
        exit 1
    else
        echo "Large value test: $expected"
        echo "output => $output"
    fi
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
try_ 0 << EOF
int main(void) {
    return 0;
}
EOF

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

# Test large fibonacci values using the new try_large function
try_large 987 << EOF
int fib(int n, int a, int b)
{
    if (n == 0)
        return a;
    if (n == 1)
        return b;
    return fib(n - 1, b, a + b);
}

int test_function() {
    return fib(16, 0, 1); /* fib(16) = 987 */
}
EOF

# Test other large values
try_large 1000 << EOF
int test_function() {
    return 1000;
}
EOF

try_large 65536 << EOF
int test_function() {
    return 1 << 16; /* 2^16 = 65536 */
}
EOF

try_large 999999 << EOF
int test_function() {
    return 999999;
}
EOF

try_compile_error << EOF
int main() {
    int a = 03, b = 01118, c = 091;
    printf("%d %d %d\n", a, b, c);
    return 0;
}
EOF

try_compile_error << EOF
int main(void v) {}
EOF

try_compile_error << EOF
int main(void, int i) {}
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

# struct with multiple pointer declarations in same line
try_ 42 << EOF
typedef struct chunk {
    struct chunk *next, *prev;
    int size;
} chunk_t;

int main() {
    chunk_t c;
    c.size = 42;
    return c.size;
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

# Mixed subscript and arrow / dot operators,
# excerpted and modified from issue #165
try_output 0 "DDDDDDMMMEEE1" << EOF
#include <stdlib.h>
#include <string.h>

char a[100];

typedef struct {
    char *raw;
} data_t;

int main() {
    strcpy(a, "DATA");
    data_t *data = malloc(sizeof(data_t));
    data->raw = a;
    data_t data2;
    data2.raw = a;
    char *raw = data->raw;
    char *raw2 = data2.raw;
    /* mixed arrow / dot with subscript operators dereference */
    printf("%c", a[0]);
    printf("%c", raw[0]);
    printf("%c", data->raw[0]);
    printf("%c", a[0]);
    printf("%c", raw2[0]);
    printf("%c", data2.raw[0]);
    /* mixed arrow / dot with subscript operators assignment */
    data2.raw[0] = 'M';
    data->raw[1] = 'E';
    printf("%c", a[0]);
    printf("%c", raw[0]);
    printf("%c", data->raw[0]);
    printf("%c", a[1]);
    printf("%c", raw2[1]);
    printf("%c", data2.raw[1]);
    /* their addresses should be same */
    printf("%d", &data2.raw[0] == &data->raw[0]);
    free(data);
    return 0;
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

# sizeof with expressions
items 4 "int x = 42; return sizeof(x);"
items 4 "int arr[5]; return sizeof(arr[0]);"
items 4 "int x = 10; int *ptr = &x; return sizeof(*ptr);"
items 1 "char c = 'A'; return sizeof(c);"
items 4 "int a = 1, b = 2; return sizeof(a + b);"

# sizeof with complex expressions
try_ 4 << EOF
int main() {
    int arr[10];
    int i = 5;
    return sizeof(arr[i]);
}
EOF

try_ 4 << EOF
int main() {
    int x = 100;
    int *p = &x;
    int **pp = &p;
    return sizeof(**pp);
}
EOF

try_ 4 << EOF
int main() {
    int values[3];
    values[1] = 2;
    values[2] = 3;
    return sizeof(values[1] + values[2]);
}
EOF

# sizeof with function calls
try_ 4 << EOF
int get_value() { return 42; }
int main() {
    return sizeof(get_value());
}
EOF

# sizeof with ternary expressions
try_ 4 << EOF
int main() {
    int a = 5, b = 10;
    return sizeof(a > b ? a : b);
}
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

# macro parameter substitution works in expression contexts
try_ 15 << EOF
#define ADD_PARAMS(a, b) ((a) + (b))
int main()
{
    int x = 5, y = 10;
    return ADD_PARAMS(x, y);
}
EOF

# macro with assignment operators
try_ 18 << EOF
#define ASSIGN_MACRO(variable, val) \
    variable = variable + val + 10
int main()
{
    int x = 5;
    ASSIGN_MACRO(x, 3);
    return x;
}
EOF

try_ 27 << EOF
#define COMPOUND_ASSIGN(variable, val) \
    variable += val + 10
int main()
{
    int y = 10;
    COMPOUND_ASSIGN(y, 7);
    return y;
}
EOF

try_ 42 << EOF
#define SET_VAR(var, value) var = value
int main()
{
    int z = 0;
    SET_VAR(z, 42);
    return z;
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

# global string initialization and modification
try_output 0 "Hello World!Hallo World!" << EOF
char *data = "Hello World!";

int main(void)
{
    printf(data);
    data[1] = 'a';
    printf(data);
    return 0;
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

# printf family, including truncation and zero size input
try_output 11 "Hello World" << EOF
int main() {
    int written = printf("Hello World");
    return written;
}
EOF

# tests printf returns EBADF (errno 9) when stdout is closed
try_output 1 "" << EOF
int main()
{
    __syscall(__syscall_close, 1);
    int written = printf("Hello\n");
    return written == -9;
}
EOF

try_output 11 "Hello World" << EOF
int main() {
    char buffer[50];
    int written = sprintf(buffer, "Hello World");
    printf("%s", buffer);
    return written;
}
EOF

try_output 16 "Hello World 1123" << EOF
int main() {
    char buffer[50];
    int written = sprintf(buffer, "Hello %s %d", "World", 1123);
    printf("%s", buffer);
    return written;
}
EOF

# The following cases validate the behavior and return value of
# snprintf().
#
# This case is a normal case and outputs the complete string
# because the given buffer size is large enough.
try_output 16 "Hello World 1123" << EOF
int main() {
    char buffer[50];
    int written = snprintf(buffer, 50, "Hello %s %d", "World", 1123);
    printf("%s", buffer);
    return written;
}
EOF

# If n is zero, nothing is written.
#
# Thus, the output should be the string containing 19 characters
# for this test case.
try_output 11 "0000000000000000000" << EOF
int main() {
    char buffer[20];
    for (int i = 0; i < 19; i++)
        buffer[i] = '0';
    buffer[19] = 0;
    int written = snprintf(buffer, 0, "Number: %d", -37);
    printf("%s", buffer);
    return written;
}
EOF

# In this case, snprintf() only writes at most 10 bytes (including '\0'),
# but the return value is 11, which corresponds to the length of
# "Number: -37".
try_output 11 "Number: -" << EOF
int main() {
    char buffer[10];
    for (int i = 0; i < 9; i++)
        buffer[i] = '0';
    buffer[9] = 0;
    int written = snprintf(buffer, 10, "Number: %d", -37);
    printf("%s", buffer);
    return written;
}
EOF

try_output 14 " 4e 75 6d 62 65 72 3a 20 2d 0 30 30 30 30 30 30 30 30 30 0" << EOF
int main()
{
    char buffer[20];
    for (int i = 0; i < 19; i++)
        buffer[i] = '0';
    buffer[19] = 0;

    int written = snprintf(buffer, 10, "Number: %06d", -35337);

    for (int i = 0; i < 20; i++)
        printf(" %x", buffer[i]);
    return written;
}
EOF

# A complex test case for snprintf().
ans="written = 24
buffer  = buf - 00000
written = 13
buffer  = aaaa - 0
written = 19
buffer  = aaaa - 000000777777
written = 14
buffer  = aaaa - 000000777777
 61 61 61 61 20 2d 20 30 30 30 30 30 30 37 37 37 37 37 37 0 30 30 30 30 30 30 30 30 30 0"
try_output 0 "$ans" << EOF
int main()
{
    char buffer[30];
    for (int i = 0; i < 29; i++)
        buffer[i] = '0';
    buffer[29] = 0;

    int written = snprintf(buffer, 12, "%s - %018d", "buf", 35133127);
    printf("written = %d\nbuffer  = %s\n", written, buffer);
    written = snprintf(buffer, 9, "%s - %#06x", "aaaa", 0xFF);
    printf("written = %d\nbuffer  = %s\n", written, buffer);
    written = snprintf(buffer, 30, "%s - %#012o", "aaaa", 0777777);
    printf("written = %d\nbuffer  = %s\n", written, buffer);
    written = snprintf(buffer, 0, "%s - %#05x", "bbbbb", 0xAAFF);
    printf("written = %d\nbuffer  = %s\n", written, buffer);

    for (int i = 0; i < 30; i++)
        printf(" %x", buffer[i]);
    printf("\n");
    return 0;
}
EOF

# test the return value when calling fputc().
#
# Since the FILE data type is defined as an int in
# the built-in C library, and most of the functions
# such as fputc(), fgetc(), fclose() and fgets() directly
# treat the "stream" parameter (of type FILE *) as a file
# descriptor for performing input/output operations, the
# following test cases define "stdout" as 1, which is the
# file descriptor for the standard output.
try_output 0 "awritten = a" << EOF
#define stdout 1
int main()
{
	int c = fputc('a', stdout);
	printf("written = %c", c);
	return 0;
}
EOF

try_output 1 "" << EOF
#define stdout 1
int main()
{
	__syscall(__syscall_close, 1);
	int c = fputc('a', stdout);
	return c == -1;
}
EOF

# tests integer type conversion
# excerpted and modified from issue #166
try_output 0 "a = -127, b = -78, c = -93, d = -44" << EOF
int main()
{
    char a = 0x11, b = 0x22, c = 0x33, d = 0x44;
    a += 6000;
    b += 400;
    c -= 400;
    d -= 6000;
    printf("a = %d, b = %d, c = %d, d = %d\n", a, b, c, d);
    return 0;
}
EOF

try_output 0 "-1 -1" << EOF
int main()
{
    char a = 0xFF;
    int b = a;
    printf("%d %d\n", a, b);
    return 0;
}
EOF

# Test memset()
ans=" 7d 7d 7d 7d 7d 7d 7d 7d 7d 7d 7d 00 00 00 00 00
 00 00 00 00 00 00 7d 7d 7d 7d 7d 00 00 00 00 00
 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a 3a"
try_output 0 "$ans" << EOF
void print_array(char *ptr, int sz)
{
    for (int i = 0; i < sz; i++)
        printf(" %02x", ptr[i]);
    printf("\n");
}

int main(void)
{
    int sz = sizeof(char) * 16;
    char *ptr = malloc(sz);

    if (ptr != memset(ptr, 0x7D, sizeof(char) * 11))
        exit(1);
    print_array(ptr, sz);
    if (ptr != memset(ptr, 0, sizeof(char) * 6))
        exit(1);
    print_array(ptr, sz);
    if (ptr != memset(ptr, 0x3A, sz))
        exit(1);
    print_array(ptr, sz);

    free(ptr);
    return 0;
}
EOF

try_output 0 "2748 6719 105884 0" << EOF
int main()
{
    int a = 0XABC;
    int b = 0X1a3f;
    int c = 0XDEaD + 0xBeEF;
    int d = 0X0;
    printf("%d %d %d %d", a, b, c, d);
    return 0;
}
EOF

try_compile_error << EOF
int main()
{
    int x = 0X;
    return 0;
}
EOF

try_compile_error << EOF
int main()
{
    int x = 0XGHI;
    return 0;
}
EOF

# Binary literal tests (0b/0B prefix)
# Test basic binary literals
expr 0 "0b0"
expr 1 "0b1"
expr 2 "0b10"
expr 3 "0b11"
expr 4 "0b100"
expr 8 "0b1000"
expr 15 "0b1111"
expr 16 "0b10000"
expr 255 "0b11111111"

# Test uppercase B prefix
expr 10 "0B1010"
expr 240 "0B11110000"

# Test binary literals in arithmetic expressions
expr 15 "0b1100 | 0b0011"
expr 0 "0b1100 & 0b0011"
expr 15 "0b1100 ^ 0b0011"
expr 24 "0b110 << 2"
expr 3 "0b1100 >> 2"

# Test binary literals in variables
items 10 "int a = 0b1010; return a;"
items 255 "int b = 0B11111111; return b;"
items 45 "int x = 0b101101; return x;"

# Test binary literals in complex expressions
items 54 "int a = 0b1111; int b = 0b0011; return (a + b) * 3;"
items 160 "int mask = 0b11110000; int value = 0b10101010; return value & mask;"

# Test combination of different number bases
expr 45 "0b1111 + 0xF + 017"  # 15 + 15 + 15 = 45
expr 90 "0b110000 + 0x10 + 032"  # 48 + 16 + 26 = 90

# Test binary literals in comparisons
expr 1 "0b1010 == 10"
expr 1 "0b11111111 == 255"
expr 0 "0b1000 != 8"
expr 1 "0b10000 > 0xF"
expr 1 "0B1111 < 020"  # 15 < 16 (octal)

# Test binary literals with large values
try_large 1023 << EOF
int test_function() {
    return 0b1111111111;  /* 10 bits set = 1023 */
}
EOF

try_large 65535 << EOF
int test_function() {
    return 0b1111111111111111;  /* 16 bits set = 65535 */
}
EOF

# Test invalid binary literal errors
try_compile_error << EOF
int main()
{
    int x = 0b;  /* No binary digits */
    return 0;
}
EOF

try_compile_error << EOF
int main()
{
    int x = 0b2;  /* Invalid binary digit */
    return 0;
}
EOF

try_compile_error << EOF
int main()
{
    int x = 0B9;  /* Invalid binary digit */
    return 0;
}
EOF

# New escape sequence tests (\a, \b, \v, \f)
# Test character literals with new escape sequences
try_ 7 << EOF
int main() {
    char bell = '\a';  /* ASCII 7 - bell/alert */
    return bell;
}
EOF

try_ 8 << EOF
int main() {
    char backspace = '\b';  /* ASCII 8 - backspace */
    return backspace;
}
EOF

try_ 11 << EOF
int main() {
    char vtab = '\v';  /* ASCII 11 - vertical tab */
    return vtab;
}
EOF

try_ 12 << EOF
int main() {
    char formfeed = '\f';  /* ASCII 12 - form feed */
    return formfeed;
}
EOF

# Test all escape sequences together
try_output 0 "7 8 11 12" << EOF
int main() {
    printf("%d %d %d %d", '\a', '\b', '\v', '\f');
    return 0;
}
EOF

# Test escape sequences in strings
try_ 65 << EOF
int main() {
    char *str = "A\a\b\v\f";
    return str[0];  /* Should return 'A' = 65 */
}
EOF

try_ 7 << EOF
int main() {
    char *str = "A\a\b\v\f";
    return str[1];  /* Should return '\a' = 7 */
}
EOF

try_ 8 << EOF
int main() {
    char *str = "A\a\b\v\f";
    return str[2];  /* Should return '\b' = 8 */
}
EOF

# Test that existing escape sequences still work
try_output 0 "10 9 13 0" << EOF
int main() {
    printf("%d %d %d %d", '\n', '\t', '\r', '\0');
    return 0;
}
EOF

# Test additional escape sequences (\?, \e, unknown escapes)
try_ 63 << EOF
int main() {
    return '\?';  /* Should return 63 (ASCII '?') */
}
EOF

try_ 27 << EOF
int main() {
    return '\e';  /* GNU extension: ESC character (ASCII 27) */
}
EOF

try_ 122 << EOF
int main() {
    return '\z';  /* Unknown escape should return 'z' (ASCII 122) */
}
EOF

# Test hexadecimal escape sequences
try_ 65 << EOF
int main() {
    return '\x41';  /* Should return 65 (ASCII 'A') */
}
EOF

try_ 72 << EOF
int main() {
    return '\x48';  /* Should return 72 (ASCII 'H') */
}
EOF

# Test octal escape sequences
try_ 65 << EOF
int main() {
    return '\101';  /* Should return 65 (octal 101 = ASCII 'A') */
}
EOF

try_ 10 << EOF
int main() {
    return '\12';   /* Should return 10 (octal 12 = newline) */
}
EOF

try_ 8 << EOF
int main() {
    return '\10';   /* Should return 8 (octal 10 = backspace) */
}
EOF

# Test hex escapes in strings
try_output 0 "Hello World" << EOF
int main() {
    char *s = "\x48\x65\x6C\x6C\x6F \x57\x6F\x72\x6C\x64";
    printf("%s", s);
    return 0;
}
EOF

# Test octal escapes in strings
try_output 0 "ABC" << EOF
int main() {
    char *s = "\101\102\103";
    printf("%s", s);
    return 0;
}
EOF

# Test escape sequences in printf
# Note: The bell character (\a) is non-printable but present in output
try_output 0 "$(printf 'Bell: \a Tab:\t Newline:\n')" << EOF
int main() {
    printf("Bell: %c Tab:%c Newline:%c", '\a', '\t', '\n');
    return 0;
}
EOF

# Test adjacent string literal concatenation
try_output 0 "Hello World" << EOF
int main() {
    char *s = "Hello " "World";
    printf("%s", s);
    return 0;
}
EOF

try_output 0 "Testing string concatenation" << EOF
int main() {
    char *s = "Testing " "string " "concatenation";
    printf("%s", s);
    return 0;
}
EOF

try_output 0 "Multiple adjacent strings work!" << EOF
int main() {
    char *s = "Multiple " "adjacent " "strings " "work!";
    printf("%s", s);
    return 0;
}
EOF

# va_list and variadic function tests
# Note: These tests demonstrate both direct pointer arithmetic and
# va_list typedef forwarding between functions, now fully supported.

# Test 1: Sum calculation using variadic arguments
try_output 0 "Sum: 15" << EOF
int calculate_sum(int count, ...)
{
    int sum = 0;
    int i;
    int *p;

    p = &count;
    p++;

    for (i = 0; i < count; i++)
        sum += p[i];

    return sum;
}

int main()
{
    int result = calculate_sum(5, 1, 2, 3, 4, 5);
    printf("Sum: %d", result);
    return 0;
}
EOF

# Test 2: Multiple integer arguments
try_output 0 "Multi: 10 20 255" << EOF
void multi_arg_test(int first, ...)
{
    int *p;
    int val1, val2;

    /* Point to variadic arguments */
    p = &first;
    p++;

    /* Get integer values */
    val1 = p[0];
    val2 = p[1];

    printf("Multi: %d %d %d", first, val1, val2);
}

int main()
{
    multi_arg_test(10, 20, 255);
    return 0;
}
EOF

# Test 3: Variable argument count with different values
try_output 0 "Args: 1=100 2=200 3=300" << EOF
void print_args(int count, ...)
{
    int *p = &count;
    int i;

    p++;
    printf("Args:");
    for (i = 0; i < count; i++)
        printf(" %d=%d", i + 1, p[i]);
}

int main()
{
    print_args(3, 100, 200, 300);
    return 0;
}
EOF

# Test 4: Mixed argument types (integers with different sizes)
try_output 0 "Values: 42 -17 0 999" << EOF
void mixed_args(int first, ...)
{
    int *p = &first;

    printf("Values: %d", first);
    printf(" %d", *(++p));
    printf(" %d", *(++p));
    printf(" %d", *(++p));
}

int main()
{
    mixed_args(42, -17, 0, 999);
    return 0;
}
EOF

# Test 5: Minimum and maximum finder
try_output 0 "Min: 5, Max: 50" << EOF
void find_min_max(int count, ...)
{
    int *p = &count;
    int i, min, max;

    p++;
    min = p[0];
    max = p[0];

    for (i = 1; i < count; i++) {
        if (p[i] < min) min = p[i];
        if (p[i] > max) max = p[i];
    }

    printf("Min: %d, Max: %d", min, max);
}

int main()
{
    find_min_max(4, 10, 50, 5, 25);
    return 0;
}
EOF

# Test 6: Simple printf-like function
try_output 0 "ERROR: Failed with code 42" << EOF
void error_log(int code, ...)
{
    printf("ERROR: Failed with code %d", code);
}

int main()
{
    error_log(42);
    return 0;
}
EOF

# Test 7: Function with single variadic argument
try_output 0 "Single extra: 123" << EOF
void single_extra(int base, ...)
{
    int *p = &base;
    p++;
    printf("Single extra: %d", *p);
}

int main()
{
    single_extra(0, 123);
    return 0;
}
EOF

# Test 8: Zero additional arguments
try_output 0 "Only required: 77" << EOF
void only_required(int value, ...)
{
    printf("Only required: %d", value);
}

int main()
{
    only_required(77);
    return 0;
}
EOF

# Test 9: Arithmetic operations on variadic arguments
try_output 0 "Result: 25" << EOF
int arithmetic_va(int count, ...)
{
    int *p = &count;
    int result = 0;
    int i;

    p++;
    for (i = 0; i < count; i++) {
        if (i % 2 == 0)
            result += p[i];
        else
            result -= p[i];
    }
    return result;
}

int main()
{
    int res = arithmetic_va(4, 20, 5, 15, 5);  /* 20 - 5 + 15 - 5 = 25 */
    printf("Result: %d", res);
    return 0;
}
EOF

# Test 10: Simple working variadic function test
try_output 0 "Variadic: 60" << EOF
int sum_three(int a, ...)
{
    int *p = &a;
    int v1 = p[0];
    int v2 = p[1];
    int v3 = p[2];
    return v1 + v2 + v3;
}

int main()
{
    printf("Variadic: %d", sum_three(10, 20, 30));
    return 0;
}
EOF

# va_list typedef forwarding tests
# These tests demonstrate va_list typedef forwarding between functions

# Test 11: Basic va_list typedef forwarding
try_output 0 "Test: 42" << EOF
typedef int *va_list;

void print_with_va_list(va_list args)
{
    printf("Test: %d", args[0]);
}

int main()
{
    int values[3];
    values[0] = 42;
    values[1] = 100;
    values[2] = 200;
    va_list myargs = values;
    print_with_va_list(myargs);
    return 0;
}
EOF

# Test 12: va_list array indexing
try_output 0 "args[0] = 42" << EOF
typedef int *va_list;

int main()
{
    int x = 42;
    va_list args = &x;
    printf("args[0] = %d", args[0]);
    return 0;
}
EOF

# Test 13: Built-in va_list usage (from lib/c.c)
try_output 0 "Built-in: 777" << EOF
int main()
{
    int test_val = 777;
    va_list args = &test_val;
    printf("Built-in: %d", args[0]);
    return 0;
}
EOF

# typedef pointer tests
try_ 42 << EOF
typedef int *int_ptr;

int main(void)
{
    int x = 42;
    int_ptr p = &x;
    return *p;
}
EOF

try_output 0 "Hello" << EOF
typedef char *string;

int main(void)
{
    char buf[] = "Hello";
    string str = buf;
    printf("%s", str);
    return 0;
}
EOF

try_output 0 "Pointer arithmetic: 10 20 30" << EOF
typedef int *int_ptr;

int main(void)
{
    int a = 10, b = 20, c = 30;
    int_ptr ptr = &a;

    printf("Pointer arithmetic:");
    printf(" %d", *ptr);
    ptr = &b;
    printf(" %d", *ptr);
    ptr = &c;
    printf(" %d", *ptr);
    return 0;
}
EOF

try_output 0 "Value: 42" << EOF
typedef int *int_ptr;

int main(void)
{
    int value = 42;
    int_ptr iptr = &value;
    printf("Value: %d", *iptr);
    return 0;
}
EOF

# Complex pointer arithmetic tests
# Testing enhanced parser capability to handle expressions like *(ptr + offset)

# Test 1: Basic pointer arithmetic on RHS
try_output 0 "Values: 10 20 30" << EOF
int main()
{
    int arr[3];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    int *ptr = arr;
    printf("Values: %d %d %d", *(ptr + 0), *(ptr + 1), *(ptr + 2));
    return 0;
}
EOF

# Test 2: Complex pointer arithmetic with variables on RHS
try_output 0 "Complex: 25 35 45" << EOF
int main()
{
    int data[5];
    data[0] = 5;
    data[1] = 15;
    data[2] = 25;
    data[3] = 35;
    data[4] = 45;
    int *p = data;
    int offset = 2;
    printf("Complex: %d %d %d", *(p + offset), *(p + offset + 1), *(p + (offset + 2)));
    return 0;
}
EOF

# Test 3: Pointer arithmetic with negative offsets on RHS
try_output 0 "Negative: 30 20 10" << EOF
int main()
{
    int values[3];
    values[0] = 10;
    values[1] = 20;
    values[2] = 30;
    int *ptr = &values[2];  /* Point to last element */
    printf("Negative: %d %d %d", ptr[0], ptr[-1], ptr[-2]);
    return 0;
}
EOF

# Test 4: Multiple levels of pointer arithmetic on RHS
try_output 0 "Multi: 100 200 300" << EOF
int main()
{
    int matrix[3];
    matrix[0] = 100;
    matrix[1] = 200;
    matrix[2] = 300;
    int *base = matrix;
    int i = 1, j = 2;
    printf("Multi: %d %d %d", *(base + 0), *(base + i), *(base + j));
    return 0;
}
EOF

# Test 5: Complex expressions in pointer arithmetic on RHS
try_output 0 "Expr: 42 84 126" << EOF
int main()
{
    int nums[6];
    nums[0] = 0;
    nums[1] = 42;
    nums[2] = 84;
    nums[3] = 126;
    nums[4] = 168;
    nums[5] = 210;
    int *p = nums;
    int step = 1;
    printf("Expr: %d %d %d", *(p + 1), *(p + 2), *(p + 3));
    return 0;
}
EOF

# Test 6: Pointer arithmetic on LHS for assignment
try_ 42 << EOF
int main()
{
    int arr[3];
    arr[0] = 0;
    arr[1] = 0;
    arr[2] = 0;
    int *ptr = arr;
    ptr[0] = 10;
    ptr[1] = 20;
    ptr[2] = 12;
    return ptr[0] + ptr[1] + ptr[2];
}
EOF

# Test 7: Complex LHS assignment with variables
try_output 0 "LHS: 5 15 25" << EOF
int main()
{
    int data[3];
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    int *p = data;
    int offset = 1;
    p[0] = 5;
    p[offset] = 15;
    p[offset + 1] = 25;
    printf("LHS: %d %d %d", data[0], data[1], data[2]);
    return 0;
}
EOF

# Test 8: LHS assignment with negative offsets
try_output 0 "Reverse: 10 20 30" << EOF
int main()
{
    int vals[3];
    vals[0] = 0;
    vals[1] = 0;
    vals[2] = 0;
    int *ptr = &vals[2];  /* Point to last element */
    ptr[-2] = 10;
    ptr[-1] = 20;
    ptr[0] = 30;
    printf("Reverse: %d %d %d", vals[0], vals[1], vals[2]);
    return 0;
}
EOF

# Test 9: Multi-level pointer dereference with arithmetic
try_ 9 << EOF
int main()
{
    int value = 777;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    int ***ptr3 = &ptr2;
    return ***(ptr3 + 0);
}
EOF

# Test 10: Complex multi-level pointer arithmetic
try_output 0 "Complex multi: 100 200" << EOF
int main()
{
    int arr[2];
    arr[0] = 100;
    arr[1] = 200;
    int *ptrs[2];
    ptrs[0] = &arr[0];
    ptrs[1] = &arr[1];
    int **pptr = ptrs;
    printf("Complex multi: %d %d", **(pptr + 0), **(pptr + 1));
    return 0;
}
EOF

# Test 11: Mixed pointer arithmetic and array indexing
try_output 0 "Mixed: 11 22 33" << EOF
int main()
{
    int matrix[3];
    matrix[0] = 11;
    matrix[1] = 22;
    matrix[2] = 33;
    int *p = matrix;
    printf("Mixed: %d %d %d", p[0], *(p + 1), matrix[2]);
    return 0;
}
EOF

# Test 12: Pointer arithmetic in function calls
try_output 0 "Function: 45" << EOF
int get_value(int *ptr, int offset)
{
    return *(ptr + offset);
}

int main()
{
    int data[3];
    data[0] = 15;
    data[1] = 30;
    data[2] = 45;
    printf("Function: %d", get_value(data, 2));
    return 0;
}
EOF

# Test 13: Complex pointer arithmetic with structure members
try_output 0 "Struct: 10 20" << EOF
typedef struct {
    int x;
    int y;
} point_t;

int main()
{
    point_t points[2];
    points[0].x = 10;
    points[0].y = 20;
    points[1].x = 30;
    points[1].y = 40;
    point_t *p = points;
    printf("Struct: %d %d", p->x, p->y);
    return 0;
}
EOF

# Test 14: Arithmetic with pointer dereferencing in expressions
try_output 0 "Arithmetic: 35" << EOF
int main()
{
    int nums[3];
    nums[0] = 10;
    nums[1] = 15;
    nums[2] = 20;
    int *p = nums;
    int result = *(p + 0) + *(p + 1) + *(p + 2) - 10;
    printf("Arithmetic: %d", result);
    return 0;
}
EOF

# Test 15: Complex LHS with compound assignment operators
try_output 0 "Compound: 15 25 35" << EOF
int main()
{
    int arr[3];
    arr[0] = 10;
    arr[1] = 20;
    arr[2] = 30;
    int *ptr = arr;
    ptr[0] += 5;
    ptr[1] += 5;
    ptr[2] += 5;
    printf("Compound: %d %d %d", arr[0], arr[1], arr[2]);
    return 0;
}
EOF

# Test 16: Pointer arithmetic with character arrays
try_output 0 "Chars: ABC" << EOF
int main()
{
    char str[4];
    str[0] = 'A';
    str[1] = 'B';
    str[2] = 'C';
    str[3] = '\0';
    char *p = str;
    printf("Chars: %c%c%c", *(p + 0), *(p + 1), *(p + 2));
    return 0;
}
EOF

# Test 17: Complex nested pointer arithmetic
try_output 0 "Nested: 42" << EOF
int main()
{
    int data[5];
    data[0] = 0;
    data[1] = 10;
    data[2] = 20;
    data[3] = 42;
    data[4] = 50;
    int *base = data;
    int offset1 = 2, offset2 = 1;
    printf("Nested: %d", *(base + offset1 + offset2));
    return 0;
}
EOF

# Test 18: Pointer arithmetic with conditional expressions
try_output 0 "Conditional: 100" << EOF
int main()
{
    int vals[2];
    vals[0] = 50;
    vals[1] = 100;
    int *p = vals;
    int flag = 1;
    printf("Conditional: %d", *(p + (flag ? 1 : 0)));
    return 0;
}
EOF

# Test 19: Complex triple dereference with arithmetic
try_output 0 "Triple deref: 777" << EOF
int main()
{
    int value = 777;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    int ***ptr3 = &ptr2;
    printf("Triple deref: %d", ***(ptr3 + 0));
    return 0;
}
EOF

# Test 20: Complex double dereference with arithmetic
try_output 0 "Double deref: 888" << EOF
int main()
{
    int value = 888;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    printf("Double deref: %d", **(ptr2 + 0));
    return 0;
}
EOF

# Test 21: Complex nested parentheses with multiple dereference
try_output 0 "Nested parens: 999" << EOF
int main()
{
    int value = 999;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    int ***ptr3 = &ptr2;
    printf("Nested parens: %d", ***((ptr3 + 0)));
    return 0;
}
EOF

# Test 22: Variable offset in complex dereference
try_output 0 "Variable offset: 555" << EOF
int main()
{
    int value = 555;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    int ***ptr3 = &ptr2;
    int offset = 0;
    printf("Variable offset: %d", ***(ptr3 + offset));
    return 0;
}
EOF

# Test 23: Array of pointers with complex dereference
try_output 0 "Array ptr: 111 222 333" << EOF
int main()
{
    int a = 111, b = 222, c = 333;
    int *arr[3];
    arr[0] = &a;
    arr[1] = &b;
    arr[2] = &c;
    int **parr = arr;
    printf("Array ptr: %d %d %d", **(parr + 0), **(parr + 1), **(parr + 2));
    return 0;
}
EOF

# Test 24: Mixed single and multiple dereference
try_output 0 "Mixed: 666 666 666" << EOF
int main()
{
    int value = 666;
    int *ptr1 = &value;
    int **ptr2 = &ptr1;
    printf("Mixed: %d %d %d", *ptr1, **ptr2, **(ptr2 + 0));
    return 0;
}
EOF

# Test compound literals: basic int/char and arrays
try_ 42 << EOF
int main() {
    /* Basic int compound literal */
    return (int){42};
}
EOF

try_ 65 << EOF
int main() {
    /* Basic char compound literal */
    return (char){65};
}
EOF

try_ 25 << EOF
int main() {
    /* Single element array compound literal */
    return (int[]){25};
}
EOF

try_ 10 << EOF
int main() {
    /* Multi-element array compound literal - returns first element */
    return (int[]){10, 20, 30};
}
EOF

try_ 100 << EOF
int main() {
    /* Array compound literal assignment */
    int x = (int[]){100, 200, 300};
    return x;
}
EOF

try_ 50 << EOF
int main() {
    /* Char array compound literal */
    return (char[]){50, 60, 70};
}
EOF

# Test compound literals: advanced features
try_ 35 << EOF
int main() {
    /* Compound literals in arithmetic expressions */
    return (int){10} + (int){20} + (int[]){5, 15, 25};
}
EOF

try_ 42 << EOF
int add_values(int a, int b) { return a + b; }
int main() {
    /* Compound literals as function arguments */
    return add_values((int){30}, (int){12});
}
EOF

try_ 75 << EOF
int main() {
    /* Multiple array compound literals */
    int a = (int[]){25, 35, 45};
    int b = (int[]){50, 60, 70};
    return a + b; /* 25 + 50 = 75 */
}
EOF

try_ 120 << EOF
int main() {
    /* Complex expression with mixed compound literals */
    return (int){40} + (char){80} + (int[]){0, 0, 0};  /* 40 + 80 + 0 = 120 */
}
EOF

try_ 200 << EOF
int main() {
    /* Compound literal with larger numbers */
    return (int[]){200, 300, 400};
}
EOF

# Test compound literals: edge cases
try_ 0 << EOF
int main() {
    /* Empty compound literal */
    return (int){};
}
EOF

try_ 0 << EOF
int main() {
    /* Empty array compound literal */
    return (int[]){};
}
EOF

try_ 90 << EOF
int main() {
    /* Multiple compound literals in expression */
    return (int[]){30, 60} + (int[]){60, 30};  /* 30 + 60 = 90 */
}
EOF

try_ 255 << EOF
int main() {
    /* Large char compound literal */
    return (char){255};
}
EOF

try_ 150 << EOF
int main() {
    /* Mixed compound literal expressions */
    int a = (int){50};
    int b = (int[]){100, 200, 300};
    return a + b;  /* 50 + 100 = 150 */
}
EOF

# Test pointer compound literals
try_ 0 << EOF
int main()
{
    /* Test NULL pointer compound literal */
    int *p = (int*){};
    return p ? 1 : 0;
}
EOF

try_ 0 << EOF
int main()
{
    /* Test pointer compound literal with zero */
    int *p = (int*){0};
    return p ? 1 : 0;
}
EOF

# Test char pointer compound literals
try_ 0 << EOF
int main()
{
    char *p = (char*){};
    return p ? 1 : 0;
}
EOF

# Test typedef pointer compound literals
try_ 0 << EOF
typedef int* IntPtr;

int main()
{
    IntPtr p = (IntPtr){0};
    return p ? 1 : 0;
}
EOF

# Additional struct initialization tests from refine-parser
# Test: Local struct initialization (working with field-by-field assignment)
try_ 42 << EOF
typedef struct {
    int x;
    int y;
} point_t;

int main() {
    point_t p;
    p.x = 10;
    p.y = 32;
    return p.x + p.y;  /* Returns 42 */
}
EOF

# Test: Simple array initialization
try_ 15 << EOF
int main() {
    int nums[3];
    nums[0] = 1;
    nums[1] = 5;
    nums[2] = 9;
    return nums[0] + nums[1] + nums[2];  /* Returns 15 */
}
EOF

# Test: Character array with integer values
try_ 24 << EOF
int main() {
    char arr[3];
    arr[0] = 5;
    arr[1] = 9;
    arr[2] = 10;
    return arr[0] + arr[1] + arr[2];  /* Returns 24 (5+9+10) */
}
EOF

# Test: Simple 3-element array
try_ 6 << EOF
int main() {
    int arr[3];
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    return arr[0] + arr[1] + arr[2];  /* Returns 6 (1+2+3) */
}
EOF

# Test: Mixed scalar fields in struct
try_ 42 << EOF
typedef struct {
    int scalar;
    int x, y;
} mixed_t;

int main() {
    mixed_t m;
    m.scalar = 0;
    m.x = 10;
    m.y = 32;
    return m.x + m.y;  /* Returns 42 */
}
EOF

# Union support tests
# Basic union declaration and field access
try_ 42 << EOF
typedef union {
    int i;
    char c;
} basic_union_t;

int main() {
    basic_union_t u;
    u.i = 42;
    return u.i;  /* Returns 42 */
}
EOF

# Union field access - different types sharing same memory
try_ 65 << EOF
typedef union {
    int i;
    char c;
} char_int_union_t;

int main() {
    char_int_union_t u;
    u.c = 65;  /* ASCII 'A' */
    return u.c;  /* Returns 65 */
}
EOF

# Union with multiple integer fields
try_ 100 << EOF
typedef union {
    int value;
    int number;
    int data;
} multi_int_union_t;

int main() {
    multi_int_union_t u;
    u.value = 100;
    return u.number;  /* Returns 100 - same memory location */
}
EOF

# Union size calculation - should be size of largest member
try_ 4 << EOF
typedef union {
    int i;      /* 4 bytes */
    char c;     /* 1 byte */
} size_union_t;

int main() {
    return sizeof(size_union_t);  /* Returns 4 (size of int) */
}
EOF

# Union with different data types
try_output 0 "Value as int: 1094795585, as char: 65" << EOF
typedef union {
    int i;
    char c;
} data_union_t;

int main() {
    data_union_t u;
    u.i = 1094795585;  /* 0x41414141 in hex - four 'A' characters */
    printf("Value as int: %d, as char: %d", u.i, u.c);
    return 0;
}
EOF

# Nested union in struct
try_ 50 << EOF
typedef union {
    int value;
    char byte;
} nested_union_t;

typedef struct {
    int id;
    nested_union_t data;
} container_t;

int main() {
    container_t c;
    c.id = 10;
    c.data.value = 40;
    return c.id + c.data.value;  /* Returns 50 */
}
EOF

# Array of unions
try_ 30 << EOF
typedef union {
    int i;
    char c;
} array_union_t;

int main() {
    array_union_t arr[3];
    arr[0].i = 10;
    arr[1].i = 20;
    arr[2].i = 0;  /* Will be overridden */
    arr[2].c = 0;  /* Sets to 0 */
    return arr[0].i + arr[1].i + arr[2].i;  /* Returns 30 */
}
EOF

# Union with pointer fields
try_ 42 << EOF
typedef union {
    int *int_ptr;
    char *char_ptr;
} ptr_union_t;

int main() {
    int value = 42;
    ptr_union_t u;
    u.int_ptr = &value;
    return *(u.int_ptr);  /* Returns 42 */
}
EOF

# Complex union with struct member
try_ 77 << EOF
typedef struct {
    int x;
    int y;
} point_t;

typedef union {
    point_t pt;
    int values[2];
} point_union_t;

int main() {
    point_union_t u;
    u.pt.x = 30;
    u.pt.y = 47;
    return u.values[0] + u.values[1];  /* Returns 77 (30+47) */
}
EOF

# Union assignment and memory sharing (endianness-neutral)
try_output 0 "Union works: 100" << EOF
typedef union {
    int i;
    char bytes[4];
} byte_union_t;

int main() {
    byte_union_t u;
    u.i = 100;
    printf("Union works: %d", u.i);
    return 0;
}
EOF

# Union with typedef pointer
try_ 99 << EOF
typedef int *int_ptr_t;

typedef union {
    int_ptr_t ptr;
    int direct;
} typedef_ptr_union_t;

int main() {
    int value = 99;
    typedef_ptr_union_t u;
    u.ptr = &value;
    return *(u.ptr);  /* Returns 99 */
}
EOF

# Union initialization with different members
try_ 25 << EOF
typedef union {
    int integer;
    char character;
} init_union_t;

int main() {
    init_union_t u1, u2;
    u1.integer = 25;
    u2.character = 25;
    return u1.integer;  /* Returns 25 */
}
EOF

# Union with function pointers
try_ 15 << EOF
int add_func(int a, int b) { return a + b; }
int mult_func(int a, int b) { return a * b; }

typedef union {
    int (*add_ptr)(int, int);
    int (*mult_ptr)(int, int);
} func_union_t;

int main() {
    func_union_t u;
    u.add_ptr = add_func;
    return u.add_ptr(7, 8);  /* Returns 15 */
}
EOF

# Sizeof union with mixed types
try_ 4 << EOF
typedef union {
    char c;
    int i;
    char *p;
} mixed_union_t;

int main() {
    return sizeof(mixed_union_t);  /* Returns 4 (size of largest member) */
}
EOF

# Union field modification
try_ 200 << EOF
typedef union {
    int total;
    int sum;
} modify_union_t;

int main() {
    modify_union_t u;
    u.total = 100;
    u.sum += 100;  /* Modifies same memory location */
    return u.total;  /* Returns 200 */
}
EOF

# Named union inside struct
try_ 88 << EOF
typedef union {
    int value;
    char byte;
} inner_union_t;

typedef struct {
    int id;
    inner_union_t data;
} named_union_container_t;

int main() {
    named_union_container_t c;
    c.id = 8;
    c.data.value = 80;
    return c.id + c.data.value;  /* Returns 88 */
}
EOF

# Union with array members
try_ 15 << EOF
typedef union {
    int array[3];
    char bytes[12];
} array_union_t;

int main() {
    array_union_t u;
    u.array[0] = 5;
    u.array[1] = 10;
    u.array[2] = 0;
    return u.array[0] + u.array[1] + u.array[2];  /* Returns 15 */
}
EOF

# Complex union with nested structures
try_ 33 << EOF
typedef struct {
    int a;
    int b;
} pair_t;

typedef union {
    pair_t pair;
    int values[2];
    char bytes[8];
} complex_union_t;

int main() {
    complex_union_t u;
    u.pair.a = 11;
    u.pair.b = 22;
    return u.values[0] + u.values[1];  /* Returns 33 */
}
EOF

# Union as function parameter
try_ 60 << EOF
typedef union {
    int i;
    char c;
} param_union_t;

int process_union(param_union_t u) {
    return u.i;
}

int main() {
    param_union_t u;
    u.i = 60;
    return process_union(u);  /* Returns 60 */
}
EOF

# Union as return type
try_ 45 << EOF
typedef union {
    int value;
    char byte;
} return_union_t;

return_union_t create_union(int val) {
    return_union_t u;
    u.value = val;
    return u;
}

int main() {
    return_union_t result = create_union(45);
    return result.value;  /* Returns 45 */
}
EOF

# Multiple union declarations
try_ 120 << EOF
typedef union {
    int x;
    char c;
} union1_t;

typedef union {
    int y;
    char d;
} union2_t;

int main() {
    union1_t u1;
    union2_t u2;
    u1.x = 50;
    u2.y = 70;
    return u1.x + u2.y;  /* Returns 120 */
}
EOF

# Type Casting Tests
echo "Testing type casting functionality..."

# Basic int to char cast
try_ 65 << EOF
int main() {
    int x = 65;
    char c = (char)x;
    return c;  /* Returns 65 ('A') */
}
EOF

# Char to int cast
try_ 42 << EOF
int main() {
    char c = 42;
    int x = (int)c;
    return x;  /* Returns 42 */
}
EOF

# Cast in expressions
try_ 130 << EOF
int main() {
    int a = 65;
    int b = 65;
    return (char)a + (char)b;  /* Returns 130 */
}
EOF

# Cast with arithmetic
try_ 42 << EOF
int main() {
    int x = 50;
    return (int)((char)(x - 8));  /* Returns 42 */
}
EOF

# Multiple casts in sequence
try_ 100 << EOF
int main() {
    int x = 100;
    char c = (char)x;
    int y = (int)c;
    return y;  /* Returns 100 */
}
EOF

# Cast with function parameters
try_ 88 << EOF
int test_func(char c) {
    return (int)c;
}

int main() {
    int x = 88;
    return test_func((char)x);  /* Returns 88 */
}
EOF

# Cast in return statement
try_ 123 << EOF
int get_char() {
    int x = 123;
    return (char)x;
}

int main() {
    return get_char();  /* Returns 123 */
}
EOF

# Nested casts
try_ 200 << EOF
int main() {
    int x = 200;
    return (int)((char)((int)x));  /* Returns 200 */
}
EOF

# Cast with assignment
try_ 150 << EOF
int main() {
    int x = 150;
    char c;
    c = (char)x;
    return (int)c;  /* Returns 150 */
}
EOF

# String literal and escape coverage (additional)
try_output 0 "AZ" << 'EOF'
int main() {
    printf("%s", "\\x41Z"); /* hex escape then normal char */
    return 0;
}
EOF

try_output 0 "AZ" << 'EOF'
int main() {
    printf("%s", "A\\132"); /* octal escape for 'Z' */
    return 0;
}
EOF

# Cast zero value
try_ 0 << EOF
int main() {
    int x = 0;
    char c = (char)x;
    return (int)c;  /* Returns 0 */
}
EOF

# Local array initializers - verify compilation and correct values
# Test 1: Implicit size array with single element
try_ 1 << 'EOF'
int main() {
    int a[] = {1};
    return a[0];  /* Should return 1 */
}
EOF

# Test 2: Explicit size array with single element
try_ 42 << 'EOF'
int main() {
    int a[1] = {42};
    return a[0];  /* Should return 42 */
}
EOF

# Test 3: Multiple elements - verify all are initialized
try_ 6 << 'EOF'
int main() {
    int a[3] = {1, 2, 3};
    return a[0] + a[1] + a[2];  /* Should return 1+2+3=6 */
}
EOF

# Test 4: Character array initialization
try_ 97 << 'EOF'
int main() {
    char s[] = {'a', 'b', 'c'};
    return s[0];  /* Should return ASCII value of 'a' = 97 */
}
EOF

# Test 5: Empty initializer (all zeros)
try_ 0 << 'EOF'
int main() {
    int a[5] = {};
    return a[0] + a[1] + a[2] + a[3] + a[4];  /* Should return 0 */
}
EOF

# Test 6: Partial initialization (remaining should be zero)
try_ 15 << 'EOF'
int main() {
    int a[5] = {5, 10};
    return a[0] + a[1] + a[2] + a[3] + a[4];  /* Should return 5+10+0+0+0=15 */
}
EOF

# Test 7: Pass initialized array to function
try_ 30 << 'EOF'
int sum(int *p, int n) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total += p[i];
    return total;
}
int main() {
    int a[] = {5, 10, 15};
    return sum(a, 3);  /* Should return 5+10+15=30 */
}
EOF

# Test 8: Nested scope with array initialization
try_ 100 << 'EOF'
int main() {
    {
        int values[] = {25, 25, 25, 25};
        return values[0] + values[1] + values[2] + values[3];
    }
}
EOF

echo OK
