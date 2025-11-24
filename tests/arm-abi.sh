#!/usr/bin/env bash

# AAPCS (ARM Architecture Procedure Call Standard) Compliance Test Suite

set -u

# Test Configuration
readonly VERBOSE_MODE="${VERBOSE:-1}"
readonly SHOW_SUMMARY="${SHOW_SUMMARY:-1}"
readonly SHOW_PROGRESS="${SHOW_PROGRESS:-1}"
readonly COLOR_OUTPUT="${COLOR_OUTPUT:-1}"

# Test Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0

# Category Tracking
declare -A CATEGORY_TESTS
declare -A CATEGORY_PASSED
declare -A CATEGORY_FAILED
CURRENT_CATEGORY="Parameter Passing"

# Performance Metrics
TEST_START_TIME=$(date +%s)
PROGRESS_COUNT=0

# Colors
if [[ "$COLOR_OUTPUT" == "1" && -t 1 ]]; then
    RED='\033[0;31m'
    GREEN='\033[0;32m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    CYAN='\033[0;36m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED='' GREEN='' YELLOW='' BLUE='' CYAN='' BOLD='' NC=''
fi

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
    exit 1
fi

case "$1" in
    "0")
        readonly SHECC="$PWD/out/shecc"
        readonly STAGE="Stage 0 (Host Compiler)" ;;
    "1")
        readonly SHECC="${TARGET_EXEC:-} $PWD/out/shecc-stage1.elf"
        readonly STAGE="Stage 1 (Cross-compiled)" ;;
    "2")
        readonly SHECC="${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf"
        readonly STAGE="Stage 2 (Self-hosted)" ;;
    *)
        echo "Error: Invalid stage '$1'. Use 0, 1, or 2."
        exit 1 ;;
esac

DYNLINK="${2:-0}"

# Banner
echo -e "${BLUE}${BOLD}========================================${NC}"
echo -e "${BLUE}${BOLD}AAPCS Compliance Test Suite${NC}"
echo -e "${BLUE}${BOLD}========================================${NC}"
echo -e "Stage:       $STAGE"
echo -e "Link Mode:   $([ "$DYNLINK" == "1" ] && echo "Dynamic" || echo "Static")"
echo -e "Compiler:    $SHECC"
echo ""

# Helper Functions
update_category_stats() {
    local category="$1"
    local result="$2"  # "pass" or "fail"

    if [[ -z "${CATEGORY_TESTS[$category]:-}" ]]; then
        CATEGORY_TESTS[$category]=0
        CATEGORY_PASSED[$category]=0
        CATEGORY_FAILED[$category]=0
    fi

    CATEGORY_TESTS[$category]=$((${CATEGORY_TESTS[$category]} + 1))

    if [[ "$result" == "pass" ]]; then
        CATEGORY_PASSED[$category]=$((${CATEGORY_PASSED[$category]} + 1))
    else
        CATEGORY_FAILED[$category]=$((${CATEGORY_FAILED[$category]} + 1))
    fi
}

show_progress() {
    if [[ "$SHOW_PROGRESS" == "1" ]]; then
        echo -n "."
        PROGRESS_COUNT=$((PROGRESS_COUNT + 1))
        if [[ $((PROGRESS_COUNT % 50)) -eq 0 ]]; then
            echo ""
        fi
    fi
}

# Test execution function
run_abi_test() {
    local test_name="$1"
    local category="$2"
    local source_code="$3"
    local expected_output="$4"
    local skip_static="${5:-0}"

    CURRENT_CATEGORY="$category"
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    # Skip if dynamic linking required but we're in static mode
    if [[ "$skip_static" == "1" && "$DYNLINK" == "0" ]]; then
        if [[ "$VERBOSE_MODE" == "1" ]]; then
            echo -e "${YELLOW}SKIP${NC}: $test_name (requires dynamic linking)"
        fi
        SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
        show_progress
        return
    fi

    # Create temporary test file
    local test_file="/tmp/shecc_abi_test_$$.c"
    echo "$source_code" > "$test_file"

    # Compile
    local compile_cmd="$SHECC"
    if [[ "$DYNLINK" == "1" ]]; then
        compile_cmd="$compile_cmd --dynlink"
    fi
    compile_cmd="$compile_cmd -o /tmp/shecc_abi_test_$$.elf $test_file"

    local compile_output
    if ! compile_output=$(eval "$compile_cmd" 2>&1); then
        if [[ "$VERBOSE_MODE" == "1" ]]; then
            echo -e "${RED}FAIL${NC}: $test_name (compilation failed)"
            echo "$compile_output" | sed 's/^/  /'
        fi
        FAILED_TESTS=$((FAILED_TESTS + 1))
        update_category_stats "$category" "fail"
        rm -f "$test_file"
        show_progress
        return
    fi

    # Run
    chmod +x "/tmp/shecc_abi_test_$$.elf"
    local run_cmd="${TARGET_EXEC:-}"
    run_cmd="$run_cmd /tmp/shecc_abi_test_$$.elf"

    local run_output
    local exit_code
    run_output=$(eval "$run_cmd" 2>&1)
    exit_code=$?

    # Check result
    if [[ $exit_code -eq 0 ]]; then
        if [[ "$VERBOSE_MODE" == "1" ]]; then
            echo -e "${GREEN}PASS${NC}: $test_name"
            if [[ -n "$expected_output" && "$run_output" != *"$expected_output"* ]]; then
                echo -e "${YELLOW}Warning: Output mismatch${NC}"
                echo "Expected: $expected_output"
                echo "Got: $run_output"
            fi
        fi
        PASSED_TESTS=$((PASSED_TESTS + 1))
        update_category_stats "$category" "pass"
    else
        if [[ "$VERBOSE_MODE" == "1" ]]; then
            echo -e "${RED}FAIL${NC}: $test_name (exit code $exit_code)"
            echo "$run_output" | sed 's/^/  /'
        fi
        FAILED_TESTS=$((FAILED_TESTS + 1))
        update_category_stats "$category" "fail"
    fi

    # Cleanup
    rm -f "$test_file" "/tmp/shecc_abi_test_$$.elf"
    show_progress
}

# Parameter Passing Tests

test_one_arg() {
    run_abi_test "One argument (r0)" "Parameter Passing" '
#include <stdio.h>
int add_42(int x) { return x + 42; }
int main() {
    int result = add_42(8);
    if (result == 50) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected 50, got %d\n", result);
    return 1;
}
' "PASS"
}

test_two_args() {
    run_abi_test "Two arguments (r0, r1)" "Parameter Passing" '
#include <stdio.h>
int add(int a, int b) { return a + b; }
int main() {
    int result = add(10, 20);
    if (result == 30) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected 30, got %d\n", result);
    return 1;
}
' "PASS"
}

test_four_args() {
    run_abi_test "Four arguments (r0-r3)" "Parameter Passing" '
#include <stdio.h>
int sum4(int a, int b, int c, int d) { return a + b + c + d; }
int main() {
    int result = sum4(10, 20, 30, 40);
    if (result == 100) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected 100, got %d\n", result);
    return 1;
}
' "PASS"
}

test_five_args() {
    run_abi_test "Five arguments (r0-r3 + stack)" "Parameter Passing" '
#include <stdio.h>
int sum5(int a, int b, int c, int d, int e) { return a + b + c + d + e; }
int main() {
    int result = sum5(1, 2, 3, 4, 5);
    if (result == 15) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected 15, got %d\n", result);
    return 1;
}
' "PASS"
}

test_eight_args() {
    run_abi_test "Eight arguments (stack-heavy)" "Parameter Passing" '
#include <stdio.h>
int sum8(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}
int main() {
    int result = sum8(1, 2, 3, 4, 5, 6, 7, 8);
    if (result == 36) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: expected 36, got %d\n", result);
    return 1;
}
' "PASS"
}

# Stack Alignment Tests

test_stack_alignment_basic() {
    run_abi_test "Basic stack alignment" "Stack Alignment" '
#include <stdio.h>
int is_aligned(void *ptr) {
    int addr = (int)ptr;
    return (addr & 0x7) == 0;
}
int check_alignment(int a, int b) {
    int local;
    return !is_aligned(&local);
}
int main() {
    if (check_alignment(1, 2) == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: stack not aligned\n");
    return 1;
}
' "PASS"
}

test_stack_alignment_extended() {
    run_abi_test "Stack alignment with extended args" "Stack Alignment" '
#include <stdio.h>
int is_aligned(void *ptr) {
    int addr = (int)ptr;
    return (addr & 0x7) == 0;
}
int check_extended(int a, int b, int c, int d, int e, int f) {
    int local;
    return is_aligned(&local) ? (a+b+c+d+e+f) : -1;
}
int main() {
    int result = check_extended(1, 2, 3, 4, 5, 6);
    if (result == 21) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: result=%d\n", result);
    return 1;
}
' "PASS"
}

# Return Value Tests

test_return_char() {
    run_abi_test "Return char value" "Return Values" '
#include <stdio.h>
char get_char(void) { return '\''A'\''; }
int main() {
    if (get_char() == '\''A'\'') {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS"
}

test_return_int() {
    run_abi_test "Return int value" "Return Values" '
#include <stdio.h>
int get_value(void) { return 12345; }
int main() {
    if (get_value() == 12345) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS"
}

test_return_pointer() {
    run_abi_test "Return pointer value" "Return Values" '
#include <stdio.h>
int *return_ptr(int *p) { return p; }
int main() {
    int x = 42;
    int *ptr = return_ptr(&x);
    if (ptr == &x && *ptr == 42) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS"
}

# External Function Call Tests (Dynamic Linking Only)

test_printf_one_arg() {
    run_abi_test "printf with 1 argument" "External Calls" '
#include <stdio.h>
int main() {
    printf("PASS\n");
    return 0;
}
' "PASS" 1
}

test_printf_multi_args() {
    run_abi_test "printf with 5 arguments" "External Calls" '
#include <stdio.h>
int main() {
    printf("Values: %d %d %d %d\n", 1, 2, 3, 4);
    printf("PASS\n");
    return 0;
}
' "PASS" 1
}

test_strlen() {
    run_abi_test "strlen external call" "External Calls" '
#include <stdio.h>
#include <string.h>
int main() {
    char str[] = "Hello";
    if (strlen(str) == 5) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS" 1
}

test_strcpy() {
    run_abi_test "strcpy external call" "External Calls" '
#include <stdio.h>
#include <string.h>
int main() {
    char dest[20];
    char src[] = "Test";
    strcpy(dest, src);
    if (strcmp(dest, "Test") == 0) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS" 1
}

test_memcpy() {
    run_abi_test "memcpy external call" "External Calls" '
#include <stdio.h>
#include <string.h>
int main() {
    int src[3] = {1, 2, 3};
    int dst[3];
    memcpy(dst, src, 3 * sizeof(int));
    if (dst[0] == 1 && dst[1] == 2 && dst[2] == 3) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS" 1
}

# Register Preservation Tests

test_local_vars_preserved() {
    run_abi_test "Local variables preserved across calls" "Register Preservation" '
#include <stdio.h>
int dummy(int a, int b, int c, int d, int e, int f, int g, int h) {
    return a + b + c + d + e + f + g + h;
}
int main() {
    int v1 = 100, v2 = 200, v3 = 300, v4 = 400;
    dummy(1, 2, 3, 4, 5, 6, 7, 8);
    if (v1 == 100 && v2 == 200 && v3 == 300 && v4 == 400) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL: locals corrupted\n");
    return 1;
}
' "PASS"
}

test_recursive_preservation() {
    run_abi_test "Register preservation in recursion" "Register Preservation" '
#include <stdio.h>
int factorial(int n) {
    if (n <= 1) return 1;
    int local = n;
    int result = factorial(n - 1);
    return (local == n) ? n * result : -1;
}
int main() {
    if (factorial(5) == 120) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS"
}

# Structure Passing Tests

test_small_struct() {
    run_abi_test "Small struct passing (≤4 bytes)" "Structure Passing" '
#include <stdio.h>
typedef struct { char a; char b; short c; } SmallStruct;
int sum_struct(SmallStruct s) { return s.a + s.b + s.c; }
int main() {
    SmallStruct s = {10, 20, 30};
    if (sum_struct(s) == 60) {
        printf("PASS\n");
        return 0;
    }
    printf("FAIL\n");
    return 1;
}
' "PASS"
}

# Run all tests

echo -e "${CYAN}Running Parameter Passing Tests...${NC}"
test_one_arg
test_two_args
test_four_args
test_five_args
test_eight_args

echo ""
echo -e "${CYAN}Running Stack Alignment Tests...${NC}"
test_stack_alignment_basic
test_stack_alignment_extended

echo ""
echo -e "${CYAN}Running Return Value Tests...${NC}"
test_return_char
test_return_int
test_return_pointer

echo ""
if [[ "$DYNLINK" == "1" ]]; then
    echo -e "${CYAN}Running External Function Call Tests...${NC}"
    test_printf_one_arg
    test_printf_multi_args
    test_strlen
    test_strcpy
    test_memcpy
else
    echo -e "${YELLOW}Skipping External Function Call Tests (requires dynamic linking)${NC}"
    SKIPPED_TESTS=$((SKIPPED_TESTS + 5))
fi

echo ""
echo -e "${CYAN}Running Register Preservation Tests...${NC}"
test_local_vars_preserved
test_recursive_preservation

echo ""
echo -e "${CYAN}Running Structure Passing Tests...${NC}"
test_small_struct

# SUMMARY

echo ""
echo ""

if [[ "$SHOW_SUMMARY" == "1" ]]; then
    echo -e "${BLUE}${BOLD}========================================${NC}"
    echo -e "${BLUE}${BOLD}Category Summary${NC}"
    echo -e "${BLUE}${BOLD}========================================${NC}"

    for category in "${!CATEGORY_TESTS[@]}"; do
        total="${CATEGORY_TESTS[$category]}"
        passed="${CATEGORY_PASSED[$category]}"
        failed="${CATEGORY_FAILED[$category]}"
        pct=0
        if [[ $total -gt 0 ]]; then
            pct=$((passed * 100 / total))
        fi

        printf "%-25s: " "$category"
        if [[ $failed -eq 0 ]]; then
            echo -e "${GREEN}$passed/$total PASSED${NC} (${pct}%%)"
        else
            echo -e "${RED}$passed/$total PASSED${NC}, ${RED}$failed FAILED${NC} (${pct}%%)"
        fi
    done
    echo ""
fi

echo -e "${BLUE}${BOLD}========================================${NC}"
echo -e "${BLUE}${BOLD}Overall Test Results${NC}"
echo -e "${BLUE}${BOLD}========================================${NC}"
echo -e "Total Tests:    $TOTAL_TESTS"
echo -e "${GREEN}Passed:         $PASSED_TESTS${NC}"

if [[ $FAILED_TESTS -gt 0 ]]; then
    echo -e "${RED}Failed:         $FAILED_TESTS${NC}"
else
    echo -e "Failed:         $FAILED_TESTS"
fi

if [[ $SKIPPED_TESTS -gt 0 ]]; then
    echo -e "${YELLOW}Skipped:        $SKIPPED_TESTS${NC}"
fi

TEST_END_TIME=$(date +%s)
TEST_DURATION=$((TEST_END_TIME - TEST_START_TIME))
echo -e "Duration:       ${TEST_DURATION}s"
echo ""

if [[ $FAILED_TESTS -gt 0 ]]; then
    echo -e "${RED}${BOLD}Some ABI tests FAILED!${NC}"
    exit 1
else
    echo -e "${GREEN}${BOLD}All ABI tests PASSED!${NC}"
    exit 0
fi
