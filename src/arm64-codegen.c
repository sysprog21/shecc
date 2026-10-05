/*
 * AArch64 Linux code generator. The allocator's virtual registers map to
 * x0..x7, x20..x22; x16/x17 are reserved scratch and x19 is the global base.
 */
#include "arm64.c"
#include "defs.h"
#include "globals.c"

/* IP0, the linker's own scratch register, is free for a code generator to use
 * between instructions and never allocated to a value. x19 is the base of the
 * synthetic global frame, held for the life of the program.
 */
#define A64_GP 19

/* AAPCS64 keeps SP 16-byte aligned, and the prologue saves five registers in
 * three pairs. Every frame calculation derives from these two.
 */
#define A64_STACK_ALIGN 16
#define A64_SAVE_BYTES 48
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
    return r < 8 ? r : r + 12;
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
static void a64_load_constant(int d, const ph2_ir_t *p)
{
    unsigned long long value;

    if (p->size_bytes == 8)
        value = (unsigned int) p->src0 |
                (unsigned long long) (unsigned int) p->src1 << 32;
    else if (p->is_unsigned)
        value = (unsigned int) p->src0;
    else
        value = (unsigned long long) (long long) p->src0;
    emit_arm64_wide_const(d, value);
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

void cfg_flatten(void)
{
    func_t *f;
    ph2_ir_prepare(false);
    bool has_main = MAIN_BB != NULL;

    /* Entry sequence lengths, which the block offsets below start after.
     * Static: 12 instructions of setup, then the 9-instruction __syscall
     * helper, so 21 in all. Dynamic: 25, having no __syscall helper but saving
     * the original stack pointer for __libc_start_main's stack_end argument,
     * spilling argc/argv, and clearing the frame with an inline loop.
     */
    f = find_func("__syscall");
    if (f && f->bbs) {
        if (dynlink)
            f->bbs->elf_offset = 0;
        else
            f->bbs->elf_offset = 12 * 4;
    }
    if (dynlink)
        elf_offset = 25 * 4;
    else
        elf_offset = 21 * 4;
    GLOBAL_FUNC->bbs->elf_offset = elf_offset;
    ph2_ir_visit_globals(update_elf_offset);

    /* code_generate() emits the call to main only when there is one, so a
     * translation unit without main must not be charged for it either.
     */
    if (has_main) {
        int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);
        if (dynlink)
            elf_offset += (10 + a64_mem_count(8, global_frame) +
                           a64_mem_count(8, global_frame + 8)) *
                          4;
        else
            elf_offset += 6 * 4;
    }
    for (f = FUNC_LIST.head; f; f = f->next) {
        if (!f->bbs)
            continue;
        ph2_ir_t *d = add_ph2_ir(OP_define);
        d->src0 = f->stack_size;
        d->func_name = intern_string(f->return_def.var_name);

        /* Where the caller's stack arguments sit, seen from the callee's own
         * SP. This must round the frame exactly as the prologue does: rounding
         * to MIN_ALIGNMENT instead left it eight bytes short whenever
         * stack_size % 16 == 8, and every stack-passed argument was then read
         * one slot low.
         */
        int stack_top_ofs =
            ALIGN_UP(f->stack_size, A64_STACK_ALIGN) + A64_SAVE_BYTES;
        int prologue_bytes = arm64_count_ph2_ir_words(d) * 4;
        ph2_ir_flatten_function(f, stack_top_ofs, prologue_bytes, 0, false,
                                update_elf_offset);
    }
}

void emit_ph2_ir(ph2_ir_t *p)
{
    int d = a64_reg(p->dest), n = a64_reg(p->src0), m = a64_reg(p->src1);
    if (op_is_comparison(p->op)) {
        emit(a64_cmp_reg_insn(a64_cmp_wide(p), n, m));
        emit(a64_cset_insn(
            a64_cmp_wide(p), d,
            a64_cond(p->op, p->src0_is_unsigned || p->src1_is_unsigned)));
        return;
    }
    switch (p->op) {
    case OP_define: {
        bool reload_global_base = dynlink && !strcmp(p->func_name, "main");

        fatal_function_context = p->func_name;
        emit(a64_stp_pre_insn(A64_FP, A64_LR, A64_SP, -16));
        emit(a64_add_imm_insn(true, A64_FP, A64_SP, 0));
        emit(a64_stp_pre_insn(20, 21, A64_SP, -16));

        /* x19 holds the synthetic global-frame base, but AAPCS64 makes it
         * callee-saved and glibc calls into this code at main. Saving it
         * alongside x22 costs nothing: the slot was half empty anyway.
         */
        emit(a64_stp_pre_insn(22, A64_GP, A64_SP, -16));
        a64_mov_imm(A64_IP0, ALIGN_UP(p->src0, A64_STACK_ALIGN));
        a64_sub(A64_SP, A64_SP, A64_IP0);

        /* Reload x19 rather than trust what called us: glibc enters at main,
         * and AAPCS64 lets everything in between clobber a callee-saved
         * register it has saved. A64_LOAD is a64_mem()'s load selector, so this
         * reads the base back from the word parked at elf_data_start. The frame
         * itself is allocated once in code_generate(), which is also where the
         * question of clearing it is settled.
         */
        if (reload_global_base) {
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
        a64_mem(a64_load_access(p), p->src1, d, n, 0);
        return;
    case OP_write:
        a64_mem(A64_STORE, p->dest, m, n, 0);
        return;
    case OP_add:
        a64_add(d, n, m);
        return;
    case OP_sub:
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
        emit(a64_lslv_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_rshift:
        emit(a64_rshift_insn(p, d, n, m));
        return;
    case OP_bit_and:
        emit(a64_and_reg_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_bit_or:
        emit(a64_orr_reg_insn(p->size_bytes == 8, d, n, m));
        return;
    case OP_bit_xor:
        emit(a64_eor_reg_insn(p->size_bytes == 8, d, n, m));
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
    case OP_branch:
        a64_cbnz(p->src0_is_pointer || p->size_bytes == 8, n,
                 p->then_bb->elf_offset);
        a64_b(p->else_bb->elf_offset);
        return;
    case OP_jump:
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
        a64_mov_imm(A64_IP0, ALIGN_UP(p->src1, A64_STACK_ALIGN));
        a64_add(A64_SP, A64_SP, A64_IP0);
        emit(a64_ldp_post_insn(22, A64_GP, A64_SP, 16));
        emit(a64_ldp_post_insn(20, 21, A64_SP, 16));
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

    /* At Linux process entry argc is at [sp] and argv begins at sp + 8; they
     * are loaded into x20 and x21. Those two are allocatable by the global
     * initialiser, so the values are parked in x23/x24 -- callee-saved, and
     * outside this backend's allocation set -- to survive it.
     */
    if (dynlink)
        a64_mov_sp(25);
    a64_mem(A64_LOAD, 8, 20, A64_SP, 0);
    emit(a64_add_imm_insn(true, 21, A64_SP, 8));
    a64_mov(23, 20);
    a64_mov(24, 21);
    if (dynlink) {
        a64_mov_imm(A64_IP0, A64_STACK_ALIGN);
        a64_sub(A64_SP, A64_SP, A64_IP0);
        a64_mem(A64_STORE, 8, 23, A64_SP, 0);
        a64_mem(A64_STORE, 8, 24, A64_SP, 8);
    }

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
        if (dynlink) {
            a64_mem(A64_LOAD, 8, 23, A64_SP, global_frame);
            a64_mem(A64_LOAD, 8, 24, A64_SP, global_frame + 8);
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
