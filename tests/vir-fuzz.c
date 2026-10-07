#include "vir.h"

#include <assert.h>

int main(void)
{
    unsigned int state = 1;
    int round;

    for (round = 0; round < 64; round++) {
        vir_function_t func;
        vir_block_t *entry;
        vir_block_t *loop;
        vir_block_t *merge;
        vir_value_t *values[18];
        vir_value_t *params[2];
        vir_value_t *loop_param;
        vir_value_t *arg;
        vir_value_t *alternate_condition;
        vir_value_t *widened;
        vir_value_t *narrowed;
        vir_value_t *address;
        vir_value_t *loaded;
        vir_effect_t *store;
        vir_value_t *merge_args[2];
        vir_value_t *loop_args[1];
        char *error;
        int i;
        int old_operand;
        vir_use_t *use;
        vir_edge_t *old_edge;
        vir_block_t *old_return_block;
        vir_block_t *old_branch_block;

        vir_function_init(&func, 128);
        assert(vir_function_set_pointer_bits(&func, 32));
        entry = vir_block_create(&func);
        loop = vir_block_create(&func);
        merge = vir_block_create(&func);
        loop_param = vir_block_add_param(&func, loop, VIR_TYPE_I32);
        params[0] = vir_block_add_param(&func, merge, VIR_TYPE_I32);
        params[1] = vir_block_add_param(&func, merge, VIR_TYPE_I32);
        state = state * 1103515245u + 12345u;
        values[0] = vir_const_i32(&func, entry, (int) (state & 255));
        state = state * 1103515245u + 12345u;
        values[1] = vir_const_i32(&func, entry, (int) (state & 255));
        for (i = 2; i < 18; i++) {
            state = state * 1103515245u + 12345u;
            values[i] = vir_binary(
                &func, entry, state & 1 ? VIR_OP_ADD : VIR_OP_MUL, VIR_TYPE_I32,
                values[state % i], values[(state >> 8) % i]);
            assert(values[i]);
        }
        arg = vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1, values[16],
                         values[17]);
        alternate_condition = vir_binary(&func, entry, VIR_OP_EQ, VIR_TYPE_I1,
                                         values[17], values[16]);
        widened = vir_zext_i1(&func, entry, arg);
        narrowed = vir_trunc_i1(&func, entry, values[17]);
        assert(widened && narrowed && widened->type == VIR_TYPE_I32 &&
               narrowed->type == VIR_TYPE_I1);
        address = vir_stack_addr(&func, entry, (unsigned int) round, 4, 4);
        store = vir_store(&func, entry, address, values[0]);
        loaded = vir_load(&func, entry, address, VIR_TYPE_I32);
        assert(address && store && loaded);
        merge_args[0] = widened;
        merge_args[1] = values[16];
        loop_args[0] = values[17];
        vir_edge_args_t true_desc = {loop, loop_args, 1};
        vir_edge_args_t false_desc = {loop, loop_args, 1};
        assert(
            vir_block_set_branch(&func, entry, arg, &true_desc, &false_desc));
        assert(params[0]->type == VIR_TYPE_I32 &&
               params[1]->type == VIR_TYPE_I32);
        loop_args[0] = loop_param;
        true_desc.to = loop;
        true_desc.args = loop_args;
        true_desc.arg_count = 1;
        false_desc.to = merge;
        false_desc.args = merge_args;
        false_desc.arg_count = 2;
        assert(vir_block_set_branch(&func, loop, arg, &true_desc, &false_desc));
        assert(vir_block_set_return(&func, merge, params[0]));
        assert(vir_verify(&func, &error));

        /* Exercise every reverse-use kind with a dominating replacement. */
        assert(vir_replace_all_uses(&func, values[0], values[1]));
        assert(vir_replace_all_uses(&func, loop_param, values[16]));
        assert(vir_replace_all_uses(&func, params[0], values[16]));
        assert(vir_replace_all_uses(&func, arg, alternate_condition));
        assert(vir_verify(&func, &error));

        /* Deterministic malformed-graph mutations exercise the verifier's
         * reverse links, CFG shape, edge arguments, and placement checks.
         */
        merge->incoming = NULL;
        assert(!vir_verify(&func, &error));
        merge->incoming = loop->false_edge;
        assert(vir_verify(&func, &error));

        entry->false_edge = entry->true_edge;
        assert(!vir_verify(&func, &error));
        entry->false_edge = entry->outgoing;
        assert(vir_verify(&func, &error));

        loop->false_edge->args[0] = alternate_condition;
        assert(!vir_verify(&func, &error));
        loop->false_edge->args[0] = merge_args[0];
        assert(vir_verify(&func, &error));

        use = values[1]->uses;
        while (use && !use->user)
            use = use->next;
        assert(use);
        old_operand = use->operand;
        use->operand = 7;
        assert(!vir_verify(&func, &error));
        use->operand = old_operand;
        assert(vir_verify(&func, &error));

        use = values[16]->uses;
        while (use && !use->edge)
            use = use->next;
        assert(use);
        old_edge = use->edge;
        use->edge = NULL;
        assert(!vir_verify(&func, &error));
        use->edge = old_edge;
        assert(vir_verify(&func, &error));

        use = values[16]->uses;
        while (use && !use->return_block)
            use = use->next;
        assert(use);
        old_return_block = use->return_block;
        use->return_block = NULL;
        assert(!vir_verify(&func, &error));
        use->return_block = old_return_block;
        assert(vir_verify(&func, &error));

        use = alternate_condition->uses;
        while (use && !use->branch_block)
            use = use->next;
        assert(use);
        old_branch_block = use->branch_block;
        use->branch_block = NULL;
        assert(!vir_verify(&func, &error));
        use->branch_block = old_branch_block;
        assert(vir_verify(&func, &error));
        vir_function_release(&func);
    }
    return 0;
}
