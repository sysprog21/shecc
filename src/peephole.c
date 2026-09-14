/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Determines if an instruction can be fused with a following OP_assign. Fusible
 * instructions are those whose results can be directly written to the final
 * destination register, eliminating intermediate moves.
 */
bool is_fusible_insn(const ph2_ir_t *ph2_ir)
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
    case OP_load_rodata_address:
        return true;
    default:
        return false;
    }
}

/* Opcodes whose "dest" names a register they write. Only opcodes that certainly
 * do are listed, so a live register is never mistaken for dead.
 */
bool op_writes_dest(opcode_t op)
{
    switch (op) {
    case OP_cmov:
    case OP_load:
    case OP_load_constant:
    case OP_global_load:
    case OP_assign:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_bit_not:
    case OP_negate:
    case OP_log_not:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_read:
    case OP_address_of:
    case OP_global_address_of:
    case OP_trunc:
    case OP_sign_ext:
        return true;
    default:
        return false;
    }
}

/* Opcodes whose src0 holds something other than a register number. Anything not
 * listed is assumed to read src0, which only costs a missed rewrite.
 */
bool op_src0_is_reg(opcode_t op)
{
    switch (op) {
    case OP_load:
    case OP_load_constant:
    case OP_global_load:
    case OP_address_of:
    case OP_global_address_of:
    case OP_load_data_address:
    case OP_load_rodata_address:
    case OP_define:
    case OP_label:
    case OP_jump:
        return false;
    default:
        return true;
    }
}

/* Opcodes whose src1 is a register rather than a width, slot or immediate. */
bool op_src1_is_reg(opcode_t op)
{
    switch (op) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
    case OP_write:
    case OP_cmov:
        return true;
    default:
        return false;
    }
}

/* Whether src2 names a register. Only a select does: it is the value kept when
 * the condition does not hold, and a scan that missed it would take that value
 * for dead and drop whatever computed it.
 */
bool op_src2_is_reg(opcode_t op)
{
    return op == OP_cmov;
}

/* Whether @ir reads @reg as an operand. A 32-bit target names the high half of
 * a wide operand in src0_hi or src1_hi, which are -1 when unused.
 */
bool ir_reads_reg(ph2_ir_t *ir, int reg)
{
    if (reg >= 0 && (ir->src0_hi == reg || ir->src1_hi == reg))
        return true;
    if (op_src2_is_reg(ir->op) && ir->src2 == reg)
        return true;
    if (op_src0_is_reg(ir->op) && ir->src0 == reg)
        return true;
    return op_src1_is_reg(ir->op) && ir->src1 == reg;
}

/* Main peephole optimization function that applies pattern matching and
 * transformation rules to consecutive IR instructions.
 * Returns true if any optimization was applied, false otherwise. Drop the
 * instructions after @ir through @last, keeping ph2_ir_list.tail on a node
 * still in the list.
 *
 * Every removal in this file goes through here. A bare "ir->next = last->next"
 * leaves tail pointing at a removed node, which is why x64-codegen.c used to
 * walk a block rather than read its tail.
 */
void ph2_ir_drop_after(basic_block_t *bb, ph2_ir_t *ir, ph2_ir_t *last)
{
    ir->next = last->next;
    if (!ir->next)
        bb->ph2_ir_list.tail = ir;
}

/* How many argument registers the call @ir reads. A call names none of them. A
 * prototyped, non-variadic callee reads only the words its own parameters
 * occupy, the aggregate-return address included; any other call, or a call
 * through a pointer, may read all of them.
 */
int call_arg_regs(ph2_ir_t *ir)
{
    func_t *callee = ir->op == OP_call ? find_func(ir->func_name) : NULL;

    if (!callee || !callee->has_prototype || callee->va_args)
        return MAX_ARGS_IN_REG;

    int words = callee->returns_aggregate ? 1 : 0;
    for (int i = 0; i < callee->num_params && words < MAX_ARGS_IN_REG; i++)
        words = abi_arg_next(words, &callee->param_defs[i], false);
    return words < MAX_ARGS_IN_REG ? words : MAX_ARGS_IN_REG;
}

/* True when @reg still holds a live value past the end of @bb.
 *
 * reg_alloc() hands its register file to a successor that this block is the
 * only way into (bb_export_regs()), so a register can outlive the block that
 * filled it. Scans that stop at the block boundary conclude there according to
 * this: with nothing carried, every register dies at the end of a block and an
 * unread value is dead.
 */
bool reg_live_out_of_bb(basic_block_t *bb, int reg)
{
    basic_block_t *succs[3];

    if (!bb || reg < 0 || reg >= REG_CNT)
        return false;

    /* A register the allocator pinned holds its variable on every path, so it
     * is live out of every block regardless of what the successors record.
     */
    if (bb->belong_to && ((bb->belong_to->pinned_regs >> reg) & 1))
        return true;

    succs[0] = bb->next;
    succs[1] = bb->then_;
    succs[2] = bb->else_;

    for (int i = 0; i < 3; i++) {
        if (succs[i] && succs[i]->entry_regs && succs[i]->entry_regs[reg])
            return true;
    }
    return false;
}

/* Whether @reg may still be read after @ir, before anything writes it again.
 *
 * The scan covers the rest of @bb, with a call reading its argument registers.
 * Past the end of the block a register keeps its value only when it is pinned,
 * or when reg_alloc() handed it to a successor through bb_export_regs(); every
 * other successor loads what it reads.
 */
bool reg_read_after(basic_block_t *bb, ph2_ir_t *ir, int reg)
{
    if (reg < 0)
        return false;
    if (reg >= REG_CNT)
        return true;

    for (ph2_ir_t *p = ir->next; p; p = p->next) {
        if (p->op == OP_call || p->op == OP_indirect) {
            if (reg < call_arg_regs(p))
                return true;
            continue;
        }
        if (ir_reads_reg(p, reg))
            return true;
        if (op_writes_dest(p->op) && (p->dest == reg || p->dest_hi == reg))
            return false;
    }

    return reg_live_out_of_bb(bb, reg);
}

/* Whether folding @ir into @last loses a value that is still wanted. The folded
 * instruction writes only what @last writes, so a register @ir wrote and @last
 * does not keeps whatever it held before, and nothing may read it afterwards.
 * peephole() hands every window with a register pair to pair_insn_fusion(), so
 * both are single registers here.
 */
bool fold_loses_dest(basic_block_t *bb, ph2_ir_t *ir, ph2_ir_t *last)
{
    return ir->dest != last->dest && reg_read_after(bb, last, ir->dest);
}

/* Whether {li t, K; op rd, a, b}, with t one of the operands, may become a
 * single instruction on rd. Every such rewrite leaves t without the constant,
 * which is dropped, moved to rd or replaced by another one. So t has to be dead
 * after the operation, and it cannot be both operands, since the rewrite still
 * reads the other one.
 */
bool const_fold_ok(basic_block_t *bb, ph2_ir_t *li, ph2_ir_t *op)
{
    return op->src0 != op->src1 && !fold_loses_dest(bb, li, op);
}

/* Whether @ir names a 32-bit target's register pair. */
bool ph2_ir_has_pair(const ph2_ir_t *ir)
{
    return ir && (ir->dest_hi >= 0 || ir->src0_hi >= 0 || ir->src1_hi >= 0);
}

/* The rewrites in this file match registers by their low halves and build their
 * results without high halves, so none of them is sound on a register pair.
 * Only the plain move fusion is kept, when the move copies the whole pair and
 * its destination is disjoint from every register the operation reads: the
 * backends emit a pair operation expecting the allocator's guarantee that its
 * result does not overlap an operand, and a low result written first would
 * otherwise clobber a high operand still to be read.
 */
bool pair_insn_fusion(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;

    if (!next || next->op != OP_assign || !is_fusible_insn(ph2_ir))
        return false;
    if (ph2_ir->dest_hi < 0 || ph2_ir->dest != next->src0 ||
        ph2_ir->dest_hi != next->src0_hi || next->dest_hi < 0)
        return false;
    if (ir_reads_reg(ph2_ir, next->dest) || ir_reads_reg(ph2_ir, next->dest_hi))
        return false;
    ph2_ir->dest = next->dest;
    ph2_ir->dest_hi = next->dest_hi;
    ph2_ir_drop_after(bb, ph2_ir, next);
    return true;
}

bool insn_fusion(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* ALU instruction fusion. Eliminates redundant move operations following
     * arithmetic/logical operations. This is the most fundamental optimization
     * that removes temporary register usage.
     */
    if (next->op == OP_assign) {
        if (is_fusible_insn(ph2_ir) && ph2_ir->dest == next->src0 &&
            !fold_loses_dest(bb, ph2_ir, next)) {
            /* Pattern: {ALU rn, rs1, rs2; mv rd, rn} → {ALU rd, rs1, rs2}
             * Example: {add t1, a, b; mv result, t1} → {add result, a, b}
             *
             * Only when nothing reads rn afterwards. A value with two names is
             * copied and then read through the first one as well, and the fused
             * instruction no longer writes it.
             */
            ph2_ir->dest = next->dest;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    /* Arithmetic identity with zero constant */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        ph2_ir->src1 == 0) {
        if (next->op == OP_add &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1) &&
            const_fold_ok(bb, ph2_ir, next)) {
            /* Pattern: {li 0; add x, 0} → {mov x} (additive identity: x+0 = x)
             * Handles both operand positions due to addition commutativity
             * Example: {li t1, 0; add result, var, t1} → {mov result, var}
             */
            int non_zero_src =
                (ph2_ir->dest == next->src0) ? next->src1 : next->src0;

            ph2_ir->op = OP_assign;
            ph2_ir->src0 = non_zero_src;
            ph2_ir->dest = next->dest;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }

        if (next->op == OP_sub && const_fold_ok(bb, ph2_ir, next)) {
            if (ph2_ir->dest == next->src1) {
                /* Pattern: {li 0; sub x, 0} → {mov x} (x - 0 = x)
                 * Example: {li t1, 0; sub result, var, t1} → {mov result, var}
                 */
                ph2_ir->op = OP_assign;
                ph2_ir->src0 = next->src0;
                ph2_ir->dest = next->dest;
                ph2_ir_drop_after(bb, ph2_ir, next);
                return true;
            }

            if (ph2_ir->dest == next->src0) {
                /* Pattern: {li 0; sub 0, x} → {neg x} (0 - x = -x)
                 * Example: {li t1, 0; sub result, t1, var} → {neg result, var}
                 */
                ph2_ir->op = OP_negate;
                ph2_ir->src0 = next->src1;
                ph2_ir->dest = next->dest;
                ph2_ir_drop_after(bb, ph2_ir, next);
                return true;
            }
        }

        if (next->op == OP_mul &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1) &&
            const_fold_ok(bb, ph2_ir, next)) {
            /* Pattern: {li 0; mul x, 0} → {li 0} (absorbing element: x * 0 = 0)
             * Example: {li t1, 0; mul result, var, t1} → {li result, 0}
             * Eliminates multiplication entirely
             */
            ph2_ir->op = OP_load_constant;
            ph2_ir->src0 = 0;
            ph2_ir->dest = next->dest;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    /* Multiplicative identity with one constant */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 1 &&
        ph2_ir->src1 == 0) {
        if (next->op == OP_mul &&
            (ph2_ir->dest == next->src0 || ph2_ir->dest == next->src1) &&
            const_fold_ok(bb, ph2_ir, next)) {
            /* Pattern: {li 1; mul x, 1} → {mov x} (multiplicative identity: x *
             * 1 = x) Example: {li t1, 1; mul result, var, t1} → {mov result,
             * var} Handles both operand positions due to multiplication
             * commutativity
             */
            ph2_ir->op = OP_assign;
            ph2_ir->src0 = ph2_ir->dest == next->src0 ? next->src1 : next->src0;
            ph2_ir->dest = next->dest;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    /* Bitwise identity operations. src1 is the constant's high word, which an
     * eight-byte operation includes: 0xffffffffULL is no identity for it.
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == -1 &&
        ph2_ir->src1 == (next->size_bytes > 4 ? -1 : 0) &&
        next->op == OP_bit_and && ph2_ir->dest == next->src1 &&
        const_fold_ok(bb, ph2_ir, next)) {
        /* Pattern: {li -1; and x, -1} → {mov x} (x & 0xFFFFFFFF = x) Example:
         * {li t1, -1; and result, var, t1} → {mov result, var} Eliminates
         * bitwise AND with all-ones mask
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        ph2_ir->src1 == 0 && (next->op == OP_lshift || next->op == OP_rshift) &&
        ph2_ir->dest == next->src1 && const_fold_ok(bb, ph2_ir, next)) {
        /* Pattern: {li 0; shl/shr x, 0} → {mov x} (x << 0 = x >> 0 = x)
         * Example: {li t1, 0; shl result, var, t1} → {mov result, var}
         * Eliminates no-op shift operations
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        ph2_ir->src1 == 0 && next->op == OP_bit_or &&
        ph2_ir->dest == next->src1 && const_fold_ok(bb, ph2_ir, next)) {
        /* Pattern: {li 0; or x, 0} → {mov x} (x | 0 = x) Example: {li t1, 0; or
         * result, var, t1} → {mov result, var} Eliminates bitwise OR with zero
         * (identity element)
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Power-of-2 multiplication to shift conversion. Shift operations are
     * significantly faster than multiplication
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 > 0 &&
        ph2_ir->src1 == 0 && next->op == OP_mul && ph2_ir->dest == next->src1) {
        int shift_amount = exact_log2(ph2_ir->src0);
        if (shift_amount >= 0 && const_fold_ok(bb, ph2_ir, next)) {
            /* Pattern: {li 2^n; mul x, 2^n} → {li n; shl x, n} Example: {li t1,
             * 4; mul result, var, t1} →
             *          {li t1, 2; shl result, var, t1}
             */
            ph2_ir->src0 = shift_amount;
            next->op = OP_lshift;
            return true;
        }
    }

    /* XOR identity operation */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        ph2_ir->src1 == 0 && next->op == OP_bit_xor &&
        ph2_ir->dest == next->src1 && const_fold_ok(bb, ph2_ir, next)) {
        /* Pattern: {li 0; xor x, 0} → {mov x} (x ^ 0 = x) Example: {li t1, 0;
         * xor result, var, t1} → {mov result, var} Completes bitwise identity
         * optimization coverage
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Extended multiplicative identity (operand position variant) Handles the
     * case where constant 1 is in src0 position of multiplication
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 1 &&
        ph2_ir->src1 == 0 && next->op == OP_mul && ph2_ir->dest == next->src0 &&
        const_fold_ok(bb, ph2_ir, next)) {
        /* Pattern: {li 1; mul 1, x} → {mov x} (1 * x = x) Example: {li t1, 1;
         * mul result, t1, var} → {mov result, var} Covers multiplication
         * commutativity edge case
         */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src1;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    return false;
}

/* Redundant move elimination Eliminates unnecessary move operations that are
 * overwritten or redundant
 */
bool redundant_move_elim(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* Pattern 1: Consecutive assignments to same destination {mov rd, rs1; mov
     * rd, rs2} → {mov rd, rs2} The first move is completely overwritten by the
     * second
     */
    if (ph2_ir->op == OP_assign && next->op == OP_assign &&
        ph2_ir->dest == next->dest) {
        /* Replace first move with second, skip second */
        ph2_ir->src0 = next->src0;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 2: Redundant load immediately overwritten {load rd, offset; mov
     * rd, rs} → {mov rd, rs} Loading a value that's immediately replaced is
     * wasteful
     */
    if ((ph2_ir->op == OP_load || ph2_ir->op == OP_global_load) &&
        next->op == OP_assign && ph2_ir->dest == next->dest) {
        /* Replace load with move */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = 0; /* Clear unused field */
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 3: Load constant immediately overwritten {li rd, imm; mov rd, rs}
     * → {mov rd, rs} Loading a constant that's immediately replaced
     */
    if (ph2_ir->op == OP_load_constant && next->op == OP_assign &&
        ph2_ir->dest == next->dest) {
        /* Replace constant load with move */
        ph2_ir->op = OP_assign;
        ph2_ir->src0 = next->src0;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 4: Consecutive loads to same register {load rd, offset1; load rd,
     * offset2} → {load rd, offset2} First load is pointless if immediately
     * overwritten
     */
    if ((ph2_ir->op == OP_load || ph2_ir->op == OP_global_load) &&
        (next->op == OP_load || next->op == OP_global_load) &&
        ph2_ir->dest == next->dest) {
        /* Keep only the second load */
        ph2_ir->op = next->op;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = next->src1;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 5: Consecutive constant loads (already handled in main loop but
     * included here for completeness) {li rd, imm1; li rd, imm2} → {li rd,
     * imm2}
     */
    if (ph2_ir->op == OP_load_constant && next->op == OP_load_constant &&
        ph2_ir->dest == next->dest) {
        /* Keep only the second constant */
        ph2_ir->src0 = next->src0;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 6: Move followed by load {mov rd, rs; load rd, offset} → {load
     * rd, offset} The move is pointless if immediately overwritten by load
     */
    if (ph2_ir->op == OP_assign &&
        (next->op == OP_load || next->op == OP_global_load) &&
        ph2_ir->dest == next->dest) {
        /* Replace move+load with just the load */
        ph2_ir->op = next->op;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = next->src1;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 7: Move followed by constant load {mov rd, rs; li rd, imm} → {li
     * rd, imm} The move is pointless if immediately overwritten by constant
     */
    if (ph2_ir->op == OP_assign && next->op == OP_load_constant &&
        ph2_ir->dest == next->dest) {
        /* Replace move+li with just the li */
        ph2_ir->op = OP_load_constant;
        ph2_ir->src0 = next->src0;
        ph2_ir->src1 = 0; /* Clear unused field */
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    return false;
}

/* Load/store elimination for consecutive memory operations. Removes redundant
 * loads and dead stores that access the same memory location. Conservative
 * implementation to maintain bootstrap stability.
 */
bool eliminate_load_store_pairs(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* Only handle local loads/stores for now (not globals) to be safe */

    /* Pattern 1: Consecutive stores to same local location {store [addr], val1;
     * store [addr], val2} → {store [addr], val2} First store is dead if
     * immediately overwritten
     */
    if (ph2_ir->op == OP_store && next->op == OP_store) {
        /* Same slot written twice in a row: only the second is observable.
         * Rewrite this one into the second and drop it, which keeps the list
         * links the caller is holding valid.
         *
         * Only at equal width. A wide store followed by a narrow one to the
         * same slot leaves the bytes the second does not cover holding what the
         * first put there, so dropping the first loses them.
         */
        if (ph2_ir->src1 == next->src1 && ph2_ir->src1 >= 0 &&
            ph2_ir->size_bytes == next->size_bytes &&
            ph2_ir->is_pointer == next->is_pointer &&
            ph2_ir->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            ph2_ir->src0 = next->src0;
            ph2_ir->size_bytes = next->size_bytes;
            ph2_ir->is_pointer = next->is_pointer;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    /* Pattern 2: Redundant consecutive loads from same local location {load
     * rd1, [addr]; load rd2, [addr]} → {load rd1, [addr]; mov rd2, rd1} Second
     * load can reuse the first load's result Only apply if addresses are simple
     * (not complex expressions)
     */
    if (ph2_ir->op == OP_load && next->op == OP_load) {
        /* Check if loading from same memory location */
        if (ph2_ir->src0 == next->src0 && ph2_ir->src1 == next->src1 &&
            ph2_ir->src0 >= 0 && ph2_ir->src1 >= 0) {
            /* Replace second load with move */
            next->op = OP_assign;
            next->src0 = ph2_ir->dest; /* Result of first load */
            next->src1 = 0;
            return true;
        }
    }

    /* Pattern 3: Store followed by load from same location (store-to-load
     * forwarding) {store [ofs], reg; load rd, [ofs]} → {store [ofs], reg; mov
     * rd, reg} The load can use the stored value directly.
     *
     * A store names its value in src0 and its slot in src1, while a load names
     * its slot in src0 -- so the two have to be matched field by field, and
     * only at equal width, since a store narrower than the slot leaves the load
     * sign-extending fewer bits than the register holds.
     */
    if (ph2_ir->op == OP_store && next->op == OP_load) {
        if (ph2_ir->src1 == next->src0 && ph2_ir->src0 >= 0 &&
            ph2_ir->size_bytes == next->size_bytes &&
            ph2_ir->is_pointer == next->is_pointer &&
            ph2_ir->size_bytes >= PTR_SIZE &&
            ph2_ir->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            next->op = OP_assign;
            next->src0 = ph2_ir->src0; /* the register that was stored */
            next->src1 = 0;
            return true;
        }
    }

    /* Pattern 4: Load followed by a store of the value just loaded back into
     * the slot it came from. {load rd, [ofs]; store [ofs], rd} → {load rd,
     * [ofs]} -- the slot already holds that value.
     *
     * Only at equal width, since a load narrower than the store brings back a
     * sign-extended byte and the store writes that extension over bytes the
     * load never read.
     */
    if (ph2_ir->op == OP_load && next->op == OP_store) {
        if (ph2_ir->dest == next->src0 && ph2_ir->src0 == next->src1 &&
            ph2_ir->src0 >= 0 && ph2_ir->size_bytes == next->size_bytes &&
            ph2_ir->is_pointer == next->is_pointer &&
            ph2_ir->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    /* Pattern 5: Global store/load optimizations (carefully enabled) */
    if (ph2_ir->op == OP_global_store && next->op == OP_global_store) {
        /* Consecutive global stores to same location */
        if (ph2_ir->src0 == next->src0 && ph2_ir->src1 == next->src1) {
            /* Remove first store - it's dead */
            ph2_ir->dest = next->dest;
            ph2_ir_drop_after(bb, ph2_ir, next);
            return true;
        }
    }

    if (ph2_ir->op == OP_global_load && next->op == OP_global_load) {
        /* Consecutive global loads from same location */
        if (ph2_ir->src0 == next->src0 && ph2_ir->src1 == next->src1) {
            /* Replace second load with move */
            next->op = OP_assign;
            next->src0 = ph2_ir->dest;
            next->src1 = 0;
            return true;
        }
    }

    return false;
}

/* Algebraic simplification: Apply mathematical identities to simplify
 * expressions
 *
 * This function handles patterns that SSA cannot see:
 * - Self-operations on registers (x-x, x^x, x|x, x&x)
 * - These patterns emerge after register allocation when different
 *   variables are assigned to the same register
 *
 * SSA handles: Constant folding with known values (5+3 → 8) Peephole handles:
 * Register-based patterns (r1-r1 → 0)
 *
 * Returns true if optimization was applied
 */
/* Division/modulo strength reduction: Optimize division and modulo by
 * power-of-2
 *
 * This pattern is unique to peephole optimizer. SSA cannot perform this
 * optimization because it works on virtual registers before actual constant
 * values are loaded.
 *
 * Returns true if optimization was applied
 */
bool strength_reduction(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    if (!ph2_ir || !ph2_ir->next)
        return false;

    ph2_ir_t *next = ph2_ir->next;

    /* Check for constant load followed by division or modulo */
    if (ph2_ir->op != OP_load_constant)
        return false;

    /* Each rewrite below loads a different constant into the register, which
     * const_fold_ok() allows only once nothing else wants the original.
     */
    if (next->op != OP_div && next->op != OP_mod && next->op != OP_mul)
        return false;
    if (next->src0 != ph2_ir->dest && next->src1 != ph2_ir->dest)
        return false;

    int value = ph2_ir->src0;

    /* Check if value is a power of 2. src1 is the high word of an eight-byte
     * constant, and 0x100000004ULL is no power of two.
     */
    if (ph2_ir->src1 || value <= 0 || (value & (value - 1)) != 0)
        return false;
    if (!const_fold_ok(bb, ph2_ir, next))
        return false;

    /* Calculate shift amount for power of 2 */
    int shift = 0;
    int tmp = value;
    while (tmp > 1) {
        shift++;
        tmp >>= 1;
    }

    /* Pattern 1: Division by power of 2 → right shift x / 2^n = x >> n (for
     * unsigned)
     */
    if (next->op == OP_div && next->src0_is_unsigned &&
        next->src1 == ph2_ir->dest) {
        /* Convert division to right shift */
        ph2_ir->src0 = shift; /* Load shift amount instead */
        next->op = OP_rshift;
        return true;
    }

    /* Pattern 2: Modulo by power of 2 → bitwise AND x % 2^n = x & (2^n - 1) */
    if (next->op == OP_mod && next->src0_is_unsigned &&
        next->src1 == ph2_ir->dest) {
        /* Convert modulo to bitwise AND */
        ph2_ir->src0 = value - 1; /* Load mask (2^n - 1) */
        next->op = OP_bit_and;
        return true;
    }

    /* Pattern 3: Multiplication by power of 2 → left shift
     * x * 2^n = x << n
     */
    if (next->op == OP_mul) {
        if (next->src0 == ph2_ir->dest) {
            /* 2^n * x = x << n */
            ph2_ir->src0 = shift; /* Load shift amount */
            next->op = OP_lshift;
            next->src0 = next->src1;   /* Move x to src0 */
            next->src1 = ph2_ir->dest; /* Shift amount in src1 */
            return true;
        } else if (next->src1 == ph2_ir->dest) {
            /* x * 2^n = x << n */
            ph2_ir->src0 = shift; /* Load shift amount */
            next->op = OP_lshift;
            return true;
        }
    }

    return false;
}

/* Simplify bitwise patterns the SSA optimizer cannot see, because they only
 * become visible once registers are assigned.
 *
 * Returns true when it rewrote something.
 */
bool bitwise_optimization(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    if (!ph2_ir || !ph2_ir->next)
        return false;

    ph2_ir_t *next = ph2_ir->next;

    /* Pattern 1: Double complement → identity ~(~x) = x. The first complement
     * no longer writes its register, so nothing else may read it.
     */
    if (ph2_ir->op == OP_bit_not && next->op == OP_bit_not &&
        next->src0 == ph2_ir->dest && !fold_loses_dest(bb, ph2_ir, next)) {
        /* Replace with simple assignment */
        ph2_ir->op = OP_assign;
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    /* Pattern 2: AND with zero → zero x & 0 = 0, and OR with all-ones →
     * all-ones x | 0xFFFFFFFF = 0xFFFFFFFF. The constant load is retargeted to
     * the result and the operation goes. An eight-byte OR needs the high word,
     * src1, all ones as well. The identities x & -1, x | 0, x ^ 0 and x << 0
     * belong to insn_fusion(), which tries them first.
     */
    if (ph2_ir->op == OP_load_constant &&
        ((ph2_ir->src0 == 0 && ph2_ir->src1 == 0 && next->op == OP_bit_and) ||
         (ph2_ir->src0 == -1 &&
          ph2_ir->src1 == (next->size_bytes > 4 ? -1 : 0) &&
          next->op == OP_bit_or)) &&
        (next->src0 == ph2_ir->dest || next->src1 == ph2_ir->dest) &&
        const_fold_ok(bb, ph2_ir, next)) {
        ph2_ir->dest = next->dest;
        ph2_ir_drop_after(bb, ph2_ir, next);
        return true;
    }

    return false;
}

/* Triple pattern optimization: Handle 3-instruction sequences These patterns
 * are more complex but offer significant optimization opportunities Returns
 * true if optimization was applied
 */
bool triple_pattern_optimization(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    if (!ph2_ir || !ph2_ir->next || !ph2_ir->next->next)
        return false;

    ph2_ir_t *second = ph2_ir->next;
    ph2_ir_t *third = second->next;

    /* Pattern 1: Store-load-store elimination {store val1, addr; load r, addr;
     * store val2, addr} The middle load is pointless if not used elsewhere
     */
    if (ph2_ir->op == OP_store && second->op == OP_load &&
        third->op == OP_store &&
        ph2_ir->src1 == second->src0 && /* same address */
        ph2_ir->dest == second->src1 && /* same offset */
        second->src0 == third->src1 &&  /* same address */
        second->src1 == third->dest) {  /* same offset */
        /* Only when nothing reads the loaded value, the third store included:
         * without the load the register keeps what it held before.
         */
        if (!reg_read_after(bb, second, second->dest)) {
            /* The load result is not used, can eliminate it */
            ph2_ir->next = third;
            return true;
        }
    }

    /* Pattern 2: Consecutive stores to same location {store v1, addr; store v2,
     * addr; store v3, addr} Only the last store matters
     */
    if (ph2_ir->op == OP_store && second->op == OP_store &&
        third->op == OP_store && ph2_ir->src1 == second->src1 &&
        ph2_ir->dest == second->dest && second->src1 == third->src1 &&
        second->dest == third->dest) {
        /* All three stores go to the same location Only the last one matters,
         * eliminate first two
         */
        ph2_ir->src0 = third->src0;           /* Use last value */
        ph2_ir_drop_after(bb, ph2_ir, third); /* Skip middle stores */
        return true;
    }

    /* FIXME: Additional optimization patterns to implement:
     *
     * Pattern 3: Load-op-store with same location {load r1, [addr]; op r2, r1,
     * ...; store r2, [addr]} Can optimize to in-place operation if possible
     * Requires architecture-specific support in codegen.
     *
     * Pattern 4: Redundant comparison after boolean operation {cmp a, b; load
     * 1; load 0} → simplified when used in branch The comparison already
     * produces 0 or 1, constants may be redundant
     *
     * Pattern 5: Consecutive loads that can be combined {load r1, [base+off1];
     * load r2, [base+off2]; op r3, r1, r2} Useful for struct member access
     * patterns Needs alignment checking and architecture support.
     *
     * Pattern 6: Load-Load-Select pattern {load r1, c1; load r2, c2;
     * select/cmov based on condition} Can optimize by loading only the needed
     * value Requires control flow analysis.
     *
     * Pattern 7: Add-Add-Add chain simplification {add r1, r0, c1; add r2, r1,
     * c2; add r3, r2, c3} Can be simplified if all are constants Requires
     * tracking constant values through the chain.
     *
     * Pattern 8: Global load followed by immediate use {global_load r1; op r2,
     * r1, ...; store r2} Track global access patterns Could optimize to atomic
     * operations or direct memory ops. Needs careful synchronization analysis.
     */

    return false;
}

/* Main peephole optimization driver.
 *
 * This runs on ph2_ir_t, after register allocation, and so sees only what
 * assigning registers makes visible. Constant folding, common subexpression
 * elimination and dead code elimination have already run over insn_t in the SSA
 * optimizer and are not repeated here.
 *
 * What is left to do at this level:
 * - self-assignment elimination, for assignments allocation itself created
 * - instruction fusion, including strength reduction for a power of two, which
 *   needs the constant actually loaded into a register
 * - bitwise identities on registers
 * - load/store pattern elimination
 */
void peephole(void)
{
    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        /* Local peephole optimizations on post-register-allocation IR */
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                ph2_ir_t *next = ir->next;
                if (!next)
                    continue;

                /* Self-assignment elimination Keep this as a safety net: SSA
                 * handles most cases, but register allocation might create new
                 * self-assignments
                 */
                if (next->op == OP_assign && next->dest == next->src0 &&
                    next->dest_hi == next->src0_hi) {
                    ph2_ir_drop_after(bb, ir, next);
                    continue;
                }

                /* Every rewrite below moves this instruction's result to the
                 * destination of the one after it, dropping the write to the
                 * register it named. That is fine for a temporary, whose value
                 * nothing wants again, and wrong for a pinned register: a
                 * variable lives there for the whole function and nothing else
                 * ever reloads it, so "li rbx, 0; add rax, rsi, rbx" must not
                 * become "mov rax, rsi" and leave rbx unwritten.
                 */
                if (ir->dest >= 0 && ir->dest < REG_CNT &&
                    ((func->pinned_regs >> ir->dest) & 1))
                    continue;

                if (PTR_SIZE < 8 &&
                    (ph2_ir_has_pair(ir) || ph2_ir_has_pair(next) ||
                     ph2_ir_has_pair(next->next))) {
                    pair_insn_fusion(bb, ir);
                    continue;
                }

                /* Try triple pattern optimization first (3-instruction
                 * sequences)
                 */
                if (triple_pattern_optimization(bb, ir))
                    continue;

                /* Try instruction fusion (2-instruction sequences) */
                if (insn_fusion(bb, ir))
                    continue;

                /* Apply strength reduction for power-of-2 operations */
                if (strength_reduction(bb, ir))
                    continue;

                /* Apply bitwise operation optimizations */
                if (bitwise_optimization(bb, ir))
                    continue;

                /* Apply redundant move elimination */
                if (redundant_move_elim(bb, ir))
                    continue;

                /* Apply load/store elimination */
                if (eliminate_load_store_pairs(bb, ir))
                    continue;
            }
        }
    }
}
