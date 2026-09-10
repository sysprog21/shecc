#!/usr/bin/env bash

set -u

# Configuration and Test Metrics

# Test Configuration
readonly VERBOSE_MODE="${VERBOSE:-0}"
readonly SHOW_SUMMARY="${SHOW_SUMMARY:-1}"
readonly SHOW_PROGRESS="${SHOW_PROGRESS:-1}"
readonly COLOR_OUTPUT="${COLOR_OUTPUT:-1}"
# Substring match against the category name; empty runs everything.
readonly TEST_FILTER="${TEST_FILTER:-}"

# 1 stops at the first failure. The default reports every failure and still
# exits non-zero at the end, so one bad case no longer hides the other 600.
readonly FAIL_FAST="${FAIL_FAST:-0}"

# Everything the run creates goes here, so it can be removed in one step -- the
# suite used to leave ~2400 files in /tmp per invocation. Kept on failure,
# because report_test_failure names the files it wants you to look at.
readonly TEST_TMPDIR="$(mktemp -d)"
export TMPDIR="$TEST_TMPDIR"
function cleanup()
{
    if [ "$FAILED_TESTS" -eq 0 ]; then
        rm -rf "$TEST_TMPDIR"
    else
        echo "Test files kept in $TEST_TMPDIR"
    fi
}
trap cleanup EXIT

# Set by begin_category; tests outside the selected categories return early.
CATEGORY_SELECTED=1

function test_selected()
{
    [ "$CATEGORY_SELECTED" = "1" ]
}

# Directory holding this script and the checked-in test programs beside it, so
# try_file works regardless of the directory make was invoked from.
readonly TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"

# Pointer width of the configured target. The sizeof tests below assert on it,
# and it differs between the 32-bit targets and x86-64.
PTR_SZ=$(sed -n 's/^#define PTR_SIZE \([0-9]*\).*/\1/p' \
    "$TESTS_DIR/../config" 2> /dev/null | head -1)
[ -n "${PTR_SZ}" ] || PTR_SZ=4

# Variadic arguments occupy one pointer-sized slot each, so an int-based walk
# over them advances this many int elements per argument: 1 on the 32-bit
# targets, 2 on LP64.
VS=$((PTR_SZ / 4))

# Test Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Category Tracking
declare -A CATEGORY_TESTS
declare -A CATEGORY_PASSED
declare -A CATEGORY_FAILED
CURRENT_CATEGORY="Initialization"

# Performance Metrics
TEST_START_TIME=$(date +%s)
PROGRESS_COUNT=0

# Command Line Arguments

if [ "$#" -lt 1 ]; then
    echo "Usage: $0 <stage> [<dynlink>]"
    echo "  stage: 0 (host compiler), 1 (stage1), or 2 (stage2)"
    echo "  dynlink: 0 (static linking), 1 (dynamic linking)"
    echo ""
    echo "Environment Variables:"
    echo "  VERBOSE=1         Enable verbose output"
    echo "  SHOW_SUMMARY=1    Show category summaries (default)"
    echo "  SHOW_PROGRESS=1   Show progress dots (default)"
    echo "  COLOR_OUTPUT=1    Enable colored output (default)"
    echo "  TEST_FILTER=<str> Run only categories whose name contains <str>"
    echo "  FAIL_FAST=1       Stop at the first failure (default: report all)"
    exit 1
fi

case "$1" in
    "0")
        readonly SHECC="$PWD/out/shecc"
        readonly STAGE="Stage 0 (Host Compiler)"
        ;;
    "1")
        readonly SHECC="${TARGET_EXEC:-} $PWD/out/shecc-stage1.elf"
        readonly STAGE="Stage 1 (Cross-compiled)"
        ;;
    "2")
        readonly SHECC="${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf"
        readonly STAGE="Stage 2 (Self-hosted)"
        ;;
    *)
        echo "$1 is not a valid stage"
        exit 1
        ;;
esac

if [ $# -ge 2 ] && [ "$2" = "1" ]; then
    readonly SHECC_CFLAGS="--dynlink"
    readonly LINK_MODE="dynamic"
else
    readonly SHECC_CFLAGS=""
    readonly LINK_MODE="static"
fi

# Utility Functions

# Color output functions
function print_color()
{
    if [ "$COLOR_OUTPUT" = "1" ]; then
        case "$1" in
            green) echo -ne "\033[32m$2\033[0m" ;;
            red) echo -ne "\033[31m$2\033[0m" ;;
            yellow) echo -ne "\033[33m$2\033[0m" ;;
            blue) echo -ne "\033[34m$2\033[0m" ;;
            bold) echo -ne "\033[1m$2\033[0m" ;;
            *) echo -n "$2" ;;
        esac
    else
        echo -n "$2"
    fi
}

# Begin a new test category
function begin_category()
{
    local category="$1"
    local description="${2:-}"

    # Save previous category summary if needed
    if [ "$CURRENT_CATEGORY" != "Initialization" ] && [ "$SHOW_SUMMARY" = "1" ]; then
        if [ "${CATEGORY_TESTS[$CURRENT_CATEGORY]:-0}" -gt 0 ]; then
            local passed="${CATEGORY_PASSED[$CURRENT_CATEGORY]:-0}"
            local total="${CATEGORY_TESTS[$CURRENT_CATEGORY]}"
            if [ "$VERBOSE_MODE" = "1" ]; then
                echo ""
                echo "  Subtotal: $passed/$total tests passed"
            fi
        fi
    fi

    CURRENT_CATEGORY="$category"
    if [ -z "$TEST_FILTER" ]; then
        CATEGORY_SELECTED=1
    else
        case "$category" in
            *"$TEST_FILTER"*) CATEGORY_SELECTED=1 ;;
            *) CATEGORY_SELECTED=0 ;;
        esac
    fi
    CATEGORY_TESTS["$category"]=0
    CATEGORY_PASSED["$category"]=0
    CATEGORY_FAILED["$category"]=0

    if [ "$VERBOSE_MODE" = "1" ] || [ "$SHOW_PROGRESS" = "1" ]; then
        echo ""
        print_color bold "=== "
        print_color blue "$category"
        if [ -n "$description" ]; then
            echo " - $description"
        else
            echo ""
        fi
    fi
}

# Show progress indicator
function show_progress()
{
    if [ "$SHOW_PROGRESS" = "1" ]; then
        ((PROGRESS_COUNT++))
        if [ $((PROGRESS_COUNT % 10)) -eq 0 ]; then
            echo -n "."
            if [ $((PROGRESS_COUNT % 50)) -eq 0 ]; then
                echo ""
            fi
        fi
    fi
}

# Core test failure reporting function (consolidated)
function report_test_failure()
{
    local test_type="$1"
    local tmp_in="$2"
    local tmp_exe="$3"
    local expected="$4"
    local actual="$5"
    local output="$6"
    local expected_output="${7:-}"
    local stderr_file="${8:-}"

    ((FAILED_TESTS++))
    ((CATEGORY_FAILED["$CURRENT_CATEGORY"]++))
    echo ""
    print_color red "FAILED $test_type in Category: $CURRENT_CATEGORY"
    echo
    echo "Expected exit code: $expected"
    echo "Actual exit code: $actual"
    if [ -n "$expected_output" ]; then
        echo "Expected output: '$expected_output'"
    fi
    echo "Actual output: '$output'"
    echo ""
    print_color yellow "Complete Test Program Code:"
    echo
    echo "=================================================="
    cat -n "$tmp_in"
    echo "=================================================="
    echo ""
    echo "Compiler command: $SHECC $SHECC_CFLAGS -o $tmp_exe $tmp_in"
    echo "Test files: input=$tmp_in, executable=$tmp_exe"
    if [ -n "$stderr_file" ] && [ -s "$stderr_file" ]; then
        echo ""
        print_color yellow "Compiler stderr:"
        echo
        cat "$stderr_file"
    fi
    echo ""
    if [ "$FAIL_FAST" = "1" ]; then
        exit 1
    fi
}

# Main test execution function
function try()
{
    local expected="$1"
    local expected_output=""
    local input=""
    local check_output=0

    if [ $# -eq 2 ]; then
        input="$2"
    elif [ $# -eq 3 ]; then
        expected_output="$2"
        input="$3"

        # An expectation was supplied, so compare against it -- including when
        # it is empty, which asserts that the program prints nothing.
        check_output=1
    fi

    test_selected || return 0

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_err="$(mktemp)"
    echo "$input" > "$tmp_in"

    # Keep the compiler's diagnostic rather than discarding it: without it a
    # failure reports only an exit-code mismatch and never says why.
    $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" 2> "$tmp_err"
    chmod +x $tmp_exe

    local output=''
    output=$(${TARGET_EXEC:-} "$tmp_exe")
    local actual="$?"

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))

    if [ "$actual" != "$expected" ]; then
        report_test_failure "TEST" "$tmp_in" "$tmp_exe" "$expected" "$actual" "$output" "$expected_output" "$tmp_err"
    elif [ "$check_output" = 1 ] && [ "$output" != "$expected_output" ]; then
        report_test_failure "TEST" "$tmp_in" "$tmp_exe" "$expected" "$actual" "$output" "$expected_output" "$tmp_err"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
        if [ "$VERBOSE_MODE" = "1" ]; then
            echo "$input"
            echo "exit code => $actual"
            echo "output => $output"
        fi
    fi
}

function try_()
{
    local expected="$1"
    local input="$(cat)"
    try "$expected" "$input"
}

function try_output()
{
    local expected="$1"
    local expected_output="$2"
    local input="$(cat)"
    try "$expected" "$expected_output" "$input"
}

# Compile and run a checked-in program through the same path as inline cases.
# This keeps the small end-to-end programs in both stage-0 and stage-2 runs.
function try_file()
{
    if [ "$#" -eq 2 ]; then
        try "$1" "$(< "$2")"
    else
        try "$1" "$2" "$(< "$3")"
    fi
}

# try_compile_error - test shecc with invalid C program Usage:
# - try_compile_error invalid_input_code compile "invalid_input_code" with shecc
# so that shecc generates a compilation error message.
#
# This function uses shecc to compile invalid code and obtains the exit code
# returned by shecc. The exit code must be a non-zero value to indicate that
# shecc has the ability to parse the invalid code and output an error message.
function try_compile_error()
{
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    echo "$input" > "$tmp_in"

    # Suppress compiler error output and "Aborted" messages completely Run in a
    # subshell with job control disabled
    (
        set +m 2> /dev/null # Disable job control messages
        $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" 2>&1
    ) > /dev/null 2>&1
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))

    if [ 0 == $exit_code ]; then
        report_test_failure "COMPILE ERROR TEST" "$tmp_in" "$tmp_exe" "non-zero" "0" "Compilation succeeded unexpectedly"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
        if [ "$VERBOSE_MODE" = "1" ]; then
            echo "Compilation error correctly detected"
        fi
    fi
}

function try_compile_error_message()
{
    local expected="$1"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_log="$(mktemp)"
    echo "$input" > "$tmp_in"

    $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" > "$tmp_log" 2>&1
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$exit_code" -eq 0 ] || ! rg -Fq "$expected" "$tmp_log"; then
        report_test_failure "COMPILE ERROR MESSAGE TEST" "$tmp_in" "$tmp_exe" \
            "$expected" "$exit_code" "$(< "$tmp_log")"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
}

# Verify a successful compilation emits a specific diagnostic. This is used for
# C constructs that are permitted but deserve a warning, such as casting away
# const qualification.
function try_compile_warning()
{
    local expected="$1"
    local extra_flags="${2:-}"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_log="$(mktemp)"
    echo "$input" > "$tmp_in"

    $SHECC $SHECC_CFLAGS $extra_flags -o "$tmp_exe" "$tmp_in" > "$tmp_log" 2>&1
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$exit_code" -ne 0 ] || ! rg -Fq "$expected" "$tmp_log"; then
        report_test_failure "COMPILE WARNING TEST" "$tmp_in" "$tmp_exe" \
            "$expected" "$exit_code" "$(< "$tmp_log")"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
}

function items()
{
    local expected="$1"
    local input="$2"
    try "$expected" "int main(int argc, int argv) { $input }"
}

function expr()
{
    local expected="$1"
    local input="$2"
    items "$expected" "exit($input);"
}

# Batch test runners for common patterns
function run_expr_tests()
{
    local -n tests_ref=$1
    for test in "${tests_ref[@]}"; do
        IFS=' ' read -r expected code <<< "$test"
        expr "$expected" "$code"
    done
}

function run_try_tests()
{
    local -n tests_ref=$1
    for test in "${tests_ref[@]}"; do
        local expected=$(echo "$test" | head -n1)
        local code=$(echo "$test" | tail -n+2)
        try_ "$expected" <<< "$code"
    done
}

function run_items_tests()
{
    local -n tests_ref=$1
    for test in "${tests_ref[@]}"; do
        IFS=' ' read -r expected code <<< "$test"
        items "$expected" "$code"
    done
}

# try_large - test shecc with large return values (> 255) Usage:
# - try_large expected_value input_code compile "input_code" with shecc and
# verify the return value by printing it instead of using exit code (which is
# limited to 0-255).
function try_large()
{
    local expected="$1"
    local input="$(cat)"

    test_selected || return 0
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

    # Suppress compiler warnings by redirecting stderr
    $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" 2> /dev/null
    chmod +x $tmp_exe

    local output=$(${TARGET_EXEC:-} "$tmp_exe")
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))

    if [ "$exit_code" != "0" ] || [ "$output" != "$expected" ]; then
        ((FAILED_TESTS++))
        ((CATEGORY_FAILED["$CURRENT_CATEGORY"]++))
        echo ""
        print_color red "FAILED LARGE VALUE TEST in Category: $CURRENT_CATEGORY"
        echo "Expected output: $expected"
        echo "Actual output: $output"
        echo "Exit code: $exit_code"
        echo ""
        print_color yellow "Complete Test Program Code:"
        echo
        echo "=================================================="
        cat -n "$tmp_in"
        echo "=================================================="
        echo ""
        print_color yellow "Original Input Code:"
        echo
        echo "--------------------------------------------------"
        echo "$input"
        echo "--------------------------------------------------"
        echo ""
        echo "Compiler command: $SHECC $SHECC_CFLAGS -o $tmp_exe $tmp_in"
        echo "Test files: input=$tmp_in, executable=$tmp_exe"
        exit 1
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
        if [ "$VERBOSE_MODE" = "1" ]; then
            echo "Large value test: $expected (output => $output)"
        fi
    fi
}

# Test Execution Begins

echo "[[[ shecc Test Suite ]]]"
echo ""
echo "Date:     $(date '+%Y-%m-%d %H:%M:%S')"
echo "Compiler: $SHECC"
echo "Stage:    $STAGE"
echo ""

if [ "$SHOW_PROGRESS" = "1" ]; then
    echo "Running tests..."
fi

# Category: Checked-in end-to-end programs
begin_category "Standalone Programs" "Testing checked-in end-to-end programs"

try_file 0 'F(10) = 55' "$TESTS_DIR/fib.c"
try_file 0 $'1\nHello World' "$TESTS_DIR/hello.c"
try_file 0 '' "$TESTS_DIR/strength-reduce.c"

# Category: Basic Literals and Constants
begin_category "Literals and Constants" "Testing integer, character, and string literals"

# just a number
expr 0 0

# C99 _Bool conversions store the truth value, not a truncated source byte.
try_ 4 << EOF
_Bool echo_bool(_Bool value) { return value; }
int main(void) {
    _Bool positive = 2;
    _Bool negative = -7;
    _Bool zero = 0;
    positive = 42;
    return (positive == 1) + (negative == 1) + (zero == 0) +
           (echo_bool(-3) == 1);
}
EOF

try_ 2 << EOF
_Bool global_true = 2;
int main(void) {
    static _Bool local_true = -3;
    return (global_true == 1) + (local_true == 1);
}
EOF

try_ 31 << EOF
int bool_pointer_object;
_Bool pointer_to_bool(int *value) { return value; }
_Bool bool_identity(_Bool value) { return value; }
int main(void) {
    _Bool direct = &bool_pointer_object;
    int *null_value = 0;
    _Bool passed = pointer_to_bool(&bool_pointer_object);
    _Bool null_result = pointer_to_bool(null_value);
    return direct + 2 * passed + 4 * (!null_result) +
           8 * (bool_identity(&bool_pointer_object) == 1) +
           16 * (bool_identity(null_value) == 0);
}
EOF
expr 42 42

# octal constant (satisfying re(0[0-7]+))
expr 10 012
expr 65 0101

# Category: Arithmetic Operations
begin_category "Arithmetic Operations" "Testing +, -, *, /, % operators"

# Unsigned int operations must use modular arithmetic, logical right shift,
# unsigned division, and unsigned relational comparisons after the value's high
# bit is set at run time.
try_ 4 << EOF
int main(void)
{
    unsigned int bits = 2147483647;
    bits = bits + bits + 1;
    return (bits >> 31) + (bits / 2 == 2147483647) +
           (bits / 3 == 1431655765) + (bits > 0);
}
EOF

try_ 1 << EOF
int main(void) {
    unsigned int bits = 2147483647;
    bits = bits + bits + 1;
    return bits >> 31;
}
EOF

try_ 1 << EOF
int main(void) {
    unsigned int bits = 2147483647;
    bits = bits + bits + 1;
    return bits / 2 == 2147483647;
}
EOF
try_ 2 << EOF
int main(void) {
    int negative = -1;
    unsigned int divisor = 2U;
    return (negative / divisor == 2147483647U) +
           (negative % divisor == 1U);
}
EOF
try_ 1 << EOF
int main(void) {
    unsigned int all_bits = 4294967295U;
    return all_bits >> 31;
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned int value = 1UL;
    return (value + 1L == 2U) + (sizeof(long) == 4);
}
EOF
try_ 2 << EOF
int main(void) {
    return (0xffffffff >> 31) + (-2147483648 < 0);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned int one = 1U;
    return ((~one) >> 31) + ((1 ? one - 2 : -1) >> 31);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned char byte = 1;
    unsigned short half = 1;
    return (-byte == -1) + (-half == -1);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned char byte = 255U;
    unsigned short half = 65535U;
    byte++;
    half++;
    return (byte == 0U) + (half == 0U);
}
EOF
if [ "$PTR_SZ" -ge 8 ]; then
    try_ 3 << EOF
long long identity(long long value) { return value; }
unsigned long long uidentity(unsigned long long value) { return value; }
int main(void) {
    long long signed_value = 1000;
    unsigned long long unsigned_value = 2000U;
    return (sizeof(signed_value) == 8) +
           (identity(signed_value) == 1000) +
           (uidentity(unsigned_value) == 2000U);
}
EOF
    try_ 4 << EOF
long int identity_long_int(long int value) { return value; }
signed long long int identity_signed_wide(signed long long int value) {
    return value;
}
unsigned long long int identity_unsigned_wide(unsigned long long int value) {
    return value;
}
int main(void) {
    long int narrow = -7L;
    signed long long int negative = -0x100000000LL;
    unsigned long long int positive = 0x100000000ULL;
    return (identity_long_int(narrow) == -7L) +
           (identity_signed_wide(negative) == -0x100000000LL) +
           (identity_unsigned_wide(positive) == 0x100000000ULL) +
           (sizeof(unsigned long long int) == 8);
}
EOF
    try_ 2 << EOF
typedef long long signed_wide;
typedef unsigned long long unsigned_wide;
int main(void) {
    signed_wide signed_value = -1LL;
    unsigned_wide unsigned_value = 1ULL << 32;
    return (signed_value < 0LL) + ((unsigned_value >> 32) == 1ULL);
}
EOF
    try_ 2 << EOF
typedef long unsigned long reordered_unsigned_wide;
typedef const long long signed_wide_const;
int main(void) {
    reordered_unsigned_wide value = 1ULL << 32;
    signed_wide_const negative = -1LL;
    return ((value >> 32) == 1ULL) + (negative < 0LL);
}
EOF
fi
try_compile_error << EOF
unsigned unsigned int invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
signed unsigned int invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
signed signed int invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
long long long invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
typedef unsigned unsigned int invalid;
int main(void) { return 0; }
EOF
try_compile_error << EOF
typedef signed unsigned int invalid;
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { return (signed unsigned int) 1; }
EOF
try_compile_error << EOF
int main(void) { return (unsigned unsigned int) 1; }
EOF
try_compile_error << EOF
enum invalid_enum { invalid_value };
unsigned enum invalid_enum invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
enum invalid_enum { invalid_value };
int main(void) { return (long enum invalid_enum) invalid_value; }
EOF
try_compile_error << EOF
enum invalid_enum { invalid_value };
int main(void) { return sizeof(signed enum invalid_enum); }
EOF
try_compile_error << EOF
int main(void) { return sizeof(signed unsigned int); }
EOF
try_compile_error << EOF
int main(void) { return sizeof(unsigned unsigned int); }
EOF
try_compile_error << EOF
int signed char invalid;
int main(void) { return invalid; }
EOF
try_compile_error << EOF
typedef int unsigned char invalid;
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { return (int signed char) 1; }
EOF
try_compile_error << EOF
int main(void) { return sizeof(int unsigned char); }
EOF
try_compile_error << EOF
typedef long long long invalid;
int main(void) { return 0; }
EOF
try_compile_error << EOF
long incompatible_object;
int incompatible_object;
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned long incompatible_function(unsigned long value);
unsigned int incompatible_function(unsigned int value);
int main(void) { return 0; }
EOF
try_compile_error << EOF
static int object_then_function;
static int object_then_function(void) { return 1; }
int main(void) { return object_then_function(); }
EOF
try_compile_error << EOF
static int function_then_object(void) { return 1; }
static int function_then_object;
int main(void) { return function_then_object(); }
EOF
try_ 3 << EOF
int main(void) {
    long signed_long = -1L;
    unsigned int unsigned_int = 1U;
    unsigned long unsigned_long = 1UL;
    int signed_int = -2;
    return (sizeof(1L) == sizeof(long)) +
           ((signed_long + unsigned_int) == 0UL) +
           ((unsigned_long + signed_int) == 0xffffffffUL);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned long value = 0xffffffffUL;
    return (sizeof(long unsigned) == 4) +
           ((long unsigned) value == 0xffffffffUL);
}
EOF
try_ 8 << EOF
typedef short unsigned int base_first_ushort;
short unsigned int preserve_half(short unsigned int value)
{
    return value;
}
int main(void) {
    short unsigned int half = 65535U;
    char unsigned byte = 255U;
    int signed whole = -1;
    int unsigned short reordered_half = 65535U;
    base_first_ushort typedef_half = 65535U;
    return (sizeof(half) == 2) + (sizeof(byte) == 1) +
           (half == 65535U && byte == 255U && whole < 0) +
           (preserve_half(half) == 65535U) +
           (reordered_half == 65535U) +
           ((short unsigned int)-1 == 65535U) +
           (sizeof(short unsigned int) == 2) +
           (sizeof(typedef_half) == 2 && typedef_half == 65535U);
}
EOF
if [ "$PTR_SZ" -ge 8 ]; then
    try_ 1 << EOF
int main(void) { return (unsigned long long) 1 == 1ULL; }
EOF
fi
if [ "$PTR_SZ" -ge 8 ]; then
    try_ 4 << EOF
int main(void) {
    long long decimal = 4294967296;
    long long hexadecimal = 0x100000000;
    unsigned long long all_bits = 0xffffffffffffffff;
    return ((decimal >> 32) == 1LL) +
           ((hexadecimal >> 32) == 1LL) +
           ((all_bits >> 63) == 1ULL) +
           (all_bits > 0ULL);
}
EOF
    try_ 1 << EOF
int main(void) {
    unsigned long long maximum = 0xffffffffffffffffULL;
    return maximum + 1ULL == 0ULL;
}
EOF
    try_ 4 << EOF
int main(void) {
    return (sizeof(2147483647) == 4) +
           (sizeof(2147483648) == 8) +
           (sizeof(0xffffffff) == 4) +
           (sizeof(0x100000000) == 8);
}
EOF
    try_ 6 << EOF
int main(void) {
    /* C99 chooses candidates from the suffix-specific list: the current
     * ABI has 32-bit long, so overflow moves to the 64-bit long-long tier. */
    return (sizeof(2147483647L) == 4) +
           (sizeof(2147483648L) == 8) +
           (sizeof(0x80000000L) == 4) +
           (sizeof(0x80000000LL) == 8) +
           (sizeof(4294967295U) == 4) +
           (sizeof(4294967296U) == 8);
}
EOF
fi
if [ "$PTR_SZ" -lt 8 ]; then
    try_compile_error << EOF
int main(void) { return 4294967296U; }
EOF
else
    try_ 1 << EOF
int main(void) { return 4294967296U >> 32; }
EOF
fi
if [ "$PTR_SZ" -lt 8 ]; then
    try_compile_error << EOF
int main(void) { return 1LL; }
EOF
    try_compile_error << EOF
long long unsupported_value;
int main(void) { return 0; }
EOF
    try_compile_error << EOF
long long unsupported_return(void) { return 0; }
int main(void) { return 0; }
EOF
    try_compile_error << EOF
long long (*unsupported_callback)(void);
int main(void) { return 0; }
EOF
    try_ 2 << EOF
int main(void) {
    return (sizeof(long long) == 8) +
           (sizeof(unsigned long long) == 8);
}
EOF
    try_ 1 << EOF
typedef long long *wide_pointer;
long long *identity_wide_pointer(long long *value) { return value; }
int main(void) {
    int storage = 0;
    wide_pointer alias = (wide_pointer)&storage;
    return identity_wide_pointer(alias) == alias;
}
EOF
else
    try_ 3 << EOF
int main(void) {
    return (sizeof(1LL) == 8) + ((1LL << 32) != 0) +
           (((1ULL << 32) >> 32) == 1U);
}
EOF
    try_ 3 << EOF
int main(void) {
    unsigned long long value = 0x100000000ULL;
    unsigned long long pattern = 0x123456789abcdef0ULL;
    return ((value >> 32) == 1ULL) +
           ((pattern >> 32) == 0x12345678ULL) +
           ((value + 7ULL) == 0x100000007ULL);
}
EOF
    try_ 3 << EOF
int main(void) {
    unsigned long long value = 4294967296ULL;
    unsigned long long pattern = 1311768467463790320ULL;
    return ((value >> 32) == 1ULL) +
           ((pattern >> 32) == 305419896ULL) +
           ((value + 7ULL) == 4294967303ULL);
}
EOF
    try_ 3 << EOF
int main(void) {
    unsigned long long octal = 040000000000ULL;
    unsigned long long binary = 0b100000000000000000000000000000000ULL;
    return ((octal >> 32) == 1ULL) +
           ((binary >> 32) == 1ULL) +
           ((octal + binary) == 0x200000000ULL);
}
EOF
    try_ 4 << EOF
int main(void) {
    unsigned long long all = 18446744073709551615ULL;
    long long min = -9223372036854775808LL;
    return (all == 0xffffffffffffffffULL) +
           ((all >> 63) == 1ULL) +
           (min < 0LL) +
           ((min >> 63) == -1LL);
}
EOF
    try_ 2 << EOF
unsigned long long global_value = 0x123456789abcdef0ULL;
int main(void) {
    return ((global_value >> 32) == 0x12345678ULL) +
           (((global_value + 1ULL) >> 32) == 0x12345678ULL);
}
EOF
    try_ 2 << EOF
long long global_min = -9223372036854775808LL;
int main(void) {
    return (global_min < 0LL) + ((global_min >> 63) == -1LL);
}
EOF
    try_ 2 << EOF
unsigned long long global_sum = 0x100000000ULL + 7ULL;
int main(void) {
    return ((global_sum >> 32) == 1ULL) +
           ((global_sum - 7ULL) == 0x100000000ULL);
}
EOF
    try_ 2 << EOF
unsigned long long global_parenthesized = (0x100000000ULL + 7ULL);
int main(void) {
    return ((global_parenthesized >> 32) == 1ULL) +
           ((unsigned int)global_parenthesized == 7U);
}
EOF

    try_ 2 << EOF
unsigned long long global_nested_parenthesized = ((0x100000000ULL + 7ULL));
int main(void) {
    return ((global_nested_parenthesized >> 32) == 1ULL) +
           ((unsigned int)global_nested_parenthesized == 7U);
}
EOF

    try_ 2 << EOF
unsigned long long global_grouped_outer = (0x100000000ULL + 7ULL) * 2ULL;
int main(void) {
    return ((global_grouped_outer >> 32) == 2ULL) +
           ((unsigned int)global_grouped_outer == 14U);
}
EOF

    try_ 2 << EOF
unsigned long long global_wide_late = (3 + 0x100000000ULL) * 2ULL;
int main(void) {
    return ((global_wide_late >> 32) == 2ULL) +
           ((unsigned int)global_wide_late == 6U);
}
EOF

    try_ 2 << EOF
unsigned long long global_nested_grouped =
    (0x100000000ULL + (3ULL * 4ULL)) - 5ULL;
int main(void) {
    return ((global_nested_grouped >> 32) == 1ULL) +
           ((unsigned int)global_nested_grouped == 7U);
}
EOF

    try_ 2 << EOF
long long global_negated_grouped = -(0x100000000LL + 7LL);
int main(void) {
    return ((global_negated_grouped >> 32) == -2LL) +
           ((unsigned int) global_negated_grouped == 0xfffffff9U);
}
EOF

    try_ 4 << EOF
unsigned long long global_wide_complement = ~0ULL;
long long global_wide_double_negation = -(~0LL);
unsigned long long global_wide_unary_plus = +0x100000000ULL;
unsigned long long global_wide_logical_not = !0ULL;
int main(void) {
    return (global_wide_complement == 0xffffffffffffffffULL) +
           (global_wide_double_negation == 1LL) +
           (global_wide_unary_plus == 0x100000000ULL) +
           (global_wide_logical_not == 1ULL);
}
EOF

    try_ 2 << EOF
int main(void) {
    unsigned int all = 0xffffffffU;
    return (all == 0xffffffffU) + (all > 1U);
}
EOF

    try_ 2 << EOF
long long global_negative_wide = -0x100000000LL;
int main(void) {
    return ((global_negative_wide >> 32) == -1LL) +
           ((unsigned int)global_negative_wide == 0U);
}
EOF

    try_ 2 << EOF
unsigned long long global_unsigned_negative_wide = -0x100000000LL;
int main(void) {
    return ((global_unsigned_negative_wide >> 32) == 0xffffffffULL) +
           ((unsigned int)global_unsigned_negative_wide == 0U);
}
EOF
    try_ 3 << EOF
unsigned long long global_unsuffixed = 0x100000000;
unsigned long long global_all_bits = 0xffffffffffffffff;
long long global_decimal = 2147483648;
int main(void) {
    return ((global_unsuffixed >> 32) == 1ULL) +
           ((global_all_bits >> 63) == 1ULL) +
           (global_decimal == 2147483648LL);
}
EOF
    try_ 2 << EOF
unsigned int global_octal_max = 037777777777;
unsigned long long global_octal_wide = 040000000000;
int main(void) {
    return (global_octal_max >> 31) +
           ((global_octal_wide >> 32) == 1ULL);
}
EOF
    try_ 3 << EOF
unsigned long long global_quotient =
    0x123456789abcdef0ULL / 0x100000000ULL;
unsigned long long global_remainder =
    0x123456789abcdef0ULL % 0x100000000ULL;
int main(void) {
    return (global_quotient == 0x12345678ULL) +
           (global_remainder == 0x9abcdef0ULL) +
           ((global_quotient << 32) == 0x1234567800000000ULL);
}
EOF
    try_ 2 << EOF
unsigned long long global_precedence =
    0x100000000ULL + 3ULL * 4ULL - 5ULL;
int main(void) {
    return ((global_precedence >> 32) == 1ULL) +
           ((unsigned int)global_precedence == 7U);
}
EOF
    try_ 2 << EOF
unsigned long long identity_wide(unsigned long long value) { return value; }
int main(void) {
    unsigned long long value = identity_wide(0x123456789abcdef0ULL);
    return ((value >> 32) == 0x12345678ULL) +
           ((value - 0x1234567800000000ULL) == 0x9abcdef0ULL);
}
EOF
    try_ 3 << EOF
int main(void) {
    unsigned long long high = 0x100000000ULL;
    unsigned long long mask = 0xffffffffffffffffULL;
    return (((1ULL | high) >> 32) == 1ULL) +
           (((high & mask) >> 32) == 1ULL) +
           ((high ^ high) == 0ULL);
}
EOF
    try_ 3 << EOF
int main(void) {
    unsigned long long value = 0x123456789abcdef0ULL;
    return ((value / 0x100000000ULL) == 0x12345678ULL) +
           ((value % 0x100000000ULL) == 0x9abcdef0ULL) +
           ((0x100000000ULL / 3ULL) == 1431655765ULL);
}
EOF
    try_compile_error << EOF
int main(void) { return 18446744073709551616ULL != 0ULL; }
EOF
    try_compile_error << EOF
int main(void) { return 9223372036854775808LL != 0LL; }
EOF
    try_compile_error << EOF
int main(void) { return 9223372036854775808 != 0LL; }
EOF
    try_ 2 << EOF
int main(void) {
    unsigned long long first = 0x100000000lLu;
    unsigned long long second = 0x100000000Ull;
    return ((first >> 32) == 1ULL) + ((second >> 32) == 1ULL);
}
EOF
    try_ 2 << EOF
int main(void) {
    unsigned long long all_bits = ~0ULL;
    return ((all_bits >> 63) == 1ULL) +
           (!0x100000000ULL == 0);
}
EOF
    try_compile_error << EOF
int main(void) { return 1UU; }
EOF
    try_compile_error << EOF
int main(void) { return 1LLL; }
EOF
    try_compile_error << EOF
int main(void) { return 1LUL; }
EOF
    try_ 2 << EOF
int main(void) {
    int value = -6;
    return (value / 4 == -1) + (value % 4 == -2);
}
EOF
    try_ 2 << EOF
int main(void) {
    return (sizeof(long long) == 8) + (sizeof(unsigned long long) == 8);
}
EOF
    try_ 5 << EOF
int main(void) {
    unsigned int high = 0xffffffffU;
    unsigned long long widened_unsigned = (unsigned long long) high;
    long long widened_signed = (long long) -1;
    unsigned long long shifted = (unsigned long long) 1U << 32;
    return ((widened_unsigned >> 32) == 0ULL) +
           ((widened_unsigned >> 31) == 1ULL) +
           (widened_signed == -1LL) +
           ((shifted >> 32) == 1ULL) +
           ((unsigned int) shifted == 0U);
}
EOF
    try_ 2 << EOF
long long bump(long long value) { return value + 1LL; }
unsigned long long twice(unsigned long long value) { return value * 2ULL; }
int main(void) {
    long long signed_value = 1LL << 32;
    unsigned long long unsigned_value = 1ULL << 32;
    return ((bump(signed_value) >> 32) == 1LL) +
           ((twice(unsigned_value) >> 33) == 1ULL);
}
EOF
    try_ 4 << EOF
int main(void) {
    long long signed_value = 1LL << 33;
    unsigned long long unsigned_value = 1ULL << 33;
    return (((signed_value / 2LL) >> 32) == 1LL) +
           ((signed_value % 3LL) == 2LL) +
           (((unsigned_value / 2ULL) >> 32) == 1ULL) +
           ((unsigned_value % 3ULL) == 2ULL);
}
EOF
    try_ 2 << EOF
long long eighth(long long a, long long b, long long c, long long d,
                 long long e, long long f, long long g, long long h) {
    return h;
}
unsigned long long ueighth(unsigned long long a, unsigned long long b,
                            unsigned long long c, unsigned long long d,
                            unsigned long long e, unsigned long long f,
                            unsigned long long g, unsigned long long h) {
    return h;
}
int main(void) {
    long long signed_value = 1LL << 32;
    unsigned long long unsigned_value = 1ULL << 32;
    return ((eighth(1LL, 2LL, 3LL, 4LL, 5LL, 6LL, 7LL, signed_value) >> 32) == 1LL) +
           ((ueighth(1ULL, 2ULL, 3ULL, 4ULL, 5ULL, 6ULL, 7ULL, unsigned_value) >> 32) == 1ULL);
}
EOF
    try_ 1 << EOF
int main(void) {
    unsigned long long value = 0xffffffffU;
    value = value * 16 + 0;
    return (value >> 32) == 15ULL;
}
EOF
    try_ 1 << EOF
unsigned long long scale(unsigned long long value, int factor) {
    unsigned long long product = value * factor;
    return product;
}
int main(void) {
    unsigned long long value = 0xffffffffU;
    return (scale(value, 16) >> 32) == 15ULL;
}
EOF
    try_ 4 << EOF
int main(void) {
    unsigned int high = 0xffffffffU;
    long long one = 1LL;
    return ((high + one) == 4294967296LL) +
           ((high * one) == 4294967295LL) +
           ((-1 + 1ULL) == 0ULL) +
           ((-1 > 1ULL) == 1);
}
EOF
fi
try_ 1 << EOF
unsigned int identity(unsigned int value) { return value; }
int main(void) { return identity(4294967295U) >> 31; }
EOF
try_ 2 << EOF
unsigned char byte_identity(unsigned char value) { return value; }
unsigned short half_identity(unsigned short value) { return value; }
int main(void) {
    return (byte_identity(255) == 255) + (half_identity(65535) == 65535);
}
EOF
try_ 1 << EOF
unsigned int eighth(unsigned int a, unsigned int b, unsigned int c,
                    unsigned int d, unsigned int e, unsigned int f,
                    unsigned int g, unsigned int h) { return h; }
int main(void) {
    return eighth(1U, 2U, 3U, 4U, 5U, 6U, 7U, 4294967295U) >> 31;
}
EOF
try_ 2 << EOF
int main(void) {
    return (((unsigned int)-1) >> 31) +
           (((unsigned short)-1) >> 15);
}
EOF
try_ 5 << EOF
/* C99 integer promotions preserve the magnitude of narrow unsigned values:
 * unary operators promote to int, and the promoted operands then participate
 * in ordinary binary arithmetic.  Sign-extending either source would make
 * these comparisons false. */
int main(void) {
    unsigned char byte = 255;
    unsigned short half = 65535;
    return (+byte == 255) + (~byte == -256) + (-byte == -255) +
           (+half == 65535) + (~half == -65536);
}
EOF
try_ 3 << EOF
typedef signed char signed_byte;
signed_byte echo_signed_byte(signed_byte value);
signed char echo_signed_byte(signed char value) { return value; }
signed_byte compatible_signed_byte_object;
signed char compatible_signed_byte_object;
int main(void) {
    signed_byte value = -1;
    return (sizeof(signed char) == 1) + (echo_signed_byte(value) == -1) +
           (compatible_signed_byte_object == 0);
}
EOF
try_compile_error << EOF
char incompatible_character_object;
signed char incompatible_character_object;
int main(void) { return 0; }
EOF
try_compile_error << EOF
char incompatible_character_function(char value);
signed char incompatible_character_function(signed char value);
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) {
    char plain = 0;
    signed char signed_value = 0;
    char *plain_pointer = &plain;
    signed char *signed_pointer = &signed_value;
    plain_pointer = signed_pointer;
    return *plain_pointer;
}
EOF
try_compile_error << EOF
void take_plain(char *value) { }
int main(void) {
    signed char value = 0;
    take_plain(&value);
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    char plain = 0;
    signed char signed_value = 0;
    return &plain == &signed_value;
}
EOF
try_ 0 << EOF
int main(void) {
    signed char value = 0;
    void *bridge = &value;
    signed char *round_trip = bridge;
    return *round_trip;
}
EOF
try_ 0 << EOF
int main(void) {
    unsigned char byte = 255;
    unsigned short half = 65535;
    byte++;
    half++;
    return byte + half;
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned char byte = 255;
    unsigned short half = 65535;
    byte /= 2;
    half /= 2;
    return (byte == 127) + (half == 32767);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned char bytes[1] = {255};
    unsigned short halves[1] = {65535};
    bytes[0] /= 2;
    halves[0] /= 2;
    return (bytes[0] == 127) + (halves[0] == 32767);
}
EOF
try_ 2 << EOF
typedef unsigned char *uchar_pointer;
typedef unsigned short *ushort_pointer;
int main(void) {
    unsigned char bytes[1] = {255};
    unsigned short halves[1] = {65535};
    uchar_pointer byte_ptr = bytes;
    ushort_pointer half_ptr = halves;
    return (byte_ptr[0] / 2 == 127) + (half_ptr[0] / 2 == 32767);
}
EOF
try_ 1 << EOF
typedef unsigned char *uchar_pointer;
typedef uchar_pointer *uchar_pointer_pointer;
int main(void) {
    unsigned char bytes[1] = {255};
    uchar_pointer pointer = bytes;
    uchar_pointer_pointer pointer_to_pointer = &pointer;
    uchar_pointer loaded = *pointer_to_pointer;
    return loaded[0] / 2 == 127;
}
EOF
try_ 1 << EOF
typedef unsigned char *uchar_pointer;
typedef uchar_pointer *uchar_pointer_pointer;
typedef uchar_pointer_pointer *uchar_pointer_pointer_pointer;
int main(void) {
    unsigned char bytes[1] = {255};
    uchar_pointer pointer = bytes;
    uchar_pointer_pointer pointer_to_pointer = &pointer;
    uchar_pointer_pointer_pointer pointer_to_pointer_to_pointer =
        &pointer_to_pointer;
    uchar_pointer_pointer loaded_pointer_to_pointer =
        *pointer_to_pointer_to_pointer;
    uchar_pointer loaded_pointer = *loaded_pointer_to_pointer;
    return loaded_pointer[0] / 2 == 127;
}
EOF
try_ 1 << EOF
int main(void) { return '\x123' == 0x23; }
EOF
try_ 3 << EOF
#if __STDC__ != 1
#error __STDC__ must be one
#endif
#if __STDC_VERSION__ != 199901L
#error expected C99 version macro
#endif
int main(void) { return __STDC__ + !__STDC_HOSTED__ +
                        (__STDC_VERSION__ == 199901L); }
EOF
try_ 2 << EOF
struct unsigned_members { unsigned char byte; unsigned short half; };
int main(void) {
    struct unsigned_members value = {255, 65535};
    return (value.byte / 2 == 127) + (value.half / 2 == 32767);
}
EOF
try_ 7 << EOF
struct typedef_pair { int left; int right; };
typedef struct typedef_pair *pair_pointer;
int main(void) {
    struct typedef_pair value = {3, 4};
    pair_pointer pointer = &value;
    return pointer[0].left + pointer[0].right;
}
EOF
try_ 9 << EOF
union typedef_value { int left; int right; };
typedef union typedef_value *value_pointer;
int main(void) {
    union typedef_value value;
    value.right = 9;
    value_pointer pointer = &value;
    return pointer[0].right;
}
EOF
try_ 42 << EOF
struct packet { int value; char data[]; };
int main(void) {
    char storage[sizeof(struct packet) + sizeof(long unsigned int) - 1];
    struct packet *packet = (struct packet *)storage;
    packet->value = 39;
    packet->data[0] = 3;
    return (sizeof(struct packet) == 4 ? packet->value : 0) + packet->data[0];
}
EOF
try_ 42 << EOF
struct matrix { int tag; int cells[][2]; };
int main(void) {
    char storage[sizeof(struct matrix) + 16];
    struct matrix *matrix = (struct matrix *)storage;
    matrix->cells[1][1] = 42;
    return matrix->cells[1][1];
}
EOF
try_compile_error << EOF
struct invalid { char data[]; int value; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct invalid { int value, data[]; int after; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct invalid { char data[]; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
union invalid { char data[]; int value; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
struct invalid { int header; struct packet packet; };
int main(void) { return 0; }
EOF
try_ 1 << EOF
struct packet { int value; char data[]; };
union holder { struct packet packet; int value; };
int main(void) { return sizeof(union holder) == sizeof(int); }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
union holder { struct packet packet; int value; };
struct invalid { union holder holder; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
typedef struct packet packet_t;
struct invalid { packet_t packet; };
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
struct packet packets[2];
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
typedef struct packet packet_t;
packet_t packets[2];
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct packet { int value; char data[]; };
union holder { struct packet packet; int value; };
union holder holders[2];
int main(void) { return 0; }
EOF
try_ 4 << EOF
struct packet { int value; char data[]; };
struct packet *packets[2];
typedef struct packet *packet_ptr;
packet_ptr typedef_packets[2];
int main(void) {
    return (sizeof(packets) == 2 * sizeof(struct packet *)) +
           (sizeof packets == 2 * sizeof(struct packet *)) +
           (sizeof(typedef_packets) == 2 * sizeof(packet_ptr)) +
           (sizeof typedef_packets == 2 * sizeof(packet_ptr));
}
EOF
try_ 7 << EOF
typedef struct { int left; int right; } *anonymous_pair_pointer;
int main(void) {
    int values[4] = {1, 2, 3, 4};
    anonymous_pair_pointer pointer = (anonymous_pair_pointer)values;
    return pointer[1].left + pointer[1].right;
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned char bytes[1] = {255};
    unsigned short halves[1] = {65535};
    return (bytes[0] / 2 == 127) + (halves[0] / 2 == 32767);
}
EOF
try_ 2 << EOF
int main(void) {
    unsigned int one = 1U;
    int minus_two = -2;
    int minus_one = -1;
    return ((one + minus_two) >> 31) + (minus_one > one);
}
EOF
try_ 4 << EOF
typedef unsigned short ushort;
int main(void) {
    ushort small = 65535;
    return (small > 0) + (sizeof(unsigned char) == 1) +
           (sizeof(unsigned short) == 2) + (sizeof(unsigned long) == 4);
}
EOF

declare -a arithmetic_tests=(
    "42 24+18"
    "30 58-28"
    "10 5*2"
    "4 16>>2"
    "20 8+3*4"
    "54 (11-2)*6"
    "10 9/3+7"
    "8 8/(4-3)"
    "35 8+3*5+2*6"
    "55 1+2+3+4+5+6+7+8+9+10"
    "55 ((((((((1+2)+3)+4)+5)+6)+7)+8)+9)+10"
    "55 1+(2+(3+(4+(5+(6+(7+(8+(9+10))))))))"
    "210 1+(2+(3+(4+(5+(6+(7+(8+(9+(10+(11+(12+(13+(14+(15+(16+(17+(18+(19+20))))))))))))))))))"
    "11 1+012"
    "25 017+012"
    "2 5%3"
)

run_expr_tests arithmetic_tests
expr 6 "111 % 7"

try_output 0 "1 1 -1 -1" << EOF
int v1 = 5 % 4;
int v2 = 5 % -4;
int v3 = -5 % 4;
int v4 = -5 % -4;
int main() {
    printf("%d %d %d %d", v1, v2, v3, v4);
    return 0;
}
EOF

try_compile_error << EOF
int value = 1 / 0;
int main() { return value; }
EOF

try_compile_error << EOF
int value = 1 % 0;
int main() { return value; }
EOF

# Category: Overflow Behavior
begin_category "Overflow Behavior" "Testing integer overflow handling"

try_output 0 "-2147483647" << EOF
int main()
{
    int a = 2147483647;
    a += 2;
    printf("%d\n", a);
    return 0;
}
EOF

try_output 0 "-32767" << EOF
int main() {
    short a = 32767;
    a += 2;
    printf("%d\n", a);
    return 0;
}
EOF

try_output 0 "-127" << EOF
int main() {
    char a = 127;
    a += 2;
    printf("%d\n", a);
    return 0;
}
EOF

# Category: Comparison Operations
begin_category "Comparison Operations" "Testing relational and equality operators"

declare -a comparison_tests=(
    "1 10>5"
    "1 3+3>5"
    "0 30==20"
    "0 5>=10"
    "1 5>=5"
    "1 30!=20"
    "1 010==8"
    "1 011<11"
    "0 021>=21"
    "1 (012-5)==5"
    "16 0100>>2"
    "18 ~0355"
)

run_expr_tests comparison_tests

# Category: Logical Operations
begin_category "Logical Operations" "Testing logical AND, OR, NOT operators"

declare -a logical_tests=(
    "0 !237"
    "18 ~237"
    "0 0||0"
    "1 1||0"
    "1 1||1"
    "0 0&&0"
    "0 1&&0"
    "1 1&&1"
)

run_expr_tests logical_tests

# Logical negation of a pointer yields int, irrespective of the pointed-to type.
# A record pointer used to carry its record size into a following comparison,
# producing an invalid truncation on 32-bit targets.
try_ 2 << EOF
struct pair { int first; int second; };
int main(void) {
    struct pair value;
    struct pair *present = &value;
    struct pair *absent = 0;
    return (!present == 0) + (!absent == 1);
}
EOF

# Category: Bitwise Operations
begin_category "Bitwise Operations" "Testing bitwise shift, AND, OR, XOR operators"

declare -a bitwise_tests=(
    "16 2<<3"
    "32 256>>3"
)

run_expr_tests bitwise_tests
try_output 0 "128 59926 -6 -4 -500283" << EOF
int main() {
  printf("%d %d %d %d %d", 32768 >> 8, 245458999 >> 12, -11 >> 1, -16 >> 2, -1000565 >> 1);
  return 0;
}
EOF
expr 239 "237 | 106"
declare -a more_bitwise_tests=(
    "135 237^106"
    "104 237&106"
)

run_expr_tests more_bitwise_tests

# Category: Return Statements
begin_category "Return Statements" "Testing return statement functionality"

declare -a return_tests=(
    "1 return 1;"
    "42 return 2*21;"
)

run_items_tests return_tests

# Category: Variables and Assignments
begin_category "Variables and Assignments" "Testing variable declarations and assignments"

declare -a variable_tests=(
    "10 int var; var = 10; return var;"
    "42 int va; int vb; va = 11; vb = 31; int vc; vc = va + vb; return vc;"
    "50 int v; v = 30; v = 50; return v;"
    "25 short s; s = 25; return s;"
    "50 short sa = 20; short sb = 30; short sc = sa + sb; return sc;"
)

run_items_tests variable_tests

# A block-scope register declaration has ordinary automatic storage behavior.
try_ 9 << EOF
int main(void) {
    register int value = 4;
    value += 5;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    register int value = 4;
    int *pointer = &value;
    return *pointer;
}
EOF

try_ 12 << EOF
int identity(register int value) { return value; }
int main(void) { return identity(12); }
EOF

try_ 13 << EOF
int increment(int *restrict value) { *value += 1; return *value; }
int main(void) {
    int value = 12;
    int *restrict pointer = &value;
    return increment(pointer);
}
EOF

try_ 14 << EOF
inline int increment_inline(int value) { return value + 1; }
static inline int twice_inline(int value) { return value * 2; }
int inline trailing_inline(int value) { return value - 1; }
int main(void) {
    return twice_inline(increment_inline(6)) + trailing_inline(1);
}
EOF

try_compile_error << EOF
inline int invalid_inline_object;
int main(void) { return 0; }
EOF

try_ 13 << EOF
volatile int global_counter = 11, global_limit = 1;
int increment_volatile(volatile int *counter) { *counter += 1; return *counter; }
int main(void) {
    volatile int local_counter = 12, local_limit = 1;
    for (volatile int count = 0, limit = global_limit; count < limit; count++)
        global_counter += count;
    return increment_volatile(&local_counter) + local_limit - 1;
}
EOF

try_ 1 << EOF
int named_for_func(void) { return __func__[0] == 'n'; }
int main(void) { return named_for_func(); }
EOF

try_ 1 << EOF
int function_name_width(void) { return sizeof __func__ == 20; }
int main(void) {
    int value = 0;
    int *pointer = &value;
    return function_name_width() && sizeof value == 4 && sizeof *pointer == 4;
}
EOF

try_ 13 << EOF
int main(void) {
    return sizeof "cat" + sizeof "a" "bc" + sizeof("tool");
}
EOF

try_ 12 << EOF
int main(void) { return sizeof((int[]){1, 2, 3}); }
EOF

try_ 6 << EOF
int main(void) {
    int value = 6;
    int *restrict direct = (int *restrict)&value;
    int *volatile indirect = (int *volatile)direct;
    return *indirect;
}
EOF

try_ 13 << EOF
int increment(int value) { return value + 1; }
int main(void) {
    int (*restrict callback)(int) = increment;
    return callback(12);
}
EOF

try_compile_error << EOF
int invalid(register int value) { return *(&value); }
int main(void) { return invalid(1); }
EOF

# Narrow signed values must stay negative through promotion and through a store
# and reload. An LP64 backend holds them in a 64-bit register, so a load that
# zero-extends or a promotion that forgets to extend turns a small negative
# number into a large positive one. The array-element form belongs here too, but
# it fails on the Arm backend, whose char elements load zero-extended, so it
# stays in tests/arm64-abi.sh until that is fixed.
try_ 42 << EOF
int main() {
    char c = -5;
    short s = -1000;
    int ci = c;
    int si = s;
    if (ci != -5)
        return 1;
    if (si != -1000)
        return 2;
    if (c >= 0)
        return 3;
    if (s >= 0)
        return 4;
    return 42;
}
EOF

# A pointer is wider than an int on an LP64 target, so testing one for truth has
# to consider the whole value, not just its low word. No fixture can force the
# case that separates the two, since pinning a pointer whose low word is zero
# needs a 64-bit literal and shecc has no integer constant that wide. What is
# testable is that every path which tests an address agrees: the backend emits a
# different width for a branch, for a logical negation and for a comparison, so
# each is reached here with a null and a non-null pointer.
try_ 42 << EOF
struct holder {
    int *ptr;
};

int *pick(int *p, int take)
{
    if (take)
        return p;
    return 0;
}

int main() {
    int v = 42;
    int *p = &v;
    int *n = 0;
    struct holder h;
    int seen = 0;

    if (!p)
        return 1;
    if (n)
        return 2;
    if (p == 0)
        return 3;
    if (n != 0)
        return 4;

    while (n)
        return 5;

    seen = p ? 1 : 0;
    if (!seen)
        return 6;
    seen = n ? 1 : 0;
    if (seen)
        return 7;

    if (p && !n)
        seen = 2;
    if (seen != 2)
        return 8;
    if (n || !p)
        return 9;

    /* A pointer that reaches the test through a return value or a struct
     * field has been through a store and a reload on the way.
     */
    if (!pick(p, 1))
        return 10;
    if (pick(p, 0))
        return 11;

    h.ptr = n;
    if (h.ptr)
        return 12;
    h.ptr = p;
    if (!h.ptr)
        return 13;

    int *q = h.ptr;
    return *q;
}
EOF

# Category: Compound Literals
begin_category "Compound Literals" "Testing C99 compound literal features"

# C99 permits long to use int's representation where both meet the required
# minimum range. Exercise spelling, typedefs, pointer scaling, and ABI slots.
try_ 13 << EOF
typedef long count_t;
typedef long signed signed_count_t;
long add(long left, long int right, signed long extra) {
    return left + right + extra;
}
int main(void) {
    count_t values[3] = {3, 4, 5};
    signed_count_t extra = (long signed)values[2];
    return add(values[0], values[1], extra) +
           (sizeof(long const) == sizeof(int));
}
EOF

if [ "$PTR_SZ" -lt 8 ]; then
    try_compile_error << EOF
long long value;
int main(void) { return 0; }
EOF
fi

# Compound literal support - C90/C99 compliant implementation Basic struct
# compound literals (verified working)
try_compile_error << EOF
int main(void) { return (int){1, 2}; }
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    return (int *){&value, &value} != 0;
}
EOF

try_ 1 << EOF
int main(void) { return (int){1,}; }
EOF

try_ 42 << EOF
typedef struct { int x; int y; } point_t;
int main() {
    point_t p = {42, 100};
    return p.x;
}
EOF

# Typedef record declarations use the shared aggregate initializer path for both
# their first and continuation declarators.
try_ 6 << EOF
typedef struct { int x; int y; int z; } point_t;
int main() {
    point_t first = {1}, second = {2, 3};
    return first.x + first.y + first.z + second.x + second.y + second.z;
}
EOF

# A union initializer selects exactly one member, including through a typedef.
try_compile_error << EOF
typedef union { int a; char b; } value_t;
int main() {
    value_t value = {1, 2};
    return value.a;
}
EOF

try_ 42 << EOF
typedef struct { short x; short y; } point_t;
int main() {
    point_t p = {42, 100};
    return p.x;
}
EOF

try_ 100 << EOF
typedef struct { int x; int y; } point_t;
int main() {
    point_t p = {42, 100};
    return p.y;
}
EOF

try_ 5 << EOF
typedef struct { int x; } s_t;
int main() {
    s_t s = {5};
    return s.x;
}
EOF

# Multi-field struct compound literals
try_ 30 << EOF
struct point { int x; int y; };
int main(void) {
    struct point p = (struct point){10, 20};
    return p.x + p.y;
}
EOF

# C99 aggregate initialization zero-fills all omitted record members.
try_ 0 << EOF
struct point { int x; int y; int z; };
int main(void) {
    struct point p = (struct point){7};
    return p.y != 0 || p.z != 0;
}
EOF

# Explicitly bounded array compound literals support index designators and
# zero-fill the slots those designators skip.
try_ 7 << EOF
int main(void) {
    int *values = (int[4]){[3] = 5, [1] = 2};
    return values[0] + values[1] + values[2] + values[3];
}
EOF

# Array compound literals use the same aggregate-element path as ordinary array
# initializers, including nested braces, omitted members, and element
# designators.
try_ 8 << EOF
struct pair { int first; int second; };
int main(void) {
    struct pair *values = (struct pair[]){ {1, 2}, {3, 4} };
    return values[0].first + values[1].second + values[1].first;
}
EOF

try_ 10 << EOF
struct pair { int first; int second; };
int main(void) {
    struct pair *values = (struct pair[3]){
        [2] = {.second = 9}, [0] = {.first = 1}
    };
    return values[0].first + values[0].second + values[1].first +
           values[1].second + values[2].first + values[2].second;
}
EOF

# An omitted bound is inferred through the largest designator. Reordered
# designators must preserve earlier stores while all untouched records remain
# zero-initialized.
try_ 10 << EOF
struct pair { int first; int second; };
int main(void) {
    struct pair *values = (struct pair[]){
        [3] = {.second = 3}, [1] = {.first = 1}, [4] = {.second = 6}
    };
    return values[0].first + values[0].second + values[1].first +
           values[2].second + values[3].second + values[4].second;
}
EOF

# Nested record braces initialize the nested object before continuing at the
# following outer member.
try_ 12 << EOF
struct pair { int x; int y; };
struct outer { struct pair pair; int tail; };
int main(void) {
    struct outer value = {{2, 3}, 7};
    return value.pair.x + value.pair.y + value.tail;
}
EOF

# Nested array braces initialize the member array before the next outer field.
try_ 12 << EOF
struct outer { int values[2]; int tail; };
int main(void) {
    struct outer value = {{2, 3}, 7};
    return value.values[0] + value.values[1] + value.tail;
}
EOF

try_ 9 << EOF
struct outer { int values[2]; int tail; };
int main(void) {
    struct outer value = {{2}, 7};
    return value.values[0] + value.values[1] + value.tail;
}
EOF

try_ 12 << EOF
struct outer { int values[2]; int tail; };
int main(void) {
    struct outer value = (struct outer){{2, 3}, 7};
    return value.values[0] + value.values[1] + value.tail;
}
EOF

try_ 12 << EOF
struct outer { int values[2]; int tail; };
struct outer value = {{2, 3}, 7};
int main(void) {
    return value.values[0] + value.values[1] + value.tail;
}
EOF

try_ 15 << EOF
struct pair { int x; int y; };
struct outer { struct pair values[2]; int tail; };
int main(void) {
    struct outer value = {{{1, 2}, {3, 4}}, 5};
    return value.values[0].x + value.values[0].y + value.values[1].x +
           value.values[1].y + value.tail;
}
EOF

# Two-dimensional member arrays preserve row braces and then continue with the
# next outer member.
try_ 15 << EOF
struct outer { int values[2][2]; int tail; };
int main(void) {
    struct outer value = {{{1, 2}, {3, 4}}, 5};
    return value.values[0][0] + value.values[0][1] + value.values[1][0] +
           value.values[1][1] + value.tail;
}
EOF

try_ 15 << EOF
struct outer { int values[2][2]; int tail; };
struct outer value = {{{1, 2}, {3, 4}}, 5};
int main(void) {
    return value.values[0][0] + value.values[0][1] + value.values[1][0] +
           value.values[1][1] + value.tail;
}
EOF

try_ 15 << EOF
struct outer { int values[2][2]; int tail; };
int main(void) {
    struct outer value = (struct outer){{{1, 2}, {3, 4}}, 5};
    return value.values[0][0] + value.values[0][1] + value.values[1][0] +
           value.values[1][1] + value.tail;
}
EOF

try_ 12 << EOF
struct pair { int x; int y; };
struct outer { struct pair pair; int tail; };
int main(void) {
    struct outer value = (struct outer){{2, 3}, 7};
    return value.pair.x + value.pair.y + value.tail;
}
EOF

try_ 12 << EOF
struct pair { int x; int y; };
struct outer { struct pair pair; int tail; };
struct outer value = {{2, 3}, 7};
int main(void) {
    return value.pair.x + value.pair.y + value.tail;
}
EOF

# C99 member designators may reorder fields and leave other members zeroed.
try_ 7 << EOF
struct values { int first; int second; int third; };
int main(void) {
    struct values value = {.third = 5, .second = 2};
    return value.first + value.second + value.third;
}
EOF

try_ 7 << EOF
struct values { int first; int second; int third; };
int main(void) {
    struct values value = (struct values){.third = 5, .second = 2};
    return value.first + value.second + value.third;
}
EOF

# Compound literals use the record initializer path for unions as well.
try_ 42 << EOF
union number { int integer; char character; };
int main(void) {
    union number value = (union number){42};
    return value.integer;
}
EOF

# Record assignment copies every byte, not just the scalar slot used by the
# register allocator. Five ints exercise a copy larger than one pointer.
try_ 150 << EOF
struct values { int a; int b; int c; int d; int e; };
int main(void) {
    struct values first = {10, 20, 30, 40, 50};
    struct values second;
    second = first;
    return second.a + second.b + second.c + second.d + second.e;
}
EOF

# A non-word-sized record exercises the byte tail of aggregate copying.
try_ 66 << EOF
struct mixed { int value; char tag; };
int main(void) {
    struct mixed first = {65, 1};
    struct mixed second;
    second = first;
    return second.value + second.tag;
}
EOF

# Record arguments are passed by value: the callee receives every byte, but a
# write to its parameter cannot modify the caller's object.
try_ 250 << EOF
struct values { int a; int b; int c; int d; int e; };
int consume(struct values value) {
    value.a = 100;
    return value.a + value.b + value.c + value.d + value.e;
}
int main(void) {
    struct values source = {10, 20, 30, 40, 50};
    return consume(source) + source.a;
}
EOF

# The aggregate ABI slot must work after all register argument slots are full.
try_ 25 << EOF
struct pair { int first; int second; };
int consume(int a, int b, int c, int d, int e, int f, struct pair value) {
    return a + b + c + d + e + f + value.first + value.second;
}
int main(void) {
    struct pair value = {7, 8};
    return consume(1, 2, 3, 4, 0, 0, value);
}
EOF

try_compile_error << EOF
struct point { int x; int y; };
int main(void) {
    struct point p = (struct point){1, 2, 3};
    return p.x;
}
EOF

try_compile_error << EOF
union number { int integer; char character; };
int main(void) {
    union number value = {1, 2};
    return value.integer;
}
EOF

try_ 5 << EOF
union number { int integer; char character; };
int main(void) {
    union number value = {.character = 5};
    return value.character;
}
EOF

try_compile_error << EOF
union number { int integer; char character; };
int main(void) {
    union number first = {1}, second = {2, 3};
    return first.integer + second.integer;
}
EOF

try_ 30 << EOF
typedef struct { int a; int b; int c; } data_t;
int main() {
    data_t d = {10, 20, 30};
    return d.c;
}
EOF

# Array initialization
try_ 20 << EOF
int main() {
    int arr[3] = {10, 20, 30};
    return arr[1];
}
EOF

# A declared-bound compound literal is an array object that decays to its first
# element when assigned to a pointer.
try_ 12 << EOF
int main(void) {
    int *values = (int[3]){3, 4, 5};
    return values[0] + values[1] + values[2];
}
EOF

# A declared bound remains part of the type: omitted members are zero-filled.
try_ 0 << EOF
int main(void) {
    int *values = (int[4]){3, 4};
    return values[2] != 0 || values[3] != 0;
}
EOF
try_compile_error << EOF
int main(void) {
    int *values = (int[2]){3, 4, 5};
    return values[0];
}
EOF

try_compile_error << EOF
int change(signed const int value) {
    value = 2;
    return value;
}
int main(void) { return change(1); }
EOF

try_compile_error << EOF
typedef const int const_int;
int change(const_int value) {
    value = 2;
    return value;
}
int main(void) { return change(1); }
EOF

# Extended compound literal tests (C99-style brace initialization)

# Additional struct compound literals with different field counts
try_ 12 << EOF
typedef struct { int a; int b; int c; int d; } quad_t;
int main() {
    quad_t q = {3, 4, 5, 0};
    return q.a + q.b + q.c;  /* 3 + 4 + 5 = 12 */
}
EOF

# Array of int initialization
try_ 35 << EOF
int main() {
    int values[4] = {5, 10, 15, 5};
    return values[0] + values[1] + values[2] + values[3];  /* 5 + 10 + 15 + 5 = 35 */
}
EOF

# Array initialization with struct compound literals - Advanced C99 features
# NOTE: These tests document the current implementation status

# Test: Single element array of struct
try_ 10 << EOF
struct point { int x; int y; };
int main() {
    /* Single element struct arrays now work correctly */
    struct point pts[1] = { {10, 20} };
    return pts[0].x;  /* Returns 10 correctly */
}
EOF

# Test: Multi-element array of structs
try_ 1 << EOF
struct point { int x; int y; };
int main() {
    /* Multi-element arrays: first element after index 0 may not initialize correctly */
    struct point pts[2] = { {1, 2}, {3, 4} };
    return pts[0].x;  /* Expected: 1, Actual: 1 (may be coincidental) */
}
EOF

try_ 7 << EOF
struct point { int x; int y; };
int main(void) {
    struct point pts[2] = { {1, 2}, {3, 4} };
    return pts[1].x + pts[1].y;
}
EOF

# Test: Mixed array and struct compound literals
try_ 40 << EOF
struct point { int x; int y; };
int main() {
    /* Verify that regular int arrays still work correctly */
    int arr[3] = {10, 15, 10};

    /* Verify that individual struct initialization still works */
    struct point p = {5, 0};

    return arr[0] + arr[1] + arr[2] + p.x;  /* 10 + 15 + 10 + 5 = 40 */
}
EOF

# Global arrays of structs with compound literals
try_ 9 << EOF
struct global_compound_pair { int first; int second; };
struct global_compound_pair global_compound_value =
    (struct global_compound_pair){.second = 6, .first = 3};
int main(void) {
    return global_compound_value.first + global_compound_value.second;
}
EOF

try_compile_error << EOF
struct global_compound_left { int value; };
struct global_compound_right { int value; };
struct global_compound_left global_compound_mismatch =
    (struct global_compound_right){1};
int main(void) { return 0; }
EOF

try_ 12 << EOF
int global_compound_scalar = (int){12};
int main(void) { return global_compound_scalar; }
EOF

try_compile_error << EOF
char global_compound_wrong_type = (int){1};
int main(void) { return 0; }
EOF

try_ 9 << EOF
int *global_compound_array = (int[]){2, 3, 4};
int main(void) {
    return global_compound_array[0] + global_compound_array[1] +
           global_compound_array[2];
}
EOF

try_ 5 << EOF
int *global_compound_bounded = (int[4]){5};
int main(void) {
    return global_compound_bounded[0] + global_compound_bounded[1] +
           global_compound_bounded[2] + global_compound_bounded[3];
}
EOF

try_ 10 << EOF
struct global_compound_record { int first; int second; };
struct global_compound_record *global_compound_records =
    (struct global_compound_record[]){ {1, 2}, {3, 4} };
int main(void) {
    return global_compound_records[0].first +
           global_compound_records[1].first +
           global_compound_records[1].second +
           global_compound_records[0].second;
}
EOF

try_compile_error << EOF
struct global_compound_array_left { int value; };
struct global_compound_array_right { int value; };
struct global_compound_array_left *global_compound_array_mismatch =
    (struct global_compound_array_right[]){ {1} };
int main(void) { return 0; }
EOF

try_ 60 << EOF
struct nested_compound_point { int x; int y; };
struct nested_compound_value {
    struct nested_compound_point point;
    int z;
};
int main(void) {
    struct nested_compound_value value = (struct nested_compound_value){
        .point = (struct nested_compound_point){10, 20}, .z = 30
    };
    return value.point.x + value.point.y + value.z;
}
EOF

try_ 7 << EOF
struct point { int x; int y; };
struct point gpts1[] = { {3, 4} };
int main() {
    return gpts1[0].x + gpts1[0].y; /* 3 + 4 = 7 */
}
EOF

try_ 7 << EOF
struct point { int x; int y; };
struct point gpts2[2] = { {1, 2}, {3, 4}, };
int main() {
    return gpts2[1].x + gpts2[1].y; /* 3 + 4 = 7 */
}
EOF

try_ 9 << EOF
typedef struct { int x; int y; } point_t;
point_t gpts3[] = { {4, 5} };
int main() {
    return gpts3[0].x + gpts3[0].y; /* 4 + 5 = 9 */
}
EOF

# Enhanced compound literal tests - C99 features with non-standard extensions
# These tests validate both standard C99 compound literals and the non-standard
# behavior required by the test suite (array compound literals in scalar
# contexts)

# Test: Array compound literal assigned to scalar int (non-standard)
try_ 100 << EOF
int main() {
    /* Non-standard: Assigns first element of array to scalar int */
    int x = (int[]){100, 200, 300};
    return x;
}
EOF

# Test: Array compound literal assigned to scalar short (non-standard)
try_ 100 << EOF
int main() {
    /* Non-standard: Assigns first element of array to scalar short */
    short x = (short[]){100, 200, 300};
    return x;
}
EOF

# Test: Array compound literal in arithmetic expression
try_ 150 << EOF
int main() {
    int a = 50;
    /* Non-standard: Uses first element (100) in addition */
    int b = a + (int[]){100, 200};
    return b;
}
EOF

# Test: Array compound literal in arithmetic expression
try_ 150 << EOF
int main() {
    short a = 50;
    /* Non-standard: Uses first element (100) in addition */
    short b = a + (short[]){100, 200};
    return b;
}
EOF

# Test: Mixed scalar and array compound literals
try_ 35 << EOF
int main() {
    /* Scalar compound literals work normally */
    /* Array compound literal contributes its first element (5) */
    return (int){10} + (int){20} + (int[]){5, 15, 25};
}
EOF

# Test: Return statement with array compound literal
try_ 42 << EOF
int main() {
    /* Non-standard: Returns first element of array */
    return (int[]){42, 84, 126};
}
EOF

# Test: Multiple array compound literals in expression
try_ 30 << EOF
int main() {
    /* Both arrays contribute their first elements: 10 + 20 = 30 */
    int result = (int[]){10, 30, 50} + (int[]){20, 40, 60};
    return result;
}
EOF

# Test: Array compound literal with single element
try_ 99 << EOF
int main() {
    int val = (int[]){99};
    return val;
}
EOF

# Test: Array compound literal decay to pointer in initializer
try_ 0 << EOF
int main(void) {
    int *arr = (int[]){1, 2, 3, 4, 5};
    return arr[0] != 1 || arr[4] != 5;
}
EOF

# Test: Passing array compound literal as pointer argument
try_ 0 << EOF
int sum(int *p, int n) {
    int s = 0;
    for (int i = 0; i < n; i++)
        s += p[i];
    return s;
}
int main(void) {
    int s = sum((int[]){1, 2, 3, 0, 0}, 3);
    return s != 6;
}
EOF

# Test: Complex expression with compound literals
try_ 77 << EOF
int main() {
    int a = 7;
    /* (7 * 10) + (100 / 10) - 3 = 70 + 10 - 3 = 77 */
    int b = (a * (int){10}) + ((int[]){100, 200} / 10) - (int[]){3};
    return b;
}
EOF

# Test: Compound literal in conditional expression
try_ 25 << EOF
int main() {
    int flag = 1;
    /* Ternary with compound literals */
    int result = flag ? (int[]){25, 50} : (int){15};
    return result;
}
EOF

# Test: Nested compound literals in function calls
try_ 15 << EOF
int add(int a, int b) {
    return a + b;
}

int main() {
    /* Function arguments with compound literals */
    return add((int){5}, (int[]){10, 20, 30});
}
EOF

# Test: Array compound literal with variable initialization
try_ 60 << EOF
int main() {
    int x = (int[]){10, 20, 30};  /* x = 10 */
    int y = (int[]){20, 40};      /* y = 20 */
    int z = (int[]){30};           /* z = 30 */
    return x + y + z;
}
EOF

# Test: Compound assignment with array compound literal
try_ 125 << EOF
int main() {
    int sum = 25;
    sum += (int[]){100, 200};  /* sum += 100 */
    return sum;
}
EOF

# Test: Array compound literal in loop
try_ 55 << EOF
int main() {
    int sum = 0;
    for (int i = 0; i < 5; i++) {
        /* Each iteration adds 10 (first element) to sum */
        sum += (int[]){10, 20, 30};
    }
    return sum + (int[]){5};  /* 50 + 5 = 55 */
}
EOF

# Test: Scalar compound literals (standard C99)
try_ 42 << EOF
int main() {
    /* Standard scalar compound literals */
    int a = (int){42};
    return a;
}
EOF

# Test: Char compound literals
try_ 65 << EOF
int main() {
    char c = (char){'A'};  /* 'A' = 65 */
    return c;
}
EOF

# File-scope scalar compound literals have static storage duration. They use the
# target object's initializer directly, but retain the C99 one-element
# constraint (with an optional trailing comma).
try_ 42 << EOF
int value = (int){42};
int main() { return value; }
EOF
try_ 7 << EOF
int value = (int){7,};
int main() { return value; }
EOF
try_compile_error << EOF
int invalid = (int){1, 2};
int main() { return 0; }
EOF

# Test: Empty array compound literal (edge case)
try_ 0 << EOF
int main() {
    /* Empty compound literal defaults to 0 */
    int x = (int[]){};
    return x;
}
EOF

# variable with octal literals
items 10 "int var; var = 012; return var;"
items 100 "int var; var = 10 * 012; return var;"
items 32 "int var; var = 0100 / 2; return var;"
items 65 "int var; var = 010 << 3; var += 1; return var;"

# Category: Conditional Statements
begin_category "Conditional Statements" "Testing if/else control flow"

# if
items 5 "if (1) return 5; else return 20;"
items 10 "if (0) return 5; else if (0) return 20; else return 10;"
items 10 "int a; a = 0; int b; b = 0; if (a) b = 10; else if (0) return a; else if (a) return b; else return 10;"
items 27 "int a; a = 15; int b; b = 2; if(a - 15) b = 10; else if (b) return a + b + 10; else if (a) return b; else return 10;"

items 8 "if (1) return 010; else return 11;"
items 10 "int a; a = 012 - 10; int b; b = 0100 - 64; if (a) b = 10; else if (0) return a; else if (a) return b; else return 10;"

# The values on both sides of the select, its condition, and unrelated values
# are all used after the join. This keeps the register file full when the
# allocator has to choose the select result's register.
try_ 30 << EOF
int pick(int a, int b, int c, int d, int e, int f, int g) {
    int selected;
    int hold = g;
    if (a)
        selected = b;
    else
        selected = c;
    return a + b + c + d + e + f + hold + selected;
}

int main() {
    return pick(1, 2, 3, 4, 5, 6, 7);
}
EOF

# Category: Compound Statements
begin_category "Compound Statements" "Testing block scoping and compound statements"

# compound
items 5 "{ return 5; }"
items 10 "{ int a; a = 5; { a = 5 + a; } return a; }"
items 20 "int a; a = 10; if (1) { a = 20; } else { a = 10; } return a;"
items 30 "int a; a = 10; if (a) { if (a - 10) { a = a + 1; } else { a = a + 20; } a = a - 10; } else { a = a + 5; } return a + 10;"

# Category: Loop Constructs
begin_category "Loop Constructs" "Testing while, do-while, and for loops"

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
items 1 "int i = 0; for (;;) { i++; if (i < 4) { continue; } break; } return i == 4;"
items 14 "int n = 0; for (int i = 0;;) { i++; if (i < 14) { continue; } n = i; break; } return n;"
items 14 "int i = 0; for (; i < 20;) { i++; if (i < 14) { continue; } break; } return i;"
items 14 "int n = 0; for (int i = 0; i < 20;) { i++; if (i < 14) { continue; } n = i; break; } return n;"
items 6 "int sum = 0; for (register int i = 1; i < 4; i++) sum += i; return sum;"
items 14 "int i = 0; for (; i < 14;) { i++; } return i;"
items 0 "int i = 0; for (;; i++) { break; } return i;"

# Category: Comments
begin_category "Comments" "Testing C-style and C++-style comment parsing"

# C-style comments / C++-style comments Start
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

# Category: Functions
begin_category "Functions" "Testing function definitions, calls, and recursion"

# `signed` is the existing signed scalar domain; both its explicit and
# omitted-`int` forms are valid declaration specifiers.
try_ 5 << EOF
signed sum(signed int left, signed right)
{
    signed char delta = -1;
    signed short extra = 1;
    return left + right + delta + extra;
}
int main(void)
{
    return sum(2, 3);
}
EOF

try_compile_error << EOF
int main(void)
{
    signed const int first = 1, second = 2;
    second = 3;
    return first + second;
}
EOF

try_ 5 << EOF
int main(void)
{
    signed char value = -1;
    return (signed int)value + (signed)6 + sizeof(signed short) - 2;
}
EOF

try_ 7 << EOF
typedef signed int signed_count_t;
typedef signed char signed_delta_t;
int main(void)
{
    signed_count_t count = 8;
    signed_delta_t delta = -1;
    return count + delta;
}
EOF

# functions
try_ 0 << EOF
int main(void) {
    return 0;
}
EOF

# Named struct/union specifiers are valid parameter declaration specifiers.
try_ 10 << EOF
struct value { int first; };
int first(struct value input) {
    return input.first;
}
int main(void) {
    struct value input = {10};
    return first(input);
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

# Test function with short parameters and return type
try_ 35 << EOF
short add_shorts(short a, short b) {
    return a + b;
}

int main() {
    return add_shorts(15, 20);
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

# Unreachable declaration should not cause prog segmentation fault (prog should
# leave normally with exit code 0)
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

# Category: Pointer Operations
begin_category "Pointer Operations" "Testing pointer declarations, dereferencing, and arithmetic"

# pointers
items 3 "int x; int *y; x = 3; y = &x; return y[0];"
items 5 "int b; int *a; b = 10; a = &b; a[0] = 5; return b;"
items 2 "int x[2]; int y; x[1] = 2; y = *(x + 1); return y;"
items 2 "int x; int *y; int z; z = 2; y = &z; x = *y; return x;"
items 2 "short x; short *y; short z; z = 2; y = &z; x = *y; return x;"

# pointer dereference immediately after declaration
items 42 "int x; x = 10; int *p; p = &x; p[0] = 42; exit(x);"
items 10 "int val; val = 5; int *ptr; ptr = &val; ptr[0] = 10; exit(val);"
items 7 "int a; a = 3; int *b; b = &a; b[0] = 7; exit(a);"

# asterisk dereference for reading after declaration
items 42 "int x; x = 42; int *p; p = &x; int y; y = *p; exit(y);"
items 15 "int val; val = 15; int *ptr; ptr = &val; exit(*ptr);"
items 100 "int a; a = 100; int *b; b = &a; int c; c = *b; exit(c);"

# complex pointer dereference patterns after declaration
try_ 25 << EOF
int main() {
    int x;
    int *p;
    x = 10;
    p = &x;       /* pointer declaration and assignment */
    p[0] = 25;    /* array-style assignment immediately after */
    return x;
}
EOF

try_ 50 << EOF
int main() {
    int arr[3];
    int *ptr;
    arr[0] = 10; arr[1] = 20; arr[2] = 30;
    ptr = arr;
    ptr[0] = 50;  /* should modify arr[0] */
    return arr[0];
}
EOF

try_ 50 << EOF
int main() {
    int a, b;
    int *p1, *p2;
    a = 5; b = 15;
    p1 = &a;
    p2 = &b;
    p1[0] = 100;  /* multiple pointer assignments in same block */
    p2[0] = 200;
    return p1[0] / 2;  /* 100 / 2 = 50 */
}
EOF

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

# typedef pointer tests - testing fixes for typedef pointer compilation issues
# These tests verify typedef pointer functionality after:
# 1. Removing incorrect pointer level inheritance in read_full_var_decl()
# 2. Adding typedef pointer recognition in array indexing operations
# 3. Implementing proper pointer arithmetic scaling for typedef pointers

# Test 1: Basic typedef pointer declaration and dereference
try_ 42 << EOF
typedef int *int_ptr;
int main() {
    int x = 42;
    int_ptr p = &x;
    return *p;  /* Basic dereference - WORKING */
}
EOF

# Test 2: Multiple typedef pointer variables
try_ 55 << EOF
typedef int *int_ptr;
int main() {
    int a = 55, b = 100;
    int_ptr p1 = &a;
    int_ptr p2 = &b;
    return *p1;  /* Should return 55 - WORKING */
}
EOF

# Test 3: Typedef pointer in function parameters
try_ 30 << EOF
typedef int *int_ptr;
int add_via_ptr(int_ptr a, int_ptr b) {
    return *a + *b;
}
int main() {
    int x = 10, y = 20;
    return add_via_ptr(&x, &y);  /* Function call with typedef pointers - WORKING */
}
EOF

# Test 4: Multiple typedef declarations
try_ 7 << EOF
typedef int *int_ptr;
typedef char *char_ptr;
int main() {
    int x = 7;
    char c = 'A';
    int_ptr ip = &x;
    char_ptr cp = &c;
    return *ip;  /* Different typedef pointer types - WORKING */
}
EOF

# Test 5: Global typedef pointer
try_ 88 << EOF
typedef int *int_ptr;
int global_value = 88;
int_ptr global_ptr;
int main() {
    global_ptr = &global_value;
    return *global_ptr;  /* Global typedef pointer - WORKING */
}
EOF

# Test 6: Typedef pointer initialization
try_ 100 << EOF
typedef int *int_ptr;
int main() {
    int val = 100;
    int_ptr p = &val;  /* Initialize at declaration */
    int result = *p;
    return result;  /* Indirect usage - WORKING */
}
EOF

# Test 7: Nested typedef pointer usage in expressions
try_ 15 << EOF
typedef int *int_ptr;
int main() {
    int x = 5, y = 10;
    int_ptr px = &x;
    int_ptr py = &y;
    return *px + *py;  /* Expression with multiple derefs - WORKING */
}
EOF

# Test 8: Typedef pointer assignment after declaration
try_ 25 << EOF
typedef int *int_ptr;
int main() {
    int value = 25;
    int_ptr ptr;
    ptr = &value;  /* Assignment after declaration */
    return *ptr;  /* WORKING */
}
EOF

# Test 9: Typedef pointer array indexing
try_ 100 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {42, 100, 200};
    int_ptr p = values;
    return p[1];  /* Array indexing - NOW WORKING with fix */
}
EOF

# Test 10: Complex array indexing with typedef pointer
try_ 90 << EOF
typedef int *int_ptr;
int main() {
    int arr[5] = {10, 20, 30, 40, 50};
    int_ptr p = arr;
    return p[0] + p[2] + p[4];  /* Multiple array accesses */
}
EOF

# Test 11: Typedef pointer arithmetic - increment
try_ 20 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {10, 20, 30};
    int_ptr p = values;
    p++;  /* Move to next element */
    return *p;  /* Should return 20 */
}
EOF

# Test 12: Typedef pointer arithmetic - addition
try_ 40 << EOF
typedef int *int_ptr;
int main() {
    int values[5] = {10, 20, 30, 40, 50};
    int_ptr p = values;
    p = p + 3;  /* Move forward by 3 elements */
    return *p;  /* Should return 40 */
}
EOF

# Test 13: Typedef pointer arithmetic - subtraction
try_ 30 << EOF
typedef int *int_ptr;
int main() {
    int values[5] = {10, 20, 30, 40, 50};
    int_ptr p = values + 4;  /* Point to last element */
    p = p - 2;  /* Move back by 2 elements */
    return *p;  /* Should return 30 */
}
EOF

# Test 14: Typedef pointer arithmetic - prefix increment
try_ 20 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {10, 20, 30};
    int_ptr p = values;
    ++p;  /* Prefix increment */
    return *p;  /* Should return 20 */
}
EOF

# Test 15: Typedef pointer arithmetic - postfix increment
try_ 10 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {10, 20, 30};
    int_ptr p = values;
    int val = *p++;  /* Get value, then increment */
    return val;  /* Should return 10 */
}
EOF

# Test 16: Typedef pointer arithmetic - decrement
try_ 20 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {10, 20, 30};
    int_ptr p = values + 2;  /* Point to values[2] */
    p--;  /* Move back one element */
    return *p;  /* Should return 20 */
}
EOF

# Test 17: Typedef char pointer arithmetic
try_ 98 << EOF
typedef char *char_ptr;
int main() {
    char chars[5] = {'a', 'b', 'c', 'd', 'e'};
    char_ptr p = chars;
    p = p + 1;  /* Move forward by 1 byte */
    return *p;  /* Should return 'b' = 98 */
}
EOF

# Test 18: Mixed typedef pointer operations
try_ 35 << EOF
typedef int *int_ptr;
int main() {
    int values[10] = {5, 10, 15, 20, 25, 30, 35, 40, 45, 50};
    int_ptr p = values;
    p = p + 2;  /* Move to values[2] = 15 */
    p++;        /* Move to values[3] = 20 */
    p = p + 3;  /* Move to values[6] = 35 */
    return *p;
}
EOF

# Pointer difference calculations Test basic pointer subtraction returning
# element count
try_ 5 << EOF
int main() {
    char arr[10];
    char *p = arr;
    char *q = arr + 5;
    int diff = q - p;  /* Should return 5 (5 elements) */
    return diff;
}
EOF

try_ 3 << EOF
int main() {
    char str[20];
    char *start = str + 2;
    char *end = str + 5;
    return end - start;  /* Should return 3 */
}
EOF

# Test pointer difference with char pointers (element size = 1)
try_ 7 << EOF
int main() {
    char buffer[100];
    char *p1 = buffer;
    char *p2 = buffer + 7;
    return p2 - p1;  /* Should return 7 */
}
EOF

# Test reverse pointer difference
try_ 5 << EOF
int main() {
    char data[50];
    char *high = data + 10;
    char *low = data + 5;
    return high - low;  /* Should return 5 */
}
EOF

# C99 6.5.6: pointer subtraction yields an element count, not a byte count.
try_ 5 << EOF
int main(void) {
    int values[10];
    return &values[7] - &values[2];
}
EOF

try_ 5 << EOF
struct point { int x; int y; };
int main(void) {
    struct point values[10];
    return &values[8] - &values[3];
}
EOF

# C99 6.5.6 requires the two pointer operands to point at compatible types.
try_compile_error << EOF
int main(void) {
    int words[2];
    char bytes[2];
    return &words[1] - &bytes[1];
}
EOF

try_compile_error << EOF
struct first { int value; };
struct second { int value; };
int main(void) {
    struct first left[2];
    struct second right[2];
    return &left[1] - &right[1];
}
EOF

# C99 6.5.6 permits subtraction only for pointers to complete object types.
try_compile_error << EOF
int main(void) {
    void *left = 0;
    void *right = 0;
    return left - right;
}
EOF
try_compile_error << EOF
int first(void) { return 1; }
int second(void) { return 2; }
int main(void) {
    int (*left)(void) = first;
    int (*right)(void) = second;
    return left - right;
}
EOF
try_compile_error << EOF
int callback(void) { return 1; }
int main(void) {
    int (*pointer)(void) = callback;
    return pointer + 1;
}
EOF

try_ 1 << EOF
typedef int *int_pointer;
int main(void) {
    int values[2];
    int_pointer typed = &values[1];
    int *spelled = &values[0];
    return typed - spelled;
}
EOF

# Pointer arithmetic tests

# Basic integer pointer difference
try_ 7 << EOF
int main() {
    int arr[10];
    int *p = arr;
    int *q = arr + 7;
    return q - p;
}
EOF

# Char pointer differences
try_ 10 << EOF
int main() {
    char text[50];
    char *start = text;
    char *end = text + 10;
    return end - start;
}
EOF

try_ 0 << EOF
int main() {
    char buffer[100];
    char *p1 = buffer + 25;
    char *p2 = buffer + 25;
    return p2 - p1;  /* Same position = 0 */
}
EOF

# More complex char pointer arithmetic
try_ 15 << EOF
int main() {
    char str[100];
    char *p = str + 5;
    char *q = str + 20;
    return q - p;  /* 20 - 5 = 15 */
}
EOF

# C99 does not define arithmetic on void pointers.
try_compile_error << EOF
int main() {
    char array[20];
    void *vp1 = array;
    return vp1 + 8;
}
EOF

try_compile_error << EOF
int main() {
    char array[20];
    void *vp1 = array;
    return vp1 - 1;
}
EOF

try_compile_error << EOF
int main() {
    char array[20];
    void *vp1 = array;
    vp1++;
    return 0;
}
EOF

try_compile_error << EOF
int main() {
    char array[20];
    void *vp1 = array;
    ++vp1;
    return 0;
}
EOF

try_compile_error << EOF
int main() {
    char array[20];
    void *vp1 = array;
    vp1 += 1;
    return 0;
}
EOF

# A pointer-to-void-pointer advances over pointer objects, so it remains valid.
try_ 1 << EOF
int main() {
    void *values[2];
    void **p = values;
    p += 1;
    return (char *)p - (char *)values == sizeof(void *);
}
EOF

# An array of pointers decays to a pointer-to-pointer. Its stride is a pointer
# object, not the size of the pointee base type.
try_ 1 << EOF
int main() {
    int *values[2];
    return (char *)(values + 1) - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
int main() {
    int *values[3];
    return (values + 2) - (values + 1);
}
EOF

# Integer pointer with array indexing
try_ 3 << EOF
int main() {
    int nums[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int *first = &nums[2];
    int *second = &nums[5];
    return second - first;  /* Direct subtraction: (5-2) = 3 */
}
EOF

# Larger integer pointer difference
try_ 10 << EOF
int main() {
    int values[20];
    int *p = values;
    int *q = values + 10;
    return q - p;  /* Direct pointer arithmetic */
}
EOF

# Negative pointer difference
try_ 251 << EOF
int main() {
    int arr[10];
    int *p = arr + 8;
    int *q = arr + 3;
    return q - p;  /* 3 - 8 = -5, wraps to 251 in exit code */
}
EOF

# Zero pointer difference
try_ 0 << EOF
int main() {
    int data[10];
    int *p1 = data + 5;
    int *p2 = data + 5;
    return p2 - p1;  /* Same position = 0 */
}
EOF

# Struct pointer arithmetic
try_ 4 << EOF
struct point {
    int x;
    int y;
    int z;
};

int main() {
    struct point pts[10];
    struct point *p1 = pts;
    struct point *p2 = pts + 4;
    return p2 - p1;  /* Struct pointer difference */
}
EOF

# Mixed pointer arithmetic operations
try_ 16 << EOF
int main() {
    int arr[20];
    int *start = arr;
    int *mid = arr + 10;
    int *end = arr + 18;
    return (end - mid) + (mid - start) - 2;  /* (18-10) + (10-0) - 2 = 8 + 10 - 2 = 16 */
}
EOF

# Pointer arithmetic with typedef
try_ 6 << EOF
typedef int* int_ptr;
int main() {
    int data[15];
    int_ptr p1 = data + 2;
    int_ptr p2 = data + 8;
    return p2 - p1;  /* Typedef pointer difference: 8 - 2 = 6 */
}
EOF

# Complex expression with pointer differences
try_ 13 << EOF
int main() {
    int vals[30];
    int *a = vals;
    int *b = vals + 5;
    int *c = vals + 9;
    int *d = vals + 15;
    return (d - a) - (c - b) + 2;  /* (15-0) - (9-5) + 2 = 15 - 4 + 2 = 13 */
}
EOF

# Test negative pointer difference (converted to exit code)
try_ 253 << EOF
int main() {
    char data[20];
    char *high = data + 5;
    char *low = data + 8;
    int diff = high - low;  /* -3 */
    /* Convert negative to positive for exit code */
    return diff < 0 ? 256 + diff : diff;  /* Returns 253 (256-3) */
}
EOF

# Test short pointer
try_ 150 << EOF
int main() {
    short value = 150;
    short *ptr = &value;
    return *ptr;
}
EOF

# Test short pointer arithmetic
try_ 20 << EOF
int main() {
    short arr[3] = {10, 20, 30};
    short *p = arr;
    p++;
    return *p;
}
EOF

# Test short pointer difference
try_ 2 << EOF
int main() {
    short data[5] = {1, 2, 3, 4, 5};
    short *start = data + 1;
    short *end = data + 3;
    return end - start;
}
EOF

# Category: Function Pointers
begin_category "Function Pointers" "Testing function pointer declarations and calls"

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

# Local function pointer, direct struct member, and pointer-to-struct member.
# The first path must use the pointer value directly; the latter two must load
# the pointer from the member slot.
try_ 6 << EOF
typedef struct {
    int (*fn)(int);
} holder_t;

int suc(int x) { return x + 1; }

int main() {
    int (*local)(int);
    holder_t h;
    holder_t *p = &h;

    local = suc;
    h.fn = suc;
    p->fn = suc;

    return local(1) + h.fn(1) + p->fn(1);
}
EOF

# Assignment between function-pointer variables copies the stored function
# address; it must not treat the RHS variable name as a function symbol.
try_ 5 << EOF
int suc(int x) { return x + 1; }

int main() {
    int (*first)(int);
    int (*second)(int);

    first = suc;
    second = first;
    return second(4);
}
EOF

# A local function pointer shadows a global function. Copying it must load the
# local variable's stored target, rather than materializing the global
# function's address.
try_ 9 << EOF
int target(int x) { return x + 3; }
int replacement(int x) { return x + 8; }

int main() {
    int (*target)(int);
    int (*copy)(int);

    target = replacement;
    copy = target;
    return copy(1);
}
EOF

# An indirect call with two arguments must not leave stale argument-register
# mappings visible to a later one-argument call.
try_ 155 << EOF
typedef struct {
    int (*add)(int, int);
} pair_holder_t;

int add(int a, int b) { return a + b; }
int one(int x) { return x + 100; }
int get_right() { return 20; }

int main() {
    pair_holder_t h;
    int left = 10;
    int right = get_right();
    h.add = add;
    return h.add(left, right) + one(5) + right;
}
EOF

# An indirect call must retain the prototype parsed for its function-pointer
# declaration so a record parameter is copied and passed by value, just as it is
# for a direct call.
try_ 42 << EOF
struct pair { int left; int right; };
int total(struct pair p) { p.left = 30; return p.left + p.right; }
int main() {
    struct pair p = {3, 12};
    int (*fn)(struct pair) = total;
    return fn(p) == 42 && p.left == 3 ? 42 : 1;
}
EOF

# The same prototype metadata belongs to a function-pointer member, rather than
# only to a standalone local declaration.
try_ 42 << EOF
struct pair { int left; int right; };
struct holder { int (*fn)(struct pair); };
int total(struct pair p) { p.right = 12; return p.left + p.right; }
int main() {
    struct pair p = {30, 3};
    struct holder h;
    h.fn = total;
    return h.fn(p) == 42 && p.right == 3 ? 42 : 1;
}
EOF

# Addressing a pointer to a function-pointer aggregate must return the pointer
# variable's address, not backing storage for its pointee.
try_ 5 << EOF
typedef struct {
    int (*fn)(int);
} holder_t;

int suc(int x) { return x + 1; }

int call(holder_t *direct) {
    holder_t **indirect = &direct;
    return direct == *indirect ? direct->fn(4) : 1;
}

int main() {
    holder_t h;
    h.fn = suc;
    return call(&h);
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

# Category: Arrays
begin_category "Arrays" "Testing array declarations, indexing, and operations"

# Array element reads preserve the declared signed width. This covers both
# local-address and indexed OP_read lowering on every target.
try_ 42 << EOF
int main(void) {
    char bytes[4];
    short halves[4];
    int i;
    for (i = 0; i < 4; i++) {
        bytes[i] = -1 - i;
        halves[i] = -1000 - i;
    }
    for (i = 0; i < 4; i++) {
        if (bytes[i] != -1 - i)
            return 1;
        if (halves[i] != -1000 - i)
            return 2;
    }
    return 42;
}
EOF

try_compile_error << EOF
int main(void)
{
    int values[2] = {1, 2, 3};
    return values[0];
}
EOF

# C99 array designators may initialize sparse slots in either order; omitted
# elements retain the aggregate's implicit zero initialization.
try_ 7 << EOF
int main(void)
{
    int values[4] = {[3] = 5, [1] = 2};
    return values[0] + values[1] + values[2] + values[3];
}
EOF

# Array element initializers dispatch through the same record path for unions.
try_ 30 << EOF
union value { int integer; char character; };
int main(void)
{
    union value values[2] = {{10}, {20}};
    return values[0].integer + values[1].integer;
}
EOF

# a parameter whose first dimension is omitted is still a 2-D array: "int
# a[][4]" must index exactly like "int a[3][4]", not like "int **"
try_ 66 << EOF
int sum2(int a[][4], int rows)
{
    int t = 0;
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < 4; j++)
            t += a[i][j];
    return t;
}
int main()
{
    int m[3][4];
    int c = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            m[i][j] = c;
            c++;
        }
    return sum2(m, 3);
}
EOF

# the sized form keeps working, and both agree
try_ 66 << EOF
int sum2(int a[3][4], int rows)
{
    int t = 0;
    for (int i = 0; i < rows; i++)
        for (int j = 0; j < 4; j++)
            t += a[i][j];
    return t;
}
int main()
{
    int m[3][4];
    int c = 0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++) {
            m[i][j] = c;
            c++;
        }
    return sum2(m, 3);
}
EOF

# a single omitted dimension is still a plain pointer
try_ 66 << EOF
int sum1(int a[], int n)
{
    int t = 0;
    for (int i = 0; i < n; i++)
        t += a[i];
    return t;
}
int main()
{
    int m[12];
    for (int i = 0; i < 12; i++)
        m[i] = i;
    return sum1(m, 12);
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

# Test short array
try_ 25 << EOF
int main() {
    short arr[4] = {10, 15, 20, 25};
    return arr[3];
}
EOF

# 2D Array Tests with proper row-major indexing for multi-dimensional arrays
try_ 78 << EOF
int main() {
    int matrix[3][4];
    int sum = 0;
    int i, j;

    /* Initialize array */
    for (i = 0; i < 3; i = i + 1) {
        for (j = 0; j < 4; j = j + 1) {
            matrix[i][j] = i * 4 + j + 1;
        }
    }

    /* Calculate sum (1+2+...+12 = 78) */
    for (i = 0; i < 3; i = i + 1) {
        for (j = 0; j < 4; j = j + 1) {
            sum = sum + matrix[i][j];
        }
    }

    return sum;
}
EOF

# 2D array element access in expressions
try_ 17 << EOF
int main() {
    int grid[2][3];

    grid[0][0] = 5;
    grid[0][1] = 10;
    grid[0][2] = 15;
    grid[1][0] = 20;
    grid[1][1] = 25;
    grid[1][2] = 30;

    /* Test complex expression with 2D array elements */
    return (grid[0][1] + grid[0][2]) / 2 + grid[1][0] / 4; /* (10+15)/2 + 20/4 = 12 + 5 = 17 */
}
EOF

# Actually fix the calculation error above - should return 17, not 25
try_ 17 << EOF
int main() {
    int grid[2][3];

    grid[0][0] = 5;
    grid[0][1] = 10;
    grid[0][2] = 15;
    grid[1][0] = 20;
    grid[1][1] = 25;
    grid[1][2] = 30;

    /* Test complex expression with 2D array elements */
    return (grid[0][1] + grid[0][2]) / 2 + grid[1][0] / 4; /* (10+15)/2 + 20/4 = 12 + 5 = 17 */
}
EOF

# 2D array as multiplication table
try_ 30 << EOF
int main() {
    int table[5][6];
    int i, j;

    /* Create multiplication table */
    for (i = 0; i < 5; i = i + 1) {
        for (j = 0; j < 6; j = j + 1) {
            table[i][j] = (i + 1) * (j + 1);
        }
    }

    /* Check specific values and return 5*6 = 30 */
    if (table[2][3] != 12) return 1;  /* 3*4 = 12 */
    if (table[4][5] != 30) return 2;  /* 5*6 = 30 */

    return table[4][5];
}
EOF

# 2D array with single row/column
try_ 12 << EOF
int main() {
    int row[1][5];
    int col[5][1];
    int i;

    /* Initialize single row array */
    for (i = 0; i < 5; i = i + 1) {
        row[0][i] = i + 1;
    }

    /* Initialize single column array */
    for (i = 0; i < 5; i = i + 1) {
        col[i][0] = i + 1;
    }

    return row[0][2] + col[3][0] + row[0][4]; /* 3 + 4 + 5 = 12 */
}
EOF

# Fix the test above - the comment was wrong
try_ 12 << EOF
int main() {
    int row[1][5];
    int col[5][1];
    int i;

    /* Initialize single row array */
    for (i = 0; i < 5; i = i + 1) {
        row[0][i] = i + 1;
    }

    /* Initialize single column array */
    for (i = 0; i < 5; i = i + 1) {
        col[i][0] = i + 1;
    }

    return row[0][2] + col[3][0] + row[0][4]; /* 3 + 4 + 5 = 12 */
}
EOF

# 2D array of structs
try_ 42 << EOF
typedef struct {
    int x;
    int y;
} Point;

int main() {
    Point grid[2][2];

    grid[0][0].x = 1;
    grid[0][0].y = 2;
    grid[0][1].x = 3;
    grid[0][1].y = 4;
    grid[1][0].x = 5;
    grid[1][0].y = 6;
    grid[1][1].x = 7;
    grid[1][1].y = 8;

    /* Sum all x values: 1 + 3 + 5 + 7 = 16 */
    /* Sum all y values: 2 + 4 + 6 + 8 = 20 */
    /* Return total of x[1][1] * y[1][0] = 7 * 6 = 42 */
    return grid[1][1].x * grid[1][0].y;
}
EOF

# 2D char array (string array simulation)
try_ 65 << EOF
int main() {
    char letters[3][3];

    /* Store letters A-I in 3x3 grid */
    letters[0][0] = 'A';  /* 65 */
    letters[0][1] = 'B';
    letters[0][2] = 'C';
    letters[1][0] = 'D';
    letters[1][1] = 'E';
    letters[1][2] = 'F';
    letters[2][0] = 'G';
    letters[2][1] = 'H';
    letters[2][2] = 'I';

    /* Return the first letter */
    return letters[0][0];
}
EOF

# 2D array boundary test
try_ 100 << EOF
int main() {
    int data[10][10];
    int i, j;

    /* Initialize entire array */
    for (i = 0; i < 10; i = i + 1) {
        for (j = 0; j < 10; j = j + 1) {
            data[i][j] = i * 10 + j;
        }
    }

    /* Check corner values */
    if (data[0][0] != 0) return 1;
    if (data[9][9] != 99) return 2;
    if (data[5][5] != 55) return 3;

    /* Return sum of corners: 0 + 9 + 90 + 99 = 198 - wait let me recalculate */
    /* Actually the test says return 100, let's just return data[9][9] + 1 */
    return data[9][9] + 1;
}
EOF

# Mixed subscript and arrow / dot operators, excerpted and modified from issue
# #165
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

# Category: Global Variables
begin_category "Global Variables" "Testing global variable initialization and access"

# global initialization
try_ 20 << EOF
int a = 5 * 2;
int b = -4 * 3 + 7 + 9 / 3 * 5;
int main()
{
    return a + b;
}
EOF

# File-scope extern declarations share the later object's or function's
# definition and preserve its ordinary external linkage.
try_ 17 << EOF
extern int external_value;
extern int external_function(void);
int external_value = 10;
int external_function(void) { return 7; }
int main(void) { return external_value + external_function(); }
EOF

# A block-scope extern declaration has no automatic storage and hides an
# enclosing local name while referring to the translation unit's object or
# function declaration, including one defined later in the file.
try_ 18 << EOF
int main(void) {
    int later_value = 99;
    {
        extern int later_value;
        extern int later_extra, later_value;
        extern int later_function(void);
        later_value += later_extra + later_function();
        return later_value;
    }
}
int later_function(void) { return 8; }
int later_value = 10;
int later_extra = 0;
EOF

try_ 8 << EOF
int hidden_function(void) { return 8; }
int main(void) {
    int hidden_function = 0;
    {
        extern int hidden_function(void);
        return hidden_function();
    }
}
EOF

# C99 permits a string literal to initialize a character-array member without an
# extra brace level, for automatic and file-scope record objects.
try_ 15 << EOF
struct named_text { char text[6]; int tag; } global_text = {"hi", 7};
int main(void) {
    struct named_text local_text = {"ok", 9};
    return global_text.text[0] + global_text.text[1] + global_text.text[2] +
           global_text.tag + local_text.text[0] + local_text.text[1] +
           local_text.text[2] + local_text.tag - 428;
}
EOF

# A declaration in a C99 for initializer has block scope too. Its extern object
# and function forms must bind later file-scope definitions and hide an
# enclosing automatic object for the whole loop.
try_ 4 << EOF
int main(void) {
    int for_value = 99;
    for (extern int for_value; for_value == 0; for_value++)
        return for_value + 4;
    return 0;
}
int for_value = 0;
EOF

try_ 12 << EOF
int main(void) {
    for (extern int for_function(void); 1; )
        return for_function();
}
int for_function(void) { return 12; }
EOF

# Nested record-member designators select the resolved leaf, rather than only
# the outer record member, in local and static-storage initializers.
try_ 24 << EOF
struct nested_leaf { int first; int second; };
struct nested_outer { int prefix; struct nested_leaf inner; int suffix; };
struct nested_outer global_nested = {.inner.second = 7, .suffix = 5};
int main(void) {
    struct nested_outer local_nested = {.inner.first = 3, .inner.second = 4,
                                        .suffix = 5};
    return global_nested.inner.first + global_nested.inner.second +
           global_nested.suffix + local_nested.inner.first +
           local_nested.inner.second + local_nested.suffix;
}
EOF

# An array-member designator writes one selected element, including when the
# array is itself reached through a nested record member.
try_ 32 << EOF
struct array_inner { int items[4]; };
struct array_outer { int prefix; struct array_inner inner; int suffix; };
struct array_outer global_array = {.inner.items[2] = 7, .suffix = 5};
int main(void) {
    struct array_outer local_array = {.inner.items[1] = 9,
                                      .inner.items[3] = 11};
    return global_array.inner.items[0] + global_array.inner.items[2] +
           global_array.suffix + local_array.inner.items[1] +
           local_array.inner.items[3];
}
EOF

# A two-dimensional member designator carries the stored row stride, while a
# one-subscript designator names a row that can receive a braced initializer.
try_ 22 << EOF
struct matrix_inner { int cells[2][3]; };
struct matrix_outer { struct matrix_inner inner; int tag; };
struct matrix_outer global_matrix = {.inner.cells[1][2] = 7, .tag = 3};
int main(void) {
    struct matrix_outer local_matrix = {.inner.cells[0] = {4, 5, 6},
                                        .inner.cells[1][1] = 2};
    return global_matrix.inner.cells[1][2] + global_matrix.tag +
           local_matrix.inner.cells[0][0] + local_matrix.inner.cells[0][2] +
           local_matrix.inner.cells[1][1];
}
EOF

# Following positional initializers continue from a one-dimensional designated
# array element before advancing to the next record member.
try_ 30 << EOF
struct continued_array { int items[4]; int tail; } global_continue =
    {.items[1] = 4, 5, 6};
int main(void) {
    struct continued_array local_continue = {.items[2] = 7, 8};
    return global_continue.items[0] + global_continue.items[1] +
           global_continue.items[2] + global_continue.items[3] +
           global_continue.tail + local_continue.items[2] +
           local_continue.items[3] + local_continue.tail;
}
EOF

# Positional values after a two-dimensional member leaf advance in row-major
# order through the remaining elements.
try_ 30 << EOF
struct continued_matrix { int cells[2][3]; } global_matrix_continue =
    {.cells[0][1] = 4, 5, 6, 7, 8};
int main(void) {
    return global_matrix_continue.cells[0][0] +
           global_matrix_continue.cells[0][1] +
           global_matrix_continue.cells[0][2] +
           global_matrix_continue.cells[1][0] +
           global_matrix_continue.cells[1][1] +
           global_matrix_continue.cells[1][2];
}
EOF

# Translation phase 6 also concatenates literals made adjacent by macro
# expansion, before the expression parser sees them.
try_ 3 << EOF
#define STRING_LEFT "ab"
#define STRING_RIGHT "cd"
int main(void) {
    char *text = STRING_LEFT STRING_RIGHT;
    return (text[0] == 'a') + (text[2] == 'c') + (text[4] == 0);
}
EOF

# A declaration's base type applies to every global declarator, while each
# declarator keeps its own pointer and array modifiers and initializer.
try_ 39 << EOF
typedef int myint;
struct pair { int x; int y; } first, second, *selected;
union choice { int number; char letter; } chosen, *chosen_ptr;
int plain = 3, *pointer, array[2];
char *left = "A", *right = "B";
myint alpha = 4, beta = 5;

int main(void)
{
    pointer = &plain;
    first.x = 6;
    first.y = 7;
    second.x = 8;
    second.y = 9;
    selected = &second;
    chosen.number = 10;
    chosen_ptr = &chosen;
    array[0] = first.x;
    array[1] = selected->y;
    return *pointer + array[0] + array[1] + chosen_ptr->number + alpha + beta +
           (left != 0) + (right != 0);
}
EOF

# Unsigned scalar declarations preserve their storage widths. unsigned char and
# unsigned short promote to int in arithmetic, while unsigned int remains
# unsigned through the expression pipeline.
try_ 26 << EOF
unsigned int global_value = 1000;
int main(void)
{
    unsigned char byte = 20;
    unsigned short half = 30;
    unsigned int word = global_value;
    return byte + half + word;
}
EOF

# Static declarations have static storage duration. The local counter must be
# initialized once in the synthetic global frame, while its name stays scoped to
# next_value().
try_ 19 << EOF
const static int file_value = 4;
static int next_value(void)
{
    static int counter = 7;
    const static int bias = 0;
    return counter++ + bias;
}
int main(void)
{
    return file_value + next_value() + next_value();
}
EOF

# Continuation declarators retain their individual pointer, array, and function
# pointer forms while sharing the base type and processing each initializer.
try_ 13 << EOF
int add_one(int value) { return value + 1; }
int value = 4, *value_ref = &value, values[2] = {3, 5},
    (*apply)(int) = add_one;
int main(void) { return *value_ref + values[1] + apply(values[0]); }
EOF

try_ 5 << EOF
int static_address_constants(void)
{
    static int values[3] = {1, 2, 3};
    static int *decayed = values;
    static int *addressed = &values[0];
    return decayed[1] + addressed[2];
}
int main(void) { return static_address_constants(); }
EOF

# A block-scope aggregate static must keep both its initializer and later member
# writes across calls; this covers the global-frame lowering beyond the scalar
# counter above.
try_ 12 << EOF
struct tally { int count; int values[2]; };
int next_tally(void)
{
    static struct tally state = {1, {2, 3}};
    state.count++;
    state.values[0]++;
    return state.count + state.values[0];
}
int main(void) { return next_tally() + next_tally(); }
EOF

try_ 1 << EOF
int writable_static_string(void)
{
    static char buffer[8] = "hi";
    buffer[0] = 'H';
    return buffer[0] == 'H' && buffer[1] == 'i' && buffer[2] == 0;
}
int main(void) { return writable_static_string(); }
EOF

try_ 1 << EOF
static char inferred_file_scope_string[] = "map";

int inferred_static_string(void)
{
    static char buffer[] = "cat";
    buffer[0] = 'C';
    return buffer[0] == 'C' && buffer[1] == 'a' && buffer[2] == 't' &&
           sizeof(buffer) == 4;
}
int inferred_for_string(void)
{
    int n = 0;
    for (char word[] = "go"; word[n]; n++)
        ;
    return n == 2;
}
int inferred_for_array(void)
{
    int total = 0;
    for (int values[] = {2, 3}; values[0] && sizeof(values) == 8;
         values[0] = 0)
        total = values[0] + values[1];
    return total == 5;
}
int inferred_file_scope_string_test(void)
{
    inferred_file_scope_string[0] = 'M';
    return inferred_file_scope_string[0] == 'M' &&
           inferred_file_scope_string[1] == 'a' &&
           sizeof(inferred_file_scope_string) == 4;
}
int main(void)
{
    return inferred_static_string() && inferred_for_string() &&
           inferred_for_array() && inferred_file_scope_string_test();
}
EOF

try_ 10 << EOF
static int triangular(int value)
{
    return value ? value + triangular(value - 1) : 0;
}
int main(void) { return triangular(4); }
EOF

try_ 3 << EOF
int next_zeroed(void)
{
    static int count;
    return count++;
}
int main(void)
{
    return next_zeroed() + next_zeroed() + next_zeroed();
}
EOF

try_ 1 << EOF
int zeroed_entry(void)
{
    static int entries[2];
    entries[1]++;
    return entries[0] == 0;
}
int main(void) { return zeroed_entry(); }
EOF

# Global-storage declarators are temporarily placed on the expression stack for
# constant initialization. Aggregate and zero initializers must discard their
# own entries too, or a declaration-heavy block exhausts that stack.
try_ 33 << EOF
int many_static_aggregates(void)
{
    static int slot00[1] = {0};
    static int slot01[1] = {1};
    static int slot02[1] = {2};
    static int slot03[1] = {3};
    static int slot04[1] = {4};
    static int slot05[1] = {5};
    static int slot06[1] = {6};
    static int slot07[1] = {7};
    static int slot08[1] = {8};
    static int slot09[1] = {9};
    static int slot10[1] = {10};
    static int slot11[1] = {11};
    static int slot12[1] = {12};
    static int slot13[1] = {13};
    static int slot14[1] = {14};
    static int slot15[1] = {15};
    static int slot16[1] = {16};
    static int slot17[1] = {17};
    static int slot18[1] = {18};
    static int slot19[1] = {19};
    static int slot20[1] = {20};
    static int slot21[1] = {21};
    static int slot22[1] = {22};
    static int slot23[1] = {23};
    static int slot24[1] = {24};
    static int slot25[1] = {25};
    static int slot26[1] = {26};
    static int slot27[1] = {27};
    static int slot28[1] = {28};
    static int slot29[1] = {29};
    static int slot30[1] = {30};
    static int slot31[1] = {31};
    static int slot32[1] = {32};
    return slot00[0] + slot32[0] + slot01[0];
}
int main(void) { return many_static_aggregates(); }
EOF

try_ 1 << EOF
struct zeroed_pair { int first; int second; };
int zeroed_record(void)
{
    static struct zeroed_pair pair;
    pair.second = 1;
    return pair.first == 0;
}
int main(void) { return zeroed_record(); }
EOF

# A block-scope static initializer is lowered as global data and therefore must
# be a C99 constant expression, not a run-time call.
try_compile_error << EOF
int runtime_value(void) { return 7; }
int main(void)
{
    static int value = runtime_value();
    return value;
}
EOF

try_ 13 << EOF
struct static_compound_pair { int left; int right; };
int static_compound_literals(void)
{
    static int scalar = (int){3};
    static int *values = (int[]){4, 5};
    static struct static_compound_pair pair =
        (struct static_compound_pair){2, 3};
    return scalar + values[1] + pair.left + pair.right;
}
int main(void) { return static_compound_literals(); }
EOF

try_compile_error << EOF
int counter;
int main(void)
{
    static counter = 5;
    return counter;
}
EOF

try_compile_error << EOF
int main(void)
{
    static ++missing;
    return 0;
}
EOF

# A block-scope static array has global storage duration but retains block
# scope. Its constant initializer must be emitted with global data.
try_ 15 << EOF
int values(void)
{
    static int entries[3] = {4, 5, 6};
    return entries[0] + entries[1] + entries[2];
}
int main(void)
{
    return values();
}
EOF

try_ 10 << EOF
int values(void)
{
    static int left[2] = {1, 2}, right[2] = {3, 4};
    return left[0] + left[1] + right[0] + right[1];
}
int main(void)
{
    return values();
}
EOF

try_ 17 << EOF
struct static_pair { int first; int second; };
static struct static_pair global_pair = {8, 9};
int main(void)
{
    return global_pair.first + global_pair.second;
}
EOF

# File-scope member designators may reorder fields; omitted fields are zero.
try_ 7 << EOF
struct designated_values { int first; int second; int third; };
struct designated_values values = {.third = 5, .second = 2};
int main(void)
{
    return values.first + values.second + values.third;
}
EOF

# Keep byte-wide designated stores from clobbering an adjacent member.
try_ 7 << EOF
struct byte_designated_values { char first; char second; int third; };
struct byte_designated_values byte_values = {.third = 5, .second = 2};
int main(void)
{
    return byte_values.first + byte_values.second + byte_values.third;
}
EOF

# Bounded array designators use the same global initializer lowering.
try_ 9 << EOF
int designated_entries[4] = {[3] = 7, [1] = 2};
int main(void)
{
    return designated_entries[0] + designated_entries[1] +
           designated_entries[2] + designated_entries[3];
}
EOF

# An omitted bound is one past the highest designated element.
try_ 9 << EOF
int inferred_entries[] = {[3] = 7, [1] = 2};
int main(void)
{
    return inferred_entries[0] + inferred_entries[1] +
           inferred_entries[2] + inferred_entries[3];
}
EOF

# The same lowering serves block-scope static records.
try_ 12 << EOF
struct static_designated_values { int first; int second; int third; };
int values(void)
{
    static struct static_designated_values value = {.third = 8, .second = 4};
    return value.first + value.second + value.third;
}
int main(void)
{
    return values();
}
EOF

try_ 8 << EOF
int values(void)
{
    static int entries[3] = {[2] = 5, [0] = 3};
    return entries[0] + entries[1] + entries[2];
}
int main(void)
{
    return values();
}
EOF

try_ 8 << EOF
int values(void)
{
    int entries[] = {[2] = 5, [0] = 3};
    return entries[0] + entries[1] + entries[2];
}
int main(void)
{
    return values();
}
EOF

try_ 8 << EOF
int values(void)
{
    static int entries[] = {[2] = 5, [0] = 3};
    return entries[0] + entries[1] + entries[2];
}
int main(void)
{
    return values();
}
EOF

try_ 9 << EOF
struct local_pair { int first; int second; };
int values(void)
{
    static struct local_pair pair = {4, 5};
    return pair.first + pair.second;
}
int main(void)
{
    return values();
}
EOF

try_ 10 << EOF
struct local_pair { int first; int second; };
int values(void)
{
    static struct local_pair left = {1, 2}, right = {3, 4};
    return left.first + left.second + right.first + right.second;
}
int main(void)
{
    return values();
}
EOF

try_ 15 << EOF
struct static_grid { int values[2][2]; int tail; };
int values(void)
{
    static struct static_grid grid = {{{1, 2}, {3, 4}}, 5};
    return grid.values[0][0] + grid.values[0][1] + grid.values[1][0] +
           grid.values[1][1] + grid.tail;
}
int main(void)
{
    return values();
}
EOF

# Block scope gives same-spelled statics distinct objects in distinct functions.
try_ 62 << EOF
int first(void)
{
    static int value = 10;
    return value++;
}
int second(void)
{
    static int value = 20;
    return value++;
}
int main(void)
{
    return first() + second() + first() + second();
}
EOF

# Nested block scope must give a same-spelled static a second persistent object,
# without losing the enclosing static object between calls.
try_ 24 << EOF
int nested_static_values(void)
{
    static int value = 1;
    int outer = value++;
    {
        static int value = 10;
        return outer + value++;
    }
}
int main(void)
{
    return nested_static_values() + nested_static_values();
}
EOF

# A C99 for-init declaration has block scope, so its static object persists
# across calls but remains visible only to the loop's clauses and body.
try_ 6 << EOF
int run_once(void)
{
    int total = 0;
    for (static int count = 1; count < 4; count++)
        total += count;
    return total;
}
int main(void)
{
    return run_once() + run_once();
}
EOF

# The same global-storage lowering applies to each declarator in a for-init
# declaration.
try_ 4 << EOF
int run_once(void)
{
    int total = 0;
    for (static int count = 1, step = 2; count < 5; count += step)
        total += count;
    return total;
}
int main(void)
{
    return run_once() + run_once();
}
EOF

# Built-in specifiers in a for-init declaration take the same declaration path
# as their block-scope counterparts; this also checks unsigned wraparound in the
# loop condition/increment sequence.
try_ 3 << EOF
int main(void)
{
    int count = 0;
    for (unsigned int value = 0xfffffffeU; value != 1U; value++)
        count++;
    return count;
}
EOF

# A redeclaration without a storage class inherits an earlier static function's
# internal linkage. Reversing that order is a constraint violation.
try_ 42 << EOF
static int helper(void);
int helper(void) { return 42; }
int main(void) { return helper(); }
EOF
try_compile_error << EOF
static int missing_static_function(void);
int main(void) { return missing_static_function(); }
EOF
try_compile_error << EOF
int runtime_value(void) { return 7; }
int main(void) {
    static int invalid_static_value = runtime_value;
    return invalid_static_value;
}
EOF
try_ 42 << EOF
static int increment(int value);
int increment(int value) { return value + 1; }
int main(void) {
    int (*internal_call)(int) = increment;
    return internal_call(41);
}
EOF

# A static function designator retains internal linkage when materialized into a
# local function pointer and invoked indirectly.
try_ 42 << EOF
static int increment(int value) { return value + 1; }
int main(void) {
    int (*internal_call)(int) = increment;
    return internal_call(41);
}
EOF

# A file-scope function designator is an address constant too. This exercises
# global setup before main and both internal and external linkage targets.
try_ 49 << EOF
static int increment(int value) { return value + 1; }
int double_value(int value) { return value * 2; }
static int (*internal_call)(int) = increment;
int (*external_call)(int) = double_value;
int (*address_call)(int) = &increment;
static int internal_value = 3;
int external_value = 4;
static int *internal_value_ptr = &internal_value;
int *external_value_ptr = &external_value;
int main(void) {
    return internal_call(20) + external_call(10) + address_call(0) +
           *internal_value_ptr + *external_value_ptr;
}
EOF

try_ 13 << EOF
static int internal_values[] = {1, 2, 3};
int external_values[] = {4, 5};
static int *internal_first = internal_values;
int *internal_last = internal_values + 2;
int *external_first = &external_values;
int main(void) {
    return internal_first[2] + *internal_last + external_first[0] +
           external_first[1] - 2;
}
EOF

try_ 7 << EOF
struct global_pair { char tag; int value; };
static struct global_pair internal_pair = {1, 7};
int *internal_value = &internal_pair.value;
int main(void) { return *internal_value; }
EOF

try_ 9 << EOF
struct nested_inner { char pad; int value; };
struct nested_outer { int prefix; struct nested_inner inner; };
static struct nested_outer global_nested = {2, {1, 9}};
int *nested_value = &global_nested.inner.value;
int main(void) { return *nested_value; }
EOF

try_ 8 << EOF
struct global_array_record { int prefix; int values[3]; };
static struct global_array_record global_array = {1, {2, 8, 3}};
int *array_member_value = &global_array.values[1];
int main(void) { return *array_member_value; }
EOF

try_ 11 << EOF
struct global_array_element { int value; };
static struct global_array_element global_elements[] = {{3}, {11}};
int *array_element_value = &global_elements[1].value;
int main(void) { return *array_element_value; }
EOF

# Explicitly addressed global elements and member arrays may carry a byte-scaled
# integer constant-expression offset, not just a decayed array name or a bare
# literal.
try_ 25 << EOF
int global_values[] = {3, 5, 8};
struct global_offset_record { int values[3]; };
static struct global_offset_record global_offset = {{1, 2, 8}};
enum global_offsets {
    global_offset_count = 1 + 1,
    global_offset_shift = global_offset_count << 1
} global_marker = global_offset_shift;
static enum global_offsets global_half = global_offset_count;
int *global_last = &global_values[0] + global_offset_count;
int *global_first = &global_values[2] - (1 + 1);
int *member_last = &global_offset.values[0] + (global_offset_shift / 2);
int main(void) {
    return *global_last + *global_first + *member_last + global_marker +
           global_half;
}
EOF

# Repeated compatible file-scope declarations name the same static object. A
# later initialized definition supplies that object's initial value.
try_ 42 << EOF
static int file_value;
int file_value;
int file_value = 42;
int main(void) { return file_value; }
EOF

try_compile_error << EOF
static int initialized_once = 1;
static int initialized_once = 2;
int main(void) { return initialized_once; }
EOF

try_compile_error << EOF
int helper(void);
static int helper(void) { return 42; }
int main(void) { return helper(); }
EOF

try_compile_error << EOF
static int defined_twice(void) { return 1; }
static int defined_twice(void) { return 2; }
int main(void) { return defined_twice(); }
EOF

try_compile_error << EOF
static static int duplicated_file_storage;
int main(void) { return duplicated_file_storage; }
EOF

try_compile_error << EOF
int main(void) {
    static const static int duplicated_block_storage = 1;
    return duplicated_block_storage;
}
EOF

# Category: Const Qualifiers
begin_category "Const Qualifiers" "Testing const qualifier support for variables and parameters"

# Explicit casts may remove const in C, but must be diagnosed. Adding const
# remains legal and should not spuriously warn.
try_compile_warning "Warning: discarding const qualifier in cast" << EOF
int main(void) {
    const int value = 42;
    int *mutable = (int *)&value;
    return *mutable;
}
EOF

try_compile_warning "Warning: string literal is read-only" "--warn-string-literals" << EOF
int main(void) {
    char *text = "hello";
    return text[0];
}
EOF

try_compile_warning "Warning: string literal is read-only" "--warn-string-literals" << EOF
char *message = "global";
int main(void) {
    return message[0];
}
EOF

try_compile_warning "Warning: string literal is read-only" "--warn-string-literals" << EOF
int first(char *text) { return text[0]; }
int main(void) {
    return first("argument");
}
EOF

try_ 42 << EOF
int main(void) {
    int value = 42;
    const int *read_only = (const int *)&value;
    return *read_only;
}
EOF

# A pointer object's qualifier belongs one level inward after address-of.
# Preserve it so a valid pointer-to-const-pointer declaration is accepted, while
# the reverse conversion cannot discard that intermediate qualifier.
try_ 2 << EOF
int main(void) {
    int first = 1, second = 2;
    int * const fixed = &first;
    int * const *indirect = &fixed;
    indirect = &fixed;
    return **indirect + (*fixed == first);
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    int * const fixed = &value;
    int **mutable_indirect = &fixed;
    return **mutable_indirect;
}
EOF

try_compile_error << EOF
int main(void) {
    int first = 1, second = 2;
    int * const fixed = &first;
    int * const *indirect = &fixed;
    *indirect = &second;
    return *fixed;
}
EOF

# A cast to the same multi-level qualified type preserves the inner pointer
# qualifier instead of treating it as a discarded base-object qualifier.
try_ 1 << EOF
int main(void) {
    int value = 1;
    int * const fixed = &value;
    int * const *indirect = &fixed;
    int * const *copy = (int * const *)indirect;
    return **copy;
}
EOF

# Qualified union objects use the normal aggregate initializer path.
try_ 42 << EOF
union number { int integer; char character; };
int main(void) {
    const union number value = {42};
    return value.integer;
}
EOF

# C99 constraint violations: qualifiers make the designated object read-only.
try_compile_error << EOF
int main(void) {
    const int x = 1;
    x = 2;
    return x;
}
EOF

try_compile_error << EOF
struct pair { int x; int y; };
int main(void) {
    const struct pair value = {1, 2};
    value.x = 3;
    return value.x;
}
EOF

try_compile_error << EOF
int main(void) {
    const int values[2] = {1, 2};
    values[1] = 3;
    return values[1];
}
EOF

# Qualifiers apply to every declarator in one declaration, not just the first.
try_compile_error << EOF
int main(void) {
    const int first = 1, second = 2;
    second = 3;
    return first + second;
}
EOF

# A scalar typedef preserves const qualification on each use.
try_compile_error << EOF
typedef const int const_int;
int main(void) {
    const_int value = 1;
    value = 2;
    return value;
}
EOF

# A qualifier after a typedef declarator's star belongs to the pointer object.
# It must parse, retain its type-level depth, and reject conversion through a
# pointer-to-pointer that would permit replacing the fixed pointer.
try_ 1 << EOF
typedef int *const fixed_ptr;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    return *fixed;
}
EOF
try_compile_error << EOF
typedef int *const fixed_ptr;
typedef fixed_ptr *const fixed_handle;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    fixed_handle handle = &fixed;
    int ***mutable = &handle;
    return ***mutable;
}
EOF
try_compile_error << EOF
typedef int *const fixed_ptr;
int main(void) {
    int first = 1, second = 2;
    fixed_ptr fixed = &first;
    fixed = &second;
    return *fixed;
}
EOF
try_ 1 << EOF
typedef int *const fixed_ptr;
typedef fixed_ptr *const fixed_handle;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    fixed_handle handle = &fixed;
    return **handle;
}
EOF
try_compile_error << EOF
typedef int *const fixed_ptr;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    int **mutable = &fixed;
    return **mutable;
}
EOF
try_compile_error << EOF
int main(void) {
    const int x = 1;
    x++;
    return x;
}
EOF

# C99 permits a qualifier after the base type. It still qualifies the object,
# whereas a qualifier after `*` qualifies the pointer itself.
try_compile_error << EOF
int main(void) {
    int const value = 1;
    value = 2;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    int const *pointer = &value;
    *pointer = 2;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int first = 1, second = 2;
    int * const pointer = &first;
    pointer = &second;
    return *pointer;
}
EOF

try_compile_error << EOF
struct trailing_const_pair { int value; };
int main(void) {
    struct trailing_const_pair const pair = {1};
    pair.value = 2;
    return pair.value;
}
EOF

try_compile_error << EOF
void change(int const *pointer) {
    *pointer = 2;
}
int main(void) {
    int value = 1;
    change(&value);
    return value;
}
EOF

try_compile_error << EOF
int const trailing_global = 1;
int main(void) {
    trailing_global = 2;
    return trailing_global;
}
EOF

try_compile_error << EOF
typedef int *int_pointer;
int main(void) {
    int first = 1, second = 2;
    int_pointer const pointer = &first;
    pointer = &second;
    return *pointer;
}
EOF

try_compile_error << EOF
typedef int *int_pointer;
int main(void) {
    int first = 1, second = 2;
    const int_pointer pointer = &first;
    pointer = &second;
    return *pointer;
}
EOF

# A qualifier hidden in a typedef must survive another typedef and a
# dereference: *slot designates the const pointer object, not its int pointee.
try_compile_error << EOF
typedef int *const fixed_ptr;
typedef fixed_ptr *fixed_ptr_slot;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    fixed_ptr_slot slot = &fixed;
    *slot = &value;
    return **slot;
}
EOF

try_ 2 << EOF
typedef int *const fixed_ptr;
typedef fixed_ptr *fixed_ptr_slot;
int main(void) {
    int value = 1;
    fixed_ptr fixed = &value;
    fixed_ptr_slot slot = &fixed;
    **slot = 2;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    const int *p = &value;
    *p = 2;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    const int value = 1;
    int *p = &value;
    return *p;
}
EOF

# Adding const through two pointer levels is unsafe: a const pointer could be
# written back through the original int **.
try_compile_error << EOF
int main(void) {
    int value = 7;
    int *p = &value;
    const int **cpp = &p;
    return **cpp;
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 7;
    int *p = &value;
    int **pp = &p;
    const int **cpp;
    cpp = pp;
    return **cpp;
}
EOF

try_compile_error << EOF
int main(void) {
    int *p = 0;
    const int value = 1;
    p = &value;
    return *p;
}
EOF

# Parameter conversion must reject the same qualifier loss as an assignment.
try_compile_error << EOF
void overwrite(int *p) { *p = 2; }
int main(void) {
    const int value = 1;
    overwrite(&value);
    return value;
}
EOF

# A retained function-pointer prototype must enforce it as well.
try_compile_error << EOF
void overwrite(int *p) { *p = 2; }
int main(void) {
    const int value = 1;
    void (*fn)(int *) = overwrite;
    fn(&value);
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    const int *p = &value;
    *p += 2;
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int first = 1, second = 2;
    int * const p = &first;
    p = &second;
    return *p;
}
EOF

try_compile_error << EOF
void set_value(const int *p) {
    *p = 2;
}
int main(void) {
    int value = 1;
    set_value(&value);
    return value;
}
EOF

try_compile_error << EOF
int main(void) {
    int value = 1;
    const int *p = &value;
    const int **pp = &p;
    **pp = 2;
    return value;
}
EOF

# Test 1: Basic const local variable
try_ 42 << EOF
int main() {
    const int x = 42;
    return x;
}
EOF

# Test 2: Const global variable
try_ 100 << EOF
const int global_const = 100;
int main() {
    return global_const;
}
EOF

# Test 3: Multiple const variables
try_ 30 << EOF
int main() {
    const int a = 10;
    const int b = 20;
    return a + b;
}
EOF

# Test 4: Const parameter in function
try_ 15 << EOF
int add_five(const int x) {
    return x + 5;
}
int main() {
    return add_five(10);
}
EOF

# Test 5: Const pointer value (simplified)
try_ 25 << EOF
int main() {
    const int value = 25;
    const int *ptr = &value;
    return *ptr;
}
EOF

# Test 6: Pointer to const data
try_ 35 << EOF
int main() {
    const int value = 35;
    const int *ptr = &value;
    return *ptr;
}
EOF

# Test 7: Const in arithmetic expressions
try_ 60 << EOF
int main() {
    const int x = 20;
    const int y = 30;
    const int z = 10;
    return x + y + z;
}
EOF

# Test 8: Const with initialization from expression
try_ 50 << EOF
int main() {
    int a = 10;
    const int b = a * 5;
    return b;
}
EOF

# Test 9: Function returning through const variable
try_ 77 << EOF
int compute() {
    const int result = 77;
    return result;
}
int main() {
    return compute();
}
EOF

# Test 10: Const array element access
try_ 30 << EOF
int main() {
    const int arr[3] = {10, 20, 30};
    return arr[2];
}
EOF

# Test 11: Mixed const and non-const
try_ 45 << EOF
int main() {
    const int x = 15;
    int y = 20;
    const int z = 10;
    return x + y + z;
}
EOF

# Test 12: Const with conditional
try_ 40 << EOF
int main() {
    const int x = 40;
    const int y = 50;
    return (x < y) ? x : y;
}
EOF

# Test 13: Const value from struct (simplified)
try_ 99 << EOF
struct Point {
    int x;
    int y;
};
int main() {
    struct Point p = {99, 100};
    const int val = p.x;
    return val;
}
EOF

# Test 14: Const char array (string)
try_ 72 << EOF
int main() {
    const char str[] = "Hello";
    return str[0];  /* 'H' = 72 */
}
EOF

# Test 15: Multiple const on same line
try_ 55 << EOF
int main() {
    const int a = 10, b = 20, c = 25;
    return a + b + c;
}
EOF

# Test 16: Const with typedef
try_ 88 << EOF
typedef int myint;
int main() {
    const myint value = 88;
    return value;
}
EOF

# Test 17: Const void pointer
try_ 12 << EOF
int main() {
    int val = 12;
    const void *ptr = &val;
    const int *iptr = ptr;
    return *iptr;
}
EOF

# Test 18: Nested const usage
try_ 18 << EOF
int get_value(const int x) {
    const int multiplier = 2;
    return x * multiplier;
}
int main() {
    const int input = 9;
    return get_value(input);
}
EOF

# Test 19: Const with pointer arithmetic
try_ 30 << EOF
int main() {
    const int arr[] = {10, 20, 30, 40};
    const int *ptr = arr;
    ptr = ptr + 2;
    return *ptr;
}
EOF

# Test 20: Const with literal value
try_ 3 << EOF
int main() {
    const int x = 3;
    return x;
}
EOF

# Category: Ternary Operator
begin_category "Ternary Operator" "Testing conditional ?: operator"

# conditional operator
expr 10 "1 ? 10 : 5"
expr 25 "0 ? 10 : 25"

# Category: Compound Assignment
begin_category "Compound Assignment" "Testing +=, -=, *=, /=, %=, <<=, >>=, ^= operators"

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
items 8 "short s; s = 5; s += 3; return s;"
items 15 "short s; s = 20; s -= 5; return s;"
items 24 "short s; s = 6; s *= 4; return s;"
try_ 7 << EOF
int main(void) {
    int value = 1;
    value += 1 ? 6 : 9;
    return value;
}
EOF
try_ 6 << EOF
int main(void) {
    int value = 7;
    value ^= 0 ? 1 : 1;
    return value;
}
EOF
try_ 2 << EOF
int main(void) {
    int negative = -1;
    unsigned int divisor = 2U;
    negative /= divisor;
    int quotient = negative;
    negative %= divisor;
    return (quotient == 2147483647U) + (negative == 1U);
}
EOF
if [ "$PTR_SZ" -ge 8 ]; then
    try_ 1 << EOF
int main(void) {
    unsigned long long value = 0ULL;
    value += -1;
    return value == 0xffffffffffffffffULL;
}
EOF
    try_ 2 << EOF
int main(void) {
    unsigned long long quotient = 0x100000000ULL;
    unsigned long long remainder = 0x100000001ULL;
    int divisor = 2;
    quotient /= divisor;
    remainder %= 3;
    return (quotient == 0x80000000ULL) + (remainder == 2ULL);
}
EOF
fi

# Category: Sizeof Operator
begin_category "Sizeof Operator" "Testing sizeof operator on various types"

# sizeof
try_compile_error << EOF
int main(void)
{
    return sizeof(void);
}
EOF
try_compile_error << EOF
int value(void)
{
    return 1;
}
int main(void)
{
    return sizeof(value);
}
EOF
expr 1 "sizeof(_Bool)"
expr 1 "sizeof(char)"
expr 2 "sizeof(short)"
expr 4 "sizeof(int)"
# sizeof pointers
expr $PTR_SZ "sizeof(void*)"
expr $PTR_SZ "sizeof(_Bool*)"
expr $PTR_SZ "sizeof(char*)"
expr $PTR_SZ "sizeof(short*)"
expr $PTR_SZ "sizeof(int*)"
# sizeof multi-level pointer
expr $PTR_SZ "sizeof(void**)"
expr $PTR_SZ "sizeof(_Bool**)"
expr $PTR_SZ "sizeof(char**)"
expr $PTR_SZ "sizeof(short**)"
expr $PTR_SZ "sizeof(int**)"
# sizeof struct
try_ $PTR_SZ << EOF
typedef struct {
    int a;
    int b;
} struct_t;
int main() { return sizeof(struct_t*); }
EOF

try_ 6 << EOF
typedef struct {
    int x;
    short y;
} struct_t;

int main() { return sizeof(struct_t); }
EOF

# sizeof enum
try_ $PTR_SZ << EOF
typedef enum {
    A,
    B
} enum_t;
int main() { return sizeof(enum_t*); }
EOF

# sizeof with expressions
items 4 "int x = 42; return sizeof(x);"
items 12 "int values[3]; return sizeof(values);"
try_ 8 << EOF
int main(void)
{
    static char buffer[8] = "hi";
    return sizeof(buffer);
}
EOF
items 4 "int arr[5]; return sizeof(arr[0]);"
items 4 "int x = 10; int *ptr = &x; return sizeof(*ptr);"
items 1 "char c = 'A'; return sizeof(c);"
items 2 "short s = 100; return sizeof(s);"
items 4 "int a = 1, b = 2; return sizeof(a + b);"
items 7 "int value = 7; return +value;"
items 255 "unsigned char value = 255; return +value;"
try_compile_error << EOF
int main(void) { int value = 0; int *pointer = &value; return +pointer; }
EOF
try_compile_error << EOF
int main(void) { int value = 0; int *pointer = &value; return -pointer; }
EOF
try_compile_error << EOF
int main(void) { int value = 0; int *pointer = &value; return ~pointer; }
EOF
try_compile_error << EOF
int function_designator(void) { return 0; }
int main(void) { return -function_designator; }
EOF
items $PTR_SZ "return sizeof(volatile int) + sizeof(int *restrict) - 4;"
items 0 "int value = 0; int size = sizeof(++value); return value;"
items 0 "int value = 0; int size = sizeof(value++); return value;"

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

# Category: Switch Statements
begin_category "Switch Statements" "Testing switch-case control flow"

# switch-case
items 10 "int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } return a;"
items 10 "int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } return a;"

# Category: Enumerations
begin_category "Enumerations" "Testing enum declarations and usage"

# enum
try_ 6 << EOF
typedef enum { enum1 = 5, enum2 } enum_t;
int main() { enum_t v = enum2; return v; }
EOF

# Block-scope enum definitions supply integer constants to expressions and
# accept C99's trailing comma after their final enumerator.
try_ 4 << EOF
int main(void)
{
    enum local_values { local_base = 1 + 1, local_count = local_base << 1, };
    return local_count;
}
EOF

# A block-scope enum definition may introduce scalar declarators after its
# enumerator list, including a comma-separated declarator list.
try_ 12 << EOF
int main(void)
{
    enum local_values { local_base = 1 + 1, local_count = local_base << 1 }
        local_marker = local_count, local_next = local_marker + 1;
    return local_marker + local_next + local_base + 1;
}
EOF

try_ 14 << EOF
int main(void)
{
    enum local_values { local_base = 2, local_count = local_base << 1 };
    enum local_values local_marker = local_count;
    enum local_values local_next = local_marker + local_base + 4;
    return local_marker + local_next;
}
EOF

# A nested enum definition shadows both a file-scope tag and an enumerator; its
# tag and constants disappear when the nested block closes.
try_ 13 << EOF
enum enum_scope { scoped_value = 3 };
int main(void)
{
    int before = scoped_value;
    {
        enum enum_scope { scoped_value = 5, scoped_count = scoped_value + 1 };
        enum enum_scope values[scoped_count];
        int nested = scoped_value + scoped_count;
        int converted = (enum enum_scope) scoped_count;
        if (nested != 11 || converted != 6 || sizeof(enum enum_scope) != 4)
            return 1;
    }
    return before + scoped_value + 7;
}
EOF

# Leading qualifiers must not bypass enum declarators: const enum objects are
# read-only and static enum objects retain their initialized value between calls
# just like other block-scope static scalar objects.
try_ 10 << EOF
enum persistent_enum { persistent_fixed = 3 };
int next_persistent_enum(void)
{
    enum local_persistent_enum { persistent_start = 3 };
    static enum local_persistent_enum value = persistent_start;
    return value++;
}
int main(void)
{
    const enum persistent_enum fixed = persistent_fixed;
    return next_persistent_enum() + next_persistent_enum() + fixed;
}
EOF

try_ 12 << EOF
enum static_enum_entries { static_enum_first = 3, static_enum_second = 4 };
int static_enum_array_sum(void)
{
    static enum static_enum_entries entries[3] = {
        static_enum_first, static_enum_second, static_enum_first + 2
    };
    return entries[0] + entries[1] + entries[2];
}
int main(void) { return static_enum_array_sum(); }
EOF

try_ 10 << EOF
enum global_array_values { global_array_first = 1, global_array_second = 2 };
static int global_values[2] = {global_array_first, global_array_second};
int static_enum_array_sum(void)
{
    enum static_array_values { static_array_first = 2, static_array_second = 5 };
    static int values[3] = {
        [static_array_first - 2] = static_array_first,
        [static_array_second - 3] = static_array_second
    };
    return values[0] + values[1] + values[2];
}
int main(void) { return static_enum_array_sum() + global_values[0] + global_values[1]; }
EOF

# Enum tags are valid parameter and return type specifiers, including a
# qualified parameter declaration in an ordinary file-scope function API.
try_ 12 << EOF
enum api_state { api_ready = 5 };
enum api_state echo_api_state(const enum api_state value)
{
    return value;
}
int main(void)
{
    return echo_api_state(api_ready) + sizeof(enum api_state) + 3;
}
EOF

# Category: Memory Management
begin_category "Memory Management" "Testing malloc, free, and dynamic memory allocation"

if [ "$LINK_MODE" = "static" ]; then
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
else
    echo "Skip test cases because of using dynamic linking mode"
fi # "LINK_MODE" = "static"

try_ 1 << EOF
int main()
{
    char *ptr = "hello";
    return (0 == strcmp(ptr, "hello")) == (!strcmp(ptr, "hello"));
}
EOF

# Category: Preprocessor Directives
begin_category "Preprocessor Directives" "Testing #define, #ifdef, #ifndef, #if, #elif, #else, #endif"

# The compiler resolves quoted headers relative to the translation unit. The
# harness writes that unit into TEST_TMPDIR, so stage the nested fixture headers
# beside it before compiling the checked-in main file.
cp "$TESTS_DIR/include-base.h" "$TEST_TMPDIR/include-base.h"
cp "$TESTS_DIR/include-values.h" "$TEST_TMPDIR/include-values.h"
cp "$TESTS_DIR/include-macro.h" "$TEST_TMPDIR/include-macro.h"
try_file 24 "$TESTS_DIR/include-main.c"
try_compile_error << EOF
#define FIRST_HEADER SECOND_HEADER
#define SECOND_HEADER FIRST_HEADER
#include FIRST_HEADER
int main(void) { return 0; }
EOF
try_compile_error_message "unsupported platform configuration" << EOF
#error unsupported platform configuration
int main(void) { return 0; }
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

# A preprocessor expression is not limited to two operands. In particular,
# chained logical operators must keep consuming the directive through newline.
try_ 7 << EOF
#define A
#define C
#if defined(A) || defined(B) || defined(C)
#define RESULT 7
#else
#define RESULT 0
#endif
int main(void) { return RESULT; }
EOF

# C99 permits both defined(NAME) and defined NAME in a #if expression.
try_ 9 << EOF
#define ENABLED
#if defined ENABLED && !defined DISABLED
#define RESULT 9
#else
#define RESULT 0
#endif
int main(void) { return RESULT; }
EOF

# Conditional inclusion uses the full integer constant-expression grammar.
try_ 11 << EOF
#if ((1 << 2) == 4) && (5 % 3 == 2) && !defined UNKNOWN && (0 ? 0 : 1)
#define RESULT 11
#else
#define RESULT 0
#endif
int main(void) { return RESULT; }
EOF

# Inactive operands must be parsed but not evaluated: both divisions are invalid
# if reached, yet are protected by C99 short-circuit operators.
try_ 16 << EOF
#if 1 || (1 / 0)
#define OR_RESULT 1
#endif
#if 0 && (1 / 0)
#define AND_RESULT 0
#else
#define AND_RESULT 12
#endif
#if 1 ? 3 : (1 / 0)
#define TERNARY_RESULT 3
#endif
int main(void) { return OR_RESULT + AND_RESULT + TERNARY_RESULT; }
EOF

# Character constants are integer constants in a C99 #if expression.
try_ 14 << EOF
#if 'A' == 65 && '\\n' == 10
#define CHARACTER_RESULT 14
#else
#define CHARACTER_RESULT 0
#endif
int main(void) { return CHARACTER_RESULT; }
EOF

try_ 1 << EOF
#if '\\x123' == 0x23
#define HEX_ESCAPE_RESULT 1
#else
#define HEX_ESCAPE_RESULT 0
#endif
int main(void) { return HEX_ESCAPE_RESULT; }
EOF

try_ 2 << EOF
#if 'AB' == 0x4142
#define MULTI_CHARACTER_RESULT 1
#else
#define MULTI_CHARACTER_RESULT 0
#endif
int main(void) { return ('AB' == 0x4142) + MULTI_CHARACTER_RESULT; }
EOF

try_ 1 << EOF
int main(void) { return 'A\\0' == 0x4100; }
EOF

# Parentheses do not turn an object-like macro into a function-like macro: they
# remain available to call the replacement identifier.
try_ 15 << EOF
#define OBJECT_MACRO identity
int identity(int value) { return value; }
int main(void) { return OBJECT_MACRO(15); }
EOF

# Redefinition also replaces the old function-like signature.
try_ 4 << EOF
#define REUSED_MACRO(value) value
#undef REUSED_MACRO
#define REUSED_MACRO identity
int identity(int value) { return value; }
int main(void) { return REUSED_MACRO(4); }
EOF

# Function-like macros expand before a C99 #if expression is evaluated.
try_ 17 << EOF
#define TWO() 2
#define ADD(left, right) ((left) + (right))
#if ADD(TWO(), 3) == 5
#define FUNCTION_MACRO_RESULT 17
#else
#define FUNCTION_MACRO_RESULT 0
#endif
int main(void) { return FUNCTION_MACRO_RESULT; }
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

# An empty replacement list expands to nothing, in both macro shapes. Producing
# no tokens used to hand the caller a pointer into the dead frame that expanded
# them, which spliced the token list into a cycle the parser never left.
try_output 42 "" << EOF
#define EMPTY
#define NOTHING(x)
EMPTY int main(void)
{
    NOTHING(1)
    EMPTY return 42;
}
EOF

try_output 0 "ab" << EOF
#define BLANK
#define JOIN(a, b) printf(a); BLANK printf(b);
int main(void)
{
    JOIN("a", "b")
    return 0;
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

begin_category "Goto statements" "Testing goto and label statements"

# label undeclaration
try_compile_error << EOF
int main()
{
    goto label;
}
EOF

# label redefinition
try_compile_error << EOF
int main()
{
    goto label;
label:
label:
}
EOF

# test label namespace
try_ 1 << EOF
int main()
{
    goto label;
label:
    int label = 1;
    return label;
}
EOF

try_ 0 << EOF
int main() {
    int x = 0;
    goto skip;
    x = 1;
skip:
    return x;  /* Should return 0 */
}
EOF

# Forward reference. Statements between a goto and its label are unreachable but
# perfectly legal, and gcc accepts this silently at -Wall -Wextra -pedantic.
# shecc used to abort on the unreachable "return 1;" -- this case asserted that
# abort as a compile error; it now asserts the correct result.
try_ 0 << EOF
int main()
{
    goto end;
    return 1;
end:
    return 0;
}
EOF

# Simple loop
try_ 10 << EOF
int main()
{
    int vars0;

    vars0 = 0;
BB1:
    if (!(vars0 < 10)) goto BB6;
    vars0++;
    goto BB1;
BB6:
    return vars0;
}
EOF

# Complex loop
ans="0
0012345678910123456789201234567893012345678940123456789
1
0012345678910123456789201234567893012345678940123456789
3
0012345678910123456789201234567893012345678940123456789
4
0012345678910123456789201234567893012345678940123456789
5
0012345678910123456789201234567893012345678940123456789
6
0012345678910123456789201234567893012345678940123456789
7
0012345678910123456789201234567893012345678940123456789
8
0012345678910123456789201234567893012345678940123456789
9
0012345678910123456789201234567893012345678940123456789"
try_output 0 "$ans" << EOF
int main()
{
    int vars0;
    int vars1;
    int vars2;
    int vars3;

    vars0 = 0;
BB1:
    if (!(vars0 < 10)) goto BB47;
    if (vars0 == 2) goto BB45;
    printf("%d\n", vars0);
    vars1 = 0;
BB10:
    if (!(vars1 < 10)) goto BB27;
    if (vars1 == 5) goto BB27;
    printf("%d", vars1);
    vars2 = 0;
BB19:
    if (!(vars2 < 10)) goto BB25;
    printf("%d", vars2);
    vars2++;
    goto BB19;
BB25:
    vars1++;
    goto BB10;
BB27:
    printf("\n");
    vars3 = 5;
BB29:
    if (vars3 == 2) goto BB29;
    if (vars3 == 3) goto BB45;
    vars3--;
    if (vars3 > 0) goto BB29;
BB45:
    vars0++;
    goto BB1;
BB47:
    return 0;
}
EOF

# Category: Built-in macros
begin_category "Built-in Macros" "Testing macros defined by standard, e.g. __LINE__"

try_output 0 "3" << EOF
int main()
{
    printf("%d", __LINE__);
    return 0;
}
EOF

try_ 1 << EOF
int main()
{
    char *file_name = __FILE__;
    return !strcmp(file_name + strlen(file_name) - 2, ".c");
}
EOF

# Category: Function-like Macros
begin_category "Function-like Macros" "Testing function-like macros and variadic macros"

# An empty argument list is a valid invocation of a zero-parameter macro.
try_ 6 << EOF
#define SIX() 6
int main(void) { return SIX(); }
EOF

# stringification: '#' spells the argument as it was written
try_output 0 "hello world" << EOF
#define STR(x) #x
int main()
{
    printf("%s\n", STR(hello world));
    return 0;
}
EOF

# '#' does not expand its operand, but an extra level of macro does
try_output 0 "VER 3" << EOF
#define STR(x) #x
#define XSTR(x) STR(x)
#define VER 3
int main()
{
    printf("%s %s\n", STR(VER), XSTR(VER));
    return 0;
}
EOF

# a quote or backslash in the argument survives stringification
try_output 0 '["q\\b"]' << EOF
#define STR(x) #x
int main()
{
    printf("[%s]\n", STR("q\\\\b"));
    return 0;
}
EOF

# an empty argument stringifies to an empty string
try_output 0 "[]" << EOF
#define STR(x) #x
int main()
{
    printf("[%s]\n", STR());
    return 0;
}
EOF

# token pasting builds an identifier
try_ 11 << EOF
#define CAT(a, b) a##b
int foobar()
{
    return 11;
}
int main()
{
    return CAT(foo, bar)();
}
EOF

# pasting chains left to right, and works on numbers
try_ 123 << EOF
#define JOIN3(a, b, c) a##b##c
int main()
{
    return JOIN3(1, 2, 3);
}
EOF

# pasting in an object-like macro, and pasting an operator
try_ 42 << EOF
#define PLUSEQ +##=
#define OBJ pre##fix
int prefix = 41;
int main()
{
    int x = 1;
    x PLUSEQ prefix;
    return x;
}
EOF

# an empty operand leaves the other side of '##' standing alone
try_ 3 << EOF
#define CAT(a, b) a##b
int main()
{
    return CAT(1, ) + CAT(, 2);
}
EOF

# an omitted argument substitutes nothing rather than its own name
try_ 3 << EOF
#define TAIL(x, y) x y
int main()
{
    return TAIL(3, );
}
EOF

# a comma inside parentheses belongs to the argument, not the argument list
try_ 5 << EOF
#define ID(x) x
int add(int p, int q)
{
    return p + q;
}
int main()
{
    return ID(add(2, 3));
}
EOF

# '#' outside a macro definition is not a directive and must be rejected
try_compile_error << EOF
int main()
{
    int a = 1 # 2;
    return a;
}
EOF

# '##' with nothing on its left is rejected
try_compile_error << EOF
#define P(a) ##a
int main()
{
    return P(1);
}
EOF

# a paste that does not form a single token is rejected
try_compile_error << EOF
#define Q(a, b) a##b
int main()
{
    int Q(x, +) = 1;
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

# recursive macro expansion
try_ 4 << EOF
int A(int x)
{
    return 2;
}
#define A(x) x + B(x)
#define B(x) x + A(x)
int main()
{
    return A(1);
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
try_output 0 "Hello World!" << EOF
char *data = "Hello World!";

int main(void)
{
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

if [ "$LINK_MODE" = "static" ]; then

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

    # The following cases validate the behavior and return value of snprintf().
    #
    # This case is a normal case and outputs the complete string because the
    # given buffer size is large enough.
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
    # Thus, the output should be the string containing 19 characters for this
    # test case.
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
    # but the return value is 11, which corresponds to the length of "Number:
    # -37".
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
    # Since the FILE data type is defined as an int in the built-in C library,
    # and most of the functions such as fputc(), fgetc(), fclose() and fgets()
    # directly treat the "stream" parameter (of type FILE *) as a file
    # descriptor for performing input/output operations, the following test
    # cases define "stdout" as 1, which is the file descriptor for the standard
    # output.
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
else
    echo "Skip test cases because of using dynamic linking mode"
fi # "LINK_MODE" = "static"

# tests integer type conversion excerpted and modified from issue #166
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

# Binary literal tests (0b/0B prefix) Test basic binary literals
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
expr 45 "0b1111 + 0xF + 017"    # 15 + 15 + 15 = 45
expr 90 "0b110000 + 0x10 + 032" # 48 + 16 + 26 = 90

# Test binary literals in comparisons
expr 1 "0b1010 == 10"
expr 1 "0b11111111 == 255"
expr 0 "0b1000 != 8"
expr 1 "0b10000 > 0xF"
expr 1 "0B1111 < 020" # 15 < 16 (octal)

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

# New escape sequence tests (\a, \b, \v, \f) Test character literals with new
# escape sequences
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

# C99 hexadecimal escapes consume every following hex digit; assigning to a char
# produces the low byte.
try_ 1 << EOF
int main() {
    char *s = "\\x123";
    return s[0] == 0x23;
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

# Test escape sequences in printf Note: The bell character (\a) is non-printable
# but present in output
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

# va_list and variadic function tests Note: These tests demonstrate both direct
# pointer arithmetic and va_list typedef forwarding between functions, now fully
# supported.

# Test 1: Sum calculation using variadic arguments
try_output 0 "Sum: 15" << EOF
int calculate_sum(int count, ...)
{
    int sum = 0;
    int i;
    int *p;

    p = &count;
    p += $VS;

    for (i = 0; i < count; i++)
        sum += p[i * $VS];

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
    p += $VS;

    /* Get integer values */
    val1 = p[0];
    val2 = p[1 * $VS];

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

    p += $VS;
    printf("Args:");
    for (i = 0; i < count; i++)
        printf(" %d=%d", i + 1, p[i * $VS]);
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
    p += $VS;
    printf(" %d", *p);
    p += $VS;
    printf(" %d", *p);
    p += $VS;
    printf(" %d", *p);
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

    p += $VS;
    min = p[0];
    max = p[0];

    for (i = 1; i < count; i++) {
        if (p[i * $VS] < min) min = p[i * $VS];
        if (p[i * $VS] > max) max = p[i * $VS];
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
    p += $VS;
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

    p += $VS;
    for (i = 0; i < count; i++) {
        if (i % 2 == 0)
            result += p[i * $VS];
        else
            result -= p[i * $VS];
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
    int v2 = p[1 * $VS];
    int v3 = p[2 * $VS];
    return v1 + v2 + v3;
}

int main()
{
    printf("Variadic: %d", sum_three(10, 20, 30));
    return 0;
}
EOF

# va_list typedef forwarding tests These tests demonstrate va_list typedef
# forwarding between functions

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

# Complex pointer arithmetic tests Testing enhanced parser capability to handle
# expressions like *(ptr + offset)

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

try_ 200 << EOF
int main(void) {
    char *s = (char[]){'A', 'B', 'C', 'D', 'E'};
    return s[0] + s[1] + s[4]; /* 65 + 66 + 69 */
}
EOF

try_ 6 << EOF
int main(void) {
    short *s = (short[]){1, 2, 3, 4, 5};
    return s[0] + s[4];
}
EOF

try_ 60 << EOF
int main(void) {
    int arr[] = {10, 20, 30, 40, 50};
    int *selected = 1 ? arr : (int[]){1, 2, 3, 4, 5};
    return selected[0] + selected[4];
}
EOF

try_ 6 << EOF
int main(void) {
    int arr[] = {10, 20, 30, 40, 50};
    int *selected = 0 ? arr : (int[]){1, 2, 3, 4, 5};
    return selected[0] + selected[4];
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

# Array literal decay in initializer for pointer variable
try_ 0 << EOF
int main(void) {
    int *p = (int[]){42, 43, 44};
    return *p == 42 ? 0 : 1;
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

# Additional struct initialization tests from refine-parser Test: Local struct
# initialization (working with field-by-field assignment)
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

# Union support tests Basic union declaration and field access
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

try_ 2 << EOF
typedef union {
    short s;    /* 2 bytes */
    char c;     /* 1 byte */
} size_union_t;

int main() {
    return sizeof(size_union_t);  /* Returns 2 (size of short) */
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

# Sizeof union with mixed types. The largest member is the pointer, so the union
# is one pointer wide: 4 on the 32-bit targets, 8 on LP64.
try_ $PTR_SZ << EOF
typedef union {
    char c;
    int i;
    char *p;
} mixed_union_t;

int main() {
    return sizeof(mixed_union_t);  /* size of the largest member */
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

declare -a cast_tests=(
    "42 int var; var = (int)42; return var;"
    "10 int var; var = (short)10; return var;"
    "5 short s; s = (short)5; return s;"
    "20 short s; s = (int)20; return s;"
    "15 short sa = 10; short sb = (short)5; return sa + sb;"
    "30 int ia = 10; int ib = (int)20; return ia + ib;"
)

run_items_tests cast_tests

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
    printf("%s", "\x41Z"); /* hex escape then normal char */
    return 0;
}
EOF

try_output 0 "AZ" << 'EOF'
int main() {
    printf("%s", "A\132"); /* octal escape for 'Z' */
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

# Local array initializers - verify compilation and correct values Test 1:
# Implicit size array with single element
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

# Struct Variable Declaration Tests (Bug Fix Validation)
echo "Testing struct variable declaration functionality..."

# Test 1: Basic struct variable declaration (the original bug case)
try_ 0 << EOF
struct point {
    int x;
    int y;
};

int main() {
    struct point p;
    return 0;
}
EOF

# Test 2: Struct variable declaration with initialization
try_ 42 << EOF
struct point {
    int x;
    int y;
};

int main() {
    struct point p;
    p.x = 20;
    p.y = 22;
    return p.x + p.y;
}
EOF

# Test 3: Multiple struct variable declarations
try_ 20 << EOF
struct point {
    int x;
    int y;
};

int main() {
    struct point p1;
    struct point p2;
    p1.x = 10;
    p1.y = 15;
    p2.x = 3;
    p2.y = 2;
    return p1.x + p1.y - p2.x - p2.y;
}
EOF

# Test 4: Struct variable declaration in nested scope
try_ 100 << EOF
struct data {
    int value;
};

int main() {
    {
        struct data d;
        d.value = 100;
        return d.value;
    }
}
EOF

# Test 5: Struct with char fields
try_ 142 << EOF
struct character {
    char first;
    char second;
};

int main() {
    struct character ch;
    ch.first = 'A';
    ch.second = 'M';
    return ch.first + ch.second;  /* 65 + 77 = 142 */
}
EOF

# Test 6: Struct with typedef (working pattern)
try_ 55 << EOF
typedef struct {
    int data;
    void *next;
} node_t;

int main() {
    node_t n;
    n.data = 55;
    n.next = 0;
    return n.data;
}
EOF

# Test 7: Struct with pointer arithmetic
try_ 25 << EOF
typedef struct {
    int width;
    int height;
} rect_t;

int main() {
    rect_t rectangle;
    rectangle.width = 5;
    rectangle.height = 5;
    return rectangle.width * rectangle.height;
}
EOF

# Test 8: Struct variable with multiple fields
try_ 15 << EOF
typedef struct {
    int first;
    int second;
} container_t;

int main() {
    container_t c;
    c.first = 10;
    c.second = 5;
    return c.first + c.second;  /* 10 + 5 = 15 */
}
EOF

# Test 9: Simple struct array access
try_ 10 << EOF
typedef struct {
    int x;
    int y;
} point_t;

int main() {
    point_t p;
    p.x = 3;
    p.y = 7;
    return p.x + p.y;  /* 3+7 = 10 */
}
EOF

# Test 10: Struct variable declaration mixed with other declarations
try_ 88 << EOF
typedef struct {
    int x;
    int y;
} coord_t;

int main() {
    int a = 10;
    coord_t pos;
    int b = 20;
    coord_t vel;
    pos.x = 15;
    pos.y = 18;
    vel.x = 25;
    vel.y = 20;
    return a + b + pos.x + pos.y + vel.x;  /* 10+20+15+18+25 = 88 */
}
EOF

# Pointer dereference assignment tests Test Case 1: Simple pointer dereference
# assignment
try_ 0 << EOF
void f(int *ap) {
    *ap = 0;  // Should work now
}
int main() {
    return 0;
}
EOF

# Test Case 2: Double pointer assignment
try_ 0 << EOF
void f(int **ap) {
    *ap = 0;  // Should work now
}
int main() {
    return 0;
}
EOF

# Test Case 3: va_list Implementation (Original Context)
try_ 0 << EOF
typedef int *va_list;
void va_start(va_list *ap, void *last) {
    *ap = (int *)(&last + 1);  // Should work now
}
int main() {
    return 0;
}
EOF

# Test Case 4: Compilation test - pointer assignment with local variable
try_ 0 << EOF
void modify(int *p) {
    *p = 42;  // Tests pointer dereference assignment compilation
}
int main() {
    int x = 10;
    // Test compilation of pointer assignment - execution may have issues
    // but compilation should succeed
    return 0;
}
EOF

# Test Case 5: Compilation test - multiple pointer assignments
try_ 0 << EOF
void assign_values(int *a, int *b, int *c) {
    *a = 5;  // Multiple pointer dereference assignments
    *b = 4;
    *c = 6;
}
int main() {
    // Test compilation success for multiple pointer assignments
    return 0;
}
EOF

# Test Case 6: Compilation test - pointer arithmetic assignment
try_ 0 << EOF
void fill_array(int *arr, int size) {
    int i;
    for (i = 0; i < size; i++) {
        *(arr + i) = i;  // Pointer arithmetic assignment
    }
}
int main() {
    // Test compilation of pointer arithmetic assignments
    return 0;
}
EOF

# Test Case 7: Compilation test - nested pointer dereference
try_ 0 << EOF
void set_nested(int ***ptr) {
    ***ptr = 99;  // Triple pointer dereference assignment
}
int main() {
    // Test compilation of nested pointer assignments
    return 0;
}
EOF

# Test Case 8: Compilation test - assignment with arithmetic operations
try_ 0 << EOF
void complex_assign(int *ptr) {
    *ptr = *ptr + 42;  // Dereference on both sides
    *ptr = (*ptr * 2) + 1;  // Complex arithmetic
}
int main() {
    // Test compilation of complex pointer assignments
    return 0;
}
EOF

begin_category "Function parsing" "Forward declaration and implementation"

# Normal case
try_output 0 "Hello" << EOF
void func(char *ptr);

void func(char *ptr)
{
    while (*ptr) {
        printf("%c", *ptr);
        ptr++;
    }
}

int main()
{
    func("Hello");
    return 0;
}
EOF

# Incorrect function returning type
try_compile_error << EOF
void func(void);

int **func(void)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Incorrect number of parameters
try_compile_error << EOF
void func(void *a);

void func(void *a, int x)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Conflicting parameter types
try_compile_error << EOF
void func(void *a, char x);

void func(void *a, int x)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Conflicting parameter types (variadic parameters)
try_compile_error << EOF
void func(void *a);

void func(void *a, ...)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Incorrect function returning type (const)
try_compile_error << EOF
void *func(int *a, char x);

const void *func(int *a, char x)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Conflicting parameter types (const)
try_compile_error << EOF
void func(int *a, char x);

void func(const int *a, char x)
{
    return 3;
}

int main()
{
    func();
    return 0;
}
EOF

# Test Results Summary

echo ""
if [ "$SHOW_PROGRESS" = "1" ]; then
    echo "" # New line after progress indicators
fi

TEST_END_TIME=$(date +%s)
DURATION=$((TEST_END_TIME - TEST_START_TIME))

echo ""
echo "================================================================"
echo "                     Final Test Results                        "
echo "================================================================"
echo ""
echo "Execution Time: ${DURATION} seconds"
echo ""
echo "Overall Statistics:"
echo "  Total Tests:    $TOTAL_TESTS"
print_color green "  Passed:         $PASSED_TESTS"
if [ "$PASSED_TESTS" -gt 0 ] && [ "$TOTAL_TESTS" -gt 0 ]; then
    echo " ($((PASSED_TESTS * 100 / TOTAL_TESTS))%)"
else
    echo ""
fi

if [ "$FAILED_TESTS" -gt 0 ]; then
    print_color red "  Failed:         $FAILED_TESTS"
    echo " ($((FAILED_TESTS * 100 / TOTAL_TESTS))%)"
else
    echo "  Failed:         0"
fi

if [ "$SHOW_SUMMARY" = "1" ]; then
    echo ""
    echo "Category Breakdown:"
    echo "  +-----------------------------+-------+-------+-------+"
    echo "  | Category                    | Total | Pass  | Fail  |"
    echo "  +-----------------------------+-------+-------+-------+"

    for category in "${!CATEGORY_TESTS[@]}"; do
        if [ "${CATEGORY_TESTS[$category]}" -gt 0 ]; then
            printf "  | %-27s | %5d | " "$category" "${CATEGORY_TESTS[$category]}"
            print_color green "$(printf "%5d" "${CATEGORY_PASSED[$category]}")"
            printf " | "
            if [ "${CATEGORY_FAILED[$category]}" -gt 0 ]; then
                print_color red "$(printf "%5d" "${CATEGORY_FAILED[$category]}")"
            else
                printf "%5d" "${CATEGORY_FAILED[$category]}"
            fi
            echo " |"
        fi
    done | sort
    echo "  +-----------------------------+-------+-------+-------+"
fi

echo ""
if [ "$FAILED_TESTS" -eq 0 ]; then
    print_color green "+======================================+\n"
    print_color green "|    ALL TESTS PASSED!                 |\n"
    print_color green "+======================================+\n"
else
    echo ""
    exit 1
fi
