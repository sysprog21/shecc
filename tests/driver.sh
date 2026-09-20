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
    readonly SHECC_CFLAGS="--dynlink ${3:-lazy}"
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
    local actual="$?"
    local output=''
    if [ "$actual" -eq 0 ]; then
        chmod +x "$tmp_exe"
        output=$(${TARGET_EXEC:-} "$tmp_exe")
        actual="$?"
    fi

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

# Compile an inline program with one focused compiler-option set. Keep this
# beside try_ so feature regressions can exercise command-line behaviour without
# mutating the suite-wide linker configuration.
function try_flags()
{
    local expected="$1"
    local extra_flags="$2"
    local input=$(cat)
    test_selected || return 0

    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_err="$(mktemp)"
    echo "$input" > "$tmp_in"
    $SHECC $SHECC_CFLAGS $extra_flags -o "$tmp_exe" "$tmp_in" 2> "$tmp_err"
    local actual=$?
    if [ "$actual" -eq 0 ]; then
        chmod +x "$tmp_exe"
        ${TARGET_EXEC:-} "$tmp_exe"
        actual=$?
    fi

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$actual" -ne "$expected" ]; then
        report_test_failure "OPTION TEST" "$tmp_in" "$tmp_exe" "$expected" \
            "$actual" "$(< "$tmp_err")" "" "$tmp_err"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
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
    # subshell with job control disabled. A mistake in the input must end in a
    # diagnostic exit, so a crash (an exit status above 128) fails the test.
    (
        set +m 2> /dev/null # Disable job control messages
        $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" 2>&1
    ) > /dev/null 2>&1
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))

    if [ 0 == $exit_code ]; then
        report_test_failure "COMPILE ERROR TEST" "$tmp_in" "$tmp_exe" "non-zero" "0" "Compilation succeeded unexpectedly"
    elif [ "$exit_code" -gt 128 ]; then
        report_test_failure "COMPILE ERROR TEST" "$tmp_in" "$tmp_exe" \
            "diagnostic exit" "$exit_code" "Compiler crashed instead of reporting an error"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
        if [ "$VERBOSE_MODE" = "1" ]; then
            echo "Compilation error correctly detected"
        fi
    fi
}

# Compile invalid input with an explicit compiler option (for conformance-mode
# diagnostics) while preserving the normal test runner's accounting.
function try_compile_error_flag()
{
    local extra_flag="$1"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    echo "$input" > "$tmp_in"

    (
        set +m 2> /dev/null
        $SHECC $SHECC_CFLAGS $extra_flag -o "$tmp_exe" "$tmp_in" 2>&1
    ) > /dev/null 2>&1
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ 0 == $exit_code ]; then
        report_test_failure "CONFORMANCE ERROR TEST" "$tmp_in" "$tmp_exe" \
            "non-zero" "0" "Compilation succeeded unexpectedly"
    elif [ "$exit_code" -gt 128 ]; then
        report_test_failure "CONFORMANCE ERROR TEST" "$tmp_in" "$tmp_exe" \
            "diagnostic exit" "$exit_code" "Compiler crashed instead of reporting an error"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
}

function try_compile_flag()
{
    local extra_flag="$1"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_err="$(mktemp)"
    echo "$input" > "$tmp_in"
    $SHECC $SHECC_CFLAGS $extra_flag -o "$tmp_exe" "$tmp_in" 2> "$tmp_err"
    local exit_code=$?

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$exit_code" -ne 0 ]; then
        report_test_failure "CONFORMANCE COMPILE TEST" "$tmp_in" "$tmp_exe" \
            "0" "$exit_code" "$(< "$tmp_err")" "" "$tmp_err"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
}

function try_failure_output()
{
    local expected_text="$1"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_log="$(mktemp)"
    echo "$input" > "$tmp_in"
    $SHECC $SHECC_CFLAGS -o "$tmp_exe" "$tmp_in" > "$tmp_log" 2>&1
    local compile_status=$?
    local run_status=0
    if [ "$compile_status" -eq 0 ]; then
        chmod +x "$tmp_exe"
        ${TARGET_EXEC:-} "$tmp_exe" >> "$tmp_log" 2>&1
        run_status=$?
    fi

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$compile_status" -ne 0 ] || [ "$run_status" -eq 0 ] \
        || ! grep -Fq -- "$expected_text" "$tmp_log"; then
        report_test_failure "FAILURE OUTPUT TEST" "$tmp_in" "$tmp_exe" \
            "non-zero; $expected_text" "$run_status" "$(< "$tmp_log")" \
            "" "$tmp_log"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
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
    if [ "$exit_code" -eq 0 ] || [ "$exit_code" -gt 128 ] \
        || ! grep -Fq -- "$expected" "$tmp_log"; then
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
    if [ "$exit_code" -ne 0 ] || ! grep -Fq -- "$expected" "$tmp_log"; then
        report_test_failure "COMPILE WARNING TEST" "$tmp_in" "$tmp_exe" \
            "$expected" "$exit_code" "$(< "$tmp_log")"
    else
        ((PASSED_TESTS++))
        ((CATEGORY_PASSED["$CURRENT_CATEGORY"]++))
        show_progress
    fi
}

# Count the phase-2 instructions of one function that match an extended regular
# expression, in the --dump-ir output of an inline program. Some behaviour, such
# as each access to a volatile object reaching the generated code, cannot be
# observed by running the program. Usage: try_ir_count <expected count>
# <function> <pattern> << EOF
function try_ir_count()
{
    local expected="$1"
    local func="$2"
    local pattern="$3"
    local input=$(cat)
    test_selected || return 0
    local tmp_in="$(mktemp --suffix .c)"
    local tmp_exe="$(mktemp)"
    local tmp_ir="$(mktemp)"
    local tmp_err="$(mktemp)"
    echo "$input" > "$tmp_in"

    $SHECC $SHECC_CFLAGS --dump-ir -o "$tmp_exe" "$tmp_in" > "$tmp_ir" \
        2> "$tmp_err"
    local actual=$?
    if [ "$actual" -eq 0 ]; then

        # The phase-2 dump follows the phase-1 one; in it a function starts with
        # its name in the first column and its instructions are indented.
        actual=$(awk -v fn="$func:" '
            /<END OF INSN DUMP>/ { ph2 = 1; next }
            ph2 && /^[^\t]/ { in_fn = ($0 == fn); next }
            ph2 && in_fn' "$tmp_ir" | grep -cE -- "$pattern")
    fi

    ((TOTAL_TESTS++))
    ((CATEGORY_TESTS["$CURRENT_CATEGORY"]++))
    if [ "$actual" != "$expected" ]; then
        report_test_failure "IR COUNT TEST ($func: $pattern)" "$tmp_in" \
            "$tmp_exe" "$expected" "$actual" "$(< "$tmp_ir")" "" "$tmp_err"
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
int printf(const char *format, ...);
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
try_file 0 '' "$TESTS_DIR/escaped-param.c"

# The section header table closes an ELF32 image, so e_shoff plus its extent
# must reach exactly the end of the file, including the page padding a static
# image inserts before .data. ELF64 output carries no section headers.
try_ 0 << EOF
#include <stdio.h>
int main(int argc, char **argv) {
    FILE *f = fopen(argv[0], "rb");
    char header[52];
    int size = 0;
    int c;
    if (!f)
        return 2;
    while ((c = fgetc(f)) != -1) {
        if (size < 52)
            header[size] = c;
        size++;
    }
    fclose(f);
    if (size < 52 || header[4] != 1)
        return 0;
    int shoff = (header[32] & 255) | ((header[33] & 255) << 8) |
                ((header[34] & 255) << 16) | ((header[35] & 255) << 24);
    int shentsize = (header[46] & 255) | ((header[47] & 255) << 8);
    int shnum = (header[48] & 255) | ((header[49] & 255) << 8);
    return shoff + shentsize * shnum != size;
}
EOF

# Category: Basic Literals and Constants
begin_category "Literals and Constants" "Testing integer, character, and string literals"

try_compile_error << EOF
int main(void) { return 1.5; }
EOF
try_compile_error << EOF
int main(void) { return 1e3; }
EOF
try_compile_error << EOF
int main(void) { return .5; }
EOF
try_compile_error << EOF
int main(void) { return 0x1p3; }
EOF
try_compile_error << EOF
int main(void) { return 3.25F; }
EOF
try_compile_error << EOF
int main(void) { return 0x1.8p+2L; }
EOF
try_compile_error << EOF
int main(void) { return 0x.8p2; }
EOF
try_compile_error << EOF
int main(void) { return 0x.ABp2; }
EOF
try_compile_error << EOF
int main(void) { return 0x.p1; }
EOF

# A hexadecimal integer needs at least one digit after its prefix, including
# where a constant expression would otherwise read the bare prefix as zero.
try_compile_error_message "expected hex digit after 0x" << EOF
enum { E = 0x };
int main(void) { return E; }
EOF
try_compile_error_message "expected hex digit after 0x" << EOF
int global = 0X;
int main(void) { return global; }
EOF
try_compile_error_message "expected hex digit after 0x" << EOF
#if 0x == 0
#endif
int main(void) { return 0; }
EOF
try_compile_error_message "expected hex digit after 0x" << EOF
int main(void) { return 0xg; }
EOF

# A floating literal whose digits fill the token buffer must be diagnosed before
# its point and suffix are appended, not written past the buffer.
try_compile_error_message "Token too long" << EOF
int main(void) { return sizeof($(printf '1%.0s' {1..255}).f); }
EOF

# Record bodies nested past the type table are diagnosed, not aborted: each one
# takes a type entry before its members are read.
try_compile_error_message "Maximum number of types exceeded" << EOF
struct deep { $(printf 'struct { %.0s' {1..300}) int x; $(printf '} m;%.0s' {1..300}) };
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { return 08.5; }
EOF
try_compile_error << EOF
#define FLOAT_MACRO 1.25e+1F
int main(void) { return FLOAT_MACRO; }
EOF
try_compile_error << EOF
#if 1.0
#endif
int main(void) { return 0; }
EOF
try_compile_error << EOF
int float;
EOF
try_compile_error << EOF
double value;
EOF
try_compile_error << EOF
int main(void) { float value; return 0; }
EOF
try_compile_error << EOF
int main(void) { return (float)1; }
EOF
try_ "$((4 + 8 + (PTR_SZ == 8 ? 16 : 8) + PTR_SZ))" << EOF
int main(void) {
    return sizeof(float) + sizeof(double) + sizeof(long double) +
           sizeof(float *);
}
EOF
try_ "$((4 + 8 + (PTR_SZ == 8 ? 16 : 8)))" << EOF
typedef float float_alias;
typedef double double_alias;
typedef long double long_double_alias;
int main(void) {
    return sizeof(float_alias) + sizeof(double_alias) +
           sizeof(long_double_alias);
}
EOF
try_compile_error << EOF
int main(void) { return sizeof(unsigned double); }
EOF
try_compile_error << EOF
int main(void) { return sizeof(long float); }
EOF
try_compile_error << EOF
typedef float float_alias;
float_alias value;
EOF
try_compile_error << EOF
typedef double double_alias;
int identity(double_alias value) { return 0; }
EOF
try_compile_error << EOF
typedef float float_alias;
int main(void) { return (float_alias)1; }
EOF

try_ 0 << EOF
int main(void) {
    return sizeof(2147483648U) != sizeof(unsigned int) ||
           0xffffffff > -1 || (0xffffffff >> 31) != 1;
}
EOF

# A U-suffixed value above the one-word range selects unsigned long long.
try_ 0 << EOF
int main(void) { return sizeof(4294967296U) != 8; }
EOF

try_ 0 << EOF
enum { global_unsigned_shift_count = 1 };
unsigned int global_unsigned_shift = 0xffffffff >> 31;
int global_unsigned_compare = 0xffffffff > -1;
unsigned int global_unsigned_quotient = 0xffffffff / 2;
unsigned int global_unsigned_enum_shift =
    0xffffffff >> global_unsigned_shift_count;
int main(void) {
    return global_unsigned_shift != 1U || global_unsigned_compare ||
           global_unsigned_quotient != 2147483647U ||
           global_unsigned_enum_shift != 2147483647U;
}
EOF

try_ 0 << EOF
int static_unsigned_initializer(void) {
    enum { local_unsigned_shift_count = 1 };
    static unsigned int value = 0xffffffff >> local_unsigned_shift_count;
    return value != 2147483647U;
}
int main(void) { return static_unsigned_initializer(); }
EOF

try_ 0 << EOF
int main(void) {
    return sizeof(int[2]) != 2 * sizeof(int) ||
           sizeof(int[2][3]) != 6 * sizeof(int);
}
EOF

# A sizeof result has type size_t in every scope, so it converts a signed
# operand to unsigned just as the file-scope constant does.
try_ 0 << EOF
int global_sizeof_wraps = (sizeof(int) - 5) > 0;
int main(void) {
    int x = 3;
    int arr[4];
    int n = -1;
    long long wide = (long long) (sizeof(int) - 5);
    return !((sizeof(int) - 5) > 0) || -1 < sizeof(int) ||
           !((sizeof x - 5) / 2 > 100) ||
           n < sizeof(arr) / sizeof(arr[0]) || !global_sizeof_wraps ||
           wide != (sizeof(size_t) == 8 ? -1LL : 4294967295LL);
}
EOF

# Unsigned and long long constant operands fold with their own conversions, so a
# zero-valued unsigned constant expression is still a null pointer constant.
try_ 0 << EOF
int three(void) { return 3; }
int main(void) {
    int (*callback)(void) = three;
    int zero = 0;
    return (callback != (0UL - 0UL)) + (callback != (0ULL - 0ULL)) +
               ((1 ? callback : sizeof(int) - sizeof(int))() == 3) !=
               3 ||
           (4000000000U / 3U) != 1333333333U || (0x80000000U >> 31) != 1U ||
           (-1 < 0U) || (-1LL < 0U) != 1 || (-7LL / 2LL) != -3LL ||
           (0xFFFFFFFFFFFFFFFFULL / 3ULL) != 0x5555555555555555ULL ||
           (zero && 1U / (sizeof(int) - sizeof(int)));
}
EOF

try_ 0 << EOF
int global_abstract_array_size = sizeof(int[2][3]);
int main(void) { return global_abstract_array_size != 6 * sizeof(int); }
EOF

try_ 0 << EOF
struct abstract_array_pair { int first; int second; };
int global_abstract_record_array_size =
    sizeof(struct abstract_array_pair[2]);
int main(void) {
    return global_abstract_record_array_size !=
           4 * sizeof(int);
}
EOF

try_ 0 << EOF
int global_pointer_to_array_size = sizeof(int (*)[2]);
int global_array_of_pointer_size = sizeof(int (*[2]));
int global_pointer_to_matrix_size = sizeof(int (*)[2][3]);
int global_matrix_of_pointer_size = sizeof(int (*[2][3]));
int main(void) {
    return global_pointer_to_array_size != sizeof(int *) ||
           global_array_of_pointer_size != 2 * sizeof(int *) ||
           global_pointer_to_matrix_size != sizeof(int *) ||
           global_matrix_of_pointer_size != 6 * sizeof(int *);
}
EOF

try_ 0 << EOF
int global_function_pointer_size = sizeof(int (*)(void));
int global_typed_function_pointer_size = sizeof(int (*)(int, const char *));
int global_unprototyped_function_pointer_size = sizeof(int (*)());
int global_variadic_function_pointer_size = sizeof(int (*)(int, ...));
int global_float_function_pointer_size =
    sizeof(int (*)(float, double, long double));
int global_nested_function_pointer_size = sizeof(int (*(*)(int))[2]);
int global_returned_function_pointer_size = sizeof(int (*(*)(int))(int));
int global_deep_returned_function_pointer_size =
    sizeof(int (*(*(*)(int))(int))(int));
int global_mixed_returned_function_pointer_size =
    sizeof(int (*(*(*)(int))[2])(int));
int global_pointer_to_callback_array_size = sizeof(int (*(*)[2])(int));
int global_callback_array_returning_callback_size =
    sizeof(int (*(*[2])(int))(int));
int global_matrix_of_callbacks_size = sizeof(int (*[2][3])(int));
int global_matrix_of_callbacks_returning_callbacks_size =
    sizeof(int (*(*[2][3])(int))(int));
int global_callback_array_returning_array_size =
    sizeof(int (*(*[2])(int))[3]);
int global_array_of_callback_array_pointers_size =
    sizeof(int (*(*[2])[3])(int));
int global_matrix_of_callback_array_pointers_size =
    sizeof(int (*(*[2][3])[4])(int));
int main(void) {
    return sizeof(int (*)(void)) != sizeof(int *) ||
           global_function_pointer_size != sizeof(int *) ||
           sizeof(int (*)(int, const char *)) != sizeof(int *) ||
           global_typed_function_pointer_size != sizeof(int *) ||
           sizeof(int (*)()) != sizeof(int *) ||
           global_unprototyped_function_pointer_size != sizeof(int *) ||
           sizeof(int (*)(int, ...)) != sizeof(int *) ||
           global_variadic_function_pointer_size != sizeof(int *) ||
           sizeof(int (*)(float, double, long double)) != sizeof(int *) ||
           global_float_function_pointer_size != sizeof(int *) ||
           sizeof(int (*(*)(int))[2]) != sizeof(int *) ||
           global_nested_function_pointer_size != sizeof(int *) ||
           sizeof(int (*(*)(int))(int)) != sizeof(int *) ||
           global_returned_function_pointer_size != sizeof(int *) ||
           sizeof(int (*(*(*)(int))(int))(int)) != sizeof(int *) ||
           global_deep_returned_function_pointer_size != sizeof(int *) ||
           sizeof(int (*(*(*)(int))[2])(int)) != sizeof(int *) ||
           global_mixed_returned_function_pointer_size != sizeof(int *) ||
           sizeof(int (*(*)[2])(int)) != sizeof(int *) ||
           global_pointer_to_callback_array_size != sizeof(int *) ||
           sizeof(int (*(*[2])(int))(int)) != 2 * sizeof(int *) ||
           global_callback_array_returning_callback_size != 2 * sizeof(int *) ||
           sizeof(int (*[2][3])(int)) != 6 * sizeof(int *) ||
           global_matrix_of_callbacks_size != 6 * sizeof(int *) ||
           sizeof(int (*(*[2][3])(int))(int)) != 6 * sizeof(int *) ||
           global_matrix_of_callbacks_returning_callbacks_size !=
               6 * sizeof(int *) ||
           sizeof(int (*(*[2])(int))[3]) != 2 * sizeof(int *) ||
           global_callback_array_returning_array_size != 2 * sizeof(int *) ||
           sizeof(int (*(*[2])[3])(int)) != 2 * sizeof(int *) ||
           global_array_of_callback_array_pointers_size !=
               2 * sizeof(int *) ||
           sizeof(int (*(*[2][3])[4])(int)) != 6 * sizeof(int *) ||
           global_matrix_of_callback_array_pointers_size !=
               6 * sizeof(int *);
}
EOF

# Pointers of a declarator level apply before its array suffix, and a grouped
# inner declarator applies after the suffix that follows it. A typedef array
# keeps its own bounds when an abstract declarator adds more.
try_ 0 << EOF
typedef int abstract_triple[3];
int global_pointer_array_size = sizeof(int *[2]);
int global_array_of_array_pointers_size = sizeof(int (*[2])[3]);
int global_typedef_array_size = sizeof(abstract_triple[2]);
int pointer_array_bound[sizeof(char *const[3])];
enum { array_of_array_pointers_size = sizeof(int (*[4])[3]) };
int main(void)
{
    int local_bound[sizeof(int *[2][2])];
    switch (2 * sizeof(int *)) {
    case sizeof(int *[2]):
        break;
    default:
        return 1;
    }
    return global_pointer_array_size != 2 * sizeof(int *) ||
           global_array_of_array_pointers_size != 2 * sizeof(int *) ||
           global_typedef_array_size != 6 * sizeof(int) ||
           sizeof(pointer_array_bound) != 3 * sizeof(char *) * sizeof(int) ||
           array_of_array_pointers_size != 4 * sizeof(int *) ||
           sizeof(local_bound) != 4 * sizeof(int *) * sizeof(int) ||
           sizeof(int *[2]) != 2 * sizeof(int *) ||
           sizeof(int (*[2])[3]) != 2 * sizeof(int *) ||
           sizeof(char ([2])[3]) != 6 ||
           sizeof(abstract_triple[2]) != 6 * sizeof(int);
}
EOF

try_compile_error << EOF
struct incomplete_sizeof_record;
int main(void) { return sizeof(struct incomplete_sizeof_record); }
EOF
try_compile_error << EOF
union incomplete_sizeof_union;
int main(void) { return sizeof(union incomplete_sizeof_union); }
EOF
try_compile_error << EOF
int main(void) { return sizeof(int (*)(int)[2]); }
EOF
try_compile_error << EOF
enum { invalid_callback_array = sizeof(int [2](int)) };
int main(void) { return invalid_callback_array; }
EOF

try_ 0 << EOF
int global_nested_designated[2][2][2] = { [0] = { [1] = { 2 } }, 3, 4, 5 };
int nested_designated(void) {
    static int local[2][2][2] = { [0] = { [1] = { 2 } }, 3, 4, 5 };
    return local[0][1][0] != 2 || local[0][1][1] != 0 ||
           local[1][0][0] != 3 || local[1][0][1] != 4 ||
           local[1][1][0] != 5 || global_nested_designated[0][1][0] != 2 ||
           global_nested_designated[1][0][0] != 3 ||
           global_nested_designated[1][0][1] != 4 ||
           global_nested_designated[1][1][0] != 5;
}
int main(void) { return nested_designated(); }
EOF

# A C99 designator may traverse a record member and then select an array element
# in a file-scope aggregate initializer.
try_ 0 << EOF
struct item { int values[3]; };
struct outer { struct item member; };
struct outer value = {
    .member.values[2] = 7,
    .member.values[0] = 3,
};
int main(void) {
    return value.member.values[0] != 3 || value.member.values[1] != 0 ||
           value.member.values[2] != 7;
}
EOF

try_ 0 << EOF
int nested_hyperplane_designated(void) {
    int values[2][2][2][2] = { [0] = { [1] = { [1] = { 2 } } }, 3, 4, 5 };
    return values[0][1][1][0] != 2 || values[0][1][1][1] != 0 ||
           values[1][0][0][0] != 3 || values[1][0][0][1] != 4 ||
           values[1][0][1][0] != 5;
}
int main(void) { return nested_hyperplane_designated(); }
EOF

# A two-subscript designator of a four-dimensional array names a plane, so its
# braces hold rows. Parsing them as a whole hyperplane dropped the second row.
try_ 0 << EOF
int global_plane[2][2][2][2] = { [1][1] = { {1, 2}, {3, 4} }, [0][1][1] = { 5, 6 } };
int main(void) {
    int local_plane[2][2][2][2] = { [1][1] = { {1, 2}, {3, 4} }, [0][1][1] = { 5, 6 } };
    static int static_plane[2][2][2][2] = { [1][1] = { {1, 2}, {3, 4} }, [0][1][1] = { 5, 6 } };
    return global_plane[1][1][1][0] + global_plane[1][1][1][1] +
           global_plane[0][1][1][1] + local_plane[1][1][1][0] +
           local_plane[1][1][1][1] + local_plane[0][1][1][1] +
           static_plane[1][1][1][0] + static_plane[1][1][1][1] +
           static_plane[0][1][1][1] != 39 || local_plane[1][1][0][1] != 2 ||
           local_plane[1][0][1][1] != 0;
}
EOF

try_ 0 << EOF
int global_unbraced_designated[2][2][2] = { [0] = { [1] = 2, 3 }, 4, 5 };
int nested_unbraced_designated(void) {
    int automatic_values[2][2][2] = { [0] = { [1] = 2, 3 }, 4, 5 };
    static int static_values[2][2][2] = { [0] = { [1] = 2, 3 }, 4, 5 };
    return automatic_values[0][1][0] != 2 ||
           automatic_values[0][1][1] != 3 ||
           automatic_values[1][0][0] != 4 ||
           automatic_values[1][0][1] != 5 ||
           static_values[0][1][0] != 2 || static_values[0][1][1] != 3 ||
           static_values[1][0][0] != 4 || static_values[1][0][1] != 5 ||
           global_unbraced_designated[0][1][0] != 2 ||
           global_unbraced_designated[0][1][1] != 3 ||
           global_unbraced_designated[1][0][0] != 4 ||
           global_unbraced_designated[1][0][1] != 5;
}
int main(void) { return nested_unbraced_designated(); }
EOF

try_ 0 << EOF
int global_unbraced_hyperplane[2][2][2][2] =
    { [0] = { [1] = 2, 3, 4, 5 }, 6, 7, 8 };
int nested_unbraced_hyperplane(void) {
    int automatic_values[2][2][2][2] =
        { [0] = { [1] = 2, 3, 4, 5 }, 6, 7, 8 };
    static int static_values[2][2][2][2] =
        { [0] = { [1] = 2, 3, 4, 5 }, 6, 7, 8 };
    return automatic_values[0][1][0][0] != 2 ||
           automatic_values[0][1][0][1] != 3 ||
           automatic_values[0][1][1][0] != 4 ||
           automatic_values[0][1][1][1] != 5 ||
           automatic_values[1][0][0][0] != 6 ||
           automatic_values[1][0][0][1] != 7 ||
           automatic_values[1][0][1][0] != 8 ||
           static_values[0][1][0][0] != 2 ||
           static_values[0][1][0][1] != 3 ||
           static_values[0][1][1][0] != 4 ||
           static_values[0][1][1][1] != 5 ||
           static_values[1][0][0][0] != 6 ||
           static_values[1][0][0][1] != 7 ||
           static_values[1][0][1][0] != 8 ||
           global_unbraced_hyperplane[0][1][0][0] != 2 ||
           global_unbraced_hyperplane[0][1][0][1] != 3 ||
           global_unbraced_hyperplane[0][1][1][0] != 4 ||
           global_unbraced_hyperplane[0][1][1][1] != 5 ||
           global_unbraced_hyperplane[1][0][0][0] != 6 ||
           global_unbraced_hyperplane[1][0][0][1] != 7 ||
           global_unbraced_hyperplane[1][0][1][0] != 8;
}
int main(void) { return nested_unbraced_hyperplane(); }
EOF

try_ 1 << EOF
int type_only_float_parameter = sizeof(int (*)(double));
double still_an_unsupported_object;
EOF

try_ 0 << EOF
typedef int (*float_callback_type)(double);
int global_float_callback_type_size = sizeof(float_callback_type);
int main(void) {
    return sizeof(float_callback_type) != sizeof(int *) ||
           global_float_callback_type_size != sizeof(int *);
}
EOF

try_ 1 << EOF
typedef int (*float_callback_type)(double);
double still_an_unsupported_object;
EOF

try_ 1 << EOF
typedef int (*float_callback_type)(double);
float_callback_type still_an_unsupported_callback_object;
EOF

try_ 0 << EOF
int global_float_type_size = sizeof(float) + sizeof(double) +
                             sizeof(long double);
typedef double global_sizeof_real;
int global_typedef_float_array_size = sizeof(global_sizeof_real[2]);
int global_float_array_size = sizeof(float[2][3]);
int global_double_pointer_size = sizeof(double (*)[2]);
int main(void) {
    return global_float_type_size != 4 + 8 + (sizeof(void *) == 8 ? 16 : 8) ||
           global_float_array_size != 6 * sizeof(float) ||
           global_typedef_float_array_size != 2 * sizeof(double) ||
           global_double_pointer_size != sizeof(double *);
}
EOF
try_compile_error << EOF
int invalid_global_float_type_size = sizeof(unsigned double);
EOF
try_compile_error << EOF
int identity(float value) { return 0; }
EOF
try_compile_error << EOF
int identity(long double value) { return 0; }
EOF
try_compile_error << EOF
long double value;
EOF
try_compile_error << EOF
int _Complex;
EOF
try_compile_error << EOF
int _Imaginary;
EOF
try_compile_error << EOF
#define FLOAT_TYPE float
FLOAT_TYPE value;
EOF

# just a number
expr 0 0

# C99 6.4.4.4 requires at least one character between the quotes.
try_compile_error_message "Empty character constant" << EOF
int main(void) { return ''; }
EOF
try_compile_error_message "Empty character constant" << EOF
int main(void) { return L''; }
EOF
try_compile_error_message "Empty character constant" << EOF
#if '' == 0
#endif
int main(void) { return 0; }
EOF

# A newline cannot appear in a character constant or a string literal (C99
# 6.4.4.4, 6.4.5); only a backslash-newline, which phase 2 removes first, may
# continue one onto the next line.
try_compile_error_message "Unenclosed character literal" << EOF
int main(void) { return 'a
'; }
EOF
try_compile_error_message "Unenclosed character literal" << EOF
int main(void) { return L'
'; }
EOF
try_compile_error_message "Unenclosed string literal" << EOF
int main(void) { return sizeof("a
b"); }
EOF
try_compile_error_message "Unenclosed string literal" << EOF
int main(void) { return sizeof(L"a
b"); }
EOF
try_ 3 << 'EOF'
int main(void) { return sizeof("a\
b") + 'c\
' - 'c'; }
EOF

# The current execution wide-character representation is int. Wide character
# constants therefore share ordinary scalar expression and ICE lowering.
try_ 3 << EOF
enum { wide_a = L'A' };
static int wide_b = L'\x42';
int main(void) { return (wide_a == 65) + (wide_b == 66) + (L'C' == 67); }
EOF
try_ 1 << EOF
#define WIDE_A L'A'
int main(void) { return WIDE_A == 65; }
EOF

try_ 0 << EOF
int main(void) {
    wchar_t values[] = L"ab";
    return sizeof L"ab" != 3 * sizeof(wchar_t) ||
           values[0] != 'a' || values[1] != 'b' || values[2] != 0;
}
EOF

try_ 0 << EOF
struct item { wchar_t text[3]; };
int main(void) {
    struct item local = { L"ab" };
    static struct item saved = { .text = L"ok" };
    return local.text[1] != 'b' || saved.text[0] != 'o' ||
           saved.text[2] != 0;
}
EOF

try_ 0 << EOF
typedef wchar_t wide_unit;
int main(void) {
    wide_unit values[] = L"x";
    return values[0] != 'x' || values[1] != 0;
}
EOF

# A string literal initializes a whole innermost row of a multidimensional
# character array, zero-padded, and never stores its address into a char. The
# block-scope case dirties the stack first so that the padding is observable.
try_ 0 << 'EOF'
struct names { int id; char name[2][8]; };
typedef char label[4];
char g_s[2][4] = {"ab", "cd"};
char g_t[][3] = {"ab", "c"};
char g_u[2][4] = {[1] = "xy"};
struct names g_n = {7, {"alpha", "beta"}};
wchar_t g_w[2][3] = {L"a", L"bc"};
char g_p[2][2][3] = {{"ab", "c"}, {{"d"}}};
label g_l[2] = {"ab", {"cd"}};
static const char want_s[8] = {'a', 'b', 0, 0, 'c', 'd', 0, 0};
static const char want_t[6] = {'a', 'b', 0, 'c', 0, 0};
static const char want_u[8] = {0, 0, 0, 0, 'x', 'y', 0, 0};
static const char want_n[16] = {'a', 'l', 'p', 'h', 'a', 0, 0, 0,
                                'b', 'e', 't', 'a', 0,   0, 0, 0};
static const char want_p[12] = {'a', 'b', 0, 'c', 0, 0, 'd', 0, 0, 0, 0, 0};
int same(const char *p, const char *want, int n)
{
    for (int i = 0; i < n; i++)
        if (p[i] != want[i])
            return 0;
    return 1;
}
int check(const char *s, const char *t, int tsize, const char *u,
          const struct names *n, const wchar_t *w, const char *p)
{
    if (tsize != 6 || !same(s, want_s, 8) || !same(t, want_t, 6))
        return 1;
    if (!same(u, want_u, 8))
        return 2;
    if (n->id != 7 || !same(n->name[0], want_n, 16))
        return 3;
    if (w[0] != 'a' || w[1] || w[2] || w[3] != 'b' || w[4] != 'c' || w[5])
        return 4;
    if (!same(p, want_p, 12))
        return 5;
    return 0;
}
void dirty(void)
{
    char junk[256];
    for (int i = 0; i < 256; i++)
        junk[i] = 'Z';
}
int block(void)
{
    char s[2][4] = {"ab", "cd"};
    char t[][3] = {"ab", "c"};
    char u[2][4] = {[1] = "xy"};
    struct names n = {7, {"alpha", "beta"}};
    wchar_t w[2][3] = {L"a", L"bc"};
    char p[2][2][3] = {{"ab", "c"}, {{"d"}}};
    label l[2] = {"ab", {"cd"}};
    if (!same(l[0], want_s, 8))
        return 6;
    return check(s[0], t[0], sizeof t, u[0], &n, w[0], p[0][0]);
}
int block_static(void)
{
    static char s[2][4] = {"ab", "cd"};
    static char t[][3] = {"ab", "c"};
    static char u[2][4] = {[1] = "xy"};
    static struct names n = {7, {"alpha", "beta"}};
    static wchar_t w[2][3] = {L"a", L"bc"};
    static char p[2][2][3] = {{"ab", "c"}, {{"d"}}};
    return check(s[0], t[0], sizeof t, u[0], &n, w[0], p[0][0]);
}
int main(void)
{
    int rc = check(g_s[0], g_t[0], sizeof g_t, g_u[0], &g_n, g_w[0],
                   g_p[0][0]);
    if (rc || !same(g_l[0], want_s, 8))
        return rc + 6;
    dirty();
    rc = block();
    if (rc)
        return rc + 10;
    rc = block_static();
    return rc ? rc + 20 : 0;
}
EOF
try_compile_error_message "String literal initializer has incompatible array element type" << 'EOF'
int values[2][3] = {"ab"};
int main(void) { return 0; }
EOF
try_compile_error_message "String literal cannot initialize a single array element" << 'EOF'
int main(void) { char text[2][4] = {1, "abc"}; return 0; }
EOF

# Braces around a string literal are allowed only when it is the character
# array's whole initializer; it cannot initialize one element of any array.
try_compile_error_message "String literal cannot initialize a single array element" << 'EOF'
char text[3] = {"ab", "c"};
int main(void) { return text[0]; }
EOF
try_compile_error_message "String literal initializer has incompatible array element type" << 'EOF'
int main(void) { int values[3] = {"ab"}; return values[0]; }
EOF

# A string literal is an array lvalue and takes a subscript directly; sizeof
# then measures the element and & yields its address.
try_ 0 << 'EOF'
int id(int x) { return x; }
int main(void) {
    char c = "ab"[1];
    char s[3] = {"ab"[1], 'c'};
    const char *p = &"xyz"[1];
    int i = 2;
    if (c != 'b' || s[0] != 'b' || s[1] != 'c' || s[2])
        return 1;
    if (p[0] != 'y' || p[1] != 'z' || "hello"[i] != 'l')
        return 2;
    if (sizeof "ab"[1] != 1 || sizeof("abc"[0]) != 1 ||
        sizeof L"ab"[0] != sizeof(wchar_t))
        return 3;
    if (L"hi"[1] != 'i' || -"\x05"[0] != -5 || id("xyz"[2]) != 'z' ||
        "ab" "cd"[3] != 'd')
        return 4;
    return 0;
}
EOF

# In a static initializer an element of a narrow string literal is an arithmetic
# constant and &"ab"[1] an address constant, as gcc accepts; an element of a
# wide literal is not a constant there, nor in an enumerator.
try_ 0 << 'EOF'
struct s { char k; const char *s; int v; };
#define DECLS(S)                                             \
    S char v1 = "ab"[1];                                      \
    S int v2 = 1 + "abc"[2];                                  \
    S char v3[3] = {"xy"[0], "xy"[1], "xy"[2]};               \
    S const char *v4 = &"hello"[3];                           \
    S const char *v5 = &"hi"[0];                              \
    S int v6[2] = { 1 + "ab"[1], "cd"[0] * 2 };               \
    S const wchar_t *v7 = &L"wide"[1];                        \
    S struct s v8 = { "pq"[1], &"pq"[1], "a" "b"[1] };        \
    S const char *v9[2] = { &"uv"[1], &"uv"[2] };               \
    S int v10[2] = { "\xff"[0], "\x80"[0] + 0 };
#define CHECKS                                                     \
    int r = 0;                                                     \
    if (v1 != 'b' || v2 != 1 + 'c')                                \
        r |= 1;                                                    \
    if (v3[0] != 'x' || v3[1] != 'y' || v3[2])                     \
        r |= 2;                                                    \
    if (*v4 != 'l' || *v5 != 'h')                                  \
        r |= 4;                                                    \
    if (v6[0] != 1 + 'b' || v6[1] != 'c' * 2 || *v7 != 'i')        \
        r |= 8;                                                    \
    if (v8.k != 'q' || *v8.s != 'q' || v8.v != 'b')                \
        r |= 16;                                                   \
    if (*v9[0] != 'v' || *v9[1])                                   \
        r |= 32;                                                   \
    if (v10[0] != -1 || v10[1] != -128)                            \
        r |= 64;                                                   \
    return r;
DECLS(static)
int check_file(void) { CHECKS }
int check_block(void) { DECLS(static) CHECKS }
int main(void) { return check_file() | check_block() << 7; }
EOF
try_compile_error_message "Wide string literal element is not a constant" << 'EOF'
int main(void) { static int w = L"ab"[1]; return w; }
EOF
try_compile_error << 'EOF'
enum { E = "ab"[1] };
int main(void) { return E; }
EOF

# A string literal plus or minus an integer constant is an address constant (C99
# 6.6p7), on either side of the '+', for a pointer, a record member and an array
# element, in static storage and in automatic storage alike.
try_ 0 << 'EOF'
struct s { int k; const char *s; const char *t; };
enum { TWO = 2 };
#define DECLS(S)                                                \
    S const char *v1 = "abc" + 1;                                \
    S char *v2 = 2 + "abcd";                                     \
    S const char *v3 = "abcdef" + 4 - 2;                         \
    S const char *v4 = 1 * 2 + "wxyz" - 1;                       \
    S const wchar_t *v5 = L"wide" + TWO;                         \
    S struct s v6 = { 1, "pq" + 1, (1 + 1) + "rst" };            \
    S const char *v7[3] = { "uv" + 1, 1 + "uv", "ab" "cd" + 3 }; \
    S const char *v8 = "same" + 0;                               \
    S char *v9 = &"ab"[1];
#define CHECKS                                                     \
    int r = 0;                                                     \
    if (*v1 != 'b' || *v2 != 'c')                                  \
        r |= 1;                                                    \
    if (*v3 != 'c' || *v4 != 'x')                                  \
        r |= 2;                                                    \
    if (*v5 != 'd')                                                \
        r |= 4;                                                    \
    if (v6.k != 1 || *v6.s != 'q' || *v6.t != 't')                 \
        r |= 8;                                                    \
    if (*v7[0] != 'v' || *v7[1] != 'v' || *v7[2] != 'd')           \
        r |= 16;                                                   \
    if (*v8 != 's' || *v9 != 'b')                                  \
        r |= 32;                                                   \
    return r;
DECLS(static)
int check_file(void) { CHECKS }
int check_block(void) { DECLS(static) CHECKS }
int check_local(void) { DECLS() CHECKS }
int main(void) {
    return check_file() | check_block() << 6 | check_local() << 12;
}
EOF

try_ 0 << EOF
enum { wide_count = sizeof L"ab" / sizeof(wchar_t) };
wchar_t *global_pointer = L"xy";
int main(void) {
    L"statement";
    return wide_count != 3 || global_pointer[1] != 'y';
}
EOF

try_ 0 << EOF
int main(void) {
    wchar_t values[] = L"\u00e9";
    return sizeof values != 2 * sizeof(wchar_t) || values[0] != 0xe9 ||
           values[1] != 0;
}
EOF
try_ 0 << EOF
enum { wide_ucn = L'\u00e9' };
int main(void) { return wide_ucn != 0xe9 || L'\u00e9' != 0xe9; }
EOF
try_flags 0 "--no-libc" << EOF
#if L'\u00e9' != 0xe9
#error wide UCN preprocessing value is incorrect
#endif
int main(void) { return 0; }
EOF

# A hexadecimal or octal escape in a wide constant keeps its whole value, which
# only a narrow constant must fit in a byte, on every path that evaluates one.
try_ 0 << 'EOF'
#if L'\x1234' != 0x1234 || L'\777' != 0777
#error wide escape preprocessing value is incorrect
#endif
enum { wide_hex = L'\x1234' };
int global_hex = L'\x12345678';
int global_units[] = {L'\777', L'\x7fffffff'};
int main(void)
{
    int local = L'\x1234';
    int units[] = L"\x1234\777";

    switch (local) {
    case L'\x1234':
        break;
    default:
        return 1;
    }
    return wide_hex != 0x1234 || global_hex != 0x12345678 ||
           global_units[0] != 0777 || global_units[1] != 0x7fffffff ||
           units[0] != L'\x1234' || units[1] != L'\777';
}
EOF
try_compile_error_message "Invalid wide character escape sequence" << 'EOF'
int main(void) { return L'\x100000000'; }
EOF
try_compile_error_message "Invalid wide character escape sequence" << 'EOF'
#if L'\x100000000'
#endif
int main(void) { return 0; }
EOF
try_ 0 << EOF
struct values { int code; };
struct values global_values = {L'\u00e9'};
int main(void) { return global_values.code != 0xe9; }
EOF
try_ 0 << EOF
int global_values[] = {L'\u00e9'};
int main(void) { return global_values[0] != 0xe9; }
EOF
try_ 0 << EOF
int main(void) { return sizeof L'\u00e9' != sizeof(int); }
EOF
try_ 0 << EOF
int global_size = sizeof(L'\u00e9');
int main(void) { return global_size != sizeof(int); }
EOF
try_ 0 << EOF
int main(void) { return L'a\u00e9' != 0x61c3a9; }
EOF

try_ 0 << EOF
#define LEFT L"a"
#define RIGHT L"b"
int main(void) {
    wchar_t values[] = LEFT RIGHT;
    return sizeof(LEFT RIGHT) != 3 * sizeof(wchar_t) || values[1] != 'b';
}
EOF

# A narrow literal next to a wide one joins it, and the result is wide.
try_ 0 << 'EOF'
#define NARROW "n"
wchar_t global_values[] = "g" L"h";
int main(void) {
    wchar_t left[] = L"a" "b";
    wchar_t right[] = "c" L"d" "e";
    wchar_t *macro = NARROW L"w";
    wchar_t *escape = "\x1" L"2";
    return (sizeof left != 3 * sizeof(wchar_t) || left[1] != 'b') +
           (sizeof right != 4 * sizeof(wchar_t) || right[0] != 'c' ||
            right[2] != 'e' || right[3] != 0) *
               2 +
           (sizeof("x" L"y") != 3 * sizeof(wchar_t)) * 4 +
           (macro[0] != 'n' || macro[1] != 'w') * 8 +
           (escape[0] != 1 || escape[1] != '2') * 16 +
           (global_values[0] != 'g' || global_values[1] != 'h') * 32;
}
EOF

try_compile_error << EOF
int main(void) { char values[] = "a" L"b"; return 0; }
EOF

try_compile_error << EOF
int main(void) { char values[] = L"x"; return 0; }
EOF

try_compile_error << EOF
int main(void) { wchar_t values[] = "x"; return 0; }
EOF

try_compile_error << EOF
int main(void) { wchar_t values[] = L"\U00110000"; return 0; }
EOF

try_compile_error << EOF
int main(void) { wchar_t values[] = L"\xFFFFFFFF"; return 0; }
EOF

try_compile_error << EOF
int main(void) { wchar_t values[2][3] = L"ab"; return 0; }
EOF

try_compile_error << EOF
struct item { wchar_t values[2][3]; };
int main(void) { struct item value = { L"ab" }; return 0; }
EOF

try_ 0 << EOF
wchar_t global_values[] = L"go";
int main(void) {
    static wchar_t local_values[] = L"ok";
    return global_values[1] != 'o' || global_values[2] != 0 ||
           local_values[0] != 'o' || local_values[1] != 'k';
}
EOF

try_ 0 << EOF
int main(void) {
    wchar_t fixed[5] = L"A\0B";
    return fixed[0] != 'A' || fixed[1] != 0 || fixed[2] != 'B' ||
           fixed[3] != 0 || fixed[4] != 0;
}
EOF

# A narrow string literal keeps every byte after an embedded null character,
# including across adjacent literals, in array, member and pointer uses.
try_ 10 << EOF
struct holder { char text[6]; int tag; };
struct holder global_member = { "a\0" "bc", 7 };
char global_array[] = "a\0bc";
int main(void) {
    struct holder local_member = { "a\0bc", 9 };
    char local_array[6] = "x\0" "yz";
    char *pointer = "p\0q";
    return (global_member.text[2] == 'b') + (global_member.text[3] == 'c') +
           (global_member.tag == 7) + (sizeof(global_array) == 5) +
           (global_array[3] == 'c') + (local_member.text[3] == 'c') +
           (local_member.tag == 9) + (local_array[2] == 'y') +
           (local_array[3] == 'z') + (pointer[2] == 'q');
}
EOF

# An array with room for the characters of its string literal but not the
# terminating null takes the characters alone (C99 6.7.8p14), for arrays, record
# members and rows at every storage duration. The literal may also be enclosed
# in braces, including in a compound literal. The neighbour must stay intact,
# and a literal with more characters than elements is still an error.
try_ 0 << EOF
struct R { char n[2]; int k; };
char gs[2] = "ab";
char guard = 'q';
static char gss[2] = {"cd"};
char grows[2][2] = {"ef", "g"};
struct R gr = {"hi", 5};
wchar_t gw[2] = L"jk";
int main(void) {
    char s[3] = "lm", t[2] = "no";
    static char ss[2] = "pq";
    struct R r = {"rs", 7}, rw[2] = {"tu", 1, "vw", 2};
    char rows[][2] = {"xy", "z"};
    wchar_t w[2] = {L"AB"};
    char *cl = (char[2]){"CD"};
    if (gs[0] != 'a' || gs[1] != 'b' || guard != 'q' || gss[1] != 'd' ||
        grows[0][1] != 'f' || grows[1][0] != 'g' || grows[1][1] ||
        gr.n[1] != 'i' || gr.k != 5 || gw[0] != 'j' || gw[1] != 'k')
        return 1;
    if (t[0] != 'n' || t[1] != 'o' || s[1] != 'm' || s[2] || ss[1] != 'q' ||
        r.n[0] != 'r' || r.n[1] != 's' || r.k != 7 || rw[1].n[1] != 'w' ||
        rw[1].k != 2)
        return 2;
    return sizeof rows != 4 || rows[0][1] != 'y' || rows[1][1] ||
           w[1] != 'B' || cl[0] != 'C' || cl[1] != 'D';
}
EOF
try_compile_error << EOF
char s[1] = "ab";
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { char s[1] = {"ab"}; return s[0]; }
EOF
try_compile_error << EOF
int main(void) { static char s[1] = "ab"; return s[0]; }
EOF
try_compile_error << EOF
struct R { char n[1]; } r = {"ab"};
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { char r[2][1] = {"a", "cd"}; return r[0][0]; }
EOF
try_compile_error << EOF
int main(void) { wchar_t w[1] = L"ab"; return w[0]; }
EOF

# Without braces, string literals meeting a record member that is an array of
# pointers initialize its elements in turn. They are values for the pointers,
# not the contents of a character array, so the list continues to the next
# member instead of reporting too many initializers or an element type error.
try_ 0 << EOF
typedef char *str;
struct T { char *n[2]; int k; } gt = {"s", "t", 4};
struct T2 { str n[2]; int k; };
struct M { str n[2][2]; int k; } gm = {"a", "b", "c", "d", 3};
struct T2 gt2 = {"u", "v", 5}, gta[2] = {"a", "b", 1, "c", "d", 2};
int main(void) {
    struct T t = {"s", "t", 4};
    static struct T2 st2 = {"y", "z", 6};
    struct T2 ta[2] = {"a", "b", 1, "c", "d", 2};
    struct M m = {"e", "f", "g", "h", 7};
    if (*gt.n[1] != 't' || gt.k != 4 || *gt2.n[1] != 'v' || gt2.k != 5 ||
        *gta[1].n[0] != 'c' || gta[1].k != 2 || *gm.n[1][0] != 'c' ||
        gm.k != 3)
        return 1;
    return *t.n[0] != 's' || t.k != 4 || *st2.n[1] != 'z' || st2.k != 6 ||
           *ta[1].n[1] != 'd' || ta[1].k != 2 || *m.n[1][1] != 'h' ||
           m.k != 7;
}
EOF

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

# A cast to _Bool compares with zero rather than truncating, for narrow, long
# long and pointer operands alike, as do _Bool arguments and returns.
try_output 0 "1 1 1 1 1 0 0 2 1 1 1 1 1 1 1 1" << EOF
_Bool take(_Bool value) { return value; }
_Bool from_int(int value) { return value & 0x100; }
_Bool from_wide(long long value) { return value; }
_Bool from_pointer(char *value) { return value; }
_Bool (*callback)(_Bool) = take;
int main(void)
{
    int x = 0x1c1;
    short half = 0x100;
    long long wide = 0x100000000LL;
    unsigned long long top = 0x8000000000000000ULL;
    char *pointer = (char *) &x, *null = 0;
    printf("%d %d %d %d %d %d %d ", (_Bool) (x & 0x100), (_Bool) half,
           (_Bool) wide, (_Bool) top, (_Bool) pointer, (_Bool) null,
           (_Bool) (x & 0x200));
    printf("%d %d ", (_Bool) wide + (_Bool) 256, (_Bool) 0x100000000LL);
    printf("%d %d %d %d ", take(x & 0x100), take(wide), take(pointer),
           callback(wide));
    printf("%d %d %d", from_int(x), from_wide(wide), from_pointer(pointer));
    return 0;
}
EOF

# A function passed for, assigned to, or initializing a pointer to a function
# returning _Bool stays an address: only _Bool objects convert to 0 or 1.
try_ 0 << EOF
_Bool odd(int v) { return v & 1; }
_Bool call(_Bool (*m)(int), int v) { if (m(v)) return 1; return 0; }
_Bool (*global_ptr)(int) = odd;
int main(void)
{
    _Bool (*local_ptr)(int) = odd;
    _Bool (*assigned)(int);
    assigned = odd;
    return !call(odd, 3) | !call(local_ptr, 5) << 1 | !assigned(7) << 2 |
           !global_ptr(9) << 3 | call(odd, 2) << 4;
}
EOF

# Stores into _Bool objects reached through subscripts, pointers and members,
# and objects declared through a _Bool typedef, keep only the truth value.
try_output 0 "1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1" << EOF
typedef _Bool flag;
struct holder {
    int pad;
    _Bool plain;
    flag alias;
};
struct holder global_holder = {1, 256, 512};
flag global_flag = 256;
int main(void)
{
    int x = 0x1c1;
    long long wide = 0x100000000LL;
    char *pointer = (char *) &x;
    _Bool values[3] = {0, 0, 0}, *p = values, grid[2][2];
    _Bool (*row)[3] = &values;
    struct holder h = {1, x & 0x100, wide}, *hp = &h;
    flag local = x & 0x100;
    printf("%d %d %d %d %d ", global_holder.alias, global_flag, h.plain,
           h.alias, local);
    local = wide;
    printf("%d %d %d ", local, (flag) (x & 0x100), (flag) pointer);
    values[1] = x & 0x100;
    printf("%d ", values[1]);
    p[2] = wide;
    printf("%d ", values[2]);
    values[2] = 0;
    *(p + 2) = x & 0x100;
    printf("%d ", values[2]);
    grid[1][1] = x & 0x100;
    printf("%d ", grid[1][1]);
    (*row)[0] = x & 0x100;
    printf("%d ", values[0]);
    h.plain = 0;
    hp->alias = pointer;
    printf("%d %d ", hp->alias, (h.plain = wide));
    h.plain = x & 0x100 ? x & 0x100 : 0;
    printf("%d ", h.plain);
    values[0] = 0;
    *p++ = x & 0x100;
    printf("%d", values[0]);
    return 0;
}
EOF

# C99 integer promotions convert _Bool to int before arithmetic. This also
# exercises the common unsigned type when that promoted result meets unsigned
# int, rather than treating a one-byte _Bool as an arithmetic byte.
try_ 31 << EOF
int main(void) {
    _Bool one = 1;
    _Bool zero = 0;
    unsigned int unsigned_one = 1U;
    return ((one + one) == 2) +
           2 * ((one - zero) == 1) +
           4 * ((one << 3) == 8) +
           8 * ((one + unsigned_one) == 2U) +
           16 * ((zero - one) < 0);
}
EOF

# Volatile accesses must remain observable through qualified pointers and must
# not be replaced with a cached direct-object value across a call boundary.
try_ 31 << EOF
volatile int global_slot;
void write_volatile(volatile int *slot, int value) { *slot = value; }
int bump_volatile(volatile int *slot) {
    int before = *slot;
    *slot = before + 1;
    return *slot;
}
int main(void) {
    volatile int local_slot = 3;
    write_volatile(&local_slot, 9);
    write_volatile(&global_slot, 20);
    return (bump_volatile(&local_slot) == 10) +
           2 * (local_slot == 10) +
           4 * (bump_volatile(&global_slot) == 21) +
           8 * (global_slot == 21) +
           16 * (((volatile int *) &local_slot) == &local_slot);
}
EOF

# Accessing a volatile object is a side effect (C99 5.1.2.3p2, 6.7.3p6), so an
# expression evaluated only for its side effects still reads it: a name used as
# a statement, the left operand of a comma, a cast to void, a for clause. Each
# evaluated read reaches the generated code, and no load of the same object
# stands in for it.
try_ir_count 5 f 'load %x[0-9]+, -?[0-9]+\(gp\)' << EOF
volatile int status;
int f(void)
{
    status;
    status;
    (void) status;
    status, 0;
    for (status;;)
        break;
    return 1;
}
int main(void) { return f() - 1; }
EOF

# A postfix update of a volatile object reads it once, where it is evaluated.
try_ir_count 2 f 'load %x[0-9]+, -?[0-9]+\(gp\)' << EOF
volatile int status;
int f(void)
{
    status++;
    status--, 0;
    return 1;
}
int main(void) { return f() - 1; }
EOF

# A volatile local lives in its slot: storing it does not let the next read
# reuse the stored register.
try_ir_count 2 f 'load %x[0-9]+, -?[0-9]+\(sp\)' << EOF
int f(void)
{
    volatile int local = 1;
    local;
    local;
    return 0;
}
int main(void) { return f(); }
EOF

# Reads through a pointer to volatile, of a member of a volatile record and of
# an element of a volatile array are kept too, even when nothing uses them.
try_ir_count 4 f '= \(%x[0-9]+\)' << EOF
struct S { int a; int b; };
volatile int *p;
volatile struct S s;
volatile int a[4];
volatile struct S *sp;
int f(void)
{
    *p;
    s.b;
    a[2];
    sp->a;
    return 7;
}
int main(void) { return f() - 7; }
EOF

# A volatile parameter lives in its slot as a volatile local does: it is stored
# there on entry, each read reloads it and each assignment stores it, the
# register the value was just computed in notwithstanding.
try_ir_count 2 f 'load %x[0-9]+, -?[0-9]+\(sp\)' << EOF
int f(volatile int x)
{
    x;
    x;
    return 0;
}
int main(void) { return f(1); }
EOF
try_ir_count 2 f 'store %x[0-9]+, -?[0-9]+\(sp\)' << EOF
int f(void)
{
    volatile int local = 4;
    local = local + 1;
    return local;
}
int main(void) { return f() - 5; }
EOF

# A 32-bit target holds a long long in two registers, and a volatile one is read
# and written a whole pair at a time.
if [ "$PTR_SZ" = 4 ]; then
    LL_REG='%x[0-9]+:%x[0-9]+'
else
    LL_REG='%x[0-9]+'
fi
try_ir_count 1 f "load $LL_REG, -?[0-9]+\\(gp\\)" << EOF
volatile long long wide;
int f(void)
{
    wide;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 3 f "load $LL_REG, -?[0-9]+\\(sp\\)" << EOF
int f(volatile long long y)
{
    y;
    y = y + 1;
    return (int) y;
}
int main(void) { return f(3) - 4; }
EOF
try_ir_count 2 f "store $LL_REG, -?[0-9]+\\(sp\\)" << EOF
int f(volatile long long y)
{
    y;
    y = y + 1;
    return (int) y;
}
int main(void) { return f(3) - 4; }
EOF
try_ 10 << EOF
volatile long long vw;
int f1(volatile int x) { x; x; return x; }
int f2(volatile int x) { x = x + 1; return x; }
int f3(void) { volatile int lv = 4; lv = lv + 1; return lv; }
int f4(volatile long long y) { y; y = y + 0x100000000LL; return (int) (y >> 32); }
int main(void)
{
    vw = 0x200000001LL;
    return f1(1) + f2(2) + f3() + f4(0x100000003LL) - (int) (vw >> 32) + 1;
}
EOF

# Every write to a volatile object reaches memory, the same value written again
# and an object nothing reads by name included.
try_ir_count 3 f 'store %x[0-9]+, -?[0-9]+\(gp\)' << EOF
volatile int control;
int f(void)
{
    control = 1;
    control = 1;
    control = 1;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 3 f 'store %x[0-9]+, -?[0-9]+\(sp\)' << EOF
int f(void)
{
    volatile int local;
    local = 1;
    local = 1;
    local = 1;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 4 f 'store %x[0-9]+, -?[0-9]+\(sp\)' << EOF
int f(volatile int x)
{
    x = 1;
    x = 1;
    x = 1;
    return 0;
}
int main(void) { return f(0); }
EOF
try_ir_count 5 f '\(%x[0-9]+\) = %x[0-9]+' << EOF
struct S { int a; int b; };
volatile int *p;
volatile struct S s;
int f(void)
{
    *p = 1;
    *p = 1;
    *p = 1;
    s.b = 1;
    s.b = 1;
    return 0;
}
int main(void)
{
    static int backing;
    p = &backing;
    return f();
}
EOF
try_ir_count 3 f "store $LL_REG, -?[0-9]+\\(gp\\)" << EOF
volatile long long wide;
int f(void)
{
    wide = 1;
    wide = 1;
    wide = 1;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 3 f "store $LL_REG, -?[0-9]+\\(sp\\)" << EOF
int f(void)
{
    volatile long long local;
    local = 1;
    local = 1;
    local = 1;
    return 0;
}
int main(void) { return f(); }
EOF
try_ 7 << EOF
volatile long long wide;
void f(void)
{
    wide = 0x500000006LL;
    wide = 0x100000002LL;
    wide = 0x300000004LL;
}
int main(void)
{
    f();
    return (int) (wide >> 32) + (int) wide;
}
EOF

# A discarded assignment stores and does not read the object back: C11 6.5.16p3
# permits the read but does not require it. Storing a bit-field still reads the
# unit once, for the bits it keeps.
try_ir_count 0 f 'load %x[0-9]+, -?[0-9]+\((gp|sp)\)' << EOF
volatile int v;
int f(void)
{
    volatile int local;
    v = 3;
    local = 3;
    v = 4, local = 4;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 1 f '= \(%x[0-9]+\)' << EOF
volatile struct S { int a; int b : 3; } s;
int f(void)
{
    s.a = 3;
    s.b = 1;
    return 0;
}
int main(void) { return f(); }
EOF
try_ 2 << EOF
struct S { int a; int b : 3; };
volatile struct S s;
int main(void)
{
    int x = (s.b = 5);
    s.a = 7;
    return (x == -3) + (s.b == -3) - (s.a != 7);
}
EOF

# Only evaluated operands are read, each once. The value of "x = v" is not a
# second read of v, the operand of sizeof is not read at all, and assigning
# through a pointer to volatile need not read the object back.
try_ir_count 1 f 'load %x[0-9]+, -?[0-9]+\(gp\)' << EOF
volatile int v;
int x;
int f(void)
{
    x = v;
    sizeof v;
    return 0;
}
int main(void) { return f(); }
EOF
try_ir_count 0 f '= \(%x[0-9]+\)' << EOF
volatile int *p;
int f(void)
{
    *p = 1;
    return 0;
}
int main(void) { return f(); }
EOF

# Reading those objects still yields their values.
try_ 36 << EOF
volatile int vg;
volatile int *vp;
struct S { int a; int b; };
volatile struct S vs;
volatile struct S *vsp;
volatile char varr[4];
int f1(void) { vg; return 1; }
int f2(void) { vg; vg; return 2; }
int f3(void) { volatile int lv = 3; lv; lv; return lv; }
int f4(int c) { int x = 0; if (c) x = vg; return x + 4; }
int f5(void) { vsp->b; varr[1]; (void) vg; return (vg, 5); }
int f6(void) { for (vg; vg; vg) break; return 6; }
int main(void)
{
    static volatile int backing[2];
    vp = &backing[1];
    vsp = &vs;
    *vp;
    vg = 7;
    vs.a = 1;
    return f1() + f2() + f3() + f4(1) + f5() + f6() + vs.a + vg;
}
EOF

# A file-scope record keeps its volatile qualifier like a scalar does, so every
# redeclaration of either must repeat it.
try_ 8 << EOF
struct S { int a; };
union U { int a; char c; };
extern volatile struct S s;
volatile struct S s = { 3 };
extern volatile union U u;
volatile union U u, u2;
volatile struct R { int a; } r1, r2;
extern volatile struct R r2;
volatile int *p, q;
extern volatile int *p;
extern volatile int q;
int main(void) {
    s.a++;
    u.a = 4;
    r2.a = s.a;
    return r2.a + u.a;
}
EOF
try_compile_error << EOF
struct S { int a; };
volatile struct S s;
extern struct S s;
int main(void) { return 0; }
EOF
try_compile_error << EOF
volatile union U { int a; } u;
extern union U u;
int main(void) { return 0; }
EOF
try_compile_error << EOF
volatile int value;
extern int value;
int main(void) { return 0; }
EOF

# An omitted outer array bound is compatible with any bound on the same element
# type, in either order, and the bound completes the shared object.
try_ 0 << EOF
extern int a[];
int a[3] = {1, 2, 3};
int b[];
int guard_b = 5;
int b[4];
int c[3] = {7, 8, 9};
extern int c[];
int guard_c = 6;
extern int m[][2];
int m[3][2] = {{1, 2}, {3, 4}, {5, 6}};
int main(void) {
    for (int i = 0; i < 4; i++)
        b[i] = 11;
    return sizeof(a) != 3 * sizeof(int) || a[2] != 3 ||
           sizeof(b) != 4 * sizeof(int) || guard_b != 5 || b[3] != 11 ||
           sizeof(c) != 3 * sizeof(int) || c[2] != 9 || guard_c != 6 ||
           sizeof(m) != 6 * sizeof(int) || m[2][1] != 6;
}
EOF
try_compile_error << EOF
int d[3];
int d[4];
int main(void) { return 0; }
EOF
try_compile_error << EOF
extern int m[][2];
int m[3][3];
int main(void) { return 0; }
EOF
try_compile_error << EOF
int e;
extern int e[];
int main(void) { return 0; }
EOF
try_compile_error << EOF
extern int f[];
int *f;
int main(void) { return 0; }
EOF

# Qualifiers may also follow the type specifier, in typedefs and at both scopes,
# and a typedef passes them to every object it declares. The redeclarations
# below only match when each spelling recorded the volatile qualifier.
try_ 60 << EOF
struct S { int a; };
enum E { E2 = 2 };
typedef volatile int VI;
typedef int volatile VI2;
typedef const volatile int CVI;
typedef VI VI3;
typedef struct S volatile VS;
typedef volatile struct S VS2;
typedef struct { int a; } const CR;
typedef struct T { int a; } volatile VT;
typedef enum { E4 = 4 } volatile VE;
typedef int *P;
typedef P restrict RP;
struct S volatile vs;
extern volatile struct S vs;
union U { int a; } volatile vu;
extern volatile union U vu;
enum E volatile ve = E2;
extern volatile enum E ve;
int volatile vi = 1;
extern VI vi;
VI2 vi2 = 2;
extern volatile int vi2;
VI3 vi3 = 3;
extern volatile int vi3;
VS vrec;
extern struct S volatile vrec;
VS2 vrec2;
extern VS vrec2;
struct S const cs = {5};
int main(void)
{
    extern struct S volatile vrec;
    struct S volatile ls, *lp = &ls;
    struct R { int a; } volatile const lr = {6};
    enum E const le = E2;
    CR cr = {7};
    struct T t = {1};
    VT vt = {8};
    VE e = E4;
    CVI cv = 9;
    int x = 3;
    RP rp = &x;
    typedef struct S const LCS;
    LCS lcs = {2};
    t.a = 2;
    vs.a = 1;
    vu.a = 2;
    vrec.a = 3;
    vrec2.a = 4;
    ls.a = 1;
    return vs.a + vu.a + ve + vi + vi2 + vi3 + vrec.a + vrec2.a + cs.a +
           lp->a + lr.a + le + cr.a + t.a + vt.a + e + cv + *rp + lcs.a - 7;
}
EOF
try_compile_error << EOF
struct S { int a; };
struct S volatile s;
extern struct S s;
int main(void) { return 0; }
EOF
try_compile_error << EOF
enum E { E1 };
enum E volatile e;
extern enum E e;
int main(void) { return 0; }
EOF
try_compile_error << EOF
typedef int volatile VI;
VI value;
extern int value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct S { int a; };
typedef struct S volatile VS;
VS value;
extern struct S value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct S { int a; };
struct S value;
int main(void) { extern struct S volatile value; return 0; }
EOF
try_compile_error << EOF
typedef int const CI;
int main(void) { CI value = 1; value = 2; return value; }
EOF
try_compile_error << EOF
struct S { int a; };
typedef struct S const CS;
int main(void) { CS value = {1}; value.a = 2; return value.a; }
EOF
try_compile_error << EOF
struct S { int a; };
struct S const value = {1};
int main(void) { value.a = 2; return value.a; }
EOF
try_compile_error << EOF
enum E { E1 };
int main(void) { enum E const value = E1; value = E1; return value; }
EOF
try_compile_error_message "restrict requires a pointer type" << EOF
struct S { int a; };
struct S restrict value;
int main(void) { return 0; }
EOF
try_compile_error_message "restrict requires a pointer type" << EOF
typedef int restrict R;
int main(void) { return 0; }
EOF
try_ 3 << EOF
struct S { int a; };
struct S g = {3};
int main(void) { typedef struct S ST; extern ST g; return g.a; }
EOF

# A storage class, typedef included, may follow the type as well (C99 6.7p2;
# obsolescent per 6.11.5 but valid), at file scope, in a block, in a for
# initializer and on a parameter. It still admits no second storage class, and
# an identifier after the type is the declarator, not a specifier.
try_ 16 << EOF
struct S { int a; };
struct S static gs = { 2 };
int typedef T;
T const static k = 1;
long extern y;
long y = 1;
int static f(void) { return 3; }
int g(int register a, const register int b) { return a + b; }
int main(void) {
    T typedef U;
    U u = 1;
    struct S register rs = gs;
    long unsigned static int q = 4;
    long extern int y;
    for (int register i = 0; i < 1; i++)
        u += i;
    struct Q { int b; } static r = {5};
    return u + k + f() + rs.a + q + r.b + g(1, -1) - y + 1;
}
EOF

try_compile_error << EOF
int main(void) { int x static; return 0; }
EOF

try_compile_error_message "duplicate static storage class specifier" << EOF
int main(void) { int static static x; return x; }
EOF

try_compile_error_message "incompatible storage class specifiers" << EOF
int main(void) { unsigned static extern x; return 0; }
EOF

try_compile_error_message "typedef cannot be combined" << EOF
extern int typedef T;
int main(void) { return 0; }
EOF

try_compile_error << EOF
int f(int static a) { return a; }
int main(void) { return f(1); }
EOF

# restrict is a C99 pointer qualifier. These are non-aliasing calls by contract;
# the test covers parser/type-name acceptance and ordinary accesses without
# requiring an alias-sensitive optimization.
try_ 31 << EOF
int accumulate(int *restrict destination, const int *restrict source) {
    *destination += *source;
    return *destination;
}
int main(void) {
    int left = 4;
    int right = 5;
    int *restrict local = &left;
    const int *restrict input = &right;
    int *cast_local = (int *restrict) local;
    return (accumulate(local, input) == 9) +
           2 * (left == 9) +
           4 * (accumulate(cast_local, input) == 14) +
           8 * (*input == 5) +
           16 * (cast_local == &left);
}
EOF
expr 42 42

# octal constant (satisfying re(0[0-7]+))
expr 10 012
expr 65 0101

# Category: C99 universal character names
begin_category "Universal Character Names" "Testing UCN literals and identifiers"

# C99 universal character names in narrow literals are encoded in shecc's UTF-8
# execution character set. Exercise two-, three-, and four-byte sequences.
try_ 4 << EOF
int main(void) {
    char *s = "\\u00a9\\u20ac\\U0001f600";
    return ((unsigned char)s[0] == 0xc2) + ((unsigned char)s[1] == 0xa9) +
           ((unsigned char)s[2] == 0xe2) + ((unsigned char)s[5] == 0xf0);
}
EOF

# Multi-character constants retain the implementation-defined left-to-right
# UTF-8 byte packing used by ordinary escaped constants.
try_ 1 << EOF
int main(void) { return '\\u00a9' == 0xc2a9; }
EOF

# Identifier UCNs are decoded to a stable UTF-8 spelling before keyword,
# typedef, object, and macro lookup. Cover a continuation UCN and a four-byte
# scalar in a macro name as well as a UCN at the start of a typedef name.
try_ 42 << EOF
#define \U0001f600 40
typedef int \u03b1;
\u03b1 value\u00e9 = \U0001f600;
int main(void) { return value\u00e9 + 2; }
EOF

# C99's three exceptions to the basic-source UCN restriction remain UCN
# nondigits, despite their direct spellings not being ordinary identifiers.
try_ 42 << EOF
int \u0024 = 42;
int main(void) { return \u0024; }
EOF

try_compile_error << EOF
int main(void) { return "\\u0041"[0]; }
EOF
try_compile_error << EOF
int main(void) { return "\\uD800"[0]; }
EOF
try_compile_error << EOF
int main(void) { return "\\U00110000"[0]; }
EOF
try_compile_error << EOF
int \u0041 = 0;
EOF
try_compile_error << EOF
int \u12 = 0;
EOF

# Category: Arithmetic Operations
begin_category "Arithmetic Operations" "Testing +, -, *, /, % operators"

# Unary minus negates a grouped or cast operand, not only a name or a literal.
try_ 0 << EOF
int negate_argument(int value) { return value; }
int main(void)
{
    int x = 5;
    char c = 2;

    if (-(x) != -5) return 1;
    if (-(x + 1) != -6) return 2;
    if (negate_argument(-(x)) != -5) return 3;
    if (-(int) c != -2) return 4;
    if (1 + -(x) != -4) return 5;
    return 0;
}
EOF

# Any unary expression may follow a unary minus (C99 6.5.3), another unary
# operator among them: these were rejected as an unexpected token.
try_ 1 << EOF
int arr[3] = {1, 2, 3};
int main(void) {
    int x = 5, *p = &x;
    long long w = 3;
    int r = 0;
    r += (- - x == 5);
    r += (- -x == 5) * 2;
    r += (~ - x == 4) * 4;
    r += (! - x == 0) * 8;
    r += (- ~ x == 6) * 16;
    r += (-*p == -5) * 32;
    r += (-'a' == -97) * 64;
    r += (-(int) sizeof(int) == -4 && -sizeof(char) + 1 == 0) * 128;
    r += (- + x == -5) * 256;
    r += (- ++x == -6 && x == 6) * 512;
    r += (-2[arr] == -3 && -(x) - -1 == -5) * 1024;
    r += (- - - -w == 3 && - - -1 == -1) * 2048;
    r += (-(int) 7 == -7 && -(char) 300 == -44) * 4096;
    r += (-arr[1] == -2 && -x-- == -6) * 8192;
    r += (x == 5) * 16384;
    return r == 32767;
}
EOF

# A postfix update is complete at the next sequence point (C99 6.5.2.4, 6.5.15,
# 6.5.13, 6.5.14, 6.5.2.2p10), and happens only when its operand is evaluated:
# deferring it to the end of the statement ran skipped updates and let the
# second operand of && see the old value.
try_ 0 << EOF
int g;
int seen(int v) { return g * 10 + v; }
int main(void) {
    int x = 7, y = 0, z = 3, w;
    if (!((x-- == 7 && x == 6) ? 1 : 0)) return 1;
    0 && y++;
    1 || y++;
    w = y ? y++ : 5;
    if (y != 0 || w != 5) return 2;
    if ((z++ || z) != 1 || (z++ ? z : 0) != 5) return 3;
    g = 1;
    if (seen(g++) != 21) return 4;
    return 0;
}
EOF

# ++ and -- on a selected element or member step by the pointee of the value it
# holds: a pointer member or element stepped by one byte in postfix form, and an
# int element of a pointer, ++p[0], stepped by sizeof(int) in prefix form.
try_ 0 << EOF
struct rec { char c; int v; };
struct holder { int *ip; struct rec *rp; char **cpp; };
int main(void) {
    int arr[4] = {1, 2, 3, 4};
    struct rec recs[2];
    char *strs[2] = {"a", "b"};
    struct holder h = {arr, recs, strs}, *hp = &h;
    int *ips[2] = {arr, arr};
    int *old = ips[0]++;
    h.ip++;
    hp->rp++;
    h.cpp++;
    int *p = arr;
    ++p[0];
    p[1]--;
    ++h.ip;
    if (old != arr || ips[0] != arr + 1 || *h.ip != 3) return 1;
    if (arr[0] != 2 || arr[1] != 1) return 2;
    return h.rp != recs + 1 || hp->cpp != strs + 1;
}
EOF

# An element of an array of pointers to arrays, or of a pointer to such
# pointers, still points to a whole row, so *pas[0] designates the row; and
# arithmetic on the array or pointer steps over pointers. The dereference was
# rejected, and pas + 1 stepped by a row.
try_ 0 << EOF
struct holder { int (*row)[3]; };
int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
int (*gpas[2])[3] = {m, m + 1};
int main(void)
{
    int (*pas[2])[3] = {m, m + 1};
    int (**pr)[3] = pas;
    int (**q)[3] = pas + 1;
    struct holder h = {m + 1}, *hp = &h;
    int *e = *pas[1];
    if ((*pas[0])[2] != 3 || (*pas[1])[0] != 4 || pas[1][0][1] != 5) return 1;
    if ((*gpas[1])[2] != 6 || (*hp->row)[1] != 5 || (*h.row)[0] != 4) return 2;
    (*pas[1])[2] = 9;
    if (m[1][2] != 9 || sizeof(*pas[0]) != 3 * sizeof(int)) return 3;
    if ((*pr[1])[1] != 5 || (**pr)[2] != 3 || e[2] != 9) return 4;
    if ((**(pas + 1))[1] != 5 || (**q)[0] != 4 || q - pas != 1) return 5;
    if ((**(pr + 1))[2] != 9 || *(*pas[0] + 1) != 2) return 6;
    return 0;
}
EOF

# Compound assignment scales a pointer operand as ++ and -- step it, also when
# the pointer is selected by a member or subscript; there it was not scaled.
try_ 0 << EOF
struct h { int *ip; char **cpp; int (*row)[3]; };
int main(void)
{
    int arr[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    int m[3][3];
    char *strs[4];
    int *ps[2] = {arr, arr};
    int (*pas[1])[3] = {m};
    struct h s = {arr, strs, m}, *sp = &s;
    int *p = arr;
    s.ip += 2;
    sp->cpp += 3;
    ps[1] -= -3;
    s.row += 1;
    pas[0] += 2;
    p += 5;
    p[0] += 10;
    ps[0][1] -= 1;
    if (s.ip != arr + 2 || s.cpp != strs + 3 || ps[1] != arr + 3) return 1;
    if (s.row != m + 1 || pas[0] != m + 2 || p != arr + 5) return 2;
    return arr[5] != 15 || arr[1] != 0;
}
EOF

# The element or member step also covers a typedef'd pointer, which steps by its
# pointee, and a pointer to an array, which steps by a whole row; the latter
# stepped by one element.
try_ 0 << EOF
typedef int *iptr;
typedef struct rec { char c; int v; } *rptr;
typedef iptr *ipptr;
struct rows { int (*row)[3]; iptr ip; };
int main(void)
{
    int arr[4] = {1, 2, 3, 4};
    struct rec recs[2];
    int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
    iptr ips[2] = {arr, arr};
    rptr rps[2];
    rps[0] = recs;
    rps[1] = recs;
    ipptr ipps[1] = {ips};
    int (*pas[2])[3] = {m, m};
    struct rows r = {m, arr}, *rp = &r;
    iptr *old = ipps[0]++;
    ips[0]++;
    ++ips[1];
    ips[1]--;
    rps[1]++;
    pas[0]++;
    --pas[1];
    ++pas[1];
    int (*prev)[3] = pas[1]++;
    r.row++;
    rp->ip++;
    ++r.ip;
    if (ips[0] != arr + 1 || ips[1] != arr || *ips[0] != 2) return 1;
    if (rps[1] != recs + 1 || ipps[0] != ips + 1 || old != ips) return 2;
    if (pas[0] != m + 1 || prev != m || pas[1] != m + 1)
        return 3;
    if (r.row != m + 1 || (*r.row)[0] != 4 || r.ip != arr + 2) return 4;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { int *p = 0; return - -p != 0; }
EOF
try_compile_error << EOF
int main(void) { return - ; }
EOF

# C99 integer promotions and signed/unsigned common-type selection must retain
# the promoted arithmetic result across character, short, int, and long ranks.
try_ 0 << EOF
int main(void) {
    signed char signed_byte = -1;
    unsigned char unsigned_byte = 255;
    short signed_short = -2;
    unsigned short unsigned_short = 65535;
    unsigned int unsigned_int = 1;
    long signed_long = -1;
    unsigned long unsigned_long = 1;
    return signed_byte + unsigned_byte != 254 ||
           signed_short + unsigned_short != 65533 ||
           signed_byte < unsigned_int || signed_long < unsigned_long;
}
EOF

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

# The shift count's unsignedness must not turn a signed right shift into a
# logical one. The left operand alone determines the right-shift opcode.
try_ 0 << EOF
int main(void) {
    int value = -4;
    volatile unsigned int count = 1;
    return (value >> count) != -2;
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

# An int-width result is zero when its low word is, whatever an LP64 backend
# leaves above it, so a truth test, a ternary, a ! and an if all agree.
try_ 0 << EOF
unsigned uid(unsigned x) { return x; }
int iid(int x) { return x; }
int main(void) {
    unsigned u = uid(0xffffffffU);
    unsigned one = uid(1U);
    unsigned top = uid(0x80000000U);
    int s = iid(-1);
    int fails = 0;
    if (~u)
        fails = fails + 1;
    if ((~u) ? 1 : 0)
        fails = fails + 2;
    if (!(~u) != 1)
        fails = fails + 4;
    if (u + one)
        fails = fails + 8;
    if (top - top)
        fails = fails + 16;
    if (top * uid(2U))
        fails = fails + 32;
    if (top << uid(1U))
        fails = fails + 64;
    if ((~u) & u)
        fails = fails + 128;
    if ((~u) | (u + one))
        fails = fails + 256;
    if (u ^ uid(0xffffffffU))
        fails = fails + 512;
    if (~s + 1)
        fails = fails + 1024;
    if ((iid(3) - iid(3)) ? 1 : 0)
        fails = fails + 2048;
    if (!(iid(0) * iid(5)) != 1)
        fails = fails + 4096;
    if (((u + one) >> uid(1U)) != 0)
        fails = fails + 8192;
    if (((iid(-8) + iid(0)) >> 1) != -4)
        fails = fails + 16384;
    return fails;
}
EOF
# A folded ~ or unary minus of an unsigned int constant has a zero high word.
try_ 0 << EOF
int main(void) {
    unsigned long long flipped = (unsigned long long) (~4294967294U);
    unsigned long long negated = (unsigned long long) (-9U);
    unsigned long long grouped = (unsigned long long) -(9U);
    long long signed_flip = (long long) (~5);
    return flipped != 1 || negated != 0xfffffff7ULL ||
           grouped != 0xfffffff7ULL || signed_flip != -6;
}
EOF

# A folded negative int is sign-extended when converted to long long, and a
# folded unsigned int result is not.
try_ 0 << EOF
long long g = (long long) (0 - 42);
int main(void) {
    static unsigned long long s = (unsigned long long) (2 * -21);
    long long a = (long long) (0 - 42);
    unsigned long long b = (unsigned long long) (-45 - 32);
    long long c = (long long) (35 | -38);
    unsigned long long d = (unsigned long long) (-13 ^ 55);
    unsigned long long e = (unsigned long long) ((22 | -3) >> 28);
    unsigned long long u = (unsigned long long) (0U - 42);
    return a != -42 || b != 0xffffffffffffffb3ULL || c != -5 ||
           d != 0xffffffffffffffc4ULL || e != ~0ULL || g != -42 ||
           s != 0xffffffffffffffd6ULL || u != 0xffffffd6ULL ||
           (long long) (0 - 42) >= 0;
}
EOF
# A constant narrowed to an unsigned type folds zero-extended, not to -2.
try_ 0 << EOF
int main(void) {
    unsigned short half = 0xfffe;
    unsigned short wide = 0xfffffffeULL;
    unsigned char byte = 0xfe;
    return (int) half != 65534 || (int) wide != 65534 || (int) byte != 254;
}
EOF

# A conversion between narrow types that changes signedness leaves the result
# extended by its own type, not by its source's.
try_ 0 << EOF
unsigned short widen(signed char c) { unsigned short v = c; return v; }
int main(void) {
    signed char c = -17;
    unsigned char u = 255;
    unsigned short v = c;
    unsigned short z = (unsigned short) ((unsigned char) c);
    unsigned short m = (unsigned short) ((signed char) u);
    unsigned char b = (unsigned char) c;
    signed char s = (signed char) u;
    short h = (signed char) u;
    return (unsigned) v != 0xffef || (unsigned) z != 0xef ||
           (m >> 8) != 0xff || (long long) b != 239 || s != -1 || h != -1 ||
           1000 / (unsigned short) ((unsigned char) c) != 4 ||
           (int) widen(c) != 0xffef;
}
EOF

# A shift takes the type of its left operand alone; a long long count does not
# make an int shift 64 bits wide.
try_ 0 << EOF
int shift_not(unsigned char x, long long n) { return (~x) >> (n & 31); }
int shift_xor(int x, int y, unsigned long long n) { return (x ^ y) >> (n & 31); }
unsigned int shift_out(unsigned int x, unsigned int y, long long n) {
    return (x | y) << n;
}
int main(void) {
    return shift_not(0, 15) != -1 || shift_xor(-8, 0, 2) != -2 ||
           shift_out(0x80000000U, 1, 1) != 2;
}
EOF

# An unsigned int shifted right by a variable count ignores the borrow a 64-bit
# register can hold above an unsigned int subtraction.
try_ 0 << EOF
unsigned int borrow_shift(unsigned int a, unsigned int b, unsigned long long n) {
    return (a - b) >> (n & 15);
}
int main(void) {
    return borrow_shift(466218516U, 1243335322U, 3) != 0x1a35c46fU;
}
EOF

# An int division saves and restores the registers it borrows at full width, so
# a long long living in one of them keeps its high word.
try_ 0 << EOF
unsigned long long mix(unsigned long long a, unsigned long long b,
                       unsigned long long c, unsigned long long d,
                       unsigned long long e, unsigned long long f, int n) {
    unsigned long long w0 = a % 0x7fffffffffffffffULL;
    unsigned long long w1 = b % 0x7fffffffffffffffULL;
    unsigned long long w2 = c % 0x7fffffffffffffffULL;
    unsigned long long w3 = d % 0x7fffffffffffffffULL;
    unsigned long long w4 = e % 0x7fffffffffffffffULL;
    unsigned long long w5 = f % 0x7fffffffffffffffULL;
    unsigned long long w6 = a % 0x7ffffffffffffffeULL;
    int q = 10 % (n | 1);
    return w0 + w1 + w2 + w3 + w4 + w5 + w6 + q;
}
int main(void) {
    unsigned long long k = 0x100000000ULL;
    return mix(k, k, k, k, k, k, 112) != 0x700000000ULL + 10;
}
EOF

# A wide constant narrowed to an int argument keeps the sign of its low word,
# even though a 64-bit target loads the argument at full width.
try_ 0 << EOF
int top_two(int x) { return x >> 30; }
unsigned int top_two_unsigned(unsigned int x) { return x >> 30; }
int main(void) {
    return top_two(0xe231fffab478ULL) != -1 ||
           top_two_unsigned(0xe231fffab478ULL) != 3;
}
EOF

# A right shift reads the whole register of the value it shifts, so a negative
# int quotient or remainder must reach it sign-extended.
try_ 0 << EOF
int half_remainder(int x, int d) { return (x % d) >> 1; }
int shifted_quotient(int x, int d) { return (x / d) >> 1; }
int main(void) {
    return half_remainder(-24122, 9) != -1 ||
           shifted_quotient(-24122, 9) != -1340;
}
EOF
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

# The int spelling may accompany short and long in any specifier order, in every
# context that reads a type (C99 6.7.2p2).
try_ 12 << EOF
short int file_short = 3;
static int short file_static = 1;
typedef unsigned short int file_ushort;
short int keep_short(int long value) { return value; }
struct mixed { short int s; int short unsigned u; long int l; };
int main(void) {
    short int y = 1;
    int short w = -2;
    signed short int s = -1;
    short int signed t = -1;
    unsigned short int u = 65535;
    int long unsigned lu = 5;
    long long int ll = 1;
    long int long il = 2;
    int long long li = 3;
    short volatile unsigned int vu = 65535;
    const int short cs = 4;
    typedef short int block_short;
    block_short b = 7;
    struct mixed m;
    short int pair, *pp = &pair;
    *pp = 1;
    m.u = 65535;
    for (short int i = 0; i < 2; i++)
        y += i;
    return (sizeof(y) == 2) + (w == -2 && s == -1 && t == -1) +
           (u == 65535 && vu == 65535 && m.u == 65535) +
           (sizeof(file_ushort) == 2) + (sizeof(short int) == 2) +
           (sizeof(int long long) == 8) + ((short int) 65539 == 3) +
           (keep_short(file_short + file_static) == 4) +
           (ll + il + li == 6 && lu == 5) + (cs == 4 && b == 7) +
           (sizeof(b) == 2 && sizeof(m.s) == 2) + (y == 2 && pair == 1);
}
EOF
try_compile_error << EOF
int main(void) { short int int value = 1; return value; }
EOF
try_compile_error << EOF
int main(void) { long char int value = 1; return value; }
EOF
try_compile_error << EOF
int main(void) { short long int value = 1; return value; }
EOF

# A cast type name reads its specifiers with the declaration reader, so a
# qualifier may stand anywhere among the scalar words. A qualifier after the
# base word used to spin the cast reader forever. The name is looked up in the
# scope of the cast: a block typedef names a type, and a variable hides one.
try_ 64 << EOF
typedef int T;
struct S { int a; };
enum E { E0, E1 };
typedef int *P;
int main(void) {
    typedef short B;
    int x = 5, *p = &x;
    struct S s = { 3 };
    int r = ((char volatile) 1 - 1 == 0) +
            ((int volatile signed const) -1 == -1) * 2 +
            ((short const int unsigned) -1 == 65535) * 4 +
            ((B) 65539 == 3) * 8 +
            ((struct S const *) &s)->a;
    {
        int T = 3;
        r += ((T) - 1 == 2) * 16;
    }
    r += (*(P restrict) p == 5) * 32 + ((enum E volatile) 1 == E1) - 3;
    return r + (sizeof(enum E const) == sizeof(int)) - 1;
}
EOF
try_compile_error << EOF
int main(void) { return (int char volatile) 1; }
EOF
try_compile_error << EOF
int main(void) { return (signed volatile unsigned) 1; }
EOF
try_compile_error_message "restrict requires a pointer type" << EOF
int main(void) { return (int restrict){7}; }
EOF

# A storage class may follow any scalar word, not only a trailing int.
try_ 7 << EOF
int const volatile short unsigned typedef U;
U u = 65535;
int main(void) {
    const volatile const int short auto s = 259;
    int char_ok = 0;
    char unsigned static c = 255;
    char_ok = c == 255;
    return (u == 65535) + (s == 259) * 2 + char_ok * 4;
}
EOF
try_ 1 << EOF
int main(void) { return (unsigned long long) 1 == 1ULL; }
EOF
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

# A 32-bit target deliberately types -2147483648 as int, so that INT_MIN spelled
# that way stays usable; with an L suffix it is still a long long.
try_ "$((PTR_SZ >= 8 ? 2 : 1))" << EOF
int main(void) {
    return (sizeof(-2147483648) == 8) +
           (sizeof(-2147483648L) == 8);
}
EOF
try_flags 6 "--no-libc" << EOF
#include <limits.h>
#include <stdint.h>
int main(void) {
    return (sizeof(INT_MIN) == 4) + (sizeof(LONG_MIN) == 4) +
           (sizeof(INT32_MIN) == 4) + (sizeof(INT64_MIN) == 8) +
           (sizeof(LLONG_MIN) == 8) + (sizeof(INTMAX_MIN) == 8);
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
try_ 1 << EOF
int main(void) { return 4294967296U >> 32; }
EOF

# A 32-bit target admits long long literals, objects, returns and callbacks as
# well as sizeof and pointers.
try_ 1 << EOF
long long wide_value;
long long wide_return(void) { return 1LL; }
long long (*wide_callback)(void) = wide_return;
int main(void) { return (int)wide_callback() + (int)wide_value; }
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

# An integer cast inside a scalar file-scope initializer converts its constant
# operand, including a grouped wide operand, and the result keeps its high word.
try_ 0 << EOF
typedef long long cast_wide_t;
typedef unsigned char cast_byte_t;
enum { cast_negative = -1 };
long long cast_sum = (long long)0x100000000LL + 1;
unsigned long long cast_shift = (unsigned long long)1LL << 40;
long long cast_grouped = (long long)(0x100000000LL + 1);
cast_wide_t cast_typedef_shift = (cast_wide_t)1 << 40;
long long cast_sign_extended = (long long)(int)0xffffffffU;
_Bool cast_bool = (_Bool)0x100000000LL;
cast_byte_t cast_byte = (cast_byte_t)cast_negative;
int cast_leading = (int)3 + (char)300;
int main(void)
{
    return cast_sum != 0x100000001LL || cast_shift != 0x10000000000ULL ||
           cast_grouped != 0x100000001LL ||
           cast_typedef_shift != 0x10000000000LL ||
           cast_sign_extended != -1LL || cast_bool != 1 ||
           cast_byte != 255 || cast_leading != 47;
}
EOF
try_compile_error << EOF
int cast_object;
long long cast_non_constant = (long long)cast_object + 1;
int main(void) { return 0; }
EOF

# A cast, !, ~ and unary + bind tighter than a following binary operator. The
# operand of each is only the pointer, so `(char *) p + 1` advances by one byte
# rather than being read as the int pointer arithmetic p + 1, and `!p + 1` is 1.
try_ 0 << EOF
int id(int value) { return value; }
int main(void) {
    int arr[4] = {1, 2, 3, 4}, *p = arr, *q = arr, x = 3;
    char c = 100, *cp = (char *) p + 1;
    if (cp - (char *) p != 1 || (char *) p + 1 - (char *) p != 1) return 1;
    if (id((char *) arr + 3 - (char *) arr) != 3) return 2;
    if ((char *) (int *) p + 2 != (char *) arr + 2) return 3;
    if ((int) (char *) p + 1 - (int) (char *) p != 1) return 4;
    if (!p + 1 != 1 || ~x + 5 != 1 || +x + 1 != 4) return 5;
    if ((long) x * 2 != 6 || ((unsigned char) c << 1) != 200) return 6;
    if ((int *) (p + 2) - q != 2 || (int) arr[1] + 2 != 4) return 7;
    return *(char *) p + 1 != 2;
}
EOF
try_ 2 << EOF
unsigned long long global_sum = 0x100000000ULL + 7ULL;
int main(void) {
    return ((global_sum >> 32) == 1ULL) +
           ((global_sum - 7ULL) == 0x100000000ULL);
}
EOF
try_ 8 << EOF
unsigned long long ternary_wide_true = 1 ? 0x100000000ULL : 1U;
unsigned long long ternary_wide_false = 0 ? 0x100000000ULL : 1U;
unsigned long long ternary_signed_rank =
    -1LL < 1U ? 0x100000000ULL : 1U;
unsigned long long ternary_signed_value =
    0xffffffffU > -1LL ? 0x100000000ULL : 1U;
unsigned long long ternary_unsigned_rank =
    0ULL < -1LL ? 0x100000000ULL : 1U;
unsigned long long ternary_signed_word =
    -1 < 0 ? 0x100000000ULL : 1U;
int main(void) {
    return ((ternary_wide_true >> 32) == 1ULL) +
           ((unsigned int)ternary_wide_true == 0U) +
           ((ternary_wide_false >> 32) == 0ULL) +
           ((unsigned int)ternary_wide_false == 1U) +
           ((ternary_signed_rank >> 32) == 1ULL) +
           ((ternary_signed_value >> 32) == 1ULL) +
           ((ternary_unsigned_rank >> 32) == 1ULL) +
           ((ternary_signed_word >> 32) == 1ULL);
}
EOF
try_ 8 << EOF
unsigned long long logical_and_true =
    0x100000000ULL && 1 ? 0x100000000ULL : 1U;
unsigned long long logical_and_false =
    0 && 0x100000000ULL ? 0x100000000ULL : 1U;
unsigned long long logical_or_true =
    0 || 0x100000000ULL ? 0x100000000ULL : 1U;
unsigned long long logical_or_false =
    0 || 0 ? 0x100000000ULL : 1U;
unsigned long long logical_precedence =
    1 || 0 && 0 ? 0x100000000ULL : 1U;
unsigned long long logical_grouped_precedence =
    (1 || 0) && 0 ? 0x100000000ULL : 1U;
unsigned long long logical_unary_not =
    !0 && 0x100000000ULL ? 0x100000000ULL : 1U;
unsigned long long logical_nested =
    0 ? 1U : 1 && 0x100000000ULL ? 0x100000000ULL : 1U;
int main(void) {
    return (logical_and_true == 0x100000000ULL) +
           (logical_and_false == 1ULL) +
           (logical_or_true == 0x100000000ULL) +
           (logical_or_false == 1ULL) +
           (logical_precedence == 0x100000000ULL) +
           (logical_grouped_precedence == 1ULL) +
           (logical_unary_not == 0x100000000ULL) +
           (logical_nested == 0x100000000ULL);
}
EOF
try_ 7 << EOF
unsigned long long logical_protected_and =
    0 && (1 / 0) ? 0x100000000ULL : 1U;
unsigned long long logical_protected_or =
    1 || (1 / 0) ? 0x100000000ULL : 1U;
unsigned long long logical_direct_protected_and =
    0 && 1 / 0 ? 0x100000000ULL : 1U;
unsigned long long logical_direct_protected_or =
    1 || 1 / 0 ? 0x100000000ULL : 1U;
unsigned long long logical_protected_chain =
    0 && 1 / 0 + 1 ? 0x100000000ULL : 1U;
unsigned long long logical_nested_protected_and =
    0 && (1 || 1 / 0) ? 0x100000000ULL : 1U;
unsigned long long logical_nested_protected_or =
    1 || (0 && 1 / 0) ? 0x100000000ULL : 1U;
int main(void) {
    return (logical_protected_and == 1ULL) +
           (logical_protected_or == 0x100000000ULL) +
           (logical_direct_protected_and == 1ULL) +
           (logical_direct_protected_or == 0x100000000ULL) +
           (logical_protected_chain == 1ULL) +
           (logical_nested_protected_and == 1ULL) +
           (logical_nested_protected_or == 0x100000000ULL);
}
EOF
try_ 4 << EOF
unsigned long long ternary_protected_true =
    0 ? 1 / 0 : 1U;
unsigned long long ternary_protected_false =
    1 ? 0x100000000ULL : 1 / 0;
unsigned long long ternary_protected_nested =
    0 ? 1 / 0 : 1 ? 0x100000000ULL : 1 / 0;
unsigned long long ternary_protected_logical_condition =
    (0 && 1 / 0) ? 1 / 0 : 0x100000000ULL;
int main(void) {
    return (ternary_protected_true == 1ULL) +
           (ternary_protected_false == 0x100000000ULL) +
           (ternary_protected_nested == 0x100000000ULL) +
           (ternary_protected_logical_condition == 0x100000000ULL);
}
EOF
try_compile_error << EOF
unsigned long long invalid_wide_active_ternary_true =
    1 ? 1 / 0 : 0x100000000ULL;
EOF
try_compile_error << EOF
unsigned long long invalid_wide_active_ternary_false =
    0 ? 0x100000000ULL : 1 / 0;
EOF
try_compile_error << EOF
unsigned long long invalid_wide_ternary_condition =
    1 / 0 ? 0x100000000ULL : 1U;
EOF

# The signed minimum divided by -1 overflows its common type, as INT_MIN / -1
# does in the word-sized evaluator; the unsigned and discarded forms are valid.
try_compile_error << EOF
long long invalid_wide_min_quotient = (-9223372036854775807LL - 1) / -1;
EOF
try_compile_error << EOF
long long invalid_wide_min_remainder = (-9223372036854775807LL - 1) % -1;
EOF
try_compile_error << EOF
long long invalid_int_min_quotient_wide = (-2147483647 - 1) / -1 + 0LL;
EOF
try_ 3 << EOF
unsigned long long wide_unsigned_min_quotient = 0x8000000000000000ULL / -1;
long long wide_protected_min_quotient =
    0 ? (-9223372036854775807LL - 1) / -1 : 1LL;
long long wide_min_quotient_by_uint = (-9223372036854775807LL - 1) / -1U;
int main(void) {
    return (wide_unsigned_min_quotient == 0ULL) +
           (wide_protected_min_quotient == 1LL) +
           (wide_min_quotient_by_uint == -2147483648LL);
}
EOF
try_compile_error << EOF
int invalid_wide_discarded_ternary_object;
unsigned long long invalid_wide_discarded_ternary =
    0 ? invalid_wide_discarded_ternary_object : 1U;
EOF

# The word-sized evaluator skips a discarded arm up to its matching ':', or to
# the end of the declarator, past grouped and nested conditionals inside it.
try_ 8 << EOF
int int_ternary_grouped_true_arm = 0 ? (1 ? 2 : 3) : 4;
int int_ternary_nested_true_arm = 0 ? 1 ? 2 : 3 : 4;
int int_ternary_nested_false_arm = 1 ? 5 : 0 ? 6 : 7;
int int_ternary_nested_selected = 1 ? 1 ? 19 : 20 : 21;
int int_ternary_list = 1 ? 22 : 23, int_ternary_list_next = 24;
int int_ternary_deep = 0 ? 1 ? 2 : 3 ? 4 : 5 : 1 ? 25 : 26;
int main(void) {
    static int grouped_static = 0 ? (1 ? 2 : 3) : 18;
    static int nested_static = 1 ? 1 ? 19 : 20 : 21;
    return (int_ternary_grouped_true_arm == 4) +
           (int_ternary_nested_true_arm == 4) +
           (int_ternary_nested_false_arm == 5) +
           (int_ternary_nested_selected == 19) +
           (int_ternary_list == 22 && int_ternary_list_next == 24) +
           (int_ternary_deep == 25) + (grouped_static == 18) +
           (nested_static == 19);
}
EOF

# A conditional or logical element of a static brace initializer is folded as a
# constant. Lowered as a runtime branch, it replaced the global setup entry and
# every earlier global store read back as zero.
try_ 0 << EOF
enum { brace_constant = 3 };
int brace_before = 7;
int brace_array[] = {1 ? 2 : 3, 4};
int brace_logical[3] = {0 ? 5 : 6, 1 && 2, 0 || brace_constant};
int brace_operands[] = {sizeof(int) == 4 ? 8 : 9, -1 ? 10 : 11,
                        'a' ? 12 : 13, brace_constant ? 14 : 15};
int brace_nested[2][2] = {{1 ? 16 : 17, 18}, {0 ? 19 : 20, 21 && 0}};
struct brace_pair { int x, y; } brace_record = {1 ? 22 : 23, 0 && 1};
struct brace_pair brace_records[] = {{1 ? 24 : 25, 26}, {27, 0 ? 28 : 29}};
long long brace_wide[] = {1 ? 0x100000000LL : 0, 0 || -1};
int brace_after = 30;
int main(void) {
    static int brace_static[] = {1 ? 31 : 32, 1 && 1};
    return (brace_before != 7) + (brace_array[0] != 2) +
           (brace_array[1] != 4) + (sizeof(brace_array) != 2 * sizeof(int)) +
           (brace_logical[0] != 6) + (brace_logical[1] != 1) +
           (brace_logical[2] != 1) + (brace_operands[0] != 8) +
           (brace_operands[1] != 10) + (brace_operands[2] != 12) +
           (brace_operands[3] != 14) + (brace_nested[0][0] != 16) +
           (brace_nested[0][1] != 18) + (brace_nested[1][0] != 20) +
           (brace_nested[1][1] != 0) + (brace_record.x != 22) +
           (brace_record.y != 0) + (brace_records[0].x != 24) +
           (brace_records[1].y != 29) + (brace_wide[0] != 0x100000000LL) +
           (brace_wide[1] != 1) + (brace_after != 30) +
           (brace_static[0] != 31) + (brace_static[1] != 1);
}
EOF
try_compile_error << EOF
int brace_object;
int brace_logical_object[] = {1 && brace_object};
int main(void) { return 0; }
EOF
try_compile_error << EOF
int brace_comma[] = {(1, 2)};
int main(void) { return 0; }
EOF

# An address constant is a valid operand of a conditional or a logical operator
# in a static initializer: the conditional selects it, and the logical operators
# only test it, since it is never null.
try_ 0 << EOF
int selected_objects[3] = {4, 5, 6};
int selected_function(void) { return 9; }
char *selected_true_arm[] = {1 ? "ab" : "cd", "ef"};
char *selected_false_arm[] = {0 ? "ab" : "cd"};
char *selected_scalar = 0 ? "ab" : "cd";
struct selected_holder { int tag; char *text; int *element; };
struct selected_holder selected_record = {1, 0 ? "ab" : "cd",
                                          1 ? selected_objects : 0};
int *selected_addresses[] = {0 ? selected_objects : 0,
                             1 ? &selected_objects[2] : 0};
int (*selected_callbacks[])(void) = {1 ? selected_function : 0};
char *selected_nested[] = {0 ? "ab" : 1 ? "gh" : "ij"};
char *selected_null[] = {1 ? (char *) 0 : "ab"};
int selected_truth[] = {"ab" && 1, selected_objects || 0,
                        selected_function ? 3 : 4, !"ab"};
int selected_scalar_truth = "ab" && 2;
int main(void)
{
    static char *selected_static[] = {0 ? "ab" : "kl"};
    static char *selected_static_scalar = 1 ? "mn" : "op";

    return (selected_true_arm[0][0] != 'a') + (selected_true_arm[1][0] != 'e') +
           (selected_false_arm[0][0] != 'c') + (selected_scalar[0] != 'c') +
           (selected_record.tag != 1) + (selected_record.text[0] != 'c') +
           (selected_record.element != selected_objects) +
           (selected_addresses[0] != 0) +
           (selected_addresses[1] != &selected_objects[2]) +
           (selected_callbacks[0]() != 9) + (selected_nested[0][0] != 'g') +
           (selected_null[0] != 0) + (selected_truth[0] != 1) +
           (selected_truth[1] != 1) + (selected_truth[2] != 3) +
           (selected_truth[3] != 0) + (selected_scalar_truth != 1) +
           (selected_static[0][0] != 'k') + (selected_static_scalar[0] != 'm');
}
EOF
try_compile_error << EOF
int selected_object;
char *selected_non_constant[] = {selected_object ? "ab" : "cd"};
int main(void) { return 0; }
EOF
try_compile_error << EOF
int selected_object;
char *selected_non_constant_arm[] = {1 ? "ab" : selected_object};
int main(void) { return 0; }
EOF
try_compile_error << EOF
char *selected_comma[] = {1 ? ("ab", "cd") : "ef"};
int main(void) { return 0; }
EOF

# An address constant offset by an integer is still one inside a conditional or
# a logical operator, with the integer on either side of the sum; the step is
# the element the address points to, a whole row for a decayed matrix. A tested
# address is true, so the right operand of || is never evaluated.
try_ 0 << EOF
enum { offset_back_one = -1 };
char offset_text[] = "abc";
int offset_values[4] = {1, 2, 3, 4};
int offset_rows[3][2] = {{1, 2}, {3, 4}, {5, 6}};
char *offset_first[] = {"x" ? 1 + offset_text : 0, "x" ? 2 + "xyz" : 0};
char *offset_grouped = offset_text ? (offset_text) + 2 : 0;
int *offset_both = offset_values ? 3 + offset_values - 2 : 0;
int *offset_back = "x" ? -1 + (offset_values + 3) : 0;
int *offset_enum = "x" ? offset_back_one + (offset_values + 2) : 0;
int *offset_wide = "x" ? 1LL + offset_values : 0;
int (*offset_row)[2] = "x" ? 2 + offset_rows : 0;
int offset_truth = "x" && 1 + offset_text;
int offset_skipped = offset_values || 1 / 0;
int main(void)
{
    return (*offset_first[0] != 'b') + (*offset_first[1] != 'z') +
           (*offset_grouped != 'c') + (*offset_both != 2) +
           (*offset_back != 3) + (*offset_enum != 2) + (*offset_wide != 2) +
           ((*offset_row)[1] != 6) +
           (offset_truth != 1) + (offset_skipped != 1);
}
EOF
try_compile_error << EOF
int offset_values[4];
int *offset_negated = "x" ? 1 - offset_values : 0;
int main(void) { return 0; }
EOF

# An offset the word-sized address relocation cannot hold is refused rather than
# losing its high word or overflowing once scaled.
try_compile_error_message "Global address offset exceeds supported integer range" << EOF
int offset_values[4];
int *offset_high = "x" ? 0x100000000LL + offset_values : 0;
int main(void) { return 0; }
EOF
try_compile_error_message "Global address offset exceeds supported integer range" << EOF
int offset_values[4];
int *offset_scaled = "x" ? 0x40000000 + offset_values : 0;
int main(void) { return 0; }
EOF
try_ 18 << EOF
unsigned long long global_ternary_true =
    (1 ? 0x100000000ULL : 0ULL) + 1ULL;
unsigned long long global_ternary_false =
    0 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_precedence =
    1 - 1 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_high_condition =
    0x100000000ULL ? 7U : 1U;
unsigned long long global_ternary_nested =
    0 ? 1U : 1 ? 0x100000000ULL : 2U;
unsigned long long global_ternary_sizeof_true =
    sizeof(int) == 4 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_false =
    sizeof(int) != 4 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_string =
    sizeof "abc" == 4 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_grouped_string =
    sizeof("abc") != 4 ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_adjacent_string =
    sizeof("a" "bc") == 4 ? 7U : 1U;
unsigned long long global_ternary_sizeof_wstring =
    sizeof L"ab" == 3 * sizeof(wchar_t) ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_grouped_wstring =
    sizeof(L"ab") != 3 * sizeof(wchar_t) ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_adjacent_wstring =
    sizeof(L"a" L"b") == 3 * sizeof(wchar_t) ? 7U : 1U;
unsigned long long global_ternary_sizeof_scalar =
    sizeof 1 == sizeof(int) ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_grouped_scalar =
    sizeof(1 + 2) != sizeof(int) ? 0x100000000ULL : 1U;
int global_ternary_object;
int global_ternary_array[3];
unsigned long long global_ternary_sizeof_object =
    sizeof global_ternary_object == sizeof(int) ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_grouped_object =
    sizeof(global_ternary_object) != sizeof(int) ? 0x100000000ULL : 1U;
unsigned long long global_ternary_sizeof_array =
    sizeof global_ternary_array == 3 * sizeof(int) ? 7U : 1U;
int main(void) {
    return (global_ternary_true == 0x100000001ULL) +
           (global_ternary_false == 1ULL) +
           (global_ternary_precedence == 1ULL) +
           (global_ternary_high_condition == 7ULL) +
           (global_ternary_nested == 0x100000000ULL) +
           (global_ternary_sizeof_true == 0x100000000ULL) +
           (global_ternary_sizeof_false == 1ULL) +
           (global_ternary_sizeof_string == 0x100000000ULL) +
           (global_ternary_sizeof_grouped_string == 1ULL) +
           (global_ternary_sizeof_adjacent_string == 7ULL) +
           (global_ternary_sizeof_wstring == 0x100000000ULL) +
           (global_ternary_sizeof_grouped_wstring == 1ULL) +
           (global_ternary_sizeof_adjacent_wstring == 7ULL) +
           (global_ternary_sizeof_scalar == 0x100000000ULL) +
           (global_ternary_sizeof_grouped_scalar == 1ULL) +
           (global_ternary_sizeof_object == 0x100000000ULL) +
           (global_ternary_sizeof_grouped_object == 1ULL) +
           (global_ternary_sizeof_array == 7ULL);
}
EOF
try_ 6 << EOF
int postfix_object;
typedef char cast_byte;
struct wide_inc_rec;
typedef struct wide_inc_rec wide_inc_t;
struct wide_inc_rec *wide_inc_ptr;
wide_inc_t *wide_inc_typedef_ptr;
unsigned long long postfix_value =
    sizeof postfix_object++ == sizeof(int) ? 0x100000000ULL : 1U;
unsigned long long cast_true =
    sizeof((cast_byte)postfix_object) == 1 ? 0x100000000ULL : 1U;
unsigned long long cast_false =
    sizeof((cast_byte)postfix_object) != 1 ? 0x100000000ULL : 1U;
unsigned long long incomplete_pointer =
    sizeof wide_inc_ptr == sizeof(void *) ? 0x100000000ULL : 1U;
unsigned long long incomplete_typedef_pointer =
    sizeof wide_inc_typedef_ptr != sizeof(void *) ? 0x100000000ULL : 1U;
int main(void) {
    return (postfix_value == 0x100000000ULL) + (postfix_object == 0) +
           (cast_true == 0x100000000ULL) + (cast_false == 1ULL) +
           (incomplete_pointer == 0x100000000ULL) +
           (incomplete_typedef_pointer == 1ULL);
}
EOF
try_compile_error << EOF
int invalid_wide_sizeof_function(void) { return 0; }
unsigned long long invalid_wide_sizeof_function_value =
    sizeof invalid_wide_sizeof_function ? 0x100000000ULL : 1U;
int main(void) { return 0; }
EOF
try_compile_error << EOF
int invalid_wide_logical_operand;
unsigned long long invalid_wide_logical_value =
    0 && invalid_wide_logical_operand ? 0x100000000ULL : 1U;
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned long long invalid_wide_active_and =
    1 && (1 / 0) ? 0x100000000ULL : 1U;
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned long long invalid_wide_active_or =
    0 || (1 / 0) ? 0x100000000ULL : 1U;
int main(void) { return 0; }
EOF
try_compile_error << EOF
int invalid_wide_sizeof_grouped_function(void) { return 0; }
unsigned long long invalid_wide_sizeof_grouped_function_value =
    sizeof(invalid_wide_sizeof_grouped_function) ? 0x100000000ULL : 1U;
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned long long invalid_wide_sizeof_void = sizeof(void) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct invalid_wide_sizeof_record;
unsigned long long invalid_wide_sizeof_record_value =
    sizeof(struct invalid_wide_sizeof_record) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct invalid_wide_sizeof_record_array;
unsigned long long invalid_wide_sizeof_record_array_value =
    sizeof(struct invalid_wide_sizeof_record_array[2]) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned long long invalid_wide_sizeof_void_expression =
    sizeof((void)1) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct bad_expr_rec;
struct bad_expr_rec *bad_expr_ptr;
unsigned long long invalid_wide_sizeof_expression_record_value =
    sizeof((*bad_expr_ptr)) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct bad_expr_trec;
typedef struct bad_expr_trec bad_expr_t;
bad_expr_t *bad_expr_tptr;
unsigned long long invalid_wide_sizeof_expression_typedef_value =
    sizeof((*bad_expr_tptr)) ? 1ULL : 0ULL;
int main(void) { return 0; }
EOF
try_compile_error << EOF
int invalid_wide_sizeof_trailing_object;
unsigned long long invalid_wide_sizeof_trailing_value =
    sizeof (char)invalid_wide_sizeof_trailing_object ? 0x100000000ULL : 1U;
int main(void) { return 0; }
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

# A narrow operand of a unary operator or a shift is promoted to int first, so
# the folded result extends as an int rather than as the cast type.
try_ 6 << EOF
long long global_promoted_complement = ~(unsigned char) 0;
long long global_promoted_negation = -(unsigned short) 5;
long long global_promoted_shift = ~((unsigned char) 1 << 1);
long long global_promoted_shift_neg = -((unsigned short) 1 << 1);
int main(void) {
    static long long promoted_static = ~(unsigned char) 0;
    static long long promoted_static_shift = ~(unsigned short) 0 >> 1;
    return (global_promoted_complement == -1LL) +
           (global_promoted_negation == -5LL) +
           (global_promoted_shift == -3LL) +
           (global_promoted_shift_neg == -2LL) +
           (promoted_static == -1LL) + (promoted_static_shift == -1LL);
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

# A cast to long long extends by the source signedness, even when the source is
# the result of 32-bit arithmetic whose register holds no extension.
try_output 0 "ffffffff.fffffffb;0.ee6b2800;ffffffff.fffffffd;0.c8;0.ea60;ffffffff.fffffffb;ffffffff.f4143e00;ffffffff.ffffec78;0.7fb;ffffffff.fffffffa;" << EOF
void show(long long value)
{
    printf("%x.%x;", (unsigned) (value >> 32), (unsigned) value);
}
int main(void)
{
    int negative = -5;
    unsigned int large = 4000000000U;
    short half = -3;
    unsigned char byte = 200;
    unsigned short word = 60000;
    int product = -1000000;
    show((long long) negative);
    show((long long) large);
    show((long long) half);
    show((long long) byte);
    show((long long) word);
    show((unsigned long long) negative);
    product *= byte;
    show((long long) product);
    product = -1000000;
    product /= byte;
    show((long long) product);
    show((long long) (negative & 0x7ff));
    show((long long) (negative ^ 1));
    return 0;
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

# A wide global reduction must rebuild the high word of every int-sized operand
# from its type: an enumeration constant never stores one, and an int-sized
# intermediate such as 2U - 3U must not keep its borrow.
try_ 8 << EOF
enum { NEGATIVE_ONE = -1 };
long long wide_enum_sum = 0x100000000LL + NEGATIVE_ONE;
long long wide_enum_product = 1LL * NEGATIVE_ONE;
long long wide_unsigned_borrow = 0LL + (2U - 3U);
long long wide_unsigned_carry = 0LL + (0x7fffffffU + 1U) * 2U;
long long wide_mixed_quotient = 0LL + (-2) / 2U;
long long wide_narrow_divisor = 0x100000000LL / (2U - 3U);
long long wide_narrow_truth = 0LL + !(0x80000000U + 0x80000000U);
long long wide_enum_shift = (0LL + NEGATIVE_ONE) >> 1;
int main(void) {
    return (wide_enum_sum == 0xffffffffLL) + (wide_enum_product == -1LL) +
           (wide_unsigned_borrow == 0xffffffffLL) +
           (wide_unsigned_carry == 0LL) +
           (wide_mixed_quotient == 0x7fffffffLL) +
           (wide_narrow_divisor == 1LL) + (wide_narrow_truth == 1LL) +
           (wide_enum_shift == -1LL);
}
EOF

# A signed long long divided by an unsigned int keeps the signed long long
# common type, so the quotient and remainder follow signed rules.
try_ 4 << EOF
long long wide_signed_quotient = -2LL / 2U;
long long wide_signed_truncation = -7LL / 2U;
long long wide_signed_remainder = -7LL % 2U;
unsigned long long wide_unsigned_quotient = -2LL / 2ULL;
int main(void) {
    return (wide_signed_quotient == -1LL) +
           (wide_signed_truncation == -3LL) +
           (wide_signed_remainder == -1LL) +
           (wide_unsigned_quotient == 0x7fffffffffffffffULL);
}
EOF

# Character constants are integer constant expression operands, so a wide global
# initializer accepts them next to a long long literal.
try_ 4 << EOF
long long wide_char_sum = 0x100000000LL + 'a';
long long wide_char_first = 'a' + 1LL;
long long wide_wchar_product = L'b' * 0x100000000LL;
long long wide_char_condition = '\0' ? 1LL : 0x100000000LL;
int main(void) {
    return (wide_char_sum == 0x100000061LL) + (wide_char_first == 98LL) +
           (wide_wchar_product == 0x6200000000LL) +
           (wide_char_condition == 0x100000000LL);
}
EOF

# A constant copied into its caller by inlining keeps its high word: ~0ULL
# returned from a helper is not 0xffffffff.
try_ 0 << EOF
unsigned long long all_ones(void) { return ~0ULL; }
long long high_only(void) { return 0x100000000LL; }
int main(void) {
    return all_ones() != 0xffffffffffffffffULL || high_only() != 0x100000000LL;
}
EOF

# A post-allocation all-ones rewrite needs the constant's high word as well: an
# eight-byte OR or AND with 0xffffffffULL is no identity.
try_ 0 << EOF
int main(void) {
    unsigned long long low = 0xffffffffULL;
    unsigned long long high = 0x100000000ULL;
    low |= ~0;
    high &= 0xffffffffULL;
    return low != 0xffffffffffffffffULL || high != 0;
}
EOF

# A long long read or written through a pointer moves both words: array elements
# and a member of a record reached through a pointer.
try_ 0 << EOF
struct wide_member { char c; long long v; int i; };
int main(void) {
    long long a[4];
    long long sum = 0;
    struct wide_member s;
    struct wide_member *p = &s;
    for (int i = 0; i < 4; i++)
        a[i] = (long long)i << 33;
    for (int i = 0; i < 4; i++)
        sum += a[i];
    p->v = -3LL << 40;
    p->i = 7;
    return sum != (6LL << 33) || s.v != (-3LL << 40) || s.i != 7;
}
EOF
# Taking the address of a long long held in registers stores both words first.
try_ 0 << EOF
void bump(long long *p) { *p += 1; }
int main(void) {
    long long v = 0x1122334455667788LL;
    long long *p = &v;
    *p += 1;
    bump(&v);
    return v != 0x112233445566778aLL;
}
EOF

# A long long carried round a loop in a register pair keeps its high word when
# the phi copy stores it back to the variable's slot.
try_ 0 << EOF
long long countdown(long long a) {
    int r = 0;
    while (r < 3) {
        a = a - 0x80000000LL;
        r++;
    }
    return a;
}
int main(void) { return countdown(0x100000000LL) != -0x80000000LL; }
EOF

# A long long whose low word is zero is still true in a condition, a logical
# operator and a loop test.
try_ 61 << EOF
int is_set(long long a) { if (a) return 1; return 0; }
int is_clear(long long a) { return !a; }
int both(long long a) { return a && 1; }
int either(long long a, long long b) { return b || a; }
int drain(long long a) {
    int r = 0;
    while (a && r < 9) {
        a = a - 0x80000000LL;
        r++;
    }
    return r;
}
int choose(long long a) { return a ? 1 : 0; }
int main(void) {
    long long a = 0x100000000LL;
    return is_set(a) + 2 * is_clear(a) + 4 * both(a) + 8 * either(a, 0) +
           16 * (drain(a) == 2) + 32 * choose(a);
}
EOF

# Ordering two long longs compares their low words unsigned whatever the type,
# and the high words by the type's signedness.
try_ 0 << EOF
int order(long long a, long long b) {
    return (a < b) + 2 * (a <= b) + 4 * (a > b) + 8 * (a >= b) +
           16 * (a == b) + 32 * (a != b);
}
int uorder(unsigned long long a, unsigned long long b) {
    return (a < b) + 2 * (a <= b) + 4 * (a > b) + 8 * (a >= b) +
           16 * (a == b) + 32 * (a != b);
}
int main(void) {
    return order(0, 0x80000000LL) != 35 || order(0x80000000LL, 0) != 44 ||
           order(-1, 0) != 35 || order(0x100000000LL, 0xffffffffLL) != 44 ||
           order(-0x80000000LL, 1) != 35 || order(7, 7) != 26 ||
           uorder(0, 0x80000000ULL) != 35 || uorder(~0ULL, 1) != 44 ||
           uorder(0x100000000ULL, 0xffffffffULL) != 44;
}
EOF

# A signed right shift of a long long takes the sign from the high word; the low
# word's own top bit is data and must not be replicated.
try_ 0 << EOF
long long sar(long long v, int n) { return v >> n; }
int main(void) {
    return sar(0x80000000LL, 1) != 0x40000000LL ||
           sar(0x1ffffffffLL, 4) != 0x1fffffffLL ||
           sar(-0x100000000LL, 33) != -1 ||
           sar(-0x80000000LL, 4) != -0x8000000LL;
}
EOF
# Shifting a long long by a run-time amount of zero leaves it unchanged.
try_ 0 << EOF
int amounts[2] = {0, 63};
long long shl(long long v, int n) { return v << n; }
unsigned long long shr(unsigned long long v, int n) { return v >> n; }
long long sar(long long v, int n) { return v >> n; }
int main(void) {
    int zero = amounts[0];
    return shl(0x80000001LL, zero) != 0x80000001LL ||
           shr(0xffffffffULL, zero) != 0xffffffffULL ||
           sar(-0x80000000LL, zero) != -0x80000000LL ||
           shl(1, amounts[1]) != (long long)0x8000000000000000ULL;
}
EOF

# Issue 312: a long long argument after four ints is found where the caller put
# it, with both of its words.
try_ 0 << EOF
int test_ll(int a, int b, int c, int d, long long e) {
    return e == 1000LL && a == 1 && b == 2 && c == 3 && d == 4;
}
int main(void) { return !test_ll(1, 2, 3, 4, 1000); }
EOF

# A negative signed char widened to a short keeps its sign, whether the short is
# a local or a parameter the call converts it to.
try_ 0 << EOF
int to_ushort(signed char c) { unsigned short s = c; return s; }
int high_word(short s) { unsigned long long x = 8; x |= s; return x >> 32; }
int main(void) {
    signed char c = -3;
    return to_ushort(-108) != 65428 || high_word(c) != -1;
}
EOF

# A repeated wide subexpression becomes a copy of the first result, and fusing
# that copy into the operation must not leave the first result unwritten.
try_ 0 << EOF
void sink(unsigned long long v) {}
unsigned long long r;
void same_twice(unsigned long long a0, long long a1, unsigned long long a2,
                unsigned long long a3) {
    unsigned long long l0 = 6942535506704294056ULL;
    long long l4 = a0;
    l0 /= l4;
    sink(l0);
    r = 1 ? ((a0 & l0) ^ (l0 & a0)) : 7;
}
int main(void) {
    same_twice(18, 0x7fffffff, 0xf3a9221e0ce272ccULL, 4);
    return r != 0;
}
EOF

# One long long passed twice keeps both copies: staging the second pair must not
# release the first argument's registers to the constants loaded after it.
try_ 0 << EOF
long long g = 0x123456789LL;
int f(long long a0, long long a1, unsigned int a2, long long a3) {
    return a0 != 0x123456789LL || a1 != 0x123456789LL || a2 != 0x89f20000U ||
           a3 != 0x25085689f936b9a0LL;
}
int main(void) { return f(g, g, 0x89f20000U, 0x25085689f936b9a0LL); }
EOF

# Post-allocation rewrites of a register pair keep its high word: the move after
# a pair operation and the load of a slot written just before it.
try_ 0 << EOF
long long total(long long a, long long b, long long c) {
    long long s = 0;
    for (int i = 0; i < 3; i++) {
        long long v = (i == 0 ? a : i == 1 ? b : c) * 1;
        s = s + v;
    }
    return s;
}
int main(void) { return total(0, 0x100000000LL, -1) != 0xffffffffLL; }
EOF

# Moving a register pair one register over writes the high word before the low
# register that holds it is overwritten.
try_ 0 << EOF
long long mix(long long a, int b, long long c, int d, int e, long long f) {
    return a * 1 + b * 10 + c * 100 + d * 1000 + e * 10000 + f * 100000;
}
int main(void) {
    return mix(1, 2, 3, 4, 5, 6) != 654321 ||
           mix(-1LL << 40, 2, 3, 4, 5, 6) != (-1LL << 40) + 654320;
}
EOF

# Spilling a register pair to free one register for a word stores both of its
# words.
try_ 0 << EOF
#include <stdio.h>
int main(void) {
    long long vals[3];
    int r = 0;
    vals[0] = 0LL;
    vals[1] = -1LL;
    vals[2] = 5LL;
    for (int j = 0; j < 3; j++) {
        long long a = vals[0], b = vals[j];
        int sum = (a < b);
        sum = sum * 5 + (a > b);
        r = r * 10 + sum;
        printf("%d", sum);
    }
    printf("\n");
    return r != 15;
}
EOF

# A long long global keeps both words when a function stores a constant in it
# and when its initializer has an int-sized type.
try_ 0 << EOF
unsigned long long wide_scalar;
unsigned long long wide_narrow_init = 1 ? 7U : 1U;
long long wide_negative_init = 1 ? -7 : 1;
void set(void) { wide_scalar = 0x100000001ULL; }
int main(void) {
    set();
    return wide_scalar != 0x100000001ULL || wide_narrow_init != 7 ||
           wide_negative_init != -7;
}
EOF

# A long long operation extends an int-sized operand the parser left narrow,
# including through a pointer store and a narrow quotient of a wide dividend.
try_ 0 << EOF
int main(void) {
    int i = -1;
    unsigned u = 1;
    long long l = -2;
    long long cell = 5;
    long long *p = &cell;
    unsigned long long q = 0x300000000ULL;
    int small;
    *p = i;
    small = (int)(q / 3);
    return !(l < u) || (l + u) != -1 || cell != -1 ||
           small != 0 || (u << 31) != 0x80000000U;
}
EOF

# Pair division in a loop holds two operand pairs and a result pair at once, and
# must still find registers beside the ones kept for the loop counters.
try_ 92 << EOF
int main(void) {
    long long v[5], d[4];
    unsigned r = 0;
    v[0] = 7LL; v[1] = -7LL; v[2] = 0x123456789LL; v[3] = -0x123456789LL;
    v[4] = 0x7fffffffffffffffLL;
    d[0] = 3LL; d[1] = -3LL; d[2] = 0x10000LL; d[3] = -0x100000001LL;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++) {
            long long q = v[i] / d[j], m = v[i] % d[j];
            unsigned long long uq = (unsigned long long)v[i] / (unsigned long long)d[j];
            unsigned long long um = (unsigned long long)v[i] % (unsigned long long)d[j];
            r = r * 33 + (unsigned)q + (unsigned)(q >> 32);
            r = r * 33 + (unsigned)m + (unsigned)(m >> 32);
            r = r * 33 + (unsigned)uq + (unsigned)(uq >> 32);
            r = r * 33 + (unsigned)um + (unsigned)(um >> 32);
        }
    return r % 251;
}
EOF

# Aggregate initializers keep the high word of a long long member or element: a
# file-scope record, a nested array member and a block-scope static array.
try_ 0 << EOF
struct wide_record { int a; long long b; int c; unsigned long long d[2]; };
struct wide_record wide_global = {1, 0x1122334455667788LL, 3,
                                  {0x100000000ULL, 9}};
long long wide_static(int i) {
    static long long table[3] = {0x100000001LL, -0x200000000LL, 5};
    return table[i];
}
int main(void) {
    return wide_global.a != 1 || wide_global.b != 0x1122334455667788LL ||
           wide_global.c != 3 || wide_global.d[0] != 0x100000000ULL ||
           wide_global.d[1] != 9 || wide_static(0) != 0x100000001LL ||
           wide_static(1) != -0x200000000LL || wide_static(2) != 5;
}
EOF

# A file-scope array element computed by a wide operator is lowered in global
# setup, which must name the high registers of the operands and the result.
try_ 0 << EOF
long long wide_ops[] = {1LL << 40, -2, (1LL << 40) | 3, ~(1LL << 40),
                        -(1LL << 40), (1LL << 40) / 3, 0x100000000LL - 1};
int main(void) {
    return wide_ops[0] != 0x10000000000LL || wide_ops[1] != -2 ||
           wide_ops[2] != 0x10000000003LL || wide_ops[3] != -0x10000000001LL ||
           wide_ops[4] != -0x10000000000LL || wide_ops[5] != 0x5555555555LL ||
           wide_ops[6] != 0xffffffffLL;
}
EOF

# ++ and -- on a long long carry into and borrow from the high word, whether the
# object is a variable, an array element or a member reached through a pointer.
try_ 0 << EOF
struct wide_counter { long long count; };
int main(void) {
    unsigned long long u = 0xffffffffULL;
    long long s = 0x100000000LL;
    long long cells[2] = {0xffffffffLL, 0x100000000LL};
    struct wide_counter counter = {0xffffffffLL};
    struct wide_counter *p = &counter;
    unsigned long long old = u++;
    long long before = --s;
    cells[0]++;
    --cells[1];
    p->count++;
    return u != 0x100000000ULL || old != 0xffffffffULL ||
           s != 0xffffffffLL || before != 0xffffffffLL ||
           cells[0] != 0x100000000LL || cells[1] != 0xffffffffLL ||
           counter.count != 0x100000000LL;
}
EOF

# A narrow global is loaded and stored at its own width. Its data slot is no
# wider than its type on LP64, so a full-width store to a clobbered b and e.
try_ 0 << EOF
unsigned int a = 1;
unsigned int b = 2;
signed char c = 3;
unsigned short d = 4;
int e = 7;
unsigned long long wide = 0x678d2f31;
void set(unsigned int v) { a = v; c = -1; d = 65535; }
int main(void) {
    set(0xffffffff);
    return b != 2 || e != 7 || c != -1 || d != 65535 || a != 0xffffffff ||
           (b >> (wide & 31)) != 0 || (a >> (wide & 31)) != 0x7fff;
}
EOF

# A global is not renamed, so "g = g" is dropped as redundant; a later read of g
# must still see the store before it rather than the dropped assignment.
try_ 0 << EOF
int g = 1;
int read_g(void) { return g; }
int main(void) {
    g = g;
    if (g != 1)
        return 1;
    g = 5;
    g = g;
    return g != 5 || read_g() != 5;
}
EOF

# Algebraic and strength-reduction folds read a constant's low word only.
# 0xffffffffULL is no all-ones mask for a long long, and 0x100000004ULL is not
# the power of two its low word is.
try_ 0 << EOF
unsigned long long mask(unsigned long long x) { return x & 0xffffffffULL; }
unsigned long long fill(unsigned long long x) { return x | 0xffffffffULL; }
long long times(long long x) { return x * 0xffffffffLL; }
unsigned long long scale(unsigned long long x) { return x * 0x100000004ULL; }
unsigned long long part(unsigned long long x) { return x / 0x100000004ULL; }
long long all_ones(long long x) { return x | -1; }
int main(void) {
    return mask(0x123456789ULL) != 0x23456789ULL ||
           fill(0x123456789ULL) != 0x1ffffffffULL ||
           times(2) != 0x1fffffffeLL || scale(3) != 0x30000000cULL ||
           part(0x300000010ULL) != 3 || all_ones(5) != -1LL;
}
EOF

# A wide product needs all four words of its operands, including when the result
# replaces one of them or squares it in a loop.
try_ 0 << EOF
long long cell = 0x100000003LL;
int main(void) {
    long long x = 0x100000003LL, y = 0x200000005LL, z = -0x123456789LL;
    for (int i = 0; i < 3; i++) {
        x = x * y;
        z *= z;
        cell = cell * y;
    }
    return x != 0x23f00000177LL || z != 0xaabea71870b3341LL ||
           cell != 0x23f00000177LL;
}
EOF

# A branch threaded on a constant condition tests the whole constant:
# 0x227044f500000000ULL has a zero low word but is true.
try_ 0 << EOF
int main(void) {
    long long flag = 0xafc6260dLL;
    unsigned long long other = 0xffffffffULL;
    unsigned long long picked =
        ((unsigned long long)flag ? 0x227044f500000000ULL : 0) ? 0 : other;
    return picked != 0;
}
EOF

# Stack arguments of long longs go past the outgoing area a frame reserves for
# one word per argument, and on RISC-V, which passes eight arguments in
# registers, that area is empty. The caller's locals must survive the call.
try_ 0 << EOF
long long pick(long long a, long long b, long long c, long long d, long long e,
               long long f, long long g, int h) {
    long long r = a + b + c + d + e + f;
    for (int i = 0; i < h; i++)
        r += g;
    return r;
}
int main(void) {
    int keep = 77;
    int *p = &keep;
    long long r = pick(1, 2, 3, 4, 5, 6, 0x100000000LL, 2);
    return r != 0x200000015LL || *p != 77;
}
EOF

# A variadic function saves its named parameters from the argument words,
# including those passed on the stack, before va_start walks past them.
try_ 0 << EOF
#include <stdarg.h>
int named(int a, int b, int c, int d, int e, int f, int g, ...) {
    va_list ap;
    int x;
    va_start(ap, g);
    x = va_arg(ap, int);
    va_end(ap);
    return a + b + c + d + e + f * g + x;
}
int main(void) { return named(1, 2, 3, 4, 5, 6, 7, 100) != 157; }
EOF

# va_arg reads a long long from the pair of argument words that holds it, which
# starts at an even word on a 32-bit target, and a variadic function saves
# enough words for arguments of two words each.
try_ 0 << EOF
#include <stdarg.h>
long long sum(int n, ...) {
    va_list ap;
    long long s = 0;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (i % 2)
            s = s * 3 + va_arg(ap, int);
        else
            s = s * 3 + va_arg(ap, long long);
    }
    va_end(ap);
    return s;
}
/* Eight named words fill the RISC-V argument registers, so saving the words
 * passed on the stack goes through a7 after h has been saved from it.
 */
int eighth(int a, int b, int c, int d, int e, int f, int g, int h, ...) {
    va_list ap;
    int cells[1];
    va_start(ap, h);
    cells[0] = h;
    va_end(ap);
    return cells[0];
}
int main(void) {
    return eighth(1, 2, 3, 4, 5, 6, 7, 8) != 8 ||
           sum(4, 0x100000000LL, 2, 0x300000000LL, 4) !=
               ((0x100000000LL * 3 + 2) * 3 + 0x300000000LL) * 3 + 4 ||
           sum(7, 1LL, 2, -3LL, 4, 0x500000000LL, 6, 7LL) !=
               (((((1LL * 3 + 2) * 3 - 3) * 3 + 4) * 3 + 0x500000000LL) * 3 + 6) * 3 + 7;
}
EOF

# RV32 passes a named long long in the next two argument words, splitting it
# between a7 and the stack, and aligns only a variadic one; AAPCS32 aligns every
# one. Either way the callee must find what the caller placed.
try_ 0 << EOF
#include <stdarg.h>
long long split(int a, int b, int c, int d, int e, int f, int g, long long h) {
    return (a + b + c + d + e + f + g) * 1000 + h * 10;
}
long long after(long long a, int b, int c, int d, int e, int f, long long g,
                int h) {
    return a * 3 + (b + c + d + e + f) * 1000 + g * 10 + h;
}
long long odd(int a, long long b, int c) { return a * 100 + b * 10 + c; }
long long unnamed(int a, int b, int c, int d, int e, int f, int g, ...) {
    va_list ap;
    long long x;
    va_start(ap, g);
    x = va_arg(ap, long long);
    va_end(ap);
    return (a + b + c + d + e + f + g) + x * 5;
}
typedef long long (*after_t)(long long, int, int, int, int, int, long long, int);
int main(void) {
    after_t callback = after;
    return split(1, 2, 3, 4, 5, 6, 7, 0x800000009LL) !=
               28000 + 0x800000009LL * 10 ||
           after(0x100000000LL, 1, 2, 3, 4, 5, -0x800000009LL, 11) !=
               0x300000000LL + 15000 - 0x800000009LL * 10 + 11 ||
           callback(0x100000000LL, 1, 2, 3, 4, 5, -0x800000009LL, 11) !=
               0x300000000LL + 15000 - 0x800000009LL * 10 + 11 ||
           odd(1, 0x100000000LL, 2) != 100 + 0xa00000000LL + 2 ||
           unnamed(1, 2, 3, 4, 5, 6, 7, -0x300000000LL) != 28 - 0xf00000000LL;
}
EOF
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
int main(void) { return '\x0041' == 'A'; }
EOF
try_compile_error_message "Hexadecimal escape sequence out of range" << EOF
int main(void) { return '\x123'; }
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

# A narrow unsigned value loaded from memory must not arrive sign-extended: on
# an LP64 target an index of 200 read from a member or an element went into the
# address as -56.
try_ 0 << EOF
struct unsigned_members { unsigned char byte; unsigned short half; };
char table[60000];
int main(void) {
    struct unsigned_members value = {200, 50000};
    struct unsigned_members *p = &value;
    unsigned char bytes[1] = {200};
    table[200] = 7;
    table[50000] = 9;
    return table[p->byte] != 7 || table[p->half] != 9 ||
           table[bytes[0]] != 7 || *(table + p->byte) != 7;
}
EOF
try_ 6 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    return p[1][0];
}
EOF

# An array member of a const record decays to a pointer to const elements,
# through arithmetic too, as any pointer to const does; a function pointer
# member of it is read-only.
try_ 0 << EOF
struct S { int arr[2]; }; struct S s = {{3, 4}}; int main(void) { const struct S *p = &s; const int *q = p->arr; struct S *m = &s; *(m->arr) = 5; *((*m).arr + 1) = 6; return *q + *(p->arr + 1) + (*p).arr[1] - 17; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } struct S { int (*cb)(int); }; struct S s = {inc}; int main(void) { const struct S *p = &s; return p->cb(1) + (*p).cb(1) - 4; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct S { int arr[2]; }; struct S s; int main(void) { const struct S *p = &s; *(p->arr) = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct S { int arr[2]; }; struct S s; int main(void) { const struct S *p = &s; *((*p).arr + 1) = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct S { int arr[2]; }; const struct S cs; int main(void) { *(cs.arr) = 1; return 0; }
EOF
try_compile_error_message "discarding const qualifier" << EOF
struct S { int arr[2]; }; struct S s; int main(void) { const struct S *p = &s; int *q = p->arr; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct S { int arr[2]; }; struct S s; int main(void) { const struct S *p = &s; *(p->arr + 1) += 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a[2];
int main(void) { const int *q = a; *(q + 1) = 2; return 0; }
EOF
try_compile_error_message "discarding const qualifier" << EOF
struct S { int arr[2]; }; struct S s; int main(void) { const struct S *p = &s; int *q = (*p).arr + 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } struct S { int (*cb)(int); int n; }; struct S s = {inc, 0}; int main(void) { const struct S *p = &s; p->cb = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } struct S { int (*cb)(int); int n; }; struct S s = {inc, 0}; int main(void) { const struct S *p = &s; (*p).cb = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } struct S { int (*cb)(int); }; struct S a[1] = {{inc}}; int main(void) { const struct S *p = a; p[0].cb = dec; return 0; }
EOF

# A member reached through an explicit dereference of a pointer to a const
# record is not a modifiable lvalue, whatever the grouping or update, while its
# pointee and the members of a modifiable record stay writable.
try_ 0 << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; struct S *m = &s; (*m).a = 5; (*m).p++; (m[0]).arr[1] += 1; (*m).in.x = 7; ++(*m).a; *(*ps).p = 9; return (*ps).a + (*ps).arr[1] + (*ps).in.x + (ps[0]).arr[0] + *(*ps).p + (int) ((*ps).p - v) - (6 + 4 + 7 + 2 + 9 + 1); }
EOF
try_ 0 << EOF
struct In { int x; };
struct S { struct In *q; const struct In *cq; int a; struct In in; };
struct In i1 = {1}, i2 = {2};
struct S s = {&i1, &i2, 3, {4}};
int take(struct In in) { return in.x; }
int main(void)
{
    const struct S *ps = &s;
    struct S *m = &s;
    (*ps).q->x = 5;
    ps[0].q->x += 1;
    (*m).cq = &i1;
    (*m).in.x = 8;
    return i1.x != 6 || (*ps).cq->x != 6 || take((*ps).in) != 8 ||
           (ps[0]).a != 3;
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).p++; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).a = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).arr[0] = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (ps[0]).a += 2; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).in.x = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; ++(*ps).a; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).a--; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { cs_t *ps = &s; (*ps).a = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*ps).in = s.in; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; }; struct S { int *p; int a; int arr[2]; struct In in; }; int v[2]; struct S s = {v, 1, {2, 3}, {4}}; typedef const struct S cs_t; int main(void) { const struct S *ps = &s; (*(ps)).arr[1]++; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; };
struct S { const struct In *cq; };
struct In i1 = {1};
struct S s = {&i1};
int main(void)
{
    struct S *m = &s;
    (*m).cq->x = 2;
    return 0;
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; };
struct S { const struct In cin; int *const cp; };
int v;
struct S s = {{1}, &v};
int main(void)
{
    struct S *m = &s;
    (*m).cin.x = 2;
    return 0;
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct In { int x; };
struct S { const struct In cin; int *const cp; };
int v, w;
struct S s = {{1}, &v};
int main(void)
{
    struct S *m = &s;
    (*m).cp = &w;
    return 0;
}
EOF

# A const pointer element of a row pointer is read-only through every store,
# while the row pointer itself and non-const elements stay modifiable.
try_ 0 << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; *(*r)[0] = 9; *r[0][1] = 8; crow_t t = &arr; *(*t)[0] += 1; int (*q)[2] = &c; (*q)[0] = 7; q[0][1] += 1; (*q)[1]++; ++q[0][0]; int *(*m)[2]; int *mm[2] = {&a, &b}; m = &mm; (*m)[0] = &b; m[0][1] = &a; return a + b + (*r)[1][0] + c[0] + c[1] - (11 + 8 + 8 + 8 + 5); }
EOF
try_ 0 << EOF
int a = 1, b = 2;
const int ca = 3, cb = 4;
int *const arr[2] = {&a, &b};
int *const arr2[2] = {&b, &a};
const int *carr[2] = {&ca, &cb};
typedef int *const cip;
typedef cip (*crow_t)[2];
int main(void)
{
    int *const (*r)[2] = &arr;
    crow_t t = &arr;
    const int *(*q)[2] = &carr;
    r = &arr2;
    t = &arr2;
    (*q)[0] = &cb;
    q[0][1] = &ca;
    (*q)[1]++;
    q[0][1]--;
    *(*r)[0] = 5;
    *t[0][1] += 1;
    return b != 5 || a != 2 || *(*q)[0] != 4 || *q[0][1] != 3 ||
           *(*t)[0] != 5;
}
EOF
try_ 0 << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int *const arr2[2] = {&b, &a}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; r = &arr2; return 0; }
EOF
try_ 0 << EOF
int a[2];
const int *p = a;
const int **pp = &p;
struct S { int *p; };
struct S s = {a};
int main(void)
{
    struct S *ps = &s;
    (*pp)++;
    (*ps).p++;
    return p != a + 1 || s.p != a + 1;
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; (*r)[0] = &b; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; r[0][1] = &b; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { crow_t r = &arr; (*r)[0] = &b; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { crow_t r = &arr; r[0][1] = &a; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; (*r)[0] += 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; (*r)[1]++; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; ++r[0][1]; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a = 1, b = 2; int *const arr[2] = {&a, &b}; int c[2] = {3, 4}; const int d[2] = {5, 6}; typedef int *const cip; typedef cip (*crow_t)[2]; int main(void) { int *const (*r)[2] = &arr; --(*r)[1]; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
const int ca = 3, cb = 4;
const int *carr[2] = {&ca, &cb};
int main(void) { const int *(*q)[2] = &carr; *(*q)[0] = 1; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int a[2];
int *const cp = a;
int *const *pcp = &cp;
int main(void)
{
    (*pcp)++;
    return 0;
}
EOF

# A typedef of a pointer to an array typedef of callbacks points to the whole
# row, whose elements are called and stored as through fn_t (*rows_t)[2].
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*fn_t)(int);
typedef fn_t arr_t[2];
typedef arr_t *rows_t;
fn_t fa[2] = {inc, dec};
rows_t g = &fa;
int main(void)
{
    rows_t r = &fa;
    arr_t *q = &fa;
    (*r)[0] = dec;
    int v = fa[0](1);
    (*r)[0] = inc;
    return (*r)[0](1) - 2 + (*g)[1](1) + r[0][1](3) - 2 + (*q)[1](5) - 4 + v +
           (sizeof(*r) != 2 * sizeof(fn_t)) + (**q)(7) - 8;
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*fn_t)(int);
typedef fn_t arr_t[2];
typedef arr_t *rows_t;
fn_t fa[2] = {inc, dec};
arr_t *gq = &fa;
int main(void)
{
    arr_t *q = &fa;
    rows_t r = &fa;
    int a = (*q)[1](5);
    int b = (*gq)[1](5);
    int c = q[0][0](1);
    int d = r[0][0](1);
    int e = (*r)[0](1);
    return a + b + c + d + e - 14;
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
typedef int (*const cfn_t)(int);
typedef cfn_t carr_t[2];
typedef carr_t *crows_t;
const cfn_t fa[2] = {inc, inc};
int main(void) { crows_t r = &fa; return 0; }
EOF
try_compile_error_message "assignment of read-only variable" << EOF
int inc(int x) { return x + 1; }
typedef int (*fn_t)(int);
typedef fn_t arr_t[2];
typedef arr_t *const crows_t;
fn_t fa[2] = {inc, inc};
fn_t fb[2] = {inc, inc};
int main(void) { crows_t r = &fa; (*r)[0] = inc; r = &fb; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
typedef int (*const cfn_t)(int);
typedef cfn_t carr_t[2];
typedef carr_t *crows_t;
const cfn_t fa[2] = {inc, inc};
int main(void) { crows_t r = &fa; (*r)[0] = inc; return 0; }
EOF

# A const specifier on the callback typedef, not only on the callback's own
# pointer, makes the row const. Such a pointer was taken for a pointer to
# modifiable callbacks and could not point to a const array of them.
try_ 0 << EOF
int inc(int x) { return x + 1; }
typedef int (*fn_t)(int);
typedef const fn_t (*crows_t)[2];
const fn_t fa[2] = {inc, inc};
int main(void) { crows_t c = &fa; return (*c)[1](5) - 6 + c[0][0](1) - 2; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
typedef int (*fn_t)(int);
typedef const fn_t (*crows_t)[2];
const fn_t fa[2] = {inc, inc};
int main(void) { crows_t c = &fa; (*c)[0] = inc; return 0; }
EOF

# The same pointers to rows of callbacks may be block-scope typedefs, and a row
# of const callbacks takes the address of a const array of them.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*fn_t)(int);
fn_t fa[2] = {inc, dec};
int main(void)
{
    typedef fn_t (*rows_t)[2];
    typedef int (*(*spelled_t)[2])(int);
    rows_t r = &fa;
    spelled_t s = &fa;
    (*r)[0] = dec;
    int v = fa[0](1);
    r[0][0] = inc;
    return v + (*r)[0](1) - 2 + (*s)[1](3) - 2 + r[0][1](5) - 4 + s[0][0](0) - 1 +
           (sizeof(*r) != 2 * sizeof(fn_t)) + (sizeof(rows_t) != sizeof(void *));
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*const cfn_t)(int);
typedef int (*fn_t)(int);
const cfn_t cfa[2] = {inc, dec};
fn_t fa[2] = {inc, dec};
int main(void)
{
    typedef cfn_t (*crows_t)[2];
    typedef fn_t (*rows_t)[2], (*other_t)[2];
    crows_t c = &cfa;
    rows_t r = &fa;
    other_t o = r;
    o[0][0] = dec;
    return (*c)[1](3) - 2 + c[0][0](1) - 2 + fa[0](1);
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*const cfn_t)(int);
typedef cfn_t (*crows_t)[2];
const cfn_t cfa[2] = {inc, dec};
int main(void)
{
    crows_t c = &cfa;
    return (*c)[1](3) - 2 + c[0][0](1) - 2;
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
typedef int (*fn_t)(int);
const fn_t fa[2] = {inc, inc};
int main(void) { typedef const fn_t (*crows_t)[2]; crows_t c = &fa; (*c)[0] = inc; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
typedef int (*const cfn_t)(int);
const cfn_t cfa[2] = {inc, inc};
int main(void) { typedef cfn_t (*crows_t)[2]; crows_t c = &cfa; c[0][1] = inc; return 0; }
EOF

# So may a pointer to a block-scope array typedef of callbacks, as at file
# scope; its row was taken for a callback and no element could be called.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*fn_t)(int);
typedef fn_t garr_t[2];
int main(void)
{
    typedef fn_t arr_t[2];
    typedef arr_t *rows_t;
    typedef garr_t *grows_t;
    fn_t fa[2] = {inc, dec};
    rows_t r = &fa;
    grows_t g = &fa;
    r[0][0] = dec;
    return (*r)[1](5) - 4 + r[0][1](5) - 4 + (*g)[0](1) + g[0][1](3) - 2 +
           (sizeof(*r) != 2 * sizeof(fn_t));
}
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
typedef int (*const cfn_t)(int);
const cfn_t fa[2] = {inc, inc};
int main(void) { typedef cfn_t carr_t[2]; typedef carr_t *crows_t; crows_t r = &fa; r[0][1] = inc; return 0; }
EOF

# A pointer to a row of callbacks, through a callback typedef or spelled out,
# calls and stores each element, and keeps a const callback read-only.
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*fn_t)(int); typedef fn_t (*rows_t)[2]; fn_t fa[2] = {inc, dec}; rows_t g = &fa; int main(void) { rows_t r = &fa; return (*r)[0](1) - 2 + (*g)[1](1) + r[0][1](3) - 2 + (sizeof(*r) != 2 * sizeof(fn_t)); }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*(*rows_t)[2])(int); int (*fa[2])(int) = {inc, dec}; rows_t g = &fa; int main(void) { rows_t r = &fa; return (*r)[0](1) - 2 + (*g)[1](1) + r[0][1](3) - 2; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int (*fa[2])(int) = {inc, dec}; int main(void) { int (*(*r)[2])(int) = &fa; return (*r)[0](1) - 2 + r[0][1](3) - 2; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*fn_t)(int); typedef fn_t (*rows_t)[2]; fn_t fa[2] = {inc, dec}; int main(void) { rows_t r = &fa; (*r)[0] = dec; return fa[0](1); }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*const cfn_t)(int); typedef cfn_t (*crows_t)[2]; cfn_t cfa[2] = {inc, dec}; int main(void) { crows_t r = &cfa; return (*r)[1](3) - 2 + r[0][0](1) - 2; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*const cfn_t)(int); typedef cfn_t (*crows_t)[2]; cfn_t cfa[2] = {inc, dec}; int main(void) { crows_t r = &cfa; (*r)[0] = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*const cfn_t)(int); typedef cfn_t (*crows_t)[2]; cfn_t cfa[2] = {inc, dec}; int main(void) { crows_t r = &cfa; r[0][1] = inc; return 0; }
EOF

# A file-scope pointer-to-array typedef may take a pointer typedef as its base,
# which only makes the row's element a pointer.
try_ 0 << EOF
typedef int *ip;
typedef ip (*rows_t)[2];
typedef ip (*grid_t)[2][3];
typedef int *(*direct_t)[2];
int a = 1, b = 2;
int *arr[2] = {&a, &b};
int *grid[2][3] = {{&a, &b, &a}, {&b, &a, &b}};
rows_t global_rows = &arr;
int main(void)
{
    rows_t r = &arr;
    grid_t g = &grid;
    direct_t d = &arr;
    (*r)[0] = &b;
    return *(*r)[1] != 2 || *r[0][0] != 2 || *(*global_rows)[0] != 2 ||
           *(*g)[1][2] != 2 || *(*d)[1] != 2 || sizeof(*r) != 2 * sizeof(ip) ||
           sizeof(*g) != 6 * sizeof(ip) || sizeof(rows_t) != sizeof(void *);
}
EOF
try_ 0 << EOF
struct P { int x; };
typedef struct P *pp;
typedef pp (*prow_t)[2];
struct P p1 = {5}, p2 = {6};
struct P *parr[2] = {&p1, &p2};
int main(void)
{
    prow_t r = &parr;
    return (*r)[1]->x != 6 || r[0][0]->x != 5 || sizeof(*r) != 2 * sizeof(pp);
}
EOF
try_ 0 << EOF
typedef int *const cip;
typedef cip (*crow_t)[2];
int a = 1, b = 2;
cip arr[2] = {&a, &b};
int main(void)
{
    crow_t r = &arr;
    return 0;
}
EOF
# The same pointer-to-array aliases declared at file scope.
try_ 0 << EOF
typedef int (*row_pointer)[2];
typedef int (*plane_pointer)[2][3];
typedef char *(*string_row_pointer)[3];
int row[2] = {3, 4};
int plane[2][3] = {{1, 2, 3}, {4, 5, 6}};
char *strings[3] = {"ab", "cd", "ef"};
row_pointer global_row = &row;
int first(row_pointer p) { return (*p)[1] + p[0][0]; }
int main(void) {
    plane_pointer q = &plane;
    string_row_pointer r = &strings;
    return first(&row) != 7 || (*global_row)[1] != 4 || (*q)[1][2] != 6 ||
           r[0][1][1] != 'd' || sizeof(row_pointer) != sizeof(int *) ||
           sizeof(*global_row) != 2 * sizeof(int) ||
           sizeof(*q) != 6 * sizeof(int) || sizeof(*r) != 3 * sizeof(char *);
}
EOF

# A pointer to a pointer to an array still points to the array after one
# dereference, and a pointer typedef as the element type stays pointer-sized.
try_ 0 << EOF
typedef int row[3];
typedef int (*row_pointer)[3];
typedef int *(*slot_row_pointer)[3];
typedef char *str;
int x = 9;
row r = {4, 5, 6};
int *slots[3] = {&x, 0, 0};
str strings[3] = {"ab", "cd", "ef"};
int main(void) {
    row *g2 = &r;
    row_pointer t = &r;
    row_pointer *tt = &t;
    int (**direct)[3] = &t;
    slot_row_pointer u = &slots;
    slot_row_pointer *uu = &u;
    str (*p)[3] = &strings;
    p[0][2] = "gh";
    return (*g2)[2] != 6 || (*tt)[0][1] != 5 || (*direct)[0][2] != 6 ||
           sizeof(**tt) != 3 * sizeof(int) || **uu != slots ||
           ***uu != &x || ****uu != 9 || sizeof(*p) != 3 * sizeof(str) ||
           sizeof(p[0][1]) != sizeof(str) || p[0][1][1] != 'd' ||
           strings[2][1] != 'h';
}
EOF
# A pointer-to-array member keeps its row once loaded: `*s.rows` is the row.
try_ 0 << EOF
typedef int (*row_pointer)[3];
int row[3] = {1, 2, 3};
int plane[2][3] = {{1, 2, 3}, {4, 5, 6}};
char *strings[2] = {"ab", "cd"};
struct S {
    int (*row)[3];
    row_pointer alias;
    int (*plane)[2][3];
    char *(*strings)[2];
    int (**slot)[3];
};
int main(void) {
    struct S s;
    struct S *ps = &s;
    int *first;
    s.row = &row;
    s.alias = &row;
    s.plane = &plane;
    s.strings = &strings;
    s.slot = &s.row;
    first = *s.row;
    (*s.row)[0] = 7;
    return (*s.row)[1] != 2 || (*ps->row)[2] != 3 || first[1] != 2 ||
           (*s.alias)[0] != 7 || (*ps->plane)[1][2] != 6 ||
           (*s.strings)[1][1] != 'd' || sizeof(*s.row) != 3 * sizeof(int) ||
           (**s.slot)[2] != 3 || row[0] != 7;
}
EOF
try_compile_error << EOF
typedef const int (*plane_pointer)[2][3];
int planes[1][2][3];
int main(void) {
    plane_pointer p = &planes[0];
    return ((*p)[1][2] = 15);
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
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = ((*p)[r++][c++] = 15);
    return value != 15 || r != 1 || c != 1 || (*p)[0][0] != 15;
}
EOF
try_compile_error << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef const int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return ((*p)[1][2] = 15);
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = ((*p)[1][2] += 9);
    return value != 15 || (*p)[1][2] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = ((*p)[r++][c++] += 9);
    return value != 10 || r != 1 || c != 1 || (*p)[0][0] != 10;
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

# Such a typedef names a pointer, not the record it defines: an array or record
# of them is initialized with pointers, at file and block scope. The elements
# were initialized as records, storing garbage.
try_ 0 << EOF
typedef struct rec { char c; int v; } *rptr;
struct rec grecs[2];
rptr grps[2] = {grecs, grecs + 1};
rptr gp = grecs + 1;
struct holder { rptr p; int n; } gh = {grecs, 5};
int main(void)
{
    struct rec recs[2];
    rptr rps[2] = {recs, recs + 1};
    rptr p = recs;
    struct holder h = {recs + 1, 7};
    rptr one[] = {0, recs};
    p->v = 3;
    rps[1]->v = 4;
    if (grps[0] != grecs || grps[1] != grecs + 1 || gp != grecs + 1) return 1;
    if (rps[0] != recs || rps[1] != recs + 1) return 2;
    if (gh.p != grecs || gh.n != 5 || h.p != recs + 1 || h.n != 7) return 3;
    if (sizeof one != 2 * sizeof(rptr) || one[1] != recs) return 4;
    if (sizeof(*p) != sizeof(struct rec) || recs[0].v != 3) return 5;
    return h.p->v != 4;
}
EOF

# A pointer typedef that defines its record, tagged or not, reaches the record
# through base_struct: sizeof(*p) and sizeof p[0] are the record's size, not a
# pointer's. Its step is the padded record size, for postfix as well as prefix
# updates, and the alias is a pointer operand, never a record one.
try_ 1 << EOF
typedef struct { int x, y; char c; } *PA, A;
typedef PA *PPA;
typedef struct T { int x, y; char c; } *TP;
typedef struct { int x, y; char c; } B, *PB;
typedef int **PP;
A arr[3];
B brr[3];
struct T trr[3];
int main(void) {
    typedef struct { char b[5]; int z; } *LP;
    LP lp = 0;
    PA pa = arr, pa2 = arr;
    PPA ppa = &pa;
    TP tp = trr;
    PB pb = brr;
    int v = 0, *vp = &v, *vq = &v;
    PP pp = &vp;
    int r = (sizeof(*pa) == sizeof(A)) + (sizeof pa[0] == sizeof(A)) * 2 +
            (sizeof(*pa).c == 1) * 4 + (sizeof **ppa == sizeof(A)) * 8 +
            (sizeof *lp == 3 * sizeof(int)) * 16;
    pa++;
    ++pa2;
    tp++;
    pb--;
    pb += 2;
    pp++;
    r += ((pa - arr) == 1 && pa2 == pa) * 32;
    r += ((char *) pa - (char *) arr == sizeof(A)) * 64;
    r += (tp - trr == 1 && pb - brr == 1) * 128;
    r += ((char *) (lp + 1) - (char *) lp == 3 * sizeof(int)) * 256;
    r += ((char *) pp - (char *) &vp == sizeof(int *)) * 512;
    (void) vq;
    return r == 1023;
}
EOF

# An array operand of a cast is its address: an element wider than a pointer
# made the cast a truncation of the array, which read back garbage once the
# address had to be reloaded around a call.
try_output 0 "12 12 24" << EOF
struct S { int x, y; char c; };
struct S srr[3];
struct S trr[3];
int main(void) {
    struct S *p = srr + 1;
    struct S *q = trr + 2;
    printf("%d ", (int) ((char *) p - (char *) srr));
    printf("%d %d", (int) ((char *) p - (char *) srr),
           (int) ((char *) q - (char *) trr));
    return 0;
}
EOF

# A typedef that defines a tagged record through a pointer declarator names the
# pointer, not the record: the tag keeps the record's own layout.
try_ 6 << EOF
typedef struct wide { char bytes[100]; int count; } *wide_ptr, wide_t;
typedef union mixed { char bytes[50]; int value; } *mixed_ptr;
int main(void) {
    struct wide w;
    union mixed m;
    wide_ptr p = &w;
    p->bytes[99] = 3;
    p->count = 4;
    return (sizeof(struct wide) == 104) + (sizeof(w) == 104) +
           (sizeof(wide_t) == 104) + (sizeof(union mixed) == 52) +
           (sizeof(m) == 52) + (w.bytes[99] + w.count == 7);
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

# A value named twice is computed once and copied, then read through both names.
# Folding the copy into the operation must not leave the first name unwritten
# while something still reads it.
try_ 12 << EOF
int f(int a, int b)
{
    int r = a * b;
    int s = a * b;
    return r + s;
}

int main()
{
    return f(2, 3);
}
EOF

try_ 0 << EOF
int fold_add(int a, int b) { int r = a + b; int s = a + b; return r + s; }
int fold_sub(int a, int b) { int r = a - b; int s = a - b; return r + s; }
int fold_mul(int a, int b) { int r = a * b; int s = a * b; return r + s; }
int fold_div(int a, int b) { int r = a / b; int s = a / b; return r + s; }
int fold_mod(int a, int b) { int r = a % b; int s = a % b; return r + s; }
int fold_and(int a, int b) { int r = a & b; int s = a & b; return r + s; }
int fold_or(int a, int b) { int r = a | b; int s = a | b; return r + s; }
int fold_xor(int a, int b) { int r = a ^ b; int s = a ^ b; return r + s; }
int fold_shl(int a, int b) { int r = a << b; int s = a << b; return r + s; }
int fold_shr(int a, int b) { int r = a >> b; int s = a >> b; return r + s; }
int main()
{
    int bad = 0;
    if (fold_add(29, 3) != 64)
        bad |= 1;
    if (fold_sub(29, 3) != 52)
        bad |= 2;
    if (fold_mul(29, 3) != 174)
        bad |= 4;
    if (fold_div(29, 3) != 18)
        bad |= 8;
    if (fold_mod(29, 3) != 4)
        bad |= 16;
    if (fold_and(29, 3) != 2)
        bad |= 32;
    if (fold_or(29, 3) != 62)
        bad |= 64;
    if (fold_xor(29, 3) != 60)
        bad |= 128;
    if (fold_shl(29, 3) != 464)
        bad |= 256;
    if (fold_shr(29, 3) != 6)
        bad |= 512;
    return bad != 0;
}
EOF

try_ 38 << EOF
int f(int a, int b)
{
    int r = a * b;
    int s = a * b;
    while (b-- > 0)
        a += r + s;
    return a;
}

int main()
{
    return f(2, 3);
}
EOF

# Folding a constant load into the operation after it, as in 0 + b, leaves the
# constant's register without it, which is wrong while something else still
# reads that register. The third argument is unused, so the register holds 64.
try_ 0 << EOF
int fold_add(int a, int b, int c)
{
    int k = 0;
    int r = k + b;
    return r + (k >> a);
}

int fold_neg(int a, int b, int c)
{
    int k = 0;
    int r = k - b;
    return r + (k >> a);
}

int fold_zero(int a, int b, int c)
{
    int k = 0;
    int r = k * b;
    return r + (k >> a);
}

int fold_one(int a, int b, int c)
{
    int k = 1;
    int r = k * b;
    return r + (k >> a);
}

int main()
{
    int bad = 0;
    if (fold_add(1, 5, 64) != 5)
        bad |= 1;
    if (fold_neg(1, 5, 64) != -5)
        bad |= 2;
    if (fold_zero(1, 5, 64) != 0)
        bad |= 4;
    if (fold_one(1, 5, 64) != 5)
        bad |= 8;
    return bad;
}
EOF

# Strength reduction loads the shift count 2 over the constant 4 it replaces, so
# the constant has to be unread afterwards.
try_ 22 << EOF
int f(int a, int b, int c)
{
    int k = 4;
    int r = k * b;
    return r + (k >> a);
}

int main()
{
    return f(1, 5, 64);
}
EOF

# x & 0 becomes a load of zero into the result. The constant's register loses
# the zero, and the operation it replaces must not simply disappear.
try_ 0 << EOF
int f(int a, int b, int c)
{
    int k = 0;
    int r = k & b;
    return r + (k >> a);
}

int main()
{
    return f(1, 5, 64);
}
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

# A shift count outside the width of int has no defined result. The compiler
# must not fold one with the host's shift, which on x86 reduced "1 << 40" to 256
# while a count known only at run time gave the target's result; leaving the
# shift to the target makes both spellings agree.
try_ 1 << EOF
int shift_count(int count) { return count; }
int main(void)
{
    int wide = shift_count(40);
    int negative = shift_count(-1);

    return (1 << 40) == (1 << wide) && (8 >> 33) == (8 >> (wide - 7)) &&
           (1 << -1) == (1 << negative) && (-1 << 3) == -8;
}
EOF
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
    "4 int value = 1; return value++, value + 2;"
    "4 int value = 0; return 1 ? value++, value + 3 : 0;"
    "5 int value = 0; return 1 ? value = 2, value + 3 : 0;"
    "3 return 0 ? 1 : 0 ? 2 : 3;"
    "3 return 1 ? 0 ? 2 : 3 : 4;"
)

run_items_tests return_tests

# Category: Variables and Assignments
begin_category "Variables and Assignments" "Testing variable declarations and assignments"

try_ 5 << EOF
int main(void) {
    typedef int local_count;
    local_count value = 5;
    return value;
}
EOF
try_ 7 << EOF
int main(void) {
    typedef int *local_pointer;
    int value = 7;
    local_pointer pointer = &value;
    return *pointer;
}
EOF

# A block pointer typedef names the same type as a file-scope one: its
# dereference is the pointee, not the pointer, so arithmetic on it must not
# scale, and a subscript through a hidden pointer-to-pointer steps by slots.
try_ 8 << EOF
int main(void) { int e = 7; typedef int *EP; EP q = &e; return *q + 1; }
EOF
try_ 65 << EOF
int main(void) {
    int a[3] = {5, 6, 7};
    typedef int *EP;
    EP q = a;
    return q[0] + *(q + 1) * 10 + (q + 2)[0] * 100 - 700;
}
EOF
try_ 13 << EOF
int main(void) {
    char c[2] = {3, 9};
    typedef char *CP;
    CP q = c;
    return *q + *(q + 1) + 1;
}
EOF
try_output 0 "4 6 5 10 30 20 10 20 2 1 250 3 12 2 8 1 2 2 3 4 3" << EOF
struct S { int m; char c; int n; };
int main(void) {
    struct S s[2] = {{1, 2, 3}, {4, 5, 6}};
    typedef struct S *SP;
    SP p = s;
    SP p1 = p + 1;
    short sh[3] = {10, 20, 30};
    typedef short *SHP;
    SHP h = sh;
    typedef SHP *SHPP;
    SHPP hh = &h;
    long long ll[2] = {100, 200};
    typedef long long *LLP;
    LLP l = ll;
    typedef unsigned char *UCP;
    unsigned char uc[2] = {250, 3};
    UCP u = uc;
    typedef int *IP;
    typedef IP IP2;
    int arr[4] = {1, 2, 3, 4};
    IP2 ip = arr;
    printf("%d %d %d ", p[1].m, p1->n, p1[0].c);
    printf("%d %d %d %d %d ", *h, h[2], *(h + 1), **hh, hh[0][1]);
    printf("%d %d ", (int) (l[1] / 100), (int) (*l / 100));
    printf("%d %d ", *u, u[1]);
    printf("%d %d %d %d %d ", (int) sizeof(*p), (int) sizeof(*h),
           (int) sizeof(*l), (int) sizeof(*u), (int) sizeof(**hh));
    ip++;
    printf("%d %d %d %d", *ip, ip[1], *(ip + 2), (int) ((arr + 4) - ip));
    return 0;
}
EOF
try_ 61 << EOF
typedef int **GPP;
typedef int *GP;
struct T { GP p; GPP gp; };
int main(void) {
    int ia[3] = {10, 20, 30};
    int *ip = ia;
    typedef int **PP;
    typedef int *IP;
    PP pp = &ip;
    GPP gp = &ip;
    IP rows[2] = {ia, ia + 1};
    struct T t;
    struct T *tp = &t;
    t.p = ia;
    t.gp = &ip;
    /* 20 + 10 + 10 + 1 + 20 = 61 */
    return pp[0][1] + *gp[0] + (rows[1][1] - rows[0][1]) +
           (t.gp[0][2] == 30) + tp->p[1];
}
EOF
try_ 11 << EOF
struct S { int m; int n; };
int main(void) {
    typedef struct S *SP;
    typedef struct S **SPP;
    struct S s[2] = {{1, 2}, {3, 4}};
    SP sp = s;
    SP sps[2] = {s + 1, s};
    SPP spp = &sp;
    SPP spp2 = sps;
    /* 4 + 3 + 2 + 2 = 11 */
    return spp2[1][1].n + spp2[0]->m + spp[0]->n + sps[1]->n;
}
EOF
try_ 9 << EOF
typedef int outer_type;
int main(void) {
    outer_type outer = 9;
    { typedef int outer_type; outer_type inner = outer; return inner; }
}
EOF
try_ 8 << EOF
int main(void) {
    typedef int *(*rows_t)[2];
    int first = 3, second = 7;
    int *data[1][2] = { { &first, &second } };
    rows_t p = data;
    return *p[0][1] + 1;
}
EOF
try_ 8 << EOF
int main(void) {
    typedef int *pointer_row[2];
    typedef pointer_row pointer_row_alias;
    int first = 3, second = 7;
    pointer_row_alias values = { &first, &second };
    return *values[1] + 1;
}
EOF
try_ 8 << EOF
typedef int *global_pointer_row[2];
int main(void) {
    int first = 3, second = 7;
    global_pointer_row values = { &first, &second };
    return *values[1] + 1;
}
EOF
try_ 8 << EOF
typedef int *pointer_alias;
typedef pointer_alias global_pointer_row[2];
int main(void) {
    int first = 3, second = 7;
    global_pointer_row values = { &first, &second };
    return *values[1] + 1;
}
EOF

# A file-scope typedef reads a declarator list too, and its specifier with its
# qualifiers applies to each declarator.
try_compile_error_message "assignment of read-only location" << EOF
typedef const int CI, *CP;
int main(void) { int x = 1; CP p = &x; *p = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only variable" << EOF
typedef int *IP;
typedef const IP CP, CQ[2];
int main(void) { IP x = 0; CP p = 0; p = x; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
typedef const struct { int x, y; } CR, *CRP;
int main(void) { CR c = {1, 2}; CRP q = &c; q->x = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
typedef const enum { A1, B1 } CE, *CEP;
int main(void) { CE e = A1; CEP p = &e; *p = B1; return 0; }
EOF
try_ 42 << EOF
typedef struct { int m; int n; } S, *SP, SA[2];
typedef struct tagged { int v; } T, *TP;
typedef struct later L, *LP;
typedef union { int i; char c; } U, *UP;
typedef int I, *IP, IA[3], (*FP)(int);
typedef enum { E0, E1, E2 } E, *EP;
typedef const int *CIP;
typedef IP const CP;
struct later { int w; };
int twice(int v) { return 2 * v; }
int main(void) {
    int x = 3;
    S s[2] = {{1, 2}, {3, 4}};
    SP sp = s + 1;
    SA sa;
    T t = {5};
    TP tp = &t;
    struct tagged *raw = tp;
    L l;
    LP lp = &l;
    U u;
    UP up = &u;
    IA ia = {1, 2, 3};
    IP ip = ia;
    FP fp = twice;
    E e = E2;
    EP ep = &e;
    CIP cip = &x;
    CP cp = &x;
    lp->w = 6;
    up->i = 7;
    sa[1] = s[0];
    *cp += 1;
    /* 4 + 5 + 6 + 7 + 3 + 2 + 2 + 4 + 1 + 8 */
    return sp->n + raw->v + l.w + u.i + ip[2] + *ep + sa[1].n + *cip +
           (sizeof(sa) == 2 * sizeof(S)) + fp(4);
}
EOF

# A record typedef may derive an array at file scope as in a block, whether it
# names a tag or defines the record, and an array parameter of it is a pointer.
try_ 64 << EOF
struct P { int a, b; };
typedef struct P pair_row[2];
typedef struct { int a; } untagged_row[3], untagged_one;
typedef union { int i; char c; } union_row[2], *union_ptr;
typedef struct Q { int x; } q_grid[2][3], q_one;
typedef struct P *pair_ptr_row[2];
typedef struct P pair_alias;
typedef pair_alias alias_row[2];
pair_row global_pairs;
int second_b(pair_row row) { return row[1].b; }
int third_a(untagged_row row) { return row[2].a; }
int main(void) {
    typedef struct P local_row[4];
    typedef struct { char c; } local_untagged[5];
    pair_row pairs;
    untagged_row ur;
    untagged_one one;
    union_row un;
    union_ptr up = un;
    q_grid grid;
    q_one q;
    struct Q *qp = &grid[1][2];
    pair_ptr_row ptrs;
    alias_row aliases;
    local_row lr;
    local_untagged lu;
    pairs[1].b = 1;
    ur[2].a = 2;
    one.a = 3;
    up[1].c = 4;
    grid[1][2].x = 5;
    q.x = 6;
    ptrs[1] = &pairs[1];
    aliases[0].a = 7;
    global_pairs[1].a = 8;
    lr[3].b = 9;
    lu[4].c = 10;
    return second_b(pairs) + third_a(ur) + one.a + un[1].c + qp->x + q.x +
           ptrs[1]->b + aliases[0].a + global_pairs[1].a + lr[3].b + lu[4].c +
           (sizeof(pair_row) == 2 * sizeof(struct P)) +
           (sizeof(untagged_row) == 3 * sizeof(int)) +
           (sizeof(union_row) == 2 * sizeof(int)) +
           (sizeof(q_grid) == 6 * sizeof(int)) +
           (sizeof(pair_ptr_row) == 2 * sizeof(struct P *)) +
           (sizeof(local_row) == 4 * sizeof(struct P)) +
           (sizeof(local_untagged) == 5) + (sizeof(global_pairs) == 16);
}
EOF
try_compile_error_message "Typedef array element has incomplete record type" << EOF
struct incomplete;
typedef struct incomplete incomplete_row[2];
int main(void) { return 0; }
EOF
try_ 14 << EOF
typedef struct { int a; int b; } *AP, A;
typedef union { int i; char c[8]; } *UP, U;
typedef struct later *LP, L;
struct later { int w; int z; };
int main(void) {
    A a = {1, 2};
    AP ap = &a;
    U u;
    UP up = &u;
    L l;
    LP lp = &l;
    lp->z = 3;
    up->i = 4;
    /* 2 + 4 + 3 + 1 + 1 + 1 + 2 */
    return ap->b + u.i + l.z + (sizeof(A) == 8) +
           (sizeof(U) == 8) + (sizeof(L) == 8) + ap[0].a * 2;
}
EOF
try_compile_error << EOF
int main(void) { typedef int invalid_local = 1; return 0; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int inner_only; inner_only value = 1; }
    inner_only expired = 2;
    return expired;
}
EOF
try_compile_error << EOF
typedef int hidden_type;
int main(void) {
    { int hidden_type = 0; hidden_type value = 1; return value; }
}
EOF
try_ 11 << EOF
typedef int shadowed_type;
int main(void) {
    { typedef int shadowed_type; shadowed_type value = 11; return value; }
}
EOF
try_compile_error << EOF
int main(void) { int collision; typedef int collision; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int collision; int collision; return 0; }
EOF
try_compile_error << EOF
typedef int parameter_type;
int parameter_hides_type(int parameter_type) {
    parameter_type value = 1;
    return value;
}
EOF
try_ 12 << EOF
struct block_record { int value; };
int main(void) {
    typedef struct block_record record_alias, *record_pointer;
    record_alias value = { 5 };
    record_pointer pointer = &value;
    pointer->value += 7;
    return value.value;
}
EOF
try_ 9 << EOF
union block_union { int value; char byte; };
int main(void) {
    typedef union block_union union_alias;
    union_alias value;
    value.value = 9;
    return value.value;
}
EOF

# A block typedef may define the record or enum it names, tagged or not, and
# give it several declarators; the tag belongs to the block.
try_ 7 << EOF
int main(void) {
    typedef struct Q { int b; } R;
    R r;
    struct Q q2;
    r.b = 3;
    q2.b = 4;
    return r.b + q2.b;
}
EOF
try_ 56 << EOF
struct Q { char outer[8]; };
int bump(void *p);
int main(void) {
    typedef struct { int b; } R, *RP;
    typedef struct node { struct node *next; int v; } node_t, *node_ptr;
    typedef union { int x; char c[8]; } U;
    typedef enum { AA = 5, BB } E;
    R r = {2};
    RP p = &r;
    node_t n1, n2;
    node_ptr np = &n1;
    U u;
    E e = BB;
    n1.next = &n2;
    n2.v = 9;
    p->b += 1;
    u.x = 2;
    {
        typedef struct Q { short s; } R;
        R inner;
        struct Q tagged;
        inner.s = 4;
        tagged.s = 5;
        if (sizeof(R) != 2 || sizeof(struct Q) != 2)
            return 1;
        r.b += inner.s + tagged.s;
    }
    /* 12 + 8 + 6 + 9 + 1 + 4 + 16 = 56 */
    return r.b + sizeof(U) + e + np->next->v +
           (sizeof(node_t) == 2 * sizeof(node_ptr)) + sizeof(R) +
           (sizeof(struct Q) == 8) * 16;
}
EOF
try_ 12 << EOF
int main(void) {
    typedef struct { int a; } I;
    typedef struct { I in; int b; } N;
    typedef struct { int x, y; } const CP, *CPP;
    typedef const union { int x; } CU;
    N n = {{5}, 7};
    CP c = {1, 2};
    CPP q = &c;
    CU cu = {9};
    return n.in.a + n.b + (q->y == 2) + (cu.x == 9) - 2;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef const struct { int x; } CR;
    CR c = {1};
    c.x = 2;
    return c.x;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef struct { int x; } const CR;
    CR c = {1};
    c.x = 2;
    return c.x;
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef struct scoped { int x; } S; }
    struct scoped s;
    s.x = 1;
    return s.x;
}
EOF

# The specifiers of a typedef, with their qualifiers, belong to every declarator
# of its list; const on a pointer typedef qualifies that pointer.
try_compile_error_message "assignment of read-only location" << EOF
int main(void) { int x = 1; typedef const int CI, *CP; CP p = &x; *p = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int main(void) { int x = 1; typedef int const CI, *CP; CP p = &x; p[0] = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only variable" << EOF
int main(void) { typedef int *IP; IP x = 0; typedef const IP CP, CQ; CQ p = 0; p = x; return 0; }
EOF
try_compile_error_message "assignment of read-only variable" << EOF
int main(void) { typedef int *IP; IP x = 0; typedef IP const CP; CP p = 0; p = x; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
struct S { int x; };
int main(void) { typedef const struct S CR, *CRP; struct S s; CRP q = &s; q->x = 2; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int main(void) { typedef const enum { A1, B1 } CE, *CEP; CE e = A1; CEP p = &e; *p = B1; return 0; }
EOF
try_ 3 << EOF
int main(void) {
    int x = 1;
    typedef int *IP, **IPP;
    typedef int *const CPP, *NP;
    typedef const IP CIP, *PCIP;
    IP p = &x;
    IPP q = &p;
    NP n = &x;
    CIP c = &x;
    PCIP pc = &c;
    **q = 2;
    n = p;
    *c += 1;
    return **pc;
}
EOF
try_compile_error << EOF
struct record_expiry { int value; };
int main(void) {
    { typedef struct record_expiry inner_record; inner_record value = { 1 }; }
    inner_record expired;
    return 0;
}
EOF
try_compile_error << EOF
struct tag_namespace { int value; };
int main(void) {
    typedef struct tag_namespace ordinary_alias;
    struct ordinary_alias not_a_tag;
    return 0;
}
EOF

# A typedef naming an undeclared tag declares that tag incomplete: the alias
# serves for pointers, completes with the later definition, and still cannot
# define an object before then.
try_compile_error_message "Incomplete struct/union type cannot define an object" << EOF
int main(void) { typedef struct missing_tag missing_alias; missing_alias object; return 0; }
EOF
try_ 4 << EOF
int main(void) {
    typedef struct missing_tag missing_alias;
    missing_alias *p = 0;
    struct missing_tag { int v; } o;
    o.v = 4;
    p = &o;
    return p->v;
}
EOF
try_compile_error << EOF
union kind_check { int value; };
int main(void) { typedef struct kind_check wrong_kind; return 0; }
EOF
try_ 4 << EOF
struct forward_record;
int main(void) {
    typedef struct forward_record *forward_pointer;
    forward_pointer pointer = 0;
    return pointer == 0 ? 4 : 0;
}
EOF

# A file-scope tag declaration without declarators may repeat a tag that is
# already forward declared or complete; it refers to the same type.
try_ 12 << EOF
struct redeclared_record;
struct redeclared_record;
struct redeclared_record { int value; };
struct redeclared_record;
union redeclared_union { int value; };
union redeclared_union;
union redeclared_union;
int main(void)
{
    struct redeclared_record record;
    union redeclared_union alias;
    record.value = 5;
    alias.value = 7;
    return record.value + alias.value;
}
EOF

# A record tag declared inside a function belongs to that block: it neither
# clashes with a later file-scope tag of the other kind nor hides one from
# another function, and a local definition shadows the file-scope one.
try_ 14 << EOF
void block_union_owner(void) { union scoped_tag { int value; } u; u.value = 1; }
struct scoped_tag { int field; };
struct shadowed_tag { int a; int b; };
int shadowing_function(void) {
    struct shadowed_tag { char c; } local;
    local.c = 5;
    return local.c + sizeof(local);
}
int main(void) {
    struct scoped_tag s;
    struct shadowed_tag t;
    s.field = 3;
    t.b = 2;
    block_union_owner();
    return s.field + shadowing_function() + t.b - 3 + sizeof(t) - 2;
}
EOF

# sizeof, casts and other lookups that name a tag also see the innermost one,
# and two functions may each declare their own record under one tag.
try_ 18 << EOF
struct outer_shadow { int a, b; };
int local_sizeof(void) {
    struct outer_shadow { char c; } l;
    l.c = 0;
    return sizeof(struct outer_shadow) + l.c;
}
int first_owner(void) { struct own_tag { int a, b; } x; x.a = 0; return sizeof(struct own_tag) + x.a; }
int second_owner(void) { struct own_tag { char c; } y; y.c = 0; return sizeof(struct own_tag) + y.c; }
int main(void) { return local_sizeof() + first_owner() + second_owner() + 8; }
EOF
# Every declaration of a tag names the same kind of tag.
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
union mismatched_tag { int value; };
struct mismatched_tag;
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
struct mismatched_definition;
union mismatched_definition { int value; };
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
union mismatched_use { int value; };
struct mismatched_use object;
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
union mismatched_alias { int value; };
typedef struct mismatched_alias mismatched_alias_t;
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
enum mismatched_enum { MISMATCHED_ENUM };
int main(void) { struct mismatched_enum *p = 0; return p != 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
int main(void) { struct block_kind; union block_kind u; return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
int main(void) { struct implicit_kind *p = 0; union implicit_kind *q = 0; return p != 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
struct offsetof_kind { int a; };
int main(void) { return __builtin_offsetof(union offsetof_kind, a); }
EOF
# An enum tag is checked against record tags in the same name space too.
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
struct enum_reuse { int a; };
enum enum_reuse { ENUM_REUSE };
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
union enum_use { int a; };
enum enum_use object;
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
int main(void) { struct block_enum { int a; }; enum block_enum { BLOCK_ENUM }; return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
struct outer_enum_use { int a; };
int main(void) { enum outer_enum_use value; return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
struct param_enum_use { int a; };
int use(enum param_enum_use value) { return 0; }
int main(void) { return 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
union cast_enum_use { int a; };
int main(void) { return (enum cast_enum_use) 0; }
EOF
try_compile_error_message "tag was previously declared as a different kind of tag" << EOF
int main(void) { enum block_record { BLOCK_RECORD }; struct block_record { int a; }; return 0; }
EOF

# A scope defines the content of a tag only once (C99 6.7.2.3p1). Forward
# declarations and an inner block's own definition remain valid.
try_ 9 << EOF
struct T;
struct T;
typedef struct T T_alias;
struct T { int a; };
struct T;
union U { int a; };
enum E { E_OUTER = 3 };
int main(void) {
    struct T;
    struct T { char c; } t;
    struct T;
    t.c = 1;
    {
        struct T { int x, y; };
        union U { char z; };
        enum E { E_INNER = 4 };
        T_alias outer;
        outer.a = 5;
        return sizeof(struct T) + sizeof(union U) + t.c + E_OUTER + E_INNER +
               outer.a - 13;
    }
}
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
struct twice { int a; };
struct twice { int b; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
union twice { int a; };
union twice { int b; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
struct twice { int a; };
typedef struct twice { int b; } twice_t;
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
typedef union twice { int a; } twice_t;
union twice { int b; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
struct outer { struct nested { int a; } in; };
struct nested { int b; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
int main(void) { struct twice { int a; }; struct twice { int b; }; return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
int main(void) { union twice { int a; } u; typedef union twice { int b; } t; return 0; }
EOF

# A member list opens no scope, so defining the tag again inside its own member
# list is a redefinition, although the tag is still incomplete there.
try_compile_error_message "redefinition of struct or union tag" << EOF
struct self { struct self { int x; } inner; };
int main(void) { return sizeof(struct self); }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
union self { int a; union self { int x; } inner; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
struct self { struct middle { struct self { int y; } deepest; } inner; };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
typedef struct self { struct self { int x; } inner; } self_t;
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
typedef union self { union self { int x; } inner; } self_t;
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
int main(void) { struct self { struct self { int x; } inner; } s; return 0; }
EOF
try_compile_error_message "redefinition of struct or union tag" << EOF
int main(void) { union self { int a; union self { int x; } inner; } u; return 0; }
EOF
try_compile_error << EOF
enum self { SELF_A = sizeof(struct { enum self { SELF_B } e; }) };
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { enum self { SELF_A = sizeof(struct { enum self { SELF_B } e; }) }; return 0; }
EOF
# The tag may still be named, by pointer, and redefined in an inner block.
try_ 5 << EOF
int main(void) {
    struct self { struct self *next; struct back { struct self *owner; } b; } s;
    s.b.owner = &s;
    return s.b.owner == &s ? 5 : 0;
}
EOF
try_ 4 << EOF
int main(void) {
    struct self { int x; };
    {
        struct self { struct self *p; int y; } s;
        s.p = &s;
        s.y = 4;
        return s.p->y;
    }
}
EOF
try_compile_error_message "redefinition of enum tag" << EOF
enum twice { TWICE_A };
enum twice { TWICE_B };
int main(void) { return 0; }
EOF
try_compile_error_message "redefinition of enum tag" << EOF
int main(void) { enum twice { TWICE_A }; enum twice { TWICE_B }; return 0; }
EOF

# An inner scope may reuse the name for another kind of tag, and an enum tag
# never names, or redefines, an ordinary typedef of the same spelling.
try_ 0 << EOF
typedef char enum_typedef;
enum enum_typedef { ENUM_TYPEDEF = 1 };
union outer_union { int a; };
enum_typedef c;
int main(void) {
    struct shadowed { char x; };
    {
        enum shadowed { SHADOWED = 2 };
        enum shadowed s = SHADOWED;
        enum enum_typedef t = ENUM_TYPEDEF;
        if (sizeof(s) != sizeof(int) || sizeof(t) != sizeof(int))
            return 1;
    }
    {
        enum outer_union { OUTER_UNION = 3 };
        if (OUTER_UNION != 3)
            return 2;
    }
    if (sizeof(struct shadowed) != 1)
        return 3;
    return sizeof(enum_typedef) + sizeof(c) - 2;
}
EOF

# A tag defined in one function is not visible from another: naming it there
# declares a new incomplete tag, so a pointer is fine and an object is not.
try_compile_error_message "Incomplete struct/union type cannot define an object" << EOF
int owner(void) { struct foreign_tag { int a; } x; x.a = 1; return x.a; }
int main(void) { struct foreign_tag y; return 0; }
EOF

# A specifier naming an undeclared tag declares it incomplete in the current
# scope, and members of a block-scope record see the block's own tags.
try_ 21 << EOF
struct file_later *file_ptr;
struct file_later { int v; };
int owner(void) { struct foreign_ptr { int a, b; } x; x.a = 1; x.b = 2; return x.a + x.b; }
int user(void) { struct foreign_ptr *p = 0; return p == 0; }
int members(void) {
    struct inner { int x; };
    struct outer { struct inner in; struct unseen *link; int y; } o;
    o.in.x = 3;
    o.link = 0;
    o.y = 4;
    return o.in.x + o.y;
}
int late(void) {
    struct late_tag *lp;
    struct late_tag { int q; } l;
    l.q = 6;
    lp = &l;
    return lp->q;
}
int main(void) {
    struct file_later f;
    f.v = 7;
    file_ptr = &f;
    return owner() + user() + members() + late() + file_ptr->v - 3;
}
EOF
try_compile_error << EOF
struct incomplete_record;
int main(void) {
    typedef struct incomplete_record incomplete_alias;
    incomplete_alias value;
    return 0;
}
EOF
try_ 8 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    row_pointer q = p + 1;
    return q[0][1];
}
EOF
try_ 3 << EOF
int main(void) {
    int src[2][2] = { { 1, 2 }, { 3, 4 } };
    int (*q)[2] = src + 1;
    return q[0][0];
}
EOF

# A row of a multidimensional array decays to a pointer to its first element
# instead of being loaded as though it were a scalar element.
try_ 0 << EOF
int global_rows[3][2] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
int global_planes[2][2][3] = { { { 1, 2, 3 }, { 4, 5, 6 } },
                               { { 7, 8, 9 }, { 10, 11, 12 } } };
int second(int *row) { return row[1]; }
int main(void) {
    int v[2][2] = { { 1, 2 }, { 3, 0 } };
    int *p;
    p = v[1];
    int *q = v[0];
    int *g = global_rows[2];
    int *r = global_planes[1][0];
    int (*plane)[3] = global_planes[1];
    int (*next)[3];
    next = global_planes[0] + 1;
    int i = 1;
    return p[0] != 3 || q[1] != 2 || g[0] != 5 || r[2] != 9 ||
           plane[1][1] != 11 || next[0][2] != 6 || second(v[1]) != 0 ||
           *(v[1] + 1) != 0 || *v[0] != 1 || *(global_rows[i] + 1) != 4 ||
           v[1] - v[0] != 2 || global_rows[2] - global_rows[0] != 4 ||
           sizeof(v[1]) != 2 * sizeof(int) || !v[1] ||
           sizeof(*global_planes[1]) != 3 * sizeof(int);
}
EOF

# An array name compared against a pointer decays to the address of its first
# element. A narrow element type must not make the comparison an integer one,
# which used to sign-extend the address from its low byte.
ans="1100
1111
1110
1111
1100
0011
0111
3"
try_output 0 "$ans" << EOF
struct holder {
    char field[4];
    int numbers[2];
};

int by_param(char param[], void *raw)
{
    return (raw == param) + 2 * (param == raw) + 4 * (raw != param);
}

int main(void)
{
    char buf[8];
    char grid[2][3];
    struct holder box;
    int values[3];
    char *p = buf;
    int *ip = values;
    void *r = p;
    void *rg = grid;
    void *rg1 = grid[1];
    void *rf = box.field;
    void *rv = ip;

    printf("%d%d%d%d\n", r == buf, buf == r, r != buf, buf != r);
    printf("%d%d%d%d\n", r == &buf[0], &buf[0] == r, r == &buf, &buf == r);
    printf("%d%d%d%d\n", rg == grid, grid == rg, rg1 == grid[1],
           grid[0] != rg);
    printf("%d%d%d%d\n", rf == box.field, box.field == rf, rv == values,
           values == rv);
    printf("%d%d%d%d\n", rv == ip, ip == values, rv != values,
           rf != box.field);
    printf("%d%d%d%d\n", (char *) r < buf, buf < (char *) r,
           (char *) r >= buf, buf >= (char *) r);
    printf("%d%d%d%d\n", (char *) r > buf, (int *) rv <= values,
           (int *) rv >= values, (char *) rg1 > grid[0]);
    printf("%d\n", by_param(buf, r));
    return 0;
}
EOF
try_ 0 << EOF
struct point { int x, y; };
struct grid { int tag; int cells[2][3]; struct point points[2][2]; };
struct grid global_grid = { 7, { { 1, 2, 3 }, { 4, 5, 6 } },
                            { { { 1, 2 }, { 3, 4 } }, { { 5, 6 }, { 7, 8 } } } };
int third(const int *row) { return row[2]; }
int main(void) {
    struct grid local = global_grid;
    struct grid *pointer = &local;
    int *cells = local.cells[1];
    struct point *points = pointer->points[1];
    struct point matrix[2][2] = { { { 1, 2 }, { 3, 4 } }, { { 5, 6 }, { 7, 8 } } };
    struct point *row = matrix[1];
    struct point *second = matrix[0] + 1;
    return cells[0] != 4 || third(pointer->cells[0]) != 3 ||
           *(global_grid.cells[1] + 2) != 6 || points[1].x != 7 ||
           pointer->points[1]->y != 6 || row[1].y != 8 || matrix[1]->x != 5 ||
           second->x != 3;
}
EOF
try_ 7 << EOF
int x = 5, y = 7;
int *slots[2][2];
int main(void) {
    slots[1][0] = &x;
    slots[1][1] = &y;
    int **row = slots[1];
    return *row[1] == *slots[1][1] && row + 1 == slots[1] + 1 ? *slots[1][1] : 0;
}
EOF
try_ 0 << EOF
int rows[2][2] = { { 1, 2 }, { 3, 4 } };
int planes[2][2][3] = { { { 1, 2, 3 }, { 4, 5, 6 } },
                        { { 7, 8, 9 }, { 10, 11, 12 } } };
int *row = rows[1];
int *after = rows[0] + 1;
int (*plane_row)[3] = planes[1] + 1;
int *slots[3] = { rows[1], rows[0] + 1, planes[1][1] + 2 };
int main(void) {
    static int *local = rows[1] + 1;
    return row[1] != 4 || *after != 2 || plane_row[0][0] != 10 ||
           *slots[0] != 3 || *slots[1] != 2 || *slots[2] != 12 || *local != 4;
}
EOF
try_compile_error_message "Global initializer requires a constant address" << EOF
int rows[2][2];
int *element = rows[1][0];
int main(void) { return 0; }
EOF
try_ 3 << EOF
int main(void) {
    int src[2][2] = { { 1, 2 }, { 3, 4 } };
    int (*slots[2])[2] = { src, src + 1 };
    int (**p)[2] = slots;
    return p[1][0][0];
}
EOF
try_ 0 << EOF
int main(void) {
    int rows[2][2] = { { 2, 5 }, { 3, 7 } };
    int (*planes[])[2] = { rows, rows };
    return planes[1][1][0] != 3;
}
EOF
try_ 6 << EOF
int main(void) {
    short src[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    short (*q)[3] = src + 1;
    return q[0][2];
}
EOF
try_ 6 << EOF
int main(void) {
    short src[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    short (*q)[3] = 1 + src;
    return q[0][2];
}
EOF
try_ 5 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    row_pointer q = p + 1;
    return q[0][1];
}
EOF
try_ 1 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[1];
    row_pointer q = p - 1;
    return q[0][1] == 2 && q == &rows[0];
}
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    return sizeof(*(p - 1)) == 2 * sizeof(int);
}
EOF
try_ 9 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    int value = (*p)[1];
    return value;
}
EOF
try_ 12 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return p[1][1][2];
}
EOF
try_ 15 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    p[1][1][2] += 3;
    return p[1][1][2];
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = ++p[1][1][2];
    return value != 13 || p[1][1][2] != 13;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = p[1][1][2]++;
    return value != 12 || p[1][1][2] != 13;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = --p[1][1][2];
    return value != 11 || p[1][1][2] != 11;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = p[1][1][2]--;
    return value != 12 || p[1][1][2] != 11;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[2][2][3] = {{{1, 2, 3}, {4, 5, 6}},
                            {{7, 8, 9}, {10, 11, 12}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = (p[1][1][2] = 15);
    return value != 15 || p[1][1][2] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = ((*p)[1][2] = 15);
    return value != 15 || (*p)[1][2] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] = 15);
    return value != 15 || (*p)[1][1][1] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int i = 0;
    int j = 0;
    int k = 0;
    int value = ((*p)[i++][j++][k++] = 15);
    return value != 15 || i != 1 || j != 1 || k != 1 || (*p)[0][0][0] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] += 9);
    return value != 17 || (*p)[1][1][1] != 17;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int i = 0;
    int j = 0;
    int k = 0;
    int value = ((*p)[i++][j++][k++] ^= 9);
    return value != 8 || i != 1 || j != 1 || k != 1 || (*p)[0][0][0] != 8;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    int (*p)[2][2][2] = &cubes[0];
    int value = ((*p)[1][1][1] -= 3);
    return value != 5 || (*p)[1][1][1] != 5;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] *= 3);
    return value != 24 || (*p)[1][1][1] != 24;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] /= 2);
    return value != 4 || (*p)[1][1][1] != 4;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] %= 3);
    return value != 2 || (*p)[1][1][1] != 2;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] <<= 1);
    return value != 16 || (*p)[1][1][1] != 16;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] >>= 1);
    return value != 4 || (*p)[1][1][1] != 4;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] &= 6);
    return value != 0 || (*p)[1][1][1] != 0;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ((*p)[1][1][1] |= 2);
    return value != 10 || (*p)[1][1][1] != 10;
}
EOF
try_compile_error << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef const int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return ((*p)[1][1][1] = 15);
}
EOF
try_compile_error << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef const int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return ((*p)[1][1][1] |= 2);
}
EOF
try_compile_error << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int *q = &cubes[0][0][0];
    return ((*p)[1][1][1] += q);
}
EOF
try_compile_error << EOF
struct box { int value; };
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    struct box value = {9};
    return ((*p)[1][1][1] ^= value);
}
EOF
try_compile_error << EOF
struct box { int value; };
int main(void) {
    struct box cubes[1][2][2][2] = {{{{{1}, {2}}, {{3}, {4}}},
                                      {{{5}, {6}}, {{7}, {8}}}}};
    typedef struct box (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return ((*p)[1][1][1] += 1);
}
EOF
try_compile_error << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef const int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    return ((*p)[1][1][1][1] ^= 1);
}
EOF
try_compile_error << EOF
struct box { int value; };
int main(void) {
    struct box hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                                     {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef struct box (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    return ++(*p)[1][1][1][1];
}
EOF
try_compile_error << EOF
struct box { int value; };
int main(void) {
    struct box hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                                     {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef struct box (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    return ((*p)[1][1][1][1] += 1);
}
EOF
try_compile_error << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    return (*p)[1][1][1][1][0]++;
}
EOF
try_compile_error << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    return ++(*p)[1][1][1][1][0];
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int value = ((*p)[1][1][1][1] = 15);
    return value != 15 || (*p)[1][1][1][1] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    int (*p)[2][2][2][2] = &hyper;
    int value = ((*p)[1][1][1][1] <<= 1);
    return value != 32 || (*p)[1][1][1][1] != 32;
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int i = 0, j = 0, k = 0, l = 0;
    int value = ((*p)[i++][j++][k++][l++] |= 8);
    return value != 9 || i != 1 || j != 1 || k != 1 || l != 1 ||
           (*p)[0][0][0][0] != 9;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = (*p)[1][2]++;
    return value != 6 || (*p)[1][2] != 7;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = (*p)[r++][c++]++;
    return value != 1 || r != 1 || c != 1 || (*p)[0][0] != 2;
}
EOF
try_compile_error << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef const int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return (*p)[1][2]++;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = (*p)[1][1][1]++;
    return value != 8 || (*p)[1][1][1] != 9;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = (*p)[1][2]--;
    return value != 6 || (*p)[1][2] != 5;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = (*p)[r++][c++]--;
    return value != 1 || r != 1 || c != 1 || (*p)[0][0] != 0;
}
EOF
try_compile_error << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef const int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return (*p)[1][2]--;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = (*p)[1][1][1]--;
    return value != 8 || (*p)[1][1][1] != 7;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = ++(*p)[1][2];
    return value != 7 || (*p)[1][2] != 7;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = ++(*p)[r++][c++];
    return value != 2 || r != 1 || c != 1 || (*p)[0][0] != 2;
}
EOF
try_compile_error << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef const int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return ++(*p)[1][2];
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = ++(*p)[1][1][1];
    return value != 9 || (*p)[1][1][1] != 9;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int value = --(*p)[1][2];
    return value != 5 || (*p)[1][2] != 5;
}
EOF
try_ 0 << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    int r = 0, c = 0;
    int value = --(*p)[r++][c++];
    return value != 0 || r != 1 || c != 1 || (*p)[0][0] != 0;
}
EOF
try_compile_error << EOF
int main(void) {
    int planes[1][2][3] = {{{1, 2, 3}, {4, 5, 6}}};
    typedef const int (*plane_pointer)[2][3];
    plane_pointer p = &planes[0];
    return --(*p)[1][2];
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int value = --(*p)[1][1][1];
    return value != 7 || (*p)[1][1][1] != 7;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int i = 0, j = 0, k = 0;
    int value = (*p)[i++][j++][k++]++;
    return value != 1 || i != 1 || j != 1 || k != 1 || (*p)[0][0][0] != 2;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    int i = 0, j = 0, k = 0;
    int value = --(*p)[i++][j++][k++];
    return value != 0 || i != 1 || j != 1 || k != 1 || (*p)[0][0][0] != 0;
}
EOF
try_ 0 << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    int (*p)[2][2][2] = &cubes[0];
    int value = ++(*p)[1][1][1];
    return value != 9 || (*p)[1][1][1] != 9;
}
EOF
try_compile_error << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef const int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return (*p)[1][1][1]++;
}
EOF
try_compile_error << EOF
int main(void) {
    int cubes[1][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    typedef const int (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return --(*p)[1][1][1];
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int value = (*p)[1][1][1][1]++;
    return value != 16 || (*p)[1][1][1][1] != 17;
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int value = ++(*p)[1][1][1][1];
    return value != 17 || (*p)[1][1][1][1] != 17;
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int value = (*p)[1][1][1][1]--;
    return value != 16 || (*p)[1][1][1][1] != 15;
}
EOF
try_ 0 << EOF
int main(void) {
    int hyper[2][2][2][2] = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                               {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}};
    typedef int (*hyper_pointer)[2][2][2][2];
    hyper_pointer p = &hyper;
    int value = --(*p)[1][1][1][1];
    return value != 15 || (*p)[1][1][1][1] != 15;
}
EOF
try_compile_error << EOF
struct box { int value; };
int main(void) {
    struct box cubes[1][2][2][2] = {{{{{1}, {2}}, {{3}, {4}}},
                                      {{{5}, {6}}, {{7}, {8}}}}};
    typedef struct box (*cube_pointer)[2][2][2];
    cube_pointer p = &cubes[0];
    return ++(*p)[1][1][1];
}
EOF
try_ 0 << EOF
int main(void) {
    int row[3] = {1, 2, 3};
    typedef int (*row_pointer)[3];
    row_pointer p = &row;
    int i = 0;
    int value = ((*p)[i++] += 4);
    return value != 5 || i != 1 || (*p)[0] != 5;
}
EOF
try_ 9 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    int (*q)[2] = p - 1;
    return q[0][1];
}
EOF
try_ 6 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    p += 1;
    return p[0][2];
}
EOF
try_ 0 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[1];
    row_pointer q = &rows[0];
    return (p - q) + (q - p);
}
EOF
try_ 1 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    return (p + 1) - p;
}
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    int (*q)[2] = &rows[0];
    return p - q;
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    int *q = rows[0];
    return p - q;
}
EOF
try_compile_error << EOF
int main(void) {
    int rows2[1][2] = { { 4, 9 } };
    int rows3[1][3] = { { 1, 2, 3 } };
    int (*p)[2] = rows2;
    int (*q)[3] = rows3;
    return p - q;
}
EOF
try_ 11 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    int step = 1;
    p -= step++;
    return step + p[0][1];
}
EOF
try_ 6 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    row_pointer q = ++p;
    return q[0][2];
}
EOF
try_ 9 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    int (*q)[2] = --p;
    return q[0][1];
}
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    return sizeof(*(++p)) == 2 * sizeof(int);
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    (*p)[0] += p;
    return 0;
}
EOF
try_ 8 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    row_pointer old = p++;
    return old[0][1] + p[0][2];
}
EOF
try_ 17 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[1];
    int (*old)[2] = p--;
    return old[0][1] + p[0][1];
}
EOF
try_ 6 << EOF
int main(void) {
    short rows[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    typedef short (*row_pointer)[3];
    row_pointer p = &rows[0];
    p++;
    return p[0][2];
}
EOF
try_compile_error << EOF
int main(void) {
    short rows[1][3] = { { 1, 2, 3 } };
    typedef short (*row_pointer)[3];
    row_pointer const p = &rows[0];
    p += 1;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*const p)[2] = &rows[0];
    ++p;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*const p)[2] = &rows[0];
    p++;
    return 0;
}
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    return p + 1 == &rows[1];
}
EOF
try_ 8 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    int (*q)[2] = p + 1;
    return q[0][1];
}
EOF
try_ 6 << EOF
enum block_enum_tag { block_enum_value = 6 };
int main(void) {
    enum block_enum_tag value = block_enum_value;
    return value;
}
EOF
try_ 13 << EOF
struct separate_tag_namespace { int value; };
typedef int separate_tag_namespace;
int main(void) {
    typedef struct separate_tag_namespace record_alias;
    record_alias value = { 13 };
    return value.value;
}
EOF
try_ 8 << EOF
int main(void) {
    for (typedef int loop_count, *loop_pointer;
         sizeof(loop_count) == sizeof(int); sizeof(loop_pointer)) {
        loop_count value = 8;
        loop_pointer pointer = &value;
        return *pointer;
    }
}
EOF

# A for initializer resolves its type name in block scope, like any other
# declaration: a block typedef names a type there, and a local object hides a
# file-scope typedef.
try_ 7 << EOF
int main(void) {
    typedef int T;
    typedef int *P;
    int values[2] = { 3, 4 };
    int s = 0;
    for (T i = 0; i < 2; i++)
        s += i;
    for (P p = values; p != values + 2; p++)
        s += *p;
    return s - 1;
}
EOF
try_flags 3 --std=c99 << EOF
int T = 9;
int main(void) {
    typedef char T;
    int s = 0;
    for (T i = 0; i < 3; i++)
        s += i;
    return s + sizeof(T) - 1;
}
EOF
try_ 3 << EOF
typedef int T;
int main(void) {
    int T = 2;
    int s = 0;
    for (T = 0; T < 3; T++)
        s += T;
    return s;
}
EOF
try_ 17 << EOF
int main(void) {
    typedef unsigned long count_t, *count_p;
    count_t value = 17;
    count_p pointer = &value;
    return *pointer;
}
EOF
try_ 19 << EOF
int main(void) {
    typedef long unsigned reordered_t;
    typedef unsigned short short_t;
    reordered_t a = 12;
    short_t b = 7;
    return a + b;
}
EOF
try_ 6 << EOF
int main(void) {
    typedef int pair_t[2];
    pair_t pair = { 6, 7 };
    pair[0] = pair[0] + 1;
    return pair[0] + pair[1] - sizeof(pair_t);
}
EOF
try_ 30 << EOF
int main(void) {
    typedef int table_t[2][3];
    table_t table = { { 1, 2, 3 }, { 4, 5, 6 } };
    return table[1][2] + sizeof(table_t);
}
EOF
try_ 30 << EOF
int main(void) {
    typedef int row_t[2];
    typedef row_t matrix_t[3];
    matrix_t matrix = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
    return matrix[2][1] + sizeof(matrix_t);
}
EOF
try_ 120 << EOF
int main(void) {
    typedef int row_t[2];
    typedef row_t plane_t[3];
    typedef plane_t cube_t[2];
    typedef cube_t hyper_t[2];
    typedef hyper_t hyper_alias_t;
    hyper_alias_t values = {0};
    values[1][1][2][1] = 24;
    return sizeof(hyper_alias_t) + values[1][1][2][1];
}
EOF
try_ 120 << EOF
int main(void) {
    int values[2][2][3][2] = {0};
    typedef int hyper_t[2][2][3][2];
    hyper_t *pointer = &values;
    (*pointer)[1][1][2][1] = 24;
    return sizeof(*pointer) + (*pointer)[1][1][2][1];
}
EOF
try_ 120 << EOF
int main(void) {
    int values[2][2][3][2] = {0};
    typedef int (*hyper_pointer_t)[2][2][3][2];
    hyper_pointer_t pointer = &values;
    (*pointer)[1][1][2][1] = 24;
    return sizeof(*pointer) + (*pointer)[1][1][2][1];
}
EOF
try_ 24 << EOF
int main(void) {
    int values[2][2][3][2] = {0};
    int (*pointer)[2][2][3][2] = &values;
    pointer[0][1][1][2][1] = 24;
    return pointer[0][1][1][2][1];
}
EOF
try_ 6 << EOF
int main(void) {
    int values[3][2][3] = {0};
    values[1][1][2] = 6;
    return (values + 1)[0][1][2];
}
EOF
try_ 6 << EOF
int main(void) {
    int values[3][2][3] = {0};
    values[1][1][2] = 6;
    return (1 + values)[0][1][2];
}
EOF
try_ 6 << EOF
int main(void) {
    int values[3][2][3] = {0};
    values[1][1][2] = 6;
    return (values + 2 - 1)[0][1][2];
}
EOF
try_ 24 << EOF
int main(void) {
    int values[3][2][2][2] = {0};
    values[1][1][1][1] = 24;
    return (values + 1)[0][1][1][1];
}
EOF
try_ 8 << EOF
int main(void) {
    typedef int row_t[2];
    typedef row_t row_alias_t;
    return sizeof(row_alias_t);
}
EOF
try_ 24 << EOF
int main(void) {
    for (typedef int row_t[2]; sizeof(row_t) == 8; ) {
        typedef row_t matrix_t[3];
        matrix_t matrix = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
        return sizeof(matrix_t);
    }
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int a[1][1][1][1];
    typedef a b[1];
    return 0;
}
EOF
try_ 9 << EOF
int main(void) {
    for (typedef int row_t[2]; sizeof(row_t) == 8; ) {
        row_t row = { 4, 5 };
        return row[0] + row[1];
    }
}
EOF
try_compile_error << EOF
int main(void) { typedef int unsized_row[]; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*row_pointer)[]; return 0; }
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    typedef row_pointer row_pointer_alias;
    row_pointer p = &rows[0];
    row_pointer_alias q = p;
    return p == q;
}
EOF
try_ 7 << EOF
int main(void) {
    typedef int *pointer_row[2];
    int first = 3, second = 7;
    pointer_row values = { &first, &second };
    return *values[1];
}
EOF
try_ 8 << EOF
int main(void) {
    typedef int *pointer_row[2];
    int first = 3, second = 7;
    pointer_row values = { &first, &second };
    return *values[1] + 1;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef const int *pointer_row[2];
    int value = 3;
    pointer_row values = { &value, &value };
    *values[0] = 4;
    return 0;
}
EOF
try_ "$((2 * PTR_SZ))" << EOF
int main(void) {
    typedef int *pointer_row[2];
    return sizeof(pointer_row);
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int *pointer_row[2]; }
    pointer_row values = { 0, 0 };
    return values[0] != 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int **unsupported[2]; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int *unsupported[]; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int *unsupported[2][2]; return 0; }
EOF

# An element of a pointer to function pointers is a function pointer: it is
# loaded as a whole pointer, compares with a function, and is callable with or
# without a unary *. It was read as an int and could not be called.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int (*fps[2])(int) = {plus1, twice};
int (**fpp)(int) = fps;
struct holder { int (**slots)(int); } h = {fps};
int call_at(int (**cb)(int), int i, int v) { return cb[i](v) + (*cb[i])(v); }
int main(void)
{
    int (*one)(int) = fpp[1];
    if (fpp[1] == 0) return 1;
    if (fpp[1] != twice) return 2;
    if (fpp[0] != plus1) return 3;
    if (fpp[0](1) != 2 || (*fpp[1])(6) != 12 || one(4) != 8) return 4;
    if (h.slots[1](2) != 4 || (*h.slots[0])(2) != 3) return 5;
    return call_at(fpp, 1, 3) != 12;
}
EOF

# A function pointer loaded from an element or member keeps its prototype as a
# value, so a parenthesized selection is still callable, with or without a unary
# *, and compares with a function. These were rejected.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int (*fps[2])(int) = {plus1, twice};
int (**fpp)(int) = fps;
struct holder { int (*fp)(int); } s = {twice};
int main(void)
{
    int (*local[2])(int) = {twice, plus1};
    if ((fps[1])(4) != 8) return 1;
    if ((*(fps[1]))(4) != 8) return 2;
    if (fps[1] != twice) return 3;
    if ((*(fpp[0]))(1) != 2) return 4;
    if ((*(s.fp))(2) != 4 || (s.fp)(3) != 6) return 5;
    if ((local[1])(1) != 2 || (*(local[0]))(1) != 2) return 6;
    if ((*(*fpp))(1) != 2) return 7;
    return 0;
}
EOF

# A parameter declared as an array of function pointers is adjusted to a pointer
# to function pointers (C99 6.7.5.3p7): it can be subscripted, called through,
# measured, assigned and passed on. Unsized, it could not be subscripted, and a
# dereference of it crashed.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int (*fps[2])(int) = {plus1, twice};
int inner(int (**cb)(int), int v) { return cb[1](v); }
int unsized(int (*cb[])(int), int v);
int unsized(int (**cb)(int), int v) { return (*cb[1])(v) + (**cb)(v); }
int sized(int (*cb[2])(int), int v)
{
    int (*first)(int) = cb[0];
    if (sizeof cb != sizeof(void *) || cb[0] != plus1) return 100;
    cb = fps;
    return first(v) + cb[1](v) + inner(cb, v);
}
int main(void)
{
    if (unsized(fps, 5) != 16) return 1;
    return sized(fps, 3) != 16;
}
EOF

# A pointer to function pointers points to objects, so it takes pointer
# arithmetic and relational comparison (C99 6.5.6, 6.5.8), and so does the array
# of function pointers that decays to one, at file and block scope. The array
# was rejected, and the pointer stepped by an int on LP64.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int (*fps[3])(int) = {plus1, twice, plus1};
int (**gend)(int) = fps + 3;
int (**gmid)(int) = fps + 1;
struct box { int (**slots)(int); } gbox = {fps};
int main(void)
{
    int (*local[2])(int) = {twice, plus1};
    int (**p)(int) = fps + 1;
    int (**q)(int) = 1 + fps;
    int (**r)(int) = local + 1;
    int (**e)(int) = fps;
    int n = 0;
    if ((**p)(3) != 6 || (*q)(1) != 2 || gmid != p) return 1;
    if (gend - fps != 3 || p - fps != 1 || fps + 2 - p != 1) return 2;
    p = p + 1;
    p -= 2;
    ++p;
    p++;
    gbox.slots += 2;
    if (p != fps + 2 || gbox.slots != gend - 1) return 3;
    if ((*(fps + 2))(4) != 5 || (*(r - 1))(1) != 2) return 4;
    if (*(fps + 1) != twice || p[-1] != twice || !(p > fps)) return 5;
    while (e != gend)
        n += (*e++)(1);
    return n != 6;
}
EOF

# An array of function pointers converts to a pointer to function pointers, and
# so does the address of one of its elements, in an initializer, an assignment
# and an argument. Both were rejected, and the array was loaded as if it named a
# single function pointer.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int (*fps[2])(int) = {plus1, twice};
int apply(int (**cb)(int), int v) { return (**cb)(v) + (*cb)(v); }
int main(void)
{
    int (*local[2])(int) = {twice, plus1};
    int (**pp)(int) = fps;
    int (**qq)(int) = &fps[1];
    int (**rr)(int);
    rr = local;
    if ((**pp)(1) != 2 || pp[1](3) != 6) return 1;
    if ((**qq)(3) != 6 || qq != &fps[1]) return 2;
    if (apply(fps, 2) != 6 || apply(local, 2) != 8) return 3;
    if (apply(&fps[1], 1) != 4) return 4;
    if ((*rr)(1) != 2 || rr[1](1) != 2) return 5;
    return 0;
}
EOF

# Equality with a function designator reduced before a following operator of
# lower precedence, as in a != twice || b != plus1, compared the raw symbol and
# the pointer object's slot instead of the two addresses.
try_ 0 << EOF
int plus1(int v) { return v + 1; }
int twice(int v) { return v * 2; }
int main(void)
{
    int (*a)(int) = twice, (*b)(int) = plus1;
    if (a != twice || b != plus1) return 1;
    if (!(a == twice && plus1 == b)) return 2;
    if ((a == plus1 | b == twice) != 0) return 3;
    return a == twice ? 0 : 4;
}
EOF

# Unary * on a function pointer selected by a subscript or member, or reached
# through a pointer to function pointers, designates the function (C99
# 6.5.3.2p4), and further asterisks change nothing. These calls were rejected,
# and (*fps)(4) and *s.fp read the code as a pointer.
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int twice(int value) { return value * 2; }
int (*fps[2])(int) = {plus1, twice};
int (**fpp)(int) = fps;
struct holder { int (*fp)(int); } s = {twice};
int main(void) {
    struct holder *sp = &s;
    int (*local[2])(int) = {twice, plus1};
    int (*copy)(int) = *s.fp;
    if ((*fps[1])(4) != 8 || (**fpp)(4) != 5 || (*s.fp)(4) != 8) return 1;
    if ((*fps)(4) != 5 || (**fps)(4) != 5 || (*sp->fp)(1) != 2) return 2;
    if ((*local[1])(1) != 2 || (**local)(3) != 6 || copy(5) != 10) return 3;
    if ((***fpp)(2) != 3) return 4;
    return 0;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2])(int);
    int (*first)(int) = plus1;
    int (*second)(int) = plus1;
    slots_t slots = {&first, &second};
    return (*slots[1])(7);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2])(int);
    typedef slots_t slots_alias_t;
    int (*callback)(int) = plus1;
    slots_alias_t slots = {0, &callback};
    return sizeof(slots_t) != 2 * sizeof(void *) || (*slots[1])(4) != 5;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2])(int);
    int (*callback)(int) = plus1;
    slots_t slots = {&callback, &callback};
    return slots[0](4);
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**slots_t[2])(int);
    void (*callback)(int) = set_target;
    slots_t slots = {&callback, &callback};
    (*slots[1])(7);
    return target;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2])(int);
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_t slots = {&first, 0};
    slots[0] = &second;
    if ((*slots[0])(5) != 7)
        return 1;
    slots[0] = 0;
    return slots[0] != 0;
}
EOF
try_ 9 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**slots_t[2][2])(int);
    void (*callback)(int) = set_target;
    slots_t slots = {{0, 0}, {&callback, 0}};
    (*slots[1][0])(9);
    return target;
}
EOF
try_ 9 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**slots_t[2][2][2])(int);
    void (*callback)(int) = set_target;
    slots_t slots = {{{0, 0}, {0, 0}}, {{0, &callback}, {0, 0}}};
    (*slots[1][0][1])(9);
    return target;
}
EOF
try_ 9 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**slots_t[2][2][2][2])(int);
    void (*callback)(int) = set_target;
    slots_t slots = {0};
    slots[1][0][1][1] = &callback;
    (*slots[1][0][1][1])(9);
    return target;
}
EOF
try_compile_error << EOF
long incompatible(long value) { return value; }
int main(void) {
    typedef int (**slots_t[2])(int);
    long (*wrong)(long) = incompatible;
    slots_t slots = {0, 0};
    slots[0] = &wrong;
    return 0;
}
EOF
try_compile_error << EOF
long incompatible(long value) { return value; }
int main(void) {
    typedef int (**slots_t[2])(int);
    long (*callback)(long) = incompatible;
    slots_t slots = {&callback, &callback};
    return 0;
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**const slots_t[2])(int);
    int (*first)(int) = plus1;
    slots_t slots = {&first, 0};
    *slots[0] = plus2;
    return (*slots[0])(5);
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**const slots_t[2])(int);
    int (*first)(int) = plus1;
    slots_t slots = {&first, 0};
    slots[0] = &first;
    return 0;
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**volatile slots_t[2])(int);
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_t slots = {&first, 0};
    slots[0] = &second;
    return (*slots[0])(5);
}
EOF
try_ 6 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**restrict slots_t[2])(int);
    int (*first)(int) = plus1;
    slots_t slots = {&first, 0};
    return (*slots[0])(5);
}
EOF

try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**const row_t[2])(int);
    typedef row_t grid_t[2];
    int (*callback)(int) = plus1;
    grid_t slots = {{0, 0}, {0, &callback}};
    slots[1][1] = &callback;
    return 0;
}
EOF

try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**volatile row_t[2])(int);
    typedef row_t grid_t[2];
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    grid_t slots = {{&first, 0}, {0, 0}};
    slots[1][1] = &second;
    return (*slots[1][1])(5);
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2])(int);
    typedef slots_t const cslots_t;
    int (*callback)(int) = plus1;
    cslots_t slots = {&callback, 0};
    *slots[0] = plus2;
    return (*slots[0])(5);
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2])(int);
    typedef slots_t const cslots_t;
    int (*callback)(int) = plus1;
    cslots_t slots = {&callback, 0};
    slots[0] = &callback;
    return 0;
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2])(int);
    typedef slots_t volatile vslots_t;
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    vslots_t slots = {&first, 0};
    slots[0] = &second;
    return (*slots[0])(5);
}
EOF
try_ 6 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2])(int);
    typedef slots_t restrict rslots_t;
    int (*callback)(int) = plus1;
    rslots_t slots = {&callback, 0};
    return (*slots[0])(5);
}
EOF
try_ 0 << EOF
int main(void) {
    typedef int *values_t[2];
    typedef values_t restrict restricted_values_t;
    int value = 7;
    restricted_values_t values = {&value, 0};
    return *values[0] != 7;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int values_t[2];
    typedef values_t restrict restricted_values_t;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int (*callbacks_t[2])(int);
    typedef callbacks_t restrict restricted_callbacks_t;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (**slots_t[2])(int); }
    slots_t slots = {0, 0};
    return 0;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2][2])(int);
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_t slots = {{&first, &first}, {&second, &first}};
    return (*slots[1][0])(6);
}
EOF
try_ 9 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**row_t[2])(int);
    typedef row_t grid_t[2];
    void (*callback)(int) = set_target;
    grid_t slots = {{0, 0}, {0, &callback}};
    (*slots[1][1])(9);
    return target;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2][2])(int);
    typedef slots_t slots_alias_t;
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_alias_t slots = {{&first, 0}, {0, &second}};
    slots[0][0] = &second;
    return sizeof(slots_t) != 4 * sizeof(void *) || (*slots[0][0])(5) != 7;
}
EOF
try_compile_error << EOF
long incompatible(long value) { return value; }
int main(void) {
    typedef int (**slots_t[2][2])(int);
    long (*wrong)(long) = incompatible;
    slots_t slots = {{0, 0}, {0, 0}};
    slots[1][0] = &wrong;
    return 0;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2][2])(int);
    int (*callback)(int) = plus1;
    slots_t slots = {{&callback, &callback}, {&callback, &callback}};
    return slots[0][0](4);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2][2][2])(int);
    typedef slots_t slots_alias_t;
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_alias_t slots = {{{&first, 0}, {0, 0}}, {{0, 0}, {0, &second}}};
    slots[0][0][0] = &second;
    return sizeof(slots_t) != 8 * sizeof(void *) || (*slots[0][0][0])(5) != 7;
}
EOF
try_compile_error << EOF
long incompatible(long value) { return value; }
int main(void) {
    typedef int (**slots_t[2][2][2])(int);
    long (*wrong)(long) = incompatible;
    slots_t slots = {{{0, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
    slots[1][0][1] = &wrong;
    return 0;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2][2][2])(int);
    int (*callback)(int) = plus1;
    slots_t slots = {{{&callback, 0}, {0, 0}}, {{0, 0}, {0, 0}}};
    return slots[0][0][0](4);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**slots_t[2][2][2][2])(int);
    typedef slots_t slots_alias_t;
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    slots_alias_t slots = {
        {{{&first, 0}, {0, 0}}, {{0, 0}, {0, 0}}},
        {{{0, 0}, {0, 0}}, {{0, 0}, {0, &second}}}
    };
    slots[0][0][0][0] = &second;
    return sizeof(slots_t) != 16 * sizeof(void *) ||
           (*slots[0][0][0][0])(5) != 7;
}
EOF
try_compile_error << EOF
long incompatible(long value) { return value; }
int main(void) {
    typedef int (**slots_t[2][2][2][2])(int);
    long (*wrong)(long) = incompatible;
    slots_t slots = {0};
    slots[1][0][1][0] = &wrong;
    return 0;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slots_t[2][2][2][2])(int);
    int (*callback)(int) = plus1;
    slots_t slots = {0};
    slots[0][0][0][0] = &callback;
    return slots[0][0][0][0](4);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**row_t[2])(int);
    typedef row_t grid_t[2];
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    grid_t slots = {{&first, 0}, {0, &second}};
    return sizeof(grid_t) != 4 * sizeof(void *) || (*slots[1][1])(6) != 8;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (**row_t[2])(int);
    typedef row_t cube_t[2][2];
    int (*first)(int) = plus1;
    int (*second)(int) = plus2;
    cube_t slots = {{{&first, 0}, {0, 0}}, {{0, 0}, {0, &second}}};
    slots[0][0][0] = &second;
    return sizeof(cube_t) != 8 * sizeof(void *) || (*slots[0][0][0])(5) != 7;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (**slots_t[2][2][2][2][2])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) {
    typedef int (**slots_t[2][2][2][2])(int);
    typedef slots_t wrapped_t[2];
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (**slots_t[])(int); return 0; }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int unary_t(int);
    unary_t *callback = plus1;
    return callback(7);
}
EOF

# A file-scope function typedef names the same function type: pointers to it,
# parameters, and a declarator that declares a function. A definition cannot
# take its type from the typedef.
try_ 0 << EOF
typedef int unary_t(int), count_t;
typedef void setter_t(int *);
int plus1(int value) { return value + 1; }
void set3(int *p) { *p = 3; }
unary_t plus2, *global_callback = plus1;
static unary_t times2;
count_t three = 3;
int apply(unary_t *callback, int value) { return callback(value); }
int main(void) {
    unary_t *callback = plus1;
    setter_t *setter = set3;
    int x = 0;
    setter(&x);
    return callback(7) != 8 || global_callback(1) != 2 || plus2(1) != 3 ||
           times2(4) != 8 || apply(plus1, 2) != 3 || x != three ||
           sizeof(unary_t *) != sizeof(void *);
}
int plus2(int value) { return value + 2; }
static int times2(int value) { return value * 2; }
EOF

# A record's first member survives a spill of the register that held the
# record's allocation address, here forced by an indirect call.
try_ 0 << EOF
typedef int thunk_t(void);
int five(void) { return 5; }
int (*global_thunk)(void) = five;
struct holder { thunk_t *callback; };
int main(void) {
    typedef int local_thunk_t(void);
    struct local_holder { local_thunk_t *callback; } local;
    struct holder h;
    h.callback = five;
    local.callback = five;
    return global_thunk() + h.callback() + local.callback() != 15;
}
EOF

# Function typedefs may be variadic or return a pointer, at both scopes.
try_ 0 << EOF
typedef int count_t(int, ...);
typedef char *name_t(void);
int count(int n, ...) { return n; }
char *name(void) { return "abc"; }
count_t *global_count = count;
name_t *global_name = name;
count_t count2;
name_t name2;
int call_count(count_t *f) { return f(2, 5, 6); }
int main(void) {
    typedef int local_count_t(int, ...);
    typedef char *local_name_t(void);
    local_count_t *lc = count;
    local_name_t *ln = name2;
    name_t *nn = name;
    return lc(3, 1, 2, 3) != 3 || ln()[0] != 'x' || global_count(1, 4) != 1 ||
           global_name()[2] != 'c' || count2(7) != 7 || name2()[1] != 'y' ||
           call_count(count) != 2 || nn()[1] != 'b' ||
           sizeof(name_t *) != sizeof(void *);
}
int count2(int n, ...) { return n; }
char *name2(void) { return "xyz"; }
EOF

# Arrays of pointers to a function typedef, and pointers to one such pointer, at
# both scopes.
try_ 0 << EOF
typedef int thunk_t(void);
typedef int unary_t(int);
typedef void setter_t(int *);
int one(void) { return 1; }
int two(void) { return 2; }
int twice(int x) { return 2 * x; }
int inc(int x) { return x + 1; }
void set4(int *p) { *p = 4; }
thunk_t *global_table[2] = {one, two};
unary_t *global_ops[] = {twice, inc};
thunk_t *global_one = one;
thunk_t **global_slot = &global_one;
int call_slot(thunk_t **slot) { return (*slot)(); }
int main(void) {
    typedef int local_t(void);
    local_t *local_table[2] = {two, one};
    unary_t *ops[2];
    setter_t *setters[1] = {set4};
    thunk_t *first = one;
    thunk_t **slot = &first;
    int x = 0;
    ops[0] = inc;
    ops[1] = twice;
    setters[0](&x);
    return global_table[0]() + global_table[1]() + local_table[0]() +
           global_ops[0](3) + global_ops[1](3) + ops[0](1) + ops[1](5) + x +
           (*slot)() + (*global_slot)() + call_slot(&first) != 34 ||
           sizeof(local_table) != 2 * sizeof(local_t *) ||
           sizeof(global_ops) != 2 * sizeof(unary_t *);
}
EOF
try_compile_error << EOF
typedef int unary_t(int);
unary_t plus1 { return 1; }
int main(void) { return plus1(0); }
EOF
try_compile_error << EOF
typedef int unary_t(int);
unary_t plus1;
int plus1(void) { return 1; }
int main(void) { return plus1(); }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*callback_t)(int);
    typedef callback_t const const_callback_t;
    const_callback_t callback = plus1;
    return callback(7);
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*volatile callback_t)(int);
    callback_t callback = plus1;
    return callback(7);
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (*setter_t)(int);
    setter_t setter = set_target;
    setter(7);
    return target;
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void setter_t(int);
    setter_t *callback = set_target;
    callback(7);
    return target;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*const callback_t)(int);
    callback_t callback = plus1;
    return callback(7);
}
EOF
try_ 8 << EOF
int main(void) {
    typedef int unary_t(int);
    unary_t plus1;
    return plus1(7);
}
int plus1(int value) { return value + 1; }
EOF
try_ 9 << EOF
int main(void) {
    typedef int unary_t(int);
    unary_t plus1, plus2;
    return plus1(3) + plus2(3);
}
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
EOF
try_ 9 << EOF
int plus1(int);
int main(void) {
    typedef int unary_t(int);
    extern unary_t plus1, plus2;
    return plus1(3) + plus2(3);
}
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
EOF
try_compile_error << EOF
long plus2(long);
int main(void) {
    typedef int unary_t(int);
    unary_t plus1, plus2;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int unary_t(int);
    unary_t declared_function, *slot = plus1;
    return slot(4) != 5;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int unary_t(int);
    unary_t plus1, plus2 = 0;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t plus1, plus2; }
    return plus1(1) + plus2(1);
}
int plus1(int value) { return value; }
int plus2(int value) { return value; }
EOF
try_ 2 << EOF
int plus1(int);
int plus2(int);
int main(void) {
    { typedef int unary_t(int); unary_t plus1, plus2; }
    return plus1(1) + plus2(1);
}
int plus1(int value) { return value; }
int plus2(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static int (*callback)(int) = hidden;
    return callback(1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { extern int hidden(int); }
    return hidden(1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static int (*callback)(int) = &hidden;
    return callback(1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static int (*callback)(int) = (hidden);
    return callback(1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static int (*callbacks[1])(int) = { &*hidden };
    return callbacks[0](1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static int (*callbacks[1])(int) = { (*&hidden) };
    return callbacks[0](1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
struct holder { int (*callback)(int); };
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static struct holder saved = { &*hidden };
    return saved.callback(1);
}
int hidden(int value) { return value; }
EOF
try_compile_error << EOF
struct holder { int (*callback)(int); };
int main(void) {
    { typedef int unary_t(int); unary_t hidden; }
    static struct holder saved = { (*&hidden) };
    return saved.callback(1);
}
int hidden(int value) { return value; }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int unary_t(int);
    extern unary_t plus1;
    return plus1(7);
}
EOF
try_compile_error << EOF
long plus1(long);
int main(void) {
    typedef int unary_t(int);
    unary_t plus1;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int unary_t(int);
    unary_t plus1 = 0;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); static unary_t plus1; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); auto unary_t plus1; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); register unary_t plus1; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); const unary_t plus1; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); volatile unary_t plus1; return 0; }
EOF
try_compile_error << EOF
int main(void) {
    { typedef int unary_t(int); }
    unary_t *callback = 0;
    return callback != 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int) = 0; return 0; }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*callback_t)(int);
    callback_t callback = plus1;
    return callback(7);
}
EOF
try_ 9 << EOF
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callback_t)(int);
    typedef callback_t callback_alias_t;
    callback_alias_t callback = plus2;
    return callback(7);
}
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callback_t)(int);
    callback_t callback = plus1;
    callback = plus2;
    return callback(7);
}
EOF
try_ "$PTR_SZ" << EOF
int main(void) {
    typedef int (*callback_t)(int);
    return sizeof(callback_t);
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int apply(int (*callback)(int), int value) { return callback(value); }
int main(void) {
    typedef int (*callback_t)(int);
    extern int apply(callback_t, int);
    return apply(plus1, 7);
}
EOF
try_ "$PTR_SZ" << EOF
int main(void) {
    typedef int (*callback_t)(int);
    extern callback_t choose(void);
    return sizeof(callback_t);
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int (*const callback_t)(int);
    callback_t callback = 0;
    callback = 0;
    return 0;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*callback_t)(int);
    typedef callback_t const const_callback_t;
    const_callback_t callback = plus1;
    callback = plus1;
    return callback(7);
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (*callback_t)(int); }
    callback_t callback = 0;
    return callback != 0;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    return (*slot)(7);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t slot_alias_t;
    int (*callback)(int) = plus1;
    slot_alias_t slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_ 0 << EOF
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t slot_alias_t;
    return sizeof(slot_t) != sizeof(void *) ||
           sizeof(slot_alias_t) != sizeof(void *);
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (**slot_t)(int);
    void (*callback)(int) = set_target;
    slot_t slot = &callback;
    (*slot)(7);
    return target;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    return slot(7);
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (**slot_t)(int); }
    slot_t slot = 0;
    return slot != 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**const slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**const slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    slot = &callback;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**volatile slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**const slot)(int) = &callback;
    slot = &callback;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**restrict slot)(int) = &callback;
    slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**restrict slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t restrict rslot_t;
    int (*callback)(int) = plus1;
    rslot_t slot = &callback;
    return (*slot)(4) != 5 || sizeof(rslot_t) != sizeof(void *);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**const restrict slot_t)(int);
    int (*callback)(int) = plus1;
    slot_t slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (*restrict *slot_t)(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*restrict callback_t)(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*const *slot_t)(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*volatile *slot_t)(int); return 0; }
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t const const_slot_t;
    int (*callback)(int) = plus1;
    const_slot_t slot = &callback;
    return (*slot)(4) != 5 || sizeof(const_slot_t) != sizeof(void *);
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t const const_slot_t;
    int (*callback)(int) = plus1;
    const_slot_t slot = &callback;
    slot = &callback;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t volatile vslot_t;
    int (*callback)(int) = plus1;
    vslot_t slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int (**slot_t)(int);
    typedef slot_t *deeper_t;
    return 0;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**slot)(int) = &callback;
    return (*slot)(7);
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    void (*callback)(int) = set_target;
    void (**slot)(int) = &callback;
    (*slot)(7);
    return target;
}
EOF
try_compile_error << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**slot)(int) = &callback;
    return slot(7);
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**other)(int) = &callback;
    int (**slot)(int) = other;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
long callback(long value) { return value; }
int main(void) {
    long (*other_callback)(long) = callback;
    long (**other)(long) = &other_callback;
    int (**slot)(int) = other;
    return 0;
}
EOF
try_ 0 << EOF
int main(void) {
    int (**slot)(int) = 0;
    slot = 0;
    return slot != 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int (*callback)(int) = plus1;
int (**slot)(int) = &callback;
int main(void) { return (*slot)(4) != 5; }
EOF
try_compile_error << EOF
long callback(long value) { return value; }
long (*other)(long) = callback;
int (**slot)(int) = &other;
int main(void) { return 0; }
EOF
try_ 0 << EOF
int (**slot)(int) = 0;
int main(void) { return slot != 0; }
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**a)(int) = &callback;
    int (**b)(int) = &callback;
    int flag = 0;
    int (**slot)(int) = flag ? a : b;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int main(void) {
    int (**a)(int) = 0;
    int (**b)(long) = 0;
    int flag = 0;
    int (**slot)(int) = flag ? a : b;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int (*callback)(int) = plus1;
    int (**slot)(int) = &callback;
    int (**other)(int) = slot;
    slot = other;
    return (*slot)(4) != 5;
}
EOF
try_compile_error << EOF
int main(void) {
    int (**slot)(int);
    int (**other)(long);
    slot = other;
    return 0;
}
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int result;
void use(int (**slot)(int));
void use(int (**slot)(int)) { result = (*slot)(4); }
int main(void) { int (*callback)(int) = plus1; use(&callback); return result != 5; }
EOF
try_compile_error << EOF
void use(int (**slot)(int)) {}
int main(void) {
    int (**slot)(long) = 0;
    use(slot);
    return 0;
}
EOF
try_compile_error << EOF
void use(int (**slot)(int));
void use(int (**slot)(long));
int main(void) { return 0; }
EOF
try_compile_error << EOF
extern int (**slot)(int);
extern int (**slot)(long);
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*restrict callback_t)(int); return 0; }
EOF
try_compile_error << EOF
void use(int (*callback)(int));
void use(int (**callback)(int));
int main(void) { return 0; }
EOF
try_compile_error << EOF
typedef int (*callback_t)(int);
typedef int (*const const_callback_t)(int);
callback_t choose(void);
const_callback_t choose(void);
int main(void) { return 0; }
EOF
try_compile_error << EOF
typedef int (*callback_t)(int);
typedef int (*volatile volatile_callback_t)(int);
callback_t choose(void);
volatile_callback_t choose(void);
int main(void) { return 0; }
EOF
try_ 8 << EOF
typedef int (*global_callback_t)(int);
int plus1(int value) { return value + 1; }
global_callback_t choose(void) { return plus1; }
int main(void) {
    typedef int (*callback_t)(int);
    extern callback_t choose(void);
    return choose()(7);
}
EOF
try_compile_error << EOF
int main(void) { typedef int unary_t(int); return sizeof(unary_t); }
EOF
try_compile_error << EOF
int main(void) {
    typedef int unary_t(int);
    switch (0) { case sizeof(unary_t): return 1; }
    return 0;
}
EOF
try_ "$PTR_SZ" << EOF
int main(void) { typedef int unary_t(int); return sizeof(unary_t *); }
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2])(int);
    callbacks_t callbacks = {plus1, plus2};
    return callbacks[0](3) + callbacks[1](3);
}
EOF
try_ "$((2 * PTR_SZ))" << EOF
int main(void) {
    typedef int (*callbacks_t[2])(int);
    return sizeof(callbacks_t);
}
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2])(int);
    callbacks_t callbacks = {{plus1, plus2}, {plus2, plus1}};
    return callbacks[1][0](3) + callbacks[1][1](3);
}
EOF
try_ "$((4 * PTR_SZ))" << EOF
int main(void) {
    typedef int (*callbacks_t[2][2])(int);
    return sizeof(callbacks_t);
}
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2][2])(int);
    callbacks_t callbacks = {
        {{plus1, plus2}, {plus2, plus1}},
        {{plus2, plus1}, {plus1, plus2}}
    };
    return callbacks[1][1][0](3) + callbacks[1][0][0](3);
}
EOF
try_ "$((8 * PTR_SZ))" << EOF
int main(void) {
    typedef int (*callbacks_t[2][2][2])(int);
    return sizeof(callbacks_t);
}
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2][2][2])(int);
    callbacks_t callbacks = {
        {{{plus1, plus2}, {plus2, plus1}}, {{plus2, plus1}, {plus1, plus2}}},
        {{{plus2, plus1}, {plus1, plus2}}, {{plus1, plus2}, {plus2, plus1}}}
    };
    return callbacks[1][1][1][1](3) + callbacks[1][0][0][0](3);
}
EOF
try_ "$((16 * PTR_SZ))" << EOF
int main(void) {
    typedef int (*callbacks_t[2][2][2][2])(int);
    return sizeof(callbacks_t);
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2][2][2])(int);
    typedef callbacks_t callbacks_alias_t;
    callbacks_alias_t callbacks = {
        {{{plus1, plus2}, {plus2, plus1}}, {{plus2, plus1}, {plus1, plus2}}},
        {{{plus2, plus1}, {plus1, plus2}}, {{plus1, plus2}, {plus2, plus1}}}
    };
    return callbacks[0][1][0][1](6) + (sizeof(callbacks_alias_t) != 16 * sizeof(void *));
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (*setters_t[1][1][1][1])(int);
    setters_t setters = {{{{set_target}}}};
    setters[0][0][0][0](7);
    return target;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    for (typedef int (*callbacks_t[1][1][1][1])(int); ; ) {
        callbacks_t callbacks = {{{{plus1}}}};
        return callbacks[0][0][0][0](7);
    }
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (*callbacks_t[1][1][1][1])(int); }
    callbacks_t callbacks = {{{{0}}}};
    return callbacks[0][0][0][0] != 0;
}
EOF
try_ 7 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2][2])(int);
    typedef callbacks_t callbacks_alias_t;
    callbacks_alias_t callbacks = {
        {{plus1, plus2}, {plus2, plus1}},
        {{plus2, plus1}, {plus1, plus2}}
    };
    return callbacks[1][0][1](6) + (sizeof(callbacks_alias_t) != 8 * sizeof(void *));
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (*setters_t[1][1][1])(int);
    setters_t setters = {{{set_target}}};
    setters[0][0][0](7);
    return target;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    for (typedef int (*callbacks_t[1][1][1])(int); ; ) {
        callbacks_t callbacks = {{{plus1}}};
        return callbacks[0][0][0](7);
    }
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (*callbacks_t[1][1][1])(int); }
    callbacks_t callbacks = {{{0}}};
    return callbacks[0][0][0] != 0;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2][2])(int);
    typedef callbacks_t callbacks_alias_t;
    callbacks_alias_t callbacks = {{plus1, plus2}, {plus2, plus1}};
    return callbacks[0][1](6);
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (*setters_t[1][1])(int);
    setters_t setters = {{set_target}};
    setters[0][0](7);
    return target;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    for (typedef int (*callbacks_t[1][1])(int); ; ) {
        callbacks_t callbacks = {{plus1}};
        return callbacks[0][0](7);
    }
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (*callbacks_t[1][1])(int); }
    callbacks_t callbacks = {{0}};
    return callbacks[0][0] != 0;
}
EOF
try_ 7 << EOF
int target;
void set_target(int value) { target = value; }
int main(void) {
    typedef void (*setters_t[1])(int);
    setters_t setters = {set_target};
    setters[0](7);
    return target;
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int (*callbacks_t[2])(int);
    typedef callbacks_t callbacks_alias_t;
    callbacks_alias_t callbacks = {plus1, plus2};
    return callbacks[0](7);
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    for (typedef int (*callbacks_t[1])(int); ; ) {
        callbacks_t callbacks = {plus1};
        return callbacks[0](7);
    }
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int (*callbacks_t[1])(int); }
    callbacks_t callbacks = {0};
    return callbacks[0] != 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (*callbacks_t[])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*callbacks_t[2][2][2][2][2])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) {
    typedef int (*callbacks_t[2][2][2][2])(int);
    typedef callbacks_t wrapped_t[2];
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (*callbacks_t[2])(int) = 0; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*callbacks_t[2])(int, ...); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*const callbacks_t[2])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*volatile callbacks_t[2])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int (*restrict callbacks_t[2])(int); return 0; }
EOF
try_compile_error << EOF
int main(void) {
    typedef int (*callback_t)(int);
    typedef callback_t const callbacks_t[2];
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef float (*callbacks_t[2])(int); return 0; }
EOF
try_compile_error << EOF
struct pair { int value; };
int main(void) { typedef struct pair (*callbacks_t[2])(int); return 0; }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int *int_ptr, unary_t(int);
    int value = 7;
    int_ptr pointer = &value;
    unary_t *callback = plus1;
    return callback(*pointer);
}
EOF
try_ 9 << EOF
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    typedef int first_t(int), second_t(int);
    first_t *first = plus1;
    second_t *second = plus2;
    return first(3) + second(3);
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int (*callback_t)(int), unary_t(int);
    callback_t callback = plus1;
    unary_t *alias_callback = callback;
    return alias_callback(7);
}
EOF
try_ 5 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int row_t[2], unary_t(int);
    row_t row = {3, 4};
    unary_t *callback = plus1;
    return callback(row[1]);
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    typedef int *const pointer_t, unary_t(int);
    unary_t *callback = plus1;
    return callback(7);
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int *const pointer_t, unary_t(int);
    int value = 0;
    pointer_t pointer = &value;
    pointer = &value;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int scalar_t, unary_t(int) = 0;
    return 0;
}
EOF
try_ 3 << EOF
int count(int n, ...) { return n; }
int main(void) {
    typedef int scalar_t, variadic_t(int, ...);
    variadic_t *callback = count;
    scalar_t value = callback(3, 1, 2);
    return value;
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int scalar_t, unary_t(int); }
    unary_t *callback = 0;
    return callback != 0;
}
EOF
try_ 8 << EOF
int plus1(int);
int main(void) {
    typedef int unary_t(int);
    unary_t plus1;
    return plus1(7);
}
int plus1(int value) { return value + 1; }
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    int plus1 = 0;
    {
        typedef int unary_t(int);
        unary_t plus1;
        return plus1(7);
    }
}
EOF
try_ 8 << EOF
int plus1(int value) { return value + 1; }
int main(void) {
    for (typedef int unary_t(int); ; ) {
        unary_t *callback = plus1;
        return callback(7);
    }
}
EOF
try_ "$((2 * PTR_SZ))" << EOF
int main(void) {
    typedef int *(*rows_t)[2];
    int first = 3, second = 7;
    int *data[1][2] = { { &first, &second } };
    rows_t p = data;
    return sizeof(*p);
}
EOF
try_ 7 << EOF
int main(void) {
    for (typedef int *(*rows_t)[2]; sizeof(rows_t) == sizeof(int *); ) {
        int first = 3, second = 7;
        int *data[1][2] = { { &first, &second } };
        rows_t p = data;
        return *p[0][1];
    }
}
EOF
try_compile_error << EOF
int main(void) {
    { typedef int *(*rows_t)[2]; }
    rows_t p = 0;
    return p != 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int **(*bad)[2]; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef int *(*bad)[]; return 0; }
EOF
try_ "$((8 + PTR_SZ))" << EOF
int main(void) {
    int rows[2][2] = { { 1, 2 }, { 3, 4 } };
    for (typedef int (*row_pointer)[2]; sizeof(row_pointer) == sizeof(int *); ) {
        row_pointer p = &rows[0];
        return sizeof(*p) + sizeof(row_pointer);
    }
}
EOF
try_ 10 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    return (*p)[1] + (sizeof(*p) == 2 * sizeof(int));
}
EOF
try_ 9 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    return (*(p))[1];
}
EOF
try_ 7 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    (*p)[1] = 7;
    return rows[0][1];
}
EOF
try_ 7 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    (*p)[1] = 7;
    return rows[0][1];
}
EOF
try_ 6 << EOF
int main(void) {
    short rows[1][3] = { { 1, 2, 3 } };
    typedef short (*row_pointer)[3];
    row_pointer p = rows;
    (*p)[1] += 4;
    return rows[0][1];
}
EOF
try_ 5 << EOF
int main(void) { short rows[1][3]={{1,2,3}}; typedef short (*R)[3]; R p=rows; short old=(*p)[1]++; return old+rows[0][1]; }
EOF
try_ 13 << EOF
int main(void) { int rows[1][2]={{4,9}}; int (*p)[2]=rows; int old=(*p)[1]--; return old+rows[0][1]-4; }
EOF
try_ 9 << EOF
int main(void) { int rows[1][2]={{4,9}}; int (*p)[2]=rows; int i=0; (*p)[i++]++; return i+rows[0][0]+3; }
EOF
try_ 10 << EOF
int main(void) { int rows[1][2]={{4,9}}; int (*p)[2]=rows; int choice=1; (*p)[choice ? 1 : 0]++; return rows[0][1]; }
EOF
try_compile_error << EOF
int main(void) { int rows[1][2]={{4,9}}; typedef const int (*R)[2]; R p=rows; (*p)[1]++; return 0; }
EOF
try_ 6 << EOF
int main(void) {
    short rows[1][3] = { { 1, 2, 3 } };
    typedef short (*row_pointer)[3];
    row_pointer p = rows;
    short value = ++(*p)[1];
    return value + rows[0][1];
}
EOF
try_ 16 << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    int value = --(*p)[1];
    return value + rows[0][1];
}
EOF
try_ 11 << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    int index = 0;
    int value = ++(*p)[index++];
    return value + index + rows[0][0];
}
EOF
try_ 2 << EOF
int main(void) {
    _Bool rows[1][1] = { { 1 } };
    _Bool (*p)[1] = rows;
    _Bool value = ++(*p)[0];
    return value + rows[0][0];
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    typedef const int (*row_pointer)[2];
    row_pointer p = rows;
    return ++(*p)[1];
}
EOF
try_ 2 << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    (*p)[0] -= 2;
    return rows[0][0];
}
EOF
try_ 1 << EOF
int main(void) {
    _Bool rows[1][1] = { { 1 } };
    _Bool (*p)[1] = rows;
    (*p)[0] += 1;
    return rows[0][0];
}
EOF

# C99 defines ++ and -- as += 1 and -= 1, so on _Bool the result converts back
# to 0 or 1: b++ on 1 leaves 1, --b on 0 leaves 1 and b-- on 1 leaves 0. The
# same holds for members, elements, pointers and compound assignment.
try_output 0 "1 1 1 1 0 | 1 1 1 1 0 1 1 1 1 0 | 1 1 1 | 1 1 1 0 1 | 1 1 1 1 1 1 | 1 1 1 1 1 1 1 1 1 1" << EOF
typedef _Bool flag;
struct holder {
    int pad;
    _Bool plain;
    flag alias;
};
_Bool global_flag;
int main(void)
{
    _Bool b, values[3] = {0, 0, 0}, *p = values;
    flag alias;
    struct holder h, *hp = &h;
    int v;
    b = 1;
    b++;
    printf("%d ", b);
    b = 1;
    ++b;
    printf("%d ", b);
    b = 0;
    b--;
    printf("%d ", b);
    b = 0;
    --b;
    printf("%d ", b);
    b = 1;
    --b;
    printf("%d | ", b);
    b = 1;
    v = b++;
    printf("%d %d ", v, b);
    b = 1;
    v = ++b;
    printf("%d %d ", v, b);
    b = 0;
    v = b--;
    printf("%d %d ", v, b);
    b = 0;
    v = --b;
    printf("%d %d ", v, b);
    b = 1;
    v = b--;
    printf("%d %d | ", v, b);
    alias = 1;
    alias++;
    global_flag = 0;
    v = --global_flag;
    printf("%d %d %d | ", alias, v, global_flag);
    h.plain = 1;
    h.plain++;
    hp->alias = 0;
    v = --hp->alias;
    printf("%d %d %d ", h.plain, v, h.alias);
    h.plain = 0;
    v = h.plain--;
    printf("%d %d | ", v, h.plain);
    values[1] = 1;
    values[1]++;
    values[2] = 0;
    v = --values[2];
    printf("%d %d %d ", values[1], v, values[2]);
    p[0] = 0;
    p[0]--;
    p[1] = 1;
    v = ++p[1];
    printf("%d %d %d | ", values[0], v, values[1]);
    b = 0;
    b += 5;
    printf("%d ", b);
    b = 0;
    b -= 1;
    printf("%d ", b);
    b = 1;
    b <<= 8;
    printf("%d ", b);
    b = 0;
    v = (b += 2);
    printf("%d %d ", v, b);
    h.plain = 0;
    h.plain += 5;
    values[1] = 0;
    values[1] += 256;
    printf("%d %d ", h.plain, values[1]);
    *p = 0;
    v = (*p += 2);
    printf("%d %d ", v, values[0]);
    b = 0;
    for (v = 0; v < 3; v++)
        b++;
    printf("%d", b);
    return 0;
}
EOF
try_ 8 << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    int (*p)[2] = rows;
    int column = 0;
    (*p)[column++] += 4;
    return column + rows[0][0] - 1;
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    typedef const int (*row_pointer)[2];
    row_pointer p = rows;
    (*p)[1] += 1;
    return 0;
}
EOF
try_ 8 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    int column = 0;
    (*p)[column++] = 7;
    return column + rows[0][0];
}
EOF
try_compile_error << EOF
int main(void) {
    int rows[1][2] = { { 4, 9 } };
    typedef const int (*row_pointer)[2];
    row_pointer p = &rows[0];
    (*p)[1] = 7;
    return 0;
}
EOF
try_ 7 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    p[1][0] = 7;
    return p[1][0];
}
EOF
try_ 6 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    return p[1][0];
}
EOF
try_ 7 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    int (*p)[2] = &rows[0];
    p[1][0] = 7;
    return rows[1][0];
}
EOF
try_ 1 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    return p == &rows[0];
}
EOF
try_ 7 << EOF
int main(void) {
    int rows[2][2] = { { 4, 9 }, { 6, 8 } };
    typedef int (*row_pointer)[2];
    row_pointer p = &rows[0];
    p[1][0] = 7;
    return rows[1][0];
}
EOF
try_compile_error << EOF
int main(void) { typedef int (*row_pointer)[0]; return 0; }
EOF
try_ 0 << EOF
int main(void) {
    typedef int (*row_pointer)[2];
    typedef row_pointer table[2];
    int rows[2][2] = { { 3, 5 }, { 7, 8 } };
    table pointers = { rows, rows + 1 };
    return pointers[1][0][1] != 8 ||
           sizeof(*pointers[1]) != 2 * sizeof(int);
}
EOF
try_compile_error << EOF
int main(void) {
    typedef int (*row_pointer)[2];
    typedef row_pointer matrix[2][2];
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { typedef int (*row_pointer)[2]; typedef row_pointer *rows; return 0; }
EOF
try_compile_error << EOF
int main(void) { typedef const int readonly_t; readonly_t value = 1; value = 2; }
EOF
try_compile_error << EOF
int main(void) {
    typedef int *inner_pointer;
    typedef inner_pointer * const fixed_pointer;
    fixed_pointer value = 0;
    value = 0;
}
EOF
try_ 14 << EOF
struct loop_record { int value; };
int main(void) {
    for (typedef struct loop_record loop_alias; 1; ) {
        loop_alias value = { 14 };
        return value.value;
    }
}
EOF
try_compile_error << EOF
int main(void) {
    for (typedef int loop_only; 0; ) {}
    loop_only expired;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { for (typedef int invalid = 1; 0; ) {} return 0; }
EOF
try_ 3 << EOF
int main(void) {
    for (int value = 3; value; value = 0)
        return value;
    return 0;
}
EOF
try_compile_error << EOF
void invalid_global_void_object;
EOF
try_compile_error << EOF
int main(void) { void invalid_local_void_object; return 0; }
EOF
try_compile_error << EOF
int main(void) { for (void invalid_for_void_object; 0; ) {} return 0; }
EOF
try_compile_error << EOF
struct invalid_void_member { void member; };
EOF
try_compile_error << EOF
typedef void invalid_void_alias;
invalid_void_alias invalid_hidden_global_void_object;
EOF
try_compile_error << EOF
typedef void invalid_void_alias;
int main(void) { invalid_void_alias invalid_hidden_local_void_object; return 0; }
EOF
try_compile_error << EOF
typedef void invalid_void_alias;
struct invalid_hidden_void_member { invalid_void_alias member; };
EOF
try_compile_error << EOF
typedef void invalid_void_alias;
int invalid_void_parameter(invalid_void_alias value);
EOF
try_compile_error << EOF
int invalid_void_array_parameter(void values[]);
EOF
try_compile_error << EOF
typedef void invalid_void_alias;
int invalid_hidden_void_array_parameter(invalid_void_alias values[]);
EOF
try_compile_error << EOF
void (*invalid_void_pointer_to_array)[2];
EOF
try_ 0 << EOF
void valid_void_function(void) {}
typedef void valid_void_alias;
typedef void *valid_void_pointer_alias;
int valid_void_pointer_parameter(void *value) { return value != 0; }
int main(void) {
    void *pointer = 0;
    void *pointer_array[1] = { pointer };
    valid_void_pointer_alias alias_pointer_array[1] = { pointer };
    valid_void_alias *alias_pointer = pointer;
    valid_void_function();
    return pointer != 0 || alias_pointer != 0 ||
           pointer_array[0] != 0 || alias_pointer_array[0] != 0 ||
           valid_void_pointer_parameter(pointer);
}
EOF

items 7 "auto int value = 7; return value;"
items 3 "int total = 0; for (auto int value = 0; value < 3; value++) total += value; return total;"
items 6 "int first = 1, second = 2; first++, second += 2; return first + second;"
items 6 "int first = 0; int second = first = 3; return first + second;"
try_ 2 << EOF
int main(void) {
    int value = 0;
    value ? value = 1 : value = 2;
    return value;
}
EOF
try_ 3 << EOF
int main(void) {
    int first = 0, second = 0;
    (first = 1, second = 2);
    return first + second;
}
EOF
try_ 1 << EOF
int main(void) {
    int value = 1;
    value + 4;
    return value;
}
EOF
try_ 7 << EOF
struct pair { int left; int right; };
int main(void) {
    struct pair first = {1, 2};
    struct pair second = {3, 4};
    (first = second);
    return first.left + first.right;
}
EOF
try_ 7 << EOF
struct pair { int left; int right; };
int main(void) {
    struct pair first = {1, 2};
    struct pair second = {3, 4};
    struct pair copied = (first = second);
    return copied.left + copied.right;
}
EOF
try_compile_error << EOF
int main(void) { auto auto int value = 0; return value; }
EOF
try_compile_error << EOF
int main(void) { static auto int value = 0; return value; }
EOF
try_compile_error << EOF
auto int file_scope;
EOF

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

try_ 2 << EOF
typedef int *int_pointer;
int main(void) {
    int value = 1;
    int_pointer restrict pointer = &value;
    *pointer += 1;
    return value;
}
EOF

try_ 2 << EOF
typedef int *int_pointer;
int increment(int_pointer restrict pointer) { *pointer += 1; return *pointer; }
int main(void) { int value = 1; return increment(&value); }
EOF

try_compile_error << EOF
int restrict value;
EOF

try_compile_error << EOF
typedef int (*callback)(void);
callback restrict function;
EOF

try_ 14 << EOF
inline int increment_inline(int value) { return value + 1; }
static inline int twice_inline(int value) { return value * 2; }
int inline trailing_inline(int value) { return value - 1; }
int main(void) {
    return twice_inline(increment_inline(6)) + trailing_inline(1);
}
EOF

# C99 6.7.4 forbids an external-linkage inline definition from containing a
# modifiable static definition or naming an internal-linkage object/function.
try_compile_error << EOF
inline int invalid_external_inline_static(void) {
    static int value;
    return value;
}
EOF

try_compile_error << EOF
static int hidden_external_inline_object;
inline int invalid_external_inline_object(void) {
    return hidden_external_inline_object;
}
EOF

try_compile_error << EOF
static int hidden_external_inline_function(void) { return 1; }
inline int invalid_external_inline_function(void) {
    return hidden_external_inline_function();
}
EOF

try_compile_error << EOF
static int hidden_external_inline_address(void) { return 1; }
inline int invalid_external_inline_address(void) {
    return (&hidden_external_inline_address) != 0;
}
EOF

try_compile_error << EOF
static int hidden_external_inline_sizeof;
inline int invalid_external_inline_sizeof(void) {
    return sizeof hidden_external_inline_sizeof;
}
EOF

try_ 3 << EOF
static inline int permitted_internal_inline(void) {
    static int value;
    return ++value;
}
int main(void) {
    return permitted_internal_inline() + permitted_internal_inline();
}
EOF

try_ 2 << EOF
inline int permitted_external_inline_const(void) {
    static int const value = 1;
    static int * const pointer = 0;
    return value + (pointer == 0);
}
int main(void) { return permitted_external_inline_const(); }
EOF

try_ 13 << EOF
int third(int values[static 3]) { return values[2]; }
int sum_pair(int values[restrict static 2]) { return values[0] + values[1]; }
int main(void) {
    int values[3] = { 3, 4, 6 };
    return third(values) + sum_pair(values);
}
EOF

try_ 0 << EOF
int increment(int value) { return value + 1; }
int apply(int (*callbacks[static 1])(int)) { return callbacks[0](2) != 3; }
int main(void) { int (*callback)(int) = increment; return apply(&callback); }
EOF

try_compile_error << EOF
int invalid(int values[static]);
EOF

try_compile_error << EOF
int invalid(int values[2][static 2]);
EOF

try_compile_error << EOF
int invalid(int values[2][const 2]);
EOF

try_compile_error << EOF
int invalid(int values[2][volatile 2]);
EOF

try_compile_error << EOF
int invalid(int values[2][restrict 2]);
EOF

try_compile_error << EOF
int invalid(int (*callbacks[2][static 1])(int));
EOF

try_compile_error << EOF
int invalid(int values[static 0]);
EOF

try_compile_error << EOF
int invalid(int values[static -1]);
EOF

try_compile_error << EOF
int values[static 2];
EOF

try_compile_error << EOF
int invalid(int values[const 2]) { values = 0; return 0; }
EOF

try_compile_error << EOF
int invalid(int values[const]) { values = 0; return 0; }
EOF

try_compile_error << EOF
inline int invalid_inline_object;
int main(void) { return 0; }
EOF

try_compile_error << EOF
inline struct invalid_inline_record { int value; } object;
EOF

try_compile_error << EOF
inline union invalid_inline_union { int value; } object;
EOF

try_compile_error << EOF
inline enum invalid_inline_enum { invalid_inline_value } object;
EOF

try_compile_error << EOF
inline typedef int invalid_inline_typedef;
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

# __func__ behaves as a static character array before ordinary expression decay:
# taking its address, restoring the array, and dereferencing again yields its
# first byte.
try_ 1 << EOF
int addressed_func_name(void) { return *(*(&__func__)) == 'a'; }
int main(void) { return addressed_func_name(); }
EOF

# Postfix subscripting applies to parenthesized pointer and string expressions.
try_ 7 << EOF
int main(void) {
    int values[2] = { 4, 7 };
    int *pointer = values;
    return ("cat")[1] + (pointer)[1] - 'a';
}
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

# Block-scope tagged records use the same layout path as file-scope records,
# including immediate declarators, bit-fields, unions, and recursive pointers.
try_ 21 << EOF
int main(void) {
    struct flags { unsigned int low : 3; unsigned int high : 3; }
        bits = {2, 4};
    union payload { int number; char bytes[4]; } data = {7};
    struct node { struct node *next; int value; }
        first = {0, 3}, second = {&first, 5};
    return bits.low + bits.high + data.number + first.value +
           second.value + (second.next == &first ? 0 : 1);
}
EOF
try_ 40 << EOF
int struct_value(void) {
    static struct pair { int left; int right; } saved = {3, 4};
    saved.left++;
    return saved.left + saved.right;
}
int union_value(void) {
    static union payload { int number; char bytes[4]; } saved = {5};
    saved.number++;
    return saved.number;
}
int array_value(void) {
    static struct point { int x; int y; } points[2] = {{1, 2}, {3, 4}};
    return points[0].x + points[0].y + points[1].x + points[1].y;
}
int main(void) {
    return struct_value() + struct_value() + union_value() + union_value() +
           array_value();
}
EOF

# Brace elision: scalars that meet an array member without braces fill its
# elements in order before the next member, in automatic and static records and
# in unions. The 32-byte member is wider than any store a backend emits.
try_ 8 << EOF
typedef struct { char name[32]; int a; } named_t;
typedef struct { char tag[4]; int a; } tagged_t;
typedef struct { int m[2][2]; int b; } matrix_t;
typedef struct { int *p[2]; int c; } slots_t;
typedef union { char buf[4]; int x; } bytes_t;
int first = 5, second = 6;
tagged_t global_tagged = {1, 2, 3, 4, 5};
static slots_t global_slots = {&first, &second, 7};
int main(void) {
    named_t zero = {0};
    tagged_t tagged = {1, 2};
    matrix_t matrix = {1, 2, 3, 4, 5};
    bytes_t bytes = {1, 2};
    int r = zero.name[0] == 0 && zero.a == 0;
    r += tagged.tag[1] == 2 && tagged.tag[2] == 0 && tagged.a == 0;
    r += matrix.m[1][1] == 4 && matrix.b == 5;
    r += bytes.buf[0] == 1 && bytes.buf[1] == 2;
    r += global_tagged.tag[3] == 4 && global_tagged.a == 5;
    r += *global_slots.p[1] == 6 && global_slots.c == 7;
    zero.a = 2;
    return r + zero.a;
}
EOF
# An array of pointers to records is still an array of scalars for elision.
try_ 7 << EOF
struct row { int values[2][3]; };
struct rows { struct row *slots[2]; int tail; };
static struct row first_row, second_row;
static struct rows global_rows = {&first_row, &second_row, 4};
int main(void) {
    struct rows local_rows = {&second_row};
    return (global_rows.slots[0] == &first_row) +
           2 * (global_rows.slots[1] == &second_row) +
           (local_rows.slots[0] == &second_row) + global_rows.tail - 1;
}
EOF

# A record element or member without braces takes one initializer per member
# from the enclosing list, so { 1, 2, 3, 4 } fills two points. The elided record
# stops at its last member, at a designator of the enclosing list, or at a
# braced element, and a block scope record value still initializes the whole
# element. Checked at file scope, block scope and for block statics.
try_ 0 << EOF
typedef struct p { int x, y; } P;
struct q { struct p a; int z; };
struct r { char s[4]; int n; };
struct u { struct p a[2]; int z; };
struct b { unsigned lo : 3; unsigned : 2; unsigned hi : 3; int t; };
union n { int i; char c[4]; };
struct w { char c; long long v; };
#define DECLS(S)                                      \
    S P v1[3] = { 1, 2, [2] = 5, 6 };                 \
    S struct q v2 = { 1, .z = 3 };                    \
    S struct r v3[2] = { "abc", 1, "de", 2, };        \
    S struct u v4 = { 1, 2, {3, 4}, 5 };              \
    S struct b v5[2] = { 1, 7, 2, 3, 6, 4 };          \
    S union n v6[2] = { 5, 6 };                       \
    S struct w v7[] = { 'a', 11, 'b', 22 };           \
    S struct p v8[2][2] = { 1, 2, 3, 4, 5, 6, 7, 8 }; \
    S struct q v9[] = { 1, 2, 3, {4, 5}, 6 };         \
    S struct u v10 = { { 1, 2, 3, 4 }, 5 };           \
    S struct p v11[] = { 1, [2] = 3, 4, [0] = 5 };
#define CHECKS                                                                 \
    int r = 0;                                                                 \
    if (v1[0].x != 1 || v1[0].y != 2 || v1[1].x || v1[1].y || v1[2].x != 5 || \
        v1[2].y != 6)                                                          \
        r |= 1;                                                                \
    if (v2.a.x != 1 || v2.a.y || v2.z != 3)                                    \
        r |= 2;                                                                \
    if (v3[0].s[2] != 'c' || v3[0].n != 1 || v3[1].s[1] != 'e' ||              \
        v3[1].n != 2)                                                          \
        r |= 4;                                                                \
    if (v4.a[0].y != 2 || v4.a[1].x != 3 || v4.a[1].y != 4 || v4.z != 5)       \
        r |= 8;                                                                \
    if (v5[0].lo != 1 || v5[0].hi != 7 || v5[0].t != 2 || v5[1].lo != 3 ||     \
        v5[1].hi != 6 || v5[1].t != 4)                                         \
        r |= 16;                                                               \
    if (v6[0].i != 5 || v6[1].i != 6)                                          \
        r |= 32;                                                               \
    if (sizeof(v7) / sizeof(v7[0]) != 2 || v7[0].c != 'a' || v7[0].v != 11 ||  \
        v7[1].c != 'b' || v7[1].v != 22)                                       \
        r |= 64;                                                               \
    if (v8[0][0].x != 1 || v8[0][1].y != 4 || v8[1][0].x != 5 ||               \
        v8[1][1].y != 8)                                                       \
        r |= 128;                                                              \
    if (sizeof(v9) / sizeof(v9[0]) != 3 || v9[0].z != 3 || v9[1].a.x != 4 ||   \
        v9[1].a.y != 5 || v9[1].z || v9[2].a.x != 6 || v9[2].z)                \
        r |= 256;                                                              \
    if (v10.a[0].y != 2 || v10.a[1].x != 3 || v10.a[1].y != 4 || v10.z != 5)   \
        r |= 512;                                                              \
    if (sizeof(v11) / sizeof(v11[0]) != 3 || v11[0].x != 5 || v11[0].y ||      \
        v11[1].x || v11[1].y || v11[2].x != 3 || v11[2].y != 4)                \
        r |= 1024;                                                             \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int values(void) {
    struct p s1 = {1, 2}, s2 = {3, 4};
    struct p pair[2] = { s1, s2 };
    struct q outer[2] = { s2, 9, s1, 8 };
    struct p *lit = (struct p[]){ 7, 8, 9 };
    return pair[0].y != 2 || pair[1].x != 3 || outer[0].a.y != 4 ||
           outer[0].z != 9 || outer[1].a.x != 1 || outer[1].z != 8 ||
           lit[1].x != 9 || lit[1].y;
}
int main(void) {
    int r = check_global() != 0;
    dirty();
    return r | (check_local() != 0) << 1 | (check_static() != 0) << 2 |
           values() << 3;
}
EOF
# A tag is an identifier, so it may be as long as any other identifier.
try_ 4 << EOF
struct a_record_tag_name_longer_than_thirty_two_bytes { int value; };
int main(void) {
    struct a_record_tag_name_longer_than_thirty_two_bytes item = {4};
    return item.value;
}
EOF

# Category: Compound Literals
begin_category "Compound Literals" "Testing C99 compound literal features"

try_ 7 << EOF
int main(void) { return (int){3} = 7; }
EOF
try_ 7 << EOF
int main(void) { return (int){3} += 4; }
EOF
try_ 9 << EOF
int main(void) {
    int first = 3, second = 9;
    int *pointer = (int *){&first} = &second;
    return *pointer;
}
EOF
try_ 2 << EOF
int main(void) {
    int values[3] = {1, 2, 3};
    int *pointer = ++(int *){values};
    return *pointer;
}
EOF
try_ 2 << EOF
int main(void) {
    int values[3] = {1, 2, 3};
    int *pointer = --(int *){&values[2]};
    return *pointer;
}
EOF
try_ 3 << EOF
int main(void) {
    int values[4] = {1, 2, 3, 4};
    int *pointer = ((int *){values} += 2);
    return *pointer;
}
EOF
try_ 2 << EOF
int main(void) {
    int values[4] = {1, 2, 3, 4};
    int *pointer = ((int *){&values[3]} -= 2);
    return *pointer;
}
EOF
try_ 2 << EOF
int main(void) {
    int values[4] = {1, 2, 3, 4};
    int *pointer = (int *){values};
    pointer += 2;
    pointer -= 1;
    return *pointer;
}
EOF
try_compile_error << EOF
int main(void) {
    int value = 3;
    return (int * const){&value} = &value;
}
EOF
try_compile_error << EOF
int main(void) {
    int value = 3;
    return (int * const){&value}++;
}
EOF
try_ 4 << EOF
int main(void) { return (unsigned char){250} += 10; }
EOF
try_ 1 << EOF
int main(void) { return (_Bool){0} = 2; }
EOF
try_ 1 << EOF
int main(void) { return (_Bool){1} += 2; }
EOF
try_compile_error << EOF
int main(void) { return (const int){3} += 4; }
EOF
try_ 7 << EOF
int main(void) { return (int[]){1, 2}[1] = 7; }
EOF
try_ 6 << EOF
int main(void) {
    return (int[2][3]){{1, 2, 3}, {4, 5, 6}}[1][2];
}
EOF
try_ 3 << EOF
int main(void) {
    return (int[][2]){{1, 2}, {3, 4}}[1][0];
}
EOF
try_ 16 << EOF
int main(void) {
    return (int[2][2][2]){{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}[1][1][1] +
           ((int[2][2][2]){{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}})[1][0][0] +
           (int[][2][2]){{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}[0][1][0];
}
EOF
try_ 17 << EOF
int main(void) {
    return (int[2][2][2][2]){{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                              {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}}[1][0][1][0] +
           ((int[2][2][2][2]){{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}},
                                {{{9, 10}, {11, 12}}, {{13, 14}, {15, 16}}}})[0][1][0][1];
}
EOF
try_ 6 << EOF
typedef int matrix[2][3];
int main(void) {
    return (matrix){{1, 2, 3}, {4, 5, 6}}[1][2];
}
EOF
try_ 24 << EOF
int main(void) {
    return sizeof((int[2][3]){{1, 2, 3}, {4, 5, 6}});
}
EOF

# Fewer subscripts than an array compound literal's rank select a row, which
# sizeof measures whole and a following use decays to a pointer that steps by
# the remaining bounds.
try_ 0 << 'EOF'
int main(void)
{
    int *row = (int[2][3]){{1, 2, 3}, {4, 5, 6}}[1];

    if (sizeof((int[2][3]){{1}}[1]) != 3 * sizeof(int) ||
        sizeof((int[2][2][3]){{{1}}}[1]) != 6 * sizeof(int))
        return 1;
    if (sizeof((int[2][3]){{1}}[1][2]) != sizeof(int) ||
        sizeof((int[2][3]){{1}}) != 6 * sizeof(int))
        return 2;
    if (row[2] != 6 || *(int[2][3]){{1, 2, 3}, {4, 5, 6}}[1] != 4 ||
        (int[2][3]){{1, 2, 3}, {4, 5, 6}}[1][2] != 6)
        return 3;
    return 0;
}
EOF
try_ 7 << EOF
typedef int matrix[2][2];
int main(void) {
    matrix value = {{1, 2}, {3, 4}};
    return value[1][1] + value[0][0] * 3;
}
EOF
try_ 7 << EOF
typedef int row[2];
typedef row matrix[2];
int main(void) {
    matrix value = {{1, 2}, {3, 4}};
    return value[1][1] + value[0][0] * 3;
}
EOF
try_ 11 << EOF
typedef int row[2];
typedef row matrix[2];
typedef matrix cube[2];
int main(void) {
    cube value = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    return value[1][1][1] + value[0][0][0] * 3;
}
EOF

# An object declared as an array of an array typedef, `row m[2]`, is an array of
# rows; its size and subscript strides include the typedef's own bound. Checked
# at file scope, block scope and for block-scope statics, through parameters and
# record members, and for a later declarator of the same declaration.
try_ 0 << 'EOF'
typedef int row[2];
typedef row plane[2];
struct rows { char tag; row r[3]; };
row g_m[2] = {{1, 2}, {3, 4}};
row g_t[] = {{1, 2}, {3, 4}, {5, 6}};
row g_e[2] = {1, 2, 3, 4};
plane g_c[2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
struct rows g_r = {9, {{1, 2}, {3, 4}, {5, 6}}};
row *g_pr = &g_m[1];
row g_a, g_b;
int sum_rows(row r[], int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += r[i][0] * 10 + r[i][1];
    return s;
}
int last(row r[3]) { return r[2][1]; }
int cube_at(plane p[], int i, int j, int k) { return p[i][j][k]; }
#define SIZES(m, t, c, b) (sizeof m * 1000000 + sizeof t * 10000 + \
                          sizeof c * 100 + sizeof b)
int check(row *m, row *t, row *e, plane *c, struct rows *r, row *pr, int sizes)
{
    if (sizes != 16243208)
        return 1;
    if ((char *) &m[1][1] - (char *) m != 12)
        return 2;
    if (m[1][0] != 3 || t[2][1] != 6 || e[1][1] != 4 || e[0][1] != 2)
        return 3;
    if ((char *) &c[1][1][1] - (char *) c != 28 || c[1][0][1] != 6)
        return 4;
    if (sizeof(struct rows) != 28 || r->r[2][1] != 6 ||
        (char *) &r->r[1][0] - (char *) r != 12)
        return 5;
    if ((char *) pr - (char *) m != 8 || (*pr)[1] != 4 || pr[0][0] != 3)
        return 6;
    if (sum_rows(t, 3) != 102 || last(t) != 6 || cube_at(c, 1, 1, 1) != 8)
        return 7;
    return 0;
}
int block(void)
{
    row m[2] = {{1, 2}, {3, 4}};
    row t[] = {{1, 2}, {3, 4}, {5, 6}};
    row e[2] = {1, 2, 3, 4};
    plane c[2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    struct rows r = {9, {{1, 2}, {3, 4}, {5, 6}}};
    row *pr = &m[1];
    row a, b;
    b[1] = 7;
    a[1] = 8;
    c[0][1][1] = 41;
    if (b[1] != 7 || c[0][1][1] != 41 || c[1][0][0] != 5)
        return 8;
    c[0][1][1] = 4;
    return check(m, t, e, c, &r, pr, SIZES(m, t, c, b));
}
int block_static(void)
{
    static row m[2] = {{1, 2}, {3, 4}};
    static row t[] = {{1, 2}, {3, 4}, {5, 6}};
    static row e[2] = {1, 2, 3, 4};
    static plane c[2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
    static struct rows r = {9, {{1, 2}, {3, 4}, {5, 6}}};
    static row *pr = &m[1];
    static row a, b;
    return check(m, t, e, c, &r, pr, SIZES(m, t, c, b));
}
int main(void)
{
    int rc = check(g_m, g_t, g_e, g_c, &g_r, g_pr, SIZES(g_m, g_t, g_c, g_b));
    if (rc)
        return rc;
    rc = block();
    if (rc)
        return rc + 10;
    rc = block_static();
    return rc ? rc + 20 : 0;
}
EOF
try_ 11 << EOF
int main(void) {
    return (int[2][2]){{1, 2}, {3, 4}}[1][0] += 8;
}
EOF
try_ 3 << EOF
int main(void) {
    return (int[2][2]){{1, 2}, {3, 4}}[1][0]++;
}
EOF
try_ 5 << EOF
int main(void) {
    return (int[2]){4, 5}[1]--;
}
EOF
try_ 4 << EOF
int main(void) {
    return ++(int[2][2]){{1, 2}, {3, 4}}[1][0];
}
EOF
try_compile_error << EOF
int main(void) { return ++(const int[1]){1}[0]; }
EOF
try_compile_error << EOF
int main(void) { return (int[2][2])0; }
EOF
try_ 7 << EOF
int main(void) { return (int[]){1, 2}[1] += 5; }
EOF
try_ 7 << EOF
struct point { int x; int y; };
int main(void) { return (struct point){1, 2}.x = 7; }
EOF
try_ 7 << EOF
struct flags { unsigned int value : 3; };
int main(void) { return (struct flags){1}.value += 6; }
EOF
try_ 1 << EOF
struct point { int x; };
int main(void) { return (struct point){1}.x++; }
EOF
try_ 2 << EOF
struct point { int x; };
int main(void) { return ++(struct point){1}.x; }
EOF
try_ 7 << EOF
struct flags { unsigned int value : 3; };
int main(void) { return (struct flags){7}.value++; }
EOF
try_ 1 << EOF
struct flags { unsigned int value : 3; };
int main(void) { return ++(struct flags){0}.value; }
EOF
try_compile_error << EOF
struct point { int x; };
int main(void) { return (const struct point){1}.x++; }
EOF
try_compile_error << EOF
struct point { int x; };
int main(void) { return ++(const struct point){1}.x; }
EOF
try_ 7 << EOF
struct holder { int items[2]; };
int main(void) { return (struct holder){{1, 2}}.items[1] = 7; }
EOF
try_ 3 << EOF
struct holder { int items[2]; };
int main(void) { return ++(struct holder){{1, 2}}.items[1]; }
EOF
try_compile_error << EOF
struct holder { int items[2]; };
int main(void) { return ++(const struct holder){{1, 2}}.items[1]; }
EOF
try_ 7 << EOF
struct holder { int items[2][2]; };
int main(void) {
    return (struct holder){{{1, 2}, {3, 4}}}.items[1][0] = 7;
}
EOF
try_ 8 << EOF
struct holder { int items[2][2]; };
int main(void) {
    return (struct holder){{{1, 2}, {3, 4}}}.items[1][0] += 5;
}
EOF
try_ 9 << EOF
struct holder { int items[2][2][2]; };
int main(void) {
    return (struct holder){{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}}
        .items[1][0][1] = 9;
}
EOF
try_ 7 << EOF
struct point { int x; };
struct holder { struct point point; };
int main(void) { return (struct holder){{1}}.point.x = 7; }
EOF
try_compile_error << EOF
int main(void) { return (const int){1} = 2; }
EOF
try_compile_error << EOF
struct point { int x; };
int main(void) { return (const struct point){1}.x = 2; }
EOF
try_compile_error << EOF
int main(void) { return (const int[]){1}[0] = 2; }
EOF
try_ 1 << EOF
int main(void) { return (int){1}++; }
EOF
try_compile_error << EOF
int main(void) { return (const int){1}++; }
EOF
try_ 2 << EOF
int main(void) { return ++(int){1}; }
EOF
try_compile_error << EOF
int main(void) { return ++(const int){1}; }
EOF
try_ 7 << EOF
struct point { int x; };
int main(void) {
    struct point point = {0};
    struct point *pointer = &point;
    return (*pointer).x = 7;
}
EOF
try_ 7 << EOF
struct flags { unsigned int value : 3; };
int main(void) {
    struct flags flags = {1};
    struct flags *pointer = &flags;
    return (*pointer).value += 6;
}
EOF

# ++ and -- apply to any modifiable lvalue, not only to an identifier: a
# dereference, a subscript of one, or a member reached through either. Each
# steps by the object's own type, so a pointer advances by its element size, a
# long long carries into its upper half and a _Bool stays 0 or 1.
try_ 0 << EOF
struct rec { int x; char c; long long w; int *p; _Bool b; int m; };
struct holder { struct rec *slots[2]; };
int main(void) {
    int x = 5, arr[4] = {10, 20, 30, 40};
    int *p = &x, *ap = arr, **pp = &ap;
    char ch = 'a', *cp = &ch;
    long long wide = 0xFFFFFFFFLL, *lp = &wide;
    _Bool flag = 1, *bp = &flag;
    struct rec r = {1, 'b', 0x1FFFFFFFFLL, arr, 0, 7};
    struct rec *rp = &r, *slots[2] = {&r, &r}, **rpp = slots;
    struct holder h;
    int v;

    ++*p;
    --*p;
    (*p)++;
    (*p)--;
    if (x != 5) return 1;
    if (++*p != 6 || (*p)++ != 6 || x != 7 || --*p != 6 || (*p)-- != 6 ||
        x != 5)
        return 2;
    ++*cp;
    if (ch != 'b' || (*cp)++ != 'b' || ch != 'c') return 3;
    ++*lp;
    if (wide != 0x100000000LL || (*lp)-- != 0x100000000LL ||
        wide != 0xFFFFFFFFLL || --*lp != 0xFFFFFFFELL)
        return 4;
    ++*pp;
    (*pp)++;
    if (*ap != 30 || ap != arr + 2) return 5;
    --*pp;
    (*pp)[1]++;
    if (arr[2] != 31) return 6;
    ap = arr;
    v = ++*ap++;
    if (v != 11 || arr[0] != 11 || ap != arr + 1) return 7;
    ++*bp;
    if (flag != 1) return 8;
    (*bp)--;
    if (flag != 0) return 9;
    --*bp;
    if (flag != 1) return 10;
    ++rp->x;
    ++(*rp).m;
    (*rp).w++;
    (*rp).p++;
    if (r.x != 2 || r.m != 8 || r.w != 0x200000000LL || *r.p != 20) return 11;
    ++(*rp).b;
    (*rp).b++;
    if (r.b != 1) return 12;
    (*rpp)->x++;
    ++(*rpp)->c;
    if (r.x != 3 || r.c != 'c') return 13;
    h.slots[1] = &r;
    (h.slots[1])->m--;
    ++(h.slots[1])->m;
    (h.slots[1])->m++;
    return r.m != 9 ? 14 : 0;
}
EOF
try_compile_error << EOF
int main(void) { int x = 1, *p = &x; ++(*p + 1); return x; }
EOF
try_compile_error << EOF
int main(void) { int x = 1, *p = &x; (0, *p)++; return x; }
EOF
try_compile_error << EOF
int main(void) { const int x = 1; const int *p = &x; (*p)++; return x; }
EOF

# Any postfix expression can follow a parenthesized primary (C99 6.5.2): a
# subscript, a member selection or a call applies to the grouped value, and a
# record loaded through a pointer is a whole object rather than one word of it.
# Reads, stores and compound assignments of every member width through (*q).m
# must agree with q->m, and a record stores into a member or an element whole.
try_ 0 << EOF
struct inner { char c; long long w; short s; };
struct rec {
    int pad; char c; short s; int i; long long m; int *p;
    struct inner in; int arr[3];
};
struct point { int x; int m; };
struct holder { struct point *slots[2]; int *ip; };
struct point *identity(struct point *p) { return p; }
int main(void) {
    int x = 7;
    struct rec v, rows[2], *q = &v, *rp = rows, **rpp = &rp;
    struct inner t;
    struct point pts[2], *pp = pts, **ppp = &pp;
    struct holder h;

    v.pad = 1; v.c = 2; v.s = 3; v.i = 4; v.m = 0x100000005LL; v.p = &x;
    v.in.c = 6; v.in.w = 0x200000007LL; v.in.s = -3;
    v.arr[0] = 8; v.arr[1] = 9; v.arr[2] = 10;
    rows[1] = v;
    if (rows[1].m != 0x100000005LL || rows[1].in.w != 0x200000007LL) return 1;
    if ((*q).c != 2 || (*q).s != 3 || (*q).i != 4) return 2;
    if ((*q).m != 0x100000005LL || (*q).m != q->m) return 3;
    if (*(*q).p != 7 || (*q).in.c != 6 || (*q).in.s != -3) return 4;
    if ((*q).in.w != 0x200000007LL || (*q).arr[2] != 10) return 5;
    if ((*rpp)[1].m != 0x100000005LL || (rp)[1].in.w != 0x200000007LL) return 6;
    if ((*rpp)[1].arr[1] != 9 || (rp + 1)->i != 4) return 7;

    (*q).c = 12; (*q).s = 13; (*q).i = 14; (*q).m = 0x300000015LL;
    (*q).in.w = 0x400000017LL; (*q).arr[1] = 19; (*q).in.s = 21;
    if (v.c != 12 || v.s != 13 || v.i != 14 || v.m != 0x300000015LL) return 8;
    if (v.in.w != 0x400000017LL || v.arr[1] != 19 || v.in.s != 21) return 9;
    (*rpp)[1].m = 0x500000001LL;
    (*rpp)[1].in.w = 0x600000001LL;
    if (rows[1].m != 0x500000001LL || rows[1].in.w != 0x600000001LL) return 10;

    (*q).c += 1; (*q).s -= 1; (*q).i *= 2; (*q).m += 0x100000000LL;
    (*q).in.w -= 1; (*q).arr[1] <<= 1; (*q).p += 1;
    if (v.c != 13 || v.s != 12 || v.i != 28 || v.m != 0x400000015LL) return 11;
    if (v.in.w != 0x400000016LL || v.arr[1] != 38 || v.p != &x + 1) return 12;
    (*rpp)[1].m |= 2;
    if (rows[1].m != 0x500000003LL) return 13;

    t = (*q).in;
    if (t.w != 0x400000016LL) return 14;
    t.w = 1;
    (*q).in = t;
    if (v.in.w != 1) return 15;
    q->in.w = 2;
    t = q->in;
    rows[0].in = t;
    *rp = v;
    if (t.w != 2 || rows[0].in.w != 2 || rp->m != v.m) return 16;

    pts[1].x = 5;
    pts[1].m = 6;
    h.slots[1] = &pts[1];
    h.ip = &x;
    if ((h.slots[1])->x != 5 || (pts[1]).m != 6) return 17;
    if ((identity(&pts[1]))->x != 5 || (pp + 1)->x != 5) return 18;
    if ((*ppp)->x != pts[0].x || (h.ip)[0] != 7 || (v.arr)[2] != 10) return 19;
    (h.slots[1])->x = 15;
    (pp + 1)->m += 4;
    (*ppp)[1].x++;
    return pts[1].x != 16 || pts[1].m != 10 ? 20 : 0;
}
EOF

# A record is neither arithmetic nor scalar: it cannot be the operand of an
# arithmetic, bitwise, relational, logical or unary operator, a cast, a
# controlling expression or a scalar assignment. Each is a diagnostic rather
# than a record reaching integer lowering, which aborted on 32-bit targets.
for expr in "x = 4 & s" "x = s + 1" "x = s == s" "x = s < x" "x = -s" "x = ~s" \
    "x = !s" "x = +s" "x = s && x" "x = x || s" "x = s ? 1 : 0" "x += s" \
    "x = (int) s" "x = s" "s = x" "p->a = s" "*p = x" "s++" "--s" "++*p" \
    "if (s) x = 1" "while (s) x = 1" "for (; s;) x = 1" "do x = 1; while (s)" \
    "x = *p + 1" "x -= *p" "x = s << 1"; do
    try_compile_error << EOF
struct S { int a; int b; int c; char d; };
int main(void) {
    struct S s = {1, 2, 3, 4}, *p = &s;
    int x = 4;
    $expr;
    return x;
}
EOF
done
try_ 7 << EOF
struct S { int a; int b; int c; char d; };
int main(void) {
    struct S s = {1, 2, 3, 4}, t, *p = &s;
    int x = 1;
    (void) s;
    t = x ? s : *p;
    x = (t = *p).b;
    return x + (x ? s.a : 0) + t.d;
}
EOF

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

# A negative designator is outside every array, including one whose bound the
# compound literal leaves to be inferred.
try_compile_error_message "Array designator index is out of bounds" << EOF
int main(void) {
    int *values = (int[]){[-1] = 1, 2};
    return values[0];
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

# A record reached through a pointer, a subscript or a member is a whole object
# too, as a source and as a destination: every one of these used to move one
# register's worth of it, and a 32-bit backend could not encode the load of a
# record of odd size at all.
try_ 11 << EOF
typedef struct { _Bool a, b, c, d, e, f; } flags_t;
int count(const flags_t *spec) {
    flags_t copy;
    copy = *spec;
    return copy.a + copy.b * 2 + copy.f * 4;
}
int main(void) {
    flags_t flags = {1, 0, 0, 0, 0, 1};
    flags_t again = flags;
    return count(&flags) + count(&again) + (sizeof(flags_t) == 6);
}
EOF

try_ 44 << EOF
struct odd { char a; short b; char c; int d; char e; };
struct wrap { char tag; struct odd in; };
struct odd global;
struct odd id(struct odd value) { return value; }
struct odd pick(struct wrap *w, int i) { return i ? w->in : w[0].in; }
int main(void) {
    struct wrap w = {1, {2, 3, 4, 5, 6}};
    struct wrap *pw = &w;
    struct odd arr[2];
    struct odd *p = arr;
    arr[0] = pw->in;
    p[1] = id(*p);
    global = p[1];
    global.e = 9;
    w.in = global;
    struct odd t = 1 ? pw->in : *p;
    static struct odd s;
    s = pick(pw, 1);
    struct odd u = pick(&w, 0);
    return global.a + global.b + global.c + global.d + w.in.e + arr[1].d +
           s.e + t.b + u.c;
}
EOF

try_ 22 << EOF
struct values { int a; int b; int c; int d; int e; };
struct values source = {1, 2, 3, 4, 5};
struct values read_through(const struct values *p) { return *p; }
int main(void) {
    struct values arr[2] = {{0}};
    struct values *p = arr;
    struct values q = {6, 7, 8, 9, 10};
    struct values r;
    int first;
    *p = source;
    *(p + 1) = q;
    r = q = *p;
    first = (*p).e + p[1].a;
    r = read_through(&arr[1]);
    return first + r.e - q.a - p->c + source.e;
}
EOF

# Selecting a member of, or taking the address within, a record reached through
# a pointer reads the object in place. Copying the whole record out first cost
# instructions in proportion to its size, seconds for these 64 KiB records.
try_ 44 << EOF
struct big { char a[65536]; int m; struct { int x; int y; } in; };
struct big store;
int get_m(struct big *q) { return (*q).m; }
int get_a(struct big *q) { return (*q).a[3]; }
int *addr_m(struct big *q) { return &(*q).m; }
int main(void) {
    struct big *q = &store;
    store.a[3] = 5;
    store.m = 10;
    (*q).m++;
    ++(*q).in.y;
    (*q).in.x = 2;
    *addr_m(q) += 1;
    (void) *q;
    *q;
    return get_m(q) + get_a(q) + (*q).in.x + (*q).in.y + ((*q).in).x * 10 +
           (*(q + 0)).a[3] - 1;
}
EOF

# A record read through a pointer and then used whole is copied from the object
# it designates, once, into whatever it initializes or is assigned to.
try_output 0 "1 2 3 4 5 6 9 8 7" << EOF
struct in { int x; int y; };
struct rec { char tag; struct in in; int m; };
struct in make_in(int x) { struct in v = {x, x + 1}; return v; }
int sum(struct in v) { return v.x + v.y; }
int main(void) {
    struct rec a = {0, {1, 2}, 3}, b = {0, {7, 8}, 9};
    struct rec *p = &a, *q = &b;
    struct rec pair[2] = {*p, *q};
    struct in in = (*p).in;
    struct in picked = 0 ? (*p).in : q->in;
    struct rec c;
    c = 1 ? *p : *q;
    printf("%d %d %d ", pair[0].in.x, in.y, c.m);
    *p = *q;
    q->m = 4;
    *q = (*q, *q);
    printf("%d %d %d ", q->m, sum(p->in) - 10, (make_in(5), make_in(6)).x);
    printf("%d %d %d", (*p).m, picked.y, (1 ? *p : *q).in.x);
    return 0;
}
EOF

# A dereferenced call result, array name or double pointer is a whole record
# too. `*get(1)` applied the star to the function name, and `*table` on an array
# of records had no pointer to read through, so neither could be copied.
try_ 1 << EOF
struct S { int a; char b; long long c; };
struct S table[2] = { { 1, 2, 3 }, { 4, 5, 6 } };
struct S *get(int i) { return &table[i]; }
struct S *ptab = table;
struct S **pp = &ptab;
int take(struct S s) { return s.a * 100 + s.b * 10 + (int) s.c; }
struct S ret1(void) { return *get(1); }
struct S ret2(void) { return *table; }
struct S ret3(void) { return **pp; }
int main(void) {
    struct S s;
    int r = 0;
    s = *get(1);
    r += (s.a == 4 && s.b == 5 && s.c == 6);
    struct S t = *table;
    r += (t.a == 1 && t.c == 3) * 2;
    struct S s6 = **pp;
    r += (s6.a == 1 && s6.b == 2) * 4;
    r += (take(*get(0)) == 123) * 8;
    r += (take(*table) == 123 && take(**pp) == 123) * 16;
    r += (ret1().a == 4 && ret2().b == 2 && ret3().c == 3) * 32;
    s = *table;
    r += (s.a == 1) * 64;
    s = **pp;
    r += (s.b == 2) * 128;
    struct S u = *get(1);
    r += (u.c == 6) * 256;
    t = *(table + 1);
    r += (t.a == 4) * 512;
    return r == 1023;
}
EOF

# A unary star on an array name reads or stores its first element, whatever the
# element: read_lvalue() took `*a = v` for an assignment to the array itself,
# `**ptrs` read an int-sized word of the first pointer, and `**rows` loaded the
# first row as if it were a pointer.
try_output 0 "9 9 1 x 12 6 6 8 7 3 7" << EOF
typedef int *ip;
struct S { int a; char b; long long c; };
int ga[3];
void fill(int a[4], int n) { *a = n; *a += 1; }
void fill2(int a[][2]) { **a = 3; }
int main(void) {
    int a[3] = { 1, 2, 3 };
    long long la[2];
    char buf[4];
    int rows[2][2];
    int x = 0, y = 5;
    ip tps[2];
    int *ptrs[2];
    int loc[4];
    int m[2][2];
    struct S t[2], s = { 1, 2, 3 };
    *a += 2;
    (*a) *= 2;
    *(a) -= 1;
    x = *a = 9;
    *la = 1LL << 40;
    *buf = 'x';
    **rows = 4;
    **rows *= 3;
    *tps = &y;
    *ptrs = &y;
    **ptrs += 1;
    *ga = 8;
    fill(loc, 6);
    fill2(m);
    *t = s;
    (*t).a = 5;
    printf("%d %d %d %c %d ", *a, x, (int) (*la >> 40), *buf, **rows);
    printf("%d %d %d %d %d %d", **tps, **ptrs, *ga, *loc, **m, (*t).a + t->b);
    return 0;
}
EOF
try_compile_error_message "assignment to expression with array type" << EOF
int main(void) { int rows[2][2], r[2]; *rows = r; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int main(void) { const int a[2] = {1, 2}; *a = 3; return 0; }
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
try_ 0 << EOF
int a = 2, b = 3, c = 5, d = 7;
int *(*p)[2] = (int *[2][2]){{&a, &b}, {&c, &d}};
int main(void) {
    return *p[1][0] != 5 || *p[0][1] != 3;
}
EOF
try_ 0 << EOF
int values[2][3] = {{2, 3, 5}, {7, 11, 13}};
int *(*rows)[2] = (int *[1][2]){{&values[0][1], &values[1][2]}};
int main(void) {
    int first = *rows[0][0];
    int second = *rows[0][1];
    return first != 3 || second != 13;
}
EOF
try_ 0 << EOF
int values[2][3] = {{2, 3, 5}, {7, 11, 13}};
int *pointers[1] = {&values[1][1]};
int main(void) { return *pointers[0] != 11; }
EOF
try_ 0 << EOF
int values[2][3] = {{2, 3, 5}, {7, 11, 13}};
int (*next_row)[3] = &values[0] + 1;
int main(void) { return next_row[0][2] != 13; }
EOF
try_ 0 << EOF
int values[2][3] = {{2, 3, 5}, {7, 11, 13}};
struct holder { int (*row)[3]; };
struct holder value = {&values[0] + 1};
int main(void) { return value.row[0][1] != 11; }
EOF
try_ 0 << EOF
int main(void) {
    int a = 2, b = 3, c = 5, d = 7;
    int *data[2][2] = {{&a, &b}, {&c, &d}};
    int *(*p)[2] = data;
    return *p[1][0] != 5 || *p[0][1] != 3;
}
EOF

# Parentheses around a pointer declarator only group it, in objects, members,
# parameters and typedefs; `*p` on a pointer to a row of pointers is that row.
try_ 78 << EOF
struct M { int v; };
struct M m0 = {7};
struct M *mp = &m0;
struct M *(*gmpp) = &mp;
int gx = 5;
int (*gp) = &gx, (**gppp) = &gp;
int *const (*gcp) = &gp;
typedef int (*int_ptr), *(**int_ptr_ptr);
struct holder { int (*m); int (**mm); struct M *(*self); };
int sum(int (*q), int (**qq), int *(*rows)[2]) { return *q + **qq + *(*rows)[1]; }
int main(void) {
    int x = 3, y = 4;
    int (*p) = &x, (*q) = &y;
    int (**pp) = &p;
    int (*arr[2]) = {&x, &y};
    int *rows[2] = {&y, &x};
    int *(*rp)[2] = &rows;
    int_ptr ip = &y;
    int_ptr_ptr ipp = &pp;
    struct M *(*mpp) = &mp;
    struct holder h;
    h.m = &x;
    h.mm = &p;
    h.self = &mp;
    return *p + *q + **pp + *arr[1] + *(*rp)[0] + *(*rp)[1] + (*mpp)->v +
           (*gmpp)->v + *gp + **gppp + **gcp + *h.m + **h.mm +
           (*h.self)->v + sum(ip, pp, rp) + ***ipp +
           (sizeof(arr) == 2 * sizeof(int *)) + (sizeof(*rp) == sizeof(rows));
}
EOF

# A parameter declared as a function is adjusted to a pointer to it (C99
# 6.7.5.3p8), and in a parameter a typedef name or void in parentheses is an
# abstract function declarator (6.7.5.3p11).
try_ 0 << EOF
typedef int T;
int twice(int x) { return 2 * x; }
int seven(void) { return 7; }
int x9(T v) { return v + 9; }
char *name(void) { return "ab"; }
int apply(int fn(int), int v) { return fn(v); }
int apply2(int (fn)(int), int (v)) { return fn(v); }
int call0(int thunk(void)) { return thunk(); }
int k(int (T));
int k(int f(T)) { return f(1); }
int k2(int (void), int *(int));
int k2(int f(void), int *g(int)) { return f(); }
int strcall(char *s(void)) { return s()[1]; }
int main(void)
{
    return apply(twice, 3) + apply2(twice, 4) + call0(seven) + k(x9) +
           k2(seven, 0) + strcall(name) != 6 + 8 + 7 + 10 + 7 + 'b';
}
EOF
try_compile_error << EOF
typedef int T;
int k(int (T));
int k(int f(void)) { return f(); }
int main(void) { return 0; }
EOF
# A parenthesized name without a star declares the name itself (C99 6.7.5p6).
try_ 0 << EOF
typedef int T;
int (g) = 3;
int (ga)[2] = {1, 2};
int *(gp) = &g;
int (a1), (b1) = 2;
struct S { int (m); char (name)[4]; };
int (f)(void) { return 1; }
int h(int (*), int (y));
int h(int *p, int y) { return *p + y; }
int main(void) {
    static int (s2) = 4;
    int (s) = 5;
    int (a)[3] = {1, 2, 3};
    T (t) = 6;
    struct S r;
    int sum = 0;
    r.m = 5;
    for (int (i) = 0; i < 3; i++)
        sum += a[i];
    return g + ga[1] + *gp + b1 + r.m + f() + h(&g, 1) + s2 + s + sum + t +
           sizeof(r.name) != 45;
}
EOF
try_compile_error_message "assignment of read-only variable" << EOF
int main(void) { int x = 3; int (*const q) = &x; q = 0; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int main(void) { int x = 3, *p = &x; int *const (*cp) = &p; *cp = 0; return 0; }
EOF
try_ 1 << EOF
int main(void) {
    int (**rows)[2] = (int (*[])[2]){0, 0};
    return rows[0] == 0 && rows[1] == 0;
}
EOF
try_ 13 << EOF
int main(void) {
    int first[2] = {3, 4};
    int second[2] = {9, 10};
    int (**rows)[2] = (int (*[])[2]){&first, &second};
    return rows[0][0][1] + rows[1][0][0];
}
EOF
try_ 7 << EOF
int main(void) {
    int values[2] = {3, 7};
    int *items[1] = {values};
    int **p = items;
    return p[0][1];
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

# Multi-element record arrays must retain every element and member; this is a
# full aggregate-initialization check rather than an observation of element 0.
try_ 10 << EOF
struct point { int x; int y; };
int main() {
    struct point pts[2] = { {1, 2}, {3, 4} };
    return pts[0].x + pts[0].y + pts[1].x + pts[1].y;
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

# The element type of a file-scope array compound literal is a full type name:
# qualifiers, several keywords, enum tags and qualified typedefs.
try_flags 231 --std=c99 << EOF
enum E { A = 1, B = 2 };
typedef unsigned char byte;
struct S { int x; };
const int *global_const_compound = (const int[]){1, 2};
unsigned char *global_uchar_compound = (unsigned char[]){1, 200};
long long *global_llong_compound = (long long[2]){1, 0x100000000LL};
volatile short *global_short_compound = (volatile short[3]){1, 2, 3};
const long *global_long_compound = (long const[]){7, 8};
enum E *global_enum_compound = (enum E[]){A, B};
const byte *global_byte_compound = (const byte[]){9, 10};
const struct S *global_record_compound = (const struct S[]){{4}, {5}};
int main(void) {
    return global_const_compound[1] + global_uchar_compound[1] +
           (int) (global_llong_compound[1] >> 32) + global_short_compound[2] +
           global_long_compound[1] + global_enum_compound[1] +
           global_byte_compound[1] + global_record_compound[1].x;
}
EOF
try_compile_error << EOF
long long *global_llong_mismatch = (int[2]){1, 2};
int main(void) { return 0; }
EOF
try_compile_error << EOF
unsigned char *global_sign_mismatch = (signed char[]){1, 2};
int main(void) { return 0; }
EOF
try_ 5 << EOF
int global_pointer_array_rows[2][2] = {{1, 2}, {3, 4}};
int (*global_pointer_array)[2] = global_pointer_array_rows;
int main(void) {
    return global_pointer_array[1][0] + global_pointer_array[0][1];
}
EOF
try_ 13 << EOF
int global_pointer_array_cubes[2][2][2] =
    {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
int (*global_pointer_cube)[2][2] = global_pointer_array_cubes;
int main(void) {
    return global_pointer_cube[1][0][0] + global_pointer_cube[0][1][1] * 2;
}
EOF
try_ 6 << EOF
int global_pointer_array_rectangles[2][2][3] =
    {{{1, 2, 3}, {4, 5, 6}}, {{7, 8, 9}, {10, 11, 12}}};
int (*global_pointer_rectangle)[2][3] = global_pointer_array_rectangles;
int main(void) {
    return global_pointer_rectangle[0][1][2];
}
EOF
try_ 120 << EOF
typedef int global_typedef_row_t[2];
typedef global_typedef_row_t global_typedef_plane_t[3];
typedef global_typedef_plane_t global_typedef_cube_t[2];
typedef global_typedef_cube_t global_typedef_hyper_t[2];
typedef global_typedef_hyper_t global_typedef_hyper_alias_t;
int main(void) {
    global_typedef_hyper_alias_t values = {0};
    values[1][1][2][1] = 24;
    return sizeof(global_typedef_hyper_alias_t) + values[1][1][2][1];
}
EOF
try_compile_error << EOF
typedef int global_typedef_four_t[1][1][1][1];
typedef global_typedef_four_t global_typedef_five_t[1];
int main(void) { return 0; }
EOF
try_ 1 << EOF
int **global_compound_pointer_array = (int *[]){0, 0};
int main(void) {
    return global_compound_pointer_array[0] == 0 &&
           global_compound_pointer_array[1] == 0;
}
EOF
try_ 1 << EOF
int **global_compound_bounded_pointer_array = (int *[2]){0, 0};
int main(void) {
    return global_compound_bounded_pointer_array[0] == 0 &&
           global_compound_bounded_pointer_array[1] == 0;
}
EOF
try_ 0 << EOF
typedef int global_compound_row[2];
global_compound_row *global_compound_typedef_rows =
    (global_compound_row[]){ {3, 8}, {4, 9} };
int main(void) {
    return global_compound_typedef_rows[1][0] != 4 ||
           global_compound_typedef_rows[0][1] != 8;
}
EOF
try_ 0 << EOF
typedef int global_compound_bounded_row[2];
global_compound_bounded_row *global_compound_bounded_typedef_rows =
    (global_compound_bounded_row[2]){ {5, 1}, {6, 7} };
int main(void) {
    return global_compound_bounded_typedef_rows[1][1] != 7 ||
           global_compound_bounded_typedef_rows[0][0] != 5;
}
EOF
try_ 1 << EOF
int (**global_compound_pointer_rows)[2] = (int (*[])[2]){0, 0};
int main(void) {
    return global_compound_pointer_rows[0] == 0 &&
           global_compound_pointer_rows[1] == 0;
}
EOF
try_ 0 << EOF
int global_compound_source_rows[2][2] = {{1, 2}, {3, 4}};
int (**global_compound_initialized_pointer_rows)[2] =
    (int (*[])[2]){global_compound_source_rows,
                    global_compound_source_rows + 1,
                    global_compound_source_rows - 0};
int main(void) {
    return global_compound_initialized_pointer_rows[1][0][1] != 4 ||
           global_compound_initialized_pointer_rows[0][0][0] != 1 ||
           global_compound_initialized_pointer_rows[2][1][0] != 3;
}
EOF
try_ 0 << EOF
int global_pointer_slot_rows[2][2] = {{1, 2}, {3, 4}};
int (*global_pointer_slots[3])[2] = {
    global_pointer_slot_rows, global_pointer_slot_rows + 1,
    global_pointer_slot_rows
};
int (**global_pointer_slot_rows_view)[2] = global_pointer_slots;
int main(void) {
    return global_pointer_slot_rows_view[1][0][1] != 4 ||
           global_pointer_slot_rows_view[2][1][0] != 3;
}
EOF
try_ 0 << EOF
int global_compound_bounded_source_rows[2][2] = {{5, 6}, {7, 8}};
int (**global_compound_bounded_initialized_pointer_rows)[2] =
    (int (*[2])[2]){global_compound_bounded_source_rows + 1,
                    global_compound_bounded_source_rows};
int main(void) {
    return global_compound_bounded_initialized_pointer_rows[0][0][0] != 7 ||
           global_compound_bounded_initialized_pointer_rows[1][0][1] != 6;
}
EOF
try_ 0 << EOF
int global_compound_address_value = 12;
int **global_compound_address_values =
    (int *[]){&global_compound_address_value};
int main(void) { return **global_compound_address_values != 12; }
EOF
try_ 0 << EOF
int global_compound_address_elements[2] = {10, 13};
int **global_compound_address_element_values =
    (int *[]){&global_compound_address_elements[1]};
int main(void) {
    int *element = global_compound_address_element_values[0];
    return *element != 13;
}
EOF
try_ 0 << EOF
int global_compound_pointer_decay_values[2] = {3, 5};
int *global_compound_pointer_decay_rows[1] =
    {global_compound_pointer_decay_values};
int ***global_compound_pointer_decay =
    (int **[]){global_compound_pointer_decay_rows};
int ***global_compound_pointer_address =
    (int **[]){&global_compound_pointer_decay_rows[0]};
int main(void) {
    int **row = global_compound_pointer_decay[0];
    int **addressed_row = global_compound_pointer_address[0];
    int *element = row[0];
    int *addressed_element = addressed_row[0];
    return element[1] != 5 || addressed_element[1] != 5;
}
EOF
try_ 0 << EOF
int global_compound_address_matrix[2][3] = {{2, 3, 5}, {7, 11, 13}};
int (**global_compound_address_matrix_rows)[3] =
    (int (*[])[3]){&global_compound_address_matrix[1]};
int main(void) {
    int (*row)[3] = global_compound_address_matrix_rows[0];
    return row != &global_compound_address_matrix[1];
}
EOF
try_ 0 << EOF
int global_compound_address_offset_values[3] = {17, 19, 23};
int **global_compound_address_offset_pointers =
    (int *[]){&global_compound_address_offset_values[0] + 2,
              &global_compound_address_offset_values[2] - 1};
int main(void) {
    int *forward = global_compound_address_offset_pointers[0];
    int *backward = global_compound_address_offset_pointers[1];
    return *forward != 23 || *backward != 19;
}
EOF
try_ 0 << EOF
int global_address_row_matrix[2][3] = {{2, 3, 5}, {7, 11, 13}};
int (*global_address_row)[3] = &global_address_row_matrix[1];
int *global_address_matrix_element = &global_address_row_matrix[1][2];
int (*global_address_decay_row)[3] = global_address_row_matrix + 1;
struct global_address_matrix_holder { int matrix[2][3]; };
struct global_address_matrix_holder global_address_matrix_object =
    {{{29, 31, 37}, {41, 43, 47}}};
int *global_address_matrix_member =
    &global_address_matrix_object.matrix[1][2];
int main(void) {
    return global_address_row != &global_address_row_matrix[1] ||
           global_address_decay_row != &global_address_row_matrix[1] ||
           *global_address_matrix_element != 13 ||
           *global_address_matrix_member != 47;
}
EOF
try_ 0 << EOF
int global_address_cube[2][2][2][2] = {
    {{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
    {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}
};
int *global_address_cube_leaf = &global_address_cube[1][0][1][1];
int main(void) { return *global_address_cube_leaf != 37; }
EOF
try_ 0 << EOF
int global_address_plane_cube[2][2][2][2] = {
    {{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
    {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}
};
int (*global_address_plane)[2][2] =
    &global_address_plane_cube[0][1] + 1;
int main(void) {
    return global_address_plane != &global_address_plane_cube[1][0] ||
           global_address_plane[0][1][1] != 37;
}
EOF
try_ 1 << EOF
int (**global_compound_bounded_pointer_rows)[2] = (int (*[2])[2]){0, 0};
int main(void) {
    return global_compound_bounded_pointer_rows[0] == 0 &&
           global_compound_bounded_pointer_rows[1] == 0;
}
EOF
try_ 1 << EOF
int (**global_compound_constant_bound_pointer_rows)[2] =
    (int (*[1 + 1])[2]){0, 0};
int main(void) {
    return global_compound_constant_bound_pointer_rows[0] == 0 &&
           global_compound_constant_bound_pointer_rows[1] == 0;
}
EOF
try_ 1 << EOF
int (***global_compound_nested_pointer_rows)[2] =
    (int (**[])[2]){0, 0};
int main(void) {
    return global_compound_nested_pointer_rows[0] == 0 &&
           global_compound_nested_pointer_rows[1] == 0;
}
EOF
try_ 1 << EOF
struct global_compound_row_record { int value; };
struct global_compound_row_record (**global_compound_record_rows)[2] =
    (struct global_compound_row_record (*[])[2]){0, 0};
int main(void) {
    return global_compound_record_rows[0] == 0 &&
           global_compound_record_rows[1] == 0;
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

# Every declarator of a declaration takes that first element, not only the first
# declarator; a later one stored the literal's address.
try_ 105 << EOF
int main() {
    int a = 1, b = (int[]){100, 200}, c = (int[]){4};
    return a + b + c;
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

# A file-scope compound literal has static storage, so its address is an address
# constant for a file-scope pointer (C99 6.5.2.5p6).
try_ 0 << EOF
struct P { int x, y; };
typedef struct P PT;
int *gp = &(int){8};
long long *gl = &(long long){0x100000002LL};
char *gc = &(char){'z'};
unsigned long *gu = &(unsigned long){5};
struct P *ps = &(struct P){3, 4};
PT *pt = &(PT){.y = 6};
int *(*gpp) = &(int *){0};
void *gv = &(int){9};
int main(void)
{
    *gp += 1;
    return *gp != 9 || (int) (*gl >> 32) != 1 || (int) *gl != 2 || *gc != 'z' ||
           *gu != 5 || ps->x != 3 || ps->y != 4 || pt->y != 6 || pt->x != 0 ||
           *gpp != 0 || *(int *) gv != 9;
}
EOF
try_compile_error << EOF
int *gp = &(char){8};
int main(void) { return 0; }
EOF

# Compound literals of long types, spelled with keywords, at both scopes. The
# sole initializer is any assignment expression, not just a single token.
try_ 43 << EOF
long g = (long){7};
unsigned long h = (unsigned long){8};
long int i = (long int){9};
long long ll = (long long){-2};
int main()
{
    static long s = (long){3};
    long a = (long){7};
    unsigned long b = (unsigned long){7};
    long int c = (long int){-7};
    long long d = (long long){5} + (unsigned long long){1};
    long *p = &(long){4};
    *p += 1;
    return g + h + i + ll + s + a + b + c + d + *p;
}
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
items 4 "int value = 1; if (value++, value == 2) return value + 2; return 0;"
items 2 "if (0 ? 1 : 0) return 1; return 2;"

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

# Block-scope struct, union and enum objects take the same storage classes and
# qualifiers as scalar ones, and an untagged definition may declare objects.
try_ 12 << EOF
struct S { int a; };
struct S s = { 5 };
union U { int a; char c; };
union U u = { 7 };
enum E { E1 = 1, E2 };
enum E ge = E2;
int main(void)
{
    extern struct S s;
    register struct S r;
    register union U ru = u;
    extern union U u, *up;
    register enum E re = E1;
    extern enum E ge;
    r.a = s.a + ru.a - re - ge + 3;
    return r.a;
}
union U *up = &u;
EOF
try_ 4 << EOF
struct S { int a; };
struct S make(int v) { struct S s; s.a = v; return s; }
int main(void)
{
    const struct S cs = { 3 }, *cp = &cs;
    static struct S ss = { 4 }, *sp = &ss;
    struct S a = make(5), b = a, arr[2] = { { 1 }, { 2 } };
    volatile enum { V1 = 2 } ve = V1, *vp = &ve;
    struct { struct S inner; int k; } nest = { { 8 }, 9 }, *np = &nest;
    union { int i; char c; } un = { 0 };
    static struct { int z; } st = { 1 };
    sp->a++;
    np->k += st.z;
    return cp->a + ss.a + a.a + b.a + arr[1].a + *vp + nest.inner.a + nest.k +
           un.i - 36 + sizeof(nest) / sizeof(int) - 2;
}
EOF

# A member may define the record it has, tagged or untagged, at any scope; a
# nested tag belongs to the scope of the outer record. A union may hold a struct
# with a flexible array member.
try_ 80 << EOF
struct outer {
    struct inner { int a; char c; } in;
    union { short s; struct { char x, y, z; } t; } u;
    struct { int x; } pts[3], *first;
    int b;
} file_outer;
struct { struct { int a; } in; int b; } untagged_file;
union { struct { int lo, hi; } pair; int raw[2]; } file_union;
typedef struct { struct { int x, y; } p[2]; union { int i; char c[5]; } u; } nested_t;
int main(void) {
    struct inner reused;
    nested_t t;
    struct { struct { int a; } in; int b; } n;
    struct block_outer { struct block_inner { char q; } arr[4]; } bo;
    struct block_inner bi;
    typedef struct { union { int v; } u[2]; } block_t;
    block_t bt;
    file_outer.in.a = 1;
    file_outer.u.t.z = 2;
    file_outer.pts[2].x = 3;
    file_outer.first = file_outer.pts;
    reused.c = 4;
    untagged_file.in.a = 5;
    file_union.pair.hi = 6;
    t.p[1].y = 7;
    t.u.c[4] = 8;
    n.in.a = 9;
    bo.arr[3].q = 10;
    bi.q = 11;
    bt.u[1].v = 12;
    return file_outer.in.a + file_outer.u.t.z + file_outer.first[2].x +
           reused.c + untagged_file.in.a + file_union.raw[1] + t.p[1].y +
           t.u.c[4] + n.in.a + bo.arr[3].q + bi.q + bt.u[1].v +
           (sizeof(nested_t) == 4 * sizeof(int) + 8) + (sizeof(bo) == 4);
}
EOF
try_ 3 << EOF
struct flexible_row { int count; int values[]; };
union flexible_holder { struct flexible_row row; int count; };
int main(void) {
    union { struct flexible_row row; char tag; } local;
    local.row.count = 3;
    return local.row.count + sizeof(union flexible_holder) - sizeof(int);
}
EOF
try_compile_error << EOF
struct { struct { int a; }; int b; } anonymous_member;
int main(void) { return 0; }
EOF
try_compile_error_message "ordinary identifier conflicts with typedef name" << EOF
struct S { int a; };
int main(void) { typedef int T; struct S T; return 0; }
EOF
try_compile_error_message "ordinary identifier conflicts with typedef name" << EOF
int main(void) { typedef int T; struct S { int a; } x, T; return 0; }
EOF
try_compile_error_message "ordinary identifier conflicts with typedef name" << EOF
enum E { A };
int main(void) { typedef int T; enum E T; return 0; }
EOF
try_compile_error_message "ordinary identifier conflicts with typedef name" << EOF
int main(void) { typedef int T; union { int a; } T; return 0; }
EOF

# An enumeration constant is an ordinary identifier too: it conflicts with a
# typedef of its block and hides an outer one.
try_compile_error_message "typedef name conflicts with an ordinary identifier" << EOF
int main(void) { enum { A }; typedef int A; return 0; }
EOF
try_compile_error_message "identifier redeclared as a different kind of symbol" << EOF
int main(void) { typedef int T; enum { T = 3 }; return 0; }
EOF
try_ 4 << EOF
typedef int T;
int main(void) { enum { T = 3 }; { T: return T + 1; } }
EOF

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

# A for initializer declaration is an ordinary declaration: its initializers are
# assignment expressions and each declarator keeps the resolved base type.
try_ 23 << EOF
struct P { int x, y; };
int main(void)
{
    int j, k, n = 0;
    for (int i = j = 2; i < 4; i++)
        n += i + j;
    for (int a = 1, b = k = 3, c = a ? b : 0; a < 2; a++)
        n += a + b + c + k;
    for (unsigned u = 0, v = 0; u < 1; u++)
        v = -1, n += v > 0;
    for (struct P p = { 1, 2 }, q = p; p.x < 2; p.x++)
        n += q.y;
    for (int m = 0, *pm = &m; m < 1; m++)
        n += *pm + 1;
    return n;
}
EOF
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
items 3 "int i = 0; while (i++, i < 3) {} return i;"
items 3 "int i = 0; for (; i++, i < 3;) {} return i;"
items 3 "int i = 0; do {} while (i++, i < 3); return i;"
items 9 "int i, j; for (i = 0, j = 0; i < 3; i++, j += 2) {} return i + j;"
items 9 "int i = 0, j = 0; for (; i < 3; (i++, j += 2)) {} return i + j;"
items 3 "int i = 0; for (; i < 3; (i++)) {} return i;"
try_ 3 << EOF
int bump(int *value) { *value = *value + 1; return *value; }
int main(void) {
    int value = 0;
    for (; value < 3; bump(&value)) {}
    return value;
}
EOF
items 9 "int i = 0, j = 0; for ((i = 0, j = 0); i < 3; i++) j += 2; return i + j;"
try_ 3 << EOF
int reset(int *value) { *value = 0; return 0; }
int main(void) {
    int value = 7;
    for (reset(&value); value < 3; value++) {}
    return value;
}
EOF
try_compile_error << EOF
int main(void) { int i = 0; for (const (i = 0); i < 1; i++) {} return i; }
EOF
try_compile_error << EOF
int main(void) { int i = 0; for (static (i = 0); i < 1; i++) {} return i; }
EOF
try_ 3 << EOF
enum loop_mode { LOOP_MODE_ZERO };
int main(void) {
    int count = 0;
    for (enum loop_mode mode = LOOP_MODE_ZERO; count < 3; count++)
        count += mode;
    return count;
}
EOF
try_ 3 << EOF
int main(void) {
    enum local_loop_mode { LOCAL_LOOP_MODE_ZERO };
    int count = 0;
    for (enum local_loop_mode mode = LOCAL_LOOP_MODE_ZERO; count < 3; count++)
        count += mode;
    return count;
}
EOF
try_compile_error << EOF
int main(void) { for (enum unknown_loop_mode value = 0; value < 1; value++) {} }
EOF
try_ 3 << EOF
struct loop_record { int value; };
union loop_union { int value; char byte; };
int main(void) {
    int count = 0;
    for (struct loop_record item; count < 3; count++) item.value = count;
    count = 0;
    for (union loop_union item; count < 3; count++) item.value = count;
    return count;
}
EOF
try_ 3 << EOF
int main(void) {
    struct local_loop_record { int value; };
    union local_loop_union { int value; char byte; };
    int count = 0;
    for (struct local_loop_record item; count < 3; count++) item.value = count;
    count = 0;
    for (union local_loop_union item; count < 3; count++) item.value = count;
    return count;
}
EOF
try_compile_error << EOF
int main(void) { for (struct unknown_loop_record item; 0; ) {} }
EOF
try_compile_error << EOF
union loop_kind_union { int value; };
int main(void) { for (struct loop_kind_union item; 0; ) {} }
EOF
items 2 "int i = 0; while (i < 2 ? 1 : 0) i++; return i;"
items 2 "int i = 0; do i++; while (i < 2 ? 1 : 0); return i;"
items 2 "int i = 0; for (; i < 2 ? 1 : 0; i++) {} return i;"

# A loop whose back edge leads to a block laid out before the branch: neither
# arm of the conditional falls through, so both need an explicit jump.
try_ 3 << EOF
int count(void) {
    int n = 0;
    do {
        n++;
        if (n == 3)
            break;
    } while (1);
    return n;
}
int main(void) { return count(); }
EOF

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

# Identifier-led function calls must use the ordinary full-expression path:
# their results may participate in arithmetic and comma sequencing instead of
# being forced to be a standalone statement by declaration dispatch.
try_ 12 << EOF
int bump(int *value) { *value += 1; return *value; }
int scale(int value) { return value * 3; }
int main(void)
{
    int value = 0;
    bump(&value), bump(&value);
    return scale(value) + bump(&value) + value;
}
EOF
try_ 0 << EOF
int function_designator_statement(void) { return 1; }
int main(void)
{
    function_designator_statement;
    return 0;
}
EOF
try_ 0 << EOF
int function_designator_condition(void) { return 1; }
int main(void)
{
    if (function_designator_condition)
        return 0;
    return 1;
}
EOF
try_ 0 << EOF
int function_designator_logical(void) { return 1; }
int main(void)
{
    if (function_designator_logical && 1)
        return 0;
    return 1;
}
EOF
try_ 0 << EOF
int function_designator_or(void) { return 1; }
int main(void)
{
    if (function_designator_or || 0)
        return 0;
    return 1;
}
EOF
try_ 0 << EOF
int function_designator_not(void) { return 1; }
int main(void)
{
    if (!function_designator_not)
        return 1;
    return 0;
}
EOF
try_ 0 << EOF
int function_designator_equality(void) { return 1; }
int main(void) { return function_designator_equality == 0 ? 1 : 0; }
EOF
try_ 0 << EOF
int function_designator_inequality(void) { return 1; }
int main(void) { return function_designator_inequality != 0 ? 0 : 1; }
EOF
try_compile_error << EOF
int incompatible_equality_left(int value) { return value; }
int incompatible_equality_right(void) { return 0; }
int main(void) {
    return incompatible_equality_left == incompatible_equality_right;
}
EOF
try_compile_error << EOF
int invalid_function_relational(int value) { return value; }
int main(void) {
    return invalid_function_relational < invalid_function_relational;
}
EOF
try_ 8 << EOF
typedef int (*function_selector_t)(int);
int increment_selected(int value) { return value + 1; }
int decrement_selected(int value) { return value - 1; }
int select_and_call(int select)
{
    function_selector_t callback =
        select ? increment_selected : decrement_selected;
    return callback(4);
}
int main(void) { return select_and_call(1) + select_and_call(0); }
EOF
try_ 5 << EOF
typedef int (*nullable_selector_t)(int);
int nullable_increment(int value) { return value + 1; }
int main(void)
{
    nullable_selector_t callback = 1 ? nullable_increment : 0;
    return callback ? callback(4) : 0;
}
EOF
try_ 5 << EOF
int direct_increment(int value) { return value + 1; }
int direct_decrement(int value) { return value - 1; }
int main(void) { return (1 ? direct_increment : direct_decrement)(4); }
EOF
try_ 5 << EOF
int direct_nullable_increment(int value) { return value + 1; }
int main(void) { return (1 ? direct_nullable_increment : 0)(4); }
EOF
try_ 5 << EOF
int cast_nullable_increment(int value) { return value + 1; }
int main(void) { return (1 ? cast_nullable_increment : (int)0)(4); }
EOF
try_ 5 << EOF
int sizeof_nullable_increment(int value) { return value + 1; }
int main(void) {
    return (1 ? sizeof_nullable_increment : sizeof(int) - 4)(4);
}
EOF
try_ 5 << EOF
int narrow_nullable_increment(int value) { return value + 1; }
int main(void) {
    return (1 ? narrow_nullable_increment : (unsigned char)256)(4);
}
EOF
try_ 5 << EOF
int wide_nullable_increment(int value) { return value + 1; }
int main(void) {
    return (1 ? wide_nullable_increment : (int)4294967296LL)(4);
}
EOF
try_compile_error << EOF
int void_cast_increment(int value) { return value + 1; }
int main(void) { return (1 ? void_cast_increment : (void)0)(4); }
EOF
try_compile_error << EOF
int bool_cast_increment(int value) { return value + 1; }
int main(void) {
    return (1 ? bool_cast_increment : (_Bool)4294967296LL)(4);
}
EOF
try_compile_error << EOF
typedef int (*invalid_selector_t)(int);
int invalid_increment(int value) { return value + 1; }
int main(void) {
    invalid_selector_t callback = 1 ? invalid_increment : 1;
    return callback(0);
}
EOF
try_compile_error << EOF
typedef int (*incompatible_selector_t)(int);
int incompatible_increment(int value) { return value + 1; }
int incompatible_wrong(void) { return 0; }
int main(void) {
    incompatible_selector_t callback =
        1 ? incompatible_increment : incompatible_wrong;
    return callback(0);
}
EOF

# Assignment expressions yield their stored value in argument and explicitly
# parenthesized comma-expression contexts.
try_ 17 << EOF
int twice(int value) { return value * 2; }
int main(void) {
    int value = 1;
    return twice(value = 5) + (value = 6, value + 1);
}
EOF
try_ 3 << EOF
int main(void) {
    int first = 0, second = 0;
    return first = second = 3;
}
EOF
try_ 9 << EOF
int main(void) {
    int value = 0;
    return 1 ? value = 9 : value = 2;
}
EOF
try_ 1 << EOF
struct flags { unsigned int value : 3; };
int main(void) {
    struct flags flags = {0};
    return flags.value = 9;
}
EOF
try_ 1 << EOF
struct value { unsigned char byte; };
int main(void) {
    struct value value = {0};
    return value.byte = 257;
}
EOF
try_ 7 << EOF
int main(void) {
    int value = 0;
    if (value = 1)
        value = 3;
    switch (value = 4) {
    case 4: return value = 7;
    default: return 0;
    }
}
EOF
try_ 7 << EOF
int main(void) {
    int value = 3;
    int *pointer = &value;
    return *pointer += 4;
}
EOF
try_ 1 << EOF
int main(void) {
    unsigned char value = 0;
    unsigned char *pointer = &value;
    return *pointer = 257;
}
EOF
try_compile_error << EOF
int main(void) {
    const int value = 0;
    const int *pointer = &value;
    return *pointer = 1;
}
EOF
try_ 13 << EOF
int main(void) {
    int values[1] = {0};
    int *pointer = values;
    return ((values[0])) = 6 + ((*pointer) = 7);
}
EOF
try_compile_error << EOF
struct point { int x; };
int main(void) {
    const struct point point = {0};
    const struct point *pointer = &point;
    return (*pointer).x = 7;
}
EOF

# A member reached through a pointer to a const record is not a modifiable
# lvalue (C99 6.5.16p2), whatever the pointer itself is: plain, parameter,
# global, typedef, const or subscripted. Assignment, compound assignment and
# increment are all rejected, including members of nested records and elements
# of array members.
try_compile_error_message "read-only location" << EOF
struct S { int a; };
int main(void) { struct S s; const struct S *p = &s; p->a = 1; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int a; };
int bump(const struct S *p) { return p->a += 2; }
int main(void) { return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int a; };
const struct S *global;
int main(void) { global->a++; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int a; };
int main(void) { struct S s; const struct S *p = &s; --p->a; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int a; };
typedef const struct S *const_s_ptr;
int main(void) { struct S s; const_s_ptr p = &s; p->a = 1; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct I { int b; };
struct S { struct I in; int arr[2]; };
int main(void) { struct S s; const struct S *const p = &s; p->in.b = 1; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int arr[2]; };
int main(void) { struct S s; const struct S *p = &s; p->arr[1] = 1; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int arr[2]; };
int main(void) { const struct S cs = {{0}}; cs.arr[1] = 1; return 0; }
EOF

try_compile_error_message "read-only location" << EOF
struct S { int *ptr; struct S *next; };
int main(void) { struct S s; const struct S *p = &s; p->next = 0; return 0; }
EOF

# Reads stay valid, and so do writes through a non-const pointer, through a
# const pointer to a non-const record, and to objects a pointer member of a
# const record reaches. A pointer member to const is itself assignable, and a
# pointer typedef naming a record tag finds the tag's members.
try_ 19 << EOF
struct I { int b; };
struct S { int a; struct I in; int arr[2]; int *ptr; const int *view; struct S *next; };
typedef struct S *s_ptr;
int read_back(const struct S *p) { return p->a + p->next->in.b; }
int main(void) {
    struct S s = {0};
    struct S *p = &s;
    struct S *const fixed = &s;
    const struct S *cp = &s;
    s_ptr alias = &s;
    s.next = &s;
    s.ptr = &s.arr[1];
    p->a = 1;
    p->in.b += 2;
    ++p->a;
    fixed->arr[0] = 3;
    fixed->a++;
    cp->next->in.b++;
    cp->next->arr[1] = 4;
    *cp->ptr += 1;
    p->view = &s.a;
    alias->a += 1;
    return read_back(cp) + *cp->view + alias->arr[0] + cp->arr[1];
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

# An eight-byte union or anonymous typedef record is still an aggregate on a
# 32-bit target, not a two-register scalar. Passing it between two scalar
# arguments must not shift them into different registers.
try_ 0 << EOF
union two_words { int a[2]; };
typedef struct { int x; int y; } anon_pair;
int take_union(int k, union two_words u, int m) {
    return k * 100 + u.a[0] * 10 + u.a[1] + m * 1000;
}
int take_anon(int k, anon_pair p, int m) {
    return k * 100 + p.x * 10 + p.y + m * 1000;
}
int main(void) {
    union two_words u;
    anon_pair p;
    u.a[0] = 3;
    u.a[1] = 4;
    p.x = 5;
    p.y = 6;
    return take_union(1, u, 2) != 2134 || take_anon(1, p, 2) != 2156;
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

# A parameter list has no trailing comma in any dialect, and a comma must
# separate each parameter, including the ellipsis, from the one before it.
try_compile_error_message "trailing comma in parameter list" << EOF
int g(int a,) { return a; }
int main(void) { return g(1); }
EOF

try_compile_error_message "trailing comma in parameter list" << EOF
int g(int a, char *b,);
int main(void) { return 0; }
EOF

try_compile_error_message "trailing comma in parameter list" << EOF
int main(void) { int (*fp)(int,); return 0; }
EOF

try_compile_error_message "trailing comma in parameter list" << EOF
int apply(int (*cb)(int,), int b) { return b; }
int main(void) { return 0; }
EOF

try_compile_error_message "trailing comma in parameter list" << EOF
int main(void) { return sizeof(int (*)(int,)); }
EOF

try_compile_error << EOF
int g(void,) { return 0; }
int main(void) { return g(); }
EOF

try_compile_error << EOF
int g(int a int b) { return a + b; }
int main(void) { return g(1, 2); }
EOF

try_compile_error << EOF
int g(int a ...) { return a; }
int main(void) { return g(1, 2); }
EOF

try_ 3 << EOF
int g(int a, int (*cb)(int, char), ...) { return a; }
int main(void) { int (*fp)(int, ...) = 0; return g(3, 0) + (fp != 0); }
EOF

# Only the keyword itself starts a void parameter list. A type name that merely
# begins with those letters is an ordinary parameter type, and an unknown one
# must be diagnosed rather than read as an empty prototype.
try_ 7 << EOF
typedef int voidptr;
int add(voidptr a, int b) { return a + b; }
int main(void) { return add(3, 4); }
EOF

try_compile_error << EOF
int unknown_parameter_type(voidx);
int main(void) { return 0; }
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

# Pointers are ordered as unsigned addresses (C99 6.5.8), also on a 32-bit
# target for an address above 0x80000000; they were compared as signed.
try_ 1 << EOF
int lt(int *a, int *b) { return a < b; }
int main(void) {
    int *high = (int *) -4, *low = (int *) 16;
    int *mid = (int *) (unsigned long) 0x90000000UL;
    int r = ((int *) -1 < (int *) 0) + (high <= low) + lt(high, low);
    if (high < low || !(mid > low))
        r++;
    return r == 0 && high > low && mid >= low && low < mid;
}
EOF

# Pointer arithmetic follows operator precedence. A pointer operand used to take
# a following "+ n" as part of itself, so p + a * b became (p + a) * b and read
# past the array, and so did p + sizeof(int) * 2.
try_output 0 "2 2 9 5 2 2 2 4 7 12 21 8 6 4 7 1 1 6" << EOF
int *id(int *p) { return p; }
int main(void) {
    int arr[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    int m[3][4];
    int (*pa)[4] = m;
    int *p = arr;
    int a = 1, b = 2;
    long off = 3;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++)
            m[r][c] = r * 10 + c;
    printf("%d %d %d ", *(p + 1 * 2), *(p + a * b), *(p + 5 - 2 + 3 * 2));
    printf("%d %d %d ", *(5 + p), *(a * b + p), *(p + a + a));
    printf("%d %d %d ", *(id(p) + a * b), *(arr + 2 * 2), *(arr + a + b * 3));
    printf("%d %d ", (*(pa + 1))[2], *(*(pa + a * 2) + 1));
    printf("%d %d %d ", *(p + sizeof(int) * 2), *(p + off * 2),
           *(p + a * b + a * b));
    int *z = arr + a * b + b % 2 + b / 2 * 5 - a * 4;
    printf("%d %d %d ", *z + 4, p + a * 0 ? 1 : 2, (int) ((char *) p + 1 - (char *) p));
    p = p + 2 * 3;
    printf("%d", *p);
    return 0;
}
EOF

# Unary operators and casts read their operand recursively without passing
# through read_expr(), so a long chain of them overflowed the stack instead of
# reaching the expression nesting limit.
try_ 7 << EOF
int main(void)
{
    int x = 7;
    return $(printf '*&%.0s' $(seq 100))x + $(printf '(int)%.0s' $(seq 100))0;
}
EOF
try_compile_error_message "Expression nesting too deep" << EOF
int main(void)
{
    int x = 3;
    return $(printf '*&%.0s' $(seq 20000))x;
}
EOF
try_compile_error_message "Expression nesting too deep" << EOF
int main(void)
{
    int x = 3;
    return $(printf '(int)%.0s' $(seq 20000))x;
}
EOF

# The address of a member has the member's pointer type, so arithmetic on it
# advances by whole members.
try_ 0 << EOF
struct member_address_inner { int first; char second; };
struct member_address_outer { char tag; int value; struct member_address_inner inner; };
int main(void)
{
    struct member_address_outer object;
    struct member_address_outer *pointer = &object;

    if ((char *) (&object.value + 1) - (char *) &object.value != sizeof(int))
        return 1;
    if ((char *) (&pointer->inner + 1) - (char *) &pointer->inner !=
        sizeof(struct member_address_inner))
        return 2;
    return 0;
}
EOF

# A parameter whose address is taken later in the function is still in the
# register it arrived in when read before that. Reloading it from its slot read
# a slot nothing had written yet.
try_ 4 << EOF
int first_param(int a, int b)
{
    int x = a + b;
    int *p = &a;
    return x + *p;
}
int main(void)
{
    return first_param(1, 2);
}
EOF

try_ 51 << EOF
int second_param(int a, int b)
{
    int x = a - b;
    int *p = &b;
    *p = 1;
    return x * 10 + b;
}
int main(void)
{
    return second_param(7, 2);
}
EOF

try_ 3 << EOF
int sum_before_write(int a, int b)
{
    int x = a + b;
    int *p = &a;
    *p = 99;
    return x;
}
int main(void)
{
    return sum_before_write(1, 2);
}
EOF

# A write through a pointer can land in a global as readily as in a local. The
# global's value must not be taken from a register loaded before the write.
try_ 57 << EOF
int g;
int main(void)
{
    int *q = &g;
    g = 4;
    int x = g + 1;
    *q = 6;
    int y = g + 1;
    return x * 10 + y;
}
EOF

try_ 18 << EOF
int g;
int main(void)
{
    int a = 3;
    int *p = &a;
    int *q = &g;
    g = 4;
    int x = a * g;
    *p = 5;
    *q = 6;
    int y = a * g;
    return y - x;
}
EOF

# Assigning an address-taken variable by name and reading it through a pointer
# are two ways of reaching the same storage, in either order, on any path.
try_ 2 << EOF
int main(void)
{
    int a = 1;
    int *p = &a;
    a = 2;
    return *p;
}
EOF

try_ 10 << EOF
int main(void)
{
    int a = 3;
    int *p = &a;
    a = 9;
    *p = a + 1;
    return a;
}
EOF

try_ 5 << EOF
int pick(int a)
{
    int *p = &a;
    if (a > 2)
        a = 5;
    else
        a = 6;
    return *p;
}
int main(void)
{
    return pick(3);
}
EOF

try_ 41 << EOF
int main(void)
{
    int x = 7;
    int *p = &x;
    int y = x;
    if (y > 3)
        x = 1;
    else
        x = 2;
    int z = *p;
    *p = z + 40;
    return x;
}
EOF

try_ 10 << EOF
int main(void)
{
    int a = 0;
    int *p = &a;
    for (int k = 0; k < 5; k++) {
        *p = *p + 1;
        a = a + 1;
    }
    return a;
}
EOF

try_ 10 << EOF
int main(void)
{
    int i;
    int *p = &i;
    int s = 0;
    for (i = 0; i < 5; i++)
        s += *p;
    return s;
}
EOF

try_ 47 << EOF
int main(void)
{
    int a = 4, b = 7;
    int *ptr;
    int **pp = &ptr;
    ptr = &a;
    int x = *ptr;
    *pp = &b;
    int y = *ptr;
    return x * 10 + y;
}
EOF

try_ 120 << EOF
int main(void)
{
    int a[64];
    int i;
    int sum = 0;
    int *pi = &i;
    for (int k = 0; k < 64; k++)
        a[k] = k;
    i = 0;
loop:
    sum = sum + a[i * 4];
    *pi = *pi + 1;
    i = i + 1;
    if (i >= 12)
        goto done;
    goto loop;
done:
    return sum;
}
EOF

# asterisk dereference for reading after declaration
items 42 "int x; x = 42; int *p; p = &x; int y; y = *p; exit(y);"
items 15 "int val; val = 15; int *ptr; ptr = &val; exit(*ptr);"
items 100 "int a; a = 100; int *b; b = &a; int c; c = *b; exit(c);"
try_ 2 << EOF
int main(void) {
    int values[3] = {1, 2, 3};
    int *pointer = values;
    return *++pointer;
}
EOF
try_ 2 << EOF
int main(void) {
    int values[3] = {1, 2, 3};
    int *pointer = &values[2];
    return *--pointer;
}
EOF
try_ 7 << EOF
int main(void) {
    int values[3] = {1, 2, 3};
    int *pointer = values;
    *++pointer = 7;
    return values[1];
}
EOF

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

# Test 14b: Typedef pointer arithmetic - prefix decrement as an expression
# statement must use the same pointed-to stride as prefix increment.
try_ 20 << EOF
typedef int *int_ptr;
int main() {
    int values[3] = {10, 20, 30};
    int_ptr p = &values[2];
    --p;
    return *p;
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

try_ 0 << EOF
/* An array operand of a pointer difference decays to a pointer to its first
 * element, a row for a deeper array. The difference is a complete operand of
 * a following + or -, so end - start + 1 counts both ends.
 */
int main(void) {
    char buf[4], *bp = buf + 2, *start = buf;
    int values[5], *ip = values + 3, *first = values;
    int m[3][2], k[2][3][4], (*row)[2] = m + 1;
    if (bp - buf != 2 || buf - bp != -2 || &buf[3] - buf != 3) return 1;
    if (ip - values != 3 || values - ip + 5 != 2 || buf + 3 - buf != 3) return 2;
    if (m[2] - m[0] != 4 || (m + 2) - m != 2 || row - m != 1) return 3;
    if ((k + 1) - k != 1 || &m[2][1] - m[0] != 5) return 4;
    if (bp - start + 1 != 3 || ip - first + 1 + 2 != 6) return 5;
    return 10 - (ip - first) + 1 != 8;
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

try_ 4 << EOF
typedef int *int_ptr;
int main(void) {
    int values[10];
    int_ptr start = &values[2];
    int_ptr end = &values[6];
    return end - start;
}
EOF

try_ 2 << EOF
int main(void) {
    int *values[4];
    return &values[3] - &values[1];
}
EOF

try_ 9 << EOF
int main(void) {
    int first = 4, second = 9;
    int *values[2] = {&first, &second};
    values[0] = &second;
    return *values[0];
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

try_compile_error << EOF
typedef void *void_pointer_t;
int main() {
    void_pointer_t value = 0;
    return value + 1;
}
EOF

# A pointer-to-void-pointer advances over pointer objects, so it remains valid.
try_ 1 << EOF
int main() {
    void *values[2];
    return (char *)(values + 1) - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
typedef void *void_pointer_t;
int main() {
    void_pointer_t values[2];
    return (char *)(values + 1) - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
int main() {
    void *values[2];
    void **p = values;
    p += 1;
    return (char *)p - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
int main() {
    void *values[2];
    void **p = values;
    return (char *)(p + 1) - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
typedef void **void_pointer_slot_t;
int main() {
    void *values[2];
    void_pointer_slot_t p = values;
    p += 1;
    return (char *)p - (char *)values == sizeof(void *);
}
EOF

try_ 1 << EOF
typedef void **void_pointer_slot_t;
int main() {
    void *values[2];
    void_pointer_slot_t p = values;
    return (char *)(p + 1) - (char *)values == sizeof(void *);
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

# Taking a parameter's address must not disturb a read of that parameter made
# before the address exists. A parameter passed in a register has no stack slot
# until something needs one, and a read that went to the slot rather than the
# register picked up whatever the frame held there.
try_ 3 << EOF
int f(int a, int b)
{
    int x = a - b;
    int *p = &b;
    return x;
}
int main() { return f(5, 2); }
EOF

try_ 7 << EOF
int f(int a, int b)
{
    int x = a + b;
    int *p = &a;
    return x;
}
int main() { return f(5, 2); }
EOF

try_ 3 << EOF
int f(char a, short b)
{
    int x = a - b;
    short *p = &b;
    return x;
}
int main() { return f(5, 2); }
EOF

try_ 3 << EOF
int f(long a, long b)
{
    long x = a - b;
    long *p = &b;
    return (int) x;
}
int main() { return f(5, 2); }
EOF

# A later parameter, and one the 32-bit Arm and x86-64 conventions pass on the
# stack, written through the pointer and read back by name.
try_ 45 << EOF
int f(int a, int b, int c, int d, int e, int g, int h, int i)
{
    int x = d - c;
    int y = i - h;
    int *p = &d;
    int *q = &i;
    *q = 1;
    return x * 10 + y + i + a + b + e + g + *p - 7;
}
int main() { return f(0, 0, 3, 7, 0, 0, 2, 6); }
EOF

# The address taken in a branch and in a loop, after the parameter was read.
try_ 13 << EOF
int f(int a, int b)
{
    int x = a - b;
    if (a > 0) {
        int *p = &b;
        *p = 10;
    }
    return x + b;
}
int main() { return f(5, 2); }
EOF

try_ 11 << EOF
int f(int a, int b)
{
    int x = a - b;
    for (int i = 0; i < 3; i++) {
        int *p = &b;
        *p += x;
    }
    return b;
}
int main() { return f(5, 2); }
EOF

# The address handed to a callee that writes through it, and a function whose
# hidden aggregate-return pointer moves the parameters up one register.
try_ 12 << EOF
void set(int *p) { *p = 9; }
int f(int a, int b)
{
    int x = a - b;
    set(&b);
    return x + b;
}
int main() { return f(5, 2); }
EOF

try_ 32 << EOF
typedef struct { int a; int b; int c; int d; int e; } S;
S f(int a, int b)
{
    S s;
    int x = a - b;
    int *p = &b;
    s.a = x;
    s.b = *p;
    s.c = 0;
    s.d = 0;
    s.e = 0;
    return s;
}
int main()
{
    S s = f(5, 2);
    return s.a * 10 + s.b;
}
EOF

# Assigning to a variable by name after its address was taken must reach the
# object the pointer names. Each assignment makes a new SSA version, and every
# version has to live in the one slot the pointer holds and be written there
# before anything reads through the pointer.
try_ 5 << EOF
int main()
{
    int b = 1;
    int *p = &b;
    b = 5;
    return *p;
}
EOF

try_ 6 << EOF
int main()
{
    int b = 1;
    int *p = &b;
    b = 5;
    *p = *p + 1;
    return b;
}
EOF

try_ 7 << EOF
int main()
{
    int b = 1;
    int *p = &b;
    for (int i = 0; i < 3; i++)
        b = b + 2;
    return *p;
}
EOF

try_ 55 << EOF
int f(int c)
{
    int b = 1;
    int *p = &b;
    if (c)
        b = 2;
    else
        *p = 3;
    return b * 10 + *p;
}
int main() { return f(1) + f(0); }
EOF

try_ 7 << EOF
int main()
{
    int b = 1;
    int *p = &b;
    int i = 0;
again:
    b = b + i;
    i++;
    if (i < 4)
        goto again;
    return *p;
}
EOF

# The same for parameters, passed in a register or on the stack.
try_ 6 << EOF
int f(int a, int b)
{
    int *p = &b;
    b = a + 5;
    return *p;
}
int main() { return f(1, 2); }
EOF

try_ 67 << EOF
int f(int a, int b, int c, int d, int e, int g, int h, int i)
{
    int *p = &i;
    int *r = &b;
    i = 5 + a;
    b = i + 1;
    return *p * 10 + *r + c + d + e + g + h;
}
int main() { return f(1, 0, 0, 0, 0, 0, 0, 9); }
EOF

try_ 10 << EOF
int f(int n)
{
    int *p = &n;
    int s = 0;
    while (n > 0) {
        s += *p;
        n--;
    }
    return s;
}
int main() { return f(4); }
EOF

# Narrow, unsigned, long and pointer variables, and a value read back after a
# library call.
try_ 3 << EOF
int main()
{
    char c = 'a';
    char *p = &c;
    c = 'b';
    int r = *p;
    *p = 'c';
    return r + c - 'a' - 'a';
}
EOF

try_ 15 << EOF
int main()
{
    unsigned short u = 65535;
    unsigned short *p = &u;
    long v = 3;
    long *q = &v;
    u = u + 2;
    v = v + 4;
    return *p + *q * 2;
}
EOF

try_ 21 << EOF
int main()
{
    int x = 1, y = 2;
    int *q = &x;
    int **pp = &q;
    q = &y;
    int r = **pp;
    *pp = &x;
    return r * 10 + *q;
}
EOF

try_ 41 << EOF
#include <stdio.h>
#include <string.h>
int main()
{
    char buf[16];
    int n = 0;
    int *pn = &n;
    n = 3;
    sprintf(buf, "%d", *pn);
    n = strlen(buf) + 40;
    return *pn;
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

# C99 function designators decay to a function-pointer value when supplied as an
# argument, whether named directly, addressed explicitly, or parenthesized.
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int invoke(int (*callback)(int)) { return callback(4) - 5; }
int main(void) {
    return invoke(plus1) || invoke(&plus1) || invoke((plus1));
}
EOF

# A parenthesized function designator remains an address constant for a
# file-scope callback initializer, not merely a call-argument convenience.
try_ 0 << EOF
int plus1(int value);
int (*callback)(int) = (plus1);
int (*addressed_callback)(int) = &(plus1);
int plus1(int value) { return value + 1; }
int main(void) { return callback(4) != 5 || addressed_callback(5) != 6; }
EOF

# Aggregate constant initializers retain grouped function designators too.
try_ 0 << EOF
struct holder { int (*callback)(int); };
int plus1(int value) { return value + 1; }
struct holder global_holder = { (plus1) };
int main(void) { return global_holder.callback(6) != 7; }
EOF

try_ 0 << EOF
int plus1(int value) { return value + 1; }
int (*callbacks[2])(int) = { (plus1), &(plus1) };
int main(void) { return callbacks[0](7) != 8 || callbacks[1](8) != 9; }
EOF

# Unary address/dereference pairs around a function designator cancel before
# static initialization, while preserving the deferred code-address relocation.
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int (*first)(int) = &*plus1;
int (*second)(int) = *&plus1;
struct callbacks { int (*third)(int); int (*fourth)(int); };
struct callbacks grouped = {&*plus1, *&plus1};
int (*array[2])(int) = {&*plus1, *&plus1};
int main(void) {
    return first(1) != 2 || second(2) != 3 || grouped.third(3) != 4 ||
           grouped.fourth(4) != 5 || array[0](5) != 6 || array[1](6) != 7;
}
EOF

try_ 0 << EOF
int plus1(int value) { return value + 1; }
int (*first)(int) = &*(plus1);
int (*second)(int) = (*&plus1);
int (*third)(int) = (&*plus1);
int (*fourth)(int) = &(*plus1);
int (*fifth)(int) = *(&plus1);
int main(void) {
    return first(1) != 2 || second(2) != 3 || third(3) != 4 ||
           fourth(4) != 5 || fifth(5) != 6;
}
EOF

# Function-pointer typedefs retain their prototype through an alias chain. A
# callback parameter is already an SSA pointer value, so calling it must not
# reload its stack slot as though the slot held another function pointer.
try_ 0 << EOF
typedef int (*callback_t)(int);
typedef callback_t callback_alias_t;
int plus1(int value) { return value + 1; }
int invoke(callback_alias_t callback) { return callback(4) - 5; }
int main(void) {
    callback_t callback = plus1;
    return invoke(callback) || sizeof(callback_t) != sizeof(void *);
}
EOF

# A callback typedef preserves its prototype and relocation semantics in every
# ordinary declaration context: file scope, local storage, and parameters.
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
callback_t global_callback = plus1;
int invoke(callback_t callback) { return callback(4); }
int main(void) {
    callback_t local_callback = global_callback;
    return invoke(local_callback) != 5;
}
EOF

# A callback typedef used as a cast remains a pointer-valued callable result.
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) { return ((callback_t)plus1)(4) != 5; }
EOF

# Callback typedef values decay on return, and a direct call result remains
# callable as a postfix expression.
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
callback_t maker(void) { callback_t callback = plus1; return callback; }
int main(void) { return maker()(4) != 5; }
EOF

# The comma operator yields its final function designator, which decays before
# the callback-pointer initializer stores it.
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = (0, plus1);
    return callback(4) != 5;
}
EOF

# Pointer depth before the parenthesized callback declarator belongs to the
# callback return type, not to the typedef alias itself.
try_ 0 << EOF
typedef int *(*alloc_t)(int);
int value;
int *pick(int ignored) { return &value; }
int main(void) {
    alloc_t callback = pick;
    int *result = callback(0);
    return result != &value;
}
EOF

# Aliases with different callback prototypes must not make incompatible function
# declarations appear equivalent merely because both are pointer-sized.
try_ 1 << EOF
typedef int (*one_arg_t)(int);
typedef int (*long_arg_t)(long);
one_arg_t factory(void);
long_arg_t factory(void);
EOF

# A derived callback alias is not a direct callback object. Until the
# dereference/subscript path carries its element prototype, reject this rather
# than treating the address of the callback object as a function address.
try_ 1 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_t *slot = &callback;
    return slot(4);
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    callback_t callbacks[2] = {plus1, plus2};
    callback_t *slot = callbacks;
    return (*++slot)(4) != 6;
}
EOF
try_ 1 << EOF
typedef int (*callback_t)(int);
typedef callback_t *callback_slot_t;
int main(void) {
    callback_slot_t slot;
    return slot(4);
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_t *slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callbacks[1] = {plus1};
    return callbacks[0](4) != 5;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
typedef callback_t *callback_slot_t;
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_slot_t slot = &callback;
    return (*slot)(4) != 5;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    callback_t callbacks[2][1] = {{plus1}, {plus2}};
    return callbacks[1][0](4) != 6;
}
EOF
try_ 1 << EOF
typedef int (*callback_t)(int);
typedef callback_t *callback_slot_t;
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_slot_t slots[1] = {&callback};
    return slots[0](4);
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
typedef callback_t *callback_slot_t;
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_slot_t slots[1] = {&callback};
    return (*slots[0])(4) != 5;
}
EOF
try_ 1 << EOF
typedef int (*callback_t)(int);
typedef struct { callback_t *slot; } holder_t;
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    holder_t holder = {&callback};
    return holder.slot(4);
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
typedef struct { callback_t callback; } holder_t;
int plus1(int value) { return value + 1; }
int main(void) {
    holder_t holder = {plus1};
    return holder.callback(4) != 5;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int plus2(int value) { return value + 2; }
int main(void) {
    callback_t callbacks[2] = {plus1, plus2};
    callback_t *slot = callbacks;
    return (*(slot + 1))(4) != 6;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(int);
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_t *slot = &callback;
    callback_t **slots = &slot;
    return (**slots)(4) != 5;
}
EOF
try_ 1 << EOF
typedef int (*callback_t)(int);
typedef callback_t *callback_slot_t;
int plus1(int value) { return value + 1; }
int main(void) {
    callback_t callback = plus1;
    callback_slot_t slots[1] = {&callback};
    return (*(slots + 0))(4);
}
EOF

# The shared argument reader also serves indirect calls and must preserve a raw
# designator when it follows an ordinary scalar argument.
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int apply(int offset, int (*callback)(int)) {
    return offset + callback(4) - 7;
}
int main(void) {
    int (*indirect)(int, int (*)(int)) = apply;
    return apply(2, plus1) || indirect(2, plus1);
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

# A function designator in a record initializer is a code address. At block
# scope it was converted as an int and truncated, so the call crashed on LP64.
# Cover automatic, static, file scope, arrays of records, designators, and an
# explicit address-of.
try_ 44 << EOF
struct ops { int (*fn)(int); int k; };
int twice(int x) { return 2 * x; }
int thrice(int x) { return 3 * x; }
struct ops go = {twice, 1};
struct ops garr[2] = {{twice, 1}, {thrice, 2}};
int main(void) {
    struct ops o = {twice, 1};
    static struct ops so = {thrice, 1};
    struct ops oa[2] = {{twice, 1}, thrice, 2};
    struct ops od = {.k = 3, .fn = twice};
    struct ops oe = {&thrice, 3};
    return o.fn(1) + so.fn(1) + oa[1].fn(1) + od.fn(1) + oe.fn(1) +
           go.fn(1) + garr[1].fn(1) + garr[0].fn(1) + o.k + oa[1].k +
           od.k + 18;
}
EOF

# Parenthesized declarators may place an array suffix on the callback pointer.
# Exercise automatic, static-local, and file-scope storage separately: each
# requires pointer-sized element allocation and indexed indirect-call lowering.
try_ 23 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
int main(void) {
    int (*callbacks[2])(int) = {plus1, &plus2};
    return callbacks[0](10) + callbacks[1](10);
}
EOF
try_ 23 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
int (*callbacks[2][2])(int) = {{plus1, &plus2}, {plus2, plus1}};
int main(void) { return callbacks[1][0](10) + callbacks[1][1](10); }
EOF
try_ 48 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
int (*callbacks[2][2][2])(int) = {
    {{plus1, &plus2}, {plus2, plus1}},
    {{plus2, plus1}, {plus1, plus2}}
};
int invoke(int value) {
    static int (*local[2][2][2])(int) = {
        {{plus1, plus2}, {plus2, plus1}},
        {{plus2, plus1}, {plus1, plus2}}
    };
    return local[1][0][0](value) + local[1][1][1](value);
}
int main(void) {
    return callbacks[1][0][0](10) + callbacks[1][1][1](10) + invoke(10);
}
EOF
try_ 23 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
int invoke(int value) {
    static int (*callbacks[2])(int) = {plus1, &plus2};
    return callbacks[0](value) + callbacks[1](value);
}
int main(void) { return invoke(10); }
EOF
try_ 23 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
int (*callbacks[2])(int) = {plus1, &plus2};
int main(void) { return callbacks[0](10) + callbacks[1](10); }
EOF
try_ 23 << EOF
int plus1(int x) { return x + 1; }
int plus2(int x) { return x + 2; }
struct callbacks { int (*items[2])(int); };
int main(void) {
    struct callbacks value;
    value.items[0] = plus1;
    value.items[1] = plus2;
    return value.items[0](10) + value.items[1](10);
}
EOF

# Parenthesized function designators remain callable. In particular, a
# function-pointer dereference is a designator, not a read of the callback's
# integer return type.
try_ 42 << EOF
int add(int left, int right) { return left + right; }
int main(void) {
    int (*callback)(int, int) = add;
    return (add)(19, 23) + (callback)(8, 13) + (*callback)(5, 16) == 84
               ? 42
               : 1;
}
EOF

# RV32 stages an indirect target before all four ABI argument registers are
# populated. This scalar shape keeps that staging path covered without admitting
# the still-gated 64-bit value ABI on 32-bit targets.
try_ 10 << EOF
int sum4(int first, int second, int third, int fourth)
{
    return first + second + third + fourth;
}
int main(void)
{
    int (*callback)(int, int, int, int) = sum4;
    return callback(1, 2, 3, 4);
}
EOF

# An eight-byte callback result comes back in the integer return register, or in
# the r0/r1 or a0/a1 pair on a 32-bit target.
try_ 42 << EOF
unsigned long long add_wide(unsigned long long left,
                            unsigned long long right)
{
    return left + right;
}
int main(void)
{
    unsigned long long (*callback)(unsigned long long, unsigned long long) =
        add_wide;
    return callback(0x100000000ULL, 0x100000000ULL) == 0x200000000ULL
               ? 42
               : 1;
}
EOF

# C99 permits neither addition nor subtraction on function pointers: only
# pointers to complete object types have elements to scale or subtract.
try_compile_error << EOF
int callback(int value) { return value; }
int main(void) { return callback + 1 != 0; }
EOF
try_compile_error << EOF
int callback(int value) { return value; }
int main(void) { return 1 + callback != 0; }
EOF
try_compile_error << EOF
int callback(int value) { return value; }
int main(void) { return callback - 1 != 0; }
EOF
try_compile_error << EOF
int callback(int value) { return value; }
int main(void) { return callback - callback; }
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

# An array is not a modifiable lvalue (C99 6.3.2.1), so it cannot be assigned,
# compound-assigned, incremented or decremented, whether it is an object, a
# member or a row. A parameter declared as an array is a pointer and can be.
for expr in "a = b" "a += 1" "a++" "--a" "(a) = b" "(a)++" "++(a)" "m[1] = n[1]" \
    "m = n" "s.arr = t.arr" "p->arr = t.arr" "(*p).arr = b" "s.arr++" \
    "--p->arr" "p->m[1] = b" "int *q = (a = b)"; do
    try_compile_error_message "assignment to expression with array type" << EOF
struct S { int arr[2]; int m[2][2]; };
int main(void) {
    int a[2], b[2], m[2][2], n[2][2];
    struct S s, t, *p = &s;
    $expr;
    return 0;
}
EOF
done
try_ 5 << EOF
int values[2] = {5, 6};
int first(int a[2]) { a = values; return *a; }
struct S { int arr[2]; };
int main(void) {
    int v[2] = {1, 2}, *ip;
    struct S s;
    s.arr[1] = 3;
    ip = (s.arr);
    ip = v;
    return first(v) + ip[0] - 1;
}
EOF

# E1[E2] is (*((E1)+(E2))) (C99 6.5.2.1), so the integer may be written first:
# 2[arr] reads, stores and updates the same element as arr[2].
try_ 0 << EOF
struct P { int x; int m; };
int main(void) {
    int arr[4] = {1, 2, 3, 4}, i = 2, *p = arr, v;
    struct P ps[2] = {{1, 2}, {3, 4}};
    char *s = "hello";
    if (2[arr] != 3 || i[arr] != 3 || 1[p] + 3[p] != 6) return 1;
    if (-2[arr] + 5 != 2 || (i)[arr] + (1)[p] != 5 || 1[ps].m != 4) return 2;
    if (1["hello"] != 'e' || 4[s] != 'o') return 3;
    2[arr] = 9;
    i[p] += 5;
    if (arr[2] != 14) return 4;
    v = (3[arr] = 7);
    if (v + arr[3] != 14 || 2[arr]++ != 14 || ++1[arr] != 3) return 5;
    1[ps].m = 8;
    return arr[2] != 15 || ps[1].m != 8;
}
EOF
try_compile_error << EOF
int main(void) { int i = 1; return i[i]; }
EOF

# The operand of unary & need not start with an identifier. &*E is E without
# evaluating either operator (C99 6.5.3.2p3), and any other lvalue expression,
# such as a grouped member, a subscript written integer first or a compound
# literal, yields the address of its object. Unary * accepts such an address.
try_ 0 << EOF
struct P { int x; int m; };
int *id(int *p) { return p; }
int main(void) {
    int arr[4] = {1, 2, 3, 4}, i = 2, *p = arr, **pp = &p, *q;
    struct P pt = {5, 6}, *sp = &pt, *r;
    if (&*p != p || &*arr != arr || *&*id(p + 1) != 2 || (&*p)[3] != 4)
        return 1;
    if (*(&*p + 1) != 2 || &*arr + 1 != arr + 1 || **&*pp != 1) return 2;
    q = &2[arr];
    if (*q != 3 || *&2[arr] != 3 || &i[arr] != arr + 2) return 3;
    q = &(arr[1]);
    if (*q != 2 || &(i) != &i || *&i != 2) return 4;
    q = &(*sp).m;
    r = &(*sp);
    if (*q != 6 || &(sp)->m != q || r != sp || *&pt.m + (*&pt).x != 11)
        return 5;
    *&i = 5;
    return i != 5 || *(int *) &(int){7} != 7;
}
EOF

# &array points to the whole array, not to its first element (C99 6.5.3.2p3): it
# steps over sizeof array bytes and dereferences back to the array. This holds
# for rows of a matrix, array members, arrays of pointers and a typedef array. A
# parameter declared as an array is a pointer object instead, whose address is
# that of its own slot.
try_ 0 << EOF
typedef int row[3];
struct S { int x; int arr[5]; int mm[2][3]; };
int arr[4];
int (*gp)[4] = &arr;
char *sa[3] = {"a", "b", "c"};
#define STEP(p) ((char *) ((p) + 1) - (char *) (p))
int take(int (*p)[4]) { return (*p)[3]; }
int param(int a[4]) { int **pp = &a; return *pp == a && STEP(&a) == sizeof(int *); }
int main(void) {
    static int sl[6];
    int la[4] = {1, 2, 3, 4}, m[3][2], m3[2][3][4];
    row r = {7, 8, 9};
    struct S st[2], *sp = &st[1];
    int (*q)[4] = &arr, (*pm)[2] = &m[1];
    char *(*ps)[3] = &sa;
    arr[3] = 11;
    arr[1] = 5;
    if (take(&arr) != 11 || (&arr)[0][1] != 5 || (*&arr)[1] != 5 ||
        (*gp)[3] != 11 || sizeof(*&arr) != sizeof arr)
        return 1;
    if (STEP(&arr) != sizeof arr || STEP(&sl) != sizeof sl ||
        STEP(&la) != sizeof la || (*&la)[2] != 3 || &arr + 1 <= &arr)
        return 2;
    if (STEP(&r) != sizeof r || (*&r)[2] != 9 || STEP(&m) != sizeof m ||
        STEP(&m[1]) != sizeof m[1] || STEP(&m3[1]) != sizeof m3[1] ||
        STEP(&m3[1][2]) != sizeof m3[1][2])
        return 3;
    if (STEP(&sp->arr) != sizeof sp->arr || STEP(&st[0].mm) != sizeof st[0].mm ||
        STEP(&sp->mm[1]) != sizeof sp->mm[1])
        return 4;
    if (STEP(&sa) != sizeof sa || STEP(ps) != sizeof sa || *(*&sa)[1] != 'b' ||
        *(*ps)[2] != 'c' || *(&sa)[0][2] != 'c')
        return 5;
    m[1][1] = 6;
    if ((*pm)[1] != 6 || (*&m[1])[1] != 6 || !param(la))
        return 6;
    q++;
    if (q - &arr != 1 || STEP(q - 1) != sizeof arr)
        return 7;
    q -= 1;
    return q != &arr;
}
EOF

# sizeof on a parameter declared as an array measures the adjusted pointer (C99
# 6.7.5.3p7), while its element keeps its full extent.
try_ 0 << EOF
typedef int row[3];
struct P { int x, y; };
int f(int a[4]) { return sizeof a == sizeof(int *) && sizeof(a) == sizeof(int *) && sizeof a[0] == sizeof(int) && sizeof *a == sizeof(int); }
int g(char b[]) { return sizeof(b) == sizeof(char *); }
int h(int m[3][4]) { return sizeof m == sizeof(int *) && sizeof(m[0]) == 4 * sizeof(int) && sizeof *m == 4 * sizeof(int) && sizeof m[1][2] == sizeof(int); }
int k(row r) { return sizeof r == sizeof(int *) && sizeof(r[1]) == sizeof(int); }
int l(struct P ps[2], char *names[5]) { return sizeof(ps) == sizeof(void *) && sizeof ps[0] == sizeof(struct P) && sizeof names == sizeof(char **) && sizeof(names[0]) == sizeof(char *); }
int n(int u[][4]) { return sizeof u == sizeof(int *) && sizeof u[0] == 4 * sizeof(int); }
int main(void) {
    int x[4], y[3][4], z[3];
    struct P ps[2];
    char *names[5];
    { int a[7]; if (sizeof a != 7 * sizeof(int)) return 9; }
    return !f(x) + !g(0) * 2 + !h(y) * 4 + !k(z) * 8 + !l(ps, names) * 16 + !n(y) * 32;
}
EOF

# A pointer typedef as the array element only deepens the base type, while a
# pointer-to-array typedef carries its own element depth: one more star on the
# object makes a pointer to the typedef, stepping by a pointer.
try_ 0 << EOF
typedef int *ip;
int a = 1, b = 2, c = 3, d = 4;
#define STEP(p) ((char *) ((p) + 1) - (char *) (p))
int main(void) {
    typedef int (*T)[3];
    typedef int *(*U)[3];
    int *rows[3] = {&a, &b, &c};
    ip irows[3] = {&c, &d, 0};
    ip (*tp)[3] = &irows;
    int m[2][3] = {{1, 2, 0}, {3, 4, 0}};
    T t = &m[1], *tt = &t;
    U u = &rows, *uu = &u;
    if (*(*tp)[1] != 4 || STEP(tp) != sizeof irows || STEP(&irows) != sizeof irows ||
        *(*&irows)[0] != 3)
        return 1;
    if ((*t)[1] != 4 || STEP(t) != sizeof m[1] || STEP(tt) != sizeof(T))
        return 2;
    if (*(*u)[2] != 3 || STEP(u) != sizeof rows || STEP(uu) != sizeof(U))
        return 3;
    tt++;
    uu += 1;
    return tt - &t != 1 || uu - 1 != &u;
}
EOF

# A subscript of a pointer to an array designates an array too: &p[i] must be
# the address the subscript computed, not that of a temporary holding it, and it
# keeps the designated array as its pointee. sizeof p[0] measures the row.
try_ 0 << EOF
typedef int row[2];
int m[3][2], m3[4][2][3];
#define OFF(p, base) ((char *) (p) - (char *) (base))
int main(void) {
    row *p = &m[0];
    int (*q)[2][3] = &m3[0];
    int i = 1;
    p[1][1] = 5;
    if (OFF(&p[1], m) != sizeof(row) || OFF(&p[i], m) != sizeof(row) ||
        OFF(&p[i] + 1, m) != 2 * sizeof(row) || (*&p[i])[1] != 5)
        return 1;
    if (sizeof(p[0]) != sizeof(row) || sizeof(*p) != sizeof(row) ||
        sizeof p[0][1] != sizeof(int) || OFF(p + 1, p) != sizeof(row))
        return 2;
    if (OFF(&q[i], m3) != sizeof m3[0] || OFF(&q[1][1], m3) != 36 ||
        OFF(&q[i][1] + 1, m3) != 48 || OFF(&q[1][1][2], m3) != 44)
        return 3;
    return 0;
}
EOF

# An inner subscript of a pointer to a multidimensional array selects a row too.
# It decays to a pointer that steps by the remaining bounds, and it is never
# loaded as though it were one element, whether the pointer is named directly,
# reached through a typedef, a record member, an array slot or another pointer.
try_ 0 << 'EOF'
typedef int matrix[2][3];
int m[2][3];
int space[2][2][3];
char *names[2][2] = {{"a", "b"}, {"c", "d"}};
typedef int *slots[2][2];
int k0 = 5, k1 = 6;
slots pointers = {{&k0, &k1}, {&k1, &k0}};
struct holder { int (*direct)[2][3]; matrix *named; } holder = {&m, &m};
int main(void)
{
    matrix *p = &m;
    int (*q)[2][3] = &m;
    int (*deep)[2][2][3] = &space;
    char *(*words)[2][2] = &names;
    slots *slot = &pointers;
    int (*row_slots[2])[2][3] = {&m, &m};
    int (**indirect)[2][3] = &row_slots[1];
    struct holder *hp = &holder;
    int *row;

    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
            m[i][j] = i * 10 + j;
    space[1][1][2] = 7;
    if (p[0][1] != m[1] || q[0][1] != m[1] || p[0][1][2] != 12)
        return 1;
    row = p[0][1];
    if (row[0] != 10 || p[0][1] - p[0][0] != 3 || *(p[0][1] + 1) != 11)
        return 2;
    if (deep[0][1][1] != space[1][1] || deep[0][1][1][2] != 7 ||
        &p[0][1][2] != &m[1][2])
        return 3;
    if (words[0][1] != names[1] || words[0][1][0][0] != 'c' || *words[0][1][1] != 'd')
        return 4;
    if (slot[0][1] != pointers[1] || *slot[0][1][1] != 5)
        return 5;
    if (holder.direct[0][1] != m[1] || hp->named[0][1] != m[1] ||
        holder.direct[0][1][2] != 12)
        return 6;
    if (row_slots[1][0][1] != m[1] || indirect[0][0][1] != m[1] ||
        indirect[0][0][1][2] != 12)
        return 7;
    p[0][1][2] = 55;
    p[0][1][1]++;
    if (m[1][2] != 55 || m[1][1] != 12)
        return 8;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) { int k = 0; return &*k == 0; }
EOF
try_compile_error << EOF
int main(void) { int k = 0; return &(k + 1) == 0; }
EOF
try_compile_error << EOF
struct B { unsigned f : 3; };
int main(void) { struct B b, *p = &b; return &(*p).f == 0; }
EOF

# Nested braced rows use the same flattened backing storage as indexing. Check
# local and static storage, including omitted elements at the end of each row.
try_ 10 << EOF
int main(void) {
    int values[2][3] = {{1}, {4, 5}};
    return values[0][0] + values[0][2] + values[1][0] + values[1][1] +
           values[1][2];
}
EOF
try_ 10 << EOF
static int values[2][3] = {{1}, {4, 5}};
int main(void) {
    return values[0][0] + values[0][2] + values[1][0] + values[1][1] +
           values[1][2];
}
EOF
try_ 13 << EOF
int main(void) {
    int values[2][2][2] = {1, 2, 3, 4, 5, 6, 7, 8};
    return values[1][0][1] + values[1][1][0];
}
EOF
try_ 13 << EOF
static int values[2][2][2] = {{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}};
int main(void) { return values[1][0][1] + values[1][1][0]; }
EOF
try_ 1 << EOF
static int values[2][2][2] = {{{1}}};
int main(void) {
    return values[0][0][0] + values[0][0][1] + values[0][1][0] +
           values[1][0][0];
}
EOF
try_ 73 << EOF
int global_values[2][2][2] = {[1][0][1] = 2, 3, 4};
int local_values(void) {
    int values[2][2][2] = {[1][0] = {5, 6}, 7, 8};
    return values[1][0][0] + values[1][0][1] + values[1][1][0] +
           values[1][1][1];
}
int static_values(void) {
    static int values[2][2][2] = {[1][0] = {8, 9}, 10, 11};
    return values[1][0][0] + values[1][0][1] + values[1][1][0] +
           values[1][1][1];
}
int main(void) {
    return global_values[1][0][1] + global_values[1][1][0] +
           global_values[1][1][1] + local_values() + static_values();
}
EOF
try_ 17 << EOF
int global_values[][2] = {[3][1] = 7};
int local_values(void) {
    int values[][2] = {{1}, {2, 3}};
    return values[0][0] + values[0][1] + values[1][0] + values[1][1];
}
int static_values(void) {
    static int values[][2] = {[2][1] = 4};
    return values[2][0] + values[2][1];
}
int main(void) {
    return global_values[3][0] + global_values[3][1] + local_values() +
           static_values();
}
EOF

# A brace-elided list that stops inside a row still counts that whole row in the
# inferred outer bound, and the rest of the row is zero.
try_ 0 << EOF
#define CHECK(v, c)                                                   \
    (sizeof(v) / sizeof(v[0]) != 2 || v[1][0] != 3 || v[1][1] != 0 || \
     sizeof(c) / sizeof(c[0]) != 2 || c[1][0][0] != 5 || c[1][0][1] || \
     c[1][1][0] || c[1][1][1])
int global_values[][2] = {1, 2, 3};
int global_cube[][2][2] = {1, 2, 3, 4, 5};
void dirty(void) {
    int junk[16];
    for (int i = 0; i < 16; i++)
        junk[i] = 99;
}
int local_values(void) {
    int values[][2] = {1, 2, 3};
    int cube[][2][2] = {1, 2, 3, 4, 5};
    return CHECK(values, cube);
}
int static_values(void) {
    static int values[][2] = {1, 2, 3};
    static int cube[][2][2] = {1, 2, 3, 4, 5};
    return CHECK(values, cube);
}
int main(void) {
    dirty();
    return CHECK(global_values, global_cube) | local_values() << 1 |
           static_values() << 2;
}
EOF
try_ 13 << EOF
struct grid { int values[2][2][2]; };
int main(void) {
    struct grid value = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
    return value.values[1][0][1] + value.values[1][1][0];
}
EOF
try_ 13 << EOF
struct grid { int values[2][2][2]; };
static struct grid value = {{{{1, 2}, {3, 4}}, {{5, 6}, {7, 8}}}};
int main(void) { return value.values[1][0][1] + value.values[1][1][0]; }
EOF

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
try_compile_error << EOF
struct gb_incomplete;
static int gb_incomplete_size = sizeof(struct gb_incomplete);
int main(void) { return 0; }
EOF
try_ 5 << EOF
static int global_size = sizeof(int) + sizeof(char);
int main(void) { return global_size; }
EOF
try_ 8 << EOF
static int global_expression_size = sizeof 1 + sizeof 'A';
int main(void) { return global_expression_size; }
EOF
try_ 4 << EOF
static int global_sizeof_scalar;
static int global_sizeof_expression = sizeof(global_sizeof_scalar + 1);
int main(void) { return global_sizeof_expression; }
EOF
try_ 4 << EOF
static int global_sizeof_grouped_scalar;
static int global_sizeof_nested_group = sizeof(((global_sizeof_grouped_scalar)));
int main(void) { return global_sizeof_nested_group; }
EOF
try_ 4 << EOF
static int global_sizeof_assignment_object;
static int global_sizeof_assignment = sizeof(global_sizeof_assignment_object = 1);
int main(void) {
    return global_sizeof_assignment + global_sizeof_assignment_object;
}
EOF
try_ 4 << EOF
static int global_sizeof_prefix_object;
static int global_sizeof_prefix = sizeof(++global_sizeof_prefix_object);
int main(void) {
    return global_sizeof_prefix + global_sizeof_prefix_object;
}
EOF
try_ 4 << EOF
static int global_sizeof_postfix_object;
static int global_sizeof_postfix = sizeof(global_sizeof_postfix_object++);
int main(void) {
    return global_sizeof_postfix + global_sizeof_postfix_object;
}
EOF
try_ 16 << EOF
static int global_sizeof_unary_object;
static int global_sizeof_unary = sizeof(+global_sizeof_unary_object) +
                                 sizeof(-global_sizeof_unary_object) +
                                 sizeof(~global_sizeof_unary_object) +
                                 sizeof(!global_sizeof_unary_object);
int main(void) { return global_sizeof_unary + global_sizeof_unary_object; }
EOF
try_ 5 << EOF
static int global_sizeof_bare_unary_object;
static int global_sizeof_bare_unary = sizeof -global_sizeof_bare_unary_object + 1;
int main(void) { return global_sizeof_bare_unary + global_sizeof_bare_unary_object; }
EOF
try_ 4 << EOF
static int global_sizeof_function(void);
static int global_sizeof_call = sizeof(global_sizeof_function());
int main(void) { return global_sizeof_call; }
EOF
try_ 4 << EOF
static int global_sizeof_condition;
static int global_sizeof_conditional =
    sizeof(global_sizeof_condition ? 1 : 2);
int main(void) { return global_sizeof_conditional; }
EOF
try_ 4 << EOF
static int global_sizeof_comma_object;
static int global_sizeof_comma = sizeof((global_sizeof_comma_object++, 1));
int main(void) { return global_sizeof_comma + global_sizeof_comma_object; }
EOF
try_ $PTR_SZ << EOF
static int global_sizeof_comma_pointer_object;
static int global_sizeof_comma_pointer =
    sizeof((global_sizeof_comma_pointer_object, "text"));
int main(void) { return global_sizeof_comma_pointer; }
EOF
try_ 5 << EOF
typedef short global_sizeof_short;
static int global_sizeof_cast_source;
static int global_sizeof_casts = sizeof((char)global_sizeof_cast_source) +
                                 sizeof((global_sizeof_short)global_sizeof_cast_source) +
                                 sizeof((unsigned short)global_sizeof_cast_source);
int main(void) { return global_sizeof_casts; }
EOF
try_ $PTR_SZ << EOF
static int global_sizeof_pointer_cast_object;
static int global_sizeof_pointer_cast =
    sizeof((void *)&global_sizeof_pointer_cast_object);
int main(void) { return global_sizeof_pointer_cast; }
EOF
try_ 4 << EOF
static int global_sizeof_compound = sizeof((int){1});
int main(void) { return global_sizeof_compound; }
EOF
try_ 8 << EOF
struct global_sizeof_compound_record { char tag; int value; };
static int global_sizeof_record_compound =
    sizeof((struct global_sizeof_compound_record){0, 0});
int main(void) { return global_sizeof_record_compound; }
EOF
try_ 8 << EOF
static int global_sizeof_array_compound = sizeof((int[2]){1, 2});
int main(void) { return global_sizeof_array_compound; }
EOF
try_ 12 << EOF
static int global_sizeof_inferred_array_compound = sizeof((int[]){1, 2, 3});
int main(void) { return global_sizeof_inferred_array_compound; }
EOF
try_ 24 << EOF
static int global_sizeof_matrix_compound =
    sizeof((int[2][3]){{1, 2, 3}, {4, 5, 6}});
int main(void) { return global_sizeof_matrix_compound; }
EOF
try_ 16 << EOF
static int global_sizeof_inferred_matrix_compound =
    sizeof((int[][2]){{1, 2}, {3, 4}});
int main(void) { return global_sizeof_inferred_matrix_compound; }
EOF
try_ 16 << EOF
typedef int global_sizeof_matrix_type[2][2];
static int global_sizeof_typedef_matrix_compound =
    sizeof((global_sizeof_matrix_type){{1, 2}, {3, 4}});
int main(void) { return global_sizeof_typedef_matrix_compound; }
EOF
try_ $PTR_SZ << EOF
static int global_addressed_size_object[2];
static int global_addressed_size = sizeof &global_addressed_size_object;
int main(void) { return global_addressed_size; }
EOF
try_ $PTR_SZ << EOF
static int grouped_global_addressed_size_object[2];
static int grouped_global_addressed_size =
    sizeof(&grouped_global_addressed_size_object[1]);
int main(void) { return grouped_global_addressed_size; }
EOF
try_ $PTR_SZ << EOF
struct global_addressed_size_record { int value; };
static struct global_addressed_size_record global_addressed_size_member_object;
static int global_addressed_member_size =
    sizeof &global_addressed_size_member_object.value;
int main(void) { return global_addressed_member_size; }
EOF
try_ 12 << EOF
static int global_dereferenced_size_object[2];
static int global_dereferenced_size =
    sizeof *&global_dereferenced_size_object +
    sizeof(*&global_dereferenced_size_object[1]);
int main(void) { return global_dereferenced_size; }
EOF
try_ 4 << EOF
struct gd_member_record { int values[2]; };
static struct gd_member_record gd_member;
static int gd_member_size = sizeof *&gd_member.values[1];
int main(void) { return gd_member_size; }
EOF

# A qualified typedef of a record that is still incomplete keeps its qualifier:
# objects declared through it before the definition stay read-only after it.
try_compile_error_message "assignment of read-only location" << EOF
struct S;
typedef const struct S S_const;
S_const *gp;
struct S { int a; };
int main(void) { struct S s = {1}; gp = &s; gp->a = 2; return 0; }
EOF
try_ 0 << EOF
struct S;
typedef const struct S S_const;
S_const *gp;
struct S { int a; int b; };
int read_it(S_const *p) { return p->a + p->b; }
int main(void)
{
    struct S s = {1, 2};
    gp = &s;
    return read_it(gp) != 3 || sizeof(*gp) != sizeof(struct S);
}
EOF

# A pointer cast that initializes a static _Bool converts to 0 or 1 like any
# other scalar initializer, rather than storing the pointer's low byte.
try_ 0 << EOF
_Bool bool_from_pointer = (char *) 4;
_Bool bool_from_null = (char *) 0;
_Bool bool_from_wide = (char *) 256;
int main(void)
{
    return bool_from_pointer != 1 || bool_from_null != 0 || bool_from_wide != 1;
}
EOF

# C99 6.6 lets a static initializer convert an integer constant or an address
# constant with a pointer cast. An offset that follows advances the converted
# pointer, so its stride is that of the cast type.
try_ 0 << EOF
int pointer_cast_objects[4];
int *pointer_cast_null = (int *) 0;
void *pointer_cast_void = (void *) 0;
char *pointer_cast_chain = (char *) (void *) (1 - 1);
char *pointer_cast_string = (char *) "abc";
char *pointer_cast_bytes = (char *) pointer_cast_objects + 1;
int *pointer_cast_element = (int *) &pointer_cast_objects[1];
int *pointer_cast_outer = (int *) (char *) pointer_cast_objects + 1;
int *pointer_cast_integer = (int *) 0 + 2;
struct pointer_cast_record { char *text; int *element; };
struct pointer_cast_record pointer_cast_record = {
    (char *) "xy", (int *) &pointer_cast_objects[0] + 3
};
int main(void)
{
    char *base = (char *) pointer_cast_objects;
    char *null = (char *) 0;

    if (pointer_cast_null || pointer_cast_void || pointer_cast_chain)
        return 1;
    if (pointer_cast_string[1] != 'b')
        return 2;
    if (pointer_cast_bytes - base != 1)
        return 3;
    if (pointer_cast_element != &pointer_cast_objects[1])
        return 4;
    if (pointer_cast_outer != &pointer_cast_objects[1])
        return 5;
    if ((char *) pointer_cast_integer - null != 2 * sizeof(int))
        return 6;
    if (pointer_cast_record.text[1] != 'y' ||
        pointer_cast_record.element != &pointer_cast_objects[3])
        return 7;
    return 0;
}
EOF

# An integer converted to a pointer in a static initializer keeps every pointer
# bit on a 64-bit target and truncates to the pointer width on a 32-bit one.
try_ 0 << EOF
#include <stddef.h>
struct wide_address_holder { int tag; char *where; };
enum { wide_address_negative = -8 };
int *wide_address_scalar = (int *) 0x100000010;
char *wide_address_all_ones = (char *) 0xFFFFFFFFFFFFFFFFULL;
char *wide_address_minus_one = (char *) -1;
char *wide_address_enum = (char *) wide_address_negative;
int *wide_address_offset = (int *) 0x100000010 + 2;
char *wide_address_grouped = (char *) (0x100000000ULL + 5);
char *wide_address_cast = (char *) (unsigned long long) 0x200000003ULL;
int *wide_address_array[] = {(int *) 0x300000004, 0, (int *) 8};
struct wide_address_holder wide_address_record = {1, (char *) 0x400000005};
struct wide_address_holder wide_address_offsetof = {
    2, (char *) offsetof(struct wide_address_holder, where)};
char *wide_address_from(unsigned long long value)
{
    return (char *) value;
}
int main(void)
{
    static char *wide_address_static = (char *) 0x500000006;
    return ((char *) wide_address_scalar !=
            wide_address_from(0x100000010ULL)) +
           (((unsigned long long) wide_address_scalar >> 32) !=
            (sizeof(char *) == 8)) +
           (wide_address_all_ones != wide_address_from(0xFFFFFFFFFFFFFFFFULL)) +
           (wide_address_minus_one != wide_address_from(-1LL)) +
           (wide_address_enum != wide_address_from(-8LL)) +
           ((char *) wide_address_offset !=
            wide_address_from(0x100000010ULL + 2 * sizeof(int))) +
           (wide_address_grouped != wide_address_from(0x100000005ULL)) +
           (wide_address_cast != wide_address_from(0x200000003ULL)) +
           ((char *) wide_address_array[0] !=
            wide_address_from(0x300000004ULL)) +
           (wide_address_array[1] != 0) +
           ((char *) wide_address_array[2] != wide_address_from(8)) +
           (wide_address_record.where != wide_address_from(0x400000005ULL)) +
           (wide_address_offsetof.where !=
            wide_address_from(offsetof(struct wide_address_holder, where))) +
           (wide_address_static != wide_address_from(0x500000006ULL));
}
EOF

# A static initializer is an arithmetic constant expression, so grouped and
# unary subexpressions are as valid as bare literals.
try_ 0 << EOF
int grouped_shift = (-8 >> 1) + 12;
int grouped_product = (1 + 2) * (3 + 4);
int grouped_negation = -(1 + 2);
int grouped_logic = !(1 - 1) + ~(0);
static int grouped_nested = ((-8) >> 1) * (2 - (3 - 2));
int main(void)
{
    if (grouped_shift != 8)
        return 1;
    if (grouped_product != 21)
        return 2;
    if (grouped_negation != -3)
        return 3;
    if (grouped_logic != 0)
        return 4;
    if (grouped_nested != -4)
        return 5;
    return 0;
}
EOF

# Elements and members of static aggregates take the same constant expressions,
# sizeof and casts included.
try_ 0 << EOF
struct sized_record { int size; char *text; short bytes[2]; };
int sized_elements[] = { sizeof(int), (1 + 2), (char) 300, -(3), ~0, !(0) };
struct sized_record sized_record = {
    sizeof(struct sized_record), (char *) 0, { sizeof(short), (short) (1 << 3) }
};
struct sized_record sized_records[] = { { sizeof(char) }, { (2 + 3) } };
int sized_designated[3] = { [1] = sizeof(int) };
int main(void)
{
    enum { local_bound = 5 };
    static int local_elements[] = { sizeof(short), (local_bound + 1) };

    if (sizeof sized_elements != 6 * sizeof(int))
        return 1;
    if (sized_elements[0] != sizeof(int) || sized_elements[1] != 3 ||
        sized_elements[2] != 44 || sized_elements[3] != -3 ||
        sized_elements[4] != -1 || sized_elements[5] != 1)
        return 2;
    if (sized_record.size != sizeof(struct sized_record) || sized_record.text ||
        sized_record.bytes[0] != sizeof(short) || sized_record.bytes[1] != 8)
        return 3;
    if (sizeof sized_records != 2 * sizeof(struct sized_record) ||
        sized_records[0].size != 1 || sized_records[1].size != 5)
        return 4;
    if (sized_designated[1] != sizeof(int))
        return 5;
    if (local_elements[0] != sizeof(short) || local_elements[1] != 6)
        return 6;
    return 0;
}
EOF
try_ 12 << EOF
static int global_items[3];
static int global_array_size = sizeof global_items;
int main(void) { return global_array_size; }
EOF
try_ 12 << EOF
static int global_nested_group_items[3];
static int global_nested_group_array_size = sizeof(((global_nested_group_items)));
int main(void) { return global_nested_group_array_size; }
EOF
try_ 12 << EOF
static int global_deep_group_items[3];
static int global_deep_group_array_size = sizeof((((global_deep_group_items))));
int main(void) { return global_deep_group_array_size; }
EOF
try_ 4 << EOF
enum global_size_enum { global_size_value };
static int global_enumerator_size = sizeof global_size_value;
int main(void) { return global_enumerator_size; }
EOF
try_ 4 << EOF
enum grouped_size_enum { grouped_size_value };
static int grouped_enumerator_size = sizeof((grouped_size_value));
int main(void) { return grouped_enumerator_size; }
EOF
try_ 8 << EOF
struct global_size_record { char tag; int values[2]; };
static struct global_size_record global_size_object;
static int global_member_array_size = sizeof global_size_object.values;
int main(void) { return global_member_array_size; }
EOF
try_ 60 << EOF
struct gp_size_record { int values[2][3]; };
static struct gp_size_record *gp_size_pointer;
static int gp_size = sizeof gp_size_pointer->values[1] +
                     sizeof(gp_size_pointer->values) +
                     sizeof((gp_size_pointer->values)[1]) +
                     sizeof *&gp_size_pointer->values[1];
int main(void) { return gp_size; }
EOF
try_ 36 << EOF
struct pd_size_record { int values[2][3]; };
static struct pd_size_record *pd_size_pointer;
static int pd_size = sizeof((*pd_size_pointer).values) +
                     sizeof((*pd_size_pointer).values[1]);
int main(void) { return pd_size; }
EOF
try_ 8 << EOF
typedef struct { int values[2]; } pd_typedef_record;
typedef pd_typedef_record *pd_typedef_pointer;
static pd_typedef_pointer pd_typedef_value;
static int pd_typedef_size = sizeof((*pd_typedef_value).values);
int main(void) { return pd_typedef_size; }
EOF

# A sizeof in an integer constant expression has the value the same operand has
# at block scope, through any grouping, operator, subscript of a pointer or
# dereference of a pointer to an array. Each operand below used to fail to parse
# or to report the size of the wrong object.
try_ 0 << EOF
enum { ce_k = 3 };
int ce_g[4];
int ce_m[2][3];
int ce_v;
char *ce_cp;
int *ce_pa[3];
int (*ce_pp)[6];
int ce_sizes[] = {
    sizeof(((ce_g))[0]), sizeof((ce_m[1])[2]), sizeof((ce_v) + 1),
    sizeof(((ce_g)) + 1), sizeof(((ce_k))), sizeof(ce_k + 1), sizeof(ce_cp[1]),
    sizeof(ce_pa[1]), sizeof((*ce_pa[1])), sizeof((*ce_pp)), sizeof((*ce_pp)[1]),
};
enum { ce_row = sizeof((ce_m[1])[2]), ce_slot = sizeof(ce_pa[1]) };
int main(void) {
    char bound[sizeof((*ce_pp))];
    static int local = sizeof(((ce_m))[1]);
    int expected[] = {
        sizeof(((ce_g))[0]), sizeof((ce_m[1])[2]), sizeof((ce_v) + 1),
        sizeof(((ce_g)) + 1), sizeof(((ce_k))), sizeof(ce_k + 1), sizeof(ce_cp[1]),
        sizeof(ce_pa[1]), sizeof((*ce_pa[1])), sizeof((*ce_pp)), sizeof((*ce_pp)[1]),
    };
    for (int i = 0; i < 11; i++)
        if (ce_sizes[i] != expected[i])
            return i + 1;
    return ce_row != sizeof(int) || ce_slot != sizeof(int *) ||
           sizeof(bound) != 6 * sizeof(int) || local != 3 * sizeof(int) ||
           expected[7] != sizeof(int *) || expected[9] != 6 * sizeof(int);
}
EOF
try_compile_error << EOF
struct bad_global_member_access { int value; };
static struct bad_global_member_access *bad_global_member_pointer;
static int bad_global_member_size = sizeof bad_global_member_pointer.value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct bad_global_arrow_access { int value; };
static struct bad_global_arrow_access bad_global_arrow_object;
static int bad_global_arrow_size = sizeof bad_global_arrow_object->value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_addr_bits { unsigned int value : 1; };
static struct gb_addr_bits gb_addr_value;
static int gb_addr_size = sizeof &gb_addr_value.value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_size_bits { unsigned int value : 1; };
static struct gb_size_bits gb_size_value;
static int gb_size_direct = sizeof gb_size_value.value;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_psize_bits { unsigned int value : 1; };
static struct gb_psize_bits gb_psize_value;
static int gb_size_parenthesized = sizeof(gb_psize_value.value);
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_gsize_bits { unsigned int value : 1; };
static struct gb_gsize_bits gb_gsize_value;
static int gb_size_grouped = sizeof((gb_gsize_value.value));
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_deref_bits { unsigned int value : 1; };
static struct gb_deref_bits *gb_deref_value;
static int gb_size_dereferenced = sizeof((*gb_deref_value).value);
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_flex_size { int count; int values[]; };
static struct gb_flex_size gb_flex_value;
static int gb_flex_direct = sizeof gb_flex_value.values;
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_pflex_size { int count; int values[]; };
static struct gb_pflex_size *gb_pflex_value;
static int gb_flex_pointer = sizeof(gb_pflex_value->values);
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_dflex_size { int count; int values[]; };
static struct gb_dflex_size *gb_dflex_value;
static int gb_flex_dereferenced = sizeof((*gb_dflex_value).values);
int main(void) { return 0; }
EOF
try_compile_error << EOF
struct gb_gflex_size { int count; int values[]; };
static struct gb_gflex_size gb_gflex_value;
static int gb_flex_grouped = sizeof((gb_gflex_value.values));
int main(void) { return 0; }
EOF
try_ 8 << EOF
struct psize_record { int values[2]; };
static struct psize_record psize_object;
static int psize_member_array = sizeof(psize_object.values);
int main(void) { return psize_member_array; }
EOF
try_ 8 << EOF
struct psize_inner { int values[2]; };
struct psize_outer { struct psize_inner inner; };
static struct psize_outer psize_nested_object;
static int psize_nested_member_array = sizeof(psize_nested_object.inner.values);
int main(void) { return psize_nested_member_array; }
EOF
try_ 12 << EOF
struct psize_rows { int values[2][3]; };
static struct psize_rows psize_rows_object;
static int psize_member_row = sizeof((psize_rows_object.values)[1]);
int main(void) { return psize_member_row; }
EOF
try_ 4 << EOF
struct global_element_size_record { int values[2]; };
static struct global_element_size_record global_element_size_object;
static int global_member_element_size = sizeof global_element_size_object.values[1];
int main(void) { return global_member_element_size; }
EOF
try_ 12 << EOF
struct global_row_size_record { int values[2][3]; };
static struct global_row_size_record global_row_size_object;
static int global_member_row_size = sizeof global_row_size_object.values[1];
int main(void) { return global_member_row_size; }
EOF
try_ 8 << EOF
struct global_size_inner { int values[2]; };
struct global_size_outer { char tag; struct global_size_inner inner; };
static struct global_size_outer global_nested_size_object;
static int global_nested_member_array_size =
    sizeof global_nested_size_object.inner.values;
int main(void) { return global_nested_member_array_size; }
EOF
try_ 16 << EOF
static int parenthesized_items[3];
static int parenthesized_size = sizeof(1) + sizeof(parenthesized_items);
int main(void) { return parenthesized_size; }
EOF
try_ 4 << EOF
static int global_grouped_string_size = sizeof(("a" "bc"));
int main(void) { return global_grouped_string_size; }
EOF
try_ 4 << EOF
static int folded_expression_size = sizeof(1 + 2 * 3);
int main(void) { return folded_expression_size; }
EOF
try_ 4 << EOF
static int unary_expression_size = sizeof -1;
int main(void) { return unary_expression_size; }
EOF
try_ 4 << EOF
static int nested_expression_size = sizeof((1 + 2));
int main(void) { return nested_expression_size; }
EOF
try_ 11 << EOF
static int string_expression_size =
    sizeof "abc" + sizeof "a" "bc" + sizeof "\0x";
int main(void) { return string_expression_size; }
EOF
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

# C99 6.7p3: an identifier without linkage is declared at most once in a scope.
# Only repeated extern object or function declarations, which have linkage, may
# share a block; an object and a function never may.
try_compile_error_message "redeclaration of identifier with no linkage" << EOF
int main(void) { int x; int x; return 0; }
EOF

try_compile_error_message "redeclaration of identifier with no linkage" << EOF
int main(void) { int x = 1, x = 2; return x; }
EOF

try_compile_error_message "redeclaration of identifier with no linkage" << EOF
int main(void) { static int x; extern int x; return x; }
int x;
EOF

try_compile_error_message "redeclaration of identifier with no linkage" << EOF
int x;
int main(void) { extern int x; struct { int a; } x; return 0; }
EOF

try_compile_error_message "different kind of symbol" << EOF
int main(void) { int f(void); int f; return 0; }
EOF

try_compile_error_message "different kind of symbol" << EOF
int main(void) { int f; extern int f(void); return 0; }
EOF

try_compile_error_message "different kind of symbol" << EOF
int main(void) { enum { A }; int A; return 0; }
EOF

try_compile_error_message "different kind of symbol" << EOF
int main(void) { int A; enum { A }; return 0; }
EOF

# An enumeration constant shares the ordinary identifier name space: a nearer
# object, parameter or typedef name hides it, and a nearer constant hides an
# outer object (C99 6.2.1p4).
try_ 5 << EOF
enum { V = 1 };
int main(void) { int V = 5; return V; }
EOF
try_ 60 << EOF
enum { V = 1, P = 2 };
int param_hides(int P) { return P; }
int main(void) {
    int r = param_hides(9);                 /* 9 */
    {
        enum { W = 2 };
        { int W = 3; r += W; }              /* 12 */
        r += W;                             /* 14 */
    }
    {
        int V = 4;
        { enum { V = 6 }; r += V; }         /* 20 */
        r += V;                             /* 24 */
        { static int V = 7; r += V; }       /* 31 */
        { extern int ext; r += ext; }       /* 34 */
    }
    for (int V = 10; V < 11; V++)
        r += V;                             /* 44 */
    {
        typedef int V;
        V value = 15;
        r += value + sizeof(V) - 4;         /* 59 */
    }
    switch (r) {
    case V + 58:
        return r + V;                       /* 60 */
    }
    return 0;
}
int ext = 3;
EOF

try_compile_error_message "redeclaration of parameter" << EOF
int f(int a) { int a = 2; return a; }
int main(void) { return f(1); }
EOF

try_ 22 << EOF
int y = 2;
int twice(int);
int main(void) {
    y = 3;
    int y = 4;
    {
        extern int y;
        extern int y;
        if (y != 3)
            return 1;
    }
    extern int twice(int);
    int twice(int), twice(int);
    for (int i = 0; i < 1; i++) {
        int i = 9;
        if (i != 9)
            return 2;
    }
    {
        int y = 5;
        if (y != 5)
            return 3;
    }
    switch (y) {
        int y;
    case 4:
        y = 7;
        return twice(y) + 8 + (y - 7);
    }
    return 0;
}
int twice(int a) { return a * 2; }
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

# Default mode retains the historical extension allowing extern declarations in
# a for initializer. They bind later file-scope definitions and hide an
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

# Designators inside a braced array member may reorder rows, and a complete
# subscript path names a scalar that later positional values follow. Zero fill
# must not clear a row an earlier designator stored, including in a union, whose
# automatic storage is not cleared up front.
try_ 0 << EOF
struct rows { int m[2][2]; int k; };
union urows { int m[2][3]; char c; };
struct flat { int a[4]; int b; };
struct rows g1 = { .m = { [1] = {3, 4}, [0] = {1, 2} } };
struct rows g2 = { .m = { [1][0] = 3, 4 }, 9 };
union urows g3 = { .m = { [1] = {4, 5}, [0] = {1} } };
struct flat g4 = { .a = { [2] = 5, [0] = 1, 2 }, 7 };
int check(struct rows *r1, struct rows *r2, union urows *u, struct flat *f) {
    int r = 0;
    if (r1->m[0][0] != 1 || r1->m[0][1] != 2 || r1->m[1][0] != 3 ||
        r1->m[1][1] != 4)
        r |= 1;
    if (r2->m[0][0] || r2->m[0][1] || r2->m[1][0] != 3 || r2->m[1][1] != 4 ||
        r2->k != 9)
        r |= 2;
    if (u->m[0][0] != 1 || u->m[0][1] || u->m[0][2] || u->m[1][0] != 4 ||
        u->m[1][1] != 5 || u->m[1][2])
        r |= 4;
    if (f->a[0] != 1 || f->a[1] != 2 || f->a[2] != 5 || f->a[3] || f->b != 7)
        r |= 8;
    return r;
}
int main(void) {
    struct rows l1 = { .m = { [1] = {3, 4}, [0] = {1, 2} } };
    struct rows l2 = { .m = { [1][0] = 3, 4 }, 9 };
    union urows l3 = { .m = { [1] = {4, 5}, [0] = {1} } };
    struct flat l4 = { .a = { [2] = 5, [0] = 1, 2 }, 7 };
    static struct rows s1 = { .m = { [1] = {3, 4}, [0] = {1, 2} } };
    static struct rows s2 = { .m = { [1][0] = 3, 4 }, 9 };
    static union urows s3 = { .m = { [1] = {4, 5}, [0] = {1} } };
    static struct flat s4 = { .a = { [2] = 5, [0] = 1, 2 }, 7 };
    return check(&g1, &g2, &g3, &g4) | check(&l1, &l2, &l3, &l4) << 4 |
           check(&s1, &s2, &s3, &s4);
}
EOF

# The same holds one level down: a braced plane or row may name its rows or
# elements in any order, and zero fill leaves the ones already written alone.
try_ 0 << EOF
struct c { int m[2][2][2]; };
#define DECLS(S)                                                     \
    S int v1[2][2][2] = { { [1] = {3, 4}, [0] = {1, 2} } };          \
    S struct c v2 = { .m = { { [1] = {3, 4}, [0] = {1, 2} } } };     \
    S int v3[2][3] = { { [2] = 7, [0] = 1 }, { 4, [2] = 6, [1] = 5 } }; \
    S int v4[2][2][2][2] = { { [1] = { [1] = {7, 8} }, [0] = { {1, 2} } } };
#define CHECKS                                                              \
    int r = 0;                                                              \
    if (v1[0][0][0] != 1 || v1[0][0][1] != 2 || v1[0][1][0] != 3 ||         \
        v1[0][1][1] != 4 || v1[1][1][1])                                    \
        r |= 1;                                                             \
    if (v2.m[0][0][0] != 1 || v2.m[0][0][1] != 2 || v2.m[0][1][0] != 3 ||   \
        v2.m[0][1][1] != 4 || v2.m[1][1][1])                                \
        r |= 2;                                                             \
    if (v3[0][0] != 1 || v3[0][1] || v3[0][2] != 7 || v3[1][0] != 4 ||      \
        v3[1][1] != 5 || v3[1][2] != 6)                                     \
        r |= 4;                                                             \
    if (v4[0][0][0][0] != 1 || v4[0][0][0][1] != 2 || v4[0][0][1][0] ||     \
        v4[0][1][0][0] || v4[0][1][1][0] != 7 || v4[0][1][1][1] != 8 ||     \
        v4[1][1][1][1])                                                     \
        r |= 8;                                                             \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    return r | check_local() << 4 | check_static();
}
EOF
try_compile_error_message "Array designator index is out of bounds" << EOF
struct rows { int m[2][2]; };
int main(void) {
    struct rows r = { .m = { [2] = {1, 2} } };
    return r.m[0][0];
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

# A later member designator ends that element sequence: the positional value
# after `.b = 6` initializes c, not the array slot after items[1].
try_ 75 << EOF
struct resumed { int items[3]; int b; int c; };
struct resumed global_resumed = {.items[1] = 5, .b = 6, 7};
int score(struct resumed *v) {
    return v->items[1] + v->b + v->c * 2 + v->items[2] * 3 + v->items[0] * 5;
}
int main(void) {
    struct resumed local_resumed = {.items[1] = 5, .b = 6, 7};
    static struct resumed static_resumed = {.items[1] = 5, .b = 6, 7};
    return score(&global_resumed) + score(&local_resumed) +
           score(&static_resumed);
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

# Continuation follows a nested array-member leaf as well: after the final
# element it advances to the outer record's next field.
try_ 90 << EOF
struct inner { int items[2][2]; };
struct outer { struct inner inner; int tail; };
struct outer global = {.inner.items[0][1] = 2, 3, 4, .tail = 5};
int local(void) {
    struct outer value = {.inner.items[0][1] = 6, 7, 8, .tail = 9};
    return value.inner.items[0][1] + value.inner.items[1][0] +
           value.inner.items[1][1] + value.tail;
}
int fixed(void) {
    static struct outer value =
        {.inner.items[0][1] = 10, 11, 12, .tail = 13};
    return value.inner.items[0][1] + value.inner.items[1][0] +
           value.inner.items[1][1] + value.tail;
}
int main(void) {
    return global.inner.items[0][1] + global.inner.items[1][0] +
           global.inner.items[1][1] + global.tail + local() + fixed();
}
EOF
try_ 14 << EOF
struct inner { int cells[2][2][2]; };
struct outer { struct inner inner; int tail; };
struct outer value = {.inner.cells[1][0][1] = 2, 3, 4, .tail = 5};
int main(void) {
    return value.inner.cells[1][0][1] + value.inner.cells[1][1][0] +
           value.inner.cells[1][1][1] + value.tail;
}
EOF

# A designator that selects a row, plane or record row of a member array names
# an array subobject. An unbraced value fills its first element, and positional
# values continue through the rest of the member and then past it, while every
# element no initializer names stays zero.
try_ 0 << EOF
struct pt { int x, y; };
struct m2 { int a[2][2]; int b; int c; };
struct m3 { int a[2][2][2]; int b; };
struct m4 { char a[2][2][2][2]; int b; };
struct rec { struct pt p[2][2]; int b; };
#define DECLS(S)                                                    \
    S struct m2 v1 = { .a[1] = 3, 4 };                               \
    S struct m2 v2 = { .a[1] = 3, 4, 5, 6 };                         \
    S struct m2 v3 = { .a[1] = {3}, 5 };                             \
    S struct m3 v4 = { .a[1] = 1, 2, 3, 4, 5 };                      \
    S struct m3 v5 = { .a[0][1] = 1, 2, 3 };                         \
    S struct m4 v6 = { .a[1][1] = 1, 2, 3, 4, 5 };                   \
    S struct rec v7 = { .p[1] = 1, 2, 3, 4, 5 };                     \
    S int v8[2][2][2] = { [0][1] = 1, 2, 3 };
#define CHECKS                                                              \
    int r = 0;                                                              \
    if (v1.a[0][0] || v1.a[0][1] || v1.a[1][0] != 3 || v1.a[1][1] != 4 ||   \
        v1.b || v1.c)                                                       \
        r |= 1;                                                             \
    if (v2.a[0][0] || v2.a[0][1] || v2.a[1][1] != 4 || v2.b != 5 ||         \
        v2.c != 6)                                                          \
        r |= 2;                                                             \
    if (v3.a[0][1] || v3.a[1][0] != 3 || v3.a[1][1] || v3.b != 5 || v3.c)   \
        r |= 4;                                                             \
    if (v4.a[0][1][1] || v4.a[1][0][0] != 1 || v4.a[1][1][1] != 4 ||        \
        v4.b != 5)                                                          \
        r |= 8;                                                             \
    if (v5.a[0][0][1] || v5.a[0][1][0] != 1 || v5.a[1][0][0] != 3 ||        \
        v5.a[1][0][1] || v5.b)                                              \
        r |= 16;                                                            \
    if (v6.a[1][0][1][1] || v6.a[1][1][0][0] != 1 ||                        \
        v6.a[1][1][1][1] != 4 || v6.b != 5)                                 \
        r |= 32;                                                            \
    if (v7.p[0][1].y || v7.p[1][0].x != 1 || v7.p[1][1].y != 4 ||           \
        v7.b != 5)                                                          \
        r |= 64;                                                            \
    if (v8[0][0][1] || v8[0][1][0] != 1 || v8[1][0][0] != 3 || v8[1][0][1]) \
        r |= 128;                                                           \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    r |= check_local();
    dirty();
    return r | check_static();
}
EOF

# Positional values after a nested designator continue inside the record it
# entered, then past that record in the enclosing one (C99 6.7.8p17 and p18).
# Two designators of one bit-field unit in static storage keep both slices.
try_ 0 << EOF
struct s { int a[2][2]; int b; };
struct o { struct s in; int z; };
struct p { int x, y; };
struct h { int t; struct p arr[3]; int u; };
struct d { int k; struct o o; int m; };
struct b { int t; struct { unsigned lo : 3; unsigned hi : 5; int w; } in; int z; };
#define DECLS(S)                                                  \
    S struct o v1 = { .in.a[1][1] = 4, 5, 6 };                     \
    S struct h v2 = { .arr[1].x = 1, 2, 3, 4, 5 };                 \
    S struct o v3 = { .in.a[1] = 3, 4, 5, 6 };                     \
    S struct d v4 = { .o.in.b = 1, 2, 3 };                         \
    S struct o v5 = { .in.b = 7, .in.a[0][1] = 8, 9 };             \
    S struct b v6 = { .in.lo = 5, .in.w = 6, 7, .in.hi = 9 };      \
    S struct b v7 = { 1, .in.w = 2, 3 };
#define CHECKS                                                              \
    int r = 0;                                                              \
    if (v1.in.a[0][0] || v1.in.a[1][0] || v1.in.a[1][1] != 4 ||             \
        v1.in.b != 5 || v1.z != 6)                                          \
        r |= 1;                                                             \
    if (v2.t || v2.arr[0].x || v2.arr[1].x != 1 || v2.arr[1].y != 2 ||      \
        v2.arr[2].x != 3 || v2.arr[2].y != 4 || v2.u != 5)                  \
        r |= 2;                                                             \
    if (v3.in.a[0][1] || v3.in.a[1][0] != 3 || v3.in.a[1][1] != 4 ||        \
        v3.in.b != 5 || v3.z != 6)                                          \
        r |= 4;                                                             \
    if (v4.k || v4.o.in.b != 1 || v4.o.z != 2 || v4.m != 3)                 \
        r |= 8;                                                             \
    if (v5.in.b != 7 || v5.in.a[0][0] || v5.in.a[0][1] != 8 ||              \
        v5.in.a[1][0] != 9 || v5.z)                                         \
        r |= 16;                                                            \
    if (v6.t || v6.in.lo != 5 || v6.in.hi != 9 || v6.in.w != 6 ||           \
        v6.z != 7)                                                          \
        r |= 32;                                                            \
    if (v7.t != 1 || v7.in.lo || v7.in.w != 2 || v7.z != 3)                 \
        r |= 64;                                                            \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    r |= check_local();
    dirty();
    return r | check_static();
}
EOF

# When positional values reach a record through brace elision after a designator
# already stored some of its members, those members are kept: the elided list
# initializes only the members it names.
try_ 0 << EOF
struct p { int x, y; };
struct q { int pad; unsigned a : 4, b : 4; };
struct r { char c; struct q q[3]; int z; };
#define DECLS(S)                                                        \
    S struct r v1 = { .q[2].b = 3, .q[2].a = 5, .q[1].a = 1, 2, 7 };     \
    S struct p v2[2] = { [1].y = 2, [0].x = 1, 3, 4 };
#define CHECKS                                                              \
    int r = 0;                                                              \
    if (v1.c || v1.q[0].a || v1.q[1].pad || v1.q[1].a != 1 ||               \
        v1.q[1].b != 2 || v1.q[2].pad != 7 || v1.q[2].a != 5 ||             \
        v1.q[2].b != 3 || v1.z)                                             \
        r |= 1;                                                             \
    if (v2[0].x != 1 || v2[0].y != 3 || v2[1].x != 4 || v2[1].y != 2)       \
        r |= 2;                                                             \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    r |= check_local() << 2;
    dirty();
    return r | check_static() << 4;
}
EOF

# The same holds for an element of an array with an inferred bound, whose
# skipped elements are zero whatever form the next initializer takes, and for
# elements and rows of a braced array list.
try_ 0 << EOF
struct p { int x, y; };
struct h { struct p arr[3]; int z; };
struct h2 { struct p m[2][2]; int z; };
#define DECLS(S)                                                           \
    S struct p v1[] = { [1].y = 2, [0].x = 1, 3, 4 };                       \
    S struct p v2[] = { [2] = {5, 6}, [0].y = 7 };                          \
    S struct h v3 = { .arr = { [1] = { .y = 2 }, [0] = 1, 2, 3 }, 9 };      \
    S struct h2 v4 = { .m = { [0][1] = { .y = 2 }, [0][0] = 1, 2, 3 } };    \
    S struct h2 v5 = { .m = { [1] = { [1] = { .y = 2 }, [0] = 1, 2, 3 } } }; \
    S struct p v6[2][2] = { [1] = { [1] = { .y = 2 }, [0] = 1, 2, 3 } };
#define CHECKS                                                                \
    int r = 0;                                                                \
    if (sizeof v1 != 2 * sizeof(struct p) || v1[0].x != 1 || v1[0].y != 3 || \
        v1[1].x != 4 || v1[1].y != 2)                                         \
        r |= 1;                                                               \
    if (sizeof v2 != 3 * sizeof(struct p) || v2[0].x || v2[0].y != 7 ||      \
        v2[1].x || v2[1].y || v2[2].x != 5 || v2[2].y != 6)                   \
        r |= 2;                                                               \
    if (v3.arr[0].x != 1 || v3.arr[0].y != 2 || v3.arr[1].x != 3 ||          \
        v3.arr[1].y != 2 || v3.arr[2].x || v3.z != 9)                         \
        r |= 4;                                                               \
    if (v4.m[0][0].x != 1 || v4.m[0][0].y != 2 || v4.m[0][1].x != 3 ||       \
        v4.m[0][1].y != 2 || v4.m[1][0].x || v4.z)                            \
        r |= 8;                                                               \
    if (v5.m[1][0].x != 1 || v5.m[1][0].y != 2 || v5.m[1][1].x != 3 ||       \
        v5.m[1][1].y != 2 || v5.m[0][0].x || v5.m[0][1].y)                    \
        r |= 16;                                                              \
    if (v6[1][0].x != 1 || v6[1][0].y != 2 || v6[1][1].x != 3 ||             \
        v6[1][1].y != 2 || v6[0][0].x || v6[0][1].y)                          \
        r |= 32;                                                              \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    r |= check_local();
    dirty();
    return r | check_static();
}
EOF
try_ 0 << EOF
struct p { int x, y; };
int f(void) {
    struct p v[] = { [3] = {5, 6} };
    int m[][2] = { [2] = {1, 2} };
    return v[0].x | v[0].y | v[1].x | v[2].y | m[0][0] | m[1][1];
}
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    dirty();
    return f() != 0;
}
EOF

# Each designator in a union initializer names a member again: two designators
# of one member initialize both of its parts, a later member replaces an earlier
# one, and a positional value after a scalar member is still excess.
try_ 0 << EOF
union u { struct { int x, y; } p; int i; };
struct w { int t; union u u; };
#define DECLS(S)                                     \
    S union u v1 = { .p.x = 1, .p.y = 2 };            \
    S union u v2 = { .i = 7, .p.y = 3 };              \
    S union u v3 = { .p.y = 5, .i = 9 };              \
    S struct w v4 = { .u.p.y = 4, .u.p.x = 6, 8 };
#define CHECKS                                                         \
    int r = 0;                                                         \
    if (v1.p.x != 1 || v1.p.y != 2)                                    \
        r |= 1;                                                        \
    if (v2.p.y != 3 || v3.i != 9)                                      \
        r |= 2;                                                        \
    if (v4.t || v4.u.p.x != 6 || v4.u.p.y != 8)                        \
        r |= 4;                                                        \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
int main(void) {
    return check_global() | check_local() << 3 | check_static() << 6;
}
EOF
try_compile_error_message "Too many elements in record initializer" << EOF
union u { struct { int x, y; } p; int i; };
union u v = { .i = 1, 2 };
int main(void) { return v.i; }
EOF

# An automatic union is cleared before its initializer, so the elements of a
# member array that neither a designator nor a positional value reaches are
# zero, as C99 requires for the initialized member.
try_ 0 << EOF
union u { int a[2][2]; char c; };
struct w { int t; union u u; };
int f(void) {
    union u v = { .a[1] = 7, 8 };
    return v.a[0][0] || v.a[0][1] || v.a[1][0] != 7 || v.a[1][1] != 8;
}
int g(void) {
    union u v = { 5 };
    return v.a[0][0] != 5 || v.a[0][1] || v.a[1][1];
}
int h(void) {
    struct w v = { 1, { .a[0][1] = 2 } };
    return v.t != 1 || v.u.a[0][0] || v.u.a[0][1] != 2 || v.u.a[1][1];
}
void dirty(void) {
    int junk[16];
    for (int i = 0; i < 16; i++)
        junk[i] = 99;
}
int main(void) {
    int r;
    dirty();
    r = f();
    dirty();
    r |= g() << 1;
    dirty();
    return r | h() << 2;
}
EOF

# An array designator may continue with member designators into one record
# element. A later designator of the same element keeps the members an earlier
# one stored, and positional values continue with the element's next member.
try_ 0 << EOF
int inc(int x) { return x + 1; }
struct ops { int (*fn)(int); int k; };
struct pt { int x, y; };
struct box { struct pt inner; int z; };
struct holder { struct pt arr[3]; };
#define DECLS(S)                                                 \
    S struct ops v1[2] = { [1].fn = inc, [1].k = 3 };             \
    S struct box v2[2] = { [1].inner.x = 2, [1].z = 5 };          \
    S struct holder v3 = { .arr[2].y = 3 };                       \
    S struct pt v4[] = { [2].x = 3, 4, [1].y = 2, [1].x = 1 };
#define CHECKS                                                              \
    int r = 0;                                                              \
    if (v1[0].fn || v1[0].k || v1[1].fn(1) != 2 || v1[1].k != 3)            \
        r |= 1;                                                             \
    if (v2[0].inner.x || v2[0].z || v2[1].inner.x != 2 || v2[1].inner.y ||  \
        v2[1].z != 5)                                                       \
        r |= 2;                                                             \
    if (v3.arr[0].y || v3.arr[2].x || v3.arr[2].y != 3)                     \
        r |= 4;                                                             \
    if (sizeof(v4) != 3 * sizeof(struct pt) || v4[0].x || v4[0].y ||        \
        v4[1].x != 1 || v4[1].y != 2 || v4[2].x != 3 || v4[2].y != 4)       \
        r |= 8;                                                             \
    return r;
DECLS()
int check_global(void) { CHECKS }
int check_local(void) { DECLS() CHECKS }
int check_static(void) { DECLS(static) CHECKS }
void dirty(void) {
    int junk[64];
    for (int i = 0; i < 64; i++)
        junk[i] = 99;
}
int main(void) {
    int r = check_global();
    dirty();
    r |= check_local() << 4;
    dirty();
    return r | check_static() << 8;
}
EOF

# Four-dimensional arrays retain every inner stride for declaration, nested
# aggregate initialization, and ordinary row-major indexing.
try_ 30 << EOF
static int file_table[2][2][2][2] =
    {{{{0, 1}, {2, 3}}, {{4, 5}, {6, 7}}},
      {{{8, 9}, {10, 11}}, {{12, 13}, {14, 15}}}};
int main(void) {
    int local_table[2][2][2][2] =
        {{{{15, 14}, {13, 12}}, {{11, 10}, {9, 8}}}};
    return file_table[1][0][1][1] + local_table[0][1][1][0] +
           local_table[1][0][0][0] + (sizeof(local_table) == 64 ? 10 : 0);
}
EOF
try_ 33 << EOF
static int file_table[2][2][2][2] = {[1][0][1][1] = 7, 8, 9};
int main(void) {
    int local_table[2][2][2][2] = {[0][1][0][1] = 4, 5};
    return file_table[1][0][1][1] + file_table[1][1][0][0] +
           file_table[1][1][0][1] + local_table[0][1][0][1] +
           local_table[0][1][1][0];
}
EOF
try_ 22 << EOF
static int file_table[2][2][2][2] = {[1][1][0] = {4, 5}};
int main(void) {
    int local_table[2][2][2][2] = {[0][1][1] = {6, 7}};
    return file_table[1][1][0][0] + file_table[1][1][0][1] +
           local_table[0][1][1][0] + local_table[0][1][1][1];
}
EOF
try_ 44 << EOF
struct grid { int cells[2][2][2][2]; int tail; };
static struct grid file_grid = {.cells[1][0][1][1] = 7, 8, 9, .tail = 5};
int main(void) {
    struct grid local_grid = {.cells[0][1][0][1] = 4, 5, .tail = 6};
    return file_grid.cells[1][0][1][1] + file_grid.cells[1][1][0][0] +
           file_grid.cells[1][1][0][1] + file_grid.tail +
           local_grid.cells[0][1][0][1] + local_grid.cells[0][1][1][0] +
           local_grid.tail;
}
EOF
try_compile_error << EOF
int too_many_dimensions[1][1][1][1][1];
EOF

# Forward record tags, including aliases of them, may be used through pointers
# before their complete definition; both global static storage and block scope
# retain that rule.
try_ 17 << EOF
struct node;
union payload;
typedef struct deferred deferred_t;
static struct node *head;
static union payload *slot;
static deferred_t *deferred;
struct node { struct node *next; int value; };
union payload { int number; char bytes[4]; };
int local_forward(void) {
    struct local;
    struct local *ptr = 0;
    return !ptr;
}
int main(void) {
    struct node value;
    union payload data;
    value.next = 0;
    value.value = 7;
    data.number = 9;
    head = &value;
    slot = &data;
    return head->value + slot->number + local_forward();
}
EOF
try_compile_error << EOF
struct incomplete;
struct incomplete value;
EOF
try_compile_error << EOF
typedef struct incomplete incomplete_t;
incomplete_t value;
EOF
try_compile_error << EOF
int main(void) {
    typedef union incomplete incomplete_t;
    incomplete_t value;
    return 0;
}
EOF
try_compile_error << EOF
int main(void) {
    typedef struct incomplete incomplete_t;
    for (incomplete_t value; ; )
        return 0;
}
EOF
try_compile_error << EOF
typedef struct incomplete incomplete_t;
void invalid(incomplete_t value) { }
EOF
try_compile_error << EOF
typedef struct incomplete incomplete_t;
incomplete_t invalid(void) { }
EOF
try_ 0 << EOF
typedef struct incomplete incomplete_t;
void valid(incomplete_t values[2]) { }
int main(void) { return 0; }
EOF
try_ 9 << EOF
struct external;
typedef struct external external_t;
extern struct external direct;
extern external_t alias;
struct external { int value; };
struct external direct = {4};
external_t alias = {5};
int main(void) { return direct.value + alias.value; }
EOF

# Three-dimensional member paths retain both inner strides. A leaf designator
# also resumes positional initialization in row-major order for every storage
# duration supported by aggregate initialization.
try_ 54 << EOF
struct grid { int cells[2][2][2]; };
struct grid global_grid = {.cells[1][0][1] = 2, 3, 4};
int local_grid(void) {
    struct grid value = {.cells[1][0][1] = 5, 6, 7};
    return value.cells[1][0][1] + value.cells[1][1][0] +
           value.cells[1][1][1];
}
int static_grid(void) {
    static struct grid value = {.cells[1][0][1] = 8, 9, 10};
    return value.cells[1][0][1] + value.cells[1][1][0] +
           value.cells[1][1][1];
}
int main(void) {
    return global_grid.cells[1][0][1] + global_grid.cells[1][1][0] +
           global_grid.cells[1][1][1] + local_grid() + static_grid();
}
EOF

# The subscripts of a block-scope static's member designators are constant
# expressions of the declaration's block, even though its storage is lowered
# through the file-scope initializer.
try_ 26 << EOF
struct S { int a[3]; int m[2][2]; };
int main(void) {
    enum { K = 1 };
    static struct S s = {.a[K] = 5, .m[K][K] = 6};
    static struct S t = {.a = {[K] = 7}, .m = {[K] = {[K] = 8}}};
    return s.a[1] + s.m[1][1] + t.a[1] + t.m[1][1];
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

# A shorter string initializer zeroes the rest of an automatic array, each time
# the declaration is reached, even after the slot was overwritten.
try_ 0 << EOF
int main(void)
{
    int dirty = 0;
    for (int pass = 0; pass < 2; pass++) {
        char text[12] = "ab";
        for (int i = 2; i < 12; i++) {
            if (text[i] != 0)
                dirty++;
            text[i] = 'Q';
        }
    }
    return dirty;
}
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

# A block-scope static compound literal has global storage duration and must
# retain nested designated initializers through the global aggregate path.
try_ 11 << EOF
struct static_compound_inner { int left; int right; };
struct static_compound_outer { struct static_compound_inner inner; int tail; };
int static_nested_compound_literal(void)
{
    static struct static_compound_outer value =
        (struct static_compound_outer){.inner.right = 7, .tail = 4};
    return value.inner.left + value.inner.right + value.tail;
}
int main(void) { return static_nested_compound_literal(); }
EOF

try_ 9 << EOF
int static_designated_array_compound_literal(void)
{
    static int *values = (int[4]){[3] = 7, [1] = 2};
    return values[0] + values[1] + values[2] + values[3];
}
int main(void) { return static_designated_array_compound_literal(); }
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

# Default mode retains the historical extension allowing a static for-init
# object. It persists across calls but is visible only to the loop clauses/body.
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
try_compile_error_message "undefined static function 'missing_static_function'" << EOF
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

# Block-scope static address constants share the global initializer lowering.
# Preserve rank-four row strides both from an array root and after selecting an
# array member.
try_ 0 << EOF
int static_rank4_leaf(void) {
    static int values[2][2][2][2] = {
        {{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
        {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}
    };
    static int *leaf = &values[1][0][1][1];
    return *leaf != 37;
}
int main(void) { return static_rank4_leaf(); }
EOF
try_ 0 << EOF
struct static_rank4_box { int values[2][2][2][2]; };
int static_member_rank4_leaf(void) {
    static struct static_rank4_box box = {
        {{{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
         {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}}
    };
    static int *leaf = &box.values[1][0][1][1];
    return *leaf != 37;
}
int main(void) { return static_member_rank4_leaf(); }
EOF
try_ 0 << EOF
struct static_rank4_plane_box { int values[2][2][2][2]; };
int static_member_rank4_plane(void) {
    static struct static_rank4_plane_box box = {
        {{{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
         {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}}
    };
    static int (*plane)[2][2] = &box.values[0][1] + 1;
    return plane != &box.values[1][0] || plane[0][1][1] != 37;
}
int main(void) { return static_member_rank4_plane(); }
EOF

# Aggregate pointer initializers use the same address-constant walker. A
# trailing offset after partial rank-four subscripting advances by a plane,
# rather than by the original array's row extent.
try_ 0 << EOF
int aggregate_address_rows[2][2][2][2] = {
    {{{2, 3}, {5, 7}}, {{11, 13}, {17, 19}}},
    {{{23, 29}, {31, 37}}, {{41, 43}, {47, 53}}}
};
int *aggregate_address_leaf[] = {
    &aggregate_address_rows[1][0][1][1]
};
int (*aggregate_address_plane[])[2][2] = {
    &aggregate_address_rows[0][1] + 1
};
int main(void) {
    if (*aggregate_address_leaf[0] != 37)
        return 1;
    return aggregate_address_plane[0][0][1][1] != 37;
}
EOF

# Multiple global pointer-to-fixed-array slots must retain the selected slot's
# row shape, rather than falling back to scalar element strides after loading.
try_ 0 << EOF
int repeated_slot_rows[2][2] = {
    {2, 5}, {3, 7}
};
int (*repeated_slot_planes[])[2] = {
    repeated_slot_rows, repeated_slot_rows + 1, repeated_slot_rows
};
int main(void) {
    return repeated_slot_planes[2][1][0] != 3;
}
EOF

# This evaluates a direct unsigned long long global expression in an address
# offset.
try_ 0 << EOF
int global_wide_offset_values[] = {17, 23};
int *global_wide_offset =
    &global_wide_offset_values[0] + (0x100000000ULL >> 32);
int *global_wide_decay_offset =
    global_wide_offset_values + (0x100000000ULL >> 32);
int *global_wide_negative_offset =
    &global_wide_offset_values[1] - (0x100000000ULL >> 32);
struct global_wide_offset_holder { int *value; };
struct global_wide_offset_holder global_wide_offset_aggregate = {
    &global_wide_offset_values[0] + (0x100000000ULL >> 32)};
int main(void) {
    int *aggregate_value = global_wide_offset_aggregate.value;
    return *global_wide_offset != 23 || *global_wide_decay_offset != 23 ||
           *global_wide_negative_offset != 17 ||
           *aggregate_value != 23;
}
EOF
try_ 0 << EOF
struct pointer_member_holder { int *value; };
typedef char *pointer_member_char_ptr;
struct pointer_member_indirect_holder {
    pointer_member_char_ptr character;
    int **value;
};
int main(void) {
    int direct = 29;
    int *direct_pointer = &direct;
    char character = 'x';
    struct pointer_member_holder holder = {&direct};
    struct pointer_member_holder *pointer = &holder;
    struct pointer_member_indirect_holder indirect = {&character,
                                                       &direct_pointer};
    struct pointer_member_indirect_holder *indirect_pointer = &indirect;
    return *holder.value != 29 || *pointer->value != 29 ||
           *indirect.character != 'x' || **indirect.value != 29 ||
           **indirect_pointer->value != 29;
}
EOF
try_ 0 << EOF
struct pointer_member_inner { int field; };
struct pointer_member_outer { struct pointer_member_inner *member; };
typedef int (*pointer_member_callback)(int);
struct pointer_member_callback_holder { pointer_member_callback callback; };
int add_one(int value) { return value + 1; }
int main(void) {
    struct pointer_member_inner inner = {55};
    struct pointer_member_outer outer = {&inner};
    struct pointer_member_outer *outer_pointer = &outer;
    struct pointer_member_callback_holder callback_holder = {add_one};
    return (*outer.member).field != 55 ||
           (*outer_pointer->member).field != 55 ||
           (*callback_holder.callback)(41) != 42;
}
EOF
try_compile_error << EOF
int global_wide_offset_range_values[1];
int *global_wide_offset_out_of_range =
    &global_wide_offset_range_values[0] + 0x100000000ULL;
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

# LDRH, LDRSH and STRH take an eight-bit offset on Arm. A halfword slot or
# global field beyond 255 bytes must be addressed through a materialized offset,
# and the size estimate must match what is emitted.
try_ 0 << EOF
void bump_half(short *p) { *p = *p + 1; }
int main(void) {
    char big[600];
    short s = 1000;
    big[0] = 1;
    bump_half(&s);
    s = s + 2;
    bump_half(&s);
    return s != 1004 || big[0] != 1;
}
EOF

try_ 0 << EOF
int half_target = 9;
struct half_record { int pad[100]; short h1; short h2; int *link; };
struct half_record half_global = { .h1 = 1, .h2 = 2, .link = &half_target };
int main(void) {
    return half_global.h1 != 1 || half_global.h2 != 2 ||
           *half_global.link != 9;
}
EOF

# Address constants inside aggregate static initializers accept the same
# designators as scalar ones: members, constant-expression subscripts, offsets.
try_ 31 << EOF
struct inner { int pad; int values[3]; };
struct outer { int tag; struct inner nested[2]; };
enum { SECOND = 1 };
static struct outer record = {7, {{1, {2, 3, 4}}, {5, {6, 7, 8}}}};
struct slots { int *first; int *second; int *third; };
static struct slots table = {&record.tag, &record.nested[SECOND].values[1 + 1],
                             &record.nested[0].values[0] + 2};
int main(void) {
    static int *local[2] = {&record.nested[SECOND].pad,
                            &record.nested[1].values[0] + 1};
    return *table.first + *table.second + *table.third + *local[0] + *local[1];
}
EOF

# An initializer large enough that its setup spills temporaries must keep them
# in the global data area, not past the top of the stack.
try_ 4 << EOF
int table[1500] = {0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1,2,3,4,5,6,0,1};
int main(void) { return table[1499] + table[3]; }
EOF

# Every declarator in a block-scope list shares the resolved base type, not only
# the first: b and d below are unsigned and long like a and c.
try_ 7 << EOF
int main(void) {
    unsigned int a = 0, b = 0x80000000U;
    long c = 0, d = 0;
    unsigned e = 1, f = 0xffffffffU;
    return (b >> 31) + 2 * (sizeof(d) == sizeof(long)) + 4 * (f > e);
}
EOF

# A cast between signed and unsigned of the same width changes how the value
# extends: (int) of an unsigned int with every bit set shifts right as -1.
try_ 3 << EOF
int shift_cast(unsigned int value) { return ((int) value >> 1) == -1; }
int widen_cast(unsigned int value) { int s = (int) value; return (s >> 4) == -1; }
int main(void) { return shift_cast(0xffffffffU) + 2 * widen_cast(0xfffffff0U); }
EOF

# An old-style definition types its identifier list in a declaration list (C99
# 6.9.1p6), called with and without a prototype in scope. A prototype must agree
# with the promoted parameter types (6.7.5.3p15), and C99 has no implicit int
# for an undeclared parameter.
try_ 0 << EOF
int add(x, y) int x; int y; { return x + y; }
int proto(int a, char *s);
int proto(n, str) int n; char *str; { return n + str[1]; }
int later();
int narrow(c, s) char c; short s; { return c + s; }
int arr(a, n) int a[]; int n; { return a[n - 1]; }
int fn(f, v) int f(int); int v; { return f(v); }
int twice(int v) { return 2 * v; }
long long wide(ll, i) long long ll; register int i; { return ll + i; }
int multi(a, b, c) int a, *b; char c; { return a + *b + c; }
int main(void)
{
    int v[3] = {4, 5, 6};
    int seven = 7;
    int (*fp)() = add;
    return add(1, 2) != 3 || proto(1, "ab") != 'b' + 1 || later(5) != 6 ||
           narrow('a', 1) != 'a' + 1 || arr(v, 3) != 6 || fn(twice, 4) != 8 ||
           wide(0x100000000LL, 1) != 0x100000001LL || multi(1, &seven, 2) != 10 ||
           fp(3, 4) != 7;
}
int later(x) int x; { return x + 1; }
EOF
try_ 0 << EOF
int f(int);
int f(c) char c; { return c; }
int main(void) { return f(65) - 65; }
EOF
try_compile_error_message "parameter type defaults to int" << EOF
int f(x) { return x; }
int main(void) { return f(1) - 1; }
EOF
try_compile_error_message "parameter type defaults to int" << EOF
int f(x, y) int x; { return x + y; }
int main(void) { return 0; }
EOF
try_compile_error_message "declaration of a name not in the identifier list" << EOF
int f(x) int x; int z; { return x; }
int main(void) { return 0; }
EOF
try_compile_error_message "conflicting types for function declaration" << EOF
int f(char);
int f(c) char c; { return c; }
int main(void) { return 0; }
EOF
try_compile_error_message "identifier list requires a function definition" << EOF
int f(x);
int main(void) { return 0; }
EOF
try_compile_error_message "conflicting types for function declaration" << EOF
int f(x) int x; { return x; }
int f(char);
int main(void) { return 0; }
EOF
try_compile_error_message "conflicting types for function declaration" << EOF
int f(x, y) int x, y; { return x; }
int f(int);
int main(void) { return 0; }
EOF

# Brace initializers take casts to function pointer types, and each element of
# an array of callbacks converts like any other callback object.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int a(void) { return 7; }
struct ops { int (*f)(int); int (*g)(void); };
struct ops o = { (int (*)(int)) inc, (int (*)(void)) a };
int (*t[2])(void) = { (int (*)(void)) a, 0 };
int main(void)
{
    struct ops lo = { (int (*)(int)) inc, (int (*)(void)) 0 };
    int (*lt[2])(void) = { (int (*)(void)) a, 0 };
    return o.f(1) != 2 || o.g() != 7 || t[0]() != 7 || t[1] != 0 || lo.f(2) != 3 || lo.g != 0 || lt[0]() != 7;
}
EOF
try_ 0 << EOF
typedef int (*thunk_t)(void);
int inc(int x) { return x + 1; }
int a(void) { return 7; }
struct ops { int (*f)(int); int (*g)(void); thunk_t h; };
struct ops o = { .g = (int (*)(void)) a, .f = (int (*)(int)) &inc, .h = (thunk_t) a };
struct ops list[2] = { { (int (*)(int)) inc, 0, 0 }, { 0, (int (*)(void)) 0, (thunk_t) 0 } };
static int (*t[3])(void) = { (int (*)(void)) a, 0, (thunk_t) a };
int main(void)
{
    static struct ops so = { (int (*)(int)) inc };
    return o.f(1) != 2 || o.g() != 7 || o.h() != 7 || list[0].f(5) != 6 ||
           list[1].g != 0 || t[2]() != 7 || so.f(0) != 1;
}
EOF

# In an expression too, a cast to a pointer to a callback typedef is an object
# pointer, so it compares with a pointer of that type, with an array of the
# callbacks and with null; it was taken for a callable function pointer and the
# comparison rejected. A cast naming the function pointer itself still is one.
try_ 0 << EOF
typedef int (*callback_t)(void);
typedef int function_t(void);
int one(void) { return 1; }
int main(void)
{
    callback_t table[2] = { one, one };
    callback_t *p = table, *n = 0;
    int (**q)(void) = table;
    callback_t c = (callback_t) one;
    function_t *g = (function_t *) one;
    if (p == (callback_t *) 16 || p != (callback_t *) table) return 1;
    if (q != (int (**)(void)) table || p != q || n != (callback_t *) 0) return 2;
    if ((*(callback_t *) table)() != 1 || c() != 1 || g() != 1) return 3;
    return c != one || g != (callback_t) one;
}
EOF
try_compile_error_message "Function pointer comparison requires compatible pointers or null" << EOF
typedef int (*callback_t)(void);
int one(void) { return 1; }
int main(void) { callback_t *p = 0; return p == one; }
EOF

# A cast to a pointer to function pointers, spelled out or through a callback
# typedef, is an object pointer cast in a static initializer: it converts an
# integer or an address for a scalar, a member or an element, and keeps the
# prototype it points to for the slot check. It was rejected as a non-constant.
try_ 0 << EOF
typedef int (*callback_t)(void);
struct holder { int (**slots)(void); int n; };
int five(void) { return 5; }
int (*fs[2])(void) = { five, five };
int (**s)(void) = (int (**)(void)) 4;
int (***r)(void) = (int (***)(void)) 20;
int (**q)(void) = (int (**)(void)) fs;
callback_t *p = (callback_t *) 16;
callback_t *pa = (int (**)(void)) 24;
struct holder g = { (int (**)(void)) 12, 3 };
int (**t[2])(void) = { (int (**)(void)) 4, 0 };
int main(void)
{
    static int (**ls)(void) = (int (**)(void)) 8;
    static struct holder lg = { (int (**)(void)) 28, 1 };
    if (s != (int (**)(void)) 4 || r != (int (***)(void)) 20) return 1;
    if ((*q[1])() != 5 || (void *) p != (void *) 16 || (void *) pa != (void *) 24)
        return 2;
    if (g.slots != (int (**)(void)) 12 || g.n != 3) return 3;
    if (t[0] != (int (**)(void)) 4 || t[1] != 0) return 4;
    if (ls != (int (**)(void)) 8 || lg.slots != (int (**)(void)) 28) return 5;
    return 0;
}
EOF
try_compile_error_message "incompatible callback slot types in initializer" << EOF
int (**s)(void) = (int (**)(int)) 4;
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible callback slot types in initializer" << EOF
struct holder { int (**slots)(void); };
struct holder g = { (int (**)(int)) 12 };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible callback slot types in array initializer" << EOF
int (**t[1])(void) = { (int (**)(int)) 4 };
int main(void) { return 0; }
EOF

# So is a cast to a function typedef with two stars, which points to function
# pointers as a callback typedef with one does. A single star still names the
# function pointer itself. It was rejected as a non-constant.
try_ 0 << EOF
typedef int fnty(void);
struct holder { int (**slots)(void); };
int one(void) { return 1; }
int (**s)(void) = (fnty **) 4;
fnty **p = (fnty **) 16;
struct holder g = { (fnty **) 12 };
int (**t[2])(void) = { (fnty **) 8, 0 };
int (*f)(void) = (fnty *) one;
int (*z)(void) = (fnty *) 0;
int main(void)
{
    static int (**ls)(void) = (fnty **) 8;
    if (s != (int (**)(void)) 4 || p != (int (**)(void)) 16) return 1;
    if (g.slots != (int (**)(void)) 12 || t[0] != (fnty **) 8 || t[1]) return 2;
    if (ls != (int (**)(void)) 8 || f() != 1 || z) return 3;
    return 0;
}
EOF
try_compile_error_message "incompatible callback slot types in initializer" << EOF
typedef int fnty(int);
int (**s)(void) = (fnty **) 4;
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible callback slot types in initializer" << EOF
typedef int fnty(int);
int main(void) { static int (**s)(void) = (fnty **) 4; return 0; }
EOF

# An integer explicitly cast to a function pointer type initializes a member or
# element of a static aggregate; it was rejected as an implicit conversion.
try_ 0 << EOF
typedef int (*callback_t)(void);
struct ops { callback_t cb; int (*d)(int); };
int (*f)(void) = (int (*)(void)) 1;
callback_t c = (callback_t) 4;
int (*z)(void) = (int (*)(void)) 0;
struct ops o = { (callback_t) 1, (int (*)(int)) 8 };
struct ops arr[2] = { { (callback_t) 3 }, [1].cb = (callback_t) 4 };
int (*t[2])(void) = { (int (*)(void)) 2, (callback_t) 0 };
int main(void)
{
    static int (*sf)(void) = (int (*)(void)) 3;
    static int (*st[1])(void) = { (int (*)(void)) 5 };
    static struct ops so = { (callback_t) 6 };
    if (f != (callback_t) 1 || c != (callback_t) 4 || z != 0) return 1;
    if (o.cb != (callback_t) 1 || o.d != (int (*)(int)) 8) return 2;
    if (arr[0].cb != (callback_t) 3 || arr[1].cb != (callback_t) 4) return 3;
    if (t[0] != (callback_t) 2 || t[1] != 0) return 4;
    if (sf != (callback_t) 3 || st[0] != (callback_t) 5 || so.cb != (callback_t) 6)
        return 5;
    return 0;
}
EOF
try_compile_error_message "incompatible function pointer types" << EOF
typedef int (*callback_t)(void);
struct ops { callback_t cb; };
struct ops o = { (int (*)(int)) 1 };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int inc(int x) { return x + 1; }
struct ops { int (*f)(int); };
struct ops o = { (int (*)(void)) inc };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int inc(int x) { return x + 1; }
int (*t[1])(void) = { (int (*)(int)) inc };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int inc(int x) { return x + 1; }
int (*t[1])(void) = { inc };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int inc(int x) { return x + 1; }
int main(void) { int (*t[1])(void) = { inc }; return 0; }
EOF

# A callback may return a record, through the hidden result pointer, whether it
# is declared, named by a typedef or returned by a function.
try_ 0 << EOF
struct S { int a, b, c, d; };
typedef struct S (*maker_t)(int);
struct S make(int x) { struct S s = {x, x + 1, x + 2, x + 3}; return s; }
struct S (*cb)(int) = make;
maker_t tcb = make;
maker_t get_t(void) { return make; }
int main(void)
{
    struct S (*local)(int) = make;
    struct S r1 = cb(1);
    struct S r3 = local(20);
    struct S r4 = tcb(30);
    struct S r5 = get_t()(40);
    return r1.d != 4 || r3.c != 22 || r4.b != 31 || r5.d != 43;
}
EOF
try_ 0 << EOF
struct S { int a, b, c, d; };
union U { int i; char c[8]; };
struct S make(int x) { struct S s = {x, x + 1, x + 2, x + 3}; return s; }
union U make_u(int x) { union U u; u.i = x; return u; }
struct S (*get(void))(int) { return make; }
union U (*get_u(void))(int) { return make_u; }
struct S (*(*pg)(void))(int) = get;
struct S apply(struct S (*f)(int), int v) { return f(v); }
int main(void)
{
    struct S (*cb)(int) = get();
    struct S r1 = get()(10);
    struct S r2 = pg()(20);
    struct S r3 = apply(get(), 30);
    union U u = get_u()(7);
    return r1.a != 10 || r1.d != 13 || r2.c != 22 || r3.b != 31 ||
           cb(1).d != 4 || u.i != 7;
}
EOF

# A compound literal may have a function pointer type, spelled out or through a
# typedef; at file scope its address is an address constant.
try_ 0 << EOF
typedef int (*unary_t)(int);
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
char *name(void) { return "ab"; }
int main(void)
{
    int (*f)(int) = (int (*)(int)){inc};
    int (**p)(int) = &(int (*)(int)){inc};
    int (*z)(int) = (int (*)(int)){0};
    char *(*n)(void) = (char *(*)(void)){name};
    unary_t t = (unary_t){dec};
    int (**q)(int) = (int (**)(int)){p};
    *p = dec;
    return f(1) != 2 || (*p)(2) != 1 || z != 0 || n()[1] != 'b' || t(5) != 4 ||
           (*q)(9) != 8 || (int (*)(int)){inc}(4) != 5;
}
EOF
try_ 0 << EOF
typedef int (*unary_t)(int);
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
int (**gp)(int) = &(int (*)(int)){inc};
unary_t *gt = &(unary_t){dec};
int (**gn)(int) = &(int (*)(int)){0};
int main(void)
{
    int r = (*gp)(3) != 4 || (*gt)(3) != 2 || *gn != 0;
    *gp = dec;
    return r || (*gp)(3) != 2;
}
EOF

# A pointer to a const callback typedef object keeps the callback read-only,
# while the pointer itself stays modifiable.
try_ 0 << EOF
typedef int (*const cfn_t)(int); typedef int (*fn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } cfn_t g = dec; int main(void) { cfn_t f = inc; cfn_t *p = &f; cfn_t **pp = &p; fn_t h = inc; fn_t *q = &h; *q = dec; p = &g; *pp = &g; return (*p)(5) != 4 || (**pp)(3) != 2 || h(1) != 0; }
EOF
try_ 0 << EOF
typedef int (*const cfn_t)(int); typedef int (*fn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } cfn_t g = dec; int main(void) { cfn_t f = inc; cfn_t *p = &f; cfn_t **pp = &p; *pp = &g; return (**pp)(3) - 2; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
typedef int (*const cfn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { cfn_t f = inc; cfn_t *p = &f; *p = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
typedef int (*const cfn_t)(int); typedef int (*fn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { cfn_t f = inc; cfn_t *p = &f; p[0] = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
typedef int (*const cfn_t)(int); typedef int (*fn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { cfn_t f = inc; cfn_t *p = &f; cfn_t **pp = &p; **pp = dec; return 0; }
EOF
try_compile_error_message "incompatible callback slot types" << EOF
typedef int (*const cfn_t)(int); typedef int (*fn_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { cfn_t f = inc; fn_t *p = &f; return 0; }
EOF

# Any pointer level of a callback slot may be qualified, the callback pointer
# itself included, and a store through a const level is rejected; restrict may
# not qualify the callback pointer (C99 6.7.3p2).
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
int (*f)(int) = inc;
int (*const cf)(int) = dec;
int main(void)
{
    int (*g)(int) = dec;
    int (*const *p)(int) = &f;
    int (*const **pp)(int) = &p;
    int (**const *q)(int);
    int (**m)(int) = &g;
    int (* volatile *vp)(int) = &g;
    int r = (*p)(1) != 2 || (**pp)(2) != 3;
    p = &cf;
    r |= (*p)(5) != 4 || (**pp)(5) != 4;
    q = &m;
    **q = inc;
    r |= g(1) != 2;
    *vp = dec;
    r |= g(1) != 0 || (*vp)(3) != 2;
    return r;
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*const f)(int) = inc; int (*const *p)(int) = &f; return (*p)(1) - 2; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*f)(int) = inc; int (**const q)(int) = &f; int (**const *p)(int) = &q; **p = dec; return f(1); }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*volatile f)(int) = inc; int (*volatile *p)(int) = &f; *p = dec; return f(3) - 2; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (**restrict *slot_t)(int); int (*f)(int) = inc; int (**s)(int) = &f; int main(void) { slot_t t = &s; return (**t)(1) - 2; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*const f)(int) = inc; int (*const *p)(int) = &f; *p = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*const f)(int) = inc; int (*const *p)(int) = &f; int (*const **pp)(int) = &p; **pp = dec; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*f)(int) = inc; int (**const q)(int) = &f; int (**const *p)(int) = &q; *p = 0; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
int (*const cf)(int) = inc;
int main(void) { int (*const *p)(int) = &cf; p[0] = inc; return 0; }
EOF
try_compile_error_message "incompatible callback slot types" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int main(void) { int (*const f)(int) = inc; int (*const *p)(int) = &f; int (*const **pp)(int) = &p; int (**bad)(int) = p; return 0; }
EOF
try_compile_error_message "incompatible callback slot types" << EOF
int inc(int x) { return x + 1; }
int (*const cf)(int) = inc;
int main(void) { int (**p)(int) = &cf; return 0; }
EOF
try_compile_error_message "incompatible callback slot types" << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int main(void) { int (*const **pp)(int) = &s; return 0; }
EOF
try_compile_error_message "restrict requires a pointer to an object type" << EOF
int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } typedef int (*restrict *slot_t)(int); int main(void) { return 0; }
EOF

# A callback slot may be any number of pointers deep: declared, as a typedef at
# either scope, cast, as a compound literal, returned, or through void *.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
char *name(void) { return "ab"; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int (***t)(int) = &s;
char *(*nf)(void) = name;
char *(**ns)(void) = &nf;
char *(***nt)(void) = &ns;
int call3(int (***p)(int), int v) { return (**p)(v); }
int (***get3(void))(int) { return t; }
int main(void)
{
    int (*g)(int) = dec;
    int (**gs)(int) = &g;
    int (***lt)(int) = &s;
    int (****u)(int) = &lt;
    int r = 0;
    r |= (**t)(1) != 2;
    r |= (***u)(3) != 4;
    r |= (*nf)()[1] != 'b' || (*ns)()[0] != 'a' || (**nt)()[1] != 'b';
    r |= call3(t, 5) != 6;
    r |= (**get3())(6) != 7;
    *lt = gs;
    r |= (**lt)(5) != 4;
    **lt = inc;
    r |= g(1) != 2;
    ***u = dec;
    r |= g(1) != 0;
    r |= **t != dec;
    return r;
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int (***t)(int) = &s;
void *erased = &t;
int size3 = sizeof(int (***)(int));
int main(void)
{
    int (****u)(int) = (int (****)(int)) erased;
    int (***v)(int) = (int (***)(int)){&s};
    int (****w)(int) = &(int (***)(int)){t};
    char *(***x)(void) = (char *(***)(void)) 0;
    return (***u)(1) != 2 || (**v)(2) != 3 || (***w)(3) != 4 || x != 0 ||
           size3 != sizeof(void *) ||
           sizeof((int (****)(int)) erased) != sizeof(void *);
}
EOF
try_ 0 << EOF
typedef int (**slot_t)(int);
typedef int (***slot3_t)(int);
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
slot_t s = &f;
slot3_t t = &s;
int main(void)
{
    typedef int (***local3_t)(int);
    local3_t lt = &s;
    slot3_t *pt = &t;
    return (**t)(1) != 2 || (**lt)(2) != 3 || (***pt)(3) != 4 ||
           sizeof(slot3_t) != sizeof(void *);
}
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; } int (*f)(int) = inc; int (**s)(int) = &f; int main(void) { typedef int (***l3)(int); l3 q = &s; int (**r)(int) = *q; return (*r)(3) - 4; }
EOF
try_ 0 << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int (** const *t)(int) = &s;
int main(void)
{
    int (** const cs)(int) = &f;
    int (*** const ct)(int) = &s;
    return (**t)(1) != 2 || (*cs)(2) != 3 || (**ct)(3) != 4;
}
EOF
try_compile_error_message "incompatible callback slot types" << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int main(void) { int (***t)(int) = s; return 0; }
EOF
try_compile_error_message "incompatible callback slot types" << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int (***t)(int) = &s;
int main(void) { int (**q)(int) = t; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << EOF
int inc(int x) { return x + 1; }
int (*f)(int) = inc;
int (**s)(int) = &f;
int (** const *t)(int) = &s;
int main(void) { *t = s; return 0; }
EOF

# A call returning a callback slot typedef, or a pointer to one, keeps the slot
# on its value, directly or through a function pointer of that return type.
try_ 0 << EOF
typedef int (**slot_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int (*cb)(int) = inc; slot_t get(void) { return &cb; } int main(void) { slot_t s = get(); return (*get())(2) - 3 + (*s)(1) - 2; }
EOF
try_ 0 << EOF
typedef int (**slot_t)(int); int inc(int x) { return x + 1; } int dec(int x) { return x - 1; } int (*cb)(int) = inc; slot_t *get(void) { static slot_t s = &cb; return &s; } int main(void) { slot_t *s = get(); return (**get())(2) - 3 + (**s)(1) - 2; }
EOF
try_ 0 << EOF
typedef int (**slot_t)(int);
typedef int (*callback_t)(int);
int inc(int x) { return x + 1; }
int (*cb)(int) = inc;
slot_t get(void) { return &cb; }
slot_t *get_deeper(void) { static slot_t s = &cb; return &s; }
callback_t *get_typedef(void) { return &cb; }
int main(void)
{
    typedef int (**local_slot_t)(int);
    local_slot_t (*pg)(void) = get;
    slot_t s = get();
    slot_t *ss = get_deeper();
    callback_t *ts = get_typedef();
    local_slot_t ls = pg();
    return (*get())(2) != 3 || (*s)(1) != 2 || (**get_deeper())(3) != 4 ||
           (**ss)(4) != 5 || (*pg())(5) != 6 || (*ls)(6) != 7 ||
           (*get_typedef())(7) != 8 || (*ts)(8) != 9;
}
EOF

# A function may return a pointer to a callback slot, spelled out or through a
# typedef, and a call's result then dereferences to the callback.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int (*slot)(int) = inc;
int (**get(void))(int) { return &slot; }
int (**(*pg)(void))(int) = get;
int main(void)
{
    int (**s)(int) = get();
    return (*s)(1) != 2 || (*get())(2) != 3 || (*pg())(3) != 4;
}
EOF
try_ 0 << EOF
typedef int (*UP)(int);
int inc(int x) { return x + 1; }
UP slot = inc;
UP *get(void) { return &slot; }
UP *(*pg)(void) = get;
int main(void)
{
    UP *s = get();
    return (*s)(1) != 2 || (*get())(2) != 3 || (*pg())(3) != 4;
}
EOF

# The returned function pointer may be qualified, and a typedef may name the
# type of a function returning a function pointer, at either scope.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
typedef int (*get_t(void))(int);
typedef int (*pick_t(int which))(int);
get_t get;
pick_t pick;
int (*const cget(void))(int);
get_t *pointer_to_get = get;
int main(void)
{
    typedef int (*local_t(void))(int);
    local_t *local = get;
    return get()(1) != 2 || pick(0)(5) != 4 || cget()(2) != 3 ||
           pointer_to_get()(3) != 4 || local()(4) != 5;
}
int (*get(void))(int) { return inc; }
int (*pick(int which))(int) { return which ? inc : dec; }
int (*const cget(void))(int) { return inc; }
EOF
try_compile_error_message "function definition cannot take its type from a typedef" << EOF
int inc(int x) { return x + 1; }
typedef int (*get_t(void))(int);
get_t get { return inc; }
int main(void) { return 0; }
EOF

# A function may return a spelled function pointer, `int (*get(void))(int)`, and
# a pointer may point to such a function.
try_ 0 << EOF
int inc(int x) { return x + 1; }
int dec(int x) { return x - 1; }
char *hello(void) { return "hi"; }
int (*get(void))(int);
int (*get(void))(int) { return inc; }
int (*pick(int which))(int) { return which ? inc : dec; }
char *(*greeter(void))(void) { return hello; }
int (*(*gpg)(void))(int) = get;
int (*(*table[2])(int))(int) = {pick, pick};
int use(int (*(*g)(int))(int)) { return g(0)(10); }
int main(void)
{
    int (*local(void))(int);
    int (*(*pg)(void))(int) = get;
    int (*(*pp)(int))(int) = &pick;
    int (*cb)(int) = get();
    return get()(3) != 4 || pick(1)(5) != 6 || pick(0)(5) != 4 ||
           greeter()()[1] != 'i' || pg()(1) != 2 || gpg()(2) != 3 ||
           pp(0)(9) != 8 || cb(0) != 1 || use(pick) != 9 ||
           local()(7) != 8 || table[1](1)(1) != 2;
}
int (*local(void))(int) { return inc; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
char *hello(void) { return "hi"; }
int (*get2(void))(int) { return hello; }
int main(void) { return 0; }
EOF
try_compile_error_message "conflicting types for function declaration" << EOF
int inc(int x) { return x + 1; }
int (*get(void))(void);
int (*get(void))(int) { return inc; }
int main(void) { return 0; }
EOF

# A cast may spell a function pointer type out, in expressions, sizeof and
# static initializers alike.
try_ 0 << EOF
typedef int (*thunk_t)(void);
int one(int x) { return x + 1; }
void set(int v) {}
char *name(void) { return "ab"; }
int (*global_cast)(void) = (int (*)(void)) one;
int (*global_null)(int) = (int (*)(int)) 0;
thunk_t global_typedef = (thunk_t) one;
int size_one = sizeof(int (*)(void));
int size_two = sizeof(char *(**)(int, ...));
int main(void) {
    static int (*from_address)(int) = (int (*)(int)) &one;
    int (*f)(void) = (int (*)(void)) one;
    int (*back)(int) = (int (*)(int)) f;
    void (*g)(int, ...) = (void (*)(int, ...)) set;
    int (*fp)(int) = one;
    int (**pp)(int) = &fp;
    int (**qq)(int) = (int (**)(int)) pp;
    char *(*nf)(void) = (char *(*)(void)) 0;
    char *(*nm)(void) = (char *(*)(void)) name;
    g(1, 2);
    return back(2) != 3 || (*qq)(3) != 4 || ((int (*)(int)) f)(4) != 5 ||
           nf != 0 || nm()[1] != 'b' || global_null != 0 ||
           ((int (*)(int)) global_cast)(1) != 2 ||
           ((int (*)(int)) global_typedef)(2) != 3 || from_address(5) != 6 ||
           size_one + size_two != 2 * sizeof(void *) ||
           sizeof((int (*)(void)) one) != sizeof(void *);
}
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int one(int x) { return x + 1; }
int (*g)(int) = (int (*)(void)) one;
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int one(int x) { return x + 1; }
int main(void) { int (*fp)(int) = (int (*)(void)) one; return 0; }
EOF

# A function cast to a callback typedef is that callback, even when the
# prototypes differ, not a pointer to a callback slot.
try_ 0 << EOF
typedef int (*call_t)(int, long long);
typedef int (*thunk_t)(void);
int words(int a, int skipped, int low, int high) { return low + high; }
int one(int x) { return x; }
int main(void) {
    call_t call = (call_t) words;
    int (*thunk)(void) = (thunk_t) one;
    thunk_t again = (thunk_t) one;
    return call != (call_t) words || thunk != again;
}
EOF

# Function pointers convert implicitly only between compatible function types
# (C99 6.5.16.1p1); an unprototyped type is compatible with a prototype without
# an ellipsis or promoted parameters (6.7.5.3p15). A void pointer converts to
# none but the null pointer constant.
try_ 0 << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
typedef int unary_t(int);
typedef int (*unary_ptr)(int);
typedef void (*handler_t)(int);
int unprototyped();
int unprototyped(int x) { return x; }
int old_style() { return 3; }
int apply(int (*fn)(int), int v) { return fn(v); }
int apply_typedef(unary_t *fn, int v) { return fn(v); }
void handler(int s) {}
handler_t install(handler_t f) { return f; }
int (*global_fp)(int) = one;
unary_ptr global_addr = &one;
unary_t *global_null = 0;
int (*global_void_null)(int) = (void *) 0;
struct ops { int (*f)(int); handler_t g; } table = {one, handler};
unary_ptr get(void) { return one; }
int main(void) {
    int (*fp)(int) = one;
    int (*np)(int) = (void *) 0;
    int (*up)() = one;
    int (*up_void)() = zero;
    int (*from_unprototyped)(int) = unprototyped;
    unary_ptr u = fp;
    unary_t *deref = *one;
    struct ops local = {one, handler};
    handler_t old = install(handler);
    old = install(0);
    local.f = u;
    fp = 0;
    fp = up;
    fp = global_fp;
    fp = get();
    np = deref;
    up = old_style;
    return apply(one, 1) + apply_typedef(fp, 1) + apply(u, 1) +
           apply(0 ? one : fp, 1) + local.f(1) + get()(1) + global_addr(1) +
           from_unprototyped(1) + up(0) + table.f(1) + (np == 0) +
           (global_null != 0) + (global_void_null != 0) + (old != 0) !=
           12;
}
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int apply(int (*fn)(int), int v) { return fn(v); }
int main(void) { return apply(zero, 2); }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
typedef int unary_t(int);
int apply(unary_t *fn, int v) { return fn(v); }
int main(void) { int (*z)(void) = zero; return apply(z, 2); }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
typedef int (*unary_ptr)(int);
unary_ptr get(void) { return zero; }
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int main(void) { int (*fp)(void) = name; return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int (*global_fp)(int) = zero;
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
struct ops { int (*f)(void); } table = {one};
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int main(void) { int (*fp)(int); fp = count; return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
struct S { int (*cb)(int); };
int main(void) { struct S s; s.cb = zero; return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int main(void) { void *p = 0; int (*fp)(int) = p; return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int main(void) { void *p = one; return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int zero(void) { return 0; }
int one(int x) { return x; }
char *name(void) { return "a"; }
int count(int n, ...) { return n; }
int take_char(char c) { return c; }
int main(void) { int (*fp)() = take_char; return 0; }
EOF

# An integer becomes a pointer without a cast only as a null pointer constant
# (C99 6.5.16.1p1), in initializers, assignments, arguments and returns alike. A
# conditional with a pointer operand has that pointer type (6.5.15p6).
try_ 0 << EOF
#define NULL_ALT 0
enum { ZERO };
int *g1 = 0, *g2 = (void *) 0, *g3 = 1 - 1, *g4 = NULL_ALT;
char *g5 = ZERO;
int *ret0(void) { return 0; }
void take(int *p, char *q) {}
int main(void) {
    int x = 3;
    int *p = 0, *q = (int *) 7, *r = &x, *s = (void *) 0, *t = ZERO;
    char *u = '\0';
    int arr[2] = {5, 6};
    int *ap = x ? arr : 0;
    p = 0;
    p = 2 - 2;
    q = x ? &x : (void *) 0;
    if (*(x ? ap : arr) != 5 || *((x ? ap : arr) + 1) != 6)
        return 99;
    take(0, 0);
    take((int *) 0, (char *) 0);
    int *(cl) = (int *){0};
    return (ret0() != 0) + (p != 0) + (*q != 3) + (*r != 3) + (s != 0) +
           (t != 0) + (u != 0) + (g1 != 0) + (g2 != 0) + (g3 != 0) +
           (g4 != 0) + (g5 != 0) + (cl != 0);
}
EOF
try_ 0 << EOF
enum { ZERO, ONE };
int take(int *p) { return p == 0; }
int *ret(void) { return ZERO; }
int main(void) {
    int *t = ZERO;
    char *u;
    u = ZERO;
    return !take(ZERO) || t || u || ret() || ONE + ONE != 2;
}
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
enum { ZERO, ONE };
int main(void) { int *t = ONE; return t != 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { int *p = 7; return 0; }
EOF

# A character read out of a string literal is an integer, and only its address
# points into the literal.
try_compile_error_message "integer converted to pointer without a cast" << EOF
void sink(int *p) {}
int main(void) { sink(*("x")); return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { int *p = "xy"[1]; return p != 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { int *p; int x = 3; p = x; return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
void f(int *p) {}
int main(void) { f(1); return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
void g(int q[3]) {}
int main(void) { g(1); return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int *f(int x) { return x; }
int main(void) { return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int *p = 3 + 4;
int main(void) { return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
long long *p = 0x100000000LL;
int main(void) { return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
typedef int *P;
int main(void) { P v = (P){7}; return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { static int *p = 5; return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int (*f)(void) = 1;
int main(void) { return 0; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
typedef int (*callback_t)(void);
int main(void) { static callback_t c = 4; return c != 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int (*f)(void) = (int (*)(int)) 2;
int main(void) { return 0; }
EOF

# A function pointer, even one an explicit cast produced, does not convert to an
# object pointer in a static initializer, for a scalar, a member or an element.
try_compile_error_message "incompatible function pointer types" << EOF
int *p = (int (*)(void)) 1;
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int main(void) { static void *v = (int (*)(void)) 0; return v != 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int *t[1] = { (int (*)(void)) 1 };
int main(void) { return 0; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int f(void) { return 0; }
int main(void) { int (*g)(void) = f; int *t[1] = { g }; return t[0] != 0; }
EOF
try_ 0 << EOF
int x;
int f(void) { return 1; }
int *t[2] = { &x, (int *) 4 };
void *u[2] = { 0, &x };
int *p = (int *) 8;
int (*fs[2])(void) = { f, (int (*)(void)) 0 };
int main(void)
{
    return t[0] != &x || t[1] != (int *) 4 || u[1] != &x || p != (int *) 8 ||
           fs[0]() != 1 || fs[1] != 0;
}
EOF
try_ 0 << EOF
typedef int (*callback_t)(void);
int (*f)(void) = (int (*)(void)) 1;
callback_t c = (callback_t) 4, n = 0;
int main(void) { return f != (callback_t) 1 || c != (callback_t) 4 || n; }
EOF
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { int x = 1; int *p = x ? 1 : 2; return 0; }
EOF

# Every declarator of a block declaration has its initializer converted and
# diagnosed, not just the first one.
try_compile_error_message "integer converted to pointer without a cast" << EOF
int main(void) { int *a = 0, *b = 7; return a != b; }
EOF
try_compile_error_message "incompatible function pointer types" << EOF
int g(void) { return 0; }
int main(void) { int (*f)(void) = g, (*h)(int) = g; return f != 0 && h != 0; }
EOF
try_compile_error_message "discarding const qualifier" << EOF
int main(void) { const int c = 1; int *a = 0, *b = &c; return a != b; }
EOF
try_ 0 << EOF
int g(void) { return 3; }
int main(void) {
    int x = 1, (*h)(void) = g, (*k)(void) = &g, *p = 0, *q = &x;
    char *e = 0, *s = "x";
    return h() + k() + x != 7 || p || *q != 1 || e || s[0] != 'x';
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

# Assigning a string literal to a char pointer stores it; with the warning
# enabled it warns as well. The assignment was dropped, and as an expression it
# was rejected as yielding no value.
try_ 0 << EOF
char *g;
int main(void) {
    char *s = "a";
    char *t;
    s = "xy";
    t = g = "pq";
    for (s = "zz"; 0;)
        ;
    return s[1] != 'z' || g[1] != 'q' || t[0] != 'p';
}
EOF
try_compile_warning "Warning: string literal is read-only" "--warn-string-literals" << EOF
int main(void) {
    char *text;
    text = "hello";
    return text[0] != 'h';
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

# C99 prototype calls require exactly the declared fixed argument count.
try_compile_error << EOF
int identity(int value) { return value; }
int main(void) { return identity(); }
EOF
try_compile_error << EOF
int identity(int value) { return value; }
int main(void) { return identity(1, 2); }
EOF
try_compile_error << EOF
int identity(int value) { return value; }
int main(void) { int (*fn)(int) = identity; return fn(); }
EOF
try_compile_error << EOF
int sum(int first, ...) { return first; }
int main(void) { return sum(); }
EOF
try_ 4 << EOF
/* An empty parameter list is not a prototype in C99. */
int legacy() { return 4; }
int main(void) { return legacy(1, 2); }
EOF
try_ 4 << EOF
int refined();
int refined(int value) { return value; }
int main(void) { return refined(4); }
EOF
try_ 4 << EOF
int qualified_parameter(const int value);
int qualified_parameter(int value) { return value; }
int main(void) { return qualified_parameter(4); }
EOF
try_compile_error << EOF
int incompatible_parameter(const int *value);
int incompatible_parameter(int *value) { return *value; }
EOF
try_compile_error << EOF
int incompatible_volatile_parameter(volatile int *value);
int incompatible_volatile_parameter(int *value) { return *value; }
EOF
try_compile_error << EOF
int retained(int value);
int retained();
int retained(int value) { return value; }
int main(void) { return retained(); }
EOF
try_compile_error << EOF
int incompatible_definition(int value);
int incompatible_definition() { return 0; }
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

# Record operands are objects: the selected one is copied whole, not joined as a
# truncated scalar, whether it comes from a variable or a call result.
try_ 131 << EOF
typedef struct { int rank; int bounds[4]; } shape_t;
typedef struct { char c; short s; } narrow_t;
shape_t wide(void) { shape_t s = {3, {10, 20, 30, 40}}; return s; }
shape_t low(void) { shape_t s = {1, {5, 6, 7, 8}}; return s; }
int pick(int c) {
    shape_t local = {9, {1, 2, 3, 4}};
    shape_t chosen;
    chosen = c ? wide() : low();
    shape_t mixed = c ? local : low();
    narrow_t a = {1, 300}, b = {2, 400};
    narrow_t nested = c > 1 ? a : c ? b : a;
    return chosen.rank + chosen.bounds[3] + mixed.bounds[0] + mixed.rank +
           nested.s / 100;
}
int main(void) { return pick(1) + pick(0) + pick(2); }
EOF
try_compile_error_message "Conditional record operands must have the same type" << EOF
typedef struct { int a; } one_t;
typedef struct { int a; int b; } two_t;
int main(void) {
    one_t x = {1};
    two_t y = {1, 2};
    int c = 1;
    one_t z = c ? x : y;
    return z.a;
}
EOF

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

# Compound assignment promotes an unsigned char or unsigned short right operand
# before the usual arithmetic conversions, so it is zero-extended into a long
# long left operand exactly as in sum = sum + c.
try_output 0 "0.c8;0.c8;ffffffff.fff0bcf8;ffffffff.f4143e00;ffffffff.fffffff0;ffffffff.fffffffb;0.a840;ffffffff.fff0bdc8;ffffffff.119b95c0;ffffffff.118595c0;ffffffff.fffe17b8;100.b8;ff.ffff1658;1.47ae134f;1.a9c53b4f;" << EOF
void show(long long value)
{
    printf("%x.%x;", (unsigned) (value >> 32), (unsigned) value);
}
int main(void)
{
    unsigned char c = 200;
    unsigned short s = 60000;
    unsigned int u = 4000000000U;
    long long sum = 0;
    unsigned long long total = 0xfffffffff0ULL;
    sum += c;
    show(sum);
    sum = 0;
    sum = sum + c;
    show(sum);
    sum = -1000000;
    sum -= c;
    show(sum);
    sum = -1000000;
    sum *= c;
    show(sum);
    sum = -1000000;
    sum /= s;
    show(sum);
    sum = -5;
    sum %= c;
    show(sum);
    sum = -1000000;
    sum &= s;
    show(sum);
    sum = -1000000;
    sum |= c;
    show(sum);
    sum = -1000000;
    sum ^= u;
    show(sum);
    sum = -1000000;
    sum -= u;
    show(sum);
    sum = -1000000;
    sum >>= c - 197;
    show(sum);
    total += c;
    show(total);
    total -= s;
    show(total);
    total /= c;
    show(total);
    total ^= u;
    show(total);
    return 0;
}
EOF

# The result converts back to the left operand's type, including a change of
# signedness: int += unsigned int stores a negative int, and a signed int
# division result widens by sign extension.
try_output 0 "ffffffff.fff0be88;0.7b8a800;ffffffff.ffffec78;ffffffff.fffffffb;ffffffff.fff057a0;ffffffff.ee7a6a40;0.11a41a40;0.b8;0.0;0.fffffffb;" << EOF
void show(long long value)
{
    printf("%x.%x;", (unsigned) (value >> 32), (unsigned) value);
}
int main(void)
{
    unsigned char c = 200;
    unsigned short s = 60000;
    unsigned int u = 4000000000U;
    int i = -1000000;
    unsigned int w = 0xfffffff0U;
    i += c;
    show(i);
    i = -1000000;
    i *= s;
    show(i);
    i = -1000000;
    i /= c;
    show(i);
    i = -5;
    i %= c;
    show(i);
    i = -1000000;
    i ^= s;
    show(i);
    i = 1000000;
    i += u;
    show(i);
    i = 1000000;
    show(i -= u);
    w += c;
    show(w);
    w = 7;
    w /= c;
    show(w);
    w = 7;
    show(w += -12);
    return 0;
}
EOF

# The same conversions through members, subscripts, pointers and globals, where
# the assignment expression's value is the converted result.
try_output 0 "ffffffff.fff0be88;ffffffff.f4143e00;ffffffff.fffffffb;ffffffff.fff0bcf8;ffffffff.ffffec78;ffffffff.ee7a6a40;0.11a41a40;0.ffffff28;0.ffffff28;" << EOF
void show(long long value)
{
    printf("%x.%x;", (unsigned) (value >> 32), (unsigned) value);
}
struct record {
    long long wide;
    int narrow;
    unsigned int word;
};
long long global_sum;
int main(void)
{
    unsigned char c = 200;
    unsigned int u = 4000000000U;
    struct record r, *p = &r;
    long long sums[2];
    int narrow[2];
    long long *q = &sums[1];
    r.wide = -1000000;
    r.wide += c;
    show(r.wide);
    p->wide = -1000000;
    show(p->wide *= c);
    sums[1] = -5;
    sums[1] %= c;
    show(sums[1]);
    *q = -1000000;
    *q -= c;
    show(sums[1]);
    global_sum = -1000000;
    global_sum /= c;
    show(global_sum);
    r.narrow = 1000000;
    show(r.narrow += u);
    narrow[1] = 1000000;
    narrow[1] -= u;
    show(narrow[1]);
    r.word = 0xfffffff0U;
    show(r.word -= c);
    r.word = 0xfffffff0U;
    show(r.word -= c);
    return 0;
}
EOF

# A move eliminated by the peephole pass must not keep the narrowing flags of
# the unsigned constant load it replaced, or the member address is truncated.
try_output 0 "0.ffffff28;0.ffffff28;" << EOF
void show(long long value)
{
    printf("%x.%x;", (unsigned) (value >> 32), (unsigned) value);
}
struct record {
    unsigned int word;
};
int main(void)
{
    struct record r;
    unsigned char c = 200;
    r.word = 0xfffffff0U;
    show(r.word -= c);
    r.word = 0xfffffff0U;
    show(r.word -= c);
    return 0;
}
EOF
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

# Type specifiers may appear in any order, qualifiers among them, in every type
# name and declaration; each reader shares one specifier parser.
try_ 73 << EOF
char spelled[sizeof(char const) + sizeof(short volatile int const)];
int main(void) {
    typedef int volatile unsigned const U;
    struct { const int signed a; char c; } r = {-1, 0};
    return sizeof(spelled) + sizeof(unsigned volatile char const) * 10 +
           (sizeof(U) == 4) * 20 + (r.a < 0) * 40;
}
EOF
try_compile_error_message "assignment of read-only variable" << EOF
int main(void) { const int signed x = 1; x = 2; return x; }
EOF
try_compile_error_message "duplicate type specifier" << EOF
char bad[sizeof(char short)];
int main(void) { return 0; }
EOF
try_compile_error_message "int cannot be combined with char" << EOF
int main(void) { return sizeof(int char); }
EOF
try_compile_error_message "long cannot be combined with char" << EOF
int main(void) { long char c = 0; return c; }
EOF
try_compile_error_message "long cannot be combined with short" << EOF
typedef short long bad;
int main(void) { return 0; }
EOF
try_compile_error << EOF
typedef int T;
char bad[sizeof(unsigned T)];
int main(void) { return 0; }
EOF
try_compile_error_message "record type cannot be combined with integer specifiers" << EOF
struct S { int a; };
unsigned struct S s;
int main(void) { return 0; }
EOF
try_compile_error_message "enum type cannot be combined with integer specifiers" << EOF
enum E { A };
int main(void) { long enum E e = A; return e; }
EOF
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

try_ 8 << EOF
typedef struct {
    int x;
    short y;
} struct_t;

int main() { return sizeof(struct_t); }
EOF

# Record members use natural alignment, and a struct's trailing size is rounded
# to its strongest member so arrays of the record keep every element aligned.
try_ 4 << EOF
struct layout {
    char tag;
    int value;
    short tail;
};
int main(void) {
    struct layout value;
    struct layout values[2];
    return (sizeof(value) == 12) +
           ((char *)&value.value - (char *)&value == 4) +
           ((char *)&value.tail - (char *)&value == 8) +
           ((char *)&values[1] - (char *)&values[0] == 12);
}
EOF

# A typedef-backed nested record carries its own alignment into its enclosing
# record rather than being treated as its scalar storage size alone.
try_ 4 << EOF
typedef struct { char c; int value; } inner_t;
struct outer { char tag; inner_t inner; short tail; };
int main(void) {
    struct outer value;
    return (sizeof(value) == 16) +
           ((char *)&value.inner - (char *)&value == 4) +
           ((char *)&value.tail - (char *)&value == 12) +
           (sizeof(value.inner) == 8);
}
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
items 12 "int values[3]; return sizeof((values));"
items 12 "int values[3]; return sizeof(((values)));"
items 12 "int values[3]; return sizeof(*&values);"
items 12 "int values[3]; return sizeof *&values;"
items 24 "int values[2][3]; return sizeof(*&values);"
items 12 "int values[2][3]; return sizeof(values[0]);"
items 12 "int values[2][3]; return sizeof values[0];"
items 12 "int values[2][3], index = 0; return sizeof(values[index++]) + index;"
items 4 "int values[2][3]; return sizeof values[0][1];"
items 16 "int values[2][2][2][2]; return sizeof(values[0][0]);"
items 8 "int values[2][2][2][2]; return sizeof(values[0][0][0]);"
items 4 "int values[2][2][2][2]; return sizeof(values[0][0][0][0]);"
items 12 "struct holder { int values[3]; }; struct holder value; return sizeof value.values;"
items 12 "struct holder { int values[3]; }; struct holder value; return sizeof((value.values));"
items 12 "struct holder { int values[2][3]; }; struct holder value; return sizeof(value.values[0]);"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner rows[2]; }; struct holder value; return sizeof(value.rows[1].values[1]);"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner rows[2]; }; struct holder value; int index = 0; return sizeof(value.rows[index++].values[index++]) + index;"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner *rows[2]; }; struct holder value; return sizeof(value.rows[1]->values[1]);"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner *rows[2]; }; struct holder value; int index = 0; return sizeof(value.rows[index++]->values[index++]) + index;"

# A sizeof operand made of '&', '*', casts, member selections and subscripts has
# the type those operators build, in any order. Each check returns its own code
# so a failure names the form.
try_ 0 << EOF
struct walk_record { int value; char tag; int values[5]; };
int main(void)
{
    struct walk_record object, *pointer = &object, records[3];
    int values[4], *cursor = values, matrix[2][3], *slots[7], (*row)[6] = 0;
    char byte;

    if (sizeof &object.value != sizeof(int *)) return 1;
    if (sizeof(&pointer->tag) != sizeof(char *)) return 2;
    if (sizeof &records[1].value != sizeof(int *)) return 3;
    if (sizeof *&values[1] != sizeof(int)) return 4;
    if (sizeof(&*cursor) != sizeof(int *)) return 5;
    if (sizeof *&byte != 1) return 6;
    if (sizeof(*&object) != sizeof(struct walk_record)) return 7;
    if (sizeof (&object)->tag != 1) return 8;
    if (sizeof records->values != 5 * sizeof(int)) return 9;
    if (sizeof records[2].tag != 1) return 10;
    if (sizeof *slots != sizeof(int *)) return 11;
    if (sizeof *matrix != 3 * sizeof(int)) return 12;
    if (sizeof row[0] != 6 * sizeof(int)) return 13;
    if (sizeof (*row)[1] != sizeof(int)) return 14;
    if (sizeof &(*row)[1] != sizeof(int *)) return 15;
    if (sizeof(*(char *) cursor) != 1) return 16;
    if (sizeof((struct walk_record *) 0)->values != 5 * sizeof(int)) return 17;
    if (sizeof(&*matrix) != sizeof(int *)) return 18;
    if (sizeof((char) object.value) != 1) return 19;
    if (sizeof(*&pointer->values) != 5 * sizeof(int)) return 20;
    return 0;
}
EOF
try_ 0 << EOF
int sizeof_walk_callee(int value) { return value; }
int main(void)
{
    int (*callback)(int) = sizeof_walk_callee;
    int (*callbacks[3])(int);

    if (sizeof callback != sizeof(int (*)(int))) return 1;
    if (sizeof(&sizeof_walk_callee) != sizeof(int (*)(int))) return 2;
    if (sizeof &*callback != sizeof(int (*)(int))) return 3;
    if (sizeof callbacks[1] != sizeof(int (*)(int))) return 4;
    return 0;
}
EOF

try_compile_error_message "Member reference through '->' requires a pointer" << EOF
struct walk_arrow { int value; };
int main(void) { struct walk_arrow object; return sizeof object->value; }
EOF
try_compile_error_message "Member reference base is not a struct or union" << EOF
struct walk_dot { int value; };
int main(void) { struct walk_dot *pointer = 0; return sizeof pointer.value; }
EOF
try_compile_error_message "Cannot dereference non-pointer in sizeof" << EOF
int main(void) { char byte = 0; return sizeof *byte; }
EOF
try_compile_error_message "Cannot apply square operator to non-pointer" << EOF
int main(void) { int scalar = 0; return sizeof scalar[0]; }
EOF
try_compile_error_message "sizeof(function) is invalid" << EOF
int walk_function(void) { return 0; }
int main(void) { int (*callback)(void) = walk_function; return sizeof *callback; }
EOF
try_compile_error_message "lvalue required as unary '&' operand" << EOF
int main(void) { char byte = 0; return sizeof &(char) byte; }
EOF

# The operand of an unparenthesized sizeof ends at its identifier; a following
# "+ 1" adds to the size rather than stepping the pointer being measured.
try_ 0 << EOF
int main(void)
{
    int values[4];
    int *cursor = values;
    char byte = 0;

    if (sizeof cursor + 1 != sizeof(int *) + 1) return 1;
    if (sizeof values + 1 != 4 * sizeof(int) + 1) return 2;
    if (sizeof byte + 1 != 2) return 3;
    if (sizeof (cursor) + 1 != sizeof(int *) + 1) return 4;
    return 0;
}
EOF
try_ 12 << EOF
typedef struct { int values[2][3]; } sizeof_pointer_alias_inner;
typedef sizeof_pointer_alias_inner *sizeof_pointer_alias;
struct sizeof_pointer_alias_holder { sizeof_pointer_alias rows[2]; };
int main(void) {
    struct sizeof_pointer_alias_holder value;
    return sizeof(value.rows[1]->values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_postfix_inner { int values[2][3]; };
struct sizeof_postfix_holder { struct sizeof_postfix_inner rows[2]; };
static struct sizeof_postfix_holder sizeof_postfix_value;
static int sizeof_postfix_row = sizeof(sizeof_postfix_value.rows[1].values[1]);
int main(void) { return sizeof_postfix_row; }
EOF
try_ 12 << EOF
struct sizeof_postfix_pointer_inner { int values[2][3]; };
struct sizeof_postfix_pointer_holder { struct sizeof_postfix_pointer_inner *rows[2]; };
static struct sizeof_postfix_pointer_inner sizeof_postfix_pointer_inner_value;
static struct sizeof_postfix_pointer_holder sizeof_postfix_pointer_value =
    { &sizeof_postfix_pointer_inner_value };
static int sizeof_postfix_pointer_row =
    sizeof(sizeof_postfix_pointer_value.rows[0]->values[1]);
int main(void) { return sizeof_postfix_pointer_row; }
EOF
try_ 12 << EOF
struct sizeof_double_pointer_inner { int values[2][3]; };
struct sizeof_double_pointer_holder { struct sizeof_double_pointer_inner **rows[2]; };
int main(void) {
    struct sizeof_double_pointer_holder value;
    return sizeof((**value.rows[1]).values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_double_pointer_inner { int values[2][3]; };
struct sizeof_double_pointer_holder { struct sizeof_double_pointer_inner **rows[2]; };
int main(void) {
    struct sizeof_double_pointer_holder value;
    int index = 0;
    return sizeof((**value.rows[index++]).values[index++]) + index;
}
EOF
try_ 12 << EOF
struct sizeof_dp_global_inner { int values[2][3]; };
struct sizeof_dp_global_holder { struct sizeof_dp_global_inner **rows[2]; };
static struct sizeof_dp_global_holder sizeof_dp_global_value;
static int sizeof_dp_global_row =
    sizeof((**sizeof_dp_global_value.rows[1]).values[1]);
int main(void) { return sizeof_dp_global_row; }
EOF
try_ 12 << EOF
struct sizeof_deref_inner { int values[2][3]; };
struct sizeof_deref_holder { struct sizeof_deref_inner *rows[2]; };
int main(void) {
    struct sizeof_deref_holder value;
    return sizeof((*value.rows[1]).values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_deref_inner { int values[2][3]; };
struct sizeof_deref_holder { struct sizeof_deref_inner *rows[2]; };
int main(void) {
    struct sizeof_deref_holder value;
    int index = 0;
    return sizeof((*value.rows[index++]).values[index++]) + index;
}
EOF
try_ 12 << EOF
struct sizeof_deref_global_inner { int values[2][3]; };
struct sizeof_deref_global_holder { struct sizeof_deref_global_inner *rows[2]; };
static struct sizeof_deref_global_holder sizeof_deref_global_value;
static int sizeof_deref_global_row =
    sizeof((*sizeof_deref_global_value.rows[1]).values[1]);
int main(void) { return sizeof_deref_global_row; }
EOF
try_ 12 << EOF
struct sizeof_grouped_row_inner { int values[2][3]; };
struct sizeof_grouped_row_holder { struct sizeof_grouped_row_inner *rows[2]; };
int main(void) {
    struct sizeof_grouped_row_holder value;
    return sizeof((value.rows[1])->values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_grouped_row_inner { int values[2][3]; };
struct sizeof_grouped_row_holder { struct sizeof_grouped_row_inner *rows[2]; };
int main(void) {
    struct sizeof_grouped_row_holder value;
    int index = 0;
    return sizeof((value.rows[index++])->values[index++]) + index;
}
EOF
try_ 12 << EOF
struct sizeof_gr_global_inner { int values[2][3]; };
struct sizeof_gr_global_holder { struct sizeof_gr_global_inner *rows[2]; };
static struct sizeof_gr_global_holder sizeof_gr_global_value;
static int sizeof_gr_global_row =
    sizeof((sizeof_gr_global_value.rows[1])->values[1]);
int main(void) { return sizeof_gr_global_row; }
EOF
try_ 12 << EOF
struct sizeof_inner_group_inner { int values[2][3]; };
struct sizeof_inner_group_holder { struct sizeof_inner_group_inner *rows[2]; };
int main(void) {
    struct sizeof_inner_group_holder value;
    return sizeof((*(value.rows[1])).values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_inner_group_inner { int values[2][3]; };
struct sizeof_inner_group_holder { struct sizeof_inner_group_inner *rows[2]; };
int main(void) {
    struct sizeof_inner_group_holder value;
    int index = 0;
    return sizeof((*(value.rows[index++])).values[index++]) + index;
}
EOF
try_ 12 << EOF
struct sizeof_ig_global_inner { int values[2][3]; };
struct sizeof_ig_global_holder { struct sizeof_ig_global_inner *rows[2]; };
static struct sizeof_ig_global_holder sizeof_ig_global_value;
static int sizeof_ig_global_row =
    sizeof((*(sizeof_ig_global_value.rows[1])).values[1]);
int main(void) { return sizeof_ig_global_row; }
EOF
try_ 12 << EOF
struct sizeof_walker_inner { int values[2][3]; };
struct sizeof_walker_holder { struct sizeof_walker_inner rows[2]; };
int main(void) {
    struct sizeof_walker_holder value;
    return sizeof((value.rows)[1].values[1]);
}
EOF
try_ 12 << EOF
struct sizeof_walker_inner { int values[2][3]; };
struct sizeof_walker_holder { struct sizeof_walker_inner *rows[2]; };
int main(void) {
    struct sizeof_walker_holder value;
    int index = 0;
    return sizeof((value.rows)[index++]->values[index++]) + index;
}
EOF
try_ 12 << EOF
struct sizeof_walker_global_inner { int values[2][3]; };
struct sizeof_walker_global_holder { struct sizeof_walker_global_inner rows[2]; };
static struct sizeof_walker_global_holder sizeof_walker_global_value;
static int sizeof_walker_global_row =
    sizeof((sizeof_walker_global_value.rows)[1].values[1]);
int main(void) { return sizeof_walker_global_row; }
EOF
try_ 13 << EOF
struct sizeof_wgb_inner { int values[2][3]; };
struct sizeof_wgb_holder { struct sizeof_wgb_inner rows[2]; };
static struct sizeof_wgb_holder sizeof_wgb_value;
static int sizeof_wgb_row =
    sizeof sizeof_wgb_value.rows[1].values[1] + 1;
int main(void) { return sizeof_wgb_row; }
EOF
try_ 13 << EOF
struct sizeof_walker_boundary_inner { int values[2][3]; };
struct sizeof_walker_boundary_holder { struct sizeof_walker_boundary_inner rows[2]; };
int main(void) {
    struct sizeof_walker_boundary_holder value;
    return sizeof value.rows[1].values[1] + 1;
}
EOF
try_ 12 << EOF
typedef struct { int values[2][3]; } sizeof_walker_alias_inner;
typedef sizeof_walker_alias_inner *sizeof_walker_alias;
struct sizeof_walker_alias_holder { sizeof_walker_alias rows[2]; };
int main(void) {
    struct sizeof_walker_alias_holder value;
    return sizeof((*value.rows[1]).values[1]);
}
EOF
items 12 "struct holder { int values[2][3]; }; struct holder value; return sizeof value.values[0];"
items 12 "struct holder { int values[2][3]; }; struct holder value; int index = 0; return sizeof((value.values[index++])) + index;"
items 12 "struct holder { int values[2][3]; }; struct holder value; return sizeof((((value.values)))[1]);"
items 12 "struct holder { int values[2][3]; }; struct holder value; int index = 0; return sizeof((((value.values)))[index++]) + index;"
items 12 "struct holder { int values[2][3]; }; struct holder value; return sizeof((((value.values[1]))));"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner inner; }; struct holder value; return sizeof((((value.inner.values)))[1]);"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner inner; }; struct holder value; int index = 0; return sizeof((((value.inner.values)))[index++]) + index;"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner inner; }; struct holder value; return sizeof((((value.inner.values[1]))));"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner *inner; }; struct holder value; return sizeof((((value.inner->values)))[1]);"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner *inner; }; struct holder value; int index = 0; return sizeof((((value.inner->values)))[index++]) + index;"
items 12 "struct inner { int values[2][3]; }; struct holder { struct inner *inner; }; struct holder value; return sizeof((((value.inner->values[1]))));"
try_ 12 << EOF
struct grouped_global_holder { int values[2][3]; };
static struct grouped_global_holder grouped_global_value;
static int grouped_global_row_size =
    sizeof((((grouped_global_value.values)))[1]);
int main(void) { return grouped_global_row_size; }
EOF
try_ 12 << EOF
struct grouped_nested_inner { int values[2][3]; };
struct grouped_nested_holder { struct grouped_nested_inner inner; };
static struct grouped_nested_holder grouped_nested_value;
static int grouped_nested_row_size =
    sizeof((((grouped_nested_value.inner.values)))[1]);
int main(void) { return grouped_nested_row_size; }
EOF
try_ 12 << EOF
struct grouped_arrow_inner { int values[2][3]; };
struct grouped_arrow_holder { struct grouped_arrow_inner *inner; };
static struct grouped_arrow_inner grouped_arrow_inner_value;
static struct grouped_arrow_holder grouped_arrow_value = { &grouped_arrow_inner_value };
static int grouped_arrow_row_size =
    sizeof((((grouped_arrow_value.inner->values)))[1]);
int main(void) { return grouped_arrow_row_size; }
EOF
items 4 "struct holder { int values[2][3]; }; struct holder value; return sizeof(value.values[0][0]);"
items 12 "struct holder { int values[2][3]; }; struct holder *value = 0; return sizeof(value->values[0]);"
items 12 "struct holder { int values[3]; }; struct holder *value = 0; return sizeof(value->values);"
items 12 "struct holder { int values[3]; }; struct holder *value = 0; return sizeof((*value).values);"
items 12 "struct holder { int values[3]; }; struct holder *value = 0; return sizeof (*value).values;"
try_ 15 << EOF
typedef struct { int values[3]; } holder_t;
typedef holder_t holder_alias;
typedef holder_alias *holder_ptr;
int main(void) {
    holder_alias value = {{1, 2, 3}};
    holder_ptr pointer = &value;
    return pointer->values[2] + sizeof(pointer->values);
}
EOF
try_ 20 << EOF
struct nested { int values[2]; };
struct holder { int values[3]; struct nested nested; };
int main(void) {
    struct holder value;
    return sizeof(value.values) + sizeof(value.nested.values);
}
EOF
try_ 8 << EOF
int main(void)
{
    static char buffer[8] = "hi";
    return sizeof(buffer);
}
EOF
items 4 "int arr[5]; return sizeof(arr[0]);"
items 32 "int values[2][2][2][2]; int index = 0; return sizeof(values[index++]) + index;"
items 16 "int values[2][2][2][2]; return sizeof(values[0][0]);"
items 8 "int values[2][2][2][2]; return sizeof(values[0][0][0]);"
items 4 "int rows[2][1]; return sizeof(rows[0]);"
items 4 "int rows[2][1]; return sizeof(rows[0][0]);"
try_ 4 << EOF
struct sizeof_singleton_inner { int values[1]; };
struct sizeof_singleton_holder { struct sizeof_singleton_inner rows[2][1]; };
int main(void) {
    struct sizeof_singleton_holder value;
    int index = 0;
    return sizeof(value.rows[index++][0].values) + index;
}
EOF
try_ 4 << EOF
struct sizeof_singleton_static_inner { int values[1]; };
struct sizeof_singleton_static_holder {
    struct sizeof_singleton_static_inner rows[2][1];
};
static struct sizeof_singleton_static_holder value;
static int size = sizeof value.rows[0][0].values;
int main(void) { return size; }
EOF
items 4 "int value = 0; return sizeof(value = 9);"
items 7 "int value = 3; int size = sizeof(value = 9); return value + size;"
items 4 "int value = 0; return sizeof((value = 9, value)) + value;"
try_ 32 << EOF
int sizeof_slot_source[2][2][2][2];
int (*sizeof_slot[])[2][2] = { &sizeof_slot_source[0][1] + 1 };
static int sizeof_slot_static = sizeof(*sizeof_slot[0]);
int main(void) {
    int index = 0;
    return sizeof(*sizeof_slot[index++]) + sizeof_slot_static + index;
}
EOF
try_ 16 << EOF
int sizeof_repeated_slot_source[2][2];
int (*sizeof_repeated_slots[])[2] = {
    sizeof_repeated_slot_source, sizeof_repeated_slot_source
};
static int sizeof_repeated_slot_static = sizeof(*sizeof_repeated_slots[1]);
int main(void) {
    int index = 1;
    return sizeof(*sizeof_repeated_slots[index++]) +
           sizeof_repeated_slot_static + index - 1;
}
EOF
try_ 9 << EOF
int main(void) {
    int sizeof_automatic_slot_source[2][2];
    int (*sizeof_automatic_slots[])[2] = {
        sizeof_automatic_slot_source, sizeof_automatic_slot_source
    };
    int index = 1;
    return sizeof(*sizeof_automatic_slots[index++]) + index;
}
EOF
items 4 "int x = 10; int *ptr = &x; return sizeof(*ptr);"
items 1 "char c = 'A'; return sizeof(c);"
items 2 "short s = 100; return sizeof(s);"
items 4 "int a = 1, b = 2; return sizeof(a + b);"
items 3 "return (1, 3);"
items 4 "int value = 1; return (value++, value + 2);"
items 3 "return (1, (2, 3));"
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

# A row or element of an array of pointers is pointer-sized per element.
try_ 7 << EOF
int main(void) {
    int *rows[2][3];
    return (sizeof rows[0] == 3 * sizeof(int *)) +
           2 * (sizeof(rows[1]) == 3 * sizeof(int *)) +
           4 * (sizeof rows[1][2] == sizeof(int *));
}
EOF

# The extent of a string literal operand counts the bytes after an embedded null
# character, in each adjacent literal.
try_ 3 << EOF
int main(void) {
    int grouped = sizeof("a\0bc");
    int adjacent = sizeof "a\0" "b\0c";
    return (grouped == 5) + 2 * (adjacent == 6);
}
EOF

# Every sizeof result is an integer constant expression, so a zero-valued
# expression built from sizeof of an object or string literal is a null pointer
# constant next to a function pointer.
try_ 15 << EOF
int three(void) { return 3; }
int main(void) {
    int values[4];
    int (*callback)(void) = three;
    return (callback != !sizeof values) +
           2 * (callback != sizeof(values) - sizeof(values)) +
           4 * ((1 ? callback : !sizeof "ab")() == 3) +
           8 * ((0 ? !sizeof(values[0]) : callback)() == 3);
}
EOF

# A global initializer takes sizeof of a parenthesized narrow or wide string
# literal, with or without extra grouping, as the size of the literal array.
try_ 5 << EOF
int global_sizeof_string = sizeof("abc");
int global_sizeof_grouped_string = (sizeof(("a" "bc")));
int global_sizeof_string_sum = 1 + sizeof("a\0b");
int global_sizeof_wstring = sizeof(L"ab") / sizeof(L"");
int global_sizeof_grouped_wstring = sizeof(((L"a" L"b"))) / sizeof(L"");
int main(void) {
    return (global_sizeof_string == 4) + (global_sizeof_grouped_string == 4) +
           (global_sizeof_string_sum == 5) + (global_sizeof_wstring == 3) +
           (global_sizeof_grouped_wstring == 3);
}
EOF

# Grouping around a literal does not decay it inside sizeof in a function body
# either; an operator after the group still makes an ordinary expression.
try_ 6 << EOF
int main(void) {
    return (sizeof(("abc")) == 4) + (sizeof((("a" "b"))) == 3) +
           (sizeof((L"ab")) == 3 * sizeof(L"")) +
           (sizeof(((L"a" L"b"))) == 3 * sizeof(L"")) +
           (sizeof(("abc") + 1) == sizeof(char *)) +
           (sizeof ("a" "bc") == 4);
}
EOF

# Every integer constant expression evaluates sizeof alike: a string literal
# keeps its array size, a dereference takes the pointee type, and a typed
# literal such as 1LL takes the size its suffix selects.
try_ 0 << EOF
short *constant_short_pointer;
int global_dereference_size = sizeof *constant_short_pointer;
int global_dereference_sum = sizeof *constant_short_pointer + 1;
int global_long_long_size = sizeof(1LL) + sizeof 2LL;
int string_bound[sizeof "abc"];
int grouped_string_bound[sizeof(("a" "bc"))];
int dereference_bound[sizeof *constant_short_pointer];
enum {
    string_enum = sizeof "abcd",
    grouped_string_enum = sizeof((("ab"))),
    dereference_enum = sizeof(*constant_short_pointer),
    long_long_enum = sizeof 1LL + sizeof('a')
};
int main(void)
{
    char *local_pointer = 0;
    int local_string_bound[sizeof "ab" "c"];
    int local_long_long_bound[sizeof 1LL];
    int local_dereference_bound[sizeof *local_pointer];
    switch (sizeof(long long)) {
    case sizeof "abcdef" + sizeof *local_pointer:
        break;
    default:
        return 1;
    }
    switch (sizeof(long long)) {
    case sizeof 1LL:
        break;
    default:
        return 2;
    }
    return global_dereference_size != sizeof(short) ||
           global_dereference_sum != sizeof(short) + 1 ||
           global_long_long_size != 2 * sizeof(long long) ||
           sizeof(string_bound) != 4 * sizeof(int) ||
           sizeof(grouped_string_bound) != 4 * sizeof(int) ||
           sizeof(dereference_bound) != sizeof(short) * sizeof(int) ||
           string_enum != 5 || grouped_string_enum != 3 ||
           dereference_enum != sizeof(short) ||
           long_long_enum != sizeof(long long) + sizeof(int) ||
           sizeof(local_string_bound) != 4 * sizeof(int) ||
           sizeof(local_long_long_bound) != sizeof(long long) * sizeof(int) ||
           sizeof(local_dereference_bound) != sizeof(int);
}
EOF

# A literal too wide for an int, or unsigned beyond INT_MAX, keeps its value and
# type in every integer constant expression: an array bound, a designator, a
# bit-field width, a pointer-to-array bound or a compound literal bound. A
# result an int cannot hold is diagnosed rather than wrapped.
try_ 9 << EOF
#include <stddef.h>
struct wide_bound_record { int a; int b; };
int wide_bound_quotient[3000000000U / 1000000000U];
int wide_bound_difference[0x100000003ULL - 0x100000000ULL];
int wide_bound_signed[2147483648 / 1073741824];
int wide_bound_designated[5] = {[4294967299 - 4294967296] = 7};
char wide_bound_offset[16U - offsetof(struct wide_bound_record, b)];
struct wide_bound_bits { unsigned field : 4294967299 - 4294967296; } wide_bound_bits;
int main(void) {
    int local[3000000000U / 1500000000U];
    int (*rows)[0x100000004ULL - 0x100000000ULL] = 0;
    int *literal = (int[2U]){5, 6};
    wide_bound_bits.field = 7;
    return (sizeof(wide_bound_quotient) == 3 * sizeof(int)) +
           (sizeof(wide_bound_difference) == 3 * sizeof(int)) +
           (sizeof(wide_bound_signed) == 2 * sizeof(int)) +
           (wide_bound_designated[3] == 7) +
           (sizeof(wide_bound_offset) == 12) +
           (wide_bound_bits.field == 7) +
           (sizeof(local) == 2 * sizeof(int)) +
           (sizeof(*rows) == 4 * sizeof(int)) + (literal[1] == 6);
}
EOF
try_compile_error << EOF
int wide_bound_wrapped[4294967297];
int main(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { int *p = (int[4294967298]){1, 2}; return p[1]; }
EOF
try_compile_error << EOF
struct wide_width { unsigned field : 4294967297; };
int main(void) { return 0; }
EOF

# Category: Switch Statements
begin_category "Switch Statements" "Testing switch-case control flow"

# switch-case
items 10 "int a; a = 0; switch (3) { case 0: return 2; case 3: a = 10; break; case 1: return 0; } return a;"
items 10 "int a; a = 0; switch (3) { case 0: return 2; default: a = 10; break; } return a;"
items 7 "int value = 1; switch (value++, value + 1) { case 3: return value + 5; default: return 0; }"
items 2 "switch (0 ? 1 : 2) { case 2: return 2; default: return 0; }"
try_compile_error << EOF
int main(void) { int value = 0; switch (&value) { default: return 0; } }
EOF
try_compile_error << EOF
struct switch_record { int value; };
int main(void) {
    struct switch_record value = { 0 };
    switch (value) { default: return 0; }
}
EOF
try_compile_error << EOF
void switch_void(void) {}
int main(void) { switch (switch_void()) { default: return 0; } }
EOF
try_ 2 << EOF
int main(void) {
    unsigned char byte = 1;
    switch (byte) { case 1: return 2; default: return 0; }
}
EOF

# A case constant converts to the promoted controlling type: a long long switch
# keeps every word of a wide or unsigned label, an int switch folds it.
try_ 0 << EOF
int wide(long long x) {
    switch (x) {
    case -2: return 1;
    case 1 ? 0x200000000LL : 3: return 2;
    case (0x100000000LL << 1) + 1: return 3;
    case 0xffffffffU: return 4;
    case -0x7fffffffffffffffLL - 1: return 6;
    default: return 5;
    }
}
int narrow(unsigned x) {
    switch (x) {
    case 0x100000002LL: return 1;
    default: return 2;
    }
}
int main(void) {
    if (wide(-2) != 1 || wide(0x200000000LL) != 2 || wide(0x200000001LL) != 3)
        return 1;
    if (wide(0xffffffffLL) != 4 || wide(-1) != 5 || wide(1) != 5)
        return 2;
    if (wide(-0x7fffffffffffffffLL - 1) != 6 || wide(0x100000002LL) != 5)
        return 3;
    return narrow(2) != 1 || narrow(0x100000002LL) != 1;
}
EOF

# Category: Enumerations
begin_category "Enumerations" "Testing enum declarations and usage"

# enum
try_ 6 << EOF
typedef enum { enum1 = 5, enum2 } enum_t;
int main() { enum_t v = enum2; return v; }
EOF

# A typedef may name an existing enum tag, define a tagged enum, qualify it or
# derive a pointer or array from it, at file scope and in a block.
try_ 20 << EOF
enum E { A, B = 5 };
typedef enum E EE;
typedef enum E *EP;
typedef enum E const CE;
typedef enum { C = 3 } CA[2];
typedef enum F { D = 1 } volatile VF;
EE e = B;
CE ce = A;
CA arr = { C, D };
int main(void) {
    EP p = &e;
    VF v = D;
    enum F w = v;
    typedef enum E LE;
    LE le = B;
    return *p + ce + arr[0] + sizeof(CA) / sizeof(int) + w + le + (int) sizeof(EE);
}
EOF

try_ 13 << EOF
int main(void) {
    typedef enum E { A, B = 7 } EE;
    EE e = B;
    typedef enum { C = 2 } const CE;
    CE c = C;
    typedef volatile enum E VE;
    VE v = A;
    typedef enum E *EP;
    EP q = &e;
    int got = *q;
    enum E again = got;
    return again + c + v + sizeof(EE);
}
EOF

try_compile_error << EOF
enum E { A };
typedef enum E const CE;
CE value = A;
int main(void) { value = A; return 0; }
EOF

try_compile_error << EOF
int main(void) { enum E { A }; typedef const enum E CE; CE c = A; c = A; return c; }
EOF

try_compile_error_message "Unknown enum type" << EOF
int main(void) { typedef enum missing M; return 0; }
EOF

# C99 has no incomplete enum types (6.7.2.3p2 needs the list first), so an enum
# tag without a visible definition is diagnosed wherever it is named, and a bare
# reference to a complete one declares nothing (6.7p2).
try_compile_error_message "C99 forbids forward references to enums" << EOF
enum forward_only;
int main(void) { return 0; }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
int main(void) { enum forward_only; return 0; }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
int takes_forward(enum forward_only *p);
int main(void) { return 0; }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
struct holds_forward { enum forward_only *p; };
int main(void) { return 0; }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
int forward_bound[sizeof(enum forward_only)];
int main(void) { return 0; }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
int main(void) { return sizeof(enum forward_only); }
EOF
try_compile_error_message "C99 forbids forward references to enums" << EOF
int main(void) { return (enum forward_only) 0; }
EOF
try_compile_error_message "enum declaration without an enumerator list declares nothing" << EOF
enum complete_first { complete_a, complete_b };
enum complete_first;
int main(void) { return 0; }
EOF
try_compile_error_message "enum declaration without an enumerator list declares nothing" << EOF
enum complete_first { complete_a, complete_b };
int main(void) { enum complete_first; return 0; }
EOF
try_ 3 << EOF
enum complete_later { later_a, later_b };
enum complete_later file_value = later_b;
int main(void) {
    enum complete_later local = later_b;
    enum complete_later *p = &local;
    return file_value + *p + (sizeof(enum complete_later) == sizeof(int));
}
EOF
try_ 2 << EOF
enum trailing_values { trailing_first = 2, };
int main(void) { return trailing_first; }
EOF
try_compile_error << EOF
enum invalid_values { last_int = 2147483647, out_of_range };
EOF
try_compile_error << EOF
enum explicit_out_of_range { value = 2147483648 };
EOF
try_compile_error << EOF
enum expression_out_of_range { value = 2147483647 + 1 };
EOF
try_compile_error << EOF
enum unsigned_out_of_range { value = 0xffffffffU };
EOF
try_ 1 << EOF
enum signed_minimum { value = -2147483648 };
int main(void) { return value < 0; }
EOF
try_ 1 << EOF
enum suffixed_signed_minimum { value = -2147483648L };
int main(void) { return value < 0; }
EOF
try_ 0 << EOF
enum conditional_range { value = 0 ? 2147483648 : 1 };
int main(void) { return value - 1; }
EOF
try_ 0 << EOF
enum unsigned_int_range { zero = 0U, maximum = 2147483647U,
                          value = -1U + 1U };
int main(void) { return zero + maximum - 2147483647 + value; }
EOF
try_ 0 << EOF
enum wide_intermediates { one = 2147483648L - 2147483647L,
                          zero = 2147483648L - 2147483648L,
                          minimum = -2147483649L + 1L };
int main(void) { return one + zero + (minimum < 0) - 2; }
EOF
try_ 1 << EOF
enum explicit_reset { maximum = 2147483647, reset = 0, successor };
int main(void) { return successor; }
EOF
try_ 2 << EOF
enum typed_named_values { first = 1, second = first + 1U };
int main(void) { return second; }
EOF
try_ 0 << EOF
enum unsigned_wrap_values { first = 4294967295U + 1U,
                            second = 2147483647U + 2147483649U,
                            third = 0x80000000 * 2,
                            fourth = 0xffffffffL + 1L,
                            fifth = 0x80000000L * 2L };
int main(void) { return first + second + third + fourth + fifth; }
EOF
try_ 0 << EOF
enum typed_sizeof_values { first = sizeof(int) + 1U,
                           second = 0 ? sizeof(int) : 1U };
int main(void) { return first + second - 6; }
EOF

# A character constant is an int operand of the typed evaluator, so it can be
# shifted and extended at int width like a number.
try_ 5 << EOF
enum typed_char_values { first = 'a' << 1U, second = ('x' << 2) + 0U,
                         third = 'b' >> 1U, fourth = -'c' >> 1U };
int main(void) {
    int hit = 0;
    switch (194) {
    case 'a' << 1U:
        hit = 1;
    }
    return (first == 194) + (second == 480) + (third == 49) +
           (fourth == -50) + hit;
}
EOF
try_ 1 << EOF
enum conditional_wide_rank { first = 1 ? -1LL : 1U,
                             second = first + 1U };
int main(void) { return (first < 0) + second; }
EOF
try_ 1 << EOF
enum preserved_wide_rank { value = (-2147483648LL / 1LL) + 2147483647U };
int main(void) { return value < 0; }
EOF
try_ 3 << EOF
int main(void) {
    enum typed_local_values { first = 2, second = first + 1U };
    return second;
}
EOF
try_compile_error << EOF
enum negative_out_of_range { value = -2147483649 };
EOF
try_compile_error << EOF
enum implicit_out_of_range { maximum = 2147483647, successor };
EOF
try_compile_error << EOF
enum unary_out_of_range { value = -(-2147483648) };
EOF
try_compile_error << EOF
enum named_unary_out_of_range { minimum = -2147483648, value = -minimum };
EOF
try_compile_error << EOF
enum shifted_out_of_range { value = 1 << 31 };
EOF
try_compile_error << EOF
enum typed_shift_count_out_of_range { value = 1U << 32 };
EOF
try_compile_error << EOF
enum typed_signed_shift_out_of_range { value = (1LL << 63) >> 63 };
EOF

# A shift count outside the int width has no value in any integer constant
# expression, so the compiler must reject it rather than fold it on the host.
try_compile_error << EOF
int shifted_global = 1 << 32;
int main(void) { return shifted_global; }
EOF
try_compile_error << EOF
int main(void) { int values[-1 >> 40]; return sizeof(values); }
EOF
try_compile_error << EOF
int main(void) { switch (1) { case 1 << -1: return 1; } return 0; }
EOF
try_ 12 << EOF
enum { shift_base = 1 << 30 };
int shift_bound[(shift_base >> 29) + (3 >> 1)];
int shift_global = -8 >> 1;
int main(void)
{
    switch (4) {
    case 16 >> 2:
        return sizeof(shift_bound) / sizeof(int) + shift_global + 13;
    }
    return 0;
}
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

# C99 leaves an enum's compatible integer type implementation-defined. shecc
# selects int, so the choice must remain consistent for objects, aggregate
# layout, conversions, and the ordinary multi-argument call ABI.
try_ 0 << EOF
enum representation_state { representation_low = -3, representation_high = 7 };
struct enum_representation_record {
    char first;
    enum representation_state state;
    char last;
};
union enum_representation_union {
    enum representation_state state;
    int integer;
};
enum representation_state pass_representation(int a, int b, int c, int d,
                                                int e, int f,
                                                enum representation_state state)
{
    return (enum representation_state) (state + a - b + c - d + e - f);
}
enum representation_state return_negative_representation(void)
{
    return representation_low;
}
int main(void)
{
    enum representation_state values[2] = {
        representation_low, representation_high
    };
    struct enum_representation_record record = { 0, representation_high, 0 };
    union enum_representation_union value = { representation_low };
    return sizeof(enum representation_state) != sizeof(int) ||
           sizeof values != 2 * sizeof(int) ||
           /* Current shecc ABI: int fields align to four bytes. */
           sizeof record != 12 || sizeof value != sizeof(int) ||
           (int) values[0] != -3 || value.integer != -3 ||
           (int) return_negative_representation() != -3 ||
           pass_representation(1, 2, 3, 4, 5, 6, record.state) != 4;
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
cp -R "$TESTS_DIR/include-nested" "$TEST_TMPDIR/include-nested"
try_file 24 "$TESTS_DIR/include-main.c"

# line changes diagnostics and __FILE__, not the physical directory used to
# resolve a following quoted include.
try_ 7 << EOF
#line 10 "generated/virtual.c"
#include "include-base.h"
int main(void) { return QUOTED_INCLUDE_BASE; }
EOF

# A function-like macro name without an argument list is not expanded, so it
# cannot supply a header name, either directly or at the end of an alias chain.
try_compile_error_message "#include macro must expand to a header name" << EOF
#define BASE_HEADER() "include-base.h"
#include BASE_HEADER
int main(void) { return QUOTED_INCLUDE_BASE; }
EOF
try_compile_error_message "#include macro must expand to a header name" << EOF
#define BASE_HEADER() "include-base.h"
#define BASE_ALIAS BASE_HEADER
#include BASE_ALIAS
int main(void) { return QUOTED_INCLUDE_BASE; }
EOF

# White space before a trailing comment ends no replacement list, so it does not
# keep a macro from naming a header, directly or through an alias.
try_ 7 << EOF
#define BASE_HEADER "include-base.h" /* quoted */
#define BASE_ALIAS BASE_HEADER /* alias */
#include BASE_HEADER
#include BASE_ALIAS
int main(void) { return QUOTED_INCLUDE_BASE; }
EOF
try_flags 24 "-I$TESTS_DIR" << EOF
#define ANGLE_HEADER <include-angle.h> /* angle */
#include ANGLE_HEADER
int main(void) { return ANGLE_INCLUDE_BASE + ANGLE_INCLUDE_CHILD; }
EOF

# A header's #pragma once identity is its normalized relative path, not the
# spelling used by an includer. The outer header reaches the same header again
# through ./ and ../ components after its canonical spelling; a duplicate
# inclusion would define its object twice.
try_ 19 << EOF
#include "include-nested/outer.h"
int main(void) { return NESTED_ONCE_VALUE + nested_once_object; }
EOF

# _Pragma("once") guards its header as #pragma once does, and one produced by a
# macro from another header guards the header that invoked the macro.
printf '#define PRAGMA_OP(x) _Pragma(#x)\n' > "$TEST_TMPDIR/pragma-op-macro.h"
printf '_Pragma("once")\nint pragma_op_direct = 3;\n' \
    > "$TEST_TMPDIR/pragma-op-direct.h"
printf '#include "pragma-op-macro.h"\nPRAGMA_OP(once)\nint pragma_op_via = 4;\n' \
    > "$TEST_TMPDIR/pragma-op-via.h"
try_ 7 << EOF
#include "pragma-op-direct.h"
#include "pragma-op-via.h"
#include "pragma-op-direct.h"
#include "pragma-op-via.h"
int main(void) { return pragma_op_direct + pragma_op_via; }
EOF
try_flags 24 "-I$TESTS_DIR" << EOF
#include <include-angle.h>
#include <include-angle-child.h>
int main(void) { return ANGLE_INCLUDE_BASE + ANGLE_INCLUDE_CHILD; }
EOF
try_flags 24 "-I$TESTS_DIR" << EOF
#define ANGLE_HEADER <include-angle.h>
#include ANGLE_HEADER
int main(void) { return ANGLE_INCLUDE_BASE + ANGLE_INCLUDE_CHILD; }
EOF
try_flags 1 "--no-libc" << EOF
#include <stdbool.h>
int main(void) {
    bool value = true;
    return value == true && __bool_true_false_are_defined == 1;
}
EOF
try_flags 1 "--no-libc" << EOF
#include <iso646.h>
int main(void) {
    int value = 6;
    value and_eq 3;
    value or_eq 4;
    value xor_eq 1;
    return value == 7 and not (value not_eq 7) and
           ((5 bitand 3) == 1) and ((1 bitor 2) == 3) and
           ((1 xor 3) == 2) and ((compl 0) < 0);
}
EOF
try_flags 12 "--no-libc" << EOF
#include <limits.h>
#include <limits.h>
#if CHAR_BIT != 8 || INT_MAX != 2147483647
#error builtin limits.h macros are incorrect
#endif
int main(void) {
    return (SCHAR_MIN == -128) + (SCHAR_MAX == 127) +
           (UCHAR_MAX == 255U) + (CHAR_MIN == -128) +
           (CHAR_MAX == 127) + (SHRT_MIN == -32768) +
           (SHRT_MAX == 32767) + (USHRT_MAX == 65535U) +
           (INT_MIN < 0) + (UINT_MAX > INT_MAX) +
           (LONG_MIN < 0) + (ULONG_MAX > LONG_MAX);
}
EOF
try_flags 2 "--no-libc" << EOF
#include <limits.h>
int main(void) { return (LLONG_MIN < 0) + (ULLONG_MAX > LLONG_MAX); }
EOF
try_flags "$((8 + 2 * PTR_SZ))" "--no-libc" << EOF
#include <stddef.h>
#include <stddef.h>
int main(void) {
    int value = 3;
    int *pointer = NULL;
    size_t count = sizeof(pointer);
    ptrdiff_t delta = -2;
    wchar_t code = 65;
    return (pointer == NULL) + ((pointer ? 0 : value) == 3) +
           sizeof(count) + sizeof(delta) + sizeof(code) + (delta < 0) +
           (code == 65);
}
EOF
try_flags 4 "--no-libc" << EOF
#include <stddef.h>
struct inner { char tag; int value; };
struct outer { char lead; struct inner nested; short tail; };
typedef struct outer outer_t;
static int tail_offset = offsetof(outer_t, tail);
int main(void) {
    return (offsetof(struct outer, nested) == 4) +
           (offsetof(struct inner, value) == 4) +
           (offsetof(struct outer, nested.value) == 8) +
           (tail_offset == 12);
}
EOF
try_flags 4 "--no-libc" << EOF
#include <stddef.h>
struct offsetof_array_holder { int prefix; int values[3]; };
static int offsetof_array_value = offsetof(struct offsetof_array_holder, values[1]);
static int offsetof_array_one_past = offsetof(struct offsetof_array_holder, values[3]);
int main(void) {
    return (offsetof(struct offsetof_array_holder, values[1]) ==
            sizeof(int) + sizeof(int)) +
           (offsetof_array_value == sizeof(int) + sizeof(int)) +
           (offsetof(struct offsetof_array_holder, values[3]) ==
            offsetof(struct offsetof_array_holder, values) + 3 * sizeof(int)) +
           (offsetof_array_one_past == offsetof(struct offsetof_array_holder, values) +
            3 * sizeof(int));
}
EOF
try_compile_error << EOF
#include <stddef.h>
struct offsetof_scalar_holder { int value; };
int invalid_offsetof = offsetof(struct offsetof_scalar_holder, value[0]);
EOF
try_flags 4 "--no-libc" << EOF
#include <stddef.h>
struct offsetof_matrix_holder { int prefix; int values[2][3]; };
static int offsetof_matrix_value = offsetof(struct offsetof_matrix_holder, values[1][2]);
static int offsetof_matrix_one_past = offsetof(struct offsetof_matrix_holder, values[1][3]);
int main(void) {
    return (offsetof(struct offsetof_matrix_holder, values[1][2]) ==
            sizeof(int) + 5 * sizeof(int)) +
           (offsetof_matrix_value == sizeof(int) + 5 * sizeof(int)) +
           (offsetof(struct offsetof_matrix_holder, values[1][3]) ==
            sizeof(int) + 6 * sizeof(int)) +
           (offsetof_matrix_one_past == sizeof(int) + 6 * sizeof(int));
}
EOF
try_compile_error << EOF
#include <stddef.h>
struct offsetof_matrix_holder { int values[2][3]; };
int invalid_offsetof = offsetof(struct offsetof_matrix_holder, values[2][0]);
EOF
try_flags 4 "--no-libc" << EOF
#include <stddef.h>
struct offsetof_cube_holder { int prefix; int values[2][3][4]; };
static int offsetof_cube_value = offsetof(struct offsetof_cube_holder, values[1][2][3]);
static int offsetof_cube_one_past = offsetof(struct offsetof_cube_holder, values[1][2][4]);
int main(void) {
    return (offsetof(struct offsetof_cube_holder, values[1][2][3]) ==
            sizeof(int) + 23 * sizeof(int)) +
           (offsetof_cube_value == sizeof(int) + 23 * sizeof(int)) +
           (offsetof(struct offsetof_cube_holder, values[1][2][4]) ==
            sizeof(int) + 24 * sizeof(int)) +
           (offsetof_cube_one_past == sizeof(int) + 24 * sizeof(int));
}
EOF
try_flags 4 "--no-libc" << EOF
#include <stddef.h>
struct offsetof_hypercube_holder { int prefix; int values[2][2][2][2]; };
static int offsetof_hypercube_value = offsetof(struct offsetof_hypercube_holder, values[1][1][1][1]);
static int offsetof_hypercube_one_past = offsetof(struct offsetof_hypercube_holder, values[1][1][1][2]);
int main(void) {
    return (offsetof(struct offsetof_hypercube_holder, values[1][1][1][1]) ==
            sizeof(int) + 15 * sizeof(int)) +
           (offsetof_hypercube_value == sizeof(int) + 15 * sizeof(int)) +
           (offsetof(struct offsetof_hypercube_holder, values[1][1][1][2]) ==
            sizeof(int) + 16 * sizeof(int)) +
           (offsetof_hypercube_one_past == sizeof(int) + 16 * sizeof(int));
}
EOF
try_compile_error << EOF
#include <stddef.h>
struct offsetof_hypercube_holder { int values[2][2][2][2]; };
int invalid_offsetof = offsetof(struct offsetof_hypercube_holder, values[2][0][0][0]);
EOF
try_compile_error << EOF
#include <stddef.h>
struct offsetof_cube_holder { int values[2][3][4]; };
int invalid_offsetof = offsetof(struct offsetof_cube_holder, values[1][3][0]);
EOF
try_flags "$((38 + PTR_SZ))" "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return sizeof(int8_t) + sizeof(uint16_t) + sizeof(int32_t) +
           sizeof(uint64_t) + sizeof(int_least8_t) +
           sizeof(uint_least16_t) + sizeof(int_fast32_t) +
           sizeof(uint_fast64_t) + sizeof(intmax_t) + sizeof(uintptr_t);
}
EOF
try_flags 9 "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT8_MIN == -128) + (INT8_MAX == 127) + (UINT8_MAX == 255U) +
           (INT16_MIN == -32768) + (INT16_MAX == 32767) + (UINT16_MAX == 65535U) +
           (INT32_MIN < 0) + (INT32_MAX > 0) + (UINT32_MAX > INT32_MAX);
}
EOF
# INTPTR_MAX exceeds INT32_MAX only where pointers are eight bytes.
try_flags "$((PTR_SZ >= 8 ? 9 : 8))" "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT64_MIN < 0) + (INT64_MAX > 0) + (UINT64_MAX > INT64_MAX) +
           (INTMAX_MIN < 0) + (INTMAX_MAX > 0) + (UINTMAX_MAX > INTMAX_MAX) +
           (INTPTR_MIN < 0) + (INTPTR_MAX > INT32_MAX) + (UINTPTR_MAX > INTPTR_MAX);
}
EOF
try_flags 10 "--no-libc" << EOF
#include <stdint.h>
#include <signal.h>
#include <wchar.h>
int main(void) {
    sig_atomic_t signal_value = SIG_ATOMIC_MAX;
    wint_t wide_value = WINT_MAX;
    return (sizeof(size_t) == sizeof(ptrdiff_t)) +
           (sizeof(sig_atomic_t) == 4) + (sizeof(wint_t) == 4) +
           (PTRDIFF_MIN < 0) + (PTRDIFF_MAX > 0) +
           (SIZE_MAX > PTRDIFF_MAX) + (SIG_ATOMIC_MIN < 0) +
           (WCHAR_MIN < 0) + (WINT_MIN == 0U) +
           (signal_value > 0 && wide_value == WINT_MAX);
}
EOF
try_flags 12 "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT_LEAST8_MIN < 0) + (UINT_LEAST8_MAX > 0) +
           (INT_LEAST16_MIN < 0) + (UINT_LEAST16_MAX > 0) +
           (INT_LEAST32_MIN < 0) + (UINT_LEAST32_MAX > INT_LEAST32_MAX) +
           (INT_FAST8_MIN < 0) + (UINT_FAST8_MAX > INT_FAST8_MAX) +
           (INT_FAST16_MIN < 0) + (UINT_FAST16_MAX > INT_FAST16_MAX) +
           (INT_FAST32_MIN < 0) + (UINT_FAST32_MAX > INT_FAST32_MAX);
}
EOF
try_flags 4 "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT_LEAST64_MIN < 0) + (UINT_LEAST64_MAX > INT_LEAST64_MAX) +
           (INT_FAST64_MIN < 0) + (UINT_FAST64_MAX > INT_FAST64_MAX);
}
EOF
try_flags 12 "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT8_C(12) == 12) + (UINT8_C(12) == 12) +
           (INT16_C(12) == 12) + (UINT16_C(12) == 12) +
           (INT32_C(12) == 12) + (UINT32_C(12) == 12U) +
           (sizeof(INT8_C(12)) == 4) + (sizeof(UINT8_C(12)) == 4) +
           (sizeof(INT16_C(12)) == 4) + (sizeof(UINT16_C(12)) == 4) +
           (sizeof(INT32_C(12)) == 4) + (sizeof(UINT32_C(12)) == 4);
}
EOF
try_flags 8 "--no-libc" << EOF
#include <stdint.h>
int main(void) {
    return (INT64_C(12) == 12LL) + (UINT64_C(12) == 12ULL) +
           (INTMAX_C(12) == 12LL) + (UINTMAX_C(12) == 12ULL) +
           (sizeof(INT64_C(12)) == 8) + (sizeof(UINT64_C(12)) == 8) +
           (sizeof(INTMAX_C(12)) == 8) + (sizeof(UINTMAX_C(12)) == 8);
}
EOF
try_compile_error_message "Angle header not found in -I search paths" << EOF
#include <missing-shecc-header.h>
EOF

# An -I directory and header name that together exceed the path buffer must not
# be truncated: the truncated lookup missed this stdbool.h and silently used the
# built-in header instead. Diagnosing the length and reading the file both fail.
LONG_INCLUDE_DIR="$TEST_TMPDIR/$(printf 'd%.0s' {1..120})/$(printf 'e%.0s' {1..120})"
mkdir -p "$LONG_INCLUDE_DIR"
echo '#error the long include directory was searched' > "$LONG_INCLUDE_DIR/stdbool.h"
try_compile_error_flag "-I$LONG_INCLUDE_DIR" << EOF
#include <stdbool.h>
int main(void) { return 0; }
EOF
try_compile_error << EOF
#define FIRST_HEADER SECOND_HEADER
#define SECOND_HEADER FIRST_HEADER
#include FIRST_HEADER
int main(void) { return 0; }
EOF

# An angle header name is read through translation phases 1 and 2, so a
# backslash-newline or a trigraph splice inside it joins the name.
try_ 2 << 'EOF'
#include <std\
bool.h>
#include <stdbool.h??/
>
int main(void) { bool yes = true; return yes + (int) true; }
EOF
try_compile_error_message "unsupported platform configuration" << EOF
#error unsupported platform configuration
int main(void) { return 0; }
EOF
try_ 0 << EOF
#pragma vendor_extension ignored payload
int main(void) { return 0; }
EOF
try_ 0 << EOF
_Pragma("vendor_extension ignored payload")
#define DO_PRAGMA(value) _Pragma(#value)
DO_PRAGMA(another_extension ignored)
int main(void) { return 0; }
EOF
try_compile_error << EOF
_Pragma(123)
EOF
try_ 8 << EOF
??=define TRI_LEFT 4 ??/
+ 3
??=if TRI_LEFT == 7
??=define TRI_ENABLED 1
??=else
??=define TRI_ENABLED 0
??=endif
??=define JOIN_TRI(left, right) left ??=??= right
int JOIN_TRI(tri, graph) = TRI_LEFT;
int main(void) ??< int values??(2??) = ??< 3, trigraph ??>; char *word = "??/n"; return TRI_ENABLED * values??(1??) + (word??(0??) == '\n'); ??>
EOF
try_ 1 << EOF
int main(void) {
    // ??/
    return 0;
    return 1;
}
EOF
try_ 8 << EOF
int main(void) {
    char *raw = "a\
b";
    char *trigraph = "a??/
b";
    return (1 \
+ 2) + (1 ??/
+ 2) + (raw[1] == 'b') + (trigraph[1] == 'b');
}
EOF
try_ 7 << EOF
#def\
ine RAW_NAME ma\
in
??=def??/
ine TRI_VALUE 7
int RAW_NAME(void) { return TRI_VALUE; }
EOF
try_ 8 << EOF
%:define DIGRAPH_VALUE 7
%:if DIGRAPH_VALUE == 7
%:define DIGRAPH_ENABLED 1
%:else
%:define DIGRAPH_ENABLED 0
%:endif
%:define JOIN(left, right) left %:%: right
%:define STRINGIFY(value) %: value
int JOIN(di, graph) = DIGRAPH_VALUE;
int main(void) <% int values<:2:> = <% 3, digraph %>; char *word = STRINGIFY(ok); return DIGRAPH_ENABLED * values<:1:> + (word<:0:> == 'o'); %>
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

# An active invalid arithmetic operation in a #if expression must be diagnosed
# by the preprocessor rather than reaching host division/modulo behavior.
try_compile_error << EOF
#if 1 / 0
#endif
EOF
try_compile_error << EOF
#if 1 % 0
#endif
EOF

# A conditional nested inside a skipped group is skipped whole: its #elif, #else
# and #endif belong to it rather than to the group that encloses it.
try_ 0 << EOF
#if 0
#if 1
#endif
#endif
int main(void) { return 0; }
EOF
try_ 5 << EOF
#if 0
#ifdef __STDC__
#elif 1
#else
#endif
#elif 1
#define RESULT 5
#else
#define RESULT 0
#endif
int main(void) { return RESULT; }
EOF
try_ 7 << EOF
#ifndef __STDC__
#if 1
#ifdef __STDC__
#else
#ifndef UNKNOWN
#endif
#endif
#elif 0
#endif
#define RESULT 0
#else
#define RESULT 7
#endif
int main(void) { return RESULT; }
EOF

# Skipped groups several levels deep inside active ones, and an active group
# after a skipped #if and #elif chain at the same depth.
try_ 21 << EOF
#if 1
#define A 1
#if 0
#if 1
#if 1
#endif
#else
#endif
#define A 99
#elif 1
#ifdef UNKNOWN
#if 1
#endif
#elif 1
#define B 4
#endif
#else
#define B 99
#endif
#ifndef __STDC__
#if 0
#endif
#else
#define C 16
#endif
#endif
int main(void) { return A + B + C; }
EOF
try_compile_error_message "Unterminated conditional directive" << EOF
#if 0
#if 1
#endif
int main(void) { return 0; }
EOF
try_compile_error_message "Stray #endif" << EOF
#if 0
#if 1
#endif
#endif
#endif
int main(void) { return 0; }
EOF

# White space, comments included, may separate '#' from the directive name, and
# a line holding only '#' is the null directive (C99 6.10p2, 6.10.7).
try_ 9 << 'EOF'
#
#  define SIX 6
#	define THREE 3
 # /* comment */ ifdef SIX
%:  define NINE (SIX + THREE)
#	 endif
#	
# // a null directive with a comment
#/* a comment
   across lines */
int main(void) { return NINE; }
#
EOF
try_ 3 << 'EOF'
#  if 0
#  if 1
#
#  else
#  endif
#  define RESULT 0
#  elif 1
#	define RESULT 3
#  endif
int main(void) { return RESULT; }
EOF

# A skipped group may hold lines that are no directive shecc knows, while the
# same line in an active group is still rejected.
try_ 2 << 'EOF'
#if 0
#warning not a C99 directive
# unknown
#!
#endif
int main(void) { return 2; }
EOF
try_compile_error_message "Unsupported directive" << 'EOF'
# unknown
int main(void) { return 0; }
EOF
try_compile_error_message "Unsupported directive" << 'EOF'
#if 1
#else
#else_if
#endif
#  unknown_directive
int main(void) { return 0; }
EOF

# The '#' and '##' operators in replacement lists are not directives, spaced or
# not.
try_output 0 "[ab] 12 [c d]" << 'EOF'
#define STR(x) # x
#define CAT(a, b) a ## b
#define PAIR(x, y) %: x, %:y
int main(void)
{
    char *pair[2] = {PAIR(c, d)};
    printf("[%s] %d [%s %s]\n", STR(ab), CAT(1, 2), pair[0], pair[1]);
    return 0;
}
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

try_compile_error_message "Hexadecimal escape sequence out of range" << EOF
#if '\\x123' == 0x23
#endif
int main(void) { return 0; }
EOF

try_ 1 << EOF
#if '\\x0041' == 65
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

try_ 3 << EOF
#define PREPROCESSOR_WIDE_CHARACTER L'C'
#if L'A' == 65 && L'\x42' == 66 && PREPROCESSOR_WIDE_CHARACTER == 67
#define WIDE_CHARACTER_RESULT 3
#else
#define WIDE_CHARACTER_RESULT 0
#endif
int main(void) { return WIDE_CHARACTER_RESULT; }
EOF

try_ 1 << EOF
#include <stdint.h>
#if INT64_MIN < 0 && UINT64_MAX > 0 && UINT64_MAX > INT64_MAX && \
    (UINT64_MAX >> 63) == 1 && ~0ULL == UINT64_MAX && \
    UINT64_MAX + 1ULL == 0 && UINT64_MAX / 2ULL == INT64_MAX && \
    UINT64_MAX % 2ULL == 1 && -1LL > 1ULL && -2LL < -1LL && \
    (-2LL >> 1) == -1LL && (-1LL >> 33) == -1LL && \
    -7LL / 3LL == -2LL && -7LL % 3LL == -1LL && \
    0xffffffffffffffff == UINT64_MAX && \
    01777777777777777777777 == UINT64_MAX
#define WIDE_INTEGER_CONDITION 1
#else
#define WIDE_INTEGER_CONDITION 0
#endif
int main(void) { return WIDE_INTEGER_CONDITION; }
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

# The printf family follows C99 7.19.6.1 for everything but floating point: the
# -, +, space, # and 0 flags, width and precision given inline or by *, the hh,
# h, l, ll, j, z and t length modifiers, and %i, %X, %c, %s, %p, %n and %%. The
# expected text is glibc's. Only #, 0, width and l forms were accepted before.
printf_c99_ans=$(
    cat << 'EOF'
[42   ][ff   ][ab      ][z  ][    y]
[+5][-5][ 5][-5][+7]
[0xff][0XFF][010][0][0]
[-0042][-42  ][000ab][+0042][ 0042]
[007][][ -007][0a    ][]
[010][0][     005][ABCDEF][abcdef]
[     1][2   ][3   ][004][he][5]
[ab][       abc][x         ][][]
[0x1234][        0x10][0x20        ]
[(nil)][   (nil)]
[%][%]
[44][44][4464][4464][1170]
[-3][3000000000][feed][-9][10]
[-5000000000][18000000000][123456789a][ABCDEF012][77]
[-12345678901][12345678901][42][7][-3]
[-17][4294967295][777][ok]
abcdef
xyz|3 6 2 3 3
1   |+03|  0x11|k     |0
[                  -1][99                  ][-0000000000000000123][+5][ 6]
[-0000000042][0][0][0]
[-2147483648][2147483647]
    ab    |
12
EOF
)
try_output 0 "$printf_c99_ans" << EOF
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
int main(void)
{
    int n1 = 0, n2 = 0;
    signed char hn = 0;
    short sn = 0;
    long long lln = 0;
    int x = 0;
    char buf[64] = "abcdefgh";
    printf("[%-5d][%-5x][%-8s][%-3c][%5c]\n", 42, 255, "ab", 'z', 'y');
    printf("[%+d][%+d][% d][% d][%+ d]\n", 5, -5, 5, -5, 7);
    printf("[%#x][%#X][%#o][%#o][%#x]\n", 255, 255, 8, 0, 0);
    printf("[%05d][%-05d][%05x][%+05d][% 05d]\n", -42, -42, 171, 42, 42);
    printf("[%.3d][%.0d][%5.3d][%-6.2x][%.0x]\n", 7, 0, -7, 10, 0);
    printf("[%#.3o][%#.0o][%08.3d][%X][%x]\n", 8, 0, 5, 0xabcdef, 0xABCDEF);
    printf("[%*d][%-*d][%*d]", 6, 1, 4, 2, -4, 3);
    printf("[%.*d][%.*s][%.*d]\n", 3, 4, 2, "hello", -1, 5);
    printf("[%.2s][%10.3s][%-10.1s][%s][%.0s]\n", "abc", "abcdef", "xyz", "", "q");
    printf("[%p][%12p][%-12p]\n", (void *) 0x1234, (void *) 0x10, (void *) 0x20);
    printf("[%p][%8p]\n", (void *) 0, (void *) 0);
    printf("[%%][%5%]\n");
    printf("[%hhd][%hhu][%hd][%hu][%hx]\n", 300, 300, 70000, 70000, 70000);
    printf("[%ld][%lu][%lx][%li][%lo]\n", -3L, 3000000000UL, 0xfeedL, -9L, 8L);
    printf("[%lld][%llu][%llx][%llX][%lli]\n", -5000000000LL, 18000000000ULL,
           0x123456789aLL, 0xabcdef012LL, 77LL);
    printf("[%jd][%ju][%zu][%zd][%td]\n", (intmax_t) -12345678901LL,
           (uintmax_t) 12345678901ULL, (size_t) 42, (size_t) 7, (ptrdiff_t) -3);
    printf("[%i][%u][%o][%c%c]\n", -17, 4294967295U, 511, 'o', 'k');
    printf("abc%ndef%n\n", &n1, &n2);
    printf("xy%hhnz%hn%lln|", &hn, &sn, &lln);
    printf("%d %d %d %d %d\n", n1, n2, hn, sn, (int) lln);
    snprintf(buf, sizeof buf, "%-4d|%+.2d|%#6x|%-6s|%x", 1, 3, 17, "k", 0);
    printf("%s\n", buf);
    printf("[%20lld][%-20lld][%020lld][%+lld][% lld]\n", -1LL, 99LL, -123LL, 5LL, 6LL);
    printf("[%.10lld][%#llx][%#llo][%llu]\n", -42LL, 0LL, 0LL, 0ULL);
    printf("[%d][%d]\n", -2147483647 - 1, 2147483647);
    x = printf("%5s%-5s|\n", "a", "b");
    printf("%d\n", x);
    return 0;
}
EOF

# The printf family converts %u, the long forms %ld, %lu and %lx, and the long
# long forms %lld, %llu, %llx and %llo, whose argument takes two words on a
# 32-bit target and starts at an even slot.
try_ 0 << EOF
int main(void) {
    long long a = -1234567890123LL, c = 0x1122334455667788LL;
    unsigned long long b = 18446744073709551615ULL;
    char buf[160];
    snprintf(buf, sizeof buf, "%lld %llu %llx|%d %lld", a, b, c, 7, a);
    if (strcmp(buf, "-1234567890123 18446744073709551615 1122334455667788|"
                    "7 -1234567890123"))
        return 1;
    sprintf(buf, "%ld %lu %u %lx %5u", -5L, 4000000000UL, 4000000000U, 255L, 42U);
    if (strcmp(buf, "-5 4000000000 4000000000 ff    42"))
        return 2;
    sprintf(buf, "%020lld|%18lld|%#llx|%#llo|%llo", a, a, c, 8LL, 0LL);
    if (strcmp(buf, "-0000001234567890123|    -1234567890123|0x1122334455667788|"
                    "010|0"))
        return 3;
    sprintf(buf, "%lld", -9223372036854775807LL - 1);
    if (strcmp(buf, "-9223372036854775808"))
        return 4;
    sprintf(buf, "%s %lld %s", "x", 5LL, "y");
    return strcmp(buf, "x 5 y") != 0;
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

#line changes the logical source location observed by the standard built-ins.
try_ 1 << EOF
#line 70
int main(void) { return __LINE__ == 70; }
EOF

try_ 1 << EOF
#line 41 "generated-input.c"
int main(void) { return !strcmp(__FILE__, "generated-input.c"); }
EOF

#line operands are macro-expanded in an isolated directive token stream.
try_ 2 << EOF
#define LINE_VALUE 90
#define LINE_FILE "macro-generated.c"
#line LINE_VALUE LINE_FILE
int main(void) { return (__LINE__ == 90) + !strcmp(__FILE__, "macro-generated.c"); }
EOF

try_ 1 << EOF
#define ID(x) x
#line ID(120)
int main(void) { return __LINE__ == 120; }
EOF

try_compile_error << EOF
#line 0
int main(void) { return 0; }
EOF

# The line number is a decimal digit sequence: a leading zero is not octal, and
# a prefix, a suffix or a value past 2147483647 is rejected.
try_ 1 << EOF
#line 010
int main(void) { return __LINE__ == 10; }
EOF

try_compile_error << EOF
#line 0x10
int main(void) { return 0; }
EOF

try_compile_error << EOF
#line 10u
int main(void) { return 0; }
EOF

try_compile_error << EOF
#line 4294967297
int main(void) { return 0; }
EOF

# C99 fixes the spelling and extent of these translation-time string literals.
# Their actual value is supplied once at configuration time so all bootstrap
# stages use precisely the same expansion.
try_ 1 << EOF
int main(void)
{
    return sizeof(__DATE__) == 12 && sizeof(__TIME__) == 9;
}
EOF

# Reached through another macro's name, each still spells its own value.
try_ 1 << EOF
#define DATE_ALIAS __DATE__
#define TIME_ALIAS() __TIME__
int main(void)
{
    return sizeof(DATE_ALIAS) == 12 && sizeof(TIME_ALIAS()) == 9 &&
           !strcmp(DATE_ALIAS, __DATE__) && !strcmp(TIME_ALIAS(), __TIME__);
}
EOF

# Category: Function-like Macros
begin_category "Function-like Macros" "Testing function-like macros and variadic macros"

# An empty argument list is a valid invocation of a zero-parameter macro.
try_ 6 << EOF
#define SIX() 6
int main(void) { return SIX(); }
EOF

# An object-like replacement is expanded without consuming its following call.
try_ 9 << EOF
#define TARGET target
int target(void) { return 9; }
int main(void) { return TARGET(); }
EOF

# An argument is macro-replaced before it is substituted (C99 6.10.3.1), so a
# macro used in its own argument expands too, directly or through another macro,
# while the rescanned replacement still does not recurse.
try_output 0 "3 3 5 15 9 12 ((((1) + 1)) + 1)" << EOF
#define A(x) ((x) + 1)
#define B(y) A(y)
#define C(a, b) A(a) * A(b)
#define SELF(x) SELF
#define REC(x) REC(x)
#define STR(x) #x
#define XSTR(x) STR(x)
int SELF = 9;
int REC(int v) { return v * 2; }
int main()
{
    printf("%d %d %d %d %d %d %s\n", A(A(1)), B(A(1)), A(B(A(2))),
           C(A(1), C(1, A(0))), SELF(SELF(0)), REC(REC(3)), XSTR(A(A(1))));
    return 0;
}
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

# A '#' that a backslash-newline carries to column 1 is still inside the logical
# line before it, so it stringifies rather than opening a directive, while white
# space alone before a '#' leaves it a directive.
try_output 0 "[ab] 7" << 'EOF'
#define STR(x) \
#x
  \
#define SEVEN 7
	#ifndef SEVEN
	#error SEVEN
	#endif
int main()
{
    printf("[%s] %d\n", STR(ab), SEVEN);
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

# A call that stops after the named parameters binds __VA_ARGS__ to nothing,
# which substitutes, stringifies and pastes as the empty list.
try_ 15 << EOF
#define TAIL(a, ...) (a + 0 __VA_ARGS__)
#define SPELL(a, ...) #__VA_ARGS__
#define JOIN(a, ...) a ## __VA_ARGS__
int main()
{
    int v = 4;
    return TAIL(3) + TAIL(1, +2) + sizeof(SPELL(9)) + JOIN(v) + JOIN(v, );
}
EOF

# GNU comma elision: ", ## __VA_ARGS__" deletes the comma when the variadic
# argument is empty and keeps it, without pasting, when it is not.
try_ 12 << EOF
#define CALL(f, ...) f(0, ##__VA_ARGS__)
#define ALL(...) count(9, ##__VA_ARGS__)
int count(int first, ...) { return first; }
int sum(int a, ...) { int *p = &a; return a + p[1 * (__SIZEOF_POINTER__ / 4)]; }
int one(int a) { return a + 1; }
int main()
{
    return CALL(one) + CALL(sum, 2) + ALL() + ALL(1, 2) - 9;
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

# An operand whose address is taken can change between two identical operations
# without any instruction naming it: a store through the pointer, or by a callee
# handed the address, in the same function, a loop, or through a pointer kept in
# a record. The second operation must be computed again.
try_ 101 << EOF
int f(int a, int b)
{
    int x = a + b;
    int *p = &a;
    *p = 99;
    int y = a + b;
    return y;
}
int main(void)
{
    return f(1, 2);
}
EOF

try_ 34 << EOF
void inc(int *p)
{
    *p = *p + 1;
}
int f(int a, int b)
{
    int x = a + b;
    inc(&a);
    int y = a + b;
    return x * 10 + y;
}
int main(void)
{
    return f(1, 2);
}
EOF

try_ 56 << EOF
int f(int a, int b)
{
    int *p = &b;
    int x = a * b;
    *p = 5;
    int y = a * b;
    return x + y;
}
int main(void)
{
    return f(7, 3);
}
EOF

try_ 10 << EOF
int f(int a, int b)
{
    int x = a < b;
    int *p = &a;
    *p = 10;
    int y = a < b;
    return x * 10 + y;
}
int main(void)
{
    return f(1, 2);
}
EOF

try_ 84 << EOF
struct holder {
    int *q;
};
int main(void)
{
    int a = 3, b = 4;
    struct holder h;
    h.q = &a;
    int x = a + b;
    *h.q = 10;
    int y = a + b;
    return x * 10 + y;
}
EOF

try_ 3 << EOF
int main(void)
{
    int a = 3, b = 4, s = 0;
    int *p = &a;
    for (int k = 0; k < 3; k++) {
        int x = a + b;
        *p = *p + 1;
        int y = a + b;
        s += y - x;
    }
    return s;
}
EOF

# Reading the same element twice is a repeat only while nothing can have written
# memory in between: a store to that element, one through another pointer, a
# callee, a store on one path to the second read, or an assignment by name to
# the variable the pointer reaches. A compound assignment to the element must
# also keep the address its store goes to.
try_ 57 << EOF
int main(void)
{
    char buf[4];
    int i = 1;
    buf[1] = 5;
    int x = buf[i];
    buf[i] = 7;
    int y = buf[i];
    return x * 10 + y;
}
EOF

try_ 57 << EOF
int main(void)
{
    char buf[4];
    char *s = buf, *t = buf;
    int i = 1;
    s[1] = 5;
    int x = s[i];
    t[i] = 7;
    int y = s[i];
    return x * 10 + y;
}
EOF

try_ 19 << EOF
void put(char *s, int i)
{
    s[i] = 9;
}
int main(void)
{
    char buf[4];
    int i = 2;
    buf[2] = 1;
    int x = buf[i];
    put(buf, i);
    int y = buf[i];
    return x * 10 + y;
}
EOF

try_ 59 << EOF
int f(char *s, int i, int k)
{
    int x = s[i];
    if (k)
        s[i] = 9;
    int y = s[i];
    return x * 10 + y;
}
int main(void)
{
    char buf[4] = {0, 5, 0, 0};
    return f(buf, 1, 1);
}
EOF

try_ 57 << EOF
int main(void)
{
    char buf[4];
    char *s = buf;
    int i = 1;
    s[1] = 5;
    int x = s[i];
    s[i] += 2;
    return x * 10 + buf[1];
}
EOF

try_ 56 << EOF
int main(void)
{
    char buf[4];
    char *s = buf;
    int i = 1;
    s[1] = 5;
    int x = s[i];
    s[i]++;
    return x * 10 + buf[1];
}
EOF

try_ 15 << EOF
int f(int i)
{
    char c = 1;
    char *s = &c;
    int x = s[i];
    c = 5;
    int y = s[i];
    return x * 10 + y;
}
int main(void)
{
    return f(0);
}
EOF

try_ 15 << EOF
char g;
int f(int i)
{
    char *s = &g;
    g = 1;
    int x = s[i];
    g = 5;
    int y = s[i];
    return x * 10 + y;
}
int main(void)
{
    return f(0);
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
    # cases define "stdout" as descriptor 1 cast to FILE *, the standard output.
    try_output 0 "awritten = a" << EOF
#define stdout ((FILE *) 1)
int main()
{
	int c = fputc('a', stdout);
	printf("written = %c", c);
	return 0;
}
EOF

    try_output 1 "" << EOF
#define stdout ((FILE *) 1)
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

# The standard streams from lib/c.h, in both link modes. Under dynamic linking
# the host libc dereferences them, so a descriptor number would fault there.
try_output 0 "a1b" << EOF
int main()
{
    fputc('a', stdout);
    fprintf(stdout, "%d", 1);
    printf("b");
    fflush(stdout);
    fprintf(stderr, "");
    return fflush(stderr) != 0 || stdin == stdout || stdout == stderr;
}
EOF

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

# memcmp() orders bytes as unsigned char, so 0x80 sorts above 0x01.
try_ 0 << EOF
int main(void)
{
    char high[1] = {(char) 0x80}, low[1] = {1};
    return !(memcmp(high, low, 1) > 0 && memcmp(low, high, 1) < 0);
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

# memcpy() takes and returns void *, as memset() does, so a matching
# redeclaration and a void * function pointer both agree with the library.
try_ 0 << EOF
void *memcpy(void *dest, const void *src, int count);
struct pair {
    int a, b;
};
int main(void)
{
    struct pair x = {3, 4}, y;
    int src[2] = {5, 6}, dst[2];
    void *(*copy)(void *, const void *, int) = memcpy;
    if (memcpy(&y, &x, sizeof(x)) != &y || y.b != 4)
        return 1;
    if (copy(dst, src, sizeof(src)) != dst || dst[1] != 6)
        return 2;
    return 0;
}
EOF

# The address of a library function, which under dynamic linking is its PLT
# entry: equal wherever it is taken, and callable from a local, a file-scope
# initializer and a struct or array member. The global read after each call
# needs the global base intact once the library returns.
try_ 0 << EOF
int (*global_len)(const char *) = strlen;
struct ops {
    int (*len)(const char *);
    void *(*set)(void *, int, int);
};
struct ops global_ops = {strlen, memset};
int (*global_table[2])(const char *) = {strlen, &strlen};
int counter = 40;
int main(void)
{
    int (*local_len)(const char *) = &strlen;
    struct ops local_ops = {strlen, memset};
    int word = 0;

    if (!local_len || &memset != &memset || local_len != global_len)
        return 1;
    if (global_ops.len != global_table[1] || local_ops.set != global_ops.set)
        return 2;
    if (local_len("abc") + counter != 43 || global_len("ab") + counter != 42)
        return 3;
    if (global_table[0]("abcd") != 4 || local_ops.len("a") + counter != 41)
        return 4;
    global_ops.set(&word, 1, 1);
    if (word != 1 || counter != 40)
        return 5;
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

# C99 hexadecimal escapes consume every following hex digit, and in a narrow
# literal the value must fit an unsigned char. A narrow literal joined to a wide
# one is wide, so its escape may exceed a byte.
try_ 1 << EOF
int main() {
    char *s = "\\x0041";
    return s[0] == 'A' && s[1] == 0;
}
EOF
try_compile_error_message "Hexadecimal escape sequence out of range" << 'EOF'
int main(void) { char *s = "\x100"; return s[0]; }
EOF
try_compile_error_message "Hexadecimal escape sequence out of range" << 'EOF'
char joined[] = "a" "\x123" "b";
int main(void) { return joined[0]; }
EOF
try_ 0 << 'EOF'
int main(void) {
    wchar_t *joined = "\x100" L"b";
    wchar_t *wide = L"\x1234";
    return joined[0] != 0x100 || joined[1] != 'b' || wide[0] != 0x1234;
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

# C99 permits at most three octal digits in one escape; the trailing 2 is a
# second character in this implementation-defined packed multicharacter value.
try_ 1 << EOF
int main(void) { return '\1012' == 0x4132; }
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

# A joined literal may be longer than any single token.
try_ 2 << EOF
char joined_array[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc" "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee" "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
int main(void) {
    const char *joined = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb" "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc" "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee" "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
    return (sizeof(joined_array) == 601) + (strlen(joined) == 600);
}
EOF

# An escaped backslash does not escape the quote that follows it.
try_ 0 << 'EOF'
char trailing[] = "\\";
int main(void) {
    char *pair = "a\\\\" "b\\";
    const char *quote = "\\\"";
    return (sizeof trailing != 2 || trailing[0] != '\\') +
           (pair[1] != '\\' || pair[2] != '\\' || pair[3] != 'b' ||
            pair[4] != '\\' || pair[5] != 0) *
               2 +
           (quote[0] != '\\' || quote[1] != '"' || quote[2] != 0) * 4;
}
EOF

# Escapes end with their own literal: a digit that starts the next literal is a
# character of its own, not a further digit of a hexadecimal or short octal
# escape that ended the previous one.
try_ 0 << 'EOF'
char hex[] = "\x1" "2";
char oct[] = "\1" "2";
char oct2[] = "\12" "3";
char full[] = "\123" "4";
char escaped[] = "\\x1" "2";
char chain[] = "\x1" "2" "3";
int main(void) {
    wchar_t *wide = L"\x1" L"f";
    char *mixed = "\7" "7" "\x2" "A";
    return (sizeof hex != 3 || hex[0] != 1 || hex[1] != '2') +
           (sizeof oct != 3 || oct[0] != 1 || oct[1] != '2') * 2 +
           (sizeof oct2 != 3 || oct2[0] != 10 || oct2[1] != '3') * 4 +
           (sizeof full != 3 || full[0] != 0123 || full[1] != '4') * 8 +
           (sizeof escaped != 5 || escaped[1] != 'x' || escaped[3] != '2') *
               16 +
           (sizeof chain != 4 || chain[0] != 1 || chain[2] != '3') * 32 +
           (wide[0] != 1 || wide[1] != 'f' || wide[2] != 0) * 64 +
           (mixed[0] != 7 || mixed[1] != '7' || mixed[2] != 2 ||
            mixed[3] != 'A') *
               128;
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

# Union extent is padded to the strictest member alignment, even when its
# largest member is an odd-sized character array.
try_ 8 << EOF
typedef union {
    char bytes[5];
    int value;
} padded_union_t;
int main(void) { return sizeof(padded_union_t); }
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

# One declaration may list several prototypes, mixed with object declarators, at
# file scope and in a block extern declaration. A definition stands alone.
try_ 0 << EOF
int f(void), g(int);
int h(int), value = 4, *ptr, k(void);
struct S { int a; };
struct S make(int), other;
union U { int a; } umake(void), uobj;
enum E { E1 = 1 } emake(void), eobj = E1;
static int sf(int), sg(void);
int f(void) { return 1; }
int g(int x) { return x + 1; }
int h(int x) { return x * 2; }
int k(void) { return value; }
struct S make(int v) { struct S s; s.a = v; return s; }
union U umake(void) { union U u; u.a = 6; return u; }
enum E emake(void) { return E1; }
static int sf(int x) { return x - 1; }
static int sg(void) { return 3; }
int main(void)
{
    extern int y, late(void), z;
    struct S m = make(5);
    union U u = umake();
    ptr = &value;
    other.a = 2;
    return f() + g(2) + h(3) + k() + *ptr + m.a + other.a + sf(1) + sg() +
           u.a + emake() + eobj + late() + y + z - 46;
}
int y = 2, z = 3;
int late(void) { return 5; }
EOF
try_compile_error_message "function definition must be the only declarator" << EOF
int value, defined(void) { return 0; }
int main(void) { return 0; }
EOF
try_compile_error_message "function definition must be the only declarator" << EOF
int declared(void), defined(void) { return 0; }
int main(void) { return 0; }
EOF
try_compile_error << EOF
int clash(void), clash;
int main(void) { return 0; }
EOF

# A block-scope function declaration has external linkage without extern (C99
# 6.2.2p5), so plain prototypes may appear alone, several per declaration, mixed
# with objects, and with record, pointer or typedef return types. The name stays
# visible only in its block and hides an outer object there.
try_ 41 << EOF
struct S { int a; };
typedef int T;
typedef char *str_t;
int outer(void) { int twice(int); return twice(3); }
int main(void)
{
    int f = 1;
    struct S make(int);
    char *text(void);
    T add(int), value = 3;
    str_t name(void);
    unsigned count(void), *slot(void);
    int x = 2, *ptr(void), y = 4;
    struct S v = {1}, other(void);
    {
        int f(void);
        if (f() != 7)
            return 1;
    }
    struct S m = make(5);
    struct S o = other();
    char *t = text();
    unsigned *k = slot();
    str_t n = name();
    int *p = ptr();
    return m.a + o.a + *t + add(1) + value + count() + *k + n[0] + x + *p + y +
           v.a + f + outer();
}
int f(void) { return 7; }
int twice(int v) { return v * 2; }
struct S make(int v) { struct S s; s.a = v; return s; }
struct S other(void) { struct S s; s.a = 2; return s; }
char *text(void) { return "\001"; }
T add(int v) { return v; }
str_t name(void) { return "\002"; }
unsigned count(void) { return 2; }
unsigned *slot(void) { static unsigned k = 7; return &k; }
int *ptr(void) { static int k = 4; return &k; }
EOF
try_ 9 << EOF
int main(void) { int f(void), g = f(); return g; }
int f(void) { return 9; }
EOF
try_compile_error << EOF
int main(void) { { int hidden(void); } return hidden(); }
int hidden(void) { return 0; }
EOF
try_compile_error << EOF
int main(void) { int f(void); return f(); }
char f(void) { return 1; }
EOF
try_compile_error_message "invalid storage class for block function declaration" << EOF
int main(void) { static int f(void); return 0; }
EOF
try_compile_error_message "invalid storage class for block function declaration" << EOF
int main(void) { register int x, f(void); return 0; }
EOF

# C99 admits a function definition only at file scope. A nested one is rejected
# however its declaration is spelled, rather than lowered inside its caller.
try_compile_error_message "function definition is not allowed at block scope" << EOF
int main(void) { extern int f(void) { return 1; } return f(); }
EOF
try_compile_error_message "function definition is not allowed at block scope" << EOF
int main(void) { int f(void) { return 1; } return f(); }
EOF
try_compile_error_message "function definition is not allowed at block scope" << EOF
struct S { int a; };
int main(void) { int x; struct S make(void) { struct S s; return s; } return x; }
EOF
try_compile_error_message "function definition is not allowed at block scope" << EOF
int main(void) { typedef int F(void); F f { return 1; } return 0; }
EOF

# C99 functions may return object or void types, but never an array or another
# function. Keep declaration-only forms covered because no function body is
# needed for the constraint to apply.
try_compile_error << EOF
int returns_array(void)[2];
EOF
try_compile_error << EOF
int returns_function(void)(void);
EOF
try_compile_error << EOF
typedef int array_t[2];
array_t typedef_returns_array(void);
EOF
try_compile_error << EOF
typedef int function_t(void);
function_t typedef_returns_function(void);
EOF
try_compile_flag --std=c99 << EOF
typedef int row_t[2];
typedef row_t *row_ptr_t;
row_ptr_t returns_row(void);
EOF
try_ 7 << EOF
int callback(void) { return 7; }
int main(void) { return (*callback)(); }
EOF
try_ 3 << EOF
int callback(void) { return 7; }
_Bool returns_callback(void) { return callback; }
int main(void) {
    _Bool initialized = callback;
    _Bool assigned = 0;
    assigned = callback;
    return initialized + assigned + returns_callback();
}
EOF
try_ 7 << EOF
struct pair { int left; int right; };
struct pair make_pair(int left, int right) {
    struct pair value = {left, right};
    return value;
}
int main(void) {
    struct pair value = make_pair(3, 4);
    return value.left + value.right;
}
EOF
try_ 0 << EOF
struct pair { int left; int right; };
void use(void) { struct pair (*callback)(void); }
int main(void) { use(); return 0; }
EOF
try_ 12 << EOF
struct triple { int first; int second; int third; };
struct triple make_triple(int first, int second, int third) {
    struct triple value = {first, second, third};
    return value;
}
int main(void) {
    struct triple value = make_triple(3, 4, 5);
    return value.first + value.second + value.third;
}
EOF
try_ 44 << EOF
struct pair { int left; int right; };
struct pair make_pair(int left, int right) {
    struct pair value = {left, right};
    return value;
}
int sum_pair(struct pair value) { return value.left + value.right; }
int main(void) {
    struct pair (*callback)(int, int) = make_pair;
    struct pair (*assigned)(int, int);
    struct pair indirect = callback(7, 8);
    assigned = make_pair;
    struct pair assigned_result = assigned(2, 3);
    struct pair parenthesized = (make_pair)(9, 10);
    struct pair dereferenced = (*callback)(1, 2);
    make_pair(0, 0);
    return sum_pair(make_pair(12, 13)) + indirect.left +
           parenthesized.left + dereferenced.left + assigned_result.left;
}
EOF
try_ 40 << EOF
union number { int value; char bytes[4]; };
union number make_number(int value) {
    union number result;
    result.value = value;
    return result;
}
int take_number(union number value) { return value.value; }
int main(void) {
    union number (*callback)(int) = make_number;
    union number indirect = callback(17);
    return indirect.value + take_number(make_number(23));
}
EOF

# Record-returning calls also go through file-scope pointers, record members,
# array elements and parameters. Every function whose address is taken must be
# defined, so each such pointer names a target using the internal return
# convention. A pointer object is pointer-sized whatever it returns: `tag_fn`
# once took one byte's slot and overlapped the array after it.
try_ 0 << 'EOF'
struct triple { int a; int b; int c; };
struct tag { char x; };
struct triple make(void) { struct triple s = {1, 2, 3}; return s; }
struct triple scaled(int k);
struct tag make_tag(void) { struct tag t = {'q'}; return t; }
unsigned char small(void) { return 200; }
struct triple (*g_make)(void) = make;
struct triple (*g_scaled)(int);
struct tag (*tag_fn)(void) = make_tag;
struct triple (*g_table[2])(int) = {scaled, scaled};
unsigned char (*small_fn)(void) = small;
int after[2] = {11, 22};
struct holder {
    int id;
    struct triple (*fn)(int);
    struct tag (*tags[2])(void);
};
struct holder g_holder = {9, scaled, {make_tag, make_tag}};
static struct triple (*s_make)(void) = make;
struct triple apply(struct triple (*f)(int), int k) { return f(k); }
int block_static(void)
{
    static struct triple (*fn)(int) = scaled;
    static struct tag (*tags[2])(void) = {make_tag, make_tag};
    struct triple (*local)(int) = g_scaled;
    return fn(2).c != 6 || tags[1]().x != 'q' || local(1).b != 2;
}
int main(void)
{
    struct holder *h = &g_holder;
    struct triple v = g_make();
    g_scaled = scaled;
    struct triple w = g_scaled(4);
    struct tag t = tag_fn();
    if (v.a != 1 || v.b != 2 || v.c != 3 || w.a != 4 || w.c != 12)
        return 1;
    if (t.x != 'q' || after[0] != 11 || after[1] != 22 || small_fn() != 200)
        return 2;
    if (g_holder.fn(5).c != 15 || h->fn(7).b != 14 || h->tags[1]().x != 'q')
        return 3;
    if (g_table[1](6).b != 12 || s_make().c != 3 || (*g_make)().a != 1)
        return 4;
    if (apply(scaled, 3).c != 9 || apply(g_table[0], 2).a != 2)
        return 5;
    h->tags[0] = make_tag;
    g_table[0] = g_scaled;
    if (g_table[0](8).a != 8 || h->tags[0]().x != 'q')
        return 6;
    return block_static() ? 7 : 0;
}
struct triple scaled(int k)
{
    struct triple s = {k, k * 2, k * 3};
    return s;
}
EOF
try_compile_error << 'EOF'
struct pair { int value; };
extern struct pair foreign_pair(void);
struct pair (*callback)(void) = foreign_pair;
int main(void) { return callback().value; }
EOF

# A record returned by value is an rvalue whose members can still be selected
# (C99 6.5.2.3), whether the record is small or large.
try_ 0 << EOF
struct inner { char tag; int b; };
struct small { int a; int c; };
struct point { int x, y; };
struct node { int value; struct node *next; };
struct big {
    int a;
    struct inner inner;
    int arr[3];
    int grid[2][3];
    struct point pts[2];
    unsigned flags : 3;
    unsigned more : 5;
    struct node *next;
    long long wide;
    char name[8];
};
typedef union { int value; char bytes[4]; } number_t;
static struct node tail = { 42, 0 };
struct small make_small(int x) { struct small s = { x, x * 2 }; return s; }
number_t make_number(int value) { number_t n; n.value = value; return n; }
struct big make_big(int x) {
    struct big b = { 0 };
    b.a = x;
    b.inner.tag = 'q';
    b.inner.b = x + 1;
    b.arr[0] = 1; b.arr[1] = x * 3; b.arr[2] = 5;
    b.grid[1][0] = 10; b.grid[1][2] = 12;
    b.pts[1].x = 7; b.pts[1].y = 8;
    b.flags = 5; b.more = 17;
    b.next = &tail;
    b.wide = 0x100000003LL;
    b.name[0] = 'h'; b.name[1] = 'i';
    return b;
}
int sum(int a, int b) { return a + b; }
int main(void) {
    int x;
    struct big (*maker)(int) = make_big;
    x = make_small(4).c;
    struct inner in = make_big(9).inner;
    struct point pt = make_big(0).pts[1];
    return x != 8 || make_big(3).a != 3 || make_big(4).inner.b != 5 ||
           make_big(4).inner.tag != 'q' ||
           make_big(2).arr[1] * 10 + make_small(1).a != 61 ||
           make_big(1).grid[1][2] != 12 || *(make_big(1).grid[1] + 2) != 12 ||
           in.b != 10 || in.tag != 'q' || pt.x != 7 || pt.y != 8 ||
           make_big(0).pts[1].y != 8 || make_big(0).flags != 5 ||
           make_big(0).more != 17 || make_big(0).next->value != 42 ||
           make_big(0).wide != 0x100000003LL ||
           sum(make_small(2).a, make_big(5).arr[1]) != 17 ||
           maker(6).arr[1] != 18 || (*maker)(6).inner.b != 7 ||
           -make_small(5).a != -5 || !make_small(0).a != 1 ||
           make_big(0).name[1] != 'i' || *(make_big(0).arr + 2) != 5 ||
           (make_small(3).a ? make_big(2).inner.b : 0) != 3 ||
           (make_number(0x01020304).value & 0xff) != 4 ||
           sizeof(make_big(0).arr) != 3 * sizeof(int) ||
           sizeof make_big(0).grid[1] != 3 * sizeof(int) ||
           sizeof make_small(0).a != sizeof(int);
}
EOF
try_compile_error_message "member of a function call result is not assignable" << EOF
struct pair { int left; int right; };
struct pair make_pair(void) { struct pair value = {1, 2}; return value; }
int main(void) { make_pair().left = 3; return 0; }
EOF

# A member of what a call returns is an lvalue: it may be assigned, compounded
# and updated, whether it is reached through the returned pointer, through a
# pointer member of that record, or through a subscript of the result.
try_ 0 << 'EOF'
struct pair { int x, y; };
struct pair pairs[2] = {{1, 2}, {3, 4}};
struct pair *first(void) { return pairs; }
struct node;
typedef struct node *link;
struct node { int v; link next; };
struct node tail = {2, 0};
struct node head = {1, &tail};
struct node *list(void) { return &head; }
typedef struct pair duo[2];
duo *duos(void) { return &pairs; }
struct holder { int slots[3]; };
struct holder holder = {{1, 2, 3}};
struct holder *held(void) { return &holder; }
struct pair *(*callback)(void) = first;
int main(void)
{
    int value;

    first()->y = 8;
    first()->y += 1;
    (*first()).x = 5;
    if (pairs[0].y != 9 || pairs[0].x != 5)
        return 1;
    list()->next->v = 1;
    list()->next->v += 4;
    if (tail.v != 5)
        return 2;
    duos()[0][1].x = 7;
    held()->slots[1] = 9;
    callback()->x = 6;
    if (pairs[1].x != 7 || holder.slots[1] != 9 || pairs[0].x != 6)
        return 3;
    value = (first()->y = 12);
    first()->y++;
    ++first()->y;
    if (value != 12 || pairs[0].y != 14)
        return 4;
    return 0;
}
EOF
try_compile_error_message "Assignment requires a modifiable lvalue" << 'EOF'
struct holder { int slots[3]; };
struct holder *held(void);
int main(void) { held()->slots = 0; return 0; }
EOF
try_compile_error_message "assignment of read-only location" << 'EOF'
struct pair { const int x; int y; };
struct pair *first(void);
int main(void) { first()->x = 1; return 0; }
EOF

# A function-pointer member of a call result, or of a record reached through a
# dereference, is called like any other function pointer.
try_ 17 << EOF
struct ops { int (*apply)(int); int bias; };
int twice(int value) { return value * 2; }
struct ops make_ops(void) {
    struct ops ops = {0, 1};
    ops.apply = twice;
    return ops;
}
int main(void) {
    struct ops ops = make_ops();
    struct ops *q = &ops;
    return make_ops().apply(3) + (*q).apply(4) + (make_ops()).apply(1) +
           make_ops().bias;
}
EOF

# A pointer member whose star is hidden in a typedef is followed by -> after a
# call result or a dereferenced value too, and keeps the pointee's qualifier.
try_ 0 << 'EOF'
struct node;
typedef struct node *link;
struct node { int v; link next; };
typedef struct { char c; short s; } pair;
typedef pair *pair_ptr;
struct holder { char tag; pair_ptr p; const pair_ptr q; pair_ptr ps[2]; };
struct node n3 = {3, 0};
struct node n2 = {2, &n3};
struct node n1 = {1, &n2};
pair p0 = {10, 20};
pair p1 = {30, 40};
struct holder h = {'x', &p0, &p1, {&p0, &p1}};
struct node *head(void) { return &n1; }
struct holder *holder(void) { return &h; }
struct holder holder_value(void) { return h; }
int main(void)
{
    struct node *p = &n1;
    if (head()->next->v != 2 || head()->next->next->v != 3 || p->next->next->v != 3)
        return 1;
    if ((*head()->next).v != 2 || (*p).next->v != 2 || head()->next->next->next)
        return 2;
    if (holder()->p->s != 20 || holder_value().q->c != 30 || holder()->ps[1]->s != 40)
        return 3;
    ++head()->next->v;
    holder()->q->s++;
    if (n2.v != 3 || p1.s != 41 || sizeof(head()->next->v) != sizeof(int))
        return 4;
    return 0;
}
EOF
try_compile_error_message "assignment of read-only location" << 'EOF'
struct node { int v; };
typedef const struct node *const_link;
struct holder { const_link p; };
struct holder h;
struct holder *holder(void) { return &h; }
int main(void) { ++holder()->p->v; return 0; }
EOF
try_compile_error_message "Invalid record member access" << 'EOF'
struct node { int v; };
typedef struct node *link;
struct holder { link *pp; };
struct holder h;
struct holder *holder(void) { return &h; }
int main(void) { return holder()->pp->v; }
EOF
try_compile_error_message "Unknown struct or union member" << EOF
struct pair { int left; int right; };
struct pair make_pair(void) { struct pair value = {1, 2}; return value; }
int main(void) { return make_pair().middle; }
EOF
try_compile_error << EOF
struct pair { int value; };
extern struct pair foreign_pair(void);
int main(void) { foreign_pair(); return 0; }
EOF
try_compile_error << EOF
struct pair { int value; };
extern struct pair foreign_pair(void);
int main(void) {
    struct pair (*callback)(void) = foreign_pair;
    callback();
    return 0;
}
EOF
try_compile_error << EOF
struct pair { int value; };
struct pair local_pair(void) { struct pair value = {1}; return value; }
extern struct pair foreign_pair(void);
int main(int choose_foreign) {
    struct pair (*callback)(void);
    if (choose_foreign) callback = foreign_pair;
    else callback = local_pair;
    callback();
    return 0;
}
EOF
try_ 9 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) { return rows()[0][0] + rows()[0][1]; }
EOF
try_ 6 << EOF
typedef int grid[2][3];
grid *grids(void) {
    static grid value = {{1, 2, 3}, {4, 5, 6}};
    return &value;
}
int main(void) { return grids()[0][1][2]; }
EOF
try_ 24 << EOF
typedef int hyper_t[2][2][3][2];
hyper_t *hypers(void) {
    static hyper_t value = {0};
    value[1][1][2][1] = 24;
    return &value;
}
int main(void) { return hypers()[0][1][1][2][1]; }
EOF
try_ 9 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) { rows()[0][1] = 9; return rows()[0][1]; }
EOF
try_ 4 << EOF
typedef int *row[2];
static int second = 4;
row *pointers(void) {
    static int first = 3;
    static row value = {&first, &second};
    return &value;
}
int main(void) { return pointers()[0][1] == &second ? 4 : 0; }
EOF

# A call returning a pointer to a row of pointers keeps the element depth where
# the typedef spells it. Dereferencing the result yields that row, whose
# elements are pointers to read, not rows to select again.
try_ 0 << 'EOF'
typedef int *pointer_row[2];
static int first = 3, second = 4;
pointer_row values = {&first, &second};
pointer_row *rows(void) { return &values; }
int main(void)
{
    int **loaded = *rows();
    if (*(*rows())[1] != 4 || (*rows())[0] != &first)
        return 1;
    if (*loaded[1] != 4 || sizeof((*rows())[1]) != sizeof(int *))
        return 2;
    if (sizeof(*rows()) != 2 * sizeof(int *) || *rows()[0][1] != 4)
        return 3;
    (*rows())[1] = &first;
    if (*values[1] != 3)
        return 4;
    return 0;
}
EOF
try_compile_error_message "Expected a global object or function after '&'" << EOF
typedef int *row[2];
row *invalid_pointers(void) {
    int first = 3;
    static int second = 4;
    static row value = {&first, &second};
    return &value;
}
int main(void) { return 0; }
EOF

# A static array named in an aggregate initializer decays to an address constant
# (C99 6.6p7), optionally offset, in array and record initializers at file scope
# and in block-scope statics. An automatic array is not a constant.
try_ 0 << 'EOF'
static int g[2] = {5, 6};
int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
struct refs { int *p; int *q; char *n; };
struct refs g_s = {g, g + 1, "gs"};
int *g_a[] = {g + 1, m[1]};
int main(void)
{
    static int ls[3] = {7, 8, 9};
    static int *a[] = {g, g + 1, ls, ls + 2};
    static int *rows[] = {m[0], m[1] + 1};
    static char *names[] = {"a", "b" "c"};
    static struct refs s = {g + 1, ls, "nm"};
    static struct refs sa[2] = {{g, 0, "x"}, {ls + 1, g, "y"}};
    struct refs ds = {g, ls + 1, "z"};
    if (sizeof a != 4 * sizeof(int *) || sizeof names != 2 * sizeof(char *))
        return 1;
    if (*a[0] != 5 || *a[1] != 6 || *a[2] != 7 || *a[3] != 9)
        return 2;
    if (rows[0][2] != 3 || *rows[1] != 5 || names[1][1] != 'c')
        return 3;
    if (*s.p != 6 || *s.q != 7 || s.n[1] != 'm')
        return 4;
    if (*sa[1].p != 8 || sa[1].q != g || sa[0].n[0] != 'x' || *ds.q != 8)
        return 5;
    if (g_s.p != g || *g_s.q != 6 || g_s.n[1] != 's' || *g_a[0] != 6 ||
        g_a[1][2] != 6)
        return 6;
    return 0;
}
EOF
try_compile_error << 'EOF'
int main(void)
{
    int local[2];
    static int *slots[] = {local};
    return 0;
}
EOF
try_compile_error << 'EOF'
int main(void)
{
    int local[2];
    static struct { int *p; } s = {local};
    return 0;
}
EOF

# Subscripts and offsets of an aggregate address constant resolve in the
# declaration's scope too, so a block-scope enumerator is a valid constant.
try_ 18 << EOF
int sum(void) {
    enum { K = 1 };
    static int values[3] = {5, 6, 7};
    static int *slots[2] = {&values[K], &values[0] + K};
    static int *single = &values[K];
    return *slots[0] + *slots[1] + *single;
}
int main(void) { return sum(); }
EOF
try_ 15 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) {
    int old;
    rows()[0][1] += 2;
    old = rows()[0][1]++;
    return old + rows()[0][1];
}
EOF
try_ 6 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) { return ++rows()[0][1]; }
EOF
try_compile_error << EOF
typedef const int row[2];
row *rows(void);
int main(void) { rows()[0][1] = 1; return 0; }
EOF
try_compile_error << EOF
typedef int row[2];
const row *rows(void);
int main(void) { rows()[0][1] = 1; return 0; }
EOF
try_ 6 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) { return ++(rows())[0][1]; }
EOF
try_ 6 << EOF
typedef int row[2];
row *rows(void) { static row value = {4, 5}; return &value; }
int main(void) { row *(*callback)(void) = rows; return ++callback()[0][1]; }
EOF
try_ 6 << EOF
typedef int (*row[2])(int);
int increment(int value) { return value + 1; }
int add_two(int value) { return value + 2; }
static int (*callback_storage[2])(int) = {increment, add_two};
row *callbacks(void) {
    return &callback_storage;
}
int main(void) { return callbacks()[0][1](4); }
EOF

# Fewer subscripts on a call returning a pointer to an array leave an array, not
# one scalar: it keeps its remaining bounds for sizeof, for its own subscripts
# and for pointer arithmetic, and so does a dereferenced call result.
try_ 0 << 'EOF'
typedef int grid[2][3];
typedef int cube[2][2][3];
typedef int *pointer_row[2];
grid *grids(void)
{
    static grid value = {{1, 2, 3}, {4, 5, 6}};
    return &value;
}
cube *cubes(void)
{
    static cube value;
    value[1][1][2] = 9;
    value[1][0][1] = 7;
    return &value;
}
static int first = 3, second = 4;
pointer_row *pointers(void)
{
    static pointer_row value = {&first, &second};
    return &value;
}
struct slots { int *a[2]; } holder;
struct slots *slots(void) { return &holder; }
int main(void)
{
    int *row = grids()[0][1];
    int (*rows)[3] = grids()[0];
    int (*plane)[3] = cubes()[0][1];
    int **pp = pointers()[0];
    if (row[2] != 6 || rows[1][0] != 4 || plane[1][2] != 9)
        return 1;
    if (*grids()[0][1] != 4 || (*grids()[0])[2] != 3 || *(grids()[0] + 1)[0] != 4)
        return 2;
    if (grids()[0] + 1 - grids()[0] != 1 || *pp[1] != 4 || cubes()[0][1][0][1] != 7)
        return 3;
    if (sizeof(grids()[0]) != 6 * sizeof(int) ||
        sizeof(grids()[0][1]) != 3 * sizeof(int) ||
        sizeof(grids()[0][1][2]) != sizeof(int) || sizeof(*grids()[0][1]) != sizeof(int))
        return 4;
    if (sizeof((*grids())[1]) != 3 * sizeof(int) ||
        sizeof((*grids())[1][2]) != sizeof(int) || (*grids())[1][2] != 6)
        return 5;
    if (sizeof(cubes()[0][1]) != 6 * sizeof(int) ||
        sizeof(pointers()[0]) != 2 * sizeof(int *) ||
        sizeof(slots()->a) != 2 * sizeof(int *))
        return 6;
    return 0;
}
EOF

# A record element of a call result's array is copied as a record, never loaded
# or stored as one scalar word, whether it is read, passed or assigned.
try_ 0 << 'EOF'
struct triple { char c; int v; short s; };
typedef struct triple trio[2];
struct triple store[2] = {{1, 2, 3}, {4, 5, 6}};
trio *trios(void) { return &store; }
int sum(struct triple t) { return t.c * 100 + t.v * 10 + t.s; }
int main(void)
{
    struct triple copy = trios()[0][1];
    struct triple next = {7, 8, 9};
    if (copy.c != 4 || copy.v != 5 || copy.s != 6)
        return 1;
    if (sum(trios()[0][0]) != 123 || trios()[0][1].s != 6)
        return 2;
    copy = trios()[0][0];
    if (copy.c != 1 || copy.s != 3)
        return 3;
    trios()[0][1] = next;
    if (store[1].c != 7 || store[1].v != 8 || store[1].s != 9)
        return 4;
    if (sizeof(trios()[0][1]) != sizeof(struct triple))
        return 5;
    return 0;
}
EOF
try_compile_error << 'EOF'
struct pair { int x, y; };
typedef struct pair duo[2];
duo *duos(void);
int main(void) { duos()[0][1]++; return 0; }
EOF
try_compile_error << 'EOF'
struct pair { int x, y; };
typedef struct pair duo[2];
duo *duos(void);
int main(void) { duos()[0][1] = 1; return 0; }
EOF

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

# Function-pointer parameter declarations carry a nested function type, not
# merely pointer-sized storage. Redeclarations must reject every incompatible
# part of that nested type.
try_compile_error << EOF
int callback(int (*)(int));
int callback(int (*)(long));
EOF
try_compile_error << EOF
int callback(int (*)(int));
int callback(int (*)(int, int));
EOF
try_compile_error << EOF
int callback(int (*)(int));
int callback(long (*)(int));
EOF
try_compile_error << EOF
int callback(int (*)(int, ...));
int callback(int (*)(int));
EOF
try_compile_error << EOF
int callback(int (*)(int));
int callback(int *);
EOF
try_compile_error << EOF
int nested(int (*)(int (*)(int)));
int nested(int (*)(int (*)(long)));
EOF

# Parameter names and top-level qualifiers on the callback pointer do not change
# a C99 function type.
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int invoke(int (*)(int));
int invoke(int (* const callback)(int)) { return callback(4) - 5; }
int main(void) { int (*callback)(int) = plus1; return invoke(callback); }
EOF
try_ 0 << EOF
int plus1(int value) { return value + 1; }
int legacy(int (*)());
int legacy(int (*callback)(int)) { return callback(3) - 4; }
int main(void) { int (*callback)(int) = plus1; return legacy(callback); }
EOF

begin_category "Bit-fields"

# Prefix ++ and -- update a bit-field inside its allocation unit and yield the
# stored value; postfix -- parses. A _Bool bit-field keeps only 0 or 1.
try_output 0 "7 1 15 7 1 15 1 7 1 15 1 1 0 1 7 1 15 | 2 7 2 15 3 7 3 15 7 1 15" << EOF
struct bits {
    unsigned int low : 3;
    _Bool flag : 1;
    unsigned int high : 4;
};
struct counters {
    unsigned int low : 3;
    unsigned int mid : 2;
    unsigned int high : 4;
};
int main(void)
{
    struct bits b;
    struct counters c;
    int v;
    b.low = 7;
    b.high = 15;
    b.flag = 1;
    b.flag++;
    printf("%d %d %d ", b.low, b.flag, b.high);
    b.flag = 0;
    b.flag--;
    printf("%d %d %d ", b.low, b.flag, b.high);
    b.flag = 1;
    v = ++b.flag;
    printf("%d %d %d %d ", v, b.low, b.flag, b.high);
    b.flag = 0;
    v = --b.flag;
    printf("%d %d ", v, b.flag);
    b.flag = 0;
    v = b.flag--;
    printf("%d %d ", v, b.flag);
    b.flag = 0;
    b.flag += 2;
    printf("%d %d %d | ", b.low, b.flag, b.high);
    c.low = 7;
    c.high = 15;
    c.mid = 1;
    v = ++c.mid;
    printf("%d %d %d %d ", v, c.low, c.mid, c.high);
    c.mid = 0;
    v = --c.mid;
    printf("%d %d %d %d ", v, c.low, c.mid, c.high);
    c.mid = 2;
    c.mid--;
    printf("%d %d %d", c.low, c.mid, c.high);
    return 0;
}
EOF
try_ 0 << EOF
typedef _Bool bool_alias;
struct flags { bool_alias value : 1; };
int main(void) {
    struct flags value = {2};
    value.value = 3;
    return value.value != 1 || sizeof(struct flags) != 1;
}
EOF
try_ 0 << EOF
typedef _Bool bool_alias;
struct flags { bool_alias value : 1; };
static struct flags value = {2};
int main(void) { return value.value != 1; }
EOF
try_compile_error << EOF
typedef _Bool bool_alias;
int main(void) { bool_alias values[] = "x"; return 0; }
EOF
try_ 37 << EOF
struct flags {
    unsigned int low : 3;
    unsigned int high : 5;
    int signed_value : 4;
    unsigned int : 0;
    unsigned int tail : 1;
};
int main(void) {
    struct flags value = {0};
    value.low = 7;
    value.high = 31;
    value.signed_value = -3;
    value.tail = 1;
    return value.low + value.high + value.signed_value + value.tail +
           (sizeof(struct flags) == 8 ? 1 : 0);
}
EOF
try_ 7 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
int main(void) {
    struct flags value = {0};
    value.left = 2;
    value.right = 4;
    value.left += 1;
    return value.left + value.right;
}
EOF
try_ 6 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
int main(void) {
    struct flags value = {2, 4};
    return value.left + value.right;
}
EOF
try_ 117 << EOF
struct flags {
    unsigned int low : 3;
    unsigned int : 2;
    unsigned int high : 3;
};
static struct flags file_flags = {5, 6};
int main(void) {
    struct flags local = {1, 7};
    return file_flags.low + file_flags.high * 8 +
           local.low * 64 + local.high * 512;
}
EOF
try_ 7 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
int main(void) {
    struct flags value = {2, 4};
    value.left++;
    return value.left + value.right;
}
EOF
try_ 6 << EOF
struct flags { unsigned int value : 3; };
int main(void) {
    struct flags flags = {2};
    return (flags.value += 9) + flags.value;
}
EOF
try_ 10 << EOF
struct flags { unsigned int value : 3; unsigned int adjacent : 3; };
int main(void) {
    struct flags flags = {2, 4};
    struct flags *pointer = &flags;
    return (pointer->value += 9) + pointer->value + pointer->adjacent;
}
EOF
try_ 9 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
int main(void) {
    struct flags value = {2, 4};
    int old = value.left++;
    return old + value.left + value.right;
}
EOF
try_ 90 << EOF
struct flags { int signed_value : 4; unsigned int adjacent : 4; };
int main(void) {
    struct flags value = {-3, 5};
    struct flags *pointer = &value;
    int old = pointer->signed_value++;
    pointer->adjacent += 5;
    return (old + 4) * 64 + (pointer->signed_value + 4) * 8 +
           pointer->adjacent;
}
EOF
try_ 0 << EOF
struct flags { int signed_value : 3; unsigned int adjacent : 3; };
int main(void) {
    struct flags value = {0};
    struct flags *pointer = &value;
    return (pointer->signed_value = 7) + pointer->signed_value +
           (pointer->adjacent = 9) + pointer->adjacent;
}
EOF
try_ 2 << EOF
struct flags { unsigned int value : 3; };
int main(void) {
    struct flags flags = {1};
    return (flags.value - 8 < 0) + ((~flags.value) < 0);
}
EOF
try_ 4 << EOF
struct flags { unsigned int value : 3; };
int main(void) { struct flags flags = {1}; return sizeof(+flags.value); }
EOF
try_ 5 << EOF
union flags { unsigned int value : 3; unsigned int raw; };
int main(void) { union flags value = {0}; value.value = 5; return value.value; }
EOF
try_ 18 << EOF
union flags {
    unsigned int : 2;
    unsigned int value : 3;
    unsigned int raw;
};
static union flags first = {5}, second = {6};
int main(void) {
    union flags local = {7};
    return first.value + second.value + local.value;
}
EOF
try_ 7 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
int main(void) {
    struct flags values[2] = {{1, 2}, {3, 4}};
    return values[1].left + values[1].right;
}
EOF
try_ 3 << EOF
struct flags { _Bool left : 1; _Bool right : 1; _Bool third : 1; };
int main(void) {
    struct flags value = {0};
    value.left = 9;
    value.right = 1;
    return value.left + value.right + (sizeof(struct flags) == 1 ? 1 : 0);
}
EOF
try_ 3 << EOF
struct flags { _Bool file : 1; _Bool local : 1; };
static struct flags file_flags = {2, 0};
int main(void) {
    struct flags local = {0};
    local.local = 2;
    return file_flags.file + file_flags.local + local.file + local.local +
           (sizeof(struct flags) == 1 ? 1 : 0);
}
EOF
try_ 6 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
static struct flags value = {2, 4};
int main(void) { return value.left + value.right; }
EOF
try_ 5 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
static struct flags value = {.left = 7, .right = 3, .left = 2};
int main(void) { return value.left + value.right; }
EOF
try_ 2 << EOF
struct flags { int signed_value : 4; unsigned int narrow : 3; };
static struct flags value = {-3, 9};
int main(void) { return value.signed_value + value.narrow + 4; }
EOF
try_ 7 << EOF
struct flags { unsigned int left : 3; unsigned int right : 3; };
static struct flags values[2] = {{1, 2}, {.left = 3, .right = 4}};
int main(void) { return values[1].left + values[1].right; }
EOF
try_ 72 << EOF
struct message { char *text; unsigned int code : 4; };
static struct message value = {"hello", 7};
int main(void) { return value.text[0] + value.code - 39; }
EOF
try_ 42 << EOF
int increment(int value) { return value + 1; }
struct callback { int (*call)(int); };
static struct callback value = {increment};
int main(void) { return value.call(41); }
EOF
try_ 42 << EOF
static int increment(int value) { return value + 1; }
struct callback { int padding; int (*call)(int); };
static struct callback value = {0, &increment};
int main(void) { return value.call(41); }
EOF
try_compile_error << EOF
struct invalid { int value : 33; };
EOF
try_compile_error << EOF
struct invalid { int value : 0; };
EOF
try_compile_error << EOF
struct invalid { _Bool value : 2; };
EOF
try_compile_error << EOF
struct invalid { char value : 1; };
EOF
try_compile_error << EOF
struct invalid { unsigned int *value : 1; };
EOF
try_compile_error << EOF
struct invalid { unsigned int value[1] : 1; };
EOF
try_compile_error << EOF
struct invalid { static unsigned int value : 1; };
EOF
try_compile_error << EOF
struct invalid { extern unsigned int value : 1; };
EOF
try_compile_error << EOF
struct flags { unsigned int value : 1; };
int main(void) { struct flags value; return (int)&value.value; }
EOF
try_compile_error << EOF
struct flags { unsigned int value : 1; };
int main(void) { struct flags value; return sizeof(value.value); }
EOF
try_compile_error << EOF
struct flags { const unsigned int value : 3; };
int main(void) { struct flags value = {1}; value.value = 2; return value.value; }
EOF

begin_category "Flexible array members"
try_ 16 << EOF
struct packet { int count; int values[]; };
int main(void) {
    int storage[4] = {0};
    struct packet *packet = (struct packet *)storage;
    packet->count = 3;
    packet->values[0] = 3;
    packet->values[1] = 5;
    packet->values[2] = 4;
    return packet->count + packet->values[0] + packet->values[1] +
           packet->values[2] + (sizeof(struct packet) == sizeof(int));
}
EOF
try_compile_error << EOF
union invalid { int values[]; };
EOF
try_compile_error << EOF
struct invalid { int values[]; int trailing; };
EOF
try_compile_error << EOF
struct invalid { int values[]; };
EOF
try_compile_error << EOF
struct flexible { int count; int values[]; };
struct invalid { struct flexible member; };
EOF
try_compile_error << EOF
struct flexible { int count; int values[]; };
struct flexible invalid[2];
EOF
try_compile_error << EOF
struct flexible { int count; int values[]; };
int main(void) {
    int storage[2];
    struct flexible *value = (struct flexible *)storage;
    return sizeof(value->values);
}
EOF
try_compile_error << EOF
struct flexible { int count; int values[]; };
int main(void) {
    int storage[2];
    struct flexible *value = (struct flexible *)storage;
    return sizeof((value->values));
}
EOF
try_compile_error << EOF
struct flexible { int count; int values[]; };
int main(void) {
    int storage[2];
    struct flexible *value = (struct flexible *)storage;
    return sizeof((*value).values);
}
EOF

begin_category "C99 Conformance Mode" "Testing --std=c99 extension diagnostics"

try_compile_error_flag --std=c99 << EOF
int main(void) { for (typedef int local_t; ; ) return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int a = 1, b = (int[]){2, 3}; return a + b; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { for (static int value = 0; value; ) return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int hidden(void) { return 0; }
int main(void) { for (extern int hidden(void); 0; ) return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { for (int hidden(void); 0; ) return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { for (int i = 0, hidden(void); i < 1; i++) ; return 0; }
EOF
try_ 1 << EOF
int main(void) { for (auto int value = 1; value; value--) return value; return 0; }
EOF
try_ 1 << EOF
int main(void) { for (register int value = 1; value; value--) return value; return 0; }
EOF

# The default remains intentionally permissive for existing users and suite
# coverage; strict mode makes the C99 boundary explicit.
try_ 2 << EOF
int main(void) { return 0b10; }
EOF
try_ 27 << EOF
int main(void) { return '\e'; }
EOF
try_ 1 << EOF
int main(void) { int value = (int[]){1, 2}; return value; }
EOF

try_compile_error_flag --std=c99 << EOF
int main(void) { return 0b10; }
EOF
try_compile_error_flag --std=c99 << EOF
struct empty_extension { };
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
union empty_extension { };
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
enum empty_extension { };
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int invalid_static_bound(int values[static 0]) { return values[0]; }
int main(void) { int values[1] = {0}; return invalid_static_bound(values); }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { return '\e'; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int value = (int[]){1, 2}; return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { char value = (char[]){1, 2}; return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { return (int[]){1, 2}; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int values[1] = {}; return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
struct value { int member; };
int main(void) { struct value item = {}; return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int value = (int){}; return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int *value = (int*){}; return value != 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int *value = (int[]){}; return value != 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int variadic(...) { return 0; }
int main(void) { return variadic(); }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int (*callback)(...) = 0; return callback != 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int identity(int value) { return value; }
int main(void) { return identity(1,); }
EOF
try_compile_error_flag --std=c99 << EOF
int identity(int value) { return value; }
int main(void) { int (*callback)(int) = identity; return callback(1,); }
EOF
try_compile_error_flag --std=c99 << EOF
int prototype(int,);
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int definition(int value,) { return value; }
int main(void) { return definition(0); }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { int (*callback)(int,) = 0; return callback != 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int prototype(void *value,);
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
void value_return(void) { return 1; }
int main(void) { value_return(); return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int bare_return(void) { return; }
int main(void) { return bare_return(); }
EOF
try_compile_error_flag --std=c99 << EOF
void *bare_pointer_return(void) { return; }
int main(void) { return bare_pointer_return() != 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    switch (0) { case 1: return 1; case 1: return 2; }
    return 0;
}
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    switch (0) { case 0x100000000LL: return 1; case 0: return 2; }
    return 0;
}
EOF
try_compile_flag --std=c99 << EOF
int main(void) {
    long long x = 0;
    switch (x) { case 0x100000000LL: return 1; case 0: return 2; }
    return 0;
}
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    switch (1) { case 1: int value = 1; return value; }
    return 0;
}
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { label: int value = 1; return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) { label: typedef int value; return 0; }
EOF

# The label lookahead resolves identifiers in the label's own scope: a block
# typedef starts a declaration, and an object hiding a file-scope typedef starts
# an expression statement.
try_compile_error_flag --std=c99 << EOF
int main(void) { typedef int T; goto label; label: T value = 0; return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    typedef int T;
    switch (1) { case 1: T value = 1; return value; }
    return 0;
}
EOF
try_flags 4 --std=c99 << EOF
typedef int T;
int main(void) { int T = 3; goto label; label: T = 4; return T; }
EOF
try_flags 5 --std=c99 << EOF
typedef int T;
int main(void) {
    int T = 0;
    switch (1) { case 1: T = 5; }
    return T;
}
EOF
try_compile_error_flag --std=c99 << EOF
enum { first = 1, second = 1 };
int main(void) {
    switch (0) { case first: return 1; case second: return 2; }
    return 0;
}
EOF
try_compile_error_flag --std=c99 << EOF
int invalid(int) { return 0; }
int main(void) { return invalid(0); }
EOF
try_compile_error_flag --std=c99 << EOF
int invalid(int, int value) { return value; }
int main(void) { return invalid(0, 1); }
EOF
try_compile_error_flag --std=c99 << EOF
int invalid(int (*)(int)) { return 0; }
int main(void) { return invalid(0); }
EOF
try_compile_error_flag --std=c99 << EOF
main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
extern legacy(void);
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int invalid(int values[const]) { values = 0; return 0; }
int main(void) { return 0; }
EOF
try_compile_error_flag --std=c99 << EOF
int legacy();
int legacy(char value) { return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int legacy();
int legacy(short value) { return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int legacy();
int legacy(_Bool value) { return value; }
EOF
try_compile_error_flag --std=c99 << EOF
int legacy();
int legacy(int value, ...) { return value; }
EOF

# Ordinary C99 spellings remain accepted in strict mode.
try_flags 0 --std=c99 << EOF
int main(void) { label: ; return 0; }
EOF
try_flags 2 --std=c99 << EOF
int main(void) { return 0x2; }
EOF
try_flags 27 --std=c99 << EOF
int main(void) { return '\033'; }
EOF
try_flags 0 --std=c99 << EOF
int main(void) { int *value = (int[]){1, 2}; return value[0] != 1; }
EOF
try_ 0 << EOF
struct value { int member; };
int main(void) {
    int values[1] = {};
    struct value item = {};
    int value = (int){};
    int *pointer = (int*){};
    return values[0] || item.member || value || pointer != 0;
}
EOF
try_compile_flag --std=c99 << EOF
typedef int *int_pointer;
int_pointer values(void) { return (int[]){1, 2}; }
int main(void) { values(); return 0; }
EOF
try_compile_flag --std=c99 << EOF
typedef int *int_pointer;
int second(int_pointer value) { return value[1]; }
int main(void) {
    int_pointer value = (int[]){1, 2};
    return second((int[]){1, 2}) != 2 || value[0] != 1;
}
EOF
try_flags 7 --std=c99 << EOF
int variadic(int value, ...) { return value; }
int main(void) { return variadic(7, 1, 2); }
EOF
try_flags 2 --std=c99 << EOF
int identity(int value) { return value; }
int main(void) { return identity((1, 2)); }
EOF
try_compile_flag --std=c99 << EOF
int prototype(int value);
int variadic(int value, ...);
int main(void) { return 0; }
EOF
try_flags 4 --std=c99 << EOF
int twice(int);
int twice(int value) { return value * 2; }
int main(void) {
    extern int twice(int);
    return twice(2);
}
EOF
try_flags 5 --std=c99 << EOF
int legacy();
int legacy(int value) { return value; }
int pointer_legacy();
int pointer_legacy(int *value) { return *value; }
int main(void) { int value = 3; return legacy(2) + pointer_legacy(&value); }
EOF
try_compile_flag --std=c99 << EOF
int consume(const int *, int [static 2]);
int consume(const int *first, int values[static 2]) {
    return *first + values[1];
}
int (*callback)(int);
int main(void) { return 0; }
EOF
try_flags 5 --std=c99 << EOF
int first(const int values[const]) { return values[0]; }
int bump(int values[restrict]) { values[0]++; return values[0]; }
int main(void) {
    int values[1] = {3};
    return first(values) + bump(values) - 2;
}
EOF
try_flags 0 --std=c99 << EOF
int plus1(int value) { return value + 1; }
int invoke(int (*)(int));
int invoke(int (*callback)(int)) { return callback(4) - 5; }
int takes(int (*)(int, int));
int takes_const(int (* const)(int));
int main(void) { int (*callback)(int) = plus1; return invoke(callback); }
EOF
try_flags 1 --std=c99 << EOF
void no_value(void) { return; }
int with_value(void) { return 1; }
int main(void) { no_value(); return with_value(); }
EOF
try_flags 2 --std=c99 << EOF
int main(void) {
    switch (2) { case 1: return 1; case 2: return 2; }
    return 0;
}
EOF
try_flags 11 --std=c99 << EOF
enum { base = 2 };
int main(void) {
    switch (sizeof(int) + base) {
    case -(-((0 ? 1 : 1) << base)) + sizeof(short): return 11;
    default: return 0;
    }
}
EOF
try_flags 4 --std=c99 << EOF
int select_value(int value) {
    switch (value) { default: return 3; case 1: return 1; }
}
int main(void) { return select_value(1) + select_value(2); }
EOF
try_flags 17 --std=c99 << EOF
int score(int value) {
    int total = 0;
    switch (value) {
    case 1: total = 1;
    default: total += 2;
    case 2: total += 4;
    }
    return total;
}
int main(void) { return score(1) + score(2) + score(9); }
EOF
try_flags 7 --std=c99 << EOF
int select_label_statement(int value) {
    int total = 0;
    switch (value) {
    total = 100;
    case 1: total += 1; break;
    {
    case 2: total += 2; break;
    }
    default: total += 4;
    }
    return total;
}
int main(void) {
    return select_label_statement(1) + select_label_statement(2) +
           select_label_statement(9);
}
EOF
try_flags 2 --std=c99 << EOF
int main(void) {
    int value = 0;
    switch (3) {
    default:
        switch (2) { default: value += 1; case 2: value += 2; }
        break;
    case 1: value += 0;
    }
    return value;
}
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    switch (0) { default: return 1; default: return 2; }
}
EOF
try_compile_error_flag --std=c99 << EOF
int main(void) {
    switch (0) { case 1 + 1: return 1; case 2: return 2; }
}
EOF
try_ 0 << EOF
int variadic(...) { return 0; }
int main(void) { return variadic(); }
EOF
try_compile_error_message "Expected an argument after ','" << EOF
int identity(int value) { return value; }
int main(void) { return identity(1,); }
EOF
try_compile_error << EOF
int identity(int value) { return value; }
int main(void) { int (*callback)(int) = identity; return callback(1,); }
EOF
try_compile_error << EOF
int add(int left, int right) { return left + right; }
int main(void) { return add(1 2); }
EOF

begin_category "C99 assert.h" "Testing freestanding assertion semantics"

# A failed assertion in main names its expression and the function, in the words
# of whichever libc reports it: shecc's own for a static build, and glibc's for
# a dynamic one.
function assertion_message()
{
    if [ "$LINK_MODE" = "dynamic" ]; then
        echo "main: Assertion \`$1' failed"
    else
        echo "Assertion failed: $1, function main"
    fi
}

try_ 0 << EOF
#include <assert.h>
int main(void) { int calls = 0; assert(++calls == 1); return calls - 1; }
EOF
try_ 0 << EOF
#define NDEBUG
#include <assert.h>
int main(void) { int calls = 0; assert(++calls); return calls; }
EOF
try_ 0 << EOF
#define NDEBUG
#include <assert.h>
int main(void) { int calls = 0; (assert(++calls), calls); return calls; }
EOF
try_failure_output "$(assertion_message "0")" << EOF
#define NDEBUG
#include <assert.h>
#undef NDEBUG
#include <assert.h>
int main(void) { assert(0); return 0; }
EOF
try_ 0 << EOF
#include <assert.h>
#define NDEBUG
#include <assert.h>
int main(void) { int calls = 0; assert(++calls); return calls; }
EOF
try_failure_output "$(assertion_message "2 + 2 == 5")" << EOF
#include <assert.h>
int main(void) { assert(2 + 2 == 5); return 0; }
EOF
try_failure_output "$(assertion_message "SUM(1, 1) == 3")" << EOF
#include <assert.h>
#define SUM(a, b) a + b
int main(void) { assert(SUM(1, 1) == 3); return 0; }
EOF

# The diagnostic goes to standard error, leaving standard output to the program:
# a static build prints only abort()'s own line there, a dynamic one nothing.
if [ "$LINK_MODE" = "dynamic" ]; then
    try_output 134 "" << EOF
#include <assert.h>
int main(void) { assert(0); return 0; }
EOF
else
    try_output 255 "Abnormal program termination" << EOF
#include <assert.h>
int main(void) { assert(0); return 0; }
EOF
fi

begin_category "C99 stdarg.h" "Testing variadic argument macros"

try_ 0 << EOF
#include <stdarg.h>
int sum(int count, ...) {
    va_list ap;
    int total = 0;
    va_start(ap, count);
    for (int i = 0; i < count; i++) total += va_arg(ap, int);
    va_end(ap);
    return total;
}
int main(void) { return sum(3, 1, 2, 3) != 6; }
EOF

try_ 0 << EOF
#include <stdarg.h>
int tenth(int count, ...) {
    va_list ap;
    int value = 0;
    va_start(ap, count);
    for (int i = 0; i < count; i++) value = va_arg(ap, int);
    va_end(ap);
    return value;
}
int main(void) { return tenth(7, 1, 2, 3, 4, 5, 6, 7) != 7; }
EOF

try_ 0 << EOF
#include <stdarg.h>
int after_char(char last, ...) {
    va_list ap;
    va_start(ap, last);
    return va_arg(ap, int);
}
int after_pointer(int *last, ...) {
    va_list ap;
    va_start(ap, last);
    return va_arg(ap, int);
}
int main(void) {
    int value = 0;
    return after_char(0, 2) != 2 || after_pointer(&value, 3) != 3;
}
EOF
try_ 0 << EOF
#include <stdarg.h>
int after_wide(long long last, ...) {
    va_list ap;
    va_start(ap, last);
    return va_arg(ap, int);
}
int main(void) { return after_wide(0, 4) != 4; }
EOF

try_ 0 << EOF
#include <stdarg.h>
struct named_record { int first; int second; int third; };
int after_record(struct named_record last, ...) {
    va_list ap;
    va_start(ap, last);
    return va_arg(ap, int);
}
int main(void) {
    struct named_record value = {1, 2, 3};
    return after_record(value, 9) != 9;
}
EOF

try_ 0 << EOF
#include <stdarg.h>
struct trailing_record { int first; int second; int third; };
int after_trailing_record(int tag, struct trailing_record last, ...) {
    va_list ap;
    va_start(ap, last);
    return tag != 4 || va_arg(ap, int) != 10;
}
int main(void) {
    struct trailing_record value = {1, 2, 3};
    return after_trailing_record(4, value, 10);
}
EOF

# A variadic function returning a record receives the hidden result pointer in
# the first argument word. Its named parameter and the unnamed arguments that
# follow must be saved from the words after it.
try_ 0 << EOF
#include <stdarg.h>
struct varargs_result { int count; int first; int second; };
struct varargs_result collect(int count, ...) {
    struct varargs_result result;
    va_list ap;
    va_start(ap, count);
    result.count = count;
    result.first = va_arg(ap, int);
    result.second = va_arg(ap, int);
    va_end(ap);
    return result;
}
int main(void) {
    struct varargs_result r = collect(3, 40, 50);
    return r.count != 3 || r.first != 40 || r.second != 50;
}
EOF

# A record element selected through a pointer-to-array va_arg type is copied
# whole; a record wider than a register must not be read as one scalar.
try_ 29 << EOF
#include <stdarg.h>
struct wide_item { int first; int second; int third; };
int pick_wide_item(int count, ...) {
    va_list ap;
    va_start(ap, count);
    struct wide_item result = va_arg(ap, struct wide_item (*)[2])[0][1];
    return result.second + result.third;
}
int main(void) {
    struct wide_item items[2] = {{1, 2, 3}, {4, 9, 20}};
    return pick_wide_item(1, &items);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
long long wide_argument(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, long long);
}
int main(void) { return wide_argument(1, 1234567890123LL) != 1234567890123LL; }
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int *int_pointer;
struct item { int value; };
int type_names(int count, ...) {
    va_list ap;
    va_start(ap, count);
    unsigned int u = va_arg(ap, unsigned int);
    unsigned long l = va_arg(ap, unsigned long);
    const int *p = va_arg(ap, const int *);
    int_pointer q = va_arg(ap, int_pointer);
    struct item *r = va_arg(ap, struct item *);
    va_end(ap);
    return u != 7U || l != 8UL || *p != 9 || *q != 10 || r->value != 11;
}
int main(void) {
    int p = 9, q = 10;
    struct item r = { 11 };
    return type_names(5, 7U, 8UL, &p, &q, &r);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
enum colour { red = 3, blue = 7 };
int enum_argument(int count, ...) {
    va_list ap;
    va_start(ap, count);
    enum colour shade = va_arg(ap, enum colour);
    va_end(ap);
    return shade != blue;
}
int main(void) {
    return enum_argument(1, blue);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
int direct_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, int (*)[2])[0][1] != 7;
}
int main(void) { int row[2] = { 1, 7 }; return direct_pointer_to_array(1, &row); }
EOF

try_compile_error << EOF
#include <stdarg.h>
typedef int row[2];
int unsupported_typedef(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, row);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int row[2];
int typedef_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, row *)[0][1] != 9;
}
int main(void) { row value = { 4, 9 }; return typedef_pointer_to_array(1, &value); }
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int row[2];
typedef row *row_pointer;
int typedef_alias_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, row_pointer)[0][1] != 9;
}
int main(void) { row value = { 4, 9 }; return typedef_alias_pointer_to_array(1, &value); }
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int row3[3];
typedef row3 *row3_pointer;
int typedef_alias_row3(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, row3_pointer)[0][2] != 9;
}
int main(void) { row3 value = { 1, 2, 9 }; return typedef_alias_row3(1, &value); }
EOF

try_ 0 << EOF
#include <stdarg.h>
struct item { int first; int second; };
typedef struct item item_t;
int aggregate_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    item_t result = va_arg(ap, item_t (*)[2])[0][1];
    return result.second != 9;
}
int main(void) { item_t value[2] = {{1, 2}, {3, 9}}; return aggregate_pointer_to_array(1, &value); }
EOF

try_ 0 << EOF
#include <stdarg.h>
struct item { int value; };
int direct_aggregate_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    struct item result = va_arg(ap, struct item (*)[2])[0][1];
    return result.value != 8;
}
int main(void) { struct item value[2] = {{4}, {8}}; return direct_aggregate_pointer_to_array(1, &value); }
EOF

try_ 0 << EOF
#include <stdarg.h>
int pointer_element_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, int * (*)[2])[0][1] != 9;
}
int main(void) {
    int first = 4, second = 9;
    int *value[2] = { &first, &second };
    return pointer_element_array(1, &value);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int *pointer;
typedef pointer row[2];
int typedef_pointer_element_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, row *)[0][1] != 8;
}
int main(void) {
    int first = 3, second = 8;
    row value = { &first, &second };
    return typedef_pointer_element_array(1, &value);
}
EOF

# The element a pointer-to-array va_arg type selects is a pointer to int, and
# dereferencing it reads an int. A typedef carrying the element's star had it
# counted twice, so the dereference read a pointer's width; on an LP64 target
# that took in the neighbouring int as well.
try_ 0 << EOF
#include <stdarg.h>
typedef int *pointer;
typedef pointer row[2];
int typedef_pointer_element_width(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, row *)[0][1] != 8;
}
int main(void) {
    int pair[2] = { 8, 5 };
    row value = { &pair[1], &pair[0] };
    return typedef_pointer_element_width(1, &value);
}
EOF

# An element whose pointer comes from a typedef points at the typedef's scalar,
# so dereferencing it reads that scalar's width rather than a whole pointer.
try_ 0 << EOF
#include <stdarg.h>
typedef char *text;
typedef text words[2];
int typedef_char_pointer_element(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, words *)[0][1] != 'c';
}
int main(void) {
    words value = { "ab", "cd" };
    return typedef_char_pointer_element(1, &value);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
typedef int *pointer;
int parenthesized_pointer_element_width(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, pointer (*)[2])[0][1] != 8;
}
int main(void) {
    int pair[2] = { 8, 5 };
    pointer value[2] = { &pair[1], &pair[0] };
    return parenthesized_pointer_element_width(1, &value);
}
EOF
try_ 0 << EOF
#include <stdarg.h>
typedef char *text;
int typedef_pointer_declarator_element(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return *va_arg(ap, text (*)[2])[0][1] != 'c';
}
int main(void) {
    text value[2] = { "ab", "cd" };
    return typedef_pointer_declarator_element(1, &value);
}
EOF

try_compile_error << EOF
#include <stdarg.h>
int unsupported_void_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, void (*)[2]) != 0;
}
EOF

try_ 0 << EOF
#include <stdarg.h>
int two_dimensional_pointer_to_array(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, int (*)[2][3])[1][1][2] != 12;
}
int main(void) {
    int matrices[2][2][3] = {
        { { 1, 2, 3 }, { 4, 5, 6 } },
        { { 7, 8, 9 }, { 10, 11, 12 } }
    };
    return two_dimensional_pointer_to_array(1, matrices);
}
EOF

try_ 0 << EOF
#include <stdarg.h>
int plus_five(int value) { return value + 5; }
int invoke(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, int (*)(int))(4) != 9;
}
int main(void) { int (*callback)(int) = plus_five; return invoke(1, callback); }
EOF

try_ 0 << EOF
#include <stdarg.h>
int once(int count, ...) {
    va_list cursors[2];
    int i = 0;
    va_start(cursors[0], count);
    int value = va_arg(cursors[i++], int);
    return value != 12 ? 1 : i != 1 ? 2 : 0;
}
int main(void) { return once(1, 12); }
EOF

try_compile_error << EOF
#include <stdarg.h>
int bad(int count, ...) {
    va_list ap;
    va_start(ap, count);
    va_arg(ap, void);
    return 0;
}
EOF

try_compile_error << EOF
#include <stdarg.h>
struct incomplete;
int bad(int count, ...) {
    va_list ap;
    va_start(ap, count);
    va_arg(ap, struct incomplete);
    return 0;
}
EOF

# A va_arg type name reads its specifiers like any other type name: in any
# order, through a block-scope typedef, and without a floating type, which would
# enter the integer-only argument ABI however it is spelled.
try_ 42 << EOF
#include <stdarg.h>
int pick(int count, ...) {
    typedef long long wide;
    va_list ap;
    va_start(ap, count);
    wide first = va_arg(ap, wide);
    int second = va_arg(ap, int const unsigned);
    va_end(ap);
    return (int) first + second;
}
int main(void) { return pick(2, (long long) 2, 40); }
EOF
try_compile_error_message "Floating point types are not yet supported" << EOF
#include <stdarg.h>
int bad(int count, ...) {
    va_list ap;
    va_start(ap, count);
    va_arg(ap, double);
    return 0;
}
EOF
try_compile_error_message "Floating point types are not yet supported" << EOF
#include <stdarg.h>
typedef float real;
int bad(int count, ...) {
    va_list ap;
    va_start(ap, count);
    va_arg(ap, real);
    return 0;
}
EOF
try_compile_error_message "record type cannot be combined with integer specifiers" << EOF
#include <stdarg.h>
struct item { int value; };
int bad(int count, ...) {
    va_list ap;
    va_start(ap, count);
    va_arg(ap, signed struct item *);
    return 0;
}
EOF

try_ 0 << EOF
#include <stdarg.h>
struct payload { int first; int second; int third; };
union word { int value; int other; };
int consume(int count, ...) {
    va_list ap;
    va_start(ap, count);
    struct payload item = va_arg(ap, struct payload);
    union word choice = va_arg(ap, union word);
    int sum = item.first + item.second + item.third + choice.value;
    item.first = 99;
    va_end(ap);
    return sum;
}
int main(void) {
    struct payload value = { 1, 2, 3 };
    union word choice = { 4 };
    return consume(2, value, choice) != 10 || value.first != 1;
}
EOF

try_ 0 << EOF
#include <stdarg.h>
int inspect(int count, ...) {
    int *pointer;
    va_list ap, copy;
    va_start(ap, count);
    va_copy(copy, ap);
    va_arg(ap, int);
    pointer = va_arg(ap, int *);
    int value = va_arg(copy, int);
    va_end(copy);
    va_end(ap);
    return value != 4 || *pointer != 9;
}
int main(void) { int marker = 9; return inspect(2, 4, &marker); }
EOF

try_flags 0 "--no-libc" << EOF
#include <stdarg.h>
int first(int count, ...) {
    va_list ap;
    va_start(ap, count);
    return va_arg(ap, int);
}
int main(void) { return first(1, 0); }
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
