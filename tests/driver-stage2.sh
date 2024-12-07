#!/usr/bin/env bash

set -u

readonly SHECC="$PWD/out/shecc-stage2.elf"

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

echo OK
