#!/usr/bin/env bash
set -euo pipefail

source tests/vir-direct-common.sh

stage=$1
if test "$stage" -eq 2; then
    read -r -a compiler <<< "${TARGET_EXEC:-} $PWD/out/shecc-stage2.elf"
else
    compiler=(out/shecc)
fi
read -r -a runner <<< "${TARGET_EXEC:-}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

inputs=(tests/vir-multi-input-root.c tests/vir-multi-input-main.c)

"${compiler[@]}" -E "${inputs[@]}" > "$work/preprocessed"
grep -q '^int shared_root;$' "$work/preprocessed"
grep -q '^extern int shared_root;$' "$work/preprocessed"
printf 'int seam_a;' > "$work/seam-a.c"
printf 'int seam_b;\n' > "$work/seam-b.c"
"${compiler[@]}" -E --no-libc "$work/seam-a.c" "$work/seam-b.c" \
    > "$work/seam"
printf 'int seam_a;\nint seam_b;\n' > "$work/seam.expected"
cmp "$work/seam.expected" "$work/seam"
printf 'int seam_a;\n' > "$work/seam-a.c"
"${compiler[@]}" -E --no-libc "$work/seam-a.c" "$work/seam-b.c" \
    > "$work/seam"
cmp "$work/seam.expected" "$work/seam"

printf 'enum { vir_multi_input_hidden_enum = 5 };\n' \
    > "$work/enum-root.c"
printf 'int main(void) { return vir_multi_input_hidden_enum; }\n' \
    > "$work/enum-main.c"
if "${compiler[@]}" --no-libc -o "$work/enum-leak" \
    "$work/enum-root.c" "$work/enum-main.c" \
    > "$work/enum-leak.error" 2>&1; then
    echo "enumerator leaked across translation units" >&2
    exit 1
fi
grep -q 'Unrecognized expression token' "$work/enum-leak.error"

# Record compatibility across units compares pointer volatility as well as
# constness.
printf 'struct S { int *volatile p; };\nint f(struct S *s) { return !s; }\n' \
    > "$work/volatile-root.c"
printf 'struct S { int *p; };\nint f(struct S *s);\nint main(void) { return f(0); }\n' \
    > "$work/volatile-main.c"
if "${compiler[@]}" --no-libc -o "$work/volatile-mismatch" \
    "$work/volatile-root.c" "$work/volatile-main.c" \
    > "$work/volatile-mismatch.error" 2>&1; then
    echo "pointer volatility ignored across translation units" >&2
    exit 1
fi
grep -q 'conflicting types' "$work/volatile-mismatch.error"
printf '%s\n' \
    'enum { vir_multi_input_enum_value = 7, vir_multi_input_enum_other = 8 };' \
    'int main(void) {' \
    '    int vir_multi_input_enum_value = 3;' \
    '    return vir_multi_input_enum_value + vir_multi_input_enum_other != 11;' \
    '}' > "$work/enum-lookup.c"
"${compiler[@]}" --no-libc -o "$work/enum-lookup" "$work/enum-lookup.c"
"${runner[@]}" "$work/enum-lookup"

printf '%s\n' \
    'typedef int unit_type;' \
    'int unit_type_size(void) { return sizeof(unit_type); }' \
    > "$work/typedef-root.c"
printf '%s\n' \
    'typedef char unit_type;' \
    'extern int unit_type_size(void);' \
    'int main(void) {' \
    '    size_t builtin_size = sizeof(unit_type);' \
    '    return unit_type_size() != 4 || builtin_size != 1;' \
    '}' > "$work/typedef-main.c"
"${compiler[@]}" --no-libc -o "$work/typedef-scope" \
    "$work/typedef-root.c" "$work/typedef-main.c"
"${runner[@]}" "$work/typedef-scope"

printf '%s\n' \
    'int main(void) {' \
    '    int size_t = 0;' \
    '    return size_t + (sizeof(int) != 4);' \
    '}' > "$work/builtin-shadow.c"
"${compiler[@]}" --no-libc -o "$work/builtin-shadow" \
    "$work/builtin-shadow.c"
"${runner[@]}" "$work/builtin-shadow"

printf '%s\n' \
    'struct offset_record { int member; };' \
    'int main(void) {' \
    '    int size_t = 0;' \
    '    return size_t + __builtin_offsetof(struct offset_record, member);' \
    '}' > "$work/offsetof-shadow.c"
"${compiler[@]}" --no-libc -o "$work/offsetof-shadow" \
    "$work/offsetof-shadow.c"
"${runner[@]}" "$work/offsetof-shadow"

printf 'typedef int hidden_type;\n' > "$work/typedef-leak-root.c"
printf 'hidden_type value;\n' > "$work/typedef-leak-main.c"
if "${compiler[@]}" --no-libc -o "$work/typedef-leak" \
    "$work/typedef-leak-root.c" "$work/typedef-leak-main.c" \
    > "$work/typedef-leak.error" 2>&1; then
    echo "typedef leaked across translation units" >&2
    exit 1
fi

printf '%s\n' \
    'static int unit_value = 17;' \
    'static int unit_helper(void) { return unit_value; }' \
    'int unit_result(void) { return unit_value + unit_helper(); }' \
    > "$work/static-root.c"
printf '%s\n' \
    'static int unit_value = 29;' \
    'static int unit_helper(void) { return unit_value; }' \
    'extern int unit_result(void);' \
    'int main(void) {' \
    '    return unit_value == 29 && unit_helper() == 29 &&' \
    '           unit_result() == 34 ? 0 : 1;' \
    '}' > "$work/static-main.c"
"${compiler[@]}" --no-libc -o "$work/static-scope" \
    "$work/static-root.c" "$work/static-main.c"
"${runner[@]}" "$work/static-scope"
"${compiler[@]}" --no-libc --dump-vir --stats \
    -o "$work/static-scope-dump" "$work/static-root.c" \
    "$work/static-main.c" 2> "$work/static-scope.vir"
static_helper_symbol()
{
    vir_extract_function "$1" "$work/static-scope.vir" | awk '
        / = call[.]i32 @/ {
            symbol = $4
            sub(/^@/, "", symbol)
            sub(/[(].*/, "", symbol)
            if (symbol != "unit_result") callees[symbol] = 1
        }
        END {
            for (symbol in callees) { count++; helper = symbol }
            if (count != 1) exit 1
            print helper
        }
    '
}

static_root_symbol()
{
    vir_extract_function "$1" "$work/static-scope.vir" | awk '
        / = globaladdr / { roots[$4] = 1 }
        END {
            for (symbol in roots) { count++; root = symbol }
            if (count != 1) exit 1
            print root
        }
    '
}

root_helper=$(static_helper_symbol unit_result)
main_helper=$(static_helper_symbol main)
test "$root_helper" != "$main_helper"
root_symbol=$(static_root_symbol unit_result)
main_symbol=$(static_root_symbol main)
test "$root_symbol" != "$main_symbol"
test "$(static_root_symbol "$root_helper")" = "$root_symbol"
test "$(static_root_symbol "$main_helper")" = "$main_symbol"
printf '%s\n' \
    'static int internal_object = 1;' \
    'static int internal_function(void) { return internal_object; }' \
    > "$work/internal-root.c"
printf 'int main(void) { return internal_object; }\n' \
    > "$work/internal-object-main.c"
if "${compiler[@]}" --no-libc -o "$work/internal-object-leak" \
    "$work/internal-root.c" "$work/internal-object-main.c" \
    > "$work/internal-object.error" 2>&1; then
    echo "static object leaked across translation units" >&2
    exit 1
fi
printf 'int main(void) { return internal_function(); }\n' \
    > "$work/internal-function-main.c"
if "${compiler[@]}" --no-libc -o "$work/internal-function-leak" \
    "$work/internal-root.c" "$work/internal-function-main.c" \
    > "$work/internal-function.error" 2>&1; then
    echo "static function leaked across translation units" >&2
    exit 1
fi

printf '%s\n' \
    'static int same_unit_object = 9;' \
    'extern int same_unit_object;' \
    'static int same_unit_function(void);' \
    'extern int same_unit_function(void);' \
    'static int same_unit_function(void) { return same_unit_object; }' \
    'int main(void) {' \
    '    return same_unit_function() == 9 ? 0 : 1;' \
    '}' > "$work/static-extern.c"
"${compiler[@]}" --no-libc -o "$work/static-extern" \
    "$work/static-extern.c"
"${runner[@]}" "$work/static-extern"

printf '%s\n' \
    'static int internal_designator(int value) { return value + 3; }' \
    'static int (*saved_designator)(int) = internal_designator;' \
    'int main(void) {' \
    '    int (*local_designator)(int) = internal_designator;' \
    '    return saved_designator(4) == 7 && local_designator(5) == 8 &&' \
    '           (*internal_designator)(6) == 9 &&' \
    '           (&internal_designator)(7) == 10 ? 0 : 1;' \
    '}' > "$work/static-designator.c"
"${compiler[@]}" --no-libc -o "$work/static-designator" \
    "$work/static-designator.c"
"${runner[@]}" "$work/static-designator"

printf '%s\n' \
    'struct external_record { int value; struct external_record *next; };' \
    'int external_record_value(struct external_record *value)' \
    '{ return value->value; }' > "$work/record-definition.c"
printf '%s\n' \
    'struct external_record { int value; struct external_record *next; };' \
    'extern int external_record_value(struct external_record *value);' \
    'int main(void) {' \
    '    struct external_record value = { 42 };' \
    '    return external_record_value(&value) == 42 ? 0 : 1;' \
    '}' > "$work/record-main.c"
"${compiler[@]}" --no-libc -o "$work/compatible-record" \
    "$work/record-definition.c" "$work/record-main.c"
"${runner[@]}" "$work/compatible-record"

printf '%s\n' \
    'struct callback_node {' \
    '    int (*compare)(struct callback_node *, struct callback_node *);' \
    '};' \
    'int callback_node_has_compare(struct callback_node *value)' \
    '{ return value->compare != 0; }' > "$work/callback-node-definition.c"
printf '%s\n' \
    'struct callback_node {' \
    '    int (*compare)(struct callback_node *, struct callback_node *);' \
    '};' \
    'extern int callback_node_has_compare(struct callback_node *value);' \
    'int main(void) {' \
    '    struct callback_node value = { 0 };' \
    '    return callback_node_has_compare(&value) ? 1 : 0;' \
    '}' > "$work/callback-node-main.c"
"${compiler[@]}" --no-libc -o "$work/compatible-callback-node" \
    "$work/callback-node-definition.c" "$work/callback-node-main.c"
"${runner[@]}" "$work/compatible-callback-node"

printf '%s\n' \
    'struct incomplete_record { int value; };' \
    'int incomplete_record_is_null(struct incomplete_record *value)' \
    '{ return value == 0; }' > "$work/incomplete-record-definition.c"
printf '%s\n' \
    'struct incomplete_record;' \
    'extern int incomplete_record_is_null(struct incomplete_record *value);' \
    'int main(void) {' \
    '    return incomplete_record_is_null(0) ? 0 : 1;' \
    '}' > "$work/incomplete-record-main.c"
"${compiler[@]}" --no-libc -o "$work/incomplete-record" \
    "$work/incomplete-record-definition.c" \
    "$work/incomplete-record-main.c"
"${runner[@]}" "$work/incomplete-record"

printf '%s\n' \
    'union external_union { int first; long second; };' \
    'int external_union_first(union external_union *value)' \
    '{ return value->first; }' > "$work/union-definition.c"
printf '%s\n' \
    'union external_union { long second; int first; };' \
    'extern int external_union_first(union external_union *value);' \
    'int main(void) {' \
    '    union external_union value;' \
    '    value.first = 37;' \
    '    return external_union_first(&value) == 37 ? 0 : 1;' \
    '}' > "$work/union-main.c"
"${compiler[@]}" --no-libc -o "$work/compatible-union" \
    "$work/union-definition.c" "$work/union-main.c"
"${runner[@]}" "$work/compatible-union"

printf '%s\n' \
    'struct external_object { int value; };' \
    'struct external_object external_object = { 31 };' \
    > "$work/object-definition.c"
printf '%s\n' \
    'struct external_object { int value; };' \
    'extern struct external_object external_object;' \
    'int main(void) { return external_object.value == 31 ? 0 : 1; }' \
    > "$work/object-main.c"
"${compiler[@]}" --no-libc -o "$work/compatible-object" \
    "$work/object-definition.c" "$work/object-main.c"
"${runner[@]}" "$work/compatible-object"

printf '%s\n' \
    'struct external_record { long value; };' \
    'extern int external_record_value(struct external_record *value);' \
    > "$work/incompatible-record.c"
if "${compiler[@]}" --no-libc -o "$work/incompatible-record" \
    "$work/record-definition.c" "$work/incompatible-record.c" \
    > "$work/incompatible-record.error" 2>&1; then
    echo "incompatible cross-unit record declaration was accepted" >&2
    exit 1
fi
grep -q "conflicting types for function declaration 'external_record_value'" \
    "$work/incompatible-record.error"

printf '%s\n' \
    'struct callback_record { int (*callback)(int); };' \
    'int callback_record_use(struct callback_record *value)' \
    '{ return value->callback(1); }' > "$work/callback-record-definition.c"
printf '%s\n' \
    'struct callback_record { int (*callback)(long); };' \
    'extern int callback_record_use(struct callback_record *value);' \
    > "$work/callback-record-incompatible.c"
if "${compiler[@]}" --no-libc -o "$work/callback-record-incompatible" \
    "$work/callback-record-definition.c" \
    "$work/callback-record-incompatible.c" \
    > "$work/callback-record-incompatible.error" 2>&1; then
    echo "cross-unit record with an incompatible callback member was accepted" >&2
    exit 1
fi
grep -q "conflicting types for function declaration 'callback_record_use'" \
    "$work/callback-record-incompatible.error"

printf '%s\n' \
    'struct array_pointer_record { int (*row)[2]; };' \
    'int array_pointer_record_use(struct array_pointer_record *value)' \
    '{ return value->row != 0; }' > "$work/array-pointer-record-definition.c"
printf '%s\n' \
    'struct array_pointer_record { int (*row)[3]; };' \
    'extern int array_pointer_record_use(struct array_pointer_record *value);' \
    > "$work/array-pointer-record-incompatible.c"
if "${compiler[@]}" --no-libc -o "$work/array-pointer-record-incompatible" \
    "$work/array-pointer-record-definition.c" \
    "$work/array-pointer-record-incompatible.c" \
    > "$work/array-pointer-record-incompatible.error" 2>&1; then
    echo "cross-unit record with an incompatible pointer-to-array member was accepted" >&2
    exit 1
fi
grep -q "conflicting types for function declaration 'array_pointer_record_use'" \
    "$work/array-pointer-record-incompatible.error"

printf '%s\n' \
    'struct other_record { int value; struct other_record *next; };' \
    'extern int external_record_value(struct other_record *value);' \
    > "$work/incompatible-tag.c"
if "${compiler[@]}" --no-libc -o "$work/incompatible-tag" \
    "$work/record-definition.c" "$work/incompatible-tag.c" \
    > "$work/incompatible-tag.error" 2>&1; then
    echo "cross-unit record with a different tag was accepted" >&2
    exit 1
fi
grep -q "conflicting types for function declaration 'external_record_value'" \
    "$work/incompatible-tag.error"

printf '%s\n' \
    'struct external_object { long value; };' \
    'extern struct external_object external_object;' \
    > "$work/incompatible-object.c"
if "${compiler[@]}" --no-libc -o "$work/incompatible-object" \
    "$work/object-definition.c" "$work/incompatible-object.c" \
    > "$work/incompatible-object.error" 2>&1; then
    echo "incompatible cross-unit object declaration was accepted" >&2
    exit 1
fi
grep -q "conflicting types for global declaration 'external_object'" \
    "$work/incompatible-object.error"

printf 'int duplicate_external_object = 1;\n' > "$work/duplicate-object-a.c"
printf 'int duplicate_external_object = 2;\n' > "$work/duplicate-object-b.c"
if "${compiler[@]}" --no-libc -o "$work/duplicate-object" \
    "$work/duplicate-object-a.c" "$work/duplicate-object-b.c" \
    > "$work/duplicate-object.error" 2>&1; then
    echo "duplicate external object definitions were accepted" >&2
    exit 1
fi
grep -q "redefinition of global variable 'duplicate_external_object'" \
    "$work/duplicate-object.error"

printf 'int duplicate_external_function(void) { return 1; }\n' \
    > "$work/duplicate-function-a.c"
printf 'int duplicate_external_function(void) { return 2; }\n' \
    > "$work/duplicate-function-b.c"
if "${compiler[@]}" --no-libc -o "$work/duplicate-function" \
    "$work/duplicate-function-a.c" "$work/duplicate-function-b.c" \
    > "$work/duplicate-function.error" 2>&1; then
    echo "duplicate external function definitions were accepted" >&2
    exit 1
fi
grep -q "redefinition of function 'duplicate_external_function'" \
    "$work/duplicate-function.error"

for conflict in enum-before-object object-before-enum enum-before-function \
    function-before-enum typedef-before-enum enum-before-typedef \
    duplicate-enumerator; do
    case $conflict in
        enum-before-object)
            printf 'enum { same_name = 1 }; int same_name;\n' > "$work/enum-conflict.c"
            ;;
        object-before-enum)
            printf 'int same_name; enum { same_name = 1 };\n' > "$work/enum-conflict.c"
            ;;
        enum-before-function)
            printf 'enum { same_name = 1 }; int same_name(void);\n' > "$work/enum-conflict.c"
            ;;
        function-before-enum)
            printf 'int same_name(void); enum { same_name = 1 };\n' > "$work/enum-conflict.c"
            ;;
        typedef-before-enum)
            printf 'typedef int same_name; enum { same_name = 1 };\n' > "$work/enum-conflict.c"
            ;;
        enum-before-typedef)
            printf 'enum { same_name = 1 }; typedef int same_name;\n' > "$work/enum-conflict.c"
            ;;
        duplicate-enumerator)
            printf 'enum { same_name = 1, same_name = 2 };\n' > "$work/enum-conflict.c"
            ;;
    esac
    if "${compiler[@]}" --no-libc -o "$work/enum-conflict" \
        "$work/enum-conflict.c" > "$work/enum-conflict.error" 2>&1; then
        echo "same-translation-unit ordinary-name conflict accepted: $conflict" >&2
        exit 1
    fi
done

if "${compiler[@]}" --dot "${inputs[@]}" > "$work/dot.error" 2> /dev/null; then
    echo "--dot unexpectedly accepted multiple inputs without -o" >&2
    exit 1
fi
grep -q -- '--dot requires -o with multiple input files' "$work/dot.error"

flags=(--stats)
repeat_flags=()

"${compiler[@]}" "${flags[@]}" -o "$work/program" \
    "${inputs[@]}" \
    2> "$work/stats"
"${compiler[@]}" "${repeat_flags[@]}" -o "$work/program-again" \
    "${inputs[@]}" \
    2> "$work/stats-again"
cmp "$work/program" "$work/program-again"
grep -Eq 'stats phase=parse translation_units=2' "$work/stats"
grep -Eq 'stats phase=parse tu_index_lookups=[1-9][0-9]* tu_index_registrations=[1-9][0-9]*' \
    "$work/stats"
"${runner[@]}" "$work/program"

printf 'int shared_root;\n' > "$work/direct-root.c"
printf '%s\n' \
    'extern int shared_root;' \
    'int main(void) { shared_root = 41; return shared_root == 41 ? 0 : 1; }' \
    > "$work/direct-main.c"
"${compiler[@]}" --stats --dump-vir \
    -o "$work/direct-program" "$work/direct-root.c" "$work/direct-main.c" \
    2> "$work/direct-stats"
"${runner[@]}" "$work/direct-program"
