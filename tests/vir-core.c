#include "vir.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

static void test_overwritten_stores(void)
{
    for (int barrier = 0; barrier < 7; barrier++) {
        vir_function_t function;
        char *error = NULL;
        vir_function_init(&function, 64);
        assert(vir_function_set_pointer_bits(&function, 64));
        vir_block_t *block = vir_block_create(&function);
        vir_value_t *address = vir_stack_addr(&function, block, 0, 4, 4);
        vir_value_t *one = vir_const_i32(&function, block, 1);
        vir_value_t *two = vir_const_i32(&function, block, 2);
        vir_effect_t *first = vir_store(&function, block, address, one);
        vir_value_t *result = two;
        if (barrier == 1)
            assert(vir_volatile_load(&function, block, address, VIR_TYPE_I32));
        if (barrier == 2)
            result = vir_load(&function, block, address, VIR_TYPE_I32);
        if (barrier == 3)
            assert(
                vir_call(&function, block, "observe", NULL, 0, VIR_TYPE_VOID));
        if (barrier == 4)
            assert(vir_volatile_store(&function, block, address, one));
        if (barrier == 5)
            two = vir_const_int(&function, block, VIR_TYPE_I8, 2);
        if (barrier == 6)
            assert(vir_load(&function, block, address, VIR_TYPE_I32));
        assert(first && vir_store(&function, block, address, two));
        assert(vir_block_set_return(&function, block, result));
        assert(vir_verify(&function, &error));
        assert(vir_dce(&function, VIR_OPT_O0) == 0);
        assert(block->effects == first);
        assert(vir_dce(&function, VIR_OPT_O1) >= 0);
        assert((block->effects == first) == (barrier != 0 && barrier != 6));
        assert(vir_verify(&function, &error));
        vir_function_release(&function);
    }
}

static void test_va_start_signature_metadata(void)
{
    vir_function_t function;
    char *error;
    vir_function_init(&function, 64);
    vir_block_t *block = vir_block_create(&function);
    vir_value_t *argument = vir_const_i32(&function, block, 7);
    vir_value_t *args[] = {argument};
    vir_call_abi_type_t param = {VIR_TYPE_I32, false, false};
    unsigned int slot = 0;
    vir_call_signature_t signature = {
        {VIR_TYPE_I32, false, false}, &param, 1, false, 1, &slot, 0};
    vir_effect_t *call =
        vir_call(&function, block, "variadic", args, 1, VIR_TYPE_I32);
    assert(call && !vir_call_set_signature(&function, call, &signature));
    signature.is_variadic = true;
    assert(vir_call_set_signature(&function, call, &signature));
    assert(call->signature->va_start_slot == UINT_MAX &&
           !call->signature->param_slots);
    assert(vir_block_set_return(&function, block, call->result));
    assert(vir_verify(&function, &error));
    vir_function_release(&function);
}

static void test_sccp_effect_first_param(void)
{
    vir_function_t function;
    char *error;
    vir_function_init(&function, 64);
    assert(vir_function_set_pointer_bits(&function, 32));
    vir_block_t *entry = vir_block_create(&function);
    vir_block_t *merge = vir_block_create(&function);
    vir_value_t *address = vir_stack_addr(&function, entry, 0, 4, 4);
    vir_value_t *constant = vir_const_i32(&function, entry, 7);
    vir_value_t *parameter =
        vir_block_add_param(&function, merge, VIR_TYPE_I32);
    assert(vir_edge_create(&function, entry, merge, &constant, 1));
    vir_effect_t *store = vir_store(&function, merge, address, parameter);
    assert(store && store->order == 0);
    assert(vir_block_set_return(&function, merge, parameter));
    assert(vir_verify(&function, &error));
    assert(vir_sccp(&function, VIR_OPT_O1) > 0);
    assert(vir_verify(&function, &error));
    assert(store->stored_value->opcode == VIR_OP_CONST);
    assert(store->stored_value->order < store->order);
    vir_function_release(&function);
}

static void test_sccp_width_case(vir_type_t type,
                                 vir_opcode_t opcode,
                                 unsigned long long left_bits,
                                 unsigned long long right_bits,
                                 unsigned long long expected)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_value_t *left;
    vir_value_t *param;
    vir_value_t *right;
    vir_value_t *value;
    vir_type_t right_type = (opcode == VIR_OP_SHL || opcode == VIR_OP_ASHR ||
                             opcode == VIR_OP_LSHR) &&
                                    type == VIR_TYPE_I64
                                ? VIR_TYPE_I32
                                : type;
    vir_type_t result_type =
        opcode == VIR_OP_EQ || opcode == VIR_OP_SLT || opcode == VIR_OP_ULT
            ? VIR_TYPE_I1
            : type;
    char *error;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    left = vir_const_int(&func, entry, type, left_bits);
    param = vir_block_add_param(&func, merge, type);
    right = vir_const_int(&func, merge, right_type, right_bits);
    value = vir_binary(&func, merge, opcode, result_type, param, right);
    assert(entry && merge && left && param && right && value &&
           vir_edge_create(&func, entry, merge, &left, 1) &&
           vir_block_set_return(&func, merge, value) &&
           vir_verify(&func, &error));
    assert(vir_sccp(&func, VIR_OPT_O1) > 0 &&
           merge->return_value->opcode == VIR_OP_CONST &&
           merge->return_value->type == result_type &&
           merge->return_value->constant == expected &&
           vir_verify(&func, &error));
    vir_function_release(&func);
}

static void test_sccp_unary_width_case(vir_type_t type,
                                       vir_opcode_t opcode,
                                       unsigned long long operand_bits,
                                       unsigned long long expected)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_value_t *operand;
    vir_value_t *param;
    vir_value_t *value;
    char *error;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    operand = vir_const_int(&func, entry, type, operand_bits);
    param = vir_block_add_param(&func, merge, type);
    value = opcode == VIR_OP_NEG ? vir_neg(&func, merge, param)
                                 : vir_bitnot(&func, merge, param);
    assert(entry && merge && operand && param && value &&
           vir_edge_create(&func, entry, merge, &operand, 1) &&
           vir_block_set_return(&func, merge, value) &&
           vir_verify(&func, &error));
    assert(vir_sccp(&func, VIR_OPT_O1) > 0 &&
           merge->return_value->opcode == VIR_OP_CONST &&
           merge->return_value->type == type &&
           merge->return_value->constant == expected &&
           vir_verify(&func, &error));
    vir_function_release(&func);
}

static void test_sccp_cast_width_case(vir_type_t input_type,
                                      vir_opcode_t opcode,
                                      vir_type_t result_type,
                                      unsigned long long operand_bits,
                                      unsigned long long expected)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_value_t *operand;
    vir_value_t *param;
    vir_value_t *value;
    char *error;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    operand = vir_const_int(&func, entry, input_type, operand_bits);
    param = vir_block_add_param(&func, merge, input_type);
    value = opcode == VIR_OP_ZEXT && input_type == VIR_TYPE_I1
                ? vir_zext_i1(&func, merge, param)
            : opcode == VIR_OP_ZEXT ? vir_zext(&func, merge, param, result_type)
            : opcode == VIR_OP_SEXT
                ? vir_sext(&func, merge, param, result_type)
                : vir_trunc(&func, merge, param, result_type);
    assert(entry && merge && operand && param && value &&
           vir_edge_create(&func, entry, merge, &operand, 1) &&
           vir_block_set_return(&func, merge, value) &&
           vir_verify(&func, &error));
    assert(vir_sccp(&func, VIR_OPT_O1) > 0 &&
           merge->return_value->opcode == VIR_OP_CONST &&
           merge->return_value->type == result_type &&
           merge->return_value->constant == expected &&
           vir_verify(&func, &error));
    vir_function_release(&func);
}

static void test_sccp_width_folds(void)
{
    static const struct {
        vir_type_t type;
        vir_opcode_t opcode;
        unsigned long long left;
        unsigned long long right;
        unsigned long long expected;
    } cases[] = {
        {VIR_TYPE_I32, VIR_OP_ADD, 0x7fffffffULL, 1, 0x80000000ULL},
        {VIR_TYPE_I32, VIR_OP_SUB, 0, 1, 0xffffffffULL},
        {VIR_TYPE_I32, VIR_OP_MUL, 0x10000ULL, 0x10000ULL, 0},
        {VIR_TYPE_I32, VIR_OP_SDIV, 0xfffffff9ULL, 3, 0xfffffffeULL},
        {VIR_TYPE_I32, VIR_OP_SREM, 0xfffffff9ULL, 3, 0xffffffffULL},
        {VIR_TYPE_I32, VIR_OP_SDIV, 7, 0xfffffffdULL, 0xfffffffeULL},
        {VIR_TYPE_I32, VIR_OP_SREM, 7, 0xfffffffdULL, 1},
        {VIR_TYPE_I32, VIR_OP_SDIV, 0xfffffff9ULL, 0xfffffffdULL, 2},
        {VIR_TYPE_I32, VIR_OP_SREM, 0xfffffff9ULL, 0xfffffffdULL,
         0xffffffffULL},
        {VIR_TYPE_I32, VIR_OP_UDIV, 0xffffffffULL, 3, 0x55555555ULL},
        {VIR_TYPE_I32, VIR_OP_UREM, 0xffffffffULL, 3, 0},
        {VIR_TYPE_I32, VIR_OP_SHL, 1, 31, 0x80000000ULL},
        {VIR_TYPE_I32, VIR_OP_ASHR, 0x80000000ULL, 31, 0xffffffffULL},
        {VIR_TYPE_I32, VIR_OP_LSHR, 0x80000000ULL, 31, 1},
        {VIR_TYPE_I32, VIR_OP_SLT, 0x80000000ULL, 1, 1},
        {VIR_TYPE_I32, VIR_OP_ULT, 0x80000000ULL, 1, 0},
        {VIR_TYPE_I32, VIR_OP_BITAND, 0x8000000fULL, 0xffffffffULL,
         0x8000000fULL},
        {VIR_TYPE_I32, VIR_OP_BITOR, 0x80000000ULL, 0x0fULL, 0x8000000fULL},
        {VIR_TYPE_I32, VIR_OP_BITXOR, 0x80000000ULL, 0xffffffffULL,
         0x7fffffffULL},
        {VIR_TYPE_I32, VIR_OP_EQ, 0x80000000ULL, 0x80000000ULL, 1},
        {VIR_TYPE_I32, VIR_OP_EQ, 0x80000000ULL, 1, 0},
        {VIR_TYPE_I64, VIR_OP_ADD, 0xffffffffffffffffULL, 1, 0},
        {VIR_TYPE_I64, VIR_OP_SUB, 0, 1, 0xffffffffffffffffULL},
        {VIR_TYPE_I64, VIR_OP_MUL, 0x100000001ULL, 3, 0x300000003ULL},
        {VIR_TYPE_I64, VIR_OP_SDIV, 0xfffffffffffffff9ULL, 3,
         0xfffffffffffffffeULL},
        {VIR_TYPE_I64, VIR_OP_SREM, 0xfffffffffffffff9ULL, 3,
         0xffffffffffffffffULL},
        {VIR_TYPE_I64, VIR_OP_SDIV, 7, 0xfffffffffffffffdULL,
         0xfffffffffffffffeULL},
        {VIR_TYPE_I64, VIR_OP_SREM, 7, 0xfffffffffffffffdULL, 1},
        {VIR_TYPE_I64, VIR_OP_SDIV, 0xfffffffffffffff9ULL,
         0xfffffffffffffffdULL, 2},
        {VIR_TYPE_I64, VIR_OP_SREM, 0xfffffffffffffff9ULL,
         0xfffffffffffffffdULL, 0xffffffffffffffffULL},
        {VIR_TYPE_I64, VIR_OP_UDIV, 0xffffffffffffffffULL, 3,
         0x5555555555555555ULL},
        {VIR_TYPE_I64, VIR_OP_UREM, 0xffffffffffffffffULL, 3, 0},
        {VIR_TYPE_I64, VIR_OP_SHL, 1, 63, 0x8000000000000000ULL},
        {VIR_TYPE_I64, VIR_OP_ASHR, 0x8000000000000000ULL, 63,
         0xffffffffffffffffULL},
        {VIR_TYPE_I64, VIR_OP_LSHR, 0x8000000000000000ULL, 63, 1},
        {VIR_TYPE_I64, VIR_OP_SLT, 0x8000000000000000ULL, 1, 1},
        {VIR_TYPE_I64, VIR_OP_ULT, 0x8000000000000000ULL, 1, 0},
        {VIR_TYPE_I64, VIR_OP_BITAND, 0x800000000000000fULL, ~0ULL,
         0x800000000000000fULL},
        {VIR_TYPE_I64, VIR_OP_BITOR, 0x8000000000000000ULL, 0x0fULL,
         0x800000000000000fULL},
        {VIR_TYPE_I64, VIR_OP_BITXOR, 0x8000000000000000ULL, ~0ULL,
         0x7fffffffffffffffULL},
        {VIR_TYPE_I64, VIR_OP_EQ, 0x8000000000000000ULL, 0x8000000000000000ULL,
         1},
        {VIR_TYPE_I64, VIR_OP_EQ, 0x8000000000000000ULL, 1, 0},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        test_sccp_width_case(cases[i].type, cases[i].opcode, cases[i].left,
                             cases[i].right, cases[i].expected);
    test_sccp_unary_width_case(VIR_TYPE_I32, VIR_OP_NEG, 0x80000000ULL,
                               0x80000000ULL);
    test_sccp_unary_width_case(VIR_TYPE_I32, VIR_OP_NEG, 0x80000001ULL,
                               0x7fffffffULL);
    test_sccp_unary_width_case(VIR_TYPE_I32, VIR_OP_NEG, 1, 0xffffffffULL);
    test_sccp_unary_width_case(VIR_TYPE_I32, VIR_OP_BITNOT, 0x80000000ULL,
                               0x7fffffffULL);
    test_sccp_unary_width_case(VIR_TYPE_I64, VIR_OP_NEG, 0x8000000000000000ULL,
                               0x8000000000000000ULL);
    test_sccp_unary_width_case(VIR_TYPE_I64, VIR_OP_NEG, 0x8000000000000001ULL,
                               0x7fffffffffffffffULL);
    test_sccp_unary_width_case(VIR_TYPE_I64, VIR_OP_NEG, 1, ~0ULL);
    test_sccp_unary_width_case(VIR_TYPE_I64, VIR_OP_BITNOT,
                               0x8000000000000000ULL, 0x7fffffffffffffffULL);
    test_sccp_cast_width_case(VIR_TYPE_I8, VIR_OP_ZEXT, VIR_TYPE_I32, 0xffULL,
                              0xffULL);
    test_sccp_cast_width_case(VIR_TYPE_I1, VIR_OP_ZEXT, VIR_TYPE_I32, 1, 1);
    test_sccp_cast_width_case(VIR_TYPE_I1, VIR_OP_ZEXT, VIR_TYPE_I32, 0, 0);
    test_sccp_cast_width_case(VIR_TYPE_I8, VIR_OP_SEXT, VIR_TYPE_I32, 0xffULL,
                              0xffffffffULL);
    test_sccp_cast_width_case(VIR_TYPE_I16, VIR_OP_ZEXT, VIR_TYPE_I32,
                              0xffffULL, 0xffffULL);
    test_sccp_cast_width_case(VIR_TYPE_I16, VIR_OP_SEXT, VIR_TYPE_I32,
                              0xffffULL, 0xffffffffULL);
    test_sccp_cast_width_case(VIR_TYPE_I32, VIR_OP_ZEXT, VIR_TYPE_I64,
                              0x80000000ULL, 0x80000000ULL);
    test_sccp_cast_width_case(VIR_TYPE_I32, VIR_OP_SEXT, VIR_TYPE_I64,
                              0x80000000ULL, 0xffffffff80000000ULL);
    test_sccp_cast_width_case(VIR_TYPE_I64, VIR_OP_TRUNC, VIR_TYPE_I32,
                              0x1234567887654321ULL, 0x87654321ULL);
}

static void test_sccp_keeps_undefined_signed_division(vir_type_t type,
                                                      unsigned long long min)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_value_t *left;
    vir_value_t *param;
    vir_value_t *minus_one;
    vir_value_t *value;
    char *error;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    left = vir_const_int(&func, entry, type, min);
    param = vir_block_add_param(&func, merge, type);
    minus_one = vir_const_int(&func, merge, type, ~0ULL);
    value = param && minus_one
                ? vir_binary(&func, merge, VIR_OP_SDIV, type, param, minus_one)
                : NULL;
    assert(entry && merge && left && param && minus_one && value &&
           vir_edge_create(&func, entry, merge, &left, 1) &&
           vir_block_set_return(&func, merge, value) &&
           vir_verify(&func, &error));
    assert(vir_sccp(&func, VIR_OPT_O1) > 0 &&
           merge->return_value->opcode == VIR_OP_SDIV &&
           vir_verify(&func, &error));
    vir_function_release(&func);
}

static void test_sccp_undefined_signed_division(void)
{
    test_sccp_keeps_undefined_signed_division(VIR_TYPE_I32, 0x80000000ULL);
    test_sccp_keeps_undefined_signed_division(VIR_TYPE_I64,
                                              0x8000000000000000ULL);
}

static void test_signed_division_builder_i64(void)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_value_t *positive;
    vir_value_t *negative;
    vir_value_t *negative_seven;
    vir_value_t *negative_three;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    positive = vir_const_int(&func, entry, VIR_TYPE_I64, 7);
    negative = vir_const_int(&func, entry, VIR_TYPE_I64, ~2ULL);
    negative_seven = vir_const_int(&func, entry, VIR_TYPE_I64, ~6ULL);
    negative_three =
        vir_div(&func, entry, negative_seven, negative, false, false);
    assert(positive && negative && negative_seven && negative_three &&
           negative_three->constant == 2 &&
           vir_div(&func, entry, negative_seven, negative, false, true)
                   ->constant == ~0ULL &&
           vir_div(&func, entry, positive, negative, false, false)->constant ==
               ~1ULL &&
           vir_div(&func, entry, positive, negative, false, true)->constant ==
               1);
    vir_function_release(&func);
}

static void test_signed_division_overflow_builder(vir_type_t type,
                                                  unsigned long long min)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_value_t *minimum;
    vir_value_t *minus_one;
    vir_value_t *quotient;
    vir_value_t *remainder;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    minimum = vir_const_int(&func, entry, type, min);
    minus_one = vir_const_int(&func, entry, type, ~0ULL);
    quotient = vir_div(&func, entry, minimum, minus_one, false, false);
    remainder = vir_div(&func, entry, minimum, minus_one, false, true);
    assert(entry && minimum && minus_one && quotient && remainder &&
           quotient->opcode == VIR_OP_SDIV &&
           remainder->opcode == VIR_OP_SREM && quotient->type == type &&
           remainder->type == type);
    vir_function_release(&func);
}

int main(void)
{
    test_overwritten_stores();
    test_va_start_signature_metadata();
    test_sccp_effect_first_param();
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_block_t *otherwise;
    vir_block_t *dead;
    vir_value_t *one;
    vir_value_t *two;
    vir_value_t *entry_param;
    vir_value_t *sum;
    vir_value_t *product;
    vir_value_t *param;
    vir_value_t *stack_duplicate;
    vir_value_t *global_duplicate;
    vir_edge_t *edge;
    vir_effect_t *effect;
    vir_effect_t *call;
    char *error;
    FILE *out;
    char text[256];
    int text_len;

    test_sccp_width_folds();
    test_sccp_undefined_signed_division();
    test_signed_division_builder_i64();
    test_signed_division_overflow_builder(VIR_TYPE_I32, 0x80000000ULL);
    test_signed_division_overflow_builder(VIR_TYPE_I64, 0x8000000000000000ULL);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    func.arena.fail_after = 0;
    assert(!vir_block_create(&func));
    assert(func.blocks == entry && func.last_block == entry);
    func.arena.fail_after = -1;
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    product = vir_binary(&func, entry, VIR_OP_MUL, VIR_TYPE_I32, sum, two);
    func.arena.fail_after = 0;
    assert(!vir_const_i32(&func, entry, 3));
    assert(entry->tail == product && !product->next);
    func.arena.fail_after = -1;
    func.arena.fail_after = 0;
    assert(!vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two));
    assert(entry->tail == product && !product->next);
    func.arena.fail_after = 1;
    assert(!vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two));
    assert(entry->tail == product && !product->next);
    func.arena.fail_after = -1;
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    func.arena.fail_after = 0;
    assert(!vir_block_add_param(&func, merge, VIR_TYPE_I32));
    assert(merge->params == param && merge->last_param == param &&
           merge->param_count == 1 && !param->param_next);
    func.arena.fail_after = -1;
    assert(!vir_edge_create(&func, entry, merge, NULL, 1));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = 0;
    assert(!vir_edge_create(&func, entry, merge, &product, 1));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = 1;
    assert(!vir_edge_create(&func, entry, merge, &product, 1));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = 2;
    assert(!vir_edge_create(&func, entry, merge, &product, 1));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = -1;
    edge = vir_edge_create(&func, entry, merge, &product, 1);
    assert(edge && edge->args[0] == product);
    assert(!vir_verify(&func, &error));
    func.arena.fail_after = 0;
    assert(!vir_block_set_return(&func, merge, param));
    assert(merge->terminator == VIR_TERM_NONE && !merge->return_value);
    func.arena.fail_after = -1;
    assert(vir_block_set_return(&func, merge, param));
    assert(!vir_effect_create(&func, merge, VIR_EFFECT_STORE));
    assert(entry->id == 0);
    assert(one->id == 0 && two->id == 1 && sum->id == 2 && product->id == 3);
    assert(vir_verify(&func, &error));
    vir_use_t *saved_next = one->uses->next;
    one->uses->next = one->uses;
    assert(!vir_verify(&func, &error));
    one->uses->next = saved_next;
    assert(vir_verify(&func, &error));
    vir_stats_t stats;
    vir_collect_stats(&func, &stats);
    assert(stats.blocks == 2 && stats.values == 5 && stats.params == 1);
    assert(stats.edges == 1 && stats.effects == 0 && stats.uses == 6);
    assert(stats.arena_capacity >= stats.arena_peak_capacity);
    vir_function_t foreign;
    vir_block_t *foreign_block;
    vir_block_t *foreign_target;
    vir_value_t *foreign_value;
    vir_edge_t *foreign_edge;
    vir_function_init(&foreign, 64);
    foreign_block = vir_block_create(&foreign);
    foreign_target = vir_block_create(&foreign);
    foreign_value = vir_const_i32(&foreign, foreign_block, 9);
    foreign_edge =
        vir_edge_create(&foreign, foreign_block, foreign_target, NULL, 0);
    assert(foreign_edge);
    assert(!vir_const_i32(&func, foreign_block, 9));
    assert(!vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one,
                       foreign_value));
    assert(!vir_add(&func, entry, one, foreign_value));
    assert(!vir_eq(&func, entry, one, foreign_value));
    vir_ssa_t *foreign_ssa = vir_ssa_create(&func);
    assert(foreign_ssa && !vir_ssa_jump(foreign_ssa, entry, foreign_target));
    vir_ssa_release(foreign_ssa);
    product->op0 = foreign_value;
    assert(!vir_verify(&func, &error));
    product->op0 = sum;
    merge->incoming = foreign_edge;
    assert(!vir_verify(&func, &error));
    merge->incoming = edge;
    vir_function_release(&foreign);
    merge->incoming = NULL;
    assert(!vir_verify(&func, &error));
    merge->incoming = edge;
    sum->position = 99;
    assert(!vir_verify(&func, &error));
    assert(!strcmp(error, "invalid value/use structure"));
    sum->position = 2;
    vir_replace_all_uses(&func, sum, one);
    assert(!sum->uses && product->op0 == one);
    assert(vir_verify(&func, &error));
    vir_replace_all_uses(&func, product, one);
    assert(edge->args[0] == one && param->is_block_param);
    assert(vir_verify(&func, &error));
    vir_replace_all_uses(&func, param, two);
    assert(merge->return_value == two);
    assert(vir_verify(&func, &error));

    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(!strcmp(text,
                   "b0:\n"
                   "  %d0 = const.i32 1\n"
                   "  %d1 = const.i32 2\n"
                   "  %d2 = add.i32 %d0, %d1\n"
                   "  %d3 = mul.i32 %d0, %d1\n"
                   "  jump b1(%d0)\n"
                   "b1(%d4:i32):\n"
                   "  return %d1\n"));
    fclose(out);
    vir_function_release(&func);

    /* Redirecting a pointer-carrying edge must retain its typed argument and
     * reverse use just like the scalar transactional path.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    vir_block_t *pointer_left = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    one = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    param = vir_block_add_param(&func, merge, VIR_TYPE_PTR);
    product = vir_block_add_param(&func, otherwise, VIR_TYPE_PTR);
    assert(one && param && product &&
           vir_edge_create(&func, entry, pointer_left, NULL, 0) &&
           vir_edge_create(&func, pointer_left, merge, &one, 1) &&
           vir_block_set_return(&func, merge, param) &&
           vir_block_set_return(&func, otherwise, product));
    assert(vir_edge_redirect(&func, pointer_left->outgoing, otherwise));
    assert(pointer_left->outgoing->to == otherwise &&
           pointer_left->outgoing->args[0] == one && one->uses &&
           one->uses->edge == pointer_left->outgoing && !one->uses->next &&
           vir_verify(&func, &error));
    vir_block_t *pointer_split = vir_edge_split(&func, pointer_left->outgoing);
    assert(pointer_split && pointer_split->param_count == 1 &&
           pointer_split->params->type == VIR_TYPE_PTR &&
           pointer_left->outgoing->to == pointer_split &&
           pointer_left->outgoing->args[0] == one &&
           pointer_split->outgoing->to == otherwise &&
           pointer_split->outgoing->args[0] == pointer_split->params &&
           one->uses && one->uses->edge == pointer_left->outgoing &&
           pointer_split->params->uses &&
           pointer_split->params->uses->edge == pointer_split->outgoing &&
           vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    edge = vir_edge_create(&func, entry, merge, &one, 1);
    assert(edge && vir_edge_delete(&func, edge));
    assert(entry->terminator == VIR_TERM_NONE && !entry->outgoing &&
           !merge->incoming && !one->uses);
    assert(vir_edge_create(&func, entry, merge, &one, 1));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(!vir_function_set_pointer_bits(&func, 16));
    entry = vir_block_create(&func);
    assert(!vir_function_set_pointer_bits(&func, 32));
    assert(!vir_const_ptr(&func, entry, 0));
    assert(!vir_block_add_param(&func, entry, VIR_TYPE_PTR));
    assert(vir_block_set_return(&func, entry, vir_const_i32(&func, entry, 0)));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_const_ptr(&func, entry, 0x123456789ULL);
    one = vir_const_i32(&func, entry, 0x76543210);
    product = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    sum = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    assert(param && param->constant == 0x23456789 &&
           vir_ptrtoint(&func, entry, param, VIR_TYPE_I32)->constant ==
               0x23456789 &&
           vir_inttoptr(&func, entry, one)->constant == 0x76543210);
    assert(vir_ptradd(&func, entry, param, one)->constant == 0x99999999 &&
           vir_ptradd(&func, entry, param, vir_const_i32(&func, entry, 0)) ==
               param);
    assert(vir_ptradd(&func, entry, vir_const_ptr(&func, entry, 0xffffffff),
                      vir_const_i32(&func, entry, 1))
               ->constant == 0);
    two = vir_ptrtoint(&func, entry, product, VIR_TYPE_I32);
    assert(two && two->opcode == VIR_OP_PTRTOINT && two->op0 == product);
    assert(vir_inttoptr(&func, entry, sum)->opcode == VIR_OP_INTTOPTR);
    assert(vir_ptradd(&func, entry, product, sum)->opcode == VIR_OP_PTRADD);
    assert(!vir_ptrtoint(&func, entry, product, VIR_TYPE_I64) &&
           !vir_inttoptr(&func, entry,
                         vir_const_int(&func, entry, VIR_TYPE_I64, 1)) &&
           !vir_zext(&func, entry, product, VIR_TYPE_I64) &&
           !vir_ptradd(&func, entry, product,
                       vir_const_int(&func, entry, VIR_TYPE_I64, 1)));
    assert(vir_block_set_return(&func, entry, param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 64));
    entry = vir_block_create(&func);
    sum = vir_const_int(&func, entry, VIR_TYPE_I64, 0x123456789abcdef0ULL);
    param = vir_inttoptr(&func, entry, sum);
    assert(param && param->constant == 0x123456789abcdef0ULL &&
           vir_ptrtoint(&func, entry, param, VIR_TYPE_I64)->constant ==
               0x123456789abcdef0ULL);
    assert(vir_ptradd(&func, entry,
                      vir_const_ptr(&func, entry, 0xffffffffffffffffULL),
                      vir_const_int(&func, entry, VIR_TYPE_I64, 1))
               ->constant == 0);
    assert(vir_block_set_return(&func, entry, param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    sum = vir_ptradd(&func, entry, param, one);
    product = vir_ptradd(&func, entry, param, one);
    assert(sum && product && vir_block_set_return(&func, entry, product));
    assert(vir_local_cse(&func, VIR_OPT_O1) == 1 &&
           entry->return_value == sum && !product->uses);
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(!strcmp(text,
                   "b0(%d0:ptr, %d1:i32):\n"
                   "  %d2 = ptradd %d0, %d1\n"
                   "  %d3 = ptradd %d0, %d1\n"
                   "  return %d2\n"));
    fclose(out);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_add(&func, entry, param, one);
    product = vir_add(&func, entry, param, two);
    assert(sum && product && vir_block_set_return(&func, entry, product));
    int dce_passes;
    int dce_values_scanned;
    int dce_values_removed;
    int dce_temporary_bytes;
    int dce_temporary_peak_bytes;
    assert(
        vir_dce_with_stats(&func, VIR_OPT_O0, &dce_passes, &dce_values_scanned,
                           &dce_values_removed, &dce_temporary_bytes,
                           &dce_temporary_peak_bytes) == 0 &&
        dce_passes == 0 && dce_values_scanned == 0 && dce_values_removed == 0 &&
        dce_temporary_bytes == 0 && dce_temporary_peak_bytes == 0);
    assert(
        vir_dce_with_stats(&func, VIR_OPT_O1, &dce_passes, &dce_values_scanned,
                           &dce_values_removed, &dce_temporary_bytes,
                           &dce_temporary_peak_bytes) == 2 &&
        dce_passes == 1 && dce_values_scanned == 4 && dce_values_removed == 2 &&
        entry->head == two && entry->tail == product && two->position == 0 &&
        product->position == 1 && !sum->uses && !one->uses &&
        dce_temporary_bytes > 0 &&
        dce_temporary_bytes == dce_temporary_peak_bytes);
    assert(vir_dce(&func, VIR_OPT_O2) == 0);
    assert(vir_dce_with_stats(&func, (vir_opt_level_t) 99, &dce_passes,
                              &dce_values_scanned, &dce_values_removed,
                              &dce_temporary_bytes,
                              &dce_temporary_peak_bytes) == -1 &&
           dce_passes == 0 && dce_values_scanned == 0 &&
           dce_values_removed == 0 && dce_temporary_bytes == 0 &&
           dce_temporary_peak_bytes == 0);
    assert(
        vir_dce_with_stats(NULL, VIR_OPT_O1, &dce_passes, &dce_values_scanned,
                           &dce_values_removed, &dce_temporary_bytes,
                           &dce_temporary_peak_bytes) == -1 &&
        dce_passes == 0 && dce_values_scanned == 0 && dce_values_removed == 0 &&
        dce_temporary_bytes == 0 && dce_temporary_peak_bytes == 0);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    product = vir_add(&func, entry, one, two);
    sum = vir_load(&func, entry, param, VIR_TYPE_I32);
    assert(sum && vir_block_set_return(&func, entry, one));
    /* An unread ordinary load is dead along with its effect. */
    assert(product && vir_dce(&func, VIR_OPT_O1) == 3 && !entry->effects &&
           !entry->last_effect && !param->uses && entry->next_order == 1 &&
           vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    product = vir_add(&func, entry, one, two);
    sum = vir_volatile_load(&func, entry, param, VIR_TYPE_I32);
    assert(sum && vir_block_set_return(&func, entry, one));
    /* A volatile load is observable even when its value is unused. */
    assert(product && vir_dce(&func, VIR_OPT_O1) == 2 && sum->def_effect &&
           sum->order == 1 && sum->def_effect->order == 1 &&
           entry->next_order == 2 && vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    sum = vir_slt(&func, entry, one, two);
    product = vir_slt(&func, entry, one, two);
    vir_value_t *comparison = vir_ult(&func, entry, one, two);
    param = vir_zext_i1(&func, entry, comparison);
    two = vir_zext_i1(&func, entry, product);
    one = vir_add(&func, entry, param, two);
    assert(sum && product && comparison && param && two && one &&
           vir_block_set_return(&func, entry, one));
    assert(vir_local_cse(&func, VIR_OPT_O1) == 1 && two->op0 == sum &&
           param->op0 == comparison && !product->uses);
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(!strcmp(text,
                   "b0(%d0:i32, %d1:i32):\n"
                   "  %d2 = slt.i32 %d0, %d1\n"
                   "  %d3 = slt.i32 %d0, %d1\n"
                   "  %d4 = ult.i32 %d0, %d1\n"
                   "  %d5 = zext.i1.i32 %d4\n"
                   "  %d6 = zext.i1.i32 %d2\n"
                   "  %d7 = add.i32 %d5, %d6\n"
                   "  return %d7\n"));
    fclose(out);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    vir_edge_args_t duplicate_true = {merge, &one, 1};
    vir_edge_args_t duplicate_false = {merge, &two, 1};
    assert(vir_block_set_branch(&func, entry, sum, &duplicate_true,
                                &duplicate_false));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_edge_delete(&func, entry->true_edge));
    assert(entry->terminator == VIR_TERM_JUMP && entry->outgoing->to == merge &&
           merge->incoming == entry->outgoing &&
           entry->outgoing->args[0] == two && !one->uses && !sum->uses);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    vir_edge_args_t deleted_true = {merge, &one, 1};
    vir_edge_args_t kept_false = {otherwise, NULL, 0};
    assert(vir_block_set_branch(&func, entry, sum, &deleted_true, &kept_false));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_block_set_return(&func, otherwise, two));
    assert(vir_edge_delete(&func, entry->true_edge));
    assert(entry->terminator == VIR_TERM_JUMP && !entry->branch_condition &&
           !entry->true_edge && !entry->false_edge &&
           entry->outgoing->to == otherwise && !sum->uses && !one->uses &&
           !merge->incoming);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, otherwise, VIR_TYPE_I32);
    vir_edge_args_t kept_true = {merge, NULL, 0};
    vir_edge_args_t deleted_false = {otherwise, &two, 1};
    assert(vir_block_set_branch(&func, entry, sum, &kept_true, &deleted_false));
    assert(vir_block_set_return(&func, merge, one));
    assert(vir_block_set_return(&func, otherwise, param));
    assert(vir_edge_delete(&func, entry->false_edge));
    assert(entry->terminator == VIR_TERM_JUMP && !entry->branch_condition &&
           !entry->true_edge && !entry->false_edge &&
           entry->outgoing->to == merge && !sum->uses && !two->uses &&
           !otherwise->incoming);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* Removing an unreachable block must unlink every ordered-effect use,
     * including direct-call arguments and typed volatile memory operands.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    dead = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    param = vir_stack_addr(&func, entry, 9, 4, 4);
    assert(entry && dead && one && param);
    assert(vir_store(&func, dead, param, one));
    sum = vir_load(&func, dead, param, VIR_TYPE_I32);
    assert(sum && vir_volatile_store(&func, dead, param, one));
    product = vir_volatile_load(&func, dead, param, VIR_TYPE_I32);
    assert(product);
    {
        vir_value_t *args[] = {one, sum, product};
        assert(vir_call(&func, dead, "dead_call", args, 3, VIR_TYPE_VOID));
    }
    assert(vir_block_set_return(&func, dead, product));
    assert(vir_block_set_return(&func, entry, one));
    assert(vir_verify(&func, &error));
    assert(vir_remove_unreachable(&func));
    assert(!param->uses && one->uses && one->uses->return_block == entry &&
           !one->uses->next && !sum->uses && !product->uses &&
           vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    product = vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1, one, two);
    func.arena.fail_after = 0;
    assert(!vir_zext_i1(&func, entry, product));
    assert(entry->tail == product && !product->uses);
    func.arena.fail_after = 1;
    assert(!vir_zext_i1(&func, entry, product));
    assert(entry->tail == product && !product->uses);
    func.arena.fail_after = 0;
    assert(!vir_trunc_i1(&func, entry, sum));
    assert(entry->tail == product && !sum->uses);
    func.arena.fail_after = 1;
    assert(!vir_trunc_i1(&func, entry, sum));
    assert(entry->tail == product && !sum->uses);
    func.arena.fail_after = -1;
    param = vir_zext_i1(&func, entry, product);
    vir_value_t *truncated = vir_trunc_i1(&func, entry, sum);
    assert(param && param->opcode == VIR_OP_ZEXT &&
           param->type == VIR_TYPE_I32 && param->op0 == product);
    assert(truncated && truncated->opcode == VIR_OP_TRUNC &&
           truncated->type == VIR_TYPE_I1 && truncated->op0 == sum);
    assert(vir_zext_i1(&func, entry, vir_const_i1(&func, entry, 1))->constant ==
           1);
    assert(
        vir_trunc_i1(&func, entry, vir_const_i32(&func, entry, 3))->constant ==
        1);
    assert(!vir_zext_i1(&func, entry, one) &&
           !vir_trunc_i1(&func, entry, product));
    assert(vir_block_set_return(&func, entry, param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 64));
    entry = vir_block_create(&func);
    one = vir_const_int(&func, entry, VIR_TYPE_I8, 0xff);
    two = vir_const_int(&func, entry, VIR_TYPE_I8, 0x80);
    sum = vir_const_int(&func, entry, VIR_TYPE_I64, 0x123456789abcdef0ULL);
    product = vir_block_add_param(&func, entry, VIR_TYPE_I8);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    assert(one && two && sum && product && param && one->constant == 0xff &&
           two->constant == 0x80 && sum->constant == 0x123456789abcdef0ULL);
    assert(vir_zext(&func, entry, one, VIR_TYPE_I64)->constant == 0xff);
    assert(vir_sext(&func, entry, two, VIR_TYPE_I64)->constant == ~0x7fULL);
    assert(vir_trunc(&func, entry, sum, VIR_TYPE_I16)->constant == 0xdef0);
    assert(vir_zext(&func, entry, product, VIR_TYPE_I16)->opcode ==
           VIR_OP_ZEXT);
    assert(vir_sext(&func, entry, product, VIR_TYPE_I16)->opcode ==
           VIR_OP_SEXT);
    assert(vir_trunc(&func, entry,
                     vir_block_add_param(&func, entry, VIR_TYPE_I64),
                     VIR_TYPE_I8)
               ->opcode == VIR_OP_TRUNC);
    assert(!vir_zext(&func, entry, one, VIR_TYPE_I8) &&
           !vir_sext(&func, entry, param, VIR_TYPE_I64) &&
           !vir_trunc(&func, entry, one, VIR_TYPE_I16));
    assert(vir_block_set_return(&func, entry, one));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    product = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    assert(vir_block_set_return(&func, entry, product));
    vir_cse_stats_t cse_stats;
    assert(vir_local_cse_with_stats(&func, VIR_OPT_O0, &cse_stats) == 0 &&
           cse_stats.tables == 0 && cse_stats.table_slots == 0 &&
           cse_stats.values == 0 && cse_stats.lookups == 0 &&
           cse_stats.hits == 0 && cse_stats.probes == 0 &&
           cse_stats.replacements == 0);
    assert(vir_local_cse(&func, VIR_OPT_O0) == 0 &&
           entry->return_value == product);
    assert(vir_local_cse_with_stats(&func, VIR_OPT_O1, &cse_stats) == 1 &&
           entry->return_value == sum && !product->uses);
    assert(cse_stats.tables == 1 && cse_stats.values == 2 &&
           cse_stats.lookups == 2 && cse_stats.hits == 1 &&
           cse_stats.probes == 1 && cse_stats.replacements == 1 &&
           cse_stats.table_slots == 4);
    assert(vir_local_cse(&func, VIR_OPT_O2) == 0 &&
           vir_local_cse_with_stats(&func, (vir_opt_level_t) 99, &cse_stats) ==
               -1 &&
           cse_stats.tables == 0 && cse_stats.table_slots == 0 &&
           cse_stats.values == 0 && cse_stats.lookups == 0 &&
           cse_stats.hits == 0 && cse_stats.probes == 0 &&
           cse_stats.replacements == 0 &&
           vir_local_cse_with_stats(NULL, VIR_OPT_O1, &cse_stats) == -1 &&
           cse_stats.tables == 0 && cse_stats.table_slots == 0 &&
           cse_stats.values == 0 && cse_stats.lookups == 0 &&
           cse_stats.hits == 0 && cse_stats.probes == 0 &&
           cse_stats.replacements == 0);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    dead = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    assert(vir_edge_create(&func, entry, merge, NULL, 0));
    assert(vir_block_set_return(&func, merge, one));
    sum = vir_binary(&func, dead, VIR_OP_ADD, VIR_TYPE_I32, one, one);
    assert(sum && vir_block_set_return(&func, dead, sum));
    assert(vir_verify(&func, &error));
    assert(vir_remove_unreachable(&func));
    vir_collect_stats(&func, &stats);
    assert(stats.blocks == 2 && stats.values == 1 && one->uses &&
           one->uses->return_block == merge && !one->uses->next);
    vir_block_t *after_remove = vir_block_create(&func);
    product = vir_const_i32(&func, after_remove, 2);
    assert(after_remove->id == 3 && product->id > sum->id &&
           vir_block_set_return(&func, after_remove, product));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i32(&func, entry, 3);
    product = vir_const_i32(&func, entry, 0);
    assert(vir_add(&func, entry, one, product) == one);
    assert(vir_sub(&func, entry, two, product) == two);
    assert(vir_mul(&func, entry, one, sum) == sum);
    assert(vir_mul(&func, entry, product, sum)->constant == 0);
    assert(vir_sub(&func, entry, sum, sum)->constant == 0);
    product = vir_const_i32(&func, entry, -9);
    two = vir_const_i32(&func, entry, 2);
    assert(vir_div(&func, entry, product, two, false, false)->constant ==
               UINT_MAX - 3u &&
           vir_div(&func, entry, product, two, false, true)->constant ==
               UINT_MAX &&
           vir_div(&func, entry, product, two, true, false)->constant ==
               0x7ffffffb &&
           vir_div(&func, entry, product, two, true, true)->constant == 1);
    sum = vir_const_i32(&func, entry, 0);
    assert(vir_div(&func, entry, product, sum, false, false)->opcode ==
           VIR_OP_SDIV);
    sum = vir_const_i32(&func, entry, 3);
    param = vir_add(&func, entry, two, sum);
    assert(param->opcode == VIR_OP_CONST && param->constant == 5);
    param = vir_eq(&func, entry, one, one);
    assert(param->type == VIR_TYPE_I1 && param->constant == 1);
    param = vir_eq(&func, entry, two, one);
    assert(param->opcode == VIR_OP_CONST && param->type == VIR_TYPE_I1 &&
           param->constant == 0);
    product = vir_const_i32(&func, entry, -1);
    sum = vir_const_i32(&func, entry, 0);
    assert(vir_slt(&func, entry, product, sum)->constant == 1 &&
           vir_ult(&func, entry, product, sum)->constant == 0 &&
           vir_slt(&func, entry, sum, product)->constant == 0 &&
           vir_ult(&func, entry, sum, product)->constant == 1);
    product = vir_const_i32(&func, entry, 0x80000000);
    sum = vir_const_i32(&func, entry, 0x7fffffff);
    assert(vir_slt(&func, entry, product, sum)->constant == 1 &&
           vir_ult(&func, entry, product, sum)->constant == 0);
    assert(vir_bitand(&func, entry, product, sum)->constant == 0 &&
           vir_bitor(&func, entry, product, sum)->constant == 0xffffffffu &&
           vir_bitxor(&func, entry, product, sum)->constant == 0xffffffffu &&
           vir_bitnot(&func, entry, sum)->constant == 0x80000000u &&
           vir_neg(&func, entry, one)->constant == 0xffffffffu);
    assert(vir_shl(&func, entry, one, two)->constant == 4 &&
           vir_lshr(&func, entry, product, one)->constant == 0x40000000 &&
           vir_ashr(&func, entry, product, one)->constant == 0xc0000000u &&
           vir_shl(&func, entry, sum, product)->opcode == VIR_OP_SHL);
    param = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    assert(
        vir_slt(&func, entry, param, one)->opcode == VIR_OP_SLT &&
        vir_ult(&func, entry, param, one)->opcode == VIR_OP_ULT &&
        vir_binary(&func, entry, VIR_OP_SLT, VIR_TYPE_I1, param, one)->opcode ==
            VIR_OP_SLT &&
        !vir_binary(&func, entry, VIR_OP_SLT, VIR_TYPE_I1,
                    vir_const_int(&func, entry, VIR_TYPE_I8, 1),
                    vir_const_int(&func, entry, VIR_TYPE_I8, 2)));
    merge = vir_block_create(&func);
    vir_value_t *merge_param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    product = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, param);
    sum = vir_add(&func, entry, param, product);
    assert(sum->op0->id < sum->op1->id);
    assert(vir_edge_create(&func, entry, merge, &product, 1));
    assert(vir_block_set_return(&func, merge, merge_param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    vir_ssa_t *ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 7, one));
    vir_block_t *later_ssa = vir_block_create(&func);
    product = vir_const_i32(&func, later_ssa, 2);
    assert(!vir_ssa_write(ssa, entry, 8, product));
    assert(vir_block_set_return(&func, later_ssa, product));
    param = vir_ssa_predeclare(ssa, merge, 7, VIR_TYPE_I32);
    assert(param && vir_ssa_read(ssa, merge, 7) == param);
    assert(vir_ssa_jump(ssa, entry, merge));
    assert(!vir_ssa_predeclare(ssa, merge, 8, VIR_TYPE_I32));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    vir_block_t *then_ssa = vir_block_create(&func);
    vir_block_t *else_ssa = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_branch(ssa, entry, sum, then_ssa, else_ssa));
    assert(vir_ssa_write(ssa, then_ssa, 22, one));
    assert(vir_ssa_write(ssa, else_ssa, 22, two));
    assert(vir_ssa_jump(ssa, then_ssa, merge));
    assert(vir_ssa_jump(ssa, else_ssa, merge));
    param = vir_ssa_predeclare(ssa, merge, 22, VIR_TYPE_I32);
    assert(param && param->is_block_param && merge->param_count == 1 &&
           merge->incoming->arg_count == 1 &&
           merge->incoming->next_incoming->arg_count == 1 &&
           merge->incoming->args[0] == two &&
           merge->incoming->next_incoming->args[0] == one);
    assert(vir_ssa_seal(ssa, merge));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    then_ssa = vir_block_create(&func);
    else_ssa = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_branch(ssa, entry, sum, then_ssa, else_ssa));
    assert(vir_ssa_write(ssa, then_ssa, 23, one));
    assert(vir_ssa_write(ssa, else_ssa, 23, two));
    assert(vir_ssa_jump(ssa, then_ssa, merge));
    assert(vir_ssa_jump(ssa, else_ssa, merge));
    for (int fail = 0; fail < 5; fail++) {
        int next_value_id = func.next_value_id;

        func.arena.fail_after = fail;
        assert(!vir_ssa_predeclare(ssa, merge, 23, VIR_TYPE_I32));
        assert(merge->param_count == 0 && merge->incoming->arg_count == 0 &&
               merge->incoming->next_incoming->arg_count == 0 &&
               func.next_value_id == next_value_id);
    }
    func.arena.fail_after = -1;
    param = vir_ssa_predeclare(ssa, merge, 23, VIR_TYPE_I32);
    assert(param && vir_ssa_seal(ssa, merge));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    then_ssa = vir_block_create(&func);
    else_ssa = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_predeclare(ssa, merge, 21, VIR_TYPE_I32));
    assert(vir_ssa_branch(ssa, entry, sum, then_ssa, else_ssa));
    assert(vir_ssa_write(ssa, then_ssa, 21, one));
    assert(vir_ssa_write(ssa, else_ssa, 21, two));
    assert(vir_ssa_jump(ssa, then_ssa, merge));
    assert(vir_ssa_jump(ssa, else_ssa, merge));
    param = vir_ssa_read(ssa, merge, 21);
    assert(param && param->is_block_param && vir_ssa_seal(ssa, merge) &&
           merge->param_count == 1 && merge->incoming->args[0] == two &&
           merge->incoming->next_incoming->args[0] == one);
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    vir_block_t *cycle_middle = vir_block_create(&func);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_jump(ssa, entry, cycle_middle));
    assert(vir_ssa_jump(ssa, cycle_middle, entry));
    assert(vir_ssa_seal(ssa, entry) && vir_ssa_seal(ssa, cycle_middle));
    assert(!vir_ssa_read_sealed(ssa, entry, 16));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    vir_block_t *middle = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 15, one));
    assert(vir_ssa_jump(ssa, entry, middle));
    assert(vir_ssa_jump(ssa, middle, merge));
    assert(!vir_ssa_read_sealed(ssa, merge, 15));
    assert(vir_ssa_seal(ssa, middle) && vir_ssa_seal(ssa, merge));
    assert(vir_ssa_read_sealed(ssa, merge, 15) == one);
    assert(vir_block_set_return(&func, merge, one));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 0);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 12, two));
    param = vir_ssa_predeclare(ssa, merge, 12, VIR_TYPE_I32);
    assert(param && vir_ssa_jump(ssa, entry, merge));
    product = vir_add(&func, merge, param, one);
    assert(product && vir_ssa_write(ssa, merge, 12, product));
    assert(vir_ssa_jump(ssa, merge, merge));
    assert(vir_ssa_seal(ssa, merge) && merge->param_count == 1 &&
           vir_ssa_read(ssa, merge, 12) == product);
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    /* A loop-invariant value reaches the header from the entry and, on the back
     * edge, as the header's own parameter. Sealing drops that parameter and
     * forwards the entry value; only the changing counter stays.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 0);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 12, two) &&
           vir_ssa_write(ssa, entry, 14, one));
    param = vir_ssa_predeclare(ssa, merge, 12, VIR_TYPE_I32);
    assert(param && vir_ssa_predeclare(ssa, merge, 14, VIR_TYPE_I32) &&
           vir_ssa_jump(ssa, entry, merge));
    product = vir_add(&func, merge, param, one);
    assert(product && vir_ssa_write(ssa, merge, 12, product));
    assert(vir_ssa_jump(ssa, merge, merge));
    assert(vir_ssa_seal(ssa, merge) && merge->param_count == 1 &&
           vir_ssa_read(ssa, merge, 14) == one);
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    sum = vir_const_i1(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 10, one));
    assert(vir_ssa_write(ssa, entry, 11, one));
    assert(vir_ssa_write(ssa, entry, 13, one));
    param = vir_ssa_predeclare(ssa, merge, 10, VIR_TYPE_I32);
    product = vir_ssa_predeclare(ssa, merge, 11, VIR_TYPE_I32);
    two = vir_ssa_predeclare(ssa, merge, 13, VIR_TYPE_I32);
    assert(param && product && two &&
           vir_ssa_branch(ssa, entry, sum, merge, merge));
    assert(vir_block_set_return(&func, merge, param));
    func.arena.fail_after = 4;
    assert(!vir_ssa_seal(ssa, merge));
    assert(merge->param_count == 3 && entry->true_edge->arg_count == 3 &&
           entry->false_edge->arg_count == 3 && merge->return_value == param &&
           param->uses && vir_verify(&func, &error));
    func.arena.fail_after = -1;
    assert(vir_ssa_seal(ssa, merge));
    assert(!vir_ssa_seal(ssa, merge) && merge->param_count == 0 &&
           merge->return_value == one && entry->true_edge->arg_count == 0 &&
           entry->false_edge->arg_count == 0);
    assert(vir_ssa_read(ssa, merge, 10) == one);
    assert(vir_ssa_read(ssa, merge, 11) == one);
    assert(vir_ssa_read(ssa, merge, 13) == one);
    vir_block_t *sealed_from = vir_block_create(&func);
    two = vir_const_i32(&func, sealed_from, 2);
    assert(vir_ssa_write(ssa, sealed_from, 10, two));
    assert(!vir_ssa_jump(ssa, sealed_from, merge));
    assert(vir_block_set_return(&func, sealed_from, two));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    vir_block_t *otherwise_ssa = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    sum = vir_const_i1(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    assert(ssa && vir_ssa_write(ssa, entry, 9, one));
    param = vir_ssa_predeclare(ssa, merge, 9, VIR_TYPE_I32);
    product = vir_ssa_predeclare(ssa, otherwise_ssa, 9, VIR_TYPE_I32);
    assert(param && product);
    assert(vir_ssa_branch(ssa, entry, sum, merge, otherwise_ssa));
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_block_set_return(&func, otherwise_ssa, product));
    assert(vir_verify(&func, &error));
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    vir_block_t *left = vir_block_create(&func);
    vir_block_t *right = vir_block_create(&func);
    merge = vir_block_create(&func);
    vir_block_t *wrong = vir_block_create(&func);
    vir_value_t *wrong_return;
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    assert(vir_block_add_param(&func, wrong, VIR_TYPE_I1));
    vir_edge_args_t left_desc = {left, NULL, 0};
    vir_edge_args_t right_desc = {right, NULL, 0};
    assert(vir_block_set_branch(&func, entry, sum, &left_desc, &right_desc));
    product = vir_binary(&func, left, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    assert(vir_block_set_return(&func, left, product));
    assert(vir_edge_create(&func, right, merge, &two, 1));
    assert(vir_block_set_return(&func, merge, param));
    wrong_return = vir_const_i1(&func, wrong, 1);
    assert(vir_block_set_return(&func, wrong, wrong_return));
    assert(!vir_edge_redirect(&func, entry->true_edge, wrong));
    assert(vir_edge_redirect(&func, entry->true_edge, right));
    assert(entry->true_edge->to == right && entry->false_edge->to == right);
    for (int fail = 0; fail < 5; fail++) {
        func.arena.fail_after = fail;
        assert(!vir_edge_split(&func, right->outgoing));
        assert(right->outgoing->to == merge && func.last_block == wrong);
        assert(vir_verify(&func, &error));
    }
    func.arena.fail_after = -1;
    vir_block_t *split = vir_edge_split(&func, right->outgoing);
    assert(split && right->outgoing->to == split && split->param_count == 1);
    assert(split->outgoing->to == merge &&
           split->outgoing->args[0] == split->params);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    vir_edge_args_t split_true = {merge, &one, 1};
    vir_edge_args_t split_false = {merge, &two, 1};
    assert(vir_block_set_branch(&func, entry, sum, &split_true, &split_false));
    split = vir_edge_split(&func, entry->true_edge);
    assert(split && entry->true_edge->args[0] == one &&
           entry->false_edge->to == merge &&
           entry->false_edge->args[0] == two && split->outgoing->to == merge &&
           split->outgoing->args[0] == split->params);
    assert(vir_block_set_return(&func, merge, param));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    one = vir_const_i32(&func, entry, 1);
    edge = vir_edge_create(&func, entry, entry, &one, 1);
    split = vir_edge_split(&func, edge);
    assert(split && entry->outgoing->to == split &&
           split->outgoing->to == entry &&
           split->outgoing->args[0] == split->params);
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    vir_value_t *alternate_condition;
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1, one, two);
    alternate_condition =
        vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1, two, one);
    assert(!vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I1, one, two));
    vir_edge_args_t true_desc = {merge, NULL, 0};
    vir_edge_args_t false_desc = {NULL, NULL, 0};
    false_desc.to = otherwise;
    func.arena.fail_after = 0;
    assert(!vir_block_set_branch(&func, entry, sum, &true_desc, &false_desc));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = 1;
    assert(!vir_block_set_branch(&func, entry, sum, &true_desc, &false_desc));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = 2;
    assert(!vir_block_set_branch(&func, entry, sum, &true_desc, &false_desc));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    func.arena.fail_after = -1;
    false_desc.to = NULL;
    assert(!vir_block_set_branch(&func, entry, sum, &true_desc, &false_desc));
    assert(!entry->outgoing && entry->terminator == VIR_TERM_NONE);
    assert(vir_replace_all_uses(&func, sum, sum));
    assert(!vir_replace_all_uses(&func, sum, one));
    func.arena.fail_after = 0;
    assert(!vir_effect_create(&func, entry, VIR_EFFECT_CALL));
    assert(!entry->effects && !entry->last_effect);
    func.arena.fail_after = -1;
    assert(vir_effect_create(&func, entry, VIR_EFFECT_CALL));
    product = vir_const_i32(&func, merge, 3);
    false_desc.to = otherwise;
    assert(vir_block_set_branch(&func, entry, sum, &true_desc, &false_desc));
    assert(vir_block_set_return(&func, merge, one));
    assert(vir_block_set_return(&func, otherwise, two));
    assert(vir_verify(&func, &error));
    assert(vir_replace_all_uses(&func, sum, alternate_condition));
    assert(entry->branch_condition == alternate_condition);
    assert(!sum->uses);
    assert(vir_verify(&func, &error));
    assert(!vir_replace_all_uses(&func, two, product));
    assert(otherwise->return_value == two);
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "  %d2 = eq.i32 %d0, %d1\n") != NULL);
    assert(strstr(text, "  branch %d3, b1(), b2()\n") != NULL);
    fclose(out);
    vir_function_release(&func);

    /* Loads and stores carry reverse uses, but remain explicitly ordered
     * effects. This first memory slice deliberately has no forwarding.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    assert(entry && param && one);
    assert(!vir_load(&func, entry, one, VIR_TYPE_I32));
    assert(!vir_store(&func, entry, one, one));
    assert(!vir_effect_create(&func, entry, VIR_EFFECT_STORE));
    func.arena.fail_after = 0;
    assert(!vir_load(&func, entry, param, VIR_TYPE_I32));
    assert(!entry->head && !entry->effects && !param->uses);
    func.arena.fail_after = 1;
    assert(!vir_load(&func, entry, param, VIR_TYPE_I32));
    assert(!entry->head && !entry->effects && !param->uses);
    func.arena.fail_after = 2;
    assert(!vir_load(&func, entry, param, VIR_TYPE_I32));
    assert(!entry->head && !entry->effects && !param->uses);
    func.arena.fail_after = -1;
    sum = vir_load(&func, entry, param, VIR_TYPE_I32);
    assert(sum && sum->def_effect && sum->def_effect->position == 0);
    func.arena.fail_after = 0;
    assert(!vir_store(&func, entry, param, one));
    assert(!sum->uses && entry->last_effect == sum->def_effect);
    func.arena.fail_after = 1;
    assert(!vir_store(&func, entry, param, one));
    assert(!sum->uses && entry->last_effect == sum->def_effect);
    func.arena.fail_after = -1;
    effect = vir_store(&func, entry, param, one);
    assert(effect && effect->position == 1);
    assert(vir_replace_all_uses(&func, one, sum));
    assert(effect->stored_value == sum);
    assert(vir_block_set_return(&func, entry, sum));
    assert(vir_verify(&func, &error));
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "load.i32") && strstr(text, "store %d0, %d2"));
    fclose(out);
    vir_function_release(&func);

    /* A load result cannot flow backward into an earlier ordered effect. */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    effect = vir_store(&func, entry, param, one);
    two = vir_const_i32(&func, entry, 2);
    assert(effect && two && vir_replace_all_uses(&func, one, two));
    assert(effect->stored_value == two);
    sum = vir_load(&func, entry, param, VIR_TYPE_I32);
    assert(effect && sum && !vir_replace_all_uses(&func, two, sum));
    product = vir_add(&func, entry, sum, one);
    assert(product && !vir_replace_all_uses(&func, two, product));
    assert(effect->stored_value == two);
    assert(vir_block_set_return(&func, entry, one));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* A deferred effect cannot resolve to a pure value that depends on a later
     * ordered effect, even though the pure value itself can be hoisted.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    effect = vir_deferred_store(&func, entry, false);
    sum = vir_load(&func, entry, param, VIR_TYPE_I32);
    two = vir_const_i32(&func, entry, 2);
    product = vir_add(&func, entry, sum, two);
    assert(effect && sum && two && product &&
           !vir_deferred_store_resolve(&func, effect, param, product));
    vir_function_release(&func);

    /* Address roots retain object identity and layout, not backend placement.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    assert(!vir_stack_addr(&func, entry, 0, 4, 4));
    assert(!vir_global_addr(&func, entry, "object", 4, 4));
    assert(!entry->head && !func.next_value_id);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    assert(!vir_stack_addr(&func, entry, 0, 0, 4));
    assert(!vir_stack_addr(&func, entry, 0, 4, 3));
    assert(!vir_global_addr(&func, entry, "", 4, 4));
    param = vir_stack_addr(&func, entry, 7, 12, 4);
    assert(param && param->address_kind == VIR_ADDRESS_STACK &&
           param->address_slot == 7 && param->address_size == 12 &&
           param->address_alignment == 4);
    func.arena.fail_after = 0;
    assert(!vir_stack_addr(&func, entry, 8, 4, 4));
    assert(entry->tail == param && !param->next);
    func.arena.fail_after = -1;
    stack_duplicate = vir_stack_addr(&func, entry, 7, 12, 4);
    assert(stack_duplicate);
    assert(!vir_stack_addr(&func, entry, 7, 8, 4));
    assert(!vir_stack_addr(&func, entry, 7, 12, 8));
    func.arena.fail_after = 0;
    assert(!vir_global_addr(&func, entry, "object", 8, 8));
    assert(entry->tail == stack_duplicate && !stack_duplicate->next);
    func.arena.fail_after = 1;
    assert(!vir_global_addr(&func, entry, "object", 8, 8));
    assert(entry->tail == stack_duplicate && !stack_duplicate->next);
    func.arena.fail_after = -1;
    {
        char name[] = "object";
        one = vir_global_addr(&func, entry, name, 8, 8);
        name[0] = 'X';
    }
    assert(one && one->address_kind == VIR_ADDRESS_GLOBAL &&
           !strcmp(one->address_name, "object") && one->address_size == 8 &&
           one->address_alignment == 8);
    global_duplicate = vir_global_addr(&func, entry, "object", 8, 8);
    assert(global_duplicate);
    assert(!vir_global_addr(&func, entry, "object", 4, 4));
    two = vir_const_i32(&func, entry, 1);
    sum = vir_ptradd(&func, entry, param, two);
    assert(sum && sum->opcode == VIR_OP_PTRADD);
    effect = vir_store(&func, entry, one, two);
    product = vir_load(&func, entry, sum, VIR_TYPE_I32);
    assert(effect && product && vir_block_set_return(&func, entry, product));
    assert(vir_verify(&func, &error));
    one->address_alignment = 3;
    assert(!vir_verify(&func, &error));
    one->address_alignment = 8;
    stack_duplicate->address_alignment = 8;
    assert(!vir_verify(&func, &error));
    stack_duplicate->address_alignment = 4;
    global_duplicate->address_size = 4;
    assert(!vir_verify(&func, &error));
    global_duplicate->address_size = 8;
    two->address_kind = VIR_ADDRESS_STACK;
    assert(!vir_verify(&func, &error));
    two->address_kind = VIR_ADDRESS_NONE;
    assert(vir_verify(&func, &error));
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "stackaddr 7, 12, align 4") &&
           strstr(text, "globaladdr @object, 8, align 8"));
    fclose(out);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    param = vir_stack_addr(&func, entry, 1, 4, 4);
    one = vir_const_i32(&func, entry, 1);
    effect = vir_volatile_store(&func, entry, param, one);
    sum = vir_volatile_load(&func, entry, param, VIR_TYPE_I32);
    assert(effect && sum && effect->kind == VIR_EFFECT_VOLATILE_STORE &&
           sum->def_effect->kind == VIR_EFFECT_VOLATILE_LOAD);
    assert(vir_block_set_return(&func, entry, sum));
    assert(vir_verify(&func, &error));
    effect->callee = "bad";
    assert(!vir_verify(&func, &error));
    effect->callee = NULL;
    sum->def_effect->arg_count = 1;
    assert(!vir_verify(&func, &error));
    sum->def_effect->arg_count = 0;
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    effect = vir_effect_create(&func, entry, VIR_EFFECT_VOLATILE);
    assert(effect && vir_block_set_return(&func, entry, one));
    assert(vir_verify(&func, &error));
    effect->address = one;
    assert(!vir_verify(&func, &error));
    effect->address = NULL;
    effect->callee = "bad";
    assert(!vir_verify(&func, &error));
    effect->callee = NULL;
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* A dead arm may use an entry constant, but that use cannot make its block
     * executable or poison the live merge parameter.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    vir_block_t *dead_arm_success = vir_block_create(&func);
    vir_block_t *dead_arm_failure = vir_block_create(&func);
    one = vir_const_i1(&func, entry, 1);
    two = vir_const_i32(&func, entry, 7);
    sum = vir_const_i32(&func, entry, 9);
    product = vir_binary(&func, otherwise, VIR_OP_ADD, VIR_TYPE_I32, sum,
                         vir_const_i32(&func, entry, 0));
    param = vir_block_add_param(&func, dead, VIR_TYPE_I32);
    stack_duplicate =
        vir_binary(&func, dead, VIR_OP_EQ, VIR_TYPE_I1, param, two);
    assert(product && param && stack_duplicate &&
           vir_block_set_return(&func, dead_arm_success, two) &&
           vir_block_set_return(&func, dead_arm_failure, sum));
    {
        vir_value_t *live_args[] = {two};
        vir_value_t *dead_args[] = {product};
        vir_edge_args_t enter_live = {merge, NULL, 0};
        vir_edge_args_t enter_dead = {otherwise, NULL, 0};
        vir_edge_args_t leave_true = {dead_arm_success, NULL, 0};
        vir_edge_args_t leave_false = {dead_arm_failure, NULL, 0};
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_edge_create(&func, merge, dead, live_args, 1));
        assert(vir_edge_create(&func, otherwise, dead, dead_args, 1));
        assert(
            vir_block_set_branch(&func, entry, one, &enter_live, &enter_dead));
        assert(vir_block_set_branch(&func, dead, stack_duplicate, &leave_true,
                                    &leave_false));
        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) == 5 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_REPLACED] == 1 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 2 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 2 &&
               entry->terminator == VIR_TERM_JUMP &&
               dead->terminator == VIR_TERM_JUMP &&
               dead->outgoing->to == dead_arm_success);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* A return-only function still allocates its one-byte edge-marker sentinel
     * and must report that scoped allocation.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    assert(one && vir_block_set_return(&func, entry, one));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];
        int expected_temporary =
            (int) (sizeof(unsigned char) + sizeof(unsigned long long) +
                   sizeof(unsigned char) + sizeof(int) + sizeof(unsigned char));

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) == 0 &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_BYTES] ==
                   expected_temporary &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_PEAK_BYTES] ==
                   expected_temporary);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* Only the true duplicate successor edge executes, so the merge parameter
     * retains its true-edge constant despite the false edge's different value.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    dead = vir_block_create(&func);
    vir_block_t *success = vir_block_create(&func);
    vir_block_t *failure = vir_block_create(&func);
    one = vir_const_i1(&func, entry, 1);
    two = vir_const_i32(&func, entry, 7);
    sum = vir_const_i32(&func, entry, 9);
    param = vir_block_add_param(&func, dead, VIR_TYPE_I32);
    product = vir_binary(&func, dead, VIR_OP_EQ, VIR_TYPE_I1, param, two);
    assert(param && product && vir_block_set_return(&func, success, two) &&
           vir_block_set_return(&func, failure, sum));
    {
        vir_value_t *left_args[] = {two};
        vir_value_t *right_args[] = {sum};
        vir_edge_args_t enter_left = {dead, left_args, 1};
        vir_edge_args_t enter_right = {dead, right_args, 1};
        vir_edge_args_t leave_true = {success, NULL, 0};
        vir_edge_args_t leave_false = {failure, NULL, 0};
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(
            vir_block_set_branch(&func, entry, one, &enter_left, &enter_right));
        assert(vir_block_set_branch(&func, dead, product, &leave_true,
                                    &leave_false));
        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) == 4 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_REPLACED] == 1 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 2 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 1 &&
               entry->terminator == VIR_TERM_JUMP &&
               dead->terminator == VIR_TERM_JUMP &&
               dead->outgoing->to == success);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* SCCP arithmetic and shifts keep the selected value width. In particular,
     * an i64 high word must not be truncated through an unsigned int temporary
     * while folding a value that arrived through a block edge.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I64);
    one = vir_const_int(&func, merge, VIR_TYPE_I64, 1);
    sum = vir_binary(&func, merge, VIR_OP_ADD, VIR_TYPE_I64, param, one);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, 0x100000000ULL);
    assert(param && one && sum &&
           vir_edge_create(&func, entry, merge, &product, 1) &&
           vir_block_set_return(&func, merge, sum));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) > 0 &&
               merge->return_value->opcode == VIR_OP_CONST &&
               merge->return_value->constant == 0x100000001ULL);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I64);
    one = vir_const_int(&func, merge, VIR_TYPE_I64, 0);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, ~0ULL);
    sum = vir_binary(&func, merge, VIR_OP_SLT, VIR_TYPE_I1, param, one);
    assert(param && one && product && sum &&
           vir_edge_create(&func, entry, merge, &product, 1) &&
           vir_block_set_return(&func, merge, sum));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) > 0 &&
               merge->return_value->opcode == VIR_OP_CONST &&
               merge->return_value->constant == 1);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I64);
    one = vir_const_int(&func, merge, VIR_TYPE_I32, 32);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, 1);
    sum = vir_binary(&func, merge, VIR_OP_SHL, VIR_TYPE_I64, param, one);
    assert(param && one && product && sum &&
           vir_edge_create(&func, entry, merge, &product, 1) &&
           vir_block_set_return(&func, merge, sum));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) > 0 &&
               merge->return_value->opcode == VIR_OP_CONST &&
               merge->return_value->constant == 0x100000000ULL);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I64);
    one = vir_const_int(&func, merge, VIR_TYPE_I32, 32);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, 0x100000000ULL);
    sum = vir_binary(&func, merge, VIR_OP_LSHR, VIR_TYPE_I64, param, one);
    assert(param && one && product && sum &&
           vir_edge_create(&func, entry, merge, &product, 1) &&
           vir_block_set_return(&func, merge, sum));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) > 0 &&
               merge->return_value->opcode == VIR_OP_CONST &&
               merge->return_value->constant == 1);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I64);
    one = vir_const_int(&func, merge, VIR_TYPE_I32, 32);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, 0x8000000000000000ULL);
    sum = vir_binary(&func, merge, VIR_OP_ASHR, VIR_TYPE_I64, param, one);
    assert(param && one && product && sum &&
           vir_edge_create(&func, entry, merge, &product, 1) &&
           vir_block_set_return(&func, merge, sum));
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) > 0 &&
               merge->return_value->opcode == VIR_OP_CONST &&
               merge->return_value->constant == 0xffffffff80000000ULL);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* A loop header initially sees a constant entry edge, then a different
     * executable backedge. Its parameter and condition must become overdefined
     * rather than deleting the loop exit.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 10);
    sum = vir_const_i32(&func, entry, 0);
    product = vir_binary(&func, merge, VIR_OP_SLT, VIR_TYPE_I1, param, two);
    stack_duplicate =
        vir_binary(&func, otherwise, VIR_OP_ADD, VIR_TYPE_I32, param, one);
    assert(param && product && stack_duplicate &&
           vir_block_set_return(&func, dead, param));
    {
        vir_value_t *entry_args[] = {sum};
        vir_value_t *backedge_args[] = {stack_duplicate};
        vir_edge_args_t loop_body = {otherwise, NULL, 0};
        vir_edge_args_t loop_exit = {dead, NULL, 0};
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_edge_create(&func, entry, merge, entry_args, 1));
        assert(vir_edge_create(&func, otherwise, merge, backedge_args, 1));
        assert(vir_block_set_branch(&func, merge, product, &loop_body,
                                    &loop_exit));
        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) == 0 &&
               sccp_stats[VIR_SCCP_STAT_PASSES] >= 3 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 0 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 0 &&
               merge->terminator == VIR_TERM_BRANCH &&
               merge->true_edge->to == otherwise &&
               merge->false_edge->to == dead);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* SCCP keeps its lattice outside VIR, folds a forced non-canonical value,
     * removes the false edge, and invalidates the resulting dead block.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    product = vir_const_i32(&func, entry, 3);
    param = vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1, sum, product);
    assert(sum && product && param && vir_block_set_return(&func, merge, two) &&
           vir_block_set_return(&func, otherwise, one));
    {
        vir_edge_args_t sccp_true_desc = {merge, NULL, 0};
        vir_edge_args_t sccp_false_desc = {otherwise, NULL, 0};
        int sccp_stats[VIR_SCCP_STAT_COUNT];

        assert(vir_block_set_branch(&func, entry, param, &sccp_true_desc,
                                    &sccp_false_desc));
        assert(vir_sccp_with_stats(&func, VIR_OPT_O0, sccp_stats) == 0 &&
               sccp_stats[VIR_SCCP_STAT_PASSES] == 0 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_SCANNED] == 0 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_REPLACED] == 0 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 0 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 0 &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_BYTES] == 0 &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_PEAK_BYTES] == 0);
        assert(vir_sccp_with_stats(&func, VIR_OPT_O1, sccp_stats) == 3 &&
               sccp_stats[VIR_SCCP_STAT_PASSES] == 2 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_SCANNED] == 5 &&
               sccp_stats[VIR_SCCP_STAT_VALUES_REPLACED] == 1 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 1 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 1 &&
               entry->terminator == VIR_TERM_JUMP &&
               entry->outgoing->to == merge &&
               entry->branch_condition == NULL &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_BYTES] > 0 &&
               sccp_stats[VIR_SCCP_STAT_TEMPORARY_BYTES] ==
                   sccp_stats[VIR_SCCP_STAT_TEMPORARY_PEAK_BYTES]);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* GVN keeps its table outside VIR and only replaces a duplicate whose
     * leader dominates it.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_binary(&func, entry, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    product = vir_binary(&func, merge, VIR_OP_ADD, VIR_TYPE_I32, one, two);
    assert(sum && product && vir_edge_create(&func, entry, merge, NULL, 0) &&
           vir_block_set_return(&func, merge, product));
    {
        vir_cse_stats_t gvn_stats;

        assert(vir_gvn_with_stats(&func, VIR_OPT_O1, &gvn_stats) == 0 &&
               gvn_stats.tables == 0);
        assert(vir_gvn_with_stats(&func, VIR_OPT_O2, &gvn_stats) == 1 &&
               gvn_stats.tables == 1 && gvn_stats.hits == 1 &&
               gvn_stats.replacements == 1 && merge->return_value == sum);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* The first diamond arm is a table leader but does not dominate the second
     * arm, so GVN must retain both values.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    two = vir_const_i32(&func, entry, 2);
    sum = vir_const_i32(&func, entry, 3);
    product = vir_binary(&func, otherwise, VIR_OP_ADD, VIR_TYPE_I32, two, sum);
    stack_duplicate =
        vir_binary(&func, dead, VIR_OP_ADD, VIR_TYPE_I32, two, sum);
    assert(param && one && two && sum && product && stack_duplicate &&
           vir_block_set_return(&func, merge, param));
    {
        vir_value_t *then_args[] = {product};
        vir_value_t *else_args[] = {stack_duplicate};
        vir_edge_args_t gvn_true = {otherwise, NULL, 0};
        vir_edge_args_t gvn_false = {dead, NULL, 0};
        vir_cse_stats_t gvn_stats;

        assert(vir_edge_create(&func, otherwise, merge, then_args, 1));
        assert(vir_edge_create(&func, dead, merge, else_args, 1));
        assert(vir_block_set_branch(&func, entry, one, &gvn_true, &gvn_false));
        assert(vir_gvn_with_stats(&func, VIR_OPT_O2, &gvn_stats) == 0 &&
               gvn_stats.hits == 1 && gvn_stats.replacements == 0 &&
               product->uses && stack_duplicate->uses);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* LICM derives a natural loop only at O2 and hoists an effect-free value
     * with operands defined outside the loop into its single-edge preheader.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    sum = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    product = vir_binary(&func, otherwise, VIR_OP_MUL, VIR_TYPE_I32, two, sum);
    stack_duplicate = vir_const_i32(&func, entry, 1);
    assert(one && two && sum && param && product && stack_duplicate);
    {
        vir_value_t *entry_args[] = {stack_duplicate};
        vir_value_t *backedge_args[] = {product};
        vir_edge_args_t licm_true = {otherwise, NULL, 0};
        vir_edge_args_t licm_false = {dead, NULL, 0};
        int licm_stats[VIR_LICM_STAT_COUNT];
        int licm_hoisted;

        assert(
            vir_edge_create(&func, entry, merge, entry_args, 1) &&
            vir_edge_create(&func, otherwise, merge, backedge_args, 1) &&
            vir_block_set_branch(&func, merge, one, &licm_true, &licm_false) &&
            vir_block_set_return(&func, dead, param));
        assert(vir_licm_with_stats(&func, VIR_OPT_O1, licm_stats) == 1 &&
               licm_stats[VIR_LICM_STAT_LOOPS] == 1 &&
               licm_stats[VIR_LICM_STAT_VALUES_HOISTED] == 1 &&
               licm_stats[VIR_LICM_STAT_TEMPORARY_BYTES] > 0 &&
               product->block == entry);
        licm_hoisted = vir_licm_with_stats(&func, VIR_OPT_O2, licm_stats);
        assert(licm_hoisted == 0 && licm_stats[VIR_LICM_STAT_LOOPS] == 1 &&
               licm_stats[VIR_LICM_STAT_VALUES_HOISTED] == 0 &&
               product->block == entry && otherwise->head == NULL);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* The dominance ID map must still reject an out-of-function block whose ID
     * collides with a live block; the old LICM scan required pointer identity.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    product = vir_binary(&func, otherwise, VIR_OP_MUL, VIR_TYPE_I32, one, two);
    assert(entry && merge && otherwise && dead && entry_param && one && two &&
           product);
    {
        vir_edge_args_t loop_true = {otherwise, NULL, 0};
        vir_edge_args_t loop_false = {dead, NULL, 0};
        vir_block_t stale_block;

        assert(vir_edge_create(&func, entry, merge, NULL, 0) &&
               vir_edge_create(&func, otherwise, merge, NULL, 0) &&
               vir_block_set_branch(&func, merge, entry_param, &loop_true,
                                    &loop_false) &&
               vir_block_set_return(&func, dead, one) &&
               vir_verify(&func, &error));
        memset(&stale_block, 0, sizeof(stale_block));
        stale_block.id = entry->id;
        two->block = &stale_block;
        assert(vir_licm(&func, VIR_OPT_O2) == 0 && product->block == otherwise);
        two->block = entry;
        assert(vir_verify(&func, &error));
    }
    vir_function_release(&func);

    /* Multiple latches belong to one natural loop. LICM must union the backedge
     * regions before identifying the loop's entry preheader.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    vir_block_t *second_body = vir_block_create(&func);
    vir_block_t *first_latch = vir_block_create(&func);
    vir_block_t *second_latch = vir_block_create(&func);
    vir_value_t *loop_value;
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    sum = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I1);
    loop_value = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    product = vir_binary(&func, otherwise, VIR_OP_MUL, VIR_TYPE_I32, two, sum);
    {
        vir_value_t *entry_args[] = {entry_param, two};
        vir_value_t *backedge_args[] = {param, product};
        vir_edge_args_t outer_exit = {dead, NULL, 0};
        vir_edge_args_t first_path = {otherwise, NULL, 0};
        vir_edge_args_t two_paths = {first_latch, NULL, 0};
        vir_edge_args_t second_path = {second_body, NULL, 0};
        int multi_latch_stats[VIR_LICM_STAT_COUNT];

        assert(entry && merge && otherwise && dead && second_body &&
               first_latch && second_latch && entry_param && one && two &&
               sum && param && loop_value && product &&
               vir_edge_create(&func, entry, merge, entry_args, 2) &&
               vir_block_set_branch(&func, merge, param, &first_path,
                                    &outer_exit) &&
               vir_block_set_branch(&func, otherwise, one, &two_paths,
                                    &second_path) &&
               vir_edge_create(&func, second_body, second_latch, NULL, 0) &&
               vir_edge_create(&func, first_latch, merge, backedge_args, 2) &&
               vir_edge_create(&func, second_latch, merge, backedge_args, 2) &&
               vir_block_set_return(&func, dead, loop_value) &&
               vir_verify(&func, &error));
        assert(vir_licm_with_stats(&func, VIR_OPT_O2, multi_latch_stats) == 1 &&
               multi_latch_stats[VIR_LICM_STAT_LOOPS] == 1 &&
               multi_latch_stats[VIR_LICM_STAT_VALUES_HOISTED] == 1 &&
               multi_latch_stats[VIR_LICM_STAT_TEMPORARY_BYTES] > 0 &&
               product->block == entry && vir_verify(&func, &error));
    }
    vir_function_release(&func);

    /* LICM's pass-local dominator bitsets keep sparse large-CFG storage below
     * the former four-byte-per-block-pair matrix while retaining loop motion.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    dead = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    assert(entry && merge && dead && entry_param && one && two);
    {
        const int loop_body_blocks = 513;
        vir_block_t *loop_blocks[loop_body_blocks];
        vir_value_t *invariants[loop_body_blocks];
        vir_edge_args_t loop_true;
        vir_edge_args_t loop_false = {dead, NULL, 0};
        int licm_stats[VIR_LICM_STAT_COUNT];
        int block_count;

        for (int i = 0; i < loop_body_blocks; i++) {
            loop_blocks[i] = vir_block_create(&func);
            assert(loop_blocks[i]);
        }
        loop_true.to = loop_blocks[loop_body_blocks - 1];
        loop_true.args = NULL;
        loop_true.arg_count = 0;
        assert(vir_edge_create(&func, entry, merge, NULL, 0));
        invariants[loop_body_blocks - 1] =
            vir_binary(&func, loop_blocks[loop_body_blocks - 1], VIR_OP_MUL,
                       VIR_TYPE_I32, one, two);
        assert(invariants[loop_body_blocks - 1]);
        for (int i = loop_body_blocks - 2; i >= 0; i--) {
            invariants[i] = vir_binary(&func, loop_blocks[i], VIR_OP_ADD,
                                       VIR_TYPE_I32, invariants[i + 1], two);
            assert(invariants[i] && vir_edge_create(&func, loop_blocks[i + 1],
                                                    loop_blocks[i], NULL, 0));
        }
        assert(vir_edge_create(&func, loop_blocks[0], merge, NULL, 0) &&
               vir_block_set_branch(&func, merge, entry_param, &loop_true,
                                    &loop_false) &&
               vir_block_set_return(&func, dead, one));
        block_count = vir_function_block_count(&func);
        assert(vir_verify(&func, &error));
        assert(vir_licm_with_stats(&func, VIR_OPT_O2, licm_stats) ==
                   loop_body_blocks &&
               licm_stats[VIR_LICM_STAT_LOOPS] == 1 &&
               licm_stats[VIR_LICM_STAT_VALUES_HOISTED] == loop_body_blocks &&
               invariants[0]->block == entry &&
               invariants[loop_body_blocks - 1]->block == entry &&
               vir_verify(&func, &error));
        assert(licm_stats[VIR_LICM_STAT_TEMPORARY_BYTES] <
               block_count * block_count * (int) sizeof(int));
    }
    vir_function_release(&func);

    /* Nested-loop LICM must hoist values to the nearest valid preheader: an
     * outer invariant moves to entry, while an inner-only invariant stays
     * inside the outer loop and moves immediately before the inner header.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    vir_block_t *inner_header = vir_block_create(&func);
    vir_block_t *inner_body = vir_block_create(&func);
    vir_block_t *outer_latch = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    two = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    vir_value_t *three = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    vir_value_t *four = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    vir_value_t *outer_value = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    vir_value_t *inner_value =
        vir_block_add_param(&func, inner_header, VIR_TYPE_I32);
    vir_value_t *outer_invariant;
    vir_value_t *inner_invariant;
    vir_value_t *inner_next;
    vir_value_t *outer_next;
    int nested_licm_stats[VIR_LICM_STAT_COUNT];
    vir_edge_args_t outer_true = {otherwise, NULL, 0};
    vir_edge_args_t outer_false = {dead, NULL, 0};
    vir_edge_args_t inner_true = {inner_body, NULL, 0};
    vir_edge_args_t inner_false = {outer_latch, NULL, 0};

    assert(entry && merge && otherwise && dead && inner_header && inner_body &&
           outer_latch && entry_param && one && two && three && four &&
           outer_value && inner_value);
    outer_invariant =
        vir_binary(&func, merge, VIR_OP_MUL, VIR_TYPE_I32, two, three);
    inner_invariant = vir_binary(&func, inner_header, VIR_OP_ADD, VIR_TYPE_I32,
                                 outer_value, four);
    inner_next = vir_binary(&func, inner_body, VIR_OP_ADD, VIR_TYPE_I32,
                            inner_value, inner_invariant);
    outer_next = vir_binary(&func, outer_latch, VIR_OP_ADD, VIR_TYPE_I32,
                            outer_value, two);
    vir_value_t *initial_args[] = {two};
    vir_value_t *inner_backedge_args[] = {inner_next};
    vir_value_t *outer_backedge_args[] = {outer_next};
    assert(outer_invariant && inner_invariant && inner_next && outer_next &&
           vir_edge_create(&func, entry, merge, initial_args, 1) &&
           vir_block_set_branch(&func, merge, entry_param, &outer_true,
                                &outer_false) &&
           vir_edge_create(&func, otherwise, inner_header, initial_args, 1) &&
           vir_block_set_branch(&func, inner_header, one, &inner_true,
                                &inner_false) &&
           vir_edge_create(&func, inner_body, inner_header, inner_backedge_args,
                           1) &&
           vir_edge_create(&func, outer_latch, merge, outer_backedge_args, 1) &&
           vir_block_set_return(&func, dead, outer_invariant) &&
           vir_verify(&func, &error));
    assert(vir_licm_with_stats(&func, VIR_OPT_O2, nested_licm_stats) == 2 &&
           nested_licm_stats[VIR_LICM_STAT_LOOPS] == 2 &&
           nested_licm_stats[VIR_LICM_STAT_VALUES_HOISTED] == 2 &&
           nested_licm_stats[VIR_LICM_STAT_TEMPORARY_BYTES] > 0 &&
           outer_invariant->block == entry &&
           inner_invariant->block == otherwise && vir_verify(&func, &error));
    vir_function_release(&func);

    /* CFG simplification redirects each predecessor through an empty jump
     * block, preserves a duplicate branch target, and removes the forwarder.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    one = vir_const_i1(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    assert(one && two && vir_edge_create(&func, merge, dead, NULL, 0) &&
           vir_block_set_return(&func, dead, two));
    {
        vir_edge_args_t cfg_true_desc = {merge, NULL, 0};
        vir_edge_args_t cfg_false_desc = {otherwise, NULL, 0};

        assert(vir_edge_create(&func, otherwise, merge, NULL, 0));
        assert(vir_block_set_branch(&func, entry, one, &cfg_true_desc,
                                    &cfg_false_desc));
        assert(vir_simplify_cfg(&func, VIR_OPT_O0) == 0);
        assert(vir_simplify_cfg(&func, VIR_OPT_O1) > 0 &&
               entry->true_edge->to == dead && entry->false_edge->to == dead &&
               func.blocks == entry && entry->next == dead && !dead->next);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* Branches on promoted booleans preserve polarity and reverse-use links. */
    for (int width = 0; width < 2; width++) {
        for (int constant = 0; constant < 3; constant++) {
            vir_type_t type = width ? VIR_TYPE_I64 : VIR_TYPE_I32;
            vir_function_init(&func, 64);
            entry = vir_block_create(&func);
            merge = vir_block_create(&func);
            otherwise = vir_block_create(&func);
            param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
            vir_value_t *promoted = vir_zext(&func, entry, param, type);
            one = vir_const_int(&func, entry, type, constant);
            product = vir_eq(&func, entry, promoted, one);
            vir_edge_args_t yes = {merge, NULL, 0};
            vir_edge_args_t no = {otherwise, NULL, 0};
            assert(product &&
                   vir_block_set_branch(&func, entry, product, &yes, &no));
            assert(vir_block_set_return(&func, merge,
                                        vir_const_i32(&func, merge, 1)));
            assert(vir_block_set_return(&func, otherwise,
                                        vir_const_i32(&func, otherwise, 0)));
            assert(vir_verify(&func, &error));
            assert(vir_simplify_cfg(&func, VIR_OPT_O1) >= 0);
            assert(entry->branch_condition == (constant < 2 ? param : product));
            assert(entry->true_edge->to == (constant ? merge : otherwise));
            assert(vir_dce(&func, VIR_OPT_O1) >= 0);
            assert(vir_verify(&func, &error));
            vir_function_release(&func);
        }
    }

    /* Unreachable pruning removes rewritten forwarders and larger CFGs while
     * preserving edge/use ownership for the later optimization passes.
     */
    for (int total = 40; total <= 41; total++) {
        vir_block_t *forwarder;
        vir_block_t *target;
        vir_function_init(&func, 64);
        entry = vir_block_create(&func);
        forwarder = vir_block_create(&func);
        target = vir_block_create(&func);
        two = vir_const_i32(&func, entry, 2);
        entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I32);
        assert(entry && forwarder && target && two && entry_param);
        assert(vir_edge_create(&func, entry, forwarder, NULL, 0) &&
               vir_edge_create(&func, forwarder, target, NULL, 0) &&
               vir_block_set_return(&func, target, two));
        for (int extra = 3; extra < total; extra++) {
            vir_block_t *unreachable = vir_block_create(&func);
            vir_value_t *dead_value =
                extra == 3 ? vir_neg(&func, unreachable, entry_param) : two;
            assert(unreachable && dead_value &&
                   vir_block_set_return(&func, unreachable, dead_value));
        }
        assert(vir_simplify_cfg(&func, VIR_OPT_O1) > 0);
        assert(vir_verify(&func, &error));
        if (total == 40)
            assert(func.blocks == entry && entry->next == target &&
                   !target->next);
        assert(vir_function_block_count(&func) == 2 && func.blocks == entry &&
               entry->next == target && !target->next);
        vir_function_release(&func);
    }

    /* Keep the largest transactional-pruning case coupled to the optimizer:
     * after dead blocks are removed, verification and LICM must still walk the
     * surviving loop using the compact block-index map.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_I1);
    one = vir_block_add_param(&func, entry, VIR_TYPE_I32);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    sum = vir_binary(&func, otherwise, VIR_OP_MUL, VIR_TYPE_I32, one, one);
    assert(entry && merge && otherwise && dead && entry_param && one && param &&
           sum);
    {
        vir_value_t *entry_args[] = {one};
        vir_value_t *back_args[] = {sum};
        vir_edge_args_t loop_true = {otherwise, NULL, 0};
        vir_edge_args_t loop_false = {dead, NULL, 0};

        assert(vir_edge_create(&func, entry, merge, entry_args, 1) &&
               vir_edge_create(&func, otherwise, merge, back_args, 1) &&
               vir_block_set_branch(&func, merge, entry_param, &loop_true,
                                    &loop_false) &&
               vir_block_set_return(&func, dead, param));
    }
    for (int extra = 4; extra < 42; extra++) {
        vir_block_t *unreachable = vir_block_create(&func);
        vir_value_t *dead_value = vir_neg(&func, unreachable, one);

        assert(unreachable && dead_value &&
               vir_block_set_return(&func, unreachable, dead_value));
    }
    assert(vir_verify(&func, &error));
    assert(vir_remove_unreachable(&func));
    assert(vir_verify(&func, &error));
    assert(vir_function_block_count(&func) == 4 && func.blocks == entry &&
           entry->next == merge && merge->next == otherwise &&
           otherwise->next == dead && !dead->next);
    {
        int licm_stats[VIR_LICM_STAT_COUNT];

        assert(vir_licm_with_stats(&func, VIR_OPT_O2, licm_stats) == 1 &&
               licm_stats[VIR_LICM_STAT_LOOPS] == 1 &&
               licm_stats[VIR_LICM_STAT_VALUES_HOISTED] == 1 &&
               sum->block == entry);
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* Large CFG SCCP rewrites defer unreachable deletion until after LICM. This
     * keeps the optimizer's traversal on the original block topology; the final
     * prune still removes the dead branch and unreachable tail.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    one = vir_const_i1(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    assert(entry && merge && otherwise && one && two);
    {
        vir_edge_args_t sccp_true = {merge, NULL, 0};
        vir_edge_args_t sccp_false = {otherwise, NULL, 0};

        assert(
            vir_block_set_return(&func, merge, two) &&
            vir_block_set_return(&func, otherwise, two) &&
            vir_block_set_branch(&func, entry, one, &sccp_true, &sccp_false));
    }
    for (int extra = 3; extra < 42; extra++) {
        vir_block_t *unreachable = vir_block_create(&func);

        assert(unreachable && vir_block_set_return(&func, unreachable, two));
    }
    {
        int sccp_stats[VIR_SCCP_STAT_COUNT];
        int licm_stats[VIR_LICM_STAT_COUNT];

        assert(vir_sccp_with_stats_and_pruning(&func, VIR_OPT_O2, sccp_stats,
                                               false) > 0 &&
               sccp_stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] == 1 &&
               sccp_stats[VIR_SCCP_STAT_BLOCKS_REMOVED] == 0 &&
               vir_function_block_count(&func) == 42);
        assert(vir_licm_with_stats(&func, VIR_OPT_O2, licm_stats) >= 0 &&
               vir_verify(&func, &error));
        assert(vir_remove_unreachable(&func) &&
               vir_function_block_count(&func) == 2 &&
               vir_verify(&func, &error));
    }
    vir_function_release(&func);

    /* Dominance verification must remain bounded for large CFGs. A value
     * defined in a non-entry dominator reaches the end of a 513-block chain
     * without allocating the old quadratic int matrix.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, 32));
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    assert(entry && merge && vir_edge_create(&func, entry, merge, NULL, 0));
    one = vir_const_i32(&func, merge, 1);
    param = vir_stack_addr(&func, merge, 1, 4, 4);
    {
        vir_block_t *previous = merge;
        for (int block_index = 0; block_index < 513; block_index++) {
            vir_block_t *current = vir_block_create(&func);
            assert(current &&
                   vir_edge_create(&func, previous, current, NULL, 0));
            previous = current;
        }
        assert(param && vir_store(&func, previous, param, one));
        product = vir_load(&func, previous, param, VIR_TYPE_I32);
        assert(product && vir_block_set_return(&func, previous, product));
    }
    assert(vir_verify(&func, &error));
    vir_function_release(&func);

    /* A sibling path reaching the use without the definition must be rejected,
     * even when the CFG is otherwise well formed.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    dead = vir_block_create(&func);
    one = vir_const_i1(&func, entry, 1);
    two = vir_const_i32(&func, merge, 2);
    {
        vir_edge_args_t true_edge = {merge, NULL, 0};
        vir_edge_args_t false_edge = {otherwise, NULL, 0};
        assert(
            entry && merge && otherwise && dead && one && two &&
            vir_block_set_branch(&func, entry, one, &true_edge, &false_edge) &&
            vir_edge_create(&func, merge, dead, NULL, 0) &&
            vir_edge_create(&func, otherwise, dead, NULL, 0) &&
            vir_block_set_return(&func, dead, two));
    }
    assert(!vir_verify(&func, &error));
    vir_function_release(&func);

    /* The entry dominator row must clear every bit above the first machine
     * word. Otherwise a high-index definition is falsely reported as dominating
     * a sibling path.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    otherwise = vir_block_create(&func);
    merge = vir_block_create(&func);
    {
        const int chain_count = (int) (sizeof(unsigned long) * CHAR_BIT) + 1;
        vir_block_t *first;
        vir_block_t *previous;
        vir_block_t *definition;
        vir_value_t *condition;
        vir_edge_args_t true_edge;
        vir_edge_args_t false_edge;

        assert(entry && otherwise && merge);
        first = vir_block_create(&func);
        condition = vir_const_i1(&func, entry, 1);
        assert(first && condition);
        true_edge.to = first;
        true_edge.args = NULL;
        true_edge.arg_count = 0;
        false_edge.to = otherwise;
        false_edge.args = NULL;
        false_edge.arg_count = 0;
        assert(vir_block_set_branch(&func, entry, condition, &true_edge,
                                    &false_edge));
        previous = first;
        for (int index = 1; index < chain_count; index++) {
            vir_block_t *current = vir_block_create(&func);

            assert(current &&
                   vir_edge_create(&func, previous, current, NULL, 0));
            previous = current;
        }
        definition = vir_block_create(&func);
        assert(definition);
        two = vir_const_i32(&func, definition, 2);
        assert(two && vir_edge_create(&func, previous, definition, NULL, 0) &&
               vir_edge_create(&func, otherwise, merge, NULL, 0) &&
               vir_edge_create(&func, definition, merge, NULL, 0) &&
               vir_block_set_return(&func, merge, two));
    }
    assert(!vir_verify(&func, &error));
    vir_function_release(&func);

    /* Direct calls own variable-arity reverse uses and stay ordered effects. */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    for (int fail = 0; fail < 5; fail++) {
        vir_value_t *args[] = {one, two};
        func.arena.fail_after = fail;
        assert(!vir_call(&func, entry, "sum", args, 2, VIR_TYPE_I32));
        assert(entry->tail == two && !two->next && !entry->effects);
    }
    func.arena.fail_after = -1;
    {
        vir_value_t *args[] = {one, two};
        char callee[] = "sum";
        effect = vir_call(&func, entry, "sink", args, 2, VIR_TYPE_VOID);
        call = vir_call(&func, entry, callee, args, 2, VIR_TYPE_I32);
        callee[0] = 'X';
    }
    assert(effect && !effect->result && call && call->result);
    assert(!strcmp(call->callee, "sum"));
    assert(vir_replace_all_uses(&func, one, two));
    assert(effect->args[0] == two && call->args[0] == two);
    assert(vir_block_set_return(&func, entry, call->result));
    assert(vir_verify(&func, &error));
    call->address = one;
    assert(!vir_verify(&func, &error));
    call->address = NULL;
    effect->stored_value = two;
    assert(!vir_verify(&func, &error));
    effect->stored_value = NULL;
    assert(vir_verify(&func, &error));
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "call @sink(%d1, %d1)") &&
           strstr(text, "%d2 = call.i32 @sum(%d1, %d1)"));
    fclose(out);
    vir_function_release(&func);

    /* Indirect calls retain the callee pointer as an ordinary effect use so
     * RAUW, dominance verification, and deterministic dumps see the target.
     */
    vir_function_init(&func, 64);
    assert(vir_function_set_pointer_bits(&func, sizeof(void *) * CHAR_BIT));
    entry = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_const_i32(&func, entry, 1);
    {
        vir_value_t *args[] = {one};
        vir_call_abi_type_t param_type = {VIR_TYPE_I32, true, false};
        vir_call_signature_t signature = {{VIR_TYPE_I32, false, false},
                                          &param_type,
                                          1,
                                          false,
                                          1,
                                          NULL,
                                          (unsigned int) -1};
        vir_call_signature_t bad_signature = signature;
        vir_function_t other_func;

        call =
            vir_call_indirect(&func, entry, entry_param, args, 1, VIR_TYPE_I32);
        assert(call && !call->callee && call->callee_value == entry_param &&
               call->callee_value_use && call->arg_count == 1);
        bad_signature.result.type = VIR_TYPE_I64;
        assert(!vir_call_set_signature(&func, call, &bad_signature));
        vir_function_init(&other_func, 16);
        assert(vir_function_set_pointer_bits(&other_func,
                                             sizeof(void *) * CHAR_BIT));
        assert(vir_block_create(&other_func));
        assert(!vir_call_set_signature(&other_func, call, &signature));
        vir_function_release(&other_func);
        assert(vir_call_set_signature(&func, call, &signature));
        assert(call->signature && call->signature->params != &param_type &&
               call->signature->params[0].is_unsigned);
        param_type.is_unsigned = false;
        assert(call->signature->params[0].is_unsigned);
        param_type.is_unsigned = true;
        assert(vir_replace_all_uses(&func, entry_param, param));
        assert(call->callee_value == param);
        assert(vir_block_set_return(&func, entry, call->result));
        assert(vir_verify(&func, &error));
        {
            vir_use_t fake_callee_use = *call->callee_value_use;
            vir_use_t *callee_use = call->callee_value_use;

            call->callee_value_use = &fake_callee_use;
            assert(!vir_verify(&func, &error));
            call->callee_value_use = callee_use;
        }
        call->callee_value = NULL;
        assert(!vir_verify(&func, &error));
        call->callee_value = param;
        assert(vir_verify(&func, &error));
        {
            vir_call_signature_t *saved_signature =
                (vir_call_signature_t *) call->signature;
            vir_call_abi_type_t *saved_params =
                (vir_call_abi_type_t *) saved_signature->params;
            vir_type_t saved_type = saved_params[0].type;

            saved_params[0].type = VIR_TYPE_I64;
            assert(!vir_verify(&func, &error));
            saved_params[0].type = saved_type;
            assert(vir_verify(&func, &error));
        }
    }
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "%d3 = call.i32 %d1(%d2) sig(i32.unsigned)->i32"));
    fclose(out);
    vir_function_release(&func);

    /* A C _Bool parameter/result has an i8 ABI slot while its canonical VIR
     * value is i1. Keep both identities explicit in the attached signature.
     */
    vir_function_init(&func, 32);
    assert(vir_function_set_pointer_bits(&func, sizeof(void *) * CHAR_BIT));
    entry = vir_block_create(&func);
    entry_param = vir_block_add_param(&func, entry, VIR_TYPE_PTR);
    one = vir_const_int(&func, entry, VIR_TYPE_I1, 1);
    {
        vir_value_t *args[] = {one};
        vir_call_abi_type_t param_type = {VIR_TYPE_I8, false, true};
        vir_call_signature_t signature = {
            {VIR_TYPE_I8, false, true}, &param_type, 1, false, 1, NULL,
            (unsigned int) -1};

        call =
            vir_call_indirect(&func, entry, entry_param, args, 1, VIR_TYPE_I1);
        assert(call && vir_call_set_signature(&func, call, &signature));
        assert(vir_block_set_return(&func, entry, call->result));
        assert(vir_verify(&func, &error));
    }
    out = tmpfile();
    assert(out);
    vir_print(&func, out);
    rewind(out);
    text_len = fread(text, 1, sizeof(text) - 1, out);
    assert(text_len > 0);
    text[text_len] = '\0';
    assert(strstr(text, "sig(i8.bool)->i8.bool"));
    fclose(out);
    vir_function_release(&func);

    /* i64 pure values retain their width through folding, comparisons, and
     * shifts. Shift counts deliberately remain i32.
     */
    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    one = vir_const_int(&func, entry, VIR_TYPE_I64, 1);
    two = vir_const_int(&func, entry, VIR_TYPE_I64, 0xffffffffULL);
    product = vir_const_int(&func, entry, VIR_TYPE_I64, 0xffffffffffffffffULL);
    param = vir_const_i32(&func, entry, 32);
    sum = vir_add(&func, entry, product, one);
    assert(sum && sum->opcode == VIR_OP_CONST && sum->type == VIR_TYPE_I64 &&
           sum->constant == 0);
    sum = vir_sub(&func, entry, one, two);
    assert(sum && sum->opcode == VIR_OP_CONST &&
           sum->constant == 0xffffffff00000002ULL);
    sum = vir_shl(&func, entry, one, param);
    assert(sum && sum->opcode == VIR_OP_CONST &&
           sum->constant == 0x100000000ULL);
    assert(!vir_shl(&func, entry, one, one));
    param = vir_const_i32(&func, entry, 63);
    sum = vir_ashr(&func, entry, product, param);
    assert(sum && sum->opcode == VIR_OP_CONST &&
           sum->constant == 0xffffffffffffffffULL);
    sum = vir_slt(&func, entry, product, one);
    assert(sum && sum->opcode == VIR_OP_CONST && sum->constant == 1);
    sum = vir_ult(&func, entry, product, one);
    assert(sum && sum->opcode == VIR_OP_CONST && sum->constant == 0);
    product = vir_block_add_param(&func, entry, VIR_TYPE_I64);
    param = vir_const_i32(&func, entry, 1);
    two = vir_shl(&func, entry, product, param);
    assert(product && param && two && param->uses && param->uses->user == two);
    {
        vir_use_t *shift_count_use = param->uses;

        param->uses = NULL;
        one->uses = shift_count_use;
        two->op1 = one;
        assert(!vir_verify(&func, &error));
        two->op1 = param;
        one->uses = NULL;
        param->uses = shift_count_use;
    }
    assert(vir_block_set_return(&func, entry, sum));
    assert(vir_verify(&func, &error));
    vir_function_release(&func);
    return 0;
}
