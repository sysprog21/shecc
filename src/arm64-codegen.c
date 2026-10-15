/*
 * AArch64 Linux code generator. The allocator's virtual registers map to
 * x0..x7, x9..x15, x20..x28; x16/x17 are reserved scratch and x19 is the global
 * base.
 */
#include "arm64.c"
#include "defs.h"
#include "globals.c"

/* IP0, the linker's own scratch register, is free for a code generator to use
 * between instructions and never allocated to a value. x19 is the base of the
 * synthetic global frame, held for the life of the program.
 */
#define A64_GP 19

/* AAPCS64 keeps SP 16-byte aligned. Save used callee registers in pairs,
 * together with FP/LR and the global base when dynamic main replaces it.
 */
#define A64_STACK_ALIGN 16
#define A64_FIRST_CALLEE (REG_CNT - CALLEE_SAVED_REGS)
#define A64_SAVE_GP 16

/* A leaf with no frame, no saved register and no stack argument: it keeps
 * neither FP nor LR, so it pushes nothing and returns straight to LR.
 */
#define A64_FRAMELESS 32

/* A return that only moves its value into x0 and branches to the first return's
 * epilogue, at the code offset its dest holds, instead of repeating it.
 */
#define A64_SHARED_EXIT 64
#define A64_SHARED_EXIT_WORDS 4

/* The last pair the prologue pushes also allocates the locals, which then sit
 * above it, and the epilogue's first pop frees them again: one STP and one LDP
 * with writeback instead of a separate SUB and ADD. LDP's writeback reaches 504
 * bytes, the pair's 16 and the locals together, and the locals must not start
 * at SP when a call passes arguments there.
 */
#define A64_FOLDED_FRAME 128
#define A64_FOLDED_FRAME_MAX 480

/* A small loop should not straddle a 64-byte fetch block: on the eMAG a
 * five-instruction loop that did ran at four fifths of the speed. Function
 * entries gain nothing measurable from alignment, and no loop is worth more
 * than four no-ops of padding.
 */
#define A64_FETCH_BLOCK 64
#define A64_LOOP_PADDING 16

/* Past the two scratch registers, the value a store writes when it stores zero:
 * as the data register of a load or store, register 31 reads as zero.
 */
#define A64_ZERO (REG_CNT + 2)

/* A comparison's src2 says whether its flags feed the next branch (bit 0) and,
 * above that, the immediate replacing src1 plus A64_CMP_BIAS, or zero for the
 * register form. The bias keeps the field positive for a negative immediate.
 * A64_CMP_TESTED, past the immediate, says that the AND before it already set
 * the flags as ANDS, so that the comparison emits nothing of its own.
 */
#define A64_CMP_FUSED 1
#define A64_CMP_BIAS 4096
#define A64_CMP_TESTED (1 << 14)

/* Added to the src2 of an ADD or SUB immediate that takes LSL #12. */
#define A64_IMM_SHIFTED 4096

/* The option bit that turns a UXTW index of LDR/STR (register) into SXTW. */
#define A64_SXTW_INDEX 0x8000

static int a64_save_bytes(int registers)
{
    return 16 + ALIGN_UP(((registers & 15) + !!(registers & A64_SAVE_GP)) * 8,
                         A64_STACK_ALIGN);
}

/* The packed save list ends with x19 only when main replaces its ABI value. */
static int a64_saved_reg(int saved, int index)
{
    if (index < (saved & 15))
        return 20 + index;
    return index == (saved & 15) && (saved & A64_SAVE_GP) ? A64_GP : A64_ZR;
}

void emit(int insn);
void emit_mop(vir_mop_t *ir);
void a64_mov_imm(int d, int v);
void a64_mov_sp(int d);
void a64_mem(int access, int size, int rt, int rn, int ofs);
void a64_extend(int d, int n, int size, bool is_unsigned);
void a64_bl_addr(int target);

static void arm64_halfword_counts(unsigned long long value,
                                  int *zero_words,
                                  int *one_words)
{
    *zero_words = 0;
    *one_words = 0;
    for (int shift = 0; shift < 64; shift += 16) {
        unsigned int word = (unsigned int) (value >> shift) & 0xffffU;
        *zero_words += word != 0;
        *one_words += word != 0xffffU;
    }
}

static void emit_arm64_wide_const(int reg, unsigned long long value)
{
    int zero_words, one_words, first = -1;
    bool use_movn;

    /* Writing a W register clears the upper half, so a value confined to the
     * low word with one halfword all ones is a single MOVN on it.
     */
    if (!(value >> 32) &&
        ((value & 0xffff) == 0xffff || value >> 16 == 0xffff)) {
        int hw = (value & 0xffff) == 0xffff;
        emit(a64_movn_insn(false, reg, ~(value >> (16 * hw)) & 0xffff, hw));
        return;
    }
    arm64_halfword_counts(value, &zero_words, &one_words);
    use_movn = one_words < zero_words;
    for (int shift = 0; shift < 64; shift += 16) {
        unsigned int word = (unsigned int) (value >> shift) & 0xffffU;
        bool differs = use_movn ? word != 0xffffU : word != 0;

        if (!differs)
            continue;
        if (first < 0) {
            first = shift / 16;
            emit(use_movn ? a64_movn_insn(true, reg, (~word) & 0xffffU, first)
                          : a64_movz_insn(true, reg, word, first));
        } else {
            emit(a64_movk_insn(true, reg, word, shift / 16));
        }
    }
    if (first < 0)
        emit(use_movn ? a64_movn_insn(true, reg, 0, 0)
                      : a64_movz_insn(true, reg, 0, 0));
}

int a64_reg(int r)
{
    if (r == REG_CNT || r == REG_CNT + 1)
        return A64_IP0 + r - REG_CNT;
    if (r == A64_ZERO)
        return A64_ZR;
    return r < 8 ? r : r < A64_FIRST_CALLEE ? r + 1 : r + 5;
}
void emit(int insn)
{
    if (mop_count_only) {
        if (mop_counted_words < INT_MAX)
            mop_counted_words++;
    } else {
        elf_write_int(elf_code, insn);
    }
}
void a64_mov(int d, int n)
{
    if (d != n)
        emit(a64_mov_insn(true, d, n));
}

/* SP is not a general register in logical instructions: ORR would read ZR. ADD
 * #0 is the architectural move spelling when either operand is SP.
 */
void a64_mov_sp(int d)
{
    emit(a64_add_imm_insn(true, d, A64_SP, 0));
}
void a64_mov_imm(int d, int v)
{
    /* Integer constants participate in pointer arithmetic throughout the
     * compiler. A negative C int must therefore become a sign-extended
     * X-register value, not 0x00000000ffffffff-style zero extension. MOVN
     * supplies the upper one bits while MOVK fills the second halfword.
     *
     * Always two instructions: callers pass segment addresses that are not
     * final when code size is estimated, so the length cannot depend on v.
     */
    if (v < 0)
        emit(a64_movn_insn(true, d, ~v, 0));
    else
        emit(a64_movz_insn(true, d, v, 0));
    emit(a64_movk_insn(true, d, v >> 16, 1));
}

/* A machine-operation constant is final before sizes are estimated, so unlike
 * an address it takes only the halfwords it needs; update_elf_offset() counts
 * this very emission. An eight-byte constant keeps its upper word in src1, and
 * a smaller one widens to the X register by its C signedness.
 */
static unsigned long long a64_constant_value(const vir_mop_t *p)
{
    unsigned long long value;

    if (p->size_bytes == 8)
        value = (unsigned int) p->src0 |
                (unsigned long long) (unsigned int) p->src1 << 32;
    else if (p->is_unsigned)
        value = (unsigned int) p->src0;
    else
        value = (unsigned long long) (long long) p->src0;
    return value;
}

static void a64_load_constant(int d, const vir_mop_t *p)
{
    emit_arm64_wide_const(d, a64_constant_value(p));
}

/* Rd = Rn / Rm, unsigned when either operand is. */
int a64_div_insn(vir_mop_t *p, int d, int n, int m)
{
    bool sf = p->size_bytes == 8;

    if (p->src0_is_unsigned || p->src1_is_unsigned)
        return a64_udiv_insn(sf, d, n, m);
    return a64_sdiv_insn(sf, d, n, m);
}

/* Rd = Rn >> Rm, or by the immediate src2 - 1 when src2 is set; logical for an
 * unsigned left operand.
 */
int a64_rshift_insn(vir_mop_t *p, int d, int n, int m)
{
    bool sf = p->size_bytes == 8;

    if (p->src2)
        return a64_shr_imm_insn(sf, p->src0_is_unsigned, d, n, p->src2 - 1);
    if (p->src0_is_unsigned)
        return a64_lsrv_insn(sf, d, n, m);
    return a64_asrv_insn(sf, d, n, m);
}

/* The shift fields of a register operand that a64_fold_shift() shifted, from
 * the negative src2 of @p; zero for a plain register.
 */
static int a64_shifted(vir_mop_t *p)
{
    int code = p->src2 < 0 ? -1 - p->src2 : 0;
    return ((code & 63) << 10) | ((code >> 6) << 22);
}

/* The twelve immediate bits of an addition or subtraction with a positive src2,
 * which arm64_fold_immediates() sets to the immediate plus one, or for one
 * shifted by twelve bits, to A64_IMM_SHIFTED plus the shifted value plus one.
 */
static int a64_imm12(vir_mop_t *p)
{
    return (p->src2 - 1) % A64_IMM_SHIFTED;
}

/* Whether an addition or subtraction takes the X form. VIR arithmetic carries
 * no signedness, so the only int one marked unsigned is one whose result is
 * only zero-extended: a64_coalesce_results() gives it the W form, which leaves
 * it zero-extended already.
 */
static bool a64_arith_wide(vir_mop_t *p)
{
    return p->size_bytes != 4 || !p->is_unsigned;
}

/* SP cannot be an operand of the shifted-register form, so an addition or
 * subtraction involving it takes the extended-register form instead.
 */
void a64_add(int d, int n, int m)
{
    if (d == A64_SP || n == A64_SP)
        emit(a64_add_ext_insn(d, n, m, A64_EXT_UXTX));
    else
        emit(a64_add_reg_insn(true, d, n, m));
}
void a64_sub(int d, int n, int m)
{
    if (d == A64_SP || n == A64_SP)
        emit(a64_sub_ext_insn(d, n, m, A64_EXT_UXTX));
    else
        emit(a64_sub_reg_insn(true, d, n, m));
}

static void a64_adjust_frame(int bytes, bool subtract)
{
    if (!bytes)
        return;
    if (bytes < 1 << 24) {
        /* The upper twelve bits through the shifted form, then the rest. */
        if (bytes >> 12)
            emit((subtract
                      ? a64_sub_imm_insn(true, A64_SP, A64_SP, bytes >> 12)
                      : a64_add_imm_insn(true, A64_SP, A64_SP, bytes >> 12)) |
                 A64_IMM_LSL12);
        if (bytes & 0xfff)
            emit(subtract
                     ? a64_sub_imm_insn(true, A64_SP, A64_SP, bytes & 0xfff)
                     : a64_add_imm_insn(true, A64_SP, A64_SP, bytes & 0xfff));
    } else {
        a64_mov_imm(A64_IP0, bytes);
        if (subtract)
            a64_sub(A64_SP, A64_SP, A64_IP0);
        else
            a64_add(A64_SP, A64_SP, A64_IP0);
    }
}
/* Rd = SP + @ofs for a non-negative @ofs. */
static void a64_add_sp(int d, int ofs)
{
    if (ofs < 4096) {
        emit(a64_add_imm_insn(true, d, A64_SP, ofs));
        return;
    }
    if (ofs < 1 << 24) {
        emit(a64_add_imm_insn(true, d, A64_SP, ofs >> 12) | A64_IMM_LSL12);
        if (ofs & 0xfff)
            emit(a64_add_imm_insn(true, d, d, ofs & 0xfff));
        return;
    }
    a64_mov_imm(A64_IP0, ofs);
    a64_add(d, A64_SP, A64_IP0);
}

void a64_sxtw(int d, int n)
{
    emit(a64_sext_insn(d, n, 32));
}

/* Sign-extend the low @size bytes of Xn into Xd. Always one instruction, so
 * update_elf_offset()'s default estimate stays correct.
 */
void a64_extend(int d, int n, int size, bool is_unsigned)
{
    if (size == 1)
        emit(is_unsigned ? a64_zext_insn(d, n, 8) : a64_sext_insn(d, n, 8));
    else if (size == 2)
        emit(is_unsigned ? a64_zext_insn(d, n, 16) : a64_sext_insn(d, n, 16));
    else if (size == 4) {
        if (is_unsigned)
            emit(a64_zext_insn(d, n, 32));
        else
            a64_sxtw(d, n);
    } else
        a64_mov(d, n);
}

/* Which load reads @p's value. A narrow unsigned one has to arrive
 * zero-extended: its register feeds 64-bit address arithmetic as it stands, and
 * there a sign-extended unsigned char of 200 indexes table[-56].
 */
int a64_load_access(vir_mop_t *p)
{
    return p->is_unsigned ? A64_LOAD_ZEXT : A64_LOAD;
}

/* AArch64's ordinary LDR/STR immediate is scaled by the access width. The
 * compiler deliberately supports packed C layouts, so structure members are
 * often not naturally aligned (func_t.bbs, for example). Such offsets must use
 * the byte-addressed LDUR/STUR form; rounding them down corrupts adjacent
 * fields during self-hosting.
 */
static bool a64_mem_reaches(int size, int ofs)
{
    return (ofs >= -256 && ofs <= 255) ||
           (ofs >= 0 && ofs <= 4095 * size && !(ofs % size));
}

/* Every offset is final when sizes are estimated, so the sequence may depend on
 * it. One out of reach of both immediate forms goes through IP0: its upper bits
 * added as a shifted immediate when the rest is then in reach, and otherwise
 * added whole.
 */
void a64_mem(int access, int size, int rt, int rn, int ofs)
{
    if (size != 1 && size != 2 && size != 4 && size != 8)
        fatal("unsupported arm64 access width");
    if (!a64_mem_reaches(size, ofs)) {
        if (ofs > 0 && ofs < 1 << 24 && a64_mem_reaches(size, ofs & 0xfff)) {
            emit(a64_add_imm_insn(true, A64_IP0, rn, ofs >> 12) |
                 A64_IMM_LSL12);
            ofs &= 0xfff;
        } else {
            emit_arm64_wide_const(A64_IP0, (long long) ofs);
            a64_add(A64_IP0, rn, A64_IP0);
            ofs = 0;
        }
        rn = A64_IP0;
    }
    if (ofs >= -256 && ofs <= 255 && (ofs < 0 || ofs % size))
        emit(a64_mem_unscaled_insn(access, size, rt, rn, ofs));
    else
        emit(a64_mem_scaled_insn(access, size, rt, rn, ofs));
}

/* Masking an out-of-range displacement silently branches somewhere else. The
 * generated image is small enough that these never fire today, so say so rather
 * than let a larger input fail as a wild jump.
 */
void a64_check_disp(int disp, int bits, char *what)
{
    if (mop_count_only)
        return;
    int lim = 1 << (bits - 1);
    if (disp < -lim || disp >= lim)
        fatal(what);
}

/* Branch to @target when @rt is non-zero. CBNZ carries the same 19-bit
 * displacement a B.cond would, so it replaces a compare-and-branch pair
 * outright. @wide picks the X form, which a pointer needs: testing only Wn
 * reads an address whose low word happens to be zero as null.
 */
void a64_cbnz(bool wide, int rt, int target)
{
    int disp = (target - elf_code->size) / 4;
    a64_check_disp(disp, 19, "arm64 conditional branch out of range");
    emit(a64_cbnz_insn(wide, rt, disp));
}
void a64_b(int target)
{
    int d = (target - elf_code->size) / 4;
    a64_check_disp(d, 26, "arm64 branch out of range");
    emit(a64_b_insn(d));
}
void a64_bl_addr(int target)
{
    int d = (target - (elf_code_start + elf_code->size)) / 4;
    a64_check_disp(d, 26, "arm64 call out of range");
    emit(a64_bl_insn(d));
}
int a64_adrp_insn(int d, int pc, int target)
{
    int pages = (target >> 12) - (pc >> 12);
    a64_check_disp(pages, 21, "arm64 ADRP target out of range");
    return a64_adrp_pages_insn(d, pages);
}

/* A comparison is as wide as the values it compares. An address has to be
 * compared whole, an array included, since what reaches the instruction is its
 * decayed base. The source flags say so and is_pointer does not: it counts
 * pointer-like operands alone, for the sake of the 32-bit targets that read it.
 */
bool a64_cmp_wide(vir_mop_t *p)
{
    return p->size_bytes == 8 || p->src0_is_pointer || p->src1_is_pointer;
}

a64_cond_t a64_cond(opcode_t op, bool is_unsigned)
{
    switch (op) {
    case OP_eq:
        return A64_EQ;
    case OP_neq:
        return A64_NE;
    case OP_geq:
        return is_unsigned ? A64_HS : A64_GE;
    case OP_lt:
        return is_unsigned ? A64_LO : A64_LT;
    case OP_gt:
        return is_unsigned ? A64_HI : A64_GT;
    default:
        return is_unsigned ? A64_LS : A64_LE;
    }
}

/* Whether control leaving @bb at its end lands in @target: the next block in
 * layout, or one behind blocks that emit nothing, such as the arms a select
 * diamond emptied.
 */
static bool a64_falls_into(basic_block_t *bb, basic_block_t *target)
{
    for (basic_block_t *next = bb->rpo_next; next; next = next->rpo_next) {
        if (next == target)
            return true;
        if (next->mops.head)
            return false;
    }
    return false;
}

/* Which successor of a branch, if any, control falls into after @bb. */
static int a64_branch_fallthrough(basic_block_t *bb, vir_mop_t *ir)
{
    return a64_falls_into(bb, ir->else_bb)   ? 1
           : a64_falls_into(bb, ir->then_bb) ? 2
                                             : 0;
}

static bool a64_branch_only_compare(basic_block_t *bb, vir_mop_t *ir)
{
    return op_is_comparison(ir->op) && ir->next && ir->next->op == OP_branch &&
           ir->next->src0 == ir->dest &&
           !(bb->machine_liveout & (1U << ir->dest));
}

/* Turn the branch after an equality test against zero into CBZ or CBNZ on the
 * tested register, which makes @compare redundant. The branch reads its operand
 * at the comparison's width, and an equality test swaps the successors, since
 * the branch is taken on a nonzero operand.
 */
static bool a64_branch_on_zero(vir_mop_t *compare, vir_mop_t *branch)
{
    if ((compare->op != OP_eq && compare->op != OP_neq) ||
        compare->src2 >> 1 != A64_CMP_BIAS)
        return false;
    branch->src0 = compare->src0;
    branch->size_bytes = a64_cmp_wide(compare) ? 8 : 4;
    branch->src0_is_pointer = false;
    if (compare->op == OP_eq) {
        basic_block_t *then_bb = branch->then_bb;
        branch->then_bb = branch->else_bb;
        branch->else_bb = then_bb;
    }
    return true;
}

/* Count the actual instruction sequence so offset planning stays in step with
 * the emitter as it changes.
 */
void update_elf_offset(vir_mop_t *ir)
{
    if (ir->op != OP_allocat)
        elf_offset += mop_count_words(ir) * 4;
}

/* Registers @f may name anywhere, over-approximated by every operand field. */
static unsigned int a64_registers_named(func_t *f)
{
    unsigned int named = 0;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next) {
            int fields[] = {ir->dest,    ir->src0,    ir->src1,
                            ir->dest_hi, ir->src0_hi, ir->src1_hi};
            for (int i = 0; i < 6; i++)
                if (fields[i] >= 0 && fields[i] < REG_CNT)
                    named |= 1u << fields[i];
        }
    return named;
}

/* The registers a64_hoist_constants() gave the current function's literals, and
 * the literal each holds from its entry on.
 */
static unsigned int a64_lanes;
static unsigned long long a64_lane_values[REG_CNT];

/* Loop literals use otherwise idle callee-saved registers. Their definitions
 * move to function entry, unless a successor or call needs the literal in its
 * own register. Values and addresses outside this literal-only pass are left to
 * the shared lowerer.
 */
static void a64_hoist_constants(func_t *f)
{
    int count = 0, lanes = 0;
    vir_mop_t *constants[CALLEE_SAVED_REGS];
    int lane[CALLEE_SAVED_REGS];

    a64_lanes = 0;

    /* A leaf keeps its literals in caller-saved registers it never names, which
     * need no save; anything else uses idle callee-saved ones.
     */
    if (!func_makes_call(f)) {
        unsigned int named = a64_registers_named(f);
        for (int reg = A64_FIRST_CALLEE - 1;
             reg > 0 && lanes < CALLEE_SAVED_REGS; reg--)
            if (!(named & (1u << reg)))
                lane[lanes++] = reg;
    } else
        for (int reg = func_highest_used_reg(f, A64_FIRST_CALLEE) + 1;
             reg < REG_CNT; reg++)
            lane[lanes++] = reg;
    if (!lanes)
        return;

    /* Literals leave loop bodies only: the blocks that reach a back edge
     * without passing the header it returns to. A literal after a loop's exit,
     * or between two loops, would hold a lane across the loop for nothing.
     * Blocks keep their order along rpo_next, so one backward sweep from each
     * back edge finds its body.
     */
    int blocks = 0;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        if (bb->rpo >= blocks)
            blocks = bb->rpo + 1;
    basic_block_t **order = calloc(blocks, sizeof(*order));
    char *body = calloc(blocks, 1), *in_loop = calloc(blocks, 1);
    if (!order || !body || !in_loop)
        fatal("Out of memory");
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        order[bb->rpo] = bb;
    for (basic_block_t *latch = f->bbs; latch; latch = latch->rpo_next) {
        basic_block_t *exits[3] = {latch->next, latch->then_, latch->else_};
        for (int e = 0; e < 3; e++) {
            basic_block_t *header = exits[e];
            if (!header || header->rpo > latch->rpo)
                continue;
            for (int i = 0; i < blocks; i++)
                body[i] = 0;
            body[latch->rpo] = 1;
            for (int i = latch->rpo - 1; i >= header->rpo; i--) {
                basic_block_t *bb = order[i];
                if (!bb)
                    continue;
                basic_block_t *targets[3] = {bb->next, bb->then_, bb->else_};
                for (int t = 0; t < 3; t++)
                    if (targets[t] && targets[t] != header &&
                        targets[t]->rpo > i && targets[t]->rpo <= latch->rpo &&
                        body[targets[t]->rpo])
                        body[i] = 1;
            }
            body[header->rpo] = 1;
            for (int i = header->rpo; i <= latch->rpo; i++)
                in_loop[i] |= body[i];
        }
    }
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        if (!in_loop[bb->rpo])
            continue;
        vir_mop_t **link = &bb->mops.head, *previous = NULL;
        while (*link) {
            vir_mop_t *ir = *link;
            if (ir->op != OP_load_constant) {
                previous = ir;
                link = &ir->next;
                continue;
            }

            /* A literal that has to stay in its own register, for a successor
             * or across a call, would only become a copy of its lane, which is
             * no faster and costs the lane.
             */
            int old = ir->dest;
            bool copy = (bb->machine_liveout & (1u << old)) != 0;
            for (vir_mop_t *use = ir->next; !copy && use; use = use->next) {
                if (use->op == OP_call || use->op == OP_indirect) {
                    copy = true;
                    break;
                }
                if (op_writes_dest(use->op) && use->dest == old)
                    break;
            }
            unsigned long long value = a64_constant_value(ir);
            int index = 0;
            while (index < count &&
                   a64_constant_value(constants[index]) != value)
                index++;
            if (copy || (index == count && count >= lanes)) {
                previous = ir;
                link = &ir->next;
                continue;
            }
            if (index == count) {
                vir_mop_t *setup = mop_create(OP_load_constant);
                *setup = *ir;
                setup->dest = lane[count];
                setup->next = NULL;
                if (count)
                    constants[count - 1]->next = setup;
                constants[count++] = setup;
            }
            int reg = constants[index]->dest;
            for (vir_mop_t *use = ir->next; use; use = use->next) {
                if (op_src0_is_reg(use->op) && use->src0 == old)
                    use->src0 = reg;
                if (op_src1_is_reg(use->op) && use->src1 == old)
                    use->src1 = reg;
                if (use->src0_hi == old)
                    use->src0_hi = reg;
                if (op_writes_dest(use->op) && use->dest == old)
                    break;
            }
            *link = ir->next;
            if (bb->mops.tail == ir)
                bb->mops.tail = previous;
        }
    }
    free(order);
    free(body);
    free(in_loop);
    for (int i = 0; i < count; i++) {
        a64_lanes |= 1u << constants[i]->dest;
        a64_lane_values[constants[i]->dest] = a64_constant_value(constants[i]);
    }
    if (count) {
        constants[count - 1]->next = f->bbs->mops.head;
        f->bbs->mops.head = constants[0];
        if (!f->bbs->mops.tail)
            f->bbs->mops.tail = constants[count - 1];
    }
}

/* Whether what @def wrote is dead once @use, the next operation, has run. */
static bool a64_value_dies(basic_block_t *bb, vir_mop_t *def, vir_mop_t *use)
{
    return (op_writes_dest(use->op) && use->dest == def->dest) ||
           !reg_read_after(bb, use, def->dest);
}

/* Turn an AND that only an equality test against zero reads into TST, the ANDS
 * that keeps no result, for a comparison that sets a register or selects one.
 * Before a branch, the test becomes CBZ or CBNZ, which is no longer.
 */
static bool a64_test_bits(basic_block_t *bb, vir_mop_t *and)
{
    vir_mop_t *compare = and->next;
    if (and->op != OP_bit_and || !compare ||
        (compare->op != OP_eq && compare->op != OP_neq) ||
        compare->src0 != and->dest || compare->src2 >> 1 != A64_CMP_BIAS ||
        (compare->next && compare->next->op == OP_branch) ||
        a64_cmp_wide(compare) != (and->size_bytes == 8) ||
        !a64_value_dies(bb, and, compare))
        return false;
    and->dest = A64_ZERO;
    compare->src2 |= A64_CMP_TESTED;
    return true;
}

/* Whether @extend widens exactly what the load @load read, so that the load can
 * extend it itself: LDRSB, LDRSH and LDRSW sign-extend into the X register, and
 * the plain narrow loads zero-extend.
 */
static bool a64_fold_load_extend(vir_mop_t *load, vir_mop_t *extend)
{
    int size = load->op == OP_read ? load->src1 : load->size_bytes;
    int dst_size = extend->src1 & 0xffff;

    return (load->op == OP_load || load->op == OP_global_load ||
            load->op == OP_read) &&
           extend->op == OP_sign_ext && extend->src0 == load->dest &&
           (extend->src1 >> 16) == size && size < 8 &&
           !(dst_size == PTR_SIZE && extend->is_pointer);
}

/* Rewrite @use so that it no longer reads the dead literal @value from @reg,
 * where AArch64 has a form for it: a comparison against an immediate, a store
 * of the zero register, or an extension evaluated now.
 */
static bool a64_fold_literal(int reg, vir_mop_t *use, long long value)
{
    if (op_is_comparison(use->op) && (use->src0 == reg) != (use->src1 == reg) &&
        value >= -4095 && value <= 4095) {
        /* With the literal on the left, swap the operands and mirror the
         * comparison; equality reads the same either way.
         */
        if (use->src0 == reg) {
            use->src0 = use->src1;
            if (use->op == OP_lt)
                use->op = OP_gt;
            else if (use->op == OP_gt)
                use->op = OP_lt;
            else if (use->op == OP_leq)
                use->op = OP_geq;
            else if (use->op == OP_geq)
                use->op = OP_leq;
        }
        use->src1 = -1;
        use->src2 = ((int) value + A64_CMP_BIAS) << 1;
        return true;
    }
    if (!value && (use->op == OP_store || use->op == OP_global_store) &&
        use->src0 == reg) {
        use->src0 = A64_ZERO;
        return true;
    }
    if (!value && use->op == OP_write && use->src1 == reg && use->src0 != reg &&
        use->src0_hi != reg) {
        use->src1 = A64_ZERO;
        return true;
    }

    /* x * (2^k + 1) is x + (x << k). Only the low word of an int product is its
     * value, so the X form serves either width.
     */
    if (use->op == OP_mul && (use->src0 == reg) != (use->src1 == reg) &&
        value > 2 && value <= (1LL << (use->size_bytes == 8 ? 32 : 31)) + 1 &&
        !((value - 1) & (value - 2))) {
        int shift = 0;
        while ((1LL << shift) + 1 != value)
            shift++;
        use->op = OP_add;
        if (use->src0 == reg)
            use->src0 = use->src1;
        use->src1 = use->src0;
        use->src2 = -shift - 1;
        return true;
    }
    if ((use->op == OP_trunc || use->op == OP_sign_ext) && use->src0 == reg) {
        /* Decode the width exactly as emit_mop() does for these two. */
        int size = use->src1;
        bool is_unsigned = use->is_unsigned;
        if (use->op == OP_sign_ext) {
            size = (use->src1 >> 16) & 0xffff;
            is_unsigned = use->src0_is_unsigned;
            if ((use->src1 & 0xffff) == PTR_SIZE && use->is_pointer)
                size = 8;
        }
        unsigned long long bits = value;
        if (size == 1 || size == 2 || size == 4) {
            unsigned long long sign = 1ULL << (size * 8 - 1);
            bits &= (sign << 1) - 1;
            if (!is_unsigned && (bits & sign))
                bits -= sign << 1;
        }
        use->op = OP_load_constant;
        use->size_bytes = 8;
        use->src0 = (int) bits;
        use->src1 = (int) (bits >> 32);
        return true;
    }
    return false;
}

/* Whether @x keeps @def from moving below it: it reads or writes what @def
 * writes, overwrites what @def reads, or ends the block or calls out.
 */
static bool a64_blocks_sink(vir_mop_t *def, vir_mop_t *x)
{
    if (x->op == OP_branch || x->op == OP_jump || x->op == OP_return ||
        x->op == OP_call || x->op == OP_indirect ||
        ir_reads_reg(x, def->dest) || (mop_defs(x) & (1u << def->dest)))
        return true;
    unsigned int clobbers = mop_defs(x);
    for (int reg = 0; reg < REG_CNT; reg++)
        if ((clobbers & (1u << reg)) && ir_reads_reg(def, reg))
            return true;
    return false;
}

/* What a64_sink_defs() moves: literals, then the address arithmetic of a memory
 * access, then the extension of its index, and last a product to its addition.
 */
enum { A64_SINK_LITERAL, A64_SINK_ADDRESS, A64_SINK_INDEX, A64_SINK_PRODUCT };

static bool a64_sinks(int kind, vir_mop_t *def, vir_mop_t *reader)
{
    bool access = reader->op == OP_read || reader->op == OP_write;
    if (kind == A64_SINK_LITERAL)
        return def->op == OP_load_constant;
    if (kind == A64_SINK_ADDRESS)
        return (((def->op == OP_add || def->op == OP_sub) &&
                 def->size_bytes == 8) ||
                def->op == OP_address_of || def->op == OP_global_address_of) &&
               access && reader->src0 == def->dest;
    if (kind == A64_SINK_PRODUCT)
        return def->op == OP_mul && reader->op == OP_add;
    return def->op == OP_sign_ext && access && reader->src0_hi == def->dest;
}

/* Move each operation of @kind down to the first one that reads its result, so
 * that the folds, which pair an operation with the one after it, find it next
 * to its use. Later ones move first, so that several of them end up in their
 * order in front of a reader.
 */
static void a64_sink_defs(func_t *f, int kind)
{
    int capacity = 0;
    vir_mop_t **order = NULL;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        int count = 0;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            count++;
        if (count < 3)
            continue;
        if (count > capacity) {
            free(order);
            capacity = count * 2;
            order = malloc(capacity * sizeof(*order));
            if (!order)
                fatal("Out of memory");
        }
        count = 0;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            order[count++] = ir;
        for (int i = count - 2; i >= 0; i--) {
            vir_mop_t *def = order[i];
            if (!op_writes_dest(def->op) || def->dest_hi >= 0 ||
                def->dest < 0 || def->dest >= REG_CNT)
                continue;
            int at = i + 1;
            while (at < count && !a64_blocks_sink(def, order[at]))
                at++;
            if (at == i + 1 || at == count ||
                !ir_reads_reg(order[at], def->dest) ||
                !a64_sinks(kind, def, order[at]))
                continue;
            for (int k = i; k < at - 1; k++)
                order[k] = order[k + 1];
            order[at - 1] = def;
        }
        bb->mops.head = order[0];
        for (int i = 0; i < count - 1; i++)
            order[i]->next = order[i + 1];
        order[count - 1]->next = NULL;
        bb->mops.tail = order[count - 1];
    }
    free(order);
}

/* Feed the shift by an immediate @shift, whose result only @use reads, to @use
 * as its shifted second operand: ADD and SUB on X registers, and AND, ORR and
 * EOR on registers as wide as the shift's. src2 of @use then encodes the shift
 * as -1 - (amount + 64 * kind), kind being 0 for LSL, 1 for LSR and 2 for ASR.
 */
static bool a64_fold_shift(basic_block_t *bb, vir_mop_t *shift, vir_mop_t *use)
{
    int r = shift->dest;
    bool logic =
        use->op == OP_bit_and || use->op == OP_bit_or || use->op == OP_bit_xor;
    if ((shift->op != OP_lshift && shift->op != OP_rshift) ||
        shift->src2 <= 0 || use->src2 || (use->src0 == r) == (use->src1 == r) ||
        (use->src0 == r && use->op == OP_sub) ||
        !(logic ? shift->size_bytes == use->size_bytes
                : (use->op == OP_add || use->op == OP_sub) &&
                      shift->size_bytes == 8 && use->size_bytes == 8) ||
        !a64_value_dies(bb, shift, use))
        return false;
    int kind = shift->op == OP_lshift ? 0 : shift->src0_is_unsigned ? 1 : 2;
    if (use->src0 == r)
        use->src0 = use->src1;
    use->src1 = shift->src0;
    use->src2 = -1 - (shift->src2 - 1 + 64 * kind);
    return true;
}

/* Fold a dead adjacent literal into the arithmetic instruction. src2 is an
 * opcode-local immediate plus one; zero retains the register form.
 */
static void arm64_fold_immediates(func_t *func)
{
    a64_sink_defs(func, A64_SINK_LITERAL);
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t **link = &bb->mops.head;
        while (*link && (*link)->next) {
            vir_mop_t *literal = *link, *use = literal->next;
            long long value = (long long) a64_constant_value(literal);
            if (literal->op == OP_load_constant &&
                a64_value_dies(bb, literal, use) &&
                a64_fold_literal(literal->dest, use, value)) {
                *link = use;
                continue;
            }
            if (a64_fold_load_extend(literal, use) &&
                a64_value_dies(bb, literal, use)) {
                literal->is_unsigned = use->src0_is_unsigned;
                literal->dest = use->dest;
                literal->next = use->next;
                if (bb->mops.tail == use)
                    bb->mops.tail = literal;
                continue;
            }
            if (literal->op == OP_load_constant && !use->src2 &&
                (use->src0 == literal->dest) != (use->src1 == literal->dest) &&
                (use->op == OP_bit_and || use->op == OP_bit_or ||
                 use->op == OP_bit_xor) &&
                a64_value_dies(bb, literal, use)) {
                int field = a64_bitmask_field(a64_constant_value(literal),
                                              use->size_bytes == 8);
                if (field >= 0) {
                    if (use->src0 == literal->dest)
                        use->src0 = use->src1;
                    use->src2 = field + 1;
                    use->src1 = -1;
                    *link = use;
                    continue;
                }
            }
            if (a64_fold_shift(bb, literal, use)) {
                *link = use;
                continue;
            }
            /* An addition takes the literal from either side. */
            if (literal->op == OP_load_constant && use->op == OP_add &&
                !use->src2 && use->src0 == literal->dest &&
                use->src1 != literal->dest) {
                use->src0 = use->src1;
                use->src1 = literal->dest;
            }
            /* Unsigned, so that LLONG_MIN stays out of range too. */
            unsigned long long magnitude =
                value < 0 ? 0ULL - (unsigned long long) value
                          : (unsigned long long) value;
            if (literal->op != OP_load_constant || use->src1 != literal->dest ||
                use->src0 == literal->dest || use->src2 ||
                (use->op != OP_add && use->op != OP_sub &&
                 use->op != OP_lshift && use->op != OP_rshift) ||
                !a64_value_dies(bb, literal, use) ||
                (use->op == OP_lshift || use->op == OP_rshift
                     ? value < 0 || value >= (use->size_bytes == 8 ? 64 : 32)
                     : magnitude > 4095 &&
                           (magnitude & 0xfff || magnitude >= 1 << 24))) {
                link = &literal->next;
                continue;
            }
            if (value < 0) {
                use->op = use->op == OP_add ? OP_sub : OP_add;
                value = -value;
            }

            /* Past twelve bits, the immediate is a multiple of 4096 that the
             * instruction shifts: src2 then exceeds 4096 by that multiple.
             */
            use->src2 = (value > 4095 ? A64_IMM_SHIFTED + (int) (value >> 12)
                                      : (int) value) +
                        1;
            use->src1 = -1;
            *link = use;
        }
    }
    a64_sink_defs(func, A64_SINK_ADDRESS);
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t **link = &bb->mops.head;
        while (*link && (*link)->next) {
            vir_mop_t *address = *link, *memory = address->next;
            int size = memory->op == OP_read ? memory->src1 : memory->dest;
            if (address->size_bytes != 8 || address->src2 > A64_IMM_SHIFTED ||
                (address->op != OP_add && address->op != OP_sub) ||
                (memory->op != OP_read && memory->op != OP_write) ||
                (address->src2 <= 0 &&
                 (address->op != OP_add || address->src2 < -4 ||
                  (address->src2 < -1 &&
                   (1 << (-address->src2 - 1)) != size))) ||
                memory->src0_hi >= 0 || memory->src2 != 0 ||
                memory->src0 != address->dest ||
                (memory->op == OP_write && memory->src1 == address->dest) ||
                !a64_value_dies(bb, address, memory)) {
                link = &address->next;
                continue;
            }
            memory->src0 = address->src0;
            memory->src0_hi = address->src2 <= 0 ? address->src1 : -1;
            memory->src2 =
                address->src2 <= 0
                    ? (address->src2 ? address->src2 : -1)
                    : (address->src2 - 1) * (address->op == OP_sub ? -1 : 1);
            *link = memory;
        }
    }

    /* An access through the address of a frame slot or a global becomes an
     * access to that slot or global, which takes its offset as an immediate.
     * The address may have reached the access only through the fold above.
     */
    a64_sink_defs(func, A64_SINK_ADDRESS);
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t **link = &bb->mops.head;
        while (*link && (*link)->next) {
            vir_mop_t *address = *link, *memory = address->next;
            bool read = memory->op == OP_read;
            int offset = address->src0 + memory->src2;
            if ((address->op != OP_address_of &&
                 address->op != OP_global_address_of) ||
                (!read && memory->op != OP_write) || memory->src0_hi >= 0 ||
                memory->src0 != address->dest || offset < 0 ||
                (!read && memory->src1 == address->dest) ||
                !a64_value_dies(bb, address, memory)) {
                link = &address->next;
                continue;
            }
            bool global = address->op == OP_global_address_of;
            if (read) {
                memory->op = global ? OP_global_load : OP_load;
                memory->size_bytes = memory->src1;
                memory->src0 = offset;
                memory->src1 = -1;
            } else {
                memory->op = global ? OP_global_store : OP_store;
                memory->size_bytes = memory->dest;
                memory->src0 = memory->src1;
                memory->src1 = offset;
                memory->dest = -1;
            }
            memory->src2 = 0;
            memory->ofs_based_on_stack_top = address->ofs_based_on_stack_top;
            *link = memory;
        }
    }

    /* An int index extends in the access itself: src2 from -5 to -8 for UXTW
     * and from -9 to -12 for SXTW, each shifting by the access width or not.
     */
    a64_sink_defs(func, A64_SINK_INDEX);
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t **link = &bb->mops.head;
        while (*link && (*link)->next) {
            vir_mop_t *extend = *link, *memory = extend->next;
            if (extend->op != OP_sign_ext || extend->src1 != ((4 << 16) | 8) ||
                extend->is_pointer ||
                (memory->op != OP_read && memory->op != OP_write) ||
                memory->src0_hi != extend->dest ||
                memory->src0 == extend->dest || memory->src2 > -1 ||
                memory->src2 < -4 ||
                (memory->op == OP_write && memory->src1 == extend->dest) ||
                !a64_value_dies(bb, extend, memory)) {
                link = &extend->next;
                continue;
            }
            memory->src0_hi = extend->src0;
            memory->src2 -= extend->src0_is_unsigned ? 4 : 8;
            *link = memory;
        }
    }
}

/* A multiplication whose product only an addition of the same width reads
 * becomes MADD, which takes the other addend in src1_hi. A W-form addition
 * pairs with a 32-bit product; an int one in X form keeps upper bits that a W
 * MADD would clear, so it stays apart.
 */
static void a64_fuse_multiply_add(func_t *f)
{
    a64_sink_defs(f, A64_SINK_PRODUCT);
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        for (vir_mop_t **link = &bb->mops.head; *link && (*link)->next;) {
            vir_mop_t *mul = *link, *add = mul->next;
            int r = mul->dest;
            if (mul->op != OP_mul || mul->src2 || mul->dest_hi >= 0 ||
                mul->src1_hi >= 0 || add->op != OP_add || add->src2 ||
                add->dest_hi >= 0 || add->size_bytes != mul->size_bytes ||
                (mul->size_bytes != 8 && a64_arith_wide(add)) ||
                (add->src0 == r) == (add->src1 == r) ||
                !a64_value_dies(bb, mul, add)) {
                link = &mul->next;
                continue;
            }
            add->src1_hi = add->src0 == r ? add->src1 : add->src0;
            add->op = OP_mul;
            add->src0 = mul->src0;
            add->src1 = mul->src1;
            *link = add;
        }
}

/* Whether @ir only computes its destination register, with no other effect. */
static bool a64_pure(vir_mop_t *ir)
{
    switch (ir->op) {
    case OP_load_constant:
    case OP_address_of:
    case OP_global_address_of:
    case OP_load_rodata_address:
    case OP_assign:
    case OP_cast:
    case OP_trunc:
    case OP_sign_ext:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
        return ir->dest >= 0 && ir->dest < REG_CNT && ir->dest_hi < 0;
    default:
        return false;
    }
}

/* Drop pure operations whose result nothing reads. Folding an address or a
 * literal into its use leaves the definition it came from in another block, and
 * live sets recomputed from what the blocks now read show it unused.
 */
static void a64_drop_dead(func_t *f)
{
    int capacity = 0;
    vir_mop_t **order = NULL;
    mop_function_liveness(f);
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        int count = 0;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            count++;
        if (count > capacity) {
            free(order);
            capacity = count * 2;
            order = malloc(capacity * sizeof(*order));
            if (!order)
                fatal("Out of memory");
        }
        count = 0;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            order[count++] = ir;
        unsigned int live = bb->machine_liveout;
        vir_mop_t *next = NULL;
        for (int i = count - 1; i >= 0; i--) {
            vir_mop_t *ir = order[i];
            if (a64_pure(ir) && !(live & (1u << ir->dest)))
                continue;
            live = (live & ~mop_defs(ir)) | mop_reads(ir);
            if (!next)
                bb->mops.tail = ir;
            ir->next = next;
            next = ir;
        }
        bb->mops.head = next;
        if (!next)
            bb->mops.tail = NULL;
    }
    free(order);
}

/* How many branches and jumps in @f name @target. */
static int a64_branches_naming(func_t *f, basic_block_t *target)
{
    int count = 0;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t *tail = bb->mops.tail;
        if (tail && tail->op == OP_jump)
            count += tail->next_bb == target;
        else if (tail && tail->op == OP_branch)
            count += (tail->then_bb == target) + (tail->else_bb == target);
    }
    return count;
}

/* Join a block that only the jump ending the block before it reaches, an edge
 * copy block most often, to that block. The peephole then sees a definition and
 * the edge's move of it together, where it can write the result in place.
 */
static void a64_merge_blocks(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb;) {
        vir_mop_t *jump = bb->mops.tail;
        basic_block_t *next =
            jump && jump->op == OP_jump ? jump->next_bb : NULL;
        if (!next || !next->mops.head || !a64_falls_into(bb, next) ||
            a64_branches_naming(f, next) != 1) {
            bb = bb->rpo_next;
            continue;
        }
        vir_mop_t **link = &bb->mops.head;
        while (*link != jump)
            link = &(*link)->next;
        *link = next->mops.head;
        bb->mops.tail = next->mops.tail;
        next->mops.head = next->mops.tail = NULL;
        bb->next = next->next;
        bb->then_ = next->then_;
        bb->else_ = next->else_;
        bb->machine_liveout = next->machine_liveout;
        next->next = next->then_ = next->else_ = NULL;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            for (int tries = 0; tries < 4 && peephole_at(bb, ir); tries++)
                ;
    }
}

/* Whether @use only copies a register, emitted as a plain move. A comparison
 * leaves 0 or 1, which every extension of it keeps as it is.
 */
static bool a64_is_copy(vir_mop_t *def, vir_mop_t *use)
{
    if (use->dest_hi >= 0 || use->src0_hi >= 0 || use->src0 != def->dest)
        return false;
    if (use->op == OP_assign || use->op == OP_cast)
        return true;
    if (use->op != OP_trunc && use->op != OP_sign_ext)
        return false;
    if (op_is_comparison(def->op) || def->op == OP_log_not)
        return true;

    /* A W-form result is zero-extended already, and an int addition or
     * subtraction can take that form.
     */
    bool zero_extends = use->op == OP_trunc
                            ? use->src1 == 4 && use->is_unsigned
                            : use->src1 == ((4 << 16) | 8) &&
                                  use->src0_is_unsigned && !use->is_pointer;
    if (!zero_extends || def->size_bytes != 4)
        return false;
    switch (def->op) {
    case OP_add:
    case OP_sub:
        return def->src2 >= -32;
    case OP_mul:
    case OP_div:
    case OP_mod:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_bit_not:
        return true;
    default:
        return false;
    }
}

/* Write a result where its only reader wants it: the register of a move that
 * copies it, or x0 for the return of it. The definition moves down to that
 * reader past operations that leave its register and its operands alone, as an
 * edge copy is often apart from the constant it copies. The shared peephole
 * left these, as it only retargets adjacent instructions fusible everywhere.
 */
static void a64_coalesce_results(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        for (vir_mop_t **link = &bb->mops.head; *link;) {
            vir_mop_t *def = *link, *use = def->next;
            int r = def->dest;
            if (!op_writes_dest(def->op) || def->dest_hi >= 0 || r < 0 ||
                r >= REG_CNT) {
                link = &def->next;
                continue;
            }

            /* A load moves only into a copy right after it, so that it never
             * passes a store or a call.
             */
            bool loads = def->op == OP_load || def->op == OP_global_load ||
                         def->op == OP_read;
            for (; use; use = use->next) {
                if (loads && !a64_is_copy(def, use)) {
                    use = NULL;
                    break;
                }
                if (a64_is_copy(def, use) || ir_reads_reg(use, r) ||
                    (mop_defs(use) & (1u << r)) || use->op == OP_branch ||
                    use->op == OP_jump || use->op == OP_return)
                    break;
                unsigned int clobbers = mop_defs(use);
                bool sources = false;
                for (int reg = 0; reg < REG_CNT; reg++)
                    if ((clobbers & (1u << reg)) && ir_reads_reg(def, reg))
                        sources = true;
                if (sources)
                    break;
            }
            int to = -1;
            if (use && use->op == OP_return && use->src0 == r &&
                use->src0_hi < 0 && r)
                to = 0;
            else if (use && a64_is_copy(def, use) &&
                     a64_value_dies(bb, def, use))
                to = use->dest;
            if (to < 0) {
                link = &def->next;
                continue;
            }

            /* The definition takes the place of the copy, or goes ahead of the
             * return, which then finds its value in x0.
             */
            if (use->op == OP_return) {
                vir_mop_t *ret = mop_create(OP_return);
                *ret = *use;
                ret->src0 = 0;
                if (bb->mops.tail == use)
                    bb->mops.tail = ret;
                use->next = ret;
            }
            vir_mop_t *next = use->next;
            bool extends = use->op == OP_trunc || use->op == OP_sign_ext;
            *use = *def;
            use->dest = to;
            use->next = next;
            if (extends && (use->op == OP_add || use->op == OP_sub))
                use->is_unsigned = true;
            *link = def->next;
        }
}

/* Select between short, effect-free expression arms without a data-dependent
 * branch. Only a single live result may change; both scratch evaluations read
 * the original inputs and retain the comparison flags.
 */
static void a64_select_diamonds(func_t *func)
{
    if (func_has_op(func, OP_load_func, OP_indirect))
        return;
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t *branch = bb->mops.tail, *compare = NULL;
        if (!branch || branch->op != OP_branch)
            continue;
        for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next)
            if (ir->next == branch)
                compare = ir;
        if (!compare || !a64_branch_only_compare(bb, compare))
            continue;
        basic_block_t *arms[2][4];
        vir_mop_t *ops[2][4];
        int lengths[2] = {0}, nops[2] = {0}, join_at[2] = {-1, -1};
        bool safe = true;
        for (int side = 0; side < 2; side++) {
            basic_block_t *arm = side ? branch->else_bb : branch->then_bb;
            while (arm && lengths[side] < 4) {
                arms[side][lengths[side]++] = arm;
                vir_mop_t *tail = arm->mops.tail;
                arm = tail && tail->op == OP_jump ? tail->next_bb : NULL;
            }
        }
        for (int left = 0; left < lengths[0] && join_at[0] < 0; left++)
            for (int right = 0; right < lengths[1]; right++)
                if (arms[0][left] == arms[1][right]) {
                    join_at[0] = left;
                    join_at[1] = right;
                    break;
                }
        if (join_at[0] <= 0 || join_at[1] <= 0)
            continue;
        int output = -1;
        for (int side = 0; side < 2 && safe; side++) {
            unsigned int written = 0;
            int last = -1;
            for (int index = 0; index < join_at[side] && safe; index++) {
                basic_block_t *arm = arms[side][index];
                if (!bb_sole_pred(arm)) {
                    safe = false;
                    break;
                }
                for (vir_mop_t *ir = arm->mops.head; ir; ir = ir->next) {
                    if (ir->op == OP_jump)
                        continue;
                    if (nops[side] == 4 ||
                        (ir->op != OP_add && ir->op != OP_sub &&
                         ir->op != OP_bit_xor && ir->op != OP_bit_and &&
                         ir->op != OP_bit_or && ir->op != OP_assign) ||
                        ((written & (1u << ir->src0)) && ir->src0 != last) ||
                        (op_src1_is_reg(ir->op) && ir->src1 >= 0 &&
                         (written & (1u << ir->src1)) && ir->src1 != last)) {
                        safe = false;
                        break;
                    }
                    ops[side][nops[side]++] = ir;
                    written |= 1u << ir->dest;
                    last = ir->dest;
                }
            }
            if (!nops[side])
                safe = false;
            else {
                unsigned int live =
                    written & arms[side][join_at[side] - 1]->machine_liveout;
                if (!live || (live & (live - 1)) || !(live & (1u << last)) ||
                    (output >= 0 && output != last))
                    safe = false;
                output = last;
            }
        }
        if (!safe)
            continue;
        vir_mop_t *tail = compare;
        compare->src2 |= A64_CMP_FUSED;
        for (int side = 0; side < 2; side++) {
            int last = -1, scratch = REG_CNT + side;
            for (int index = 0; index < nops[side]; index++) {
                vir_mop_t *old = ops[side][index];
                vir_mop_t *ir = mop_create(old->op);
                *ir = *old;
                ir->next = NULL;
                if (ir->src0 == last)
                    ir->src0 = scratch;
                if (op_src1_is_reg(ir->op) && ir->src1 == last)
                    ir->src1 = scratch;
                ir->dest = scratch;
                last = old->dest;
                tail->next = ir;
                tail = ir;
            }
            for (int index = 0; index < join_at[side]; index++) {
                arms[side][index]->mops.head = NULL;
                arms[side][index]->mops.tail = NULL;
            }
        }
        vir_mop_t *select = mop_create(OP_ternary);
        select->dest = output;
        select->src0 = REG_CNT;
        select->src1 = REG_CNT + 1;
        select->src2 = a64_cond(compare->op, compare->src0_is_unsigned ||
                                                 compare->src1_is_unsigned);
        tail->next = select;
        select->next = branch;
        branch->op = OP_jump;
        branch->next_bb = arms[0][join_at[0]];
    }
}

/* Whether rodata, data and function addresses are ADR's pc-relative offsets,
 * settled before offsets are planned since it changes their length.
 */
static bool a64_pc_relative;

/* Load the absolute address @target into @d. */
static void a64_address(int d, int target)
{
    if (!a64_pc_relative) {
        a64_mov_imm(d, target);
        return;
    }
    int disp = target - (elf_code_start + elf_code->size);
    a64_check_disp(disp, 21, "arm64 ADR target out of range");
    emit(a64_adr_insn(d, disp));
}

/* Whether the block @bb ends with a branch to @target. */
static bool a64_branches_to(basic_block_t *bb, basic_block_t *target)
{
    vir_mop_t *tail = bb->mops.tail;
    if (!tail)
        return false;
    if (tail->op == OP_jump)
        return !tail->src2 && tail->next_bb == target;
    return tail->op == OP_branch &&
           ((tail->src2 != 2 && tail->then_bb == target) ||
            (tail->src2 != 1 && tail->else_bb == target));
}

/* Where the loop body starting at code offset @offset in block @start goes: on
 * the next boundary of 32 or 64 bytes if the body, from @start through the last
 * branch back to it, fits one fetch block but would cross one where it is. Code
 * starts after the headers, so addresses differ from offsets. Only an innermost
 * loop without calls is placed: a call or an inner loop dominates the time of
 * the body around it, and the padding would buy nothing.
 */
static int a64_place_loop(basic_block_t *start, int offset)
{
    int bytes = 0, length = 0;
    bool inner = false;
    for (basic_block_t *bb = start; bb && bytes <= A64_FETCH_BLOCK;
         bb = bb->rpo_next) {
        if (bb != start && bb->is_loop_start)
            inner = true;
        for (vir_mop_t *mop = bb->mops.head; mop; mop = mop->next) {
            if (mop->op == OP_call || mop->op == OP_indirect)
                inner = true;
            if (mop->op != OP_allocat)
                bytes += mop_count_words(mop) * 4;
        }
        if (a64_branches_to(bb, start) && bytes <= A64_FETCH_BLOCK && !inner)
            length = bytes;
    }
    int address = elf_header_size() + offset;
    if (!length || address % A64_FETCH_BLOCK + length <= A64_FETCH_BLOCK)
        return offset;
    int boundary =
        length <= A64_FETCH_BLOCK / 2 ? A64_FETCH_BLOCK / 2 : A64_FETCH_BLOCK;
    int padding = ALIGN_UP(address, boundary) - address;
    return padding <= A64_LOOP_PADDING ? offset + padding : offset;
}

static void a64_emit_entry(void);
static void a64_emit_main_call(void);
static void a64_emit_syscall(void);

/* The instruction words @emitter produces, measured without writing them. */
static int a64_count_words(void (*emitter)(void))
{
    bool saved_count_only = mop_count_only;
    int saved_counted_words = mop_counted_words;

    mop_count_only = true;
    mop_counted_words = 0;
    emitter();
    int words = mop_counted_words;
    mop_count_only = saved_count_only;
    mop_counted_words = saved_counted_words;
    return words;
}

/* Only a static program that calls __syscall needs the helper behind it; one
 * built with --no-libc usually never does.
 */
static bool a64_needs_syscall(void)
{
    func_t *f = find_func("__syscall");
    return !dynlink && f && f->bbs && f->is_used;
}

/* Give the entry sequence, the global initializers and every block their code
 * offsets, from the length of what emit_mop() produces for each operation.
 * cfg_flatten() has already put each function's definition in MOP_FLAT ahead of
 * its operations.
 */
static void a64_plan_offsets(void)
{
    func_t *f;
    int index = 0;

    /* The entry sequence, the global initializers, the call to main and the
     * __syscall helper, in code_generate()'s order.
     */
    elf_offset = a64_count_words(a64_emit_entry) * 4;
    GLOBAL_FUNC->bbs->elf_offset = elf_offset;
    mop_visit_globals(update_elf_offset);
    if (MAIN_BB)
        elf_offset += a64_count_words(a64_emit_main_call) * 4;
    if (a64_needs_syscall()) {
        find_func("__syscall")->bbs->elf_offset = elf_offset;
        elf_offset += a64_count_words(a64_emit_syscall) * 4;
    }
    for (f = FUNC_LIST.head; f; f = f->next) {
        if (!f->bbs)
            continue;
        vir_mop_t *define = MOP_FLAT[index++];
        if (define->op != OP_define)
            fatal("arm64 offset planning lost a function definition");

        /* Every return after the first branches to its epilogue, which starts
         * once the first has moved its own value into x0. Only a long epilogue
         * is shared: in place of two restores and a RET, a taken branch on
         * every early exit, the base case of a recursion among them, costs more
         * than it saves. All returns of a function restore the same frame.
         */
        vir_mop_t *first_return = NULL;
        int exit_offset = 0;
        for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
            if (bb->is_loop_start)
                elf_offset = a64_place_loop(bb, elf_offset);
            bb->elf_offset = elf_offset;
            if (bb == f->bbs)
                elf_offset += mop_count_words(define) * 4;
            for (vir_mop_t *mop = bb->mops.head; mop; mop = mop->next) {
                index++;
                if (mop->op == OP_return) {
                    int moves = mop->src0 >= 0 && a64_reg(mop->src0) != 0;
                    mop->src2 &= ~A64_SHARED_EXIT;
                    bool long_exit =
                        mop_count_words(mop) - moves >= A64_SHARED_EXIT_WORDS;
                    if (long_exit && !first_return) {
                        first_return = mop;
                        exit_offset = elf_offset + moves * 4;
                    } else if (long_exit) {
                        mop->src2 |= A64_SHARED_EXIT;
                        mop->dest = exit_offset;
                    }
                }
                update_elf_offset(mop);
            }
        }
    }
}

/* Mark the blocks a branch from later in the layout reaches: with the loop
 * tests rotated, these are where loop bodies start. Only an emitted branch
 * counts, not a successor that the code falls into.
 */
static void a64_mark_loop_starts(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        bb->is_loop_start = false;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t *tail = bb->mops.tail;
        basic_block_t *targets[2] = {NULL, NULL};
        if (!tail)
            continue;
        if (tail->op == OP_jump && !tail->src2)
            targets[0] = tail->next_bb;
        else if (tail->op == OP_branch) {
            if (tail->src2 != 2)
                targets[0] = tail->then_bb;
            if (tail->src2 != 1)
                targets[1] = tail->else_bb;
        }
        for (int i = 0; i < 2; i++)
            if (targets[i] && targets[i] != f->bbs &&
                targets[i]->rpo <= bb->rpo)
                targets[i]->is_loop_start = true;
    }
}

/* Whether @bb ends with a branch, jump or return, which emit_mop() points at
 * whatever block then follows. Any other block falls into the next one.
 */
static bool a64_ends_closed(basic_block_t *bb)
{
    vir_mop_t *tail = bb->mops.tail;
    return tail && (tail->op == OP_return || tail->op == OP_jump ||
                    tail->op == OP_branch);
}

/* Whether @bb ends with a branch or jump to @target. */
static bool a64_targets(basic_block_t *bb, basic_block_t *target)
{
    vir_mop_t *tail = bb->mops.tail;
    if (tail->op == OP_jump)
        return tail->next_bb == target;
    return tail->op == OP_branch &&
           (tail->then_bb == target || tail->else_bb == target);
}

/* The last block from @first up to @end that emits anything, or NULL. */
static basic_block_t *a64_last_emitting(basic_block_t *first,
                                        basic_block_t *end)
{
    basic_block_t *last = NULL;
    for (basic_block_t *bb = first; bb != end; bb = bb->rpo_next)
        if (bb->mops.head)
            last = bb;
    return last;
}

/* What each register holds where @bb ends, as a symbol plus an offset. Symbol 0
 * is a literal, set in @bb or kept in a lane. Any other symbol stands for a
 * value that is unknown but the same wherever it appears, such as what a
 * register held on entry to @bb or the base of the globals, so that two
 * registers with one symbol differ by the difference of their offsets: a
 * pointer and the end of the array it walks, say.
 */
static void a64_values_at_end(basic_block_t *bb,
                              int *symbol,
                              unsigned long long *offset)
{
    int fresh = REG_CNT + 1;
    for (int r = 0; r < REG_CNT; r++) {
        bool lane = (a64_lanes & (1u << r)) != 0;
        symbol[r] = lane ? 0 : r + 1;
        offset[r] = lane ? a64_lane_values[r] : 0;
    }
    for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next) {
        int d = ir->dest, n = ir->src0;
        unsigned int defs = mop_defs(ir);
        if (d < 0 || d >= REG_CNT || defs != 1u << d) {
            for (int r = 0; r < REG_CNT; r++)
                if (defs & (1u << r)) {
                    symbol[r] = ++fresh;
                    offset[r] = 0;
                }
            continue;
        }
        bool copies = n >= 0 && n < REG_CNT;
        if (ir->op == OP_load_constant) {
            symbol[d] = 0;
            offset[d] = a64_constant_value(ir);
        } else if (ir->op == OP_assign && copies) {
            symbol[d] = symbol[n];
            offset[d] = offset[n];
        } else if ((ir->op == OP_add || ir->op == OP_sub) && copies &&
                   ir->size_bytes == 8 && ir->src2 > 0) {
            unsigned long long imm = (unsigned long long) a64_imm12(ir)
                                     << (ir->src2 > A64_IMM_SHIFTED ? 12 : 0);
            symbol[d] = symbol[n];
            offset[d] = ir->op == OP_add ? offset[n] + imm : offset[n] - imm;
        } else if (ir->op == OP_global_address_of) {
            symbol[d] = REG_CNT + 1;
            offset[d] = ir->src0;
        } else {
            symbol[d] = ++fresh;
            offset[d] = 0;
        }
    }
}

/* Which way the test that is all of @bb goes when entered from @from: 1 to its
 * then block, 0 to its else block, -1 when that is not known there. Literals
 * settle any test; two values of one symbol settle only an equality test, as
 * the order of unknown values is not known even when their distance is.
 */
static int a64_known_branch(basic_block_t *from, basic_block_t *bb)
{
    vir_mop_t *branch = bb->mops.tail, *compare = bb->mops.head;
    int symbol[REG_CNT];
    unsigned long long offset[REG_CNT], a, b;
    if (!branch || branch->op != OP_branch)
        return -1;
    a64_values_at_end(from, symbol, offset);
    if (compare == branch) {
        int r = branch->src0;
        if (branch->src1 || r < 0 || r >= REG_CNT || symbol[r])
            return -1;
        a = offset[r];
        if (!branch->src0_is_pointer && branch->size_bytes != 8)
            a = (unsigned int) a;
        return a != 0;
    }
    int ra = compare->src0, rb = compare->src1;
    if (compare->next != branch || !op_is_comparison(compare->op) ||
        !(compare->src2 & A64_CMP_FUSED) || (compare->src2 & A64_CMP_TESTED) ||
        ra < 0 || ra >= REG_CNT)
        return -1;
    int sa = symbol[ra], sb = 0;
    a = offset[ra];
    if (compare->src2 >> 1)
        b = (long long) ((compare->src2 >> 1) - A64_CMP_BIAS);
    else if (rb >= 0 && rb < REG_CNT) {
        sb = symbol[rb];
        b = offset[rb];
    } else
        return -1;
    if (sa != sb)
        return -1;
    bool is_unsigned = compare->src0_is_unsigned || compare->src1_is_unsigned;
    if (!a64_cmp_wide(compare)) {
        a = is_unsigned ? (unsigned int) a : (long long) (int) a;
        b = is_unsigned ? (unsigned int) b : (long long) (int) b;
    }
    if (sa)
        return compare->op == OP_eq    ? a == b
               : compare->op == OP_neq ? a != b
                                       : -1;
    bool less = is_unsigned ? a < b : (long long) a < (long long) b;
    switch (compare->op) {
    case OP_eq:
        return a == b;
    case OP_neq:
        return a != b;
    case OP_lt:
        return less;
    case OP_geq:
        return !less;
    case OP_gt:
        return !less && a != b;
    case OP_leq:
        return less || a == b;
    default:
        return -1;
    }
}

/* A rotated loop keeps its first test at the entry, where the counter and the
 * bound are often both literals, as in a for loop over a constant range. That
 * test is settled now: it becomes a jump to the body.
 */
static void a64_settle_entry_tests(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        if (bb == f->bbs || a64_branches_naming(f, bb) != 1)
            continue;
        basic_block_t *from = NULL, *layout = a64_last_emitting(f->bbs, bb);
        for (basic_block_t *p = f->bbs; p; p = p->rpo_next)
            if (p->mops.tail && p->mops.tail->op == OP_jump &&
                p->mops.tail->next_bb == bb)
                from = p;
        if (!from || (layout != from && !a64_ends_closed(layout)))
            continue;
        int taken = a64_known_branch(from, bb);
        if (taken < 0)
            continue;
        vir_mop_t *jump = bb->mops.tail;
        jump->op = OP_jump;
        jump->next_bb = taken ? jump->then_bb : jump->else_bb;
        bb->mops.head = jump;
    }
}

/* A rotated loop tests again at its end and leaves for the code after it, which
 * the layout put between the entry test and the body: the body then ends with a
 * jump there. Moving that code behind the body lets the last test fall into it
 * and the entry test into the body, which saves the jump. It is not moved when
 * its own end falls into the body, which would cost a jump instead.
 */
static void a64_place_exits(func_t *f)
{
    for (basic_block_t *latch = f->bbs; latch; latch = latch->rpo_next) {
        vir_mop_t *tail = latch->mops.tail;
        if (!tail || tail->op != OP_branch)
            continue;
        basic_block_t *body = tail->then_bb, *exit = tail->else_bb;
        if (body->rpo > latch->rpo) {
            body = tail->else_bb;
            exit = tail->then_bb;
        }
        if (body->rpo > latch->rpo || exit->rpo >= body->rpo || exit == f->bbs)
            continue;
        basic_block_t *before = f->bbs;
        while (before->rpo_next != exit)
            before = before->rpo_next;
        basic_block_t *last = exit;
        while (last->rpo_next != body)
            last = last->rpo_next;
        basic_block_t *open = a64_last_emitting(exit, body);
        if (!a64_ends_closed(before) || !open || !a64_ends_closed(open) ||
            a64_targets(open, body))
            continue;
        before->rpo_next = body;
        last->rpo_next = latch->rpo_next;
        latch->rpo_next = exit;
        int rpo = 0;
        for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
            bb->rpo = rpo++;
    }
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        vir_mop_t *ir = bb->mops.tail;
        if (ir && ir->op == OP_jump)
            ir->src2 = a64_falls_into(bb, ir->next_bb);
        else if (ir && ir->op == OP_branch)
            ir->src2 = a64_branch_fallthrough(bb, ir);
    }
}

/* A load from a base register that an addition of a small literal to that
 * register follows, with nothing between that reads or writes it, becomes one
 * load that writes the sum back: LDR Wt, [Xn], #imm, as a pointer walking an
 * array does. The written-back base goes in the load's dest_hi, so that its
 * register counts as defined, and the amount in src2. This runs after every
 * other pass, which all leave an operation with a dest_hi alone.
 */
static void a64_post_index(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        for (vir_mop_t *load = bb->mops.head; load; load = load->next) {
            int base = load->src0;
            if (load->op != OP_read || load->src0_hi >= 0 || load->src2 ||
                load->dest_hi >= 0 || base < 0 || base >= REG_CNT ||
                load->dest == base)
                continue;
            vir_mop_t *before = load, *step = load->next;
            bool found = false;
            for (int scan = 0; step && scan < 4 && !found; scan++) {
                found = (step->op == OP_add || step->op == OP_sub) &&
                        step->dest == base && step->src0 == base &&
                        step->size_bytes == 8 && step->src2 > 0 &&
                        step->src2 <= A64_IMM_SHIFTED && step->dest_hi < 0;
                if (found)
                    break;
                if (ir_reads_reg(step, base) ||
                    (mop_defs(step) & (1u << base)) || step->op == OP_branch ||
                    step->op == OP_jump || step->op == OP_return)
                    break;
                before = step;
                step = step->next;
            }
            if (!found)
                continue;
            int amount = a64_imm12(step);
            if (step->op == OP_sub)
                amount = -amount;
            if (amount < -256 || amount > 255)
                continue;
            load->dest_hi = base;
            load->src2 = amount;
            before->next = step->next;
            if (bb->mops.tail == step)
                bb->mops.tail = before;
        }
}

/* An int extended to 64 bits and then shifted left by a literal is one SBFIZ,
 * or UBFIZ for an unsigned one, as when an index scales to a byte offset. The
 * shift keeps the literal in src2 and names the extension's source in src0;
 * src1, which no register number matches, says which of the two it is. This
 * runs after the folds that would take the shift into an addition.
 */
#define A64_SBFIZ (-2)
#define A64_UBFIZ (-3)

static void a64_insert_fields(func_t *f)
{
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
        for (vir_mop_t **link = &bb->mops.head; *link && (*link)->next;) {
            vir_mop_t *extend = *link, *shift = extend->next;
            if (extend->op != OP_sign_ext || extend->src1 != ((4 << 16) | 8) ||
                extend->dest_hi >= 0 || extend->src0 < 0 ||
                extend->src0 >= REG_CNT || shift->op != OP_lshift ||
                shift->size_bytes != 8 || shift->src2 < 2 || shift->src2 > 33 ||
                shift->src0 != extend->dest || shift->dest_hi >= 0 ||
                !a64_value_dies(bb, extend, shift)) {
                link = &extend->next;
                continue;
            }
            shift->src0 = extend->src0;
            shift->src1 = extend->src0_is_unsigned ? A64_UBFIZ : A64_SBFIZ;
            *link = shift;
        }
}

/* Early returns that run ahead of the frame, as fib's base case does. When @f
 * starts with nothing but copies of its arguments, then a test of them whose
 * one side only returns an argument or nothing, that test and its return need
 * no saved register, so they leave @f's blocks for its prologue, which starts
 * with the comparison, a branch past the return, and the return. Recorded in
 * @f's early_test, early_cond and early_value.
 */
static void a64_early_exit(func_t *f)
{
    basic_block_t *entry = f->bbs;
    vir_mop_t *branch = entry->mops.tail, *compare = NULL;
    int argument[REG_CNT];
    for (int reg = 0; reg < REG_CNT; reg++)
        argument[reg] = reg < MAX_ARGS_IN_REG ? reg : -1;
    if (!branch || branch->op != OP_branch || !branch->src1)
        return;
    for (vir_mop_t *ir = entry->mops.head; ir != branch; ir = ir->next) {
        if (ir->next == branch && op_is_comparison(ir->op) &&
            (ir->src2 & A64_CMP_FUSED) && !(ir->src2 & A64_CMP_TESTED) &&
            ir->src0_hi < 0 && ir->src1_hi < 0) {
            compare = ir;
            break;
        }
        if (ir->op != OP_assign || ir->dest < 0 || ir->dest >= REG_CNT ||
            ir->src0 < 0 || ir->src0 >= REG_CNT || ir->dest_hi >= 0)
            return;
        argument[ir->dest] = argument[ir->src0];
    }
    if (!compare || compare->src0 < 0 || compare->src0 >= REG_CNT ||
        argument[compare->src0] < 0 ||
        (!(compare->src2 >> 1) &&
         (compare->src1 < 0 || compare->src1 >= REG_CNT ||
          argument[compare->src1] < 0)))
        return;

    /* The side that only returns, reached from nowhere else. */
    basic_block_t *exit = NULL, *rest = NULL;
    for (int side = 0; side < 2 && !exit; side++) {
        basic_block_t *bb = side ? branch->else_bb : branch->then_bb;
        vir_mop_t *ret = bb ? bb->mops.head : NULL;
        basic_block_t *before = bb ? a64_last_emitting(f->bbs, bb) : NULL;
        if (ret && ret->op == OP_return && !ret->next &&
            !(ret->src2 & A64_SHARED_EXIT) && ret->src0_hi < 0 &&
            (ret->src0 < 0 ||
             (ret->src0 < REG_CNT && argument[ret->src0] >= 0)) &&
            a64_branches_naming(f, bb) == 1 &&
            (before == entry || (before && a64_ends_closed(before)))) {
            exit = bb;
            rest = side ? branch->then_bb : branch->else_bb;
        }
    }
    if (!exit || !rest)
        return;

    vir_mop_t *test = mop_create(compare->op);
    *test = *compare;
    test->next = NULL;
    test->src0 = argument[compare->src0];
    if (!(compare->src2 >> 1))
        test->src1 = argument[compare->src1];
    vir_mop_t *ret = exit->mops.head;
    f->early_test = test;
    f->early_cond = (branch->src1 - 1) ^ (exit == branch->else_bb);
    f->early_value = ret->src0 < 0 ? -1 : argument[ret->src0];

    /* The entry block now only copies and goes on with the rest. */
    vir_mop_t **link = &entry->mops.head;
    while (*link != compare)
        link = &(*link)->next;
    vir_mop_t *jump = mop_create(OP_jump);
    jump->next_bb = rest;
    *link = jump;
    entry->mops.tail = jump;
    entry->then_ = entry->else_ = NULL;
    entry->next = rest;
    jump->src2 = a64_falls_into(entry, rest);
    exit->mops.head = exit->mops.tail = NULL;
}

void cfg_flatten(void)
{
    func_t *f;
    for (f = FUNC_LIST.head; f; f = f->next)
        if (f->bbs) {
            a64_merge_blocks(f);
            arm64_fold_immediates(f);
            a64_coalesce_results(f);
            a64_fuse_multiply_add(f);
            a64_drop_dead(f);
            a64_hoist_constants(f);
            a64_select_diamonds(f);
            for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
                for (vir_mop_t **link = &bb->mops.head; *link;
                     link = &(*link)->next) {
                    vir_mop_t *ir = *link, *next = ir->next;
                    if (a64_test_bits(bb, ir))
                        continue;
                    if (a64_branch_only_compare(bb, ir)) {
                        if (a64_branch_on_zero(ir, next))
                            *link = ir = next;
                        else {
                            ir->src2 |= A64_CMP_FUSED;
                            next->src1 =
                                a64_cond(ir->op, ir->src0_is_unsigned ||
                                                     ir->src1_is_unsigned) +
                                1;
                        }
                    }
                    if (ir->op == OP_jump)
                        ir->src2 = a64_falls_into(bb, ir->next_bb);
                    else if (ir->op == OP_branch)
                        ir->src2 = a64_branch_fallthrough(bb, ir);
                }

            /* Repeat a small loop test at its back edge so the body needs only
             * one taken branch per iteration. Keep the original comparison
             * flags; only the copied branch's physical fallthrough changes.
             */
            for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
                vir_mop_t *jump = bb->mops.tail;
                if (!jump || jump->op != OP_jump || !jump->next_bb ||
                    jump->next_bb->rpo > bb->rpo)
                    continue;
                basic_block_t *head = jump->next_bb;
                int count = 0;
                for (vir_mop_t *ir = head->mops.head; ir; ir = ir->next)
                    count += ir->op == OP_load || ir->op == OP_load_constant ||
                                     op_is_comparison(ir->op) ||
                                     ir->op == OP_branch
                                 ? 1
                                 : 4;
                if (!count || count > 3 || head->mops.tail->op != OP_branch)
                    continue;
                for (vir_mop_t *ir = head->mops.head; ir; ir = ir->next) {
                    *jump = *ir;
                    jump->next = ir->next ? mop_create(ir->next->op) : NULL;
                    bb->mops.tail = jump;
                    if (jump->next)
                        jump = jump->next;
                }
                jump->src2 = a64_branch_fallthrough(bb, jump);
            }
            a64_settle_entry_tests(f);
            a64_place_exits(f);
            a64_post_index(f);
            a64_insert_fields(f);
        }
    mop_flat_prepare(false);
    for (f = FUNC_LIST.head; f; f = f->next) {
        if (!f->bbs)
            continue;
        vir_mop_t *d = add_mop(OP_define);
        d->src0 = f->stack_size;
        d->func_name = intern_string(f->return_def.var_name);
        int saved =
            func_highest_used_reg(f, A64_FIRST_CALLEE) - (A64_FIRST_CALLEE - 1);
        if (dynlink && !strcmp(d->func_name, "main"))
            saved |= A64_SAVE_GP;
        if (!saved && func_is_frameless_leaf(f))
            saved |= A64_FRAMELESS;
        int locals = ALIGN_UP(f->stack_size, A64_STACK_ALIGN);
        if (!(saved & A64_FRAMELESS) && locals &&
            locals <= A64_FOLDED_FRAME_MAX && !f->passes_stack_args) {
            saved |= A64_FOLDED_FRAME;
            for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
                for (vir_mop_t *ir = bb->mops.head; ir; ir = ir->next) {
                    if (ir->ofs_based_on_stack_top)
                        continue;
                    if (ir->op == OP_load || ir->op == OP_address_of)
                        ir->src0 += 16;
                    else if (ir->op == OP_store)
                        ir->src1 += 16;
                }
        }
        d->src2 = saved;
        if (!(saved & (A64_FRAMELESS | A64_SAVE_GP)))
            a64_early_exit(f);

        /* Where the caller's stack arguments sit, seen from the callee's own
         * SP. This must round the frame exactly as the prologue does: rounding
         * to MIN_ALIGNMENT instead left it eight bytes short whenever
         * stack_size % 16 == 8, and every stack-passed argument was then read
         * one slot low.
         */
        int stack_top_ofs =
            ALIGN_UP(f->stack_size, A64_STACK_ALIGN) + a64_save_bytes(saved);
        mop_flatten_function(f, stack_top_ofs, 0, saved, true, NULL);
    }

    /* ADR reaches 1 MiB either way. A static image keeps .rodata after the code
     * and .data on the page after that, so when the whole span is in reach,
     * every address the code takes is one instruction; otherwise plan again
     * with absolute addresses.
     */
    for (f = FUNC_LIST.head; f; f = f->next)
        if (f->bbs)
            a64_mark_loop_starts(f);
    a64_pc_relative = !dynlink;
    a64_plan_offsets();
    if (a64_pc_relative &&
        elf_offset + elf_rodata->size + elf_data->size + 2 * PAGESIZE >=
            1 << 20) {
        a64_pc_relative = false;
        a64_plan_offsets();
    }
}

void emit_mop(vir_mop_t *p)
{
    int d = a64_reg(p->dest), n = a64_reg(p->src0), m = a64_reg(p->src1);
    if (op_is_comparison(p->op)) {
        int imm = (p->src2 >> 1) - A64_CMP_BIAS;
        if (p->src2 & A64_CMP_TESTED)
            ;
        else if (p->src2 >> 1 == 0)
            emit(a64_cmp_reg_insn(a64_cmp_wide(p), n, m));
        else if (imm < 0)
            emit(a64_cmn_imm_insn(a64_cmp_wide(p), n, -imm));
        else
            emit(a64_cmp_imm_insn(a64_cmp_wide(p), n, imm));
        if (!(p->src2 & A64_CMP_FUSED))
            emit(a64_cset_insn(
                a64_cmp_wide(p), d,
                a64_cond(p->op, p->src0_is_unsigned || p->src1_is_unsigned)));
        return;
    }
    switch (p->op) {
    case OP_define: {
        fatal_function_context = p->func_name;
        if (p->src2 & A64_FRAMELESS)
            return;
        func_t *func = find_func(p->func_name);
        if (func && func->early_test) {
            /* See a64_early_exit(): the test, a branch past the return when it
             * fails, the argument into x0 and the return.
             */
            int value = func->early_value, words = value > 0 ? 2 : 1;
            emit_mop(func->early_test);
            emit(a64_b_cond_insn(func->early_cond ^ 1, words + 1));
            if (value > 0)
                a64_mov(0, value);
            emit(a64_ret_insn());
        }
        int saves = (p->src2 & 15) + !!(p->src2 & A64_SAVE_GP);
        int locals = ALIGN_UP(p->src0, A64_STACK_ALIGN);
        bool folded = p->src2 & A64_FOLDED_FRAME;
        emit(a64_stp_pre_insn(A64_FP, A64_LR, A64_SP,
                              folded && !saves ? -16 - locals : -16));
        emit(a64_add_imm_insn(true, A64_FP, A64_SP, 0));
        for (int reg = 0; reg < saves; reg += 2)
            emit(a64_stp_pre_insn(
                a64_saved_reg(p->src2, reg), a64_saved_reg(p->src2, reg + 1),
                A64_SP, folded && reg + 2 >= saves ? -16 - locals : -16));
        if (!folded)
            a64_adjust_frame(locals, true);

        /* Reload x19 rather than trust what called us: glibc enters at main,
         * and AAPCS64 lets everything in between clobber a callee-saved
         * register it has saved. A64_LOAD is a64_mem()'s load selector, so this
         * reads the base back from the word parked at elf_data_start. The frame
         * itself is allocated once in code_generate(), which is also where the
         * question of clearing it is settled.
         */
        if (p->src2 & A64_SAVE_GP) {
            a64_mov_imm(A64_IP0, elf_data_start);
            a64_mem(A64_LOAD, 8, A64_GP, A64_IP0, 0);
        }
        return;
    }
    case OP_load_constant:
        a64_load_constant(d, p);
        return;
    case OP_assign:
        if (d != n)
            a64_mov(d, n);
        return;
    case OP_address_of:
    case OP_global_address_of: {
        /* Both offsets are final when the size estimate runs, so a 12-bit one
         * is an immediate of the ADD itself.
         */
        int base = p->op == OP_address_of ? A64_SP : A64_GP;
        if (p->src0 >= 0 && p->src0 < 4096) {
            emit(a64_add_imm_insn(true, d, base, p->src0));
            return;
        }
        if (p->src0 > 0 && p->src0 < 1 << 24) {
            emit(a64_add_imm_insn(true, d, base, p->src0 >> 12) |
                 A64_IMM_LSL12);
            if (p->src0 & 0xfff)
                emit(a64_add_imm_insn(true, d, d, p->src0 & 0xfff));
            return;
        }
        a64_mov_imm(A64_IP0, p->src0);
        a64_add(d, base, A64_IP0);
        return;
    }
    case OP_load:
        a64_mem(a64_load_access(p), p->size_bytes, d, A64_SP, p->src0);
        return;
    case OP_global_load:
        a64_mem(a64_load_access(p), p->size_bytes, d, A64_GP, p->src0);
        return;
    case OP_store:
        a64_mem(A64_STORE, p->size_bytes, n, A64_SP, p->src1);
        return;
    case OP_global_store:
        a64_mem(A64_STORE, p->size_bytes, n, A64_GP, p->src1);
        return;
    case OP_read:
    case OP_write: {
        bool read = p->op == OP_read;
        int access = read ? a64_load_access(p) : A64_STORE;
        int size = read ? p->src1 : p->dest;
        int rt = read ? d : m;
        if (read && p->dest_hi >= 0)
            emit(a64_mem_post_insn(access, size, rt, n, p->src2));
        else if (p->src0_hi >= 0)
            emit(a64_mem_register_insn(
                     access, size, rt, n, a64_reg(p->src0_hi),
                     p->src2 != -1 && p->src2 != -5 && p->src2 != -9,
                     p->src2 <= -5) |
                 (p->src2 <= -9 ? A64_SXTW_INDEX : 0));
        else
            a64_mem(access, size, rt, n, p->src2);
        return;
    }
    case OP_add:
        if (p->src2 < 0)
            emit(a64_add_reg_insn(a64_arith_wide(p), d, n, m) | a64_shifted(p));
        else if (p->src2)
            emit(a64_add_imm_insn(a64_arith_wide(p), d, n, a64_imm12(p)) |
                 (p->src2 > A64_IMM_SHIFTED ? A64_IMM_LSL12 : 0));
        else if (a64_arith_wide(p))
            a64_add(d, n, m);
        else
            emit(a64_add_reg_insn(false, d, n, m));
        return;
    case OP_sub:
        if (p->src2 < 0)
            emit(a64_sub_reg_insn(a64_arith_wide(p), d, n, m) | a64_shifted(p));
        else if (p->src2)
            emit(a64_sub_imm_insn(a64_arith_wide(p), d, n, a64_imm12(p)) |
                 (p->src2 > A64_IMM_SHIFTED ? A64_IMM_LSL12 : 0));
        else if (a64_arith_wide(p))
            a64_sub(d, n, m);
        else
            emit(a64_sub_reg_insn(false, d, n, m));
        return;
    case OP_mul:
        emit(p->src1_hi >= 0 ? a64_madd_insn(p->size_bytes == 8, d, n, m,
                                             a64_reg(p->src1_hi))
                             : a64_mul_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_div:
        emit(a64_div_insn(p, d, n, m));
        return;
    case OP_mod:
        /* d = n % m. Do not put the quotient in d: register coalescing may make
         * d alias n, losing the minuend before MSUB reads it.
         */
        emit(a64_div_insn(p, A64_IP0, n, m));
        emit(a64_msub_insn(p->size_bytes == 8, d, A64_IP0, m, n));
        return;
    case OP_lshift:
        if (p->src1 == A64_SBFIZ || p->src1 == A64_UBFIZ) {
            /* SBFIZ or UBFIZ Xd, Xn, #s, #32: SBFM/UBFM with immr 64 - s. */
            int shift = p->src2 - 1;
            emit((p->src1 == A64_UBFIZ ? 0xd3400000U : 0x93400000U) |
                 (((64 - shift) & 63) << 16) | (31 << 10) | (n << 5) | d);
            return;
        }
        emit(p->src2 ? a64_lsl_imm_insn(p->size_bytes == 8, d, n, p->src2 - 1)
                     : a64_lslv_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_rshift:
        emit(a64_rshift_insn(p, d, n, m));
        return;
    case OP_bit_and: {
        /* Into the zero register, it is ANDS for the comparison after it. */
        int flags = p->dest == A64_ZERO ? 0x60000000 : 0;
        emit((p->src2 > 0 ? a64_logic_imm_insn(0x12000000, p->size_bytes == 8,
                                               d, n, p->src2 - 1)
                          : a64_and_reg_insn(p->size_bytes == 8, d, n, m) |
                                a64_shifted(p)) |
             flags);
        return;
    }
    case OP_bit_or:
        emit(p->src2 > 0 ? a64_logic_imm_insn(0x32000000, p->size_bytes == 8, d,
                                              n, p->src2 - 1)
                         : a64_orr_reg_insn(p->size_bytes == 8, d, n, m) |
                               a64_shifted(p));
        return;
    case OP_bit_xor:
        emit(p->src2 > 0 ? a64_logic_imm_insn(0x52000000, p->size_bytes == 8, d,
                                              n, p->src2 - 1)
                         : a64_eor_reg_insn(p->size_bytes == 8, d, n, m) |
                               a64_shifted(p));
        return;
    case OP_negate:
        emit(a64_neg_insn(p->size_bytes == 8, d, n));
        return;
    case OP_bit_not:
        emit(a64_mvn_insn(p->size_bytes == 8, d, n));
        return;

    /* Width follows the operand, exactly as the comparisons above do. A pointer
     * or a long long must be tested whole, or one whose low word happens to be
     * zero reads as zero. An int must not be: multiply, divide, shift and the
     * bitwise operations all use the W forms, which leave the upper half zeroed
     * rather than sign-extended, so only the low word is the value.
     */
    case OP_log_not:
        emit(a64_cmp_imm_insn(p->src0_is_pointer || p->size_bytes == 8, n, 0));
        emit(a64_cset_insn(false, d, A64_EQ));
        return;

    /* OP_trunc's src1 is the target width; OP_sign_ext's packs the source width
     * in its upper half (see promote_unchecked()). Decoding both the same way
     * made every promotion a plain move.
     */
    case OP_trunc:
        a64_extend(d, n, p->src1, p->is_unsigned);
        return;
    case OP_sign_ext: {
        int src_size = (p->src1 >> 16) & 0xffff, dst_size = p->src1 & 0xffff;

        /* Widening to a pointer: the register already holds a full address, so
         * extending it from 32 bits would discard the upper half.
         */
        if (dst_size == PTR_SIZE && p->is_pointer)
            a64_mov(d, n);
        else
            a64_extend(d, n, src_size, p->src0_is_unsigned);
        return;
    }
    case OP_cast:
        a64_mov(d, n);
        return;
    case OP_ternary:
        emit(a64_csel_insn(true, d, n, m, p->src2));
        return;
    case OP_branch: {
        bool wide = p->src0_is_pointer || p->size_bytes == 8;
        if (p->src1 || p->src2 == 2) {
            int target =
                p->src2 == 2 ? p->else_bb->elf_offset : p->then_bb->elf_offset;
            int disp = (target - elf_code->size) / 4;
            a64_check_disp(disp, 19, "arm64 conditional branch out of range");
            if (p->src1)
                emit(a64_b_cond_insn((p->src1 - 1) ^ (p->src2 == 2), disp));
            else
                emit(a64_cbz_insn(wide, n, disp));
        } else
            a64_cbnz(wide, n, p->then_bb->elf_offset);
        if (!p->src2)
            a64_b(p->else_bb->elf_offset);
        return;
    }
    case OP_jump:
        if (!p->src2)
            a64_b(p->next_bb->elf_offset);
        return;
    case OP_call: {
        func_t *f = find_func(p->func_name);
        if (!f)
            fatal("arm64 call to unknown function");

        if (!f->bbs) {
            if (!dynlink)
                fatal("arm64 external call requires --dynlink");
            a64_bl_addr(dynamic_sections.elf_plt_start + f->plt_offset);
        } else
            emit(a64_bl_insn((f->bbs->elf_offset - elf_code->size) / 4));
        return;
    }
    case OP_load_data_address:
        a64_address(d, p->src0 + elf_data_start);
        return;
    case OP_load_rodata_address:
        a64_address(d, p->src0 + elf_rodata_start);
        return;
    case OP_address_of_func: {
        func_t *f = find_func(p->func_name);
        int target;
        if (!f)
            fatal("arm64 address of unknown function");
        if (!f->bbs) {
            if (!dynlink)
                fatal("arm64 external function address requires --dynlink");

            /* A PLT entry is a stable callable address. With eager binding, its
             * GOT slot already holds the resolved external target.
             */
            target = dynamic_sections.elf_plt_start + f->plt_offset;
        } else
            target = elf_code_start + f->bbs->elf_offset;
        a64_address(A64_IP0, target);
        a64_mem(A64_STORE, 8, A64_IP0, n, 0);
        return;
    }
    case OP_load_func:
        a64_mov(A64_IP0, n);
        return;
    case OP_indirect:
        emit(a64_blr_insn(A64_IP0));
        return;
    case OP_return: {
        if (p->src0 >= 0)
            a64_mov(0, n);
        if (p->src2 & A64_SHARED_EXIT) {
            a64_b(p->dest);
            return;
        }
        int locals = ALIGN_UP(p->src1, A64_STACK_ALIGN), pop = 16;
        if (p->src2 & A64_FOLDED_FRAME)
            pop += locals;
        else
            a64_adjust_frame(locals, false);
        for (int reg =
                 (((p->src2 & 15) + !!(p->src2 & A64_SAVE_GP) + 1) & ~1) - 2;
             reg >= 0; reg -= 2) {
            emit(a64_ldp_post_insn(a64_saved_reg(p->src2, reg),
                                   a64_saved_reg(p->src2, reg + 1), A64_SP,
                                   pop));
            pop = 16;
        }
        if (!(p->src2 & A64_FRAMELESS))
            emit(a64_ldp_post_insn(A64_FP, A64_LR, A64_SP, pop));
        emit(a64_ret_insn());
        return;
    }
    default:
        fatal("unknown arm64 opcode");
    }
}

/* Each PLT entry is ADRP/ADD to form the GOT slot address, then an indirect
 * branch through it. ADD carries the byte offset rather than folding it into a
 * scaled LDR, because .got follows the byte-aligned interpreter string and so
 * is not guaranteed to be eight-byte aligned.
 */
void plt_generate(void)
{
    int entries = (dynamic_sections.plt_size - PLT_FIXUP_SIZE) / PLT_ENT_SIZE;
    int ent = dynamic_sections.elf_plt_start;
    int got =
        dynamic_sections.elf_got_start + PTR_SIZE * (RESERVED_GOT_NUM - 1);

    /* _dl_runtime_resolve() requires:
     *
     * - ip0: Address of got[2] (&got[2])
     * - ip1: got[2]
     * - [sp]: Address of got[N + 3] (Address of the N-th function's GOT stub.)
     * - [sp + 8]: Value stored in register lr
     *
     * PLT[0] uses the STP, ADRP, LDR, ADD and BR instructions to prepare the
     * above arguments and jump to the resolver.
     *
     * The remaining 3 nop instructions are used to pad PLT[0] to 32 bytes,
     * allowing the later sections to stay aligned.
     */
    elf_write_int(dynamic_sections.elf_plt,
                  a64_stp_pre_insn(A64_IP0, A64_LR, A64_SP, -16));
    elf_write_int(dynamic_sections.elf_plt,
                  a64_adrp_insn(A64_IP0, ent + 4, got));
    elf_write_int(dynamic_sections.elf_plt,
                  a64_add_imm_insn(true, A64_IP0, A64_IP0, got & 0xfff));
    elf_write_int(dynamic_sections.elf_plt,
                  a64_mem_scaled_insn(A64_LOAD, 8, A64_IP1, A64_IP0, 0));
    elf_write_int(dynamic_sections.elf_plt, a64_br_insn(A64_IP1));
    elf_write_int(dynamic_sections.elf_plt, a64_nop_insn());
    elf_write_int(dynamic_sections.elf_plt, a64_nop_insn());
    elf_write_int(dynamic_sections.elf_plt, a64_nop_insn());
    ent += PLT_FIXUP_SIZE;
    for (int i = 0; i < entries; i++) {
        /* Each PLT stub uses the ADRP, ADD, LDR and BR instructions to pass
         * &got[N + 3] and got[N + 3] to ip0 and ip1, respectively.
         */
        got =
            dynamic_sections.elf_got_start + PTR_SIZE * (RESERVED_GOT_NUM + i);
        elf_write_int(dynamic_sections.elf_plt,
                      a64_adrp_insn(A64_IP0, ent, got));
        elf_write_int(dynamic_sections.elf_plt,
                      a64_add_imm_insn(true, A64_IP0, A64_IP0, got & 0xfff));
        elf_write_int(dynamic_sections.elf_plt,
                      a64_mem_scaled_insn(A64_LOAD, 8, A64_IP1, A64_IP0, 0));
        elf_write_int(dynamic_sections.elf_plt, a64_br_insn(A64_IP1));
        ent += PLT_ENT_SIZE;
    }
}

/* The process entry, up to the global initializers that follow it. The
 * synthetic global frame is carved out of the runtime stack, and x19 keeps its
 * base.
 *
 * A static image leaves argc and argv where the kernel put them, just above the
 * frame. It is the first thing to run, so the stack below the entry SP is
 * untouched anonymous memory and already reads as zero.
 *
 * A dynamic one parks them above the frame instead, since global initializers
 * may use every allocated register, together with the original SP that
 * __libc_start_main wants as stack_end. It also parks the base in the data
 * segment, where main's prologue, entered from glibc, reads it back. And it
 * clears the frame: the loader and glibc's startup have run over that memory
 * first, so a global with no initializer would otherwise begin life holding
 * their leftovers.
 */
static void a64_emit_entry(void)
{
    int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);

    if (!dynlink) {
        a64_adjust_frame(global_frame, true);
        a64_mov_sp(A64_GP);
        return;
    }
    a64_mov_sp(25);
    a64_mem(A64_LOAD, 8, 20, A64_SP, 0);
    emit(a64_add_imm_insn(true, 21, A64_SP, 8));
    a64_mov_imm(A64_IP0, 32);
    a64_sub(A64_SP, A64_SP, A64_IP0);
    a64_mem(A64_STORE, 8, 20, A64_SP, 0);
    a64_mem(A64_STORE, 8, 21, A64_SP, 8);
    a64_mem(A64_STORE, 8, 25, A64_SP, 16);
    a64_mov_imm(A64_IP0, global_frame);
    a64_sub(A64_SP, A64_SP, A64_IP0);
    a64_mov_sp(A64_GP);
    a64_mov_imm(A64_IP0, elf_data_start);
    a64_mem(A64_STORE, 8, A64_GP, A64_IP0, 0);

    /* Clear the frame inline rather than through libc's memset, which a
     * --no-libc translation unit never declares. The frame is a multiple of
     * sixteen bytes and may be empty: count x2 down to zero while x0 walks up
     * through it.
     */
    a64_mov(0, A64_GP);
    a64_mov_imm(2, global_frame);
    emit(a64_cbz_insn(true, 2, 4));
    emit(a64_mem_post_insn(A64_STORE, 8, A64_ZR, 0, 8));
    emit(a64_sub_imm_insn(true, 2, 2, 8));
    emit(a64_cbnz_insn(true, 2, -2));
}

/* After the global initializers: call main with argc and argv, and leave with
 * its result, through __libc_start_main when linking dynamically.
 */
static void a64_emit_main_call(void)
{
    int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);

    if (!dynlink) {
        /* Pass argc and argv only as far as main declares them. */
        int params = find_func("main")->num_params;
        if (params > 0)
            a64_mem(A64_LOAD, 8, 0, A64_SP, global_frame);
        if (params > 1)
            a64_add_sp(1, global_frame + 8);
        emit(a64_bl_insn((MAIN_BB->elf_offset - elf_code->size) / 4));
        emit_arm64_wide_const(8, 93);
        emit(a64_svc_insn(0));
        return;
    }
    a64_mem(A64_LOAD, 8, 23, A64_SP, global_frame);
    a64_mem(A64_LOAD, 8, 24, A64_SP, global_frame + 8);
    a64_mem(A64_LOAD, 8, 25, A64_SP, global_frame + 16);
    a64_mov_imm(0, elf_code_start + MAIN_BB->elf_offset);
    a64_mov(1, 23);
    a64_mov(2, 24);
    a64_mov(3, A64_ZR);
    a64_mov(4, A64_ZR);
    a64_mov(5, A64_ZR);
    a64_mov(6, 25);
    a64_bl_addr(dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE);
    emit(a64_brk_insn(0)); /* __libc_start_main does not return */
}

/* __syscall(number,arg1,...): AArch64 Linux wants x8,x0..x5. */
static void a64_emit_syscall(void)
{
    a64_mov(8, 0);
    a64_mov(0, 1);
    a64_mov(1, 2);
    a64_mov(2, 3);
    a64_mov(3, 4);
    a64_mov(4, 5);
    a64_mov(5, 6);
    emit(a64_svc_insn(0));
    emit(a64_ret_insn());
}

/* Pad with NOPs up to @offset, where a64_plan_offsets() placed a block. */
static void a64_pad_to(int offset)
{
    while (elf_code->size < offset)
        emit(a64_nop_insn());
}

void code_generate(void)
{
    a64_emit_entry();
    mop_visit_globals(emit_mop);
    if (MAIN_BB)
        a64_emit_main_call();
    if (a64_needs_syscall())
        a64_emit_syscall();

    /* Walk the functions as a64_plan_offsets() did, padding up to the block
     * offsets it planned.
     */
    int index = 0;
    for (func_t *f = FUNC_LIST.head; f; f = f->next) {
        if (!f->bbs)
            continue;
        emit_mop(MOP_FLAT[index++]);
        for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
            a64_pad_to(bb->elf_offset);
            for (vir_mop_t *mop = bb->mops.head; mop; mop = mop->next) {
                index++;
                emit_mop(mop);
            }
        }
    }
    if (index != mop_flat_count)
        fatal("arm64 emission order differs from the flat order");
    if (elf_code->size != elf_offset)
        fatal("arm64 code-size accounting mismatch");
    if (dynlink) {
        /* Dynamic addresses depend on the final text extent. The ARM64 stream
         * is fixed-width, but .rodata may still have gained alignment padding
         * since elf_preprocess(); rebuild the address-bearing tables
         * immediately before writing PLT bytes.
         */
        elf_rodata_start = elf_code_start + elf_code->size;
        elf_layout_dynamic();
        elf_reset_dynamic_sections();
        elf_generate_dynamic_sections();
        plt_generate();
    }
}
