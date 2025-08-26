/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Determines if an instruction can be fused with a following OP_assign.
 * Fusible instructions are those whose results can be directly written
 * to the final destination register, eliminating intermediate moves.
 */
bool is_fusible_insn(ph2_ir_t *ph2_ir)
{
    switch (ph2_ir->op) {
    case OP_add: /* Arithmetic operations */
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift: /* Shift operations */
    case OP_rshift:
    case OP_bit_and: /* Bitwise operations */
    case OP_bit_or:
    case OP_bit_xor:
    case OP_log_and: /* Logical operations */
    case OP_log_or:
    case OP_log_not:
    case OP_negate: /* Unary operations */
    case OP_load:   /* Memory operations */
    case OP_global_load:
    case OP_load_data_address:
        return true;
    default:
        return false;
    }
}

/* Main peephole optimization function that applies pattern matching
 * and transformation rules to consecutive IR instructions.
 * Returns true if any optimization was applied, false otherwise.
 */
bool insn_fusion(ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* ALU instruction fusion.
     * Eliminates redundant move operations following arithmetic/logical
     * operations. This is the most fundamental optimization that removes
     * temporary register usage.
     */
    if (next->op == OP_assign) {
        if (is_fusible_insn(ph2_ir) && ph2_ir->dest == next->src0) {
            /* Pattern: {ALU rn, rs1, rs2; mv rd, rn} → {ALU rd, rs1, rs2}
             * Example: {add t1, a, b; mv result, t1} → {add result, a, b}
             */
            ph2_ir->dest = next->dest;
            ph2_ir->next = next->next;
            return true;
        }
    }

    /* Arithmetic identity with zero constant */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0) {
        if (next->op == OP_add &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1)) {
            /* Pattern: {li 0; add x, 0} → {mov x} (additive identity: x+0 = x)
             * Handles both operand positions due to addition commutativity
             * Example: {li t1, 0; add result, var, t1} → {mov result, var}
             */
            int non_zero_src =
                (ph2_ir->dest == next->src0) ? next->src1 : next->src0;

            ph2_ir->op = OP_assign;
            ph2_ir->src0 = non_zero_src;
            ph2_ir->dest = next->dest;
            ph2_ir->next = next->next;
            return true;
        }

        if (next->op == OP_sub) {
            if (ph2_ir->dest == next->src1) {
                /* Pattern: {li 0; sub x, 0} → {mov x} (x - 0 = x)
                 * Example: {li t1, 0; sub result, var, t1} → {mov result, var}
                 */
                ph2_ir->op = OP_assign;
                ph2_ir->src0 = next->src0;
                ph2_ir->dest = next->dest;
                ph2_ir->next = next->next;
                return true;
            }

            if (ph2_ir->dest == next->src0) {
                /* Pattern: {li 0; sub 0, x} → {neg x} (0 - x = -x)
                 * Example: {li t1, 0; sub result, t1, var} → {neg result, var}
                 */
                ph2_ir->op = OP_negate;
                ph2_ir->src0 = next->src1;
                ph2_ir->dest = next->dest;
                ph2_ir->next = next->next;
                return true;
            }
        }

        if (next->op == OP_mul &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1)) {
            /* Pattern: {li 0; mul x, 0} → {li 0} (absorbing element: x * 0 = 0)
             * Example: {li t1, 0; mul result, var, t1} → {li result, 0}
             * Eliminates multiplication entirely
             */
            ph2_ir->op = OP_load_constant;
            ph2_ir->src0 = 0;
            ph2_ir->dest = next->dest;
            ph2_ir->next = next->next;
            return true;
        }
    }

    /* Multiplicative identity with one constant */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 1) {
        if (next->op == OP_mul &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1)) {
            /* Pattern: {li 1; mul x, 1} → {mov x} (multiplicative identity:
             * x * 1 = x)
             * Example: {li t1, 1; mul result, var, t1} → {mov result, var}
             * Handles both operand positions due to multiplication
             * commutativity
             */
            ph2_ir->op = OP_assign;
            ph2_ir->src0 = ph2_ir->dest == next->src0 ? next->src1 : next->src0;
            ph2_ir->dest = next->dest;
            ph2_ir->next = next->next;
            return true;
        }
    }

    /* Bitwise identity operations */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == -1 &&
        next->op == OP_bit_and && ph2_ir->dest == next->src1) {
        /* Pattern: {li -1; and x, -1} → {mov x} (x & 0xFFFFFFFF = x)
         * Example: {li t1, -1; and result, var, t1} → {mov result, var}
         * Eliminates bitwise AND with all-ones mask
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir->next = next->next;
        return true;
    }

    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        (next->op == OP_lshift || next->op == OP_rshift) &&
        ph2_ir->dest == next->src1) {
        /* Pattern: {li 0; shl/shr x, 0} → {mov x} (x << 0 = x >> 0 = x)
         * Example: {li t1, 0; shl result, var, t1} → {mov result, var}
         * Eliminates no-op shift operations
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir->next = next->next;
        return true;
    }

    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        next->op == OP_bit_or && ph2_ir->dest == next->src1) {
        /* Pattern: {li 0; or x, 0} → {mov x} (x | 0 = x)
         * Example: {li t1, 0; or result, var, t1} → {mov result, var}
         * Eliminates bitwise OR with zero (identity element)
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir->next = next->next;
        return true;
    }

    /* Power-of-2 multiplication to shift conversion.
     * Shift operations are significantly faster than multiplication
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 > 0 &&
        next->op == OP_mul && ph2_ir->dest == next->src1) {
        int power = ph2_ir->src0;
        /* Detect power-of-2 using bit manipulation: (n & (n-1)) == 0 for powers
         * of 2
         */
        if (power && (power & (power - 1)) == 0) {
            /* Calculate log2(power) to determine shift amount */
            int shift_amount = 0;
            int tmp = power;
            while (tmp > 1) {
                tmp >>= 1;
                shift_amount++;
            }
            /* Pattern: {li 2^n; mul x, 2^n} → {li n; shl x, n}
             * Example: {li t1, 4; mul result, var, t1} →
             *          {li t1, 2; shl result, var, t1}
             */
            ph2_ir->op = OP_load_constant;
            ph2_ir->src0 = shift_amount;
            next->op = OP_lshift;
            return true;
        }
    }

    /* XOR identity operation */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        next->op == OP_bit_xor && ph2_ir->dest == next->src1) {
        /* Pattern: {li 0; xor x, 0} → {mov x} (x ^ 0 = x)
         * Example: {li t1, 0; xor result, var, t1} → {mov result, var}
         * Completes bitwise identity optimization coverage
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir->next = next->next;
        return true;
    }

    /* Extended multiplicative identity (operand position variant)
     * Handles the case where constant 1 is in src0 position of multiplication
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 1 &&
        next->op == OP_mul && ph2_ir->dest == next->src0) {
        /* Pattern: {li 1; mul 1, x} → {mov x} (1 * x = x)
         * Example: {li t1, 1; mul result, t1, var} → {mov result, var}
         * Covers multiplication commutativity edge case
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src1;
        ph2_ir->dest = next->dest;
        ph2_ir->next = next->next;
        return true;
    }

    return false;
}

/* Redundant move elimination
 * Eliminates unnecessary move operations that are overwritten or redundant
 */
bool redundant_move_elim(ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* Pattern 1: Consecutive assignments to same destination
     * {mov rd, rs1; mov rd, rs2} → {mov rd, rs2}
     * The first move is completely overwritten by the second
     */
    if (ph2_ir->op == OP_assign && next->op == OP_assign &&
        ph2_ir->dest == next->dest) {
        /* Replace first move with second, skip second */
        ph2_ir->src0 = next->src0;
        ph2_ir->next = next->next;
        return true;
    }

    /* Pattern 2: Redundant load immediately overwritten
     * {load rd, offset; mov rd, rs} → {mov rd, rs}
     * Loading a value that's immediately replaced is wasteful
     */
    if ((ph2_ir->op == OP_load || ph2_ir->op == OP_global_load) &&
        next->op == OP_assign && ph2_ir->dest == next->dest) {
        /* Replace load with move */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = 0; /* Clear unused field */
        ph2_ir->next = next->next;
        return true;
    }

    /* Pattern 3: Load constant immediately overwritten
     * {li rd, imm; mov rd, rs} → {mov rd, rs}
     * Loading a constant that's immediately replaced
     */
    if (ph2_ir->op == OP_load_constant && next->op == OP_assign &&
        ph2_ir->dest == next->dest) {
        /* Replace constant load with move */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->next = next->next;
        return true;
    }

    /* Pattern 4: Consecutive loads to same register
     * {load rd, offset1; load rd, offset2} → {load rd, offset2}
     * First load is pointless if immediately overwritten
     */
    if ((ph2_ir->op == OP_load || ph2_ir->op == OP_global_load) &&
        (next->op == OP_load || next->op == OP_global_load) &&
        ph2_ir->dest == next->dest) {
        /* Keep only the second load */
        ph2_ir->op = next->op;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = next->src1;
        ph2_ir->next = next->next;
        return true;
    }

    /* Pattern 5: Consecutive constant loads (already handled in main loop
     * but included here for completeness)
     * {li rd, imm1; li rd, imm2} → {li rd, imm2}
     */
    if (ph2_ir->op == OP_load_constant && next->op == OP_load_constant &&
        ph2_ir->dest == next->dest) {
        /* Keep only the second constant */
        ph2_ir->src0 = next->src0;
        ph2_ir->next = next->next;
        return true;
    }

    return false;
}

/* Simple dead instruction elimination within basic blocks.
 * Removes instructions whose results are never used (dead stores).
 * Works in conjunction with existing SSA-based DCE.
 */
bool eliminate_dead_instructions(func_t *func)
{
    if (!func || !func->bbs)
        return false;

    bool changed = false;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        ph2_ir_t *ir = bb->ph2_ir_list.head;
        while (ir && ir->next) {
            ph2_ir_t *next = ir->next;

            /* Check if next instruction immediately overwrites this one's
             * result */
            if (ir->op == OP_load_constant && next->op == OP_load_constant &&
                ir->dest == next->dest) {
                /* Consecutive constant loads to same register - first is dead
                 */
                ir->next = next->next;
                if (next == bb->ph2_ir_list.tail) {
                    bb->ph2_ir_list.tail = ir;
                }
                changed = true;
                continue;
            }

            /* Check for dead arithmetic results */
            if ((ir->op == OP_add || ir->op == OP_sub || ir->op == OP_mul) &&
                next->op == OP_assign && ir->dest == next->dest) {
                /* Arithmetic result immediately overwritten by assignment */
                ir->next = next->next;
                if (next == bb->ph2_ir_list.tail) {
                    bb->ph2_ir_list.tail = ir;
                }
                changed = true;
                continue;
            }

            ir = ir->next;
        }
    }

    return changed;
}

/* Simple constant folding for branches after SCCP.
 * Converts branches with obvious constant conditions to jumps.
 * Very conservative to maintain bootstrap stability.
 */
bool fold_constant_branches(func_t *func)
{
    if (!func || !func->bbs)
        return false;

    bool changed = false;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        if (!bb->ph2_ir_list.tail)
            continue;

        ph2_ir_t *last = bb->ph2_ir_list.tail;

        /* Only handle branches */
        if (last->op != OP_branch || last->src0 < 0)
            continue;

        /* Look for immediately preceding constant load to the same register */
        ph2_ir_t *prev = bb->ph2_ir_list.head;
        ph2_ir_t *found = NULL;

        /* Find the most recent constant load to the branch condition register
         */
        while (prev && prev != last) {
            if (prev->op == OP_load_constant && prev->dest == last->src0) {
                found = prev;
                /* Keep looking - want the most recent load */
            }
            /* Stop if we see any other write to this register */
            else if (prev->dest == last->src0) {
                found = NULL; /* Register was modified, can't fold */
            }
            prev = prev->next;
        }

        if (found) {
            /* Found constant condition - convert branch to jump */
            int const_val = found->src0;

            /* Just change the opcode, don't modify CFG edges directly */
            last->op = OP_jump;

            if (const_val != 0) {
                /* Always take then branch */
                last->next_bb = bb->then_;
            } else {
                /* Always take else branch */
                last->next_bb = bb->else_;
            }

            /* Don't modify src0 or CFG edges - let later passes handle it */
            changed = true;
        }
    }

    return changed;
}

/* Main peephole optimization driver.
 * It iterates through all functions, basic blocks, and IR instructions to apply
 * local optimizations on adjacent instruction pairs.
 */
void peephole(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Phase 1: Dead code elimination working with SCCP results */
        eliminate_dead_instructions(func);
        fold_constant_branches(func);

        /* Phase 2: Local peephole optimizations */
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                ph2_ir_t *next = ir->next;
                if (!next)
                    continue;

                /* Self-assignment elimination
                 * Removes trivial assignments where destination equals source
                 * Pattern: {mov x, x} → eliminated
                 * Common in compiler-generated intermediate code
                 */
                if (next->op == OP_assign && next->dest == next->src0) {
                    ir->next = next->next;
                    continue;
                }

                /* Try instruction fusion first */
                if (insn_fusion(ir))
                    continue;

                /* Apply redundant move elimination */
                if (redundant_move_elim(ir))
                    continue;
            }
        }
    }
}
