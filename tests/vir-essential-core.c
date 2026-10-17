#include "vir.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Block parameters bound by run_function(), newest last. */
static vir_value_t *bound_param[64];
static unsigned long long bound_bits[64];
static int bound_count;

static unsigned long long evaluate(vir_value_t *value,
                                   vir_value_t *input,
                                   unsigned long long bits)
{
    if (value == input)
        return bits;
    if (value->is_block_param) {
        for (int i = bound_count - 1; i >= 0; i--)
            if (bound_param[i] == value)
                return bound_bits[i];
        assert(0);
    }
    if (value->opcode == VIR_OP_CONST)
        return value->constant;
    unsigned long long left = evaluate(value->op0, input, bits);
    unsigned long long right =
        value->op1 ? evaluate(value->op1, input, bits) : 0;
    unsigned long long mask =
        value->type == VIR_TYPE_I32 ? 0xffffffffULL : ~0ULL;
    int width = value->type == VIR_TYPE_I32 ? 32 : 64;
    unsigned long long result = 0;
    switch (value->opcode) {
    case VIR_OP_ADD:
        result = left + right;
        break;
    case VIR_OP_SUB:
        result = left - right;
        break;
    case VIR_OP_NEG:
        result = 0 - left;
        break;
    case VIR_OP_SLT: {
        unsigned long long sign = value->op0->type == VIR_TYPE_I32
                                      ? 0x80000000ULL
                                      : 0x8000000000000000ULL;
        return (left ^ sign) < (right ^ sign);
    }
    case VIR_OP_MUL:
        result = left * right;
        break;
    case VIR_OP_BITAND:
        result = left & right;
        break;
    case VIR_OP_SHL:
        result = left << right;
        break;
    case VIR_OP_LSHR:
        result = left >> right;
        break;
    case VIR_OP_ASHR:
        result = left >> right;
        if (right && (left & (1ULL << (width - 1))))
            result |= mask ^ (mask >> right);
        break;
    default:
        assert(0);
    }
    return result & mask;
}

static void test_affine_loops(void)
{
    for (int test = 0; test < 200; test++) {
        vir_function_t f;
        char *error = NULL;
        vir_function_init(&f, 4096);
        assert(vir_function_set_pointer_bits(&f, 64));
        vir_type_t type = test & 1 ? VIR_TYPE_I32 : VIR_TYPE_I64;
        unsigned long long mask = type == VIR_TYPE_I32 ? 0xffffffffULL : ~0ULL;
        unsigned long long first = test % 7, step = test % 5 + 1;
        unsigned long long limit = first + test % 31 + 1;
        unsigned long long a = test % 8 == 0 ? mask : (unsigned int) test % 4;
        unsigned long long b =
            test % 9 == 0 ? mask - 6 : (unsigned int) test % 7;
        unsigned long long c = test % 11 == 0 ? mask : (unsigned int) test % 13;
        bool extreme = test >= 192;
        bool signed_index = extreme ? (test & 2) != 0 : (test & 4) != 0;
        if (extreme) {
            first = 0;
            step = 1;
            limit = signed_index ? mask >> 1 : mask;
            a = c = 1;
            b = 0;
        }
        vir_block_t *entry = vir_block_create(&f);
        vir_block_t *header = vir_block_create(&f);
        vir_block_t *body = vir_block_create(&f);
        vir_block_t *latch = vir_block_create(&f);
        vir_block_t *exit = vir_block_create(&f);
        vir_value_t *input = vir_block_add_param(&f, entry, type);
        vir_value_t *iv, *acc;
        int iv_index = test & 2 ? 1 : 0;
        vir_value_t *params[2];
        for (int i = 0; i < 2; i++)
            params[i] = vir_block_add_param(&f, header, type);
        iv = params[iv_index];
        acc = params[1 - iv_index];
        vir_value_t *args[2];
        args[iv_index] = vir_const_int(&f, entry, type, first);
        args[1 - iv_index] = input;
        assert(vir_edge_create(&f, entry, header, args, 2));
        vir_value_t *bound = vir_const_int(&f, header, type, limit);
        vir_value_t *condition = signed_index ? vir_slt(&f, header, iv, bound)
                                              : vir_ult(&f, header, iv, bound);
        vir_edge_args_t yes = {body, NULL, 0}, no = {exit, NULL, 0};
        assert(vir_block_set_branch(&f, header, condition, &yes, &no));
        vir_value_t *scale = vir_const_int(&f, body, type, a);
        vir_value_t *factor = vir_const_int(&f, body, type, b);
        vir_value_t *bias = vir_const_int(&f, body, type, c);
        vir_value_t *product = vir_mul(&f, body, acc, scale);
        vir_value_t *index = vir_mul(&f, body, iv, factor);
        vir_value_t *sum = vir_add(&f, body, product, index);
        vir_value_t *next = vir_add(&f, body, sum, bias);
        assert(vir_edge_create(&f, body, latch, NULL, 0));
        args[iv_index] =
            vir_add(&f, latch, iv, vir_const_int(&f, latch, type, step));
        args[1 - iv_index] = next;
        assert(vir_edge_create(&f, latch, header, args, 2));
        assert(vir_block_set_return(&f, exit, acc));
        assert(vir_verify(&f, &error));
        assert(vir_summarize_affine_loops(&f, VIR_OPT_O0) == 0);
        assert(vir_summarize_affine_loops(&f, VIR_OPT_O2) == 1);
        assert(vir_verify(&f, &error));
        assert(vir_simplify_params(&f, VIR_OPT_O2) >= 0);
        assert(vir_dce(&f, VIR_OPT_O2) >= 0);
        assert(vir_verify(&f, &error));
        for (int seed = 0; seed < 16; seed++) {
            unsigned long long initial = (mask - seed * 7919ULL) & mask;
            unsigned long long expected = initial;
            if (extreme)
                expected = (initial + limit) & mask;
            else
                for (unsigned long long i = first; i < limit; i += step)
                    expected = (expected * a + b * i + c) & mask;
            assert(evaluate(exit->return_value, input, initial) == expected);
        }
        assert(f.block_count == 3);
        vir_function_release(&f);
    }
}

/* Every mode changes one proof obligation of an otherwise eligible loop. */
static void test_affine_rejections(void)
{
    for (int mode = 0; mode < 10; mode++) {
        vir_function_t f;
        char *error = NULL;
        vir_function_init(&f, 4096);
        assert(vir_function_set_pointer_bits(&f, 64));
        vir_block_t *entry = vir_block_create(&f);
        vir_block_t *header = vir_block_create(&f);
        vir_block_t *body = vir_block_create(&f);
        vir_block_t *exit = vir_block_create(&f);
        vir_value_t *pointer = vir_block_add_param(&f, entry, VIR_TYPE_PTR);
        vir_value_t *iv = vir_block_add_param(&f, header, VIR_TYPE_I32);
        vir_value_t *acc = vir_block_add_param(&f, header, VIR_TYPE_I32);
        vir_value_t *one = vir_const_i32(&f, entry, 1);
        vir_value_t *first = vir_const_i32(&f, entry,
                                           mode == 5   ? -2
                                           : mode == 8 ? 1
                                                       : 0);
        vir_value_t *args[2] = {first, one};
        assert(vir_edge_create(&f, entry, header, args, 2));
        vir_value_t *bound = vir_const_i32(&f, header, mode == 5 ? -1 : 10);
        vir_value_t *condition = mode == 6 ? vir_slt(&f, header, first, bound)
                                           : vir_ult(&f, header, iv, bound);
        vir_edge_args_t yes = {body, NULL, 0}, no = {exit, NULL, 0};
        assert(vir_block_set_branch(&f, header, condition, &yes, &no));
        vir_value_t *next = vir_add(&f, body, acc, iv);
        if (mode == 0)
            assert(vir_store(&f, body, pointer, next));
        if (mode == 1)
            assert(vir_load(&f, body, pointer, VIR_TYPE_I32));
        if (mode == 2)
            assert(vir_volatile_load(&f, body, pointer, VIR_TYPE_I32));
        if (mode == 3)
            assert(vir_call(&f, body, "other", NULL, 0, VIR_TYPE_VOID));
        if (mode == 4)
            next = vir_mul(&f, body, acc, acc);
        args[0] =
            vir_add(&f, body, iv, mode == 5 ? vir_const_i32(&f, body, 2) : one);
        args[1] = next;
        if (mode == 7)
            args[0] = iv;
        if (mode == 8)
            args[0] = vir_sub(&f, body, iv, one);
        if (mode == 9) {
            for (int i = 0; i < 20; i++)
                next = vir_add(&f, body, next, next);
            args[1] = next;
        }
        assert(vir_edge_create(&f, body, header, args, 2));
        assert(vir_block_set_return(&f, exit, acc));
        assert(vir_verify(&f, &error));
        assert(vir_summarize_affine_loops(&f, VIR_OPT_O2) == 0);
        assert(vir_verify(&f, &error));
        vir_function_release(&f);
    }
}

static void test_tail_recursion(void)
{
    for (int mode = 0; mode < 10; mode++) {
        vir_function_t f;
        char *error = NULL;
        vir_function_init(&f, 4096);
        assert(vir_function_set_pointer_bits(&f, 64));
        vir_block_t *entry = vir_block_create(&f);
        vir_block_t *base = vir_block_create(&f);
        vir_block_t *body = vir_block_create(&f);
        vir_value_t *n = vir_block_add_param(&f, entry, VIR_TYPE_I32);
        vir_value_t *small =
            mode == 7 ? vir_block_add_param(&f, entry, VIR_TYPE_I8) : NULL;
        vir_value_t *one = vir_const_i32(&f, entry, 1);
        vir_value_t *condition = vir_ult(&f, entry, n, one);
        vir_edge_args_t yes = {base, NULL, 0}, no = {body, NULL, 0};
        assert(vir_block_set_branch(&f, entry, condition, &yes, &no));
        assert(vir_block_set_return(&f, base, n));
        vir_value_t *args[2] = {vir_sub(&f, body, n, one), small};
        int argc = small ? 2 : 1;
        vir_effect_t *call =
            vir_call(&f, body, "recursive", args, argc, VIR_TYPE_I32);
        assert(call);
        vir_value_t *result = call->result;
        if (mode == 2) {
            vir_effect_t *second =
                vir_call(&f, body, "recursive", args, argc, VIR_TYPE_I32);
            assert(second);
            result = vir_add(&f, body, result, second->result);
        } else if (mode == 3) {
            result = vir_add(&f, body, result, result);
        } else if (mode != 0) {
            result = vir_add(&f, body, result, n);
        }
        if (mode == 4) {
            vir_value_t *global = vir_global_addr(&f, body, "visible", 4, 4);
            assert(vir_store(&f, body, global, one));
        }
        if (mode == 5)
            assert(vir_stack_addr(&f, body, 0, 4, 4));
        if (mode == 6)
            assert(vir_call(&f, body, "external", NULL, 0, VIR_TYPE_VOID));
        if (mode == 8)
            assert(vir_bitnot(&f, body, call->result));
        if (mode == 9) {
            vir_call_abi_type_t param = {VIR_TYPE_I32, false, false};
            vir_call_signature_t signature = {0};
            signature.result.type = VIR_TYPE_I32;
            signature.params = &param;
            signature.param_count = signature.fixed_param_count = 1;
            signature.is_variadic = true;
            signature.va_start_slot = (unsigned int) -1;
            assert(vir_call_set_signature(&f, call, &signature));
        }
        assert(vir_block_set_return(&f, body, result));
        assert(vir_verify(&f, &error));
        assert(vir_eliminate_tail_recursion(&f, "recursive", VIR_OPT_O0) == 0);
        int transformed =
            vir_eliminate_tail_recursion(&f, "recursive", VIR_OPT_O2);
        assert(transformed == (mode < 3 ? 1 : 0));
        assert(vir_verify(&f, &error));
        if (transformed) {
            assert(f.blocks != entry && f.blocks->param_count == 1);
            assert(entry->param_count == 2 && entry->incoming &&
                   entry->incoming->next_incoming);
            assert(body->terminator == VIR_TERM_JUMP);
            assert((body->effects != NULL) == (mode == 2));
            assert(vir_dce(&f, VIR_OPT_O2) >= 0);
            assert(vir_verify(&f, &error));
        }
        vir_function_release(&f);
    }
}

/* Run @func from its entry, whose first parameter is @input, through jumps and
 * branches to the value it returns.
 */
static unsigned long long run_function(vir_function_t *func,
                                       unsigned long long bits)
{
    vir_block_t *block = func->blocks;
    vir_value_t *input = block->params;
    bound_count = 0;
    for (int steps = 0; steps < 32; steps++) {
        if (block->terminator == VIR_TERM_RETURN)
            return evaluate(block->return_value, input, bits);
        vir_edge_t *edge = block->outgoing;
        if (block->terminator == VIR_TERM_BRANCH)
            edge = evaluate(block->branch_condition, input, bits)
                       ? block->true_edge
                       : block->false_edge;
        unsigned long long args[8];
        assert(edge->arg_count <= 8);
        for (int i = 0; i < edge->arg_count; i++)
            args[i] = evaluate(edge->args[i], input, bits);
        int i = 0;
        for (vir_value_t *param = edge->to->params; param;
             param = param->param_next) {
            assert(bound_count < 64);
            bound_param[bound_count] = param;
            bound_bits[bound_count++] = args[i++];
        }
        block = edge->to;
    }
    assert(0);
    return 0;
}

static const vir_function_t *inline_target;
static bool inline_static = true;

static const vir_function_t *inline_lookup(const char *name,
                                           void *context,
                                           bool *is_static)
{
    (void) context;
    *is_static = inline_static;
    return !strcmp(name, "leaf") ? inline_target : NULL;
}

static int count_effects(const vir_function_t *func, vir_effect_kind_t kind)
{
    int count = 0;
    for (const vir_block_t *block = func->blocks; block; block = block->next)
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next)
            count += effect->kind == kind;
    return count;
}

/* Start a caller and a "leaf" callee whose entry @source takes i32 @x and y and
 * computes the returned 3 * x + y.
 */
static vir_value_t *start_leaf(vir_function_t *caller,
                               vir_function_t *callee,
                               vir_block_t **source,
                               vir_value_t **x)
{
    vir_function_init(caller, 4096);
    vir_function_init(callee, 4096);
    assert(vir_function_set_pointer_bits(caller, 64));
    assert(vir_function_set_pointer_bits(callee, 64));
    *source = vir_block_create(callee);
    *x = vir_block_add_param(callee, *source, VIR_TYPE_I32);
    vir_value_t *y = vir_block_add_param(callee, *source, VIR_TYPE_I32);
    vir_value_t *three = vir_const_i32(callee, *source, 3);
    return vir_add(callee, *source, vir_mul(callee, *source, *x, three), y);
}

/* Modes: 0 pure, 1 stores a global, 2 too large, 3 two blocks, 4 argument type
 * mismatch, 5 one value passed twice, 6 two returns, 7 calls out.
 */
static void test_call_inlining(void)
{
    for (int mode = 0; mode < 8; mode++) {
        vir_function_t caller, callee;
        char *error = NULL;
        vir_block_t *source;
        vir_value_t *x;
        vir_value_t *sum = start_leaf(&caller, &callee, &source, &x);
        if (mode == 1) {
            vir_value_t *address =
                vir_global_addr(&callee, source, "visible", 4, 4);
            assert(vir_store(&callee, source, address, x));
        }
        if (mode == 2)
            for (int i = 0; i < 45; i++)
                sum = vir_add(&callee, source, sum, x);
        if (mode == 3) {
            vir_block_t *exit = vir_block_create(&callee);
            assert(vir_edge_create(&callee, source, exit, NULL, 0));
            source = exit;
        }
        if (mode == 7)
            assert(vir_call(&callee, source, "other", NULL, 0, VIR_TYPE_VOID));
        if (mode == 6) {
            vir_block_t *negative = vir_block_create(&callee);
            vir_block_t *positive = vir_block_create(&callee);
            vir_value_t *zero = vir_const_i32(&callee, source, 0);
            vir_edge_args_t then_edge = {negative, NULL, 0};
            vir_edge_args_t else_edge = {positive, NULL, 0};
            assert(vir_block_set_branch(&callee, source,
                                        vir_slt(&callee, source, x, zero),
                                        &then_edge, &else_edge));
            assert(vir_block_set_return(&callee, negative,
                                        vir_neg(&callee, negative, x)));
            assert(vir_block_set_return(&callee, positive, x));
        } else {
            assert(vir_block_set_return(&callee, source, sum));
        }
        assert(vir_verify(&callee, &error));
        inline_target = &callee;

        vir_block_t *entry = vir_block_create(&caller);
        vir_value_t *input = vir_block_add_param(&caller, entry, VIR_TYPE_I32);
        vir_value_t *seven = vir_const_i32(&caller, entry, 7);
        vir_value_t *args[2] = {input, seven};
        if (mode == 4)
            args[0] = vir_const_int(&caller, entry, VIR_TYPE_I64, 1);
        if (mode == 5)
            args[1] = input;
        vir_effect_t *call =
            vir_call(&caller, entry, "leaf", args, 2, VIR_TYPE_I32);
        assert(call && call->result);
        vir_value_t *one = vir_const_i32(&caller, entry, 1);
        assert(vir_block_set_return(
            &caller, entry, vir_add(&caller, entry, call->result, one)));
        assert(vir_verify(&caller, &error));
        assert(vir_inline_calls(&caller, inline_lookup, NULL, VIR_OPT_O0) == 0);
        assert(count_effects(&caller, VIR_EFFECT_CALL) == 1);

        bool expected = mode != 2 && mode != 4 && mode != 7;
        assert(vir_inline_calls(&caller, inline_lookup, NULL, VIR_OPT_O2) ==
               expected);
        assert(vir_verify(&caller, &error));
        assert(count_effects(&caller, VIR_EFFECT_CALL) == !expected);
        for (int i = 0; expected && i < 16; i++) {
            unsigned long long bits = 0xffffffffULL - 17 * i;
            unsigned long long want = 3 * bits + (mode == 5 ? bits : 7);
            if (mode == 6)
                want = bits & 0x80000000ULL ? 0 - bits : bits;
            assert(run_function(&caller, bits) == ((want + 1) & 0xffffffffULL));
        }
        if (mode == 1)
            assert(count_effects(&caller, VIR_EFFECT_STORE) == 1);
        vir_function_release(&caller);
        vir_function_release(&callee);
    }
}

static void test_integer_strength_reduction(void)
{
    vir_opcode_t opcodes[] = {VIR_OP_MUL, VIR_OP_UDIV, VIR_OP_UREM, VIR_OP_SDIV,
                              VIR_OP_SREM};
    for (int wide = 0; wide < 2; wide++) {
        int width = wide ? 64 : 32;
        int shifts[] = {1, 2, 3, 4, 7, width - 2, width - 1};
        vir_type_t type = wide ? VIR_TYPE_I64 : VIR_TYPE_I32;
        unsigned long long mask = wide ? ~0ULL : 0xffffffffULL;
        for (int op = 0; op < 5; op++) {
            for (int k = 0; k < 7; k++) {
                int shift = shifts[k];
                if (op >= 3 && shift == width - 1)
                    continue;
                vir_function_t f;
                char *error = NULL;
                vir_function_init(&f, 4096);
                assert(vir_function_set_pointer_bits(&f, 64));
                vir_block_t *entry = vir_block_create(&f);
                vir_value_t *input = vir_block_add_param(&f, entry, type);
                vir_value_t *pointer =
                    vir_block_add_param(&f, entry, VIR_TYPE_PTR);
                assert(vir_volatile_load(&f, entry, pointer, type));
                unsigned long long divisor = 1ULL << shift;
                vir_value_t *constant = vir_const_int(&f, entry, type, divisor);
                vir_value_t *result =
                    vir_binary(&f, entry, opcodes[op], type, input, constant);
                assert(vir_block_set_return(&f, entry, result));
                assert(vir_verify(&f, &error));
                assert(vir_normalize(&f, VIR_OPT_O0) == 0);
                assert(vir_normalize(&f, VIR_OPT_O2) == 1);
                assert(vir_verify(&f, &error));
                unsigned long long boundaries[] = {0,
                                                   1,
                                                   divisor - 1,
                                                   divisor,
                                                   divisor + 1,
                                                   mask,
                                                   mask - divisor,
                                                   1ULL << (width - 1),
                                                   (1ULL << (width - 1)) - 1,
                                                   (1ULL << (width - 1)) + 1};
                unsigned long long random = 0x123456789abcdef0ULL;
                for (int sample = 0; sample < 138; sample++) {
                    random = random * 6364136223846793005ULL + 1;
                    unsigned long long bits =
                        sample < 10 ? boundaries[sample] & mask : random & mask;
                    unsigned long long expected;
                    if (op == 0)
                        expected = (bits * divisor) & mask;
                    else if (op == 1)
                        expected = bits / divisor;
                    else if (op == 2)
                        expected = bits % divisor;
                    else {
                        long long signed_value =
                            wide ? (long long) bits : (int) (unsigned int) bits;
                        long long signed_divisor = (long long) divisor;
                        expected =
                            (unsigned long long) (op == 3
                                                      ? signed_value /
                                                            signed_divisor
                                                      : signed_value %
                                                            signed_divisor) &
                            mask;
                    }
                    assert(evaluate(entry->return_value, input, bits) ==
                           expected);
                }
                assert(vir_dce(&f, VIR_OPT_O2) >= 0);
                assert(entry->effects &&
                       entry->effects->kind == VIR_EFFECT_VOLATILE_LOAD);
                assert(vir_verify(&f, &error));
                vir_function_release(&f);
            }
        }
    }
}

static void test_integer_strength_rejections(void)
{
    for (int wide = 0; wide < 2; wide++) {
        for (int mode = 0; mode < 6; mode++) {
            vir_function_t f;
            char *error = NULL;
            vir_function_init(&f, 4096);
            vir_type_t type = wide ? VIR_TYPE_I64 : VIR_TYPE_I32;
            unsigned long long mask = wide ? ~0ULL : 0xffffffffULL;
            unsigned long long minimum = wide ? 1ULL << 63 : 1ULL << 31;
            vir_block_t *entry = vir_block_create(&f);
            vir_value_t *input = vir_block_add_param(&f, entry, type);
            if (mode >= 4)
                input = vir_const_int(&f, entry, type, minimum);
            unsigned long long bits = mode == 0   ? 0
                                      : mode == 1 ? mask
                                      : mode == 2 ? minimum
                                      : mode == 3 ? mask - 1
                                                  : mask;
            vir_value_t *divisor = vir_const_int(&f, entry, type, bits);
            vir_value_t *result =
                vir_binary(&f, entry, mode == 5 ? VIR_OP_SREM : VIR_OP_SDIV,
                           type, input, divisor);
            assert(vir_block_set_return(&f, entry, result));
            assert(vir_normalize(&f, VIR_OPT_O2) == 0);
            assert(entry->return_value == result);
            assert(vir_verify(&f, &error));
            vir_function_release(&f);
        }
    }
}

/* An extern callee of 3 + @extra values, called once outside any loop or from a
 * block that branches back to itself. Only the tiny one, or the loop call of
 * the larger one, is copied.
 */
static void test_extern_inlining(void)
{
    for (int mode = 0; mode < 4; mode++) {
        int extra = mode & 1 ? 5 : 0;
        bool loop = mode & 2;
        vir_function_t caller, callee;
        char *error = NULL;
        vir_block_t *source;
        vir_value_t *x;
        vir_value_t *sum = start_leaf(&caller, &callee, &source, &x);
        for (int i = 0; i < extra; i++)
            sum = vir_add(&callee, source, sum, x);
        assert(vir_block_set_return(&callee, source, sum));
        assert(vir_verify(&callee, &error));
        inline_target = &callee;
        inline_static = false;

        vir_block_t *entry = vir_block_create(&caller);
        vir_block_t *body = vir_block_create(&caller);
        vir_block_t *exit = vir_block_create(&caller);
        vir_value_t *input = vir_block_add_param(&caller, entry, VIR_TYPE_I32);
        vir_value_t *seven = vir_const_i32(&caller, entry, 7);
        vir_value_t *args[2] = {input, seven};
        assert(vir_edge_create(&caller, entry, body, NULL, 0));
        vir_effect_t *call =
            vir_call(&caller, body, "leaf", args, 2, VIR_TYPE_I32);
        assert(call && call->result);
        if (loop) {
            /* slt r, r is false, so the walk leaves on the first pass. */
            vir_edge_args_t again = {body, NULL, 0};
            vir_edge_args_t leave = {exit, NULL, 0};
            assert(vir_block_set_branch(
                &caller, body,
                vir_slt(&caller, body, call->result, call->result), &again,
                &leave));
        } else {
            assert(vir_edge_create(&caller, body, exit, NULL, 0));
        }
        assert(vir_block_set_return(&caller, exit, call->result));
        assert(vir_verify(&caller, &error));

        bool expected = !extra || loop;
        assert(vir_inline_calls(&caller, inline_lookup, NULL, VIR_OPT_O2) ==
               expected);
        assert(vir_verify(&caller, &error));
        assert(count_effects(&caller, VIR_EFFECT_CALL) == !expected);
        for (int i = 0; expected && i < 16; i++) {
            unsigned long long bits = 0xffffffffULL - 17 * i;
            assert(run_function(&caller, bits) ==
                   ((3 * bits + 7 + extra * bits) & 0xffffffffULL));
        }
        inline_static = true;
        vir_function_release(&caller);
        vir_function_release(&callee);
    }
}

int main(void)
{
    test_integer_strength_rejections();
    test_integer_strength_reduction();
    test_call_inlining();
    test_extern_inlining();
    test_tail_recursion();
    test_affine_loops();
    test_affine_rejections();
    return 0;
}
