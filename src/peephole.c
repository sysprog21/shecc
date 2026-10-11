/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* These instructions can write their result straight to a following move's
 * destination, eliminating the intermediate register copy.
 */
bool is_fusible_insn(const vir_mop_t *ir)
{
    return op_property(ir->op, OP_FUSIBLE);
}

/* Scan local reads and definitions, then consult the successor live set. */
static bool reg_read_after(basic_block_t *bb, vir_mop_t *ir, int reg)
{
    if (reg < 0)
        return false;
    if (reg >= REG_CNT)
        return true;
    for (vir_mop_t *p = ir->next; p; p = p->next) {
        if (ir_reads_reg(p, reg))
            return true;
        if (mop_defs(p) & (1u << reg))
            return false;
    }
    return (bb->machine_liveout & (1u << reg)) != 0;
}

/* Whether folding @ir into @last loses a value that is still wanted. The folded
 * instruction writes only what @last writes, so a register @ir wrote and @last
 * does not keeps whatever it held before, and nothing may read it afterwards.
 * peephole() hands every window with a register pair to pair_insn_fusion(), so
 * both are single registers here.
 */
bool fold_loses_dest(basic_block_t *bb, vir_mop_t *ir, vir_mop_t *last)
{
    return ir->dest != last->dest && reg_read_after(bb, last, ir->dest);
}

/* Whether {li t, K; op rd, a, b}, with t one of the operands, may become a
 * single instruction on rd. Every such rewrite leaves t without the constant,
 * which is dropped, moved to rd or replaced by another one. So t has to be dead
 * after the operation, and it cannot be both operands, since the rewrite still
 * reads the other one.
 */
bool const_fold_ok(basic_block_t *bb, vir_mop_t *li, vir_mop_t *op)
{
    return op->src0 != op->src1 && !fold_loses_dest(bb, li, op);
}

/* Whether @ir names a 32-bit target's register pair. */
bool mop_has_pair(const vir_mop_t *ir)
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
bool pair_insn_fusion(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *next = mop->next;

    if (!next || next->op != OP_assign || !is_fusible_insn(mop))
        return false;
    if (mop->dest_hi < 0 || mop->dest != next->src0 ||
        mop->dest_hi != next->src0_hi || next->dest_hi < 0)
        return false;
    if (ir_reads_reg(mop, next->dest) || ir_reads_reg(mop, next->dest_hi))
        return false;

    /* The fused operation no longer writes its own pair, so that pair must be
     * dead after the move. Common subexpression elimination leaves exactly the
     * copy that is not: "(a & b) ^ (b & a)" became "x = a & b; y = x; x ^ y",
     * and fusing the move left the XOR reading a stale x.
     */
    if ((mop->dest != next->dest && reg_read_after(bb, next, mop->dest)) ||
        (mop->dest_hi != next->dest_hi &&
         reg_read_after(bb, next, mop->dest_hi)))
        return false;
    mop->dest = next->dest;
    mop->dest_hi = next->dest_hi;
    mop_drop_after(bb, mop, next);
    return true;
}

static bool replace_with_move(basic_block_t *bb,
                              vir_mop_t *ir,
                              vir_mop_t *next,
                              int source)
{
    ir->op = OP_assign;
    ir->src0 = source;
    ir->dest = next->dest;
    mop_drop_after(bb, ir, next);
    return true;
}

bool insn_fusion(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *next = mop->next;
    if (!next)
        return false;

    if ((mop->op == OP_eq || mop->op == OP_neq) && next->op == OP_log_not &&
        next->src0 == mop->dest && !fold_loses_dest(bb, mop, next)) {
        mop->op = mop->op == OP_eq ? OP_neq : OP_eq;
        mop->dest = next->dest;
        mop_drop_after(bb, mop, next);
        return true;
    }

    /* Fuse result moves and fold constant identities. */
    if (next->op == OP_assign) {
        if (is_fusible_insn(mop) && mop->dest == next->src0 &&
            !fold_loses_dest(bb, mop, next)) {
            /* Do not remove a temporary that still has another live use. */
            mop->dest = next->dest;
            mop_drop_after(bb, mop, next);
            return true;
        }
    }

    /* Arithmetic identity with zero constant */
    if (mop->op == OP_load_constant && mop->src0 == 0 && mop->src1 == 0) {
        if (next->op == OP_add &&
            (mop->dest == next->src0 || mop->dest == next->src1) &&
            const_fold_ok(bb, mop, next)) {
            int non_zero_src =
                (mop->dest == next->src0) ? next->src1 : next->src0;

            return replace_with_move(bb, mop, next, non_zero_src);
        }

        if (next->op == OP_sub && const_fold_ok(bb, mop, next)) {
            if (mop->dest == next->src1) {
                return replace_with_move(bb, mop, next, next->src0);
            }

            if (mop->dest == next->src0) {
                mop->op = OP_negate;
                mop->src0 = next->src1;
                mop->dest = next->dest;
                mop_drop_after(bb, mop, next);
                return true;
            }
        }

        if (next->op == OP_mul &&
            (mop->dest == next->src0 || mop->dest == next->src1) &&
            const_fold_ok(bb, mop, next)) {
            mop->op = OP_load_constant;
            mop->src0 = 0;
            mop->dest = next->dest;
            mop_drop_after(bb, mop, next);
            return true;
        }
    }

    /* Multiplicative identity with one constant */
    if (mop->op == OP_load_constant && mop->src0 == 1 && mop->src1 == 0) {
        if (next->op == OP_mul &&
            (mop->dest == next->src0 || mop->dest == next->src1) &&
            const_fold_ok(bb, mop, next)) {
            return replace_with_move(
                bb, mop, next,
                mop->dest == next->src0 ? next->src1 : next->src0);
        }
    }

    /* Bitwise identity operations. src1 is the constant's high word, which an
     * eight-byte operation includes: 0xffffffffULL is no identity for it.
     */
    if (mop->op == OP_load_constant && mop->src0 == -1 &&
        mop->src1 == (next->size_bytes > 4 ? -1 : 0) &&
        next->op == OP_bit_and && mop->dest == next->src1 &&
        const_fold_ok(bb, mop, next)) {
        return replace_with_move(bb, mop, next, next->src0);
    }

    if (mop->op == OP_load_constant && mop->src0 == 0 && mop->src1 == 0 &&
        (next->op == OP_lshift || next->op == OP_rshift ||
         next->op == OP_bit_or || next->op == OP_bit_xor) &&
        mop->dest == next->src1 && const_fold_ok(bb, mop, next))
        return replace_with_move(bb, mop, next, next->src0);

    return false;
}

/* Whether @ir does nothing but set its destination register: a move, a slot
 * load or a constant load.
 */
bool is_plain_register_def(const vir_mop_t *ir)
{
    return ir->op == OP_assign || ir->op == OP_load ||
           ir->op == OP_global_load || ir->op == OP_load_constant;
}

static bool is_full_register_copy(const vir_mop_t *ir)
{
    return ir && ir->op == OP_assign && ir->dest_hi < 0 && ir->src0_hi < 0 &&
           !(ir->is_unsigned && ir->size_bytes < PTR_SIZE) &&
           ir->dest != ir->src0 && ir->dest >= 0 && ir->dest < REG_CNT &&
           ir->src0 >= 0 && ir->src0 < REG_CNT;
}

/* Redundant move elimination: a move, load or constant load whose register is
 * immediately overwritten by another one is dead. {mov rd, rs1; mov rd, rs2},
 * {load rd, ofs; mov rd, rs}, {li rd, imm; load rd, ofs} and the other
 * combinations all reduce to their second instruction.
 *
 * The survivor is the second instruction in full. Carrying over only its opcode
 * and sources kept the first one's width and signedness, and x86-64 narrows a
 * move by those: a move of a stack address that inherited the flags of an
 * overwritten unsigned int constant kept only its low 32 bits. A constant's
 * high word in src1 was likewise dropped.
 */
bool redundant_move_elim(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *next = mop->next;
    if (!next)
        return false;

    /* A volatile load is an access the program performs, not only a value it
     * computes, so it stays even when its register is overwritten unread.
     */
    if (!is_plain_register_def(mop) || !is_plain_register_def(next) ||
        mop->is_volatile || mop->dest != next->dest ||
        (mop->dest_hi >= 0 && mop->dest_hi != next->dest_hi) ||
        ir_reads_reg(next, mop->dest) || ir_reads_reg(next, mop->dest_hi))
        return false;

    memcpy(mop, next, sizeof(vir_mop_t));
    mop_drop_after(bb, mop, mop);
    return true;
}

/* {t = r; op ..., t, ...} -> {op ..., r, ...} when t dies there. A post
 * increment ("i++" keeps i's old value in a temporary) and argument staging
 * leave such copies in every loop; forwarding the source drops the move.
 *
 * Only a plain full-width copy qualifies: a register pair, or an unsigned
 * narrowing copy that x86-64 emits as a zero extension, changes the value.
 */
bool copy_forward(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *next = mop->next;
    int t = mop->dest;
    int r = mop->src0;
    bool used = false;

    if (!next || !is_full_register_copy(mop))
        return false;
    if (next->src0_hi >= 0 || next->src1_hi >= 0 || next->dest_hi >= 0)
        return false;

    /* Only instructions that read exactly the registers they name. A call reads
     * its argument registers without naming them, and several opcodes keep
     * something other than a register in src0; the copy may stage an argument,
     * or its number may merely match an unrelated field.
     */
    if (!op_property(next->op, OP_COPY_FORWARD_SAFE))
        return false;
    /* The copy has to die here, unless the instruction itself rewrites it. */
    if (!(op_writes_dest(next->op) && next->dest == t) &&
        reg_read_after(bb, next, t))
        return false;

    /* Backends lower "rd = op a, b" as "mov rd, a; op rd, b", so an instruction
     * that writes r may read r only as its first operand: "t = r; r = sub a, t"
     * must not become "r = sub a, r".
     */
    if (op_writes_dest(next->op) && next->dest == r &&
        op_src1_is_reg(next->op) && next->src1 == t)
        return false;
    if (op_src0_is_reg(next->op) && next->src0 == t) {
        next->src0 = r;
        used = true;
    }
    if (op_src1_is_reg(next->op) && next->src1 == t) {
        next->src1 = r;
        used = true;
    }
    if (!used)
        return false;
    memcpy(mop, next, sizeof(vir_mop_t));
    mop_drop_after(bb, mop, mop);
    return true;
}

/* {t = r; li r, K; [t] = r} -> {li t, K; [r] = t} when both die at the store.
 * The copy exists only because the value was loaded into the register the
 * address was in; swapping the two roles drops it, and leaves the address
 * feeding the store directly, where a backend can fold it into the access.
 */
bool store_copy_swap(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *li = mop->next;
    vir_mop_t *wr = li ? li->next : NULL;
    int t = mop->dest;
    int r = mop->src0;

    if (!wr || !is_full_register_copy(mop))
        return false;
    if (li->op != OP_load_constant || li->dest != r || li->dest_hi >= 0)
        return false;
    if (wr->op != OP_write || wr->src0 != t || wr->src1 != r ||
        wr->src0_hi >= 0 || wr->src1_hi >= 0)
        return false;
    if (reg_read_after(bb, wr, t) || reg_read_after(bb, wr, r))
        return false;

    memcpy(mop, li, sizeof(vir_mop_t));
    mop->dest = t;
    wr->src0 = r;
    wr->src1 = t;
    mop_drop_after(bb, mop, mop);
    return true;
}

/* Load/store elimination for consecutive memory operations. Removes redundant
 * loads and dead stores that access the same memory location. Conservative
 * implementation to maintain bootstrap stability.
 */
bool eliminate_load_store_pairs(basic_block_t *bb, vir_mop_t *mop)
{
    vir_mop_t *next = mop->next;
    if (!next)
        return false;

    /* Only handle local loads/stores for now (not globals) to be safe */

    /* Pattern 1: Consecutive stores to same local location {store [addr], val1;
     * store [addr], val2} → {store [addr], val2} First store is dead if
     * immediately overwritten
     */
    if (mop->op == OP_store && next->op == OP_store) {
        /* Same slot written twice in a row: only the second is observable.
         * Rewrite this one into the second and drop it, which keeps the list
         * links the caller is holding valid.
         *
         * Only at equal width. A wide store followed by a narrow one to the
         * same slot leaves the bytes the second does not cover holding what the
         * first put there, so dropping the first loses them.
         */
        if (mop->src1 == next->src1 && mop->src1 >= 0 && !mop->is_volatile &&
            !next->is_volatile && mop->size_bytes == next->size_bytes &&
            mop->is_pointer == next->is_pointer &&
            mop->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            mop->src0 = next->src0;
            mop->size_bytes = next->size_bytes;
            mop->is_pointer = next->is_pointer;
            mop_drop_after(bb, mop, next);
            return true;
        }
    }

    /* None of the rewrites below may turn a volatile load into a copy: the
     * second access is as much a side effect as the first.
     */
    if (next->is_volatile)
        return false;

    /* Reuse only an identical load. Width, extension and frame origin affect
     * the loaded value even when the encoded offsets match.
     */
    if (mop->op == next->op &&
        (mop->op == OP_load || mop->op == OP_global_load) &&
        mop->src0 == next->src0 && mop->src1 == next->src1 &&
        (mop->op == OP_global_load || (mop->src0 >= 0 && mop->src1 >= 0)) &&
        mop->size_bytes == next->size_bytes &&
        mop->is_unsigned == next->is_unsigned &&
        mop->is_pointer == next->is_pointer &&
        mop->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
        next->op = OP_assign;
        next->src0 = mop->dest;
        next->src1 = 0;
        return true;
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
    if (mop->op == OP_store && next->op == OP_load) {
        if (mop->src1 == next->src0 && mop->src0 >= 0 &&
            mop->size_bytes == next->size_bytes &&
            mop->is_pointer == next->is_pointer &&
            mop->size_bytes >= PTR_SIZE &&
            mop->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            next->op = OP_assign;
            next->src0 = mop->src0; /* the register that was stored */
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
    if (mop->op == OP_load && next->op == OP_store) {
        if (mop->dest == next->src0 && mop->src0 == next->src1 &&
            mop->src0 >= 0 && mop->size_bytes == next->size_bytes &&
            mop->is_pointer == next->is_pointer &&
            mop->ofs_based_on_stack_top == next->ofs_based_on_stack_top) {
            mop_drop_after(bb, mop, next);
            return true;
        }
    }

    /* Pattern 5: Global store/load optimizations (carefully enabled) */
    if (mop->op == OP_global_store && next->op == OP_global_store) {
        /* Consecutive global stores to same location */
        if (mop->src0 == next->src0 && mop->src1 == next->src1) {
            /* Remove first store - it's dead */
            mop->dest = next->dest;
            mop_drop_after(bb, mop, next);
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
bool strength_reduction(basic_block_t *bb, vir_mop_t *mop)
{
    if (!mop || !mop->next)
        return false;

    vir_mop_t *next = mop->next;

    /* Check for constant load followed by division or modulo */
    if (mop->op != OP_load_constant)
        return false;

    /* Each rewrite below loads a different constant into the register, which
     * const_fold_ok() allows only once nothing else wants the original.
     */
    if (next->op != OP_div && next->op != OP_mod && next->op != OP_mul)
        return false;
    if (next->src0 != mop->dest && next->src1 != mop->dest)
        return false;

    int value = mop->src0;

    /* Check if value is a power of 2. src1 is the high word of an eight-byte
     * constant, and 0x100000004ULL is no power of two.
     */
    if (mop->src1 || value <= 0 || (value & (value - 1)) != 0)
        return false;
    if (!const_fold_ok(bb, mop, next))
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
        next->src1 == mop->dest) {
        /* Convert division to right shift */
        mop->src0 = shift; /* Load shift amount instead */
        next->op = OP_rshift;
        return true;
    }

    /* Pattern 2: Modulo by power of 2 → bitwise AND x % 2^n = x & (2^n - 1) */
    if (next->op == OP_mod && next->src0_is_unsigned &&
        next->src1 == mop->dest) {
        /* Convert modulo to bitwise AND */
        mop->src0 = value - 1; /* Load mask (2^n - 1) */
        next->op = OP_bit_and;
        return true;
    }

    /* Pattern 3: Multiplication by power of 2 → left shift
     * x * 2^n = x << n
     */
    if (next->op == OP_mul) {
        if (next->src0 == mop->dest) {
            /* 2^n * x = x << n */
            mop->src0 = shift; /* Load shift amount */
            next->op = OP_lshift;
            next->src0 = next->src1; /* Move x to src0 */
            next->src1 = mop->dest;  /* Shift amount in src1 */
            return true;
        } else if (next->src1 == mop->dest) {
            /* x * 2^n = x << n */
            mop->src0 = shift; /* Load shift amount */
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
bool bitwise_optimization(basic_block_t *bb, vir_mop_t *mop)
{
    if (!mop || !mop->next)
        return false;

    vir_mop_t *next = mop->next;

    /* Pattern 1: Double complement → identity ~(~x) = x. The first complement
     * no longer writes its register, so nothing else may read it.
     */
    if (mop->op == OP_bit_not && next->op == OP_bit_not &&
        next->src0 == mop->dest && !fold_loses_dest(bb, mop, next)) {
        return replace_with_move(bb, mop, next, mop->src0);
    }

    /* Pattern 2: AND with zero → zero x & 0 = 0, and OR with all-ones →
     * all-ones x | 0xFFFFFFFF = 0xFFFFFFFF. The constant load is retargeted to
     * the result and the operation goes. An eight-byte OR needs the high word,
     * src1, all ones as well. The identities x & -1, x | 0, x ^ 0 and x << 0
     * belong to insn_fusion(), which tries them first.
     */
    if (mop->op == OP_load_constant &&
        ((mop->src0 == 0 && mop->src1 == 0 && next->op == OP_bit_and) ||
         (mop->src0 == -1 && mop->src1 == (next->size_bytes > 4 ? -1 : 0) &&
          next->op == OP_bit_or)) &&
        (next->src0 == mop->dest || next->src1 == mop->dest) &&
        const_fold_ok(bb, mop, next)) {
        mop->dest = next->dest;
        mop_drop_after(bb, mop, next);
        return true;
    }

    return false;
}

/* Triple pattern optimization: Handle 3-instruction sequences These patterns
 * are more complex but offer significant optimization opportunities Returns
 * true if optimization was applied
 */
bool triple_pattern_optimization(basic_block_t *bb, vir_mop_t *mop)
{
    if (!mop || !mop->next || !mop->next->next)
        return false;

    vir_mop_t *second = mop->next;
    vir_mop_t *third = second->next;

    /* Pattern 1: Store-load-store elimination {store val1, addr; load r, addr;
     * store val2, addr} The middle load is pointless if not used elsewhere
     */
    if (mop->op == OP_store && second->op == OP_load && !second->is_volatile &&
        third->op == OP_store && mop->src1 == second->src0 && /* same address */
        mop->dest == second->src1 &&                          /* same offset */
        second->src0 == third->src1 &&                        /* same address */
        second->src1 == third->dest) {                        /* same offset */
        /* Only when nothing reads the loaded value, the third store included:
         * without the load the register keeps what it held before.
         */
        if (!reg_read_after(bb, second, second->dest)) {
            /* The load result is not used, can eliminate it */
            mop->next = third;
            return true;
        }
    }

    /* Pattern 2: Consecutive stores to same location {store v1, addr; store v2,
     * addr; store v3, addr} Only the last store matters
     */
    if (mop->op == OP_store && second->op == OP_store &&
        third->op == OP_store && !mop->is_volatile && !second->is_volatile &&
        !third->is_volatile && mop->src1 == second->src1 &&
        mop->dest == second->dest && second->src1 == third->src1 &&
        second->dest == third->dest) {
        /* All three stores go to the same location Only the last one matters,
         * eliminate first two
         */
        mop->src0 = third->src0;        /* Use last value */
        mop_drop_after(bb, mop, third); /* Skip middle stores */
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

/* Apply the first rule that matches the window starting at @ir, and report
 * whether one did. Every rewrite leaves @ir in place and changes what follows
 * it, so the caller tries the same position again.
 */
bool peephole_at(basic_block_t *bb, vir_mop_t *ir)
{
    vir_mop_t *next = ir->next;
    if (!next)
        return false;

    /* Remove moves to themselves and unused scalar literals. */
    if ((next->op == OP_assign && next->dest == next->src0 &&
         next->dest_hi == next->src0_hi) ||
        (next->op == OP_load_constant && next->dest_hi < 0 &&
         !reg_read_after(bb, next, next->dest))) {
        mop_drop_after(bb, ir, next);
        return true;
    }

    if (PTR_SIZE < 8 &&
        (mop_has_pair(ir) || mop_has_pair(next) || mop_has_pair(next->next)))
        return pair_insn_fusion(bb, ir);

    return triple_pattern_optimization(bb, ir) || insn_fusion(bb, ir) ||
           strength_reduction(bb, ir) || bitwise_optimization(bb, ir) ||
           store_copy_swap(bb, ir) || copy_forward(bb, ir) ||
           redundant_move_elim(bb, ir) || eliminate_load_store_pairs(bb, ir);
}

/* Main peephole optimization driver.
 *
 * This runs on vir_mop_t, after register allocation, and so sees only what
 * assigning registers makes visible. Constant folding, common subexpression
 * elimination and dead code elimination have already run over VIR and are not
 * repeated here.
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
    FOR_EACH_FUNCTION_BODY(func)
    {
        /* Local peephole optimizations on post-register-allocation IR. A
         * rewrite can expose another at the same position -- a dead load
         * dropped in front of a copy leaves the copy there to be forwarded --
         * so each position is retried, a bounded number of times in case a rule
         * reports a change that does not shrink the window.
         */
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next) {
                for (int tries = 0; tries < 4 && peephole_at(bb, ir); tries++)
                    ;
            }
        }
    }
}
