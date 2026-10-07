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
void emit_ph2_ir(ph2_ir_t *ir);
void a64_mov_imm(int d, int v);
void a64_mov_sp(int d);
void a64_mem(int access, int size, int rt, int rn, int ofs);
void a64_extend(int d, int n, int size, bool is_unsigned);
void a64_bl_addr(int target);

/* Offset planning counts the same instruction stream used for emission. */
static bool a64_count_only;
static int a64_counted_words;

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
    return r < 8 ? r : r < A64_FIRST_CALLEE ? r + 1 : r + 5;
}
void emit(int insn)
{
    if (a64_count_only) {
        if (a64_counted_words < INT_MAX)
            a64_counted_words++;
    } else {
        elf_write_int(elf_code, insn);
    }
}
void a64_mov(int d, int n)
{
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

/* A phase-2 constant is final before sizes are estimated, so unlike an address
 * it takes only the halfwords it needs; update_elf_offset() counts this very
 * emission. An eight-byte constant keeps its upper word in src1, and a smaller
 * one widens to the X register by its C signedness.
 */
static unsigned long long a64_constant_value(const ph2_ir_t *p)
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

static void a64_load_constant(int d, const ph2_ir_t *p)
{
    emit_arm64_wide_const(d, a64_constant_value(p));
}

/* Rd = Rn / Rm, unsigned when either operand is. */
int a64_div_insn(ph2_ir_t *p, int d, int n, int m)
{
    bool sf = p->size_bytes == 8;

    if (p->src0_is_unsigned || p->src1_is_unsigned)
        return a64_udiv_insn(sf, d, n, m);
    return a64_sdiv_insn(sf, d, n, m);
}

/* Rd = Rn >> Rm, logical for an unsigned left operand. */
int a64_rshift_insn(ph2_ir_t *p, int d, int n, int m)
{
    bool sf = p->size_bytes == 8;

    if (p->src0_is_unsigned)
        return a64_lsrv_insn(sf, d, n, m);
    return a64_asrv_insn(sf, d, n, m);
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
    if (bytes < 4096)
        emit(subtract ? a64_sub_imm_insn(true, A64_SP, A64_SP, bytes)
                      : a64_add_imm_insn(true, A64_SP, A64_SP, bytes));
    else {
        a64_mov_imm(A64_IP0, bytes);
        if (subtract)
            a64_sub(A64_SP, A64_SP, A64_IP0);
        else
            a64_add(A64_SP, A64_SP, A64_IP0);
    }
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
int a64_load_access(ph2_ir_t *p)
{
    return p->is_unsigned ? A64_LOAD_ZEXT : A64_LOAD;
}

/* AArch64's ordinary LDR/STR immediate is scaled by the access width. The
 * compiler deliberately supports packed C layouts, so structure members are
 * often not naturally aligned (func_t.bbs, for example). Such offsets must use
 * the byte-addressed LDUR/STUR form; rounding them down corrupts adjacent
 * fields during self-hosting.
 */
void a64_mem(int access, int size, int rt, int rn, int ofs)
{
    if (size != 1 && size != 2 && size != 4 && size != 8)
        fatal("unsupported arm64 access width");
    if (ofs >= -256 && ofs <= 255 && (ofs < 0 || ofs % size)) {
        emit(a64_mem_unscaled_insn(access, size, rt, rn, ofs));
        return;
    }
    if (ofs < 0 || ofs > 4095 * size || ofs % size) {
        a64_mov_imm(A64_IP0, ofs);
        a64_add(A64_IP0, rn, A64_IP0);
        rn = A64_IP0;
        ofs = 0;
    }
    emit(a64_mem_scaled_insn(access, size, rt, rn, ofs));
}

/* Masking an out-of-range displacement silently branches somewhere else. The
 * generated image is small enough that these never fire today, so say so rather
 * than let a larger input fail as a wild jump.
 */
void a64_check_disp(int disp, int bits, char *what)
{
    if (a64_count_only)
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
bool a64_cmp_wide(ph2_ir_t *p)
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

/* Which successor of a branch, if any, is the block laid out after @bb. */
static int a64_branch_fallthrough(basic_block_t *bb, ph2_ir_t *ir)
{
    return ir->else_bb == bb->rpo_next   ? 1
           : ir->then_bb == bb->rpo_next ? 2
                                         : 0;
}

static bool a64_branch_only_compare(basic_block_t *bb, ph2_ir_t *ir)
{
    return op_is_comparison(ir->op) && ir->next && ir->next->op == OP_branch &&
           ir->next->src0 == ir->dest &&
           !(bb->machine_liveout & (1U << ir->dest));
}

static int arm64_count_ph2_ir_words(ph2_ir_t *ir)
{
    char *saved_fatal_function_context = fatal_function_context;
    a64_count_only = true;
    a64_counted_words = 0;
    emit_ph2_ir(ir);
    int words = a64_counted_words;
    a64_count_only = false;

    fatal_function_context = saved_fatal_function_context;
    return words;
}

/* Count the actual instruction sequence so offset planning stays in step with
 * the emitter as it changes.
 */
void update_elf_offset(ph2_ir_t *ir)
{
    if (ir->op != OP_allocat)
        elf_offset += arm64_count_ph2_ir_words(ir) * 4;
}

/* Loop literals use otherwise idle callee-saved registers. Their definitions
 * move to function entry; a local copy remains when a successor or call needs
 * its old lane. Values and addresses outside this literal-only pass are left to
 * the shared lowerer.
 */
static void a64_hoist_constants(func_t *f)
{
    int first = func_highest_used_reg(f, A64_FIRST_CALLEE) + 1;
    int begin = INT_MAX, end = -1, count = 0;
    ph2_ir_t *constants[CALLEE_SAVED_REGS];
    if (first >= REG_CNT)
        return;
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        basic_block_t *targets[3] = {bb->next, bb->then_, bb->else_};
        for (int i = 0; i < 3; i++)
            if (targets[i] && targets[i]->rpo <= bb->rpo) {
                if (targets[i]->rpo < begin)
                    begin = targets[i]->rpo;
                if (bb->rpo > end)
                    end = bb->rpo;
            }
    }
    for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
        if (bb->rpo < begin || bb->rpo > end)
            continue;
        ph2_ir_t **link = &bb->ph2_ir_list.head, *previous = NULL;
        while (*link) {
            ph2_ir_t *ir = *link;
            if (ir->op != OP_load_constant) {
                previous = ir;
                link = &ir->next;
                continue;
            }
            unsigned long long value = a64_constant_value(ir);
            int index = 0;
            while (index < count &&
                   a64_constant_value(constants[index]) != value)
                index++;
            if (index == count) {
                if (first + count >= REG_CNT) {
                    previous = ir;
                    link = &ir->next;
                    continue;
                }
                ph2_ir_t *setup = ph2_ir_create(OP_load_constant);
                *setup = *ir;
                setup->dest = first + count;
                setup->next = NULL;
                if (count)
                    constants[count - 1]->next = setup;
                constants[count++] = setup;
            }
            int old = ir->dest, reg = constants[index]->dest;
            bool copy = (bb->machine_liveout & (1u << old)) != 0;
            for (ph2_ir_t *use = ir->next; !copy && use; use = use->next) {
                if (use->op == OP_call || use->op == OP_indirect) {
                    copy = true;
                    break;
                }
                if (op_writes_dest(use->op) && use->dest == old)
                    break;
            }
            if (copy) {
                ir->op = OP_assign;
                ir->src0 = reg;
                previous = ir;
                link = &ir->next;
                continue;
            }
            for (ph2_ir_t *use = ir->next; use; use = use->next) {
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
            if (bb->ph2_ir_list.tail == ir)
                bb->ph2_ir_list.tail = previous;
        }
    }
    if (count) {
        constants[count - 1]->next = f->bbs->ph2_ir_list.head;
        f->bbs->ph2_ir_list.head = constants[0];
        if (!f->bbs->ph2_ir_list.tail)
            f->bbs->ph2_ir_list.tail = constants[count - 1];
    }
}

/* Fold a dead adjacent literal into the arithmetic instruction. src2 is an
 * opcode-local immediate plus one; zero retains the register form.
 */
static void arm64_fold_immediates(func_t *func)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        ph2_ir_t **link = &bb->ph2_ir_list.head;
        while (*link && (*link)->next) {
            ph2_ir_t *literal = *link, *use = literal->next;
            long long value = (long long) a64_constant_value(literal);
            if (literal->op == OP_load_constant && use->src1 == literal->dest &&
                use->src0 != literal->dest &&
                (use->op == OP_bit_and || use->op == OP_bit_or ||
                 use->op == OP_bit_xor) &&
                !reg_read_after(bb, use, literal->dest)) {
                unsigned long long mask = a64_constant_value(literal);
                int ones = 0, bits = use->size_bytes == 8 ? 64 : 32;
                while (ones < bits && (mask & 1)) {
                    ones++;
                    mask >>= 1;
                }
                if (!mask && ones && ones < bits) {
                    use->src2 = ones;
                    use->src1 = -1;
                    *link = use;
                    continue;
                }
            }
            if (literal->op == OP_lshift && literal->src2 > 0 &&
                literal->size_bytes == 8 && use->size_bytes == 8 &&
                (use->op == OP_add || use->op == OP_sub) && !use->src2 &&
                use->src1 == literal->dest && use->src0 != literal->dest &&
                !reg_read_after(bb, use, literal->dest)) {
                use->src1 = literal->src0;
                use->src2 = -literal->src2;
                *link = use;
                continue;
            }
            if (literal->op != OP_load_constant || use->src1 != literal->dest ||
                use->src0 == literal->dest ||
                (use->op != OP_add && use->op != OP_sub &&
                 use->op != OP_lshift) ||
                reg_read_after(bb, use, literal->dest) ||
                (use->op == OP_lshift
                     ? value < 0 || value >= (use->size_bytes == 8 ? 64 : 32)
                     : value < -4095 || value > 4095)) {
                link = &literal->next;
                continue;
            }
            if (value < 0) {
                use->op = use->op == OP_add ? OP_sub : OP_add;
                value = -value;
            }
            use->src2 = (int) value + 1;
            use->src1 = -1;
            *link = use;
        }
        link = &bb->ph2_ir_list.head;
        while (*link && (*link)->next) {
            ph2_ir_t *address = *link, *memory = address->next;
            int size = memory->op == OP_read ? memory->src1 : memory->dest;
            if (address->size_bytes != 8 ||
                (address->op != OP_add && address->op != OP_sub) ||
                (memory->op != OP_read && memory->op != OP_write) ||
                (address->src2 <= 0 &&
                 (address->op != OP_add || address->src2 < -4 ||
                  (address->src2 < -1 &&
                   (1 << (-address->src2 - 1)) != size))) ||
                memory->src0_hi >= 0 || memory->src2 != 0 ||
                memory->src0 != address->dest ||
                (memory->op == OP_write && memory->src1 == address->dest) ||
                reg_read_after(bb, memory, address->dest)) {
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
        link = &bb->ph2_ir_list.head;
        while (*link && (*link)->next) {
            ph2_ir_t *extend = *link, *memory = extend->next;
            if (extend->op != OP_sign_ext || extend->src1 != ((4 << 16) | 8) ||
                !extend->src0_is_unsigned || extend->is_pointer ||
                (memory->op != OP_read && memory->op != OP_write) ||
                memory->src0_hi != extend->dest ||
                memory->src0 == extend->dest || memory->src2 > -1 ||
                memory->src2 < -4 ||
                (memory->op == OP_write && memory->src1 == extend->dest) ||
                reg_read_after(bb, memory, extend->dest)) {
                link = &extend->next;
                continue;
            }
            memory->src0_hi = extend->src0;
            memory->src2 -= 4;
            *link = memory;
        }
    }
}

/* Select between short, effect-free expression arms without a data-dependent
 * branch. Only a single live result may change; both scratch evaluations read
 * the original inputs and retain the comparison flags.
 */
static void a64_select_diamonds(func_t *func)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next)
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next)
            if (ir->op == OP_load_func || ir->op == OP_indirect)
                return;
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        ph2_ir_t *branch = bb->ph2_ir_list.tail, *compare = NULL;
        if (!branch || branch->op != OP_branch)
            continue;
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next)
            if (ir->next == branch)
                compare = ir;
        if (!compare || !a64_branch_only_compare(bb, compare))
            continue;
        basic_block_t *arms[2][4];
        ph2_ir_t *ops[2][4];
        int lengths[2] = {0}, nops[2] = {0}, join_at[2] = {-1, -1};
        bool safe = true;
        for (int side = 0; side < 2; side++) {
            basic_block_t *arm = side ? branch->else_bb : branch->then_bb;
            while (arm && lengths[side] < 4) {
                arms[side][lengths[side]++] = arm;
                ph2_ir_t *tail = arm->ph2_ir_list.tail;
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
                for (ph2_ir_t *ir = arm->ph2_ir_list.head; ir; ir = ir->next) {
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
        ph2_ir_t *tail = compare;
        compare->src2 = 1;
        for (int side = 0; side < 2; side++) {
            int last = -1, scratch = REG_CNT + side;
            for (int index = 0; index < nops[side]; index++) {
                ph2_ir_t *old = ops[side][index];
                ph2_ir_t *ir = ph2_ir_create(old->op);
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
                arms[side][index]->ph2_ir_list.head = NULL;
                arms[side][index]->ph2_ir_list.tail = NULL;
            }
        }
        ph2_ir_t *select = ph2_ir_create(OP_ternary);
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

void cfg_flatten(void)
{
    func_t *f;
    for (f = FUNC_LIST.head; f; f = f->next)
        if (f->bbs) {
            arm64_fold_immediates(f);
            a64_hoist_constants(f);
            a64_select_diamonds(f);
            for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next)
                for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
                    ph2_ir_t *next = ir->next;
                    if (a64_branch_only_compare(bb, ir)) {
                        ir->src2 = 1;
                        next->src1 =
                            a64_cond(ir->op, ir->src0_is_unsigned ||
                                                 ir->src1_is_unsigned) +
                            1;
                    }
                    if (ir->op == OP_jump)
                        ir->src2 = ir->next_bb == bb->rpo_next;
                    else if (ir->op == OP_branch)
                        ir->src2 = a64_branch_fallthrough(bb, ir);
                }

            /* Repeat a small loop test at its back edge so the body needs only
             * one taken branch per iteration. Keep the original comparison
             * flags; only the copied branch's physical fallthrough changes.
             */
            for (basic_block_t *bb = f->bbs; bb; bb = bb->rpo_next) {
                ph2_ir_t *jump = bb->ph2_ir_list.tail;
                if (!jump || jump->op != OP_jump || !jump->next_bb ||
                    jump->next_bb->rpo > bb->rpo)
                    continue;
                basic_block_t *head = jump->next_bb;
                int count = 0;
                for (ph2_ir_t *ir = head->ph2_ir_list.head; ir; ir = ir->next)
                    count += ir->op == OP_load || ir->op == OP_load_constant ||
                                     op_is_comparison(ir->op) ||
                                     ir->op == OP_branch
                                 ? 1
                                 : 4;
                if (!count || count > 3 ||
                    head->ph2_ir_list.tail->op != OP_branch)
                    continue;
                for (ph2_ir_t *ir = head->ph2_ir_list.head; ir; ir = ir->next) {
                    *jump = *ir;
                    jump->next = ir->next ? ph2_ir_create(ir->next->op) : NULL;
                    bb->ph2_ir_list.tail = jump;
                    if (jump->next)
                        jump = jump->next;
                }
                jump->src2 = a64_branch_fallthrough(bb, jump);
            }
        }
    ph2_ir_prepare(false);
    bool has_main = MAIN_BB != NULL;

    /* Entry sequence lengths, which the block offsets below start after.
     * Static: 15 instructions of setup, then the 9-instruction __syscall
     * helper, so 24 in all. Dynamic: 24, having no __syscall helper but saving
     * the original stack pointer for __libc_start_main's stack_end argument,
     * spilling argc/argv, and clearing the frame with an inline loop.
     */
    f = find_func("__syscall");
    if (f && f->bbs) {
        if (dynlink)
            f->bbs->elf_offset = 0;
        else
            f->bbs->elf_offset = 15 * 4;
    }
    elf_offset = 24 * 4;
    GLOBAL_FUNC->bbs->elf_offset = elf_offset;
    ph2_ir_visit_globals(update_elf_offset);

    /* code_generate() emits the call to main only when there is one, so a
     * translation unit without main must not be charged for it either.
     */
    if (has_main) {
        int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);
        if (dynlink)
            elf_offset += (10 + a64_mem_count(8, global_frame) +
                           a64_mem_count(8, global_frame + 8) +
                           a64_mem_count(8, global_frame + 16)) *
                          4;
        else
            elf_offset += (6 + a64_mem_count(8, global_frame) +
                           a64_mem_count(8, global_frame + 8)) *
                          4;
    }
    for (f = FUNC_LIST.head; f; f = f->next) {
        if (!f->bbs)
            continue;
        ph2_ir_t *d = add_ph2_ir(OP_define);
        d->src0 = f->stack_size;
        d->func_name = intern_string(f->return_def.var_name);
        int saved =
            func_highest_used_reg(f, A64_FIRST_CALLEE) - (A64_FIRST_CALLEE - 1);
        if (dynlink && !strcmp(d->func_name, "main"))
            saved |= A64_SAVE_GP;
        d->src2 = saved;

        /* Where the caller's stack arguments sit, seen from the callee's own
         * SP. This must round the frame exactly as the prologue does: rounding
         * to MIN_ALIGNMENT instead left it eight bytes short whenever
         * stack_size % 16 == 8, and every stack-passed argument was then read
         * one slot low.
         */
        int stack_top_ofs =
            ALIGN_UP(f->stack_size, A64_STACK_ALIGN) + a64_save_bytes(saved);
        int prologue_bytes = arm64_count_ph2_ir_words(d) * 4;
        ph2_ir_flatten_function(f, stack_top_ofs, prologue_bytes, saved, true,
                                update_elf_offset);
    }
}

void emit_ph2_ir(ph2_ir_t *p)
{
    int d = a64_reg(p->dest), n = a64_reg(p->src0), m = a64_reg(p->src1);
    if (op_is_comparison(p->op)) {
        emit(a64_cmp_reg_insn(a64_cmp_wide(p), n, m));
        if (!p->src2)
            emit(a64_cset_insn(
                a64_cmp_wide(p), d,
                a64_cond(p->op, p->src0_is_unsigned || p->src1_is_unsigned)));
        return;
    }
    switch (p->op) {
    case OP_define: {
        fatal_function_context = p->func_name;
        emit(a64_stp_pre_insn(A64_FP, A64_LR, A64_SP, -16));
        emit(a64_add_imm_insn(true, A64_FP, A64_SP, 0));
        for (int reg = 0; reg < (p->src2 & 15) + !!(p->src2 & A64_SAVE_GP);
             reg += 2)
            emit(a64_stp_pre_insn(a64_saved_reg(p->src2, reg),
                                  a64_saved_reg(p->src2, reg + 1), A64_SP,
                                  -16));
        a64_adjust_frame(ALIGN_UP(p->src0, A64_STACK_ALIGN), true);

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
        a64_mov_imm(A64_IP0, p->src0);
        a64_add(d, A64_SP, A64_IP0);
        return;
    case OP_global_address_of:
        a64_mov_imm(A64_IP0, p->src0);
        a64_add(d, A64_GP, A64_IP0);
        return;
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
        if (p->src0_hi >= 0)
            emit(a64_mem_register_insn(access, size, rt, n, a64_reg(p->src0_hi),
                                       p->src2 != -1 && p->src2 != -5,
                                       p->src2 <= -5));
        else
            a64_mem(access, size, rt, n, p->src2);
        return;
    }
    case OP_add:
        if (p->src2 < 0)
            emit(a64_add_reg_insn(true, d, n, m) | ((-p->src2 - 1) << 10));
        else if (p->src2)
            emit(a64_add_imm_insn(true, d, n, p->src2 - 1));
        else
            a64_add(d, n, m);
        return;
    case OP_sub:
        if (p->src2 < 0)
            emit(a64_sub_reg_insn(true, d, n, m) | ((-p->src2 - 1) << 10));
        else if (p->src2)
            emit(a64_sub_imm_insn(true, d, n, p->src2 - 1));
        else
            a64_sub(d, n, m);
        return;
    case OP_mul:
        emit(a64_mul_insn(p->size_bytes == 8, d, n, m));
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
        emit(p->src2 ? a64_lsl_imm_insn(p->size_bytes == 8, d, n, p->src2 - 1)
                     : a64_lslv_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_rshift:
        emit(a64_rshift_insn(p, d, n, m));
        return;
    case OP_bit_and:
        emit(p->src2 ? a64_logic_imm_insn(0x12000000, p->size_bytes == 8, d, n,
                                          p->src2)
                     : a64_and_reg_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_bit_or:
        emit(p->src2 ? a64_logic_imm_insn(0x32000000, p->size_bytes == 8, d, n,
                                          p->src2)
                     : a64_orr_reg_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_bit_xor:
        emit(p->src2 ? a64_logic_imm_insn(0x52000000, p->size_bytes == 8, d, n,
                                          p->src2)
                     : a64_eor_reg_insn(p->size_bytes == 8, d, n, m));
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
        a64_mov_imm(d, p->src0 + elf_data_start);
        return;
    case OP_load_rodata_address:
        a64_mov_imm(d, p->src0 + elf_rodata_start);
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
        a64_mov_imm(A64_IP0, target);
        a64_mem(A64_STORE, 8, A64_IP0, n, 0);
        return;
    }
    case OP_load_func:
        a64_mov(A64_IP0, n);
        return;
    case OP_indirect:
        emit(a64_blr_insn(A64_IP0));
        return;
    case OP_return:
        if (p->src0 >= 0 && n != 0)
            a64_mov(0, n);
        a64_adjust_frame(ALIGN_UP(p->src1, A64_STACK_ALIGN), false);
        for (int reg =
                 (((p->src2 & 15) + !!(p->src2 & A64_SAVE_GP) + 1) & ~1) - 2;
             reg >= 0; reg -= 2)
            emit(a64_ldp_post_insn(a64_saved_reg(p->src2, reg),
                                   a64_saved_reg(p->src2, reg + 1), A64_SP,
                                   16));
        emit(a64_ldp_post_insn(A64_FP, A64_LR, A64_SP, 16));
        emit(a64_ret_insn());
        return;
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
    int entries = dynamic_sections.plt_size / PLT_ENT_SIZE;
    for (int i = 0; i < entries; i++) {
        int ent = dynamic_sections.elf_plt_start + i * PLT_ENT_SIZE;
        int got =
            dynamic_sections.elf_got_start + PTR_SIZE * (RESERVED_GOT_NUM + i);
        elf_write_int(dynamic_sections.elf_plt,
                      a64_adrp_insn(A64_IP0, ent, got));
        elf_write_int(dynamic_sections.elf_plt,
                      a64_add_imm_insn(true, A64_IP0, A64_IP0, got & 0xfff));
        elf_write_int(dynamic_sections.elf_plt,
                      a64_mem_scaled_insn(A64_LOAD, 8, A64_IP1, A64_IP0, 0));
        elf_write_int(dynamic_sections.elf_plt, a64_br_insn(A64_IP1));
    }
}

void code_generate(void)
{
    int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);
    bool has_main = MAIN_BB != NULL;
    int main_offset = has_main ? MAIN_BB->elf_offset : -1;

    /* Park process arguments above the global frame: global initializers may
     * use every allocated register. Dynamic startup also needs the original SP.
     */
    if (dynlink)
        a64_mov_sp(25);
    a64_mem(A64_LOAD, 8, 20, A64_SP, 0);
    emit(a64_add_imm_insn(true, 21, A64_SP, 8));
    a64_mov_imm(A64_IP0, 32);
    a64_sub(A64_SP, A64_SP, A64_IP0);
    a64_mem(A64_STORE, 8, 20, A64_SP, 0);
    a64_mem(A64_STORE, 8, 21, A64_SP, 8);
    if (dynlink)
        a64_mem(A64_STORE, 8, 25, A64_SP, 16);

    /* The synthetic global frame is carved out of the runtime stack, and x19
     * keeps its base; only that base is parked in the data segment, where a
     * prologue entered from glibc can read it back.
     *
     * Clearing it is the dynamic build's job alone. A static image is the first
     * thing to run, so the stack below the entry SP is untouched anonymous
     * memory and already reads as zero. A dynamic one has had the loader and
     * glibc's startup run over that same memory first, so a global with no
     * initializer would otherwise begin life holding their leftovers.
     */
    a64_mov_imm(A64_IP0, global_frame);
    a64_sub(A64_SP, A64_SP, A64_IP0);
    a64_mov_sp(A64_GP);
    a64_mov_imm(A64_IP0, elf_data_start);
    a64_mem(A64_STORE, 8, A64_GP, A64_IP0, 0);
    if (dynlink) {
        /* Clear the frame inline rather than through libc's memset, which a
         * --no-libc translation unit never declares. The frame is a multiple of
         * sixteen bytes and may be empty: count x2 down to zero while x0 walks
         * up through it.
         */
        a64_mov(0, A64_GP);
        a64_mov_imm(2, global_frame);
        emit(a64_cbz_insn(true, 2, 4));
        emit(a64_mem_post_insn(A64_STORE, 8, A64_ZR, 0, 8));
        emit(a64_sub_imm_insn(true, 2, 2, 8));
        emit(a64_cbnz_insn(true, 2, -2));
    }
    a64_b(GLOBAL_FUNC->bbs->elf_offset);
    /* __syscall(number,arg1,...): AArch64 Linux wants x8,x0..x5. */
    if (!dynlink) {
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
    ph2_ir_visit_globals(emit_ph2_ir);
    if (has_main) {
        a64_mem(A64_LOAD, 8, 23, A64_SP, global_frame);
        a64_mem(A64_LOAD, 8, 24, A64_SP, global_frame + 8);
        if (dynlink) {
            a64_mem(A64_LOAD, 8, 25, A64_SP, global_frame + 16);
            a64_mov_imm(0, elf_code_start + main_offset);
            a64_mov(1, 23);
            a64_mov(2, 24);
            a64_mov(3, A64_ZR);
            a64_mov(4, A64_ZR);
            a64_mov(5, A64_ZR);
            a64_mov(6, 25);
            a64_bl_addr(dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE);
            emit(a64_brk_insn(0)); /* __libc_start_main does not return */
        } else {
            a64_mov(0, 23);
            a64_mov(1, 24);
            emit(a64_bl_insn((main_offset - elf_code->size) / 4));
            a64_mov_imm(8, 93);
            emit(a64_svc_insn(0));
        }
    }
    for (int i = 0; i < ph2_ir_idx; i++)
        emit_ph2_ir(PH2_IR_FLATTEN[i]);
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
