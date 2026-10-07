#include "../src/vir.c"

static int check_sccp_binary(vir_type_t type,
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
    char *error;
    int result = 0;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    left = entry ? vir_const_int(&func, entry, type, left_bits) : NULL;
    param = merge ? vir_block_add_param(&func, merge, type) : NULL;
    right = merge ? vir_const_int(&func, merge, type, right_bits) : NULL;
    value =
        param && right
            ? vir_binary(&func, merge, opcode,
                         opcode == VIR_OP_EQ ? VIR_TYPE_I1 : type, param, right)
            : NULL;
    if (!entry || !merge || !left || !param || !right || !value ||
        !vir_edge_create(&func, entry, merge, &left, 1) ||
        !vir_block_set_return(&func, merge, value) ||
        !vir_verify(&func, &error) || vir_sccp(&func, VIR_OPT_O1) <= 0 ||
        merge->return_value->opcode != VIR_OP_CONST ||
        merge->return_value->type !=
            (opcode == VIR_OP_EQ ? VIR_TYPE_I1 : type) ||
        merge->return_value->constant != expected || !vir_verify(&func, &error))
        result = 1;
    vir_function_release(&func);
    return result;
}

static int check_sccp_unary(vir_type_t type,
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
    int result = 0;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    operand = entry ? vir_const_int(&func, entry, type, operand_bits) : NULL;
    param = merge ? vir_block_add_param(&func, merge, type) : NULL;
    value = param ? (opcode == VIR_OP_NEG ? vir_neg(&func, merge, param)
                                          : vir_bitnot(&func, merge, param))
                  : NULL;
    if (!entry || !merge || !operand || !param || !value ||
        !vir_edge_create(&func, entry, merge, &operand, 1) ||
        !vir_block_set_return(&func, merge, value) ||
        !vir_verify(&func, &error) || vir_sccp(&func, VIR_OPT_O1) <= 0 ||
        merge->return_value->opcode != VIR_OP_CONST ||
        merge->return_value->type != type ||
        merge->return_value->constant != expected || !vir_verify(&func, &error))
        result = 1;
    vir_function_release(&func);
    return result;
}

static int check_sccp_cast(vir_type_t input_type,
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
    int result = 0;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    operand =
        entry ? vir_const_int(&func, entry, input_type, operand_bits) : NULL;
    param = merge ? vir_block_add_param(&func, merge, input_type) : NULL;
    value = param ? (opcode == VIR_OP_ZEXT && input_type == VIR_TYPE_I1
                         ? vir_zext_i1(&func, merge, param)
                     : opcode == VIR_OP_ZEXT
                         ? vir_zext(&func, merge, param, result_type)
                     : opcode == VIR_OP_SEXT
                         ? vir_sext(&func, merge, param, result_type)
                         : vir_trunc(&func, merge, param, result_type))
                  : NULL;
    if (!entry || !merge || !operand || !param || !value ||
        !vir_edge_create(&func, entry, merge, &operand, 1) ||
        !vir_block_set_return(&func, merge, value) ||
        !vir_verify(&func, &error) || vir_sccp(&func, VIR_OPT_O1) <= 0 ||
        merge->return_value->opcode != VIR_OP_CONST ||
        merge->return_value->type != result_type ||
        merge->return_value->constant != expected || !vir_verify(&func, &error))
        result = 1;
    vir_function_release(&func);
    return result;
}

static int check_signed_overflow_fold(vir_type_t type, unsigned long long min)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_value_t *minimum;
    vir_value_t *minus_one;
    vir_value_t *quotient;
    vir_value_t *remainder;
    int result;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    minimum = entry ? vir_const_int(&func, entry, type, min) : NULL;
    minus_one = entry ? vir_const_int(&func, entry, type, ~0ULL) : NULL;
    quotient = minimum && minus_one
                   ? vir_div(&func, entry, minimum, minus_one, false, false)
                   : NULL;
    remainder = minimum && minus_one
                    ? vir_div(&func, entry, minimum, minus_one, false, true)
                    : NULL;
    result = !entry || !minimum || !minus_one || !quotient || !remainder ||
             quotient->opcode != VIR_OP_SDIV ||
             remainder->opcode != VIR_OP_SREM;
    vir_function_release(&func);
    return result;
}

int main(void)
{
    vir_function_t func;
    vir_block_t *entry;
    vir_block_t *merge;
    vir_value_t *one;
    vir_value_t *two;
    vir_value_t *condition;
    vir_value_t *param;
    vir_edge_args_t true_args;
    vir_edge_args_t false_args;
    vir_ssa_t *ssa;

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    two = vir_const_i32(&func, entry, 2);
    condition = vir_const_i1(&func, entry, 1);
    param = vir_block_add_param(&func, merge, VIR_TYPE_I32);
    true_args.to = merge;
    true_args.args = &one;
    true_args.arg_count = 1;
    false_args.to = merge;
    false_args.args = &two;
    false_args.arg_count = 1;
    if (!entry)
        return 11;
    if (!merge)
        return 12;
    if (!one)
        return 13;
    if (!two)
        return 14;
    if (!condition)
        return 15;
    if (vir_slt(&func, entry, one, two)->constant != 1 ||
        vir_ult(&func, entry, two, one)->constant != 0)
        return 21;
    if (!param)
        return 16;
    if (!vir_block_set_branch(&func, entry, condition, &true_args, &false_args))
        return 17;
    if (!vir_block_set_return(&func, merge, param))
        return 19;
    if (!vir_edge_delete(&func, entry->true_edge))
        return 18;
    if (entry->terminator != VIR_TERM_JUMP || entry->branch_condition ||
        entry->true_edge || entry->false_edge || !entry->outgoing ||
        entry->outgoing->to != merge || entry->outgoing->args[0] != two ||
        condition->uses || one->uses)
        return 20;
    vir_function_release(&func);

    vir_function_init(&func, 64);
    entry = vir_block_create(&func);
    merge = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 1);
    ssa = vir_ssa_create(&func);
    if (!entry || !merge || !one || !ssa)
        return 31;
    if (!vir_ssa_write(ssa, entry, 0, one))
        return 32;
    if (!vir_ssa_predeclare(ssa, merge, 0, VIR_TYPE_I32))
        return 33;
    if (!vir_ssa_jump(ssa, entry, merge))
        return 34;
    if (!vir_ssa_seal(ssa, merge))
        return 35;
    if (!vir_block_set_return(&func, merge, one))
        return 36;
    if (merge->param_count != 0 || vir_ssa_read(ssa, merge, 0) != one)
        return 37;
    vir_ssa_release(ssa);
    vir_function_release(&func);

    vir_function_init(&func, 64);
    if (!vir_function_set_pointer_bits(&func, 32))
        return 41;
    entry = vir_block_create(&func);
    one = vir_const_i32(&func, entry, 0x12345678);
    param = vir_inttoptr(&func, entry, one);
    two = vir_ptrtoint(&func, entry, param, VIR_TYPE_I32);
    condition = vir_ptradd(&func, entry, param, two);
    vir_value_t *loaded = vir_load(&func, entry, param, VIR_TYPE_I32);
    vir_value_t *stack = vir_stack_addr(&func, entry, 1, 4, 4);
    vir_value_t *global = vir_global_addr(&func, entry, "stage0", 4, 4);
    vir_effect_t *call =
        vir_call(&func, entry, "stage0_call", &two, 1, VIR_TYPE_I32);
    vir_value_t *volatile_loaded =
        vir_volatile_load(&func, entry, param, VIR_TYPE_I32);
    if (!loaded || !stack || !global || !call || !call->result ||
        !volatile_loaded || !vir_store(&func, entry, param, two) ||
        !vir_volatile_store(&func, entry, param, two))
        return 47;
    if (!entry || !one || !param || !two || param->constant != 0x12345678 ||
        two->constant != 0x12345678 || !condition ||
        condition->constant != 0x2468acf0)
        return 42;
    if (!vir_block_set_return(&func, entry, loaded))
        return 43;
    vir_function_release(&func);

    vir_function_init(&func, 64);
    if (!vir_function_set_pointer_bits(&func, 64))
        return 44;
    entry = vir_block_create(&func);
    one = vir_const_int(&func, entry, VIR_TYPE_I64, 0x123456789abcdef0ULL);
    param = vir_inttoptr(&func, entry, one);
    two = vir_ptrtoint(&func, entry, param, VIR_TYPE_I64);
    stack = vir_stack_addr(&func, entry, 2, 8, 8);
    global = vir_global_addr(&func, entry, "stage0-64", 8, 8);
    if (!entry || !one || !param || !two ||
        param->constant != 0x123456789abcdef0ULL ||
        two->constant != 0x123456789abcdef0ULL || !stack || !global)
        return 45;
    if (!vir_block_set_return(&func, entry, param))
        return 46;
    vir_function_release(&func);

    if (check_sccp_binary(VIR_TYPE_I32, VIR_OP_SDIV, 0xfffffff9ULL, 3,
                          0xfffffffeULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_SREM, 0xfffffff9ULL, 3,
                          0xffffffffULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_SDIV, 7, 0xfffffffdULL,
                          0xfffffffeULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_SREM, 7, 0xfffffffdULL, 1) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_SDIV, 0xfffffff9ULL,
                          0xfffffffdULL, 2) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_SREM, 0xfffffff9ULL,
                          0xfffffffdULL, 0xffffffffULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_UDIV, 0xffffffffULL, 3,
                          0x55555555ULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_UREM, 0xffffffffULL, 3, 0) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_BITAND, 0x8000000fULL,
                          0xffffffffULL, 0x8000000fULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_BITOR, 0x80000000ULL, 0x0fULL,
                          0x8000000fULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_BITXOR, 0x80000000ULL,
                          0xffffffffULL, 0x7fffffffULL) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_EQ, 0x80000000ULL, 0x80000000ULL,
                          1) ||
        check_sccp_binary(VIR_TYPE_I32, VIR_OP_EQ, 0x80000000ULL, 1, 0) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SDIV, 0xfffffffffffffff9ULL, 3,
                          0xfffffffffffffffeULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SREM, 0xfffffffffffffff9ULL, 3,
                          0xffffffffffffffffULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SDIV, 7, 0xfffffffffffffffdULL,
                          0xfffffffffffffffeULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SREM, 7, 0xfffffffffffffffdULL,
                          1) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SDIV, 0xfffffffffffffff9ULL,
                          0xfffffffffffffffdULL, 2) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_SREM, 0xfffffffffffffff9ULL,
                          0xfffffffffffffffdULL, 0xffffffffffffffffULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_UDIV, 0xffffffffffffffffULL, 3,
                          0x5555555555555555ULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_UREM, 0xffffffffffffffffULL, 3,
                          0) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_BITAND, 0x800000000000000fULL,
                          ~0ULL, 0x800000000000000fULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_BITOR, 0x8000000000000000ULL,
                          0x0fULL, 0x800000000000000fULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_BITXOR, 0x8000000000000000ULL,
                          ~0ULL, 0x7fffffffffffffffULL) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_EQ, 0x8000000000000000ULL,
                          0x8000000000000000ULL, 1) ||
        check_sccp_binary(VIR_TYPE_I64, VIR_OP_EQ, 0x8000000000000000ULL, 1,
                          0) ||
        check_sccp_unary(VIR_TYPE_I32, VIR_OP_NEG, 0x80000000ULL,
                         0x80000000ULL) ||
        check_sccp_unary(VIR_TYPE_I32, VIR_OP_NEG, 0x80000001ULL,
                         0x7fffffffULL) ||
        check_sccp_unary(VIR_TYPE_I32, VIR_OP_NEG, 1, 0xffffffffULL) ||
        check_sccp_unary(VIR_TYPE_I32, VIR_OP_BITNOT, 0x80000000ULL,
                         0x7fffffffULL) ||
        check_sccp_unary(VIR_TYPE_I64, VIR_OP_NEG, 0x8000000000000000ULL,
                         0x8000000000000000ULL) ||
        check_sccp_unary(VIR_TYPE_I64, VIR_OP_NEG, 0x8000000000000001ULL,
                         0x7fffffffffffffffULL) ||
        check_sccp_unary(VIR_TYPE_I64, VIR_OP_NEG, 1, ~0ULL) ||
        check_sccp_unary(VIR_TYPE_I64, VIR_OP_BITNOT, 0x8000000000000000ULL,
                         0x7fffffffffffffffULL) ||
        check_sccp_cast(VIR_TYPE_I8, VIR_OP_ZEXT, VIR_TYPE_I32, 0xffULL,
                        0xffULL) ||
        check_sccp_cast(VIR_TYPE_I1, VIR_OP_ZEXT, VIR_TYPE_I32, 1, 1) ||
        check_sccp_cast(VIR_TYPE_I1, VIR_OP_ZEXT, VIR_TYPE_I32, 0, 0) ||
        check_sccp_cast(VIR_TYPE_I8, VIR_OP_SEXT, VIR_TYPE_I32, 0xffULL,
                        0xffffffffULL) ||
        check_sccp_cast(VIR_TYPE_I16, VIR_OP_ZEXT, VIR_TYPE_I32, 0xffffULL,
                        0xffffULL) ||
        check_sccp_cast(VIR_TYPE_I16, VIR_OP_SEXT, VIR_TYPE_I32, 0xffffULL,
                        0xffffffffULL) ||
        check_sccp_cast(VIR_TYPE_I32, VIR_OP_ZEXT, VIR_TYPE_I64, 0x80000000ULL,
                        0x80000000ULL) ||
        check_sccp_cast(VIR_TYPE_I32, VIR_OP_SEXT, VIR_TYPE_I64, 0x80000000ULL,
                        0xffffffff80000000ULL) ||
        check_sccp_cast(VIR_TYPE_I64, VIR_OP_TRUNC, VIR_TYPE_I32,
                        0x1234567887654321ULL, 0x87654321ULL) ||
        check_signed_overflow_fold(VIR_TYPE_I32, 0x80000000ULL) ||
        check_signed_overflow_fold(VIR_TYPE_I64, 0x8000000000000000ULL))
        return 51;
    return 0;
}
