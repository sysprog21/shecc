/*
 * Valid duplicate-successor CFG verifier regression. It runs in the stage-0 and
 * stage-2 target gates after the ARM verifier codegen defect was repaired.
 */
#include "../src/vir.c"

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
    char *error;

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
    if (!entry || !merge || !one || !two || !condition || !param)
        return 1;
    if (!vir_block_set_branch(&func, entry, condition, &true_args, &false_args))
        return 2;
    if (!vir_block_set_return(&func, merge, param))
        return 3;
    if (!vir_verify(&func, &error))
        return 4;
    vir_function_release(&func);
    return 0;
}
