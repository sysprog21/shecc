#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc"

function try() {
    local expected="$1"
    local input="$2"

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"

    echo "$input" > "$tmp_in"
    "$SHECC" -o "$tmp_exe" "$tmp_in"
    chmod +x $tmp_exe
    $ARM_EXEC "$tmp_exe"
    local actual="$?"

    if [ "$actual" = "$expected" ]; then
        echo "$input => $actual"
    else
        echo "$input => $expected expected, but got $actual"
        echo "input: $tmp_in"
        echo "executable: $tmp_exe"
        exit 1
    fi
}

function try_() {
    local expected="$1"
    local input="$(cat)"
    try "$expected" "$input"
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
# expr 210 "1 + (2 + (3 + (4 + (5 + (6 + (7 + (8 + (9 + (10 + (11 + (12 + (13 + (14 + (15 + (16 + (17 + (18 + (19 + 20))))))))))))))))))"

# expr 21 "+1+20"
# expr 10 "-15+(+35-10)"

# expr 2 "5 % 3"
# expr 6 "111 % 7"

expr 1 "10 > 5"
expr 1 "3+3 > 5"
expr 0 "30 == 20"
expr 0 "5 >= 10"
expr 1 "5 >= 5"
expr 1 "30 != 20"

# expr 0 "!237"
# expr 18 "~237"

expr 0 "0 || 0"
expr 1 "1 || 0"
expr 1 "1 || 1"
expr 0 "0 && 0"
expr 0 "1 && 0"
expr 1 "1 && 1"

expr 16 "2 << 3"
expr 32 "256 >> 3"
expr 239 "237 | 106"
# expr 135 "237 ^ 106"
expr 104 "237 & 106"

# return statements
# items 1 "return 1;";
# items 42 "return 2*21;";

# variables
items 10 "int var; var = 10; exit(var);"
items 42 "int va; int vb; va = 11; vb = 31; int vc; vc = va + vb; exit(vc);"
items 50 "int v; v = 30; v = 50; exit(v);"

# if
items 5 "if (1) exit(5); else exit(20);"
items 10 "if (0) exit(5); else if (0) exit(20); else exit(10);"
items 10 "int a; a = 0; int b; b = 0; if (a) b = 10; else if (0) exit(a); else if (a) exit(b); else exit(10);"
items 27 "int a; a = 15; int b; b = 2; if(a - 15) b = 10; else if (b) exit(a + b + 10); else if (a) exit(b); else exit(10);"

# compound
items 5 "{ exit(5); }"
items 10 "{ int a; a = 5; { a = 5 + a; } exit(a); }"
items 20 "int a; a = 10; if (1) { a = 20; } else { a = 10; } exit(a);"
items 30 "int a; a = 10; if (a) { if (a - 10) { a = a + 1; } else { a = a + 20; } a = a - 10; } else { a = a + 5; } exit(a + 10);"

# loop
items 55 "int acc; int p; acc = 0; p = 10; while (p) { acc = acc + p; p = p - 1; } exit(acc);"
items 60 "int acc; acc = 15; do { acc = acc * -2; } while (acc < 0); exit(acc);"
# items 45 "int i; int acc; acc = 0; for (i = 0; i < 10; ++i) { acc = acc + i; } exit(acc);"
# items 45 "int i; int j; i=0; j=0; while (i<10) { j=j+i; i=i+1; } exit(j);"
# items 1 "int x; x=0; do {x = x + 1; break;} while (1); exit(x);"
# items 1 "int x; x=0; do {x = x + 1; continue;} while (0); exit(x);"
# items 7 "int i; i=0; int j; for (j = 0; j < 10; j++) { if (j < 3) continue; i = i + 1; } exit(i);"
items 10 "while(0); exit(10);"
# items 10 "while(1) break; exit(10);"
# items 10 "for(;;) break; exit(10);"
items 0 "int x; for(x = 10; x > 0; x--); exit(x);"
# items 30 "int i; int acc; i = 0; acc = 0; do { i = i + 1; if (i - 1 < 5) continue; acc = acc + i; if (i == 9) break; } while (i < 10); exit(acc);"
# items 26 "int acc; acc = 0; int i; for (i = 0; i < 100; ++i) { if (i < 5) continue; if (i == 9) break; acc = acc + i; } exit(acc);"

# functions
try_ 55 << EOF
int sum(int m, int n) {
    int acc;
    acc = 0;
    int i;
    for (i = m; i <= n; i = i + 1)
        acc = acc + i;
    exit(acc);
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
    exit(fact(5));
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
    exit(is_even(20));
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
    exit(ack(3, 5));
}
EOF

# pointers
items 3 "int x; int *y; x = 3; y = &x; exit(y[0]);"
items 5 "int b; int *a; b = 10; a = &b; a[0] = 5; exit(b);"
try_ 10 << EOF
int change_it(int *p) {
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
  exit(v);
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
    exit(v0 + v1 + v2);
}
EOF

# global initialization
try_ 20 << EOF
int a = 5 * 2;
int b = -4 * 3 + 7 + 9 / 3 * 5;
int main()
{
    exit(a + b);
}
EOF

# conditional operator
# expr 10 "1 ? 10 : 5"
# expr 25 "0 ? 10 : 25"

# compound assignemnt
items 5 "int a; a = 2; a += 3; exit(a);"
items 5 "int a; a = 10; a -= 5; exit(a);"
items 20 "int *p; int a[3]; a[0] = 10; a[1] = 20; a[2] = 30; p = a; p+=1; exit(p[0]);"

# sizeof
expr 4 "sizeof(int)";
expr 1 "sizeof(char)";

# switch-case
items 10 "int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } exit(a);"
items 10 "int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } exit(a);"

# enum
try_ 6 << EOF
typedef enum { enum1 = 5, enum2 } enum_t;
int main() { enum_t v = enum2; exit(v); }
EOF

echo OK
