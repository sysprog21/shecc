/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include <stdbool.h>

#include "defs.h"
#include "globals.c"

/* Share peephole rewrite and register-write properties in one table. */
enum {
    OP_WRITES_DEST = 1,
    OP_SRC0_REG = 2,
    OP_SRC1_REG = 4,
    OP_SRC2_REG = 8,
    OP_COPY_FORWARD_SAFE = 16,
    OP_FUSIBLE = 32,
    OP_BINARY_ALU = 64,
    OP_INTEGER_BINARY = 128,
    OP_COMPARISON = 256,
    OP_SCALAR_UNARY = 512,
    OP_SCALAR_CAST = 1024,
};

#define OP_PROPERTY(op, flags) [op] = flags,
#define OP_DEST(flags) ((flags) | OP_WRITES_DEST)
#define OP_COPY(flags) ((flags) | OP_COPY_FORWARD_SAFE)
#define OP_FUSE(flags) ((flags) | OP_FUSIBLE)
#define OP_COPY_FUSE(flags) ((flags) | OP_COPY_FORWARD_SAFE | OP_FUSIBLE)
#define OP_BINARY (OP_SRC0_REG | OP_SRC1_REG | OP_BINARY_ALU)
#define OP_UNARY OP_SRC0_REG
#define OP_INTEGER (OP_BINARY | OP_INTEGER_BINARY)
/* Each macro expands to an initializer and comma. */
/* clang-format off */
static const unsigned short op_properties[] = {
    OP_PROPERTY(OP_generic, 0)
    OP_PROPERTY(OP_cmov, OP_DEST(OP_SRC0_REG | OP_SRC1_REG | OP_SRC2_REG))
    OP_PROPERTY(OP_define, 0)
    OP_PROPERTY(OP_push, OP_SRC0_REG)
    OP_PROPERTY(OP_call, 0)
    OP_PROPERTY(OP_indirect, OP_SRC0_REG)
    OP_PROPERTY(OP_return, OP_SRC0_REG)
    OP_PROPERTY(OP_va_start, 0)
    OP_PROPERTY(OP_allocat, 0)
    OP_PROPERTY(OP_assign, OP_DEST(OP_COPY(OP_SRC0_REG)))
    OP_PROPERTY(OP_load_constant, OP_DEST(0))
    OP_PROPERTY(OP_load_data_address, OP_DEST(OP_FUSE(0)))
    OP_PROPERTY(OP_load_rodata_address, OP_DEST(OP_FUSE(0)))
    OP_PROPERTY(OP_branch, OP_COPY(OP_SRC0_REG))
    OP_PROPERTY(OP_jump, 0)
    OP_PROPERTY(OP_func_ret, 0)
    OP_PROPERTY(OP_address_of_func, OP_SRC0_REG)
    OP_PROPERTY(OP_load_func, OP_SRC0_REG)
    OP_PROPERTY(OP_global_load_func, OP_SRC0_REG)
    OP_PROPERTY(OP_address_of, OP_DEST(0))
    OP_PROPERTY(OP_global_address_of, OP_DEST(0))
    OP_PROPERTY(OP_load, OP_DEST(OP_FUSE(0)))
    OP_PROPERTY(OP_global_load, OP_DEST(OP_FUSE(0)))
    OP_PROPERTY(OP_store, OP_COPY(OP_SRC0_REG))
    OP_PROPERTY(OP_global_store, OP_COPY(OP_SRC0_REG))
    OP_PROPERTY(OP_read, OP_DEST(OP_COPY(OP_SRC0_REG)))
    OP_PROPERTY(OP_write, OP_COPY(OP_SRC0_REG | OP_SRC1_REG))
    OP_PROPERTY(OP_add, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_sub, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_mul, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_div, OP_DEST(OP_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_mod, OP_DEST(OP_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_ternary, OP_DEST(OP_SRC0_REG))
    OP_PROPERTY(OP_lshift, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_rshift, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_log_and, OP_DEST(OP_FUSE(OP_UNARY)))
    OP_PROPERTY(OP_log_or, OP_DEST(OP_FUSE(OP_UNARY)))
    OP_PROPERTY(OP_log_not, OP_DEST(OP_COPY_FUSE(OP_UNARY | OP_SCALAR_UNARY)))
    OP_PROPERTY(OP_eq, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_neq, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_lt, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_leq, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_gt, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_geq, OP_DEST(OP_COPY_FUSE(OP_BINARY | OP_COMPARISON)))
    OP_PROPERTY(OP_bit_or, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_bit_and, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_bit_xor, OP_DEST(OP_COPY_FUSE(OP_INTEGER)))
    OP_PROPERTY(OP_bit_not, OP_DEST(OP_COPY_FUSE(OP_UNARY | OP_SCALAR_UNARY)))
    OP_PROPERTY(OP_negate, OP_DEST(OP_COPY_FUSE(OP_UNARY | OP_SCALAR_UNARY)))
    OP_PROPERTY(OP_trunc, OP_DEST(OP_COPY(OP_SRC0_REG | OP_SCALAR_CAST)))
    OP_PROPERTY(OP_sign_ext, OP_DEST(OP_COPY(OP_SRC0_REG | OP_SCALAR_CAST)))
    OP_PROPERTY(OP_cast, OP_DEST(OP_SRC0_REG | OP_SCALAR_CAST))
    OP_PROPERTY(OP_start, 0)
};
/* clang-format on */
#undef OP_COPY
#undef OP_FUSE
#undef OP_COPY_FUSE
#undef OP_BINARY
#undef OP_INTEGER
#undef OP_UNARY
#undef OP_DEST
#undef OP_PROPERTY
typedef char op_property_count_check
    [sizeof(op_properties) / sizeof(op_properties[0]) == OP_start + 1 ? 1 : -1];

static unsigned int op_property(opcode_t op, unsigned int property)
{
    if (op < OP_generic || op > OP_start)
        return 0;
    return op_properties[op] & property;
}

bool op_writes_dest(opcode_t op)
{
    return op_property(op, OP_WRITES_DEST) != 0;
}
bool op_is_binary_alu(opcode_t op)
{
    return op_property(op, OP_BINARY_ALU);
}
bool op_is_integer_binary(opcode_t op)
{
    return op_property(op, OP_INTEGER_BINARY);
}
bool op_is_comparison(opcode_t op)
{
    return op_property(op, OP_COMPARISON);
}
bool op_is_scalar_unary(opcode_t op)
{
    return op_property(op, OP_SCALAR_UNARY);
}
bool op_is_scalar_cast(opcode_t op)
{
    return op_property(op, OP_SCALAR_CAST);
}
bool op_src0_is_reg(opcode_t op)
{
    return op_property(op, OP_SRC0_REG);
}
bool op_src1_is_reg(opcode_t op)
{
    return op_property(op, OP_SRC1_REG);
}
bool op_src2_is_reg(opcode_t op)
{
    return op_property(op, OP_SRC2_REG);
}

int func_highest_used_reg(func_t *func, int first_callee_saved)
{
    int top = first_callee_saved - 1;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next)
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
            if (op_writes_dest(ir->op) && ir->dest > top)
                top = ir->dest;
            if (op_src0_is_reg(ir->op) && ir->src0 > top)
                top = ir->src0;
            if (op_src1_is_reg(ir->op) && ir->src1 > top)
                top = ir->src1;
            if (op_src2_is_reg(ir->op) && ir->src2 > top)
                top = ir->src2;
        }
    return top < REG_CNT ? top : REG_CNT - 1;
}

/* These instructions can write their result straight to a following move's
 * destination, eliminating the intermediate register copy.
 */
bool is_fusible_insn(const ph2_ir_t *ir)
{
    return op_property(ir->op, OP_FUSIBLE);
}

int call_arg_regs(ph2_ir_t *ir);

/* Whether @ir reads @reg as an operand. A 32-bit target names the high half of
 * a wide operand in src0_hi or src1_hi, which are -1 when unused. A call reads
 * its argument registers without naming any of them.
 */
bool ir_reads_reg(ph2_ir_t *ir, int reg)
{
    if ((ir->op == OP_call || ir->op == OP_indirect) && reg >= 0 &&
        reg < MAX_ARGS_IN_REG && reg < call_arg_regs(ir))
        return true;
    if (reg >= 0 && (ir->src0_hi == reg || ir->src1_hi == reg))
        return true;
    if (op_src2_is_reg(ir->op) && ir->src2 == reg)
        return true;
    if (op_src0_is_reg(ir->op) && ir->src0 == reg)
        return true;
    return op_src1_is_reg(ir->op) && ir->src1 == reg;
}

/* Calls read their ABI arguments before clobbering the caller registers. */
unsigned int ph2_ir_defs(ph2_ir_t *ir)
{
    unsigned int defs = 0;
    if (op_writes_dest(ir->op)) {
        if (ir->dest >= 0 && ir->dest < REG_CNT)
            defs |= 1u << ir->dest;
        if (ir->dest_hi >= 0 && ir->dest_hi < REG_CNT)
            defs |= 1u << ir->dest_hi;
    }
    if (ir->op == OP_call || ir->op == OP_indirect)
        defs |= (1u << (REG_CNT - CALLEE_SAVED_REGS)) - 1;
    return defs;
}

/* Allocated registers can cross machine edges. Three block masks suffice:
 * live-in is use | (live-out & ~def), including both halves of a pair.
 */
void ph2_compute_liveness(void)
{
    FOR_EACH_FUNCTION_BODY(func)
    {
        int count = 0;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next)
            count++;
        basic_block_t **blocks = malloc(count * sizeof(*blocks));
        if (!blocks)
            fatal("Machine liveness allocation failed");
        int index = 0;
        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            blocks[index++] = bb;
            bb->machine_use = bb->machine_def = bb->machine_liveout = 0;
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                for (int reg = 0; reg < REG_CNT; reg++)
                    if (ir_reads_reg(ir, reg))
                        bb->machine_use |= (1u << reg) & ~bb->machine_def;
                bb->machine_def |= ph2_ir_defs(ir);
            }
        }
        bool changed;
        do {
            changed = false;
            for (int at = count - 1; at >= 0; at--) {
                basic_block_t *bb = blocks[at];
                basic_block_t *successors[3] = {bb->next, bb->then_, bb->else_};
                unsigned int out = 0;
                for (int edge = 0; edge < 3; edge++) {
                    basic_block_t *next = successors[edge];
                    if (next)
                        out |= next->machine_use |
                               (next->machine_liveout & ~next->machine_def);
                }
                if (out != bb->machine_liveout) {
                    bb->machine_liveout = out;
                    changed = true;
                }
            }
        } while (changed);
        free(blocks);
    }
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

/* Scan local reads and definitions, then consult the successor live set. */
static bool reg_read_after(basic_block_t *bb, ph2_ir_t *ir, int reg)
{
    if (reg < 0)
        return false;
    if (reg >= REG_CNT)
        return true;
    for (ph2_ir_t *p = ir->next; p; p = p->next) {
        if (ir_reads_reg(p, reg))
            return true;
        if (ph2_ir_defs(p) & (1u << reg))
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

    /* The fused operation no longer writes its own pair, so that pair must be
     * dead after the move. Common subexpression elimination leaves exactly the
     * copy that is not: "(a & b) ^ (b & a)" became "x = a & b; y = x; x ^ y",
     * and fusing the move left the XOR reading a stale x.
     */
    if ((ph2_ir->dest != next->dest &&
         reg_read_after(bb, next, ph2_ir->dest)) ||
        (ph2_ir->dest_hi != next->dest_hi &&
         reg_read_after(bb, next, ph2_ir->dest_hi)))
        return false;
    ph2_ir->dest = next->dest;
    ph2_ir->dest_hi = next->dest_hi;
    ph2_ir_drop_after(bb, ph2_ir, next);
    return true;
}

static bool replace_with_move(basic_block_t *bb,
                              ph2_ir_t *ir,
                              ph2_ir_t *next,
                              int source)
{
    ir->op = OP_assign;
    ir->src0 = source;
    ir->dest = next->dest;
    ph2_ir_drop_after(bb, ir, next);
    return true;
}

bool insn_fusion(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* Fuse result moves and fold constant identities. */
    if (next->op == OP_assign) {
        if (is_fusible_insn(ph2_ir) && ph2_ir->dest == next->src0 &&
            !fold_loses_dest(bb, ph2_ir, next)) {
            /* Do not remove a temporary that still has another live use. */
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
            int non_zero_src =
                (ph2_ir->dest == next->src0) ? next->src1 : next->src0;

            return replace_with_move(bb, ph2_ir, next, non_zero_src);
        }

        if (next->op == OP_sub && const_fold_ok(bb, ph2_ir, next)) {
            if (ph2_ir->dest == next->src1) {
                return replace_with_move(bb, ph2_ir, next, next->src0);
            }

            if (ph2_ir->dest == next->src0) {
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
            return replace_with_move(
                bb, ph2_ir, next,
                ph2_ir->dest == next->src0 ? next->src1 : next->src0);
        }
    }

    /* Bitwise identity operations. src1 is the constant's high word, which an
     * eight-byte operation includes: 0xffffffffULL is no identity for it.
     */
    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == -1 &&
        ph2_ir->src1 == (next->size_bytes > 4 ? -1 : 0) &&
        next->op == OP_bit_and && ph2_ir->dest == next->src1 &&
        const_fold_ok(bb, ph2_ir, next)) {
        return replace_with_move(bb, ph2_ir, next, next->src0);
    }

    if (ph2_ir->op == OP_load_constant && ph2_ir->src0 == 0 &&
        ph2_ir->src1 == 0 &&
        (next->op == OP_lshift || next->op == OP_rshift ||
         next->op == OP_bit_or || next->op == OP_bit_xor) &&
        ph2_ir->dest == next->src1 && const_fold_ok(bb, ph2_ir, next))
        return replace_with_move(bb, ph2_ir, next, next->src0);

    return false;
}

/* Whether @ir does nothing but set its destination register: a move, a slot
 * load or a constant load.
 */
bool is_plain_register_def(const ph2_ir_t *ir)
{
    return ir->op == OP_assign || ir->op == OP_load ||
           ir->op == OP_global_load || ir->op == OP_load_constant;
}

static bool is_full_register_copy(const ph2_ir_t *ir)
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
bool redundant_move_elim(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    if (!next)
        return false;

    /* A volatile load is an access the program performs, not only a value it
     * computes, so it stays even when its register is overwritten unread.
     */
    if (!is_plain_register_def(ph2_ir) || !is_plain_register_def(next) ||
        ph2_ir->is_volatile || ph2_ir->dest != next->dest ||
        (ph2_ir->dest_hi >= 0 && ph2_ir->dest_hi != next->dest_hi) ||
        ir_reads_reg(next, ph2_ir->dest) || ir_reads_reg(next, ph2_ir->dest_hi))
        return false;

    memcpy(ph2_ir, next, sizeof(ph2_ir_t));
    ph2_ir_drop_after(bb, ph2_ir, ph2_ir);
    return true;
}

/* {t = r; op ..., t, ...} -> {op ..., r, ...} when t dies there. A post
 * increment ("i++" keeps i's old value in a temporary) and argument staging
 * leave such copies in every loop; forwarding the source drops the move.
 *
 * Only a plain full-width copy qualifies: a register pair, or an unsigned
 * narrowing copy that x86-64 emits as a zero extension, changes the value.
 */
bool copy_forward(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *next = ph2_ir->next;
    int t = ph2_ir->dest;
    int r = ph2_ir->src0;
    bool used = false;

    if (!next || !is_full_register_copy(ph2_ir))
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
        ((op_src1_is_reg(next->op) && next->src1 == t) ||
         (op_src2_is_reg(next->op) && next->src2 == t)))
        return false;
    if (op_src0_is_reg(next->op) && next->src0 == t) {
        next->src0 = r;
        used = true;
    }
    if (op_src1_is_reg(next->op) && next->src1 == t) {
        next->src1 = r;
        used = true;
    }
    if (op_src2_is_reg(next->op) && next->src2 == t) {
        next->src2 = r;
        used = true;
    }
    if (!used)
        return false;
    memcpy(ph2_ir, next, sizeof(ph2_ir_t));
    ph2_ir_drop_after(bb, ph2_ir, ph2_ir);
    return true;
}

/* {t = r; li r, K; [t] = r} -> {li t, K; [r] = t} when both die at the store.
 * The copy exists only because the value was loaded into the register the
 * address was in; swapping the two roles drops it, and leaves the address
 * feeding the store directly, where a backend can fold it into the access.
 */
bool store_copy_swap(basic_block_t *bb, ph2_ir_t *ph2_ir)
{
    ph2_ir_t *li = ph2_ir->next;
    ph2_ir_t *wr = li ? li->next : NULL;
    int t = ph2_ir->dest;
    int r = ph2_ir->src0;

    if (!wr || !is_full_register_copy(ph2_ir))
        return false;
    if (li->op != OP_load_constant || li->dest != r || li->dest_hi >= 0)
        return false;
    if (wr->op != OP_write || wr->src0 != t || wr->src1 != r ||
        wr->src0_hi >= 0 || wr->src1_hi >= 0)
        return false;
    if (reg_read_after(bb, wr, t) || reg_read_after(bb, wr, r))
        return false;

    memcpy(ph2_ir, li, sizeof(ph2_ir_t));
    ph2_ir->dest = t;
    wr->src0 = r;
    wr->src1 = t;
    ph2_ir_drop_after(bb, ph2_ir, ph2_ir);
    return true;
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
            !ph2_ir->is_volatile && !next->is_volatile &&
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

    /* None of the rewrites below may turn a volatile load into a copy: the
     * second access is as much a side effect as the first.
     */
    if (next->is_volatile)
        return false;

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
        return replace_with_move(bb, ph2_ir, next, ph2_ir->src0);
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
        !second->is_volatile && third->op == OP_store &&
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
        third->op == OP_store && !ph2_ir->is_volatile && !second->is_volatile &&
        !third->is_volatile && ph2_ir->src1 == second->src1 &&
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
/* Apply the first rule that matches the window starting at @ir, and report
 * whether one did. Every rewrite leaves @ir in place and changes what follows
 * it, so the caller tries the same position again.
 */
bool peephole_at(basic_block_t *bb, ph2_ir_t *ir)
{
    ph2_ir_t *next = ir->next;
    if (!next)
        return false;

    /* Self-assignment elimination Keep this as a safety net: SSA handles most
     * cases, but register allocation might create new self-assignments
     */
    if (next->op == OP_assign && next->dest == next->src0 &&
        next->dest_hi == next->src0_hi) {
        ph2_ir_drop_after(bb, ir, next);
        return true;
    }

    if (PTR_SIZE < 8 && (ph2_ir_has_pair(ir) || ph2_ir_has_pair(next) ||
                         ph2_ir_has_pair(next->next)))
        return pair_insn_fusion(bb, ir);

    /* Try triple pattern optimization first (3-instruction sequences) */
    if (triple_pattern_optimization(bb, ir))
        return true;

    /* Try instruction fusion (2-instruction sequences) */
    if (insn_fusion(bb, ir))
        return true;

    /* Apply strength reduction for power-of-2 operations */
    if (strength_reduction(bb, ir))
        return true;

    /* Apply bitwise operation optimizations */
    if (bitwise_optimization(bb, ir))
        return true;

    /* Swap a copied address with the constant stored through it */
    if (store_copy_swap(bb, ir))
        return true;

    /* Forward a copy into the instruction that consumes it */
    if (copy_forward(bb, ir))
        return true;

    /* Apply redundant move elimination */
    if (redundant_move_elim(bb, ir))
        return true;

    /* Apply load/store elimination */
    return eliminate_load_store_pairs(bb, ir);
}

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
            for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                for (int tries = 0; tries < 4 && peephole_at(bb, ir); tries++)
                    ;
            }
        }
    }
}
