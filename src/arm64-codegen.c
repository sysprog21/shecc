/*
 * AArch64 Linux code generator. The allocator's virtual registers map to
 * x0..x7, x20..x22; x16/x17 are reserved scratch and x19 is the global base.
 */
#include "defs.h"
#include "globals.c"

#define A64_SP 31
#define A64_ZR 31

/* IP0, the linker's own scratch register: free for a code generator to use
 * between instructions, and never allocated to a value.
 */
#define A64_IP0 16
/* Base of the synthetic global frame, held for the life of the program. */
#define A64_GP 19

/* AAPCS64 keeps SP 16-byte aligned, and the prologue saves five registers in
 * three pairs. Every frame calculation derives from these two.
 */
#define A64_STACK_ALIGN 16
#define A64_SAVE_BYTES 48

int a64_reg(int r)
{
    return r < 8 ? r : r + 12;
}
void emit(int insn)
{
    elf_write_int(elf_code, insn);
}
void a64_mov(int d, int n)
{
    emit(0xaa0003e0 | (n << 16) | d);
}

/* SP is not a general register in logical instructions: ORR would read ZR. ADD
 * #0 is the architectural move spelling when either operand is SP.
 */
void a64_mov_sp(int d)
{
    emit(0x910003e0 | (A64_SP << 5) | d);
}
void a64_mov_imm(int d, int v)
{
    /* Integer constants participate in pointer arithmetic throughout the
     * compiler. A negative C int must therefore become a sign-extended
     * X-register value, not 0x00000000ffffffff-style zero extension. MOVN
     * supplies the upper one bits while MOVK fills the second halfword.
     */
    if (v < 0)
        emit(0x92800000 | (((~v) & 0xffff) << 5) | d);
    else
        emit(0xd2800000 | ((v & 0xffff) << 5) | d);
    emit(0xf2800000 | (((v >> 16) & 0xffff) << 5) | d | (1 << 21));
}
void a64_add(int d, int n, int m)
{
    int op = (d == A64_SP || n == A64_SP) ? 0x8b206000 : 0x8b000000;
    emit(op | (m << 16) | (n << 5) | d);
}
void a64_sub(int d, int n, int m)
{
    int op = (d == A64_SP || n == A64_SP) ? 0xcb206000 : 0xcb000000;
    emit(op | (m << 16) | (n << 5) | d);
}
void a64_sxtw(int d, int n)
{
    emit(0x93407c00 | (n << 5) | d);
}

/* Xd = Xn +/- sign_extend(Wm). The extended-register forms fold the widening of
 * an int index into the address arithmetic that consumes it, so pointer
 * arithmetic costs the same one instruction as any other add.
 */
void a64_add_sxtw(int d, int n, int m)
{
    emit(0x8b20c000 | (m << 16) | (n << 5) | d);
}
void a64_sub_sxtw(int d, int n, int m)
{
    emit(0xcb20c000 | (m << 16) | (n << 5) | d);
}

/* Sign-extend the low @size bytes of Xn into Xd. Always one instruction, so
 * update_elf_offset()'s default estimate stays correct.
 */
void a64_extend(int d, int n, int size)
{
    if (size == 1)
        emit(0x93401c00 | (n << 5) | d);
    else if (size == 2)
        emit(0x93403c00 | (n << 5) | d);
    else if (size == 4)
        a64_sxtw(d, n);
    else
        a64_mov(d, n);
}

/* AArch64's ordinary LDR/STR immediate is scaled by the access width. The
 * compiler deliberately supports packed C layouts, so structure members are
 * often not naturally aligned (func_t.bbs, for example). Such offsets must use
 * the byte-addressed LDUR/STUR form; rounding them down corrupts adjacent
 * fields during self-hosting.
 */
int a64_mem_count(int size, int ofs)
{
    if (ofs >= 0 && ofs <= 4095 * size && !(ofs % size))
        return 1;
    if (ofs >= -256 && ofs <= 255)
        return 1;
    return 4; /* MOVZ/MOVK + ADD + [base] access */
}

/* Base opcode of the unscaled load/store of @size bytes. Size lives in bits
 * 31:30 and the operation in 23:22, where a load picks the sign-extending form
 * for every width narrower than a doubleword -- values sit sign-extended in
 * their X register, so a narrow load must widen the same way whichever
 * addressing form carries it. The scaled form is this plus bit 24, which is why
 * one table serves both and they can no longer disagree.
 */
int a64_mem_op(int load, int size)
{
    int sf;
    if (size == 8)
        sf = 3;
    else if (size == 4)
        sf = 2;
    else if (size == 2)
        sf = 1;
    else if (size == 1)
        sf = 0;
    else {
        fatal("unsupported arm64 access width");
        return 0;
    }
    if (!load)
        return 0x38000000 | (sf << 30);
    return 0x38000000 | (sf << 30) | ((size == 8 ? 1 : 2) << 22);
}
void a64_mem(int load, int size, int rt, int rn, int ofs)
{
    int op = a64_mem_op(load, size);
    if (ofs >= -256 && ofs <= 255 && (ofs < 0 || ofs % size)) {
        emit(op | ((ofs & 0x1ff) << 12) | (rn << 5) | rt); /* LDUR/STUR */
        return;
    }
    if (ofs < 0 || ofs > 4095 * size || ofs % size) {
        a64_mov_imm(A64_IP0, ofs);
        a64_add(A64_IP0, rn, A64_IP0);
        rn = A64_IP0;
        ofs = 0;
    }
    emit(op | (1 << 24) | ((ofs / size) << 10) | (rn << 5) | rt);
}

/* Masking an out-of-range displacement silently branches somewhere else. The
 * generated image is small enough that these never fire today, so say so rather
 * than let a larger input fail as a wild jump.
 */
void a64_check_disp(int disp, int bits, char *what)
{
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
    emit((wide ? 0xb5000000 : 0x35000000) | ((disp & 0x7ffff) << 5) | rt);
}
void a64_b(int target)
{
    int d = (target - elf_code->size) / 4;
    a64_check_disp(d, 26, "arm64 branch out of range");
    emit(0x14000000 | (d & 0x3ffffff));
}
void a64_bl_addr(int target)
{
    int d = (target - (elf_code_start + elf_code->size)) / 4;
    a64_check_disp(d, 26, "arm64 call out of range");
    emit(0x94000000 | (d & 0x3ffffff));
}
int a64_adrp_insn(int d, int pc, int target)
{
    int pages = (target >> 12) - (pc >> 12);
    a64_check_disp(pages, 21, "arm64 ADRP target out of range");
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5) |
           d;
}

/* Which operand of an address expression is the int index that must widen: 0
 * for neither, 1 for src0, 2 for src1. Exactly one source being an address is
 * what makes the other an index, so the two source flags decide alone.
 */
int a64_ptr_index(ph2_ir_t *p)
{
    if (p->src0_is_pointer == p->src1_is_pointer)
        return 0;
    return p->src0_is_pointer ? 2 : 1;
}

/* A comparison is as wide as the values it compares. An address has to be
 * compared whole, an array included, since what reaches the instruction is its
 * decayed base. The source flags say so and is_pointer does not: it counts
 * pointer-like operands alone, for the sake of the 32-bit targets that read it.
 */
bool a64_cmp_wide(ph2_ir_t *p)
{
    return p->src0_is_pointer || p->src1_is_pointer;
}

int a64_cond(opcode_t op)
{
    switch (op) {
    case OP_eq:
        return 0;
    case OP_neq:
        return 1;
    case OP_geq:
        return 10;
    case OP_lt:
        return 11;
    case OP_gt:
        return 12;
    default:
        return 13;
    }
}

/* All estimates below exactly match emit_ph2_ir(). Fixed-width AArch64 makes
 * this deliberately simpler and more reliable than a relocation pass.
 */
void update_elf_offset(ph2_ir_t *ir)
{
    int n = 1;
    switch (ir->op) {
    case OP_allocat:
        n = 0;
        break;
    case OP_assign:
        n = ir->dest == ir->src0 ? 0 : 1;
        break;
    case OP_load_constant:
    case OP_load_data_address:
    case OP_load_rodata_address:
        n = 2;
        break;
    case OP_address_of:
    case OP_global_address_of:
        n = 3;
        break;
    case OP_load:
    case OP_global_load:
        n = a64_mem_count(ir->size_bytes, ir->src0);
        break;
    case OP_store:
    case OP_global_store:
        n = a64_mem_count(ir->size_bytes, ir->src1);
        break;
    case OP_address_of_func:
        n = 3;
        break;
    case OP_return:
        n = (ir->src0 >= 0 && a64_reg(ir->src0) != 0) ? 8 : 7;
        break;
    case OP_branch:
        n = 2;
        break;
    case OP_mod:
        n = 2;
        break;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
    case OP_log_not:
        n = 2;
        break;
    default:
        break;
    }
    elf_offset += n * 4;
}

void cfg_flatten(void)
{
    func_t *f;

    /* Entry sequence lengths, which the block offsets below start after.
     * Static: 12 instructions of setup, then the 9-instruction __syscall
     * helper, so 21 in all. Dynamic: 23, having no __syscall helper but saving
     * the original stack pointer for __libc_start_main's stack_end argument,
     * spilling argc/argv, and calling memset to clear the frame.
     */
    f = find_func("__syscall");
    if (f && f->bbs) {
        if (dynlink)
            f->bbs->elf_offset = 0;
        else
            f->bbs->elf_offset = 12 * 4;
    }
    if (dynlink)
        elf_offset = 23 * 4;
    else
        elf_offset = 21 * 4;
    GLOBAL_FUNC->bbs->elf_offset = elf_offset;
    for (ph2_ir_t *p = GLOBAL_FUNC->bbs->ph2_ir_list.head; p; p = p->next)
        update_elf_offset(p);

    /* code_generate() emits the call to main only when there is one, so a
     * translation unit without main must not be charged for it either.
     */
    if (MAIN_BB) {
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
        for (basic_block_t *b = f->bbs; b; b = b->rpo_next) {
            b->elf_offset = elf_offset;

            /* The entry block's offset deliberately names the prologue so calls
             * enter at a valid function entry. Later block labels must however
             * account for that prologue before their first IR word.
             */
            if (b == f->bbs) {
                elf_offset += 7 * 4;
                if (dynlink && !strcmp(f->return_def.var_name, "main"))
                    elf_offset += 3 * 4;
            }
            for (ph2_ir_t *p = b->ph2_ir_list.head; p; p = p->next) {
                if (p->ofs_based_on_stack_top) {
                    if (p->op == OP_load || p->op == OP_address_of)
                        p->src0 += stack_top_ofs;
                    else if (p->op == OP_store)
                        p->src1 += stack_top_ofs;
                }
                ph2_ir_t *q = add_existed_ph2_ir(p);
                if (q->op == OP_return)
                    q->src1 = f->stack_size;
                update_elf_offset(q);
            }
        }
    }
}

void emit_ph2_ir(ph2_ir_t *p)
{
    int d = a64_reg(p->dest), n = a64_reg(p->src0), m = a64_reg(p->src1);
    switch (p->op) {
    case OP_define: {
        bool reload_global_base = dynlink && !strcmp(p->func_name, "main");
        emit(0xa9bf7bfd); /* stp x29, x30, [sp, #-16]! */
        emit(0x910003fd); /* mov x29, sp                */
        emit(0xa9bf57f4); /* stp x20, x21, [sp, #-16]!  */
        /* x19 holds the synthetic global-frame base, but AAPCS64 makes it
         * callee-saved and glibc calls into this code at main. Saving it
         * alongside x22 costs nothing: the slot was half empty anyway.
         */
        emit(0xa9bf4ff6); /* stp x22, x19, [sp, #-16]!  */
        a64_mov_imm(A64_IP0, ALIGN_UP(p->src0, A64_STACK_ALIGN));
        a64_sub(A64_SP, A64_SP, A64_IP0);

        /* Reload x19 rather than trust what called us: glibc enters at main,
         * and AAPCS64 lets everything in between clobber a callee-saved
         * register it has saved. The leading 1 is a64_mem()'s load selector, so
         * this reads the base back from the word parked at elf_data_start. The
         * frame itself is allocated once in code_generate(), which is also
         * where the question of clearing it is settled.
         */
        if (reload_global_base) {
            a64_mov_imm(A64_IP0, elf_data_start);
            a64_mem(1, 8, A64_GP, A64_IP0, 0);
        }
        return;
    }
    case OP_load_constant:
        a64_mov_imm(d, p->src0);
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
        a64_mem(1, p->size_bytes, d, A64_SP, p->src0);
        return;
    case OP_global_load:
        a64_mem(1, p->size_bytes, d, A64_GP, p->src0);
        return;
    case OP_store:
        a64_mem(0, p->size_bytes, n, A64_SP, p->src1);
        return;
    case OP_global_store:
        a64_mem(0, p->size_bytes, n, A64_GP, p->src1);
        return;
    case OP_read:
        a64_mem(1, p->src1, d, n, 0);
        return;
    case OP_write:
        a64_mem(0, p->dest, m, n, 0);
        return;
    case OP_add:
        /* Index expressions are int-valued, so the index has to widen before it
         * joins a 64-bit address; otherwise -1 becomes +4294967295 on LP64.
         * Addition is commutative, so either operand may be the index.
         *
         * The fall-through stays the X form for both cases a64_ptr_index()
         * reports as 0: two addresses, which is pointer minus pointer and
         * genuinely 64-bit, and two ints, whose upper half no reader looks at.
         * See the width note at the comparisons below.
         */
        if (a64_ptr_index(p) == 2)
            a64_add_sxtw(d, n, m);
        else if (a64_ptr_index(p) == 1)
            a64_add_sxtw(d, m, n);
        else
            a64_add(d, n, m);
        return;
    case OP_sub:
        /* Only pointer-minus-int widens: int-minus-pointer is not an address
         * expression, and pointer-minus-pointer is already 64-bit on both
         * sides.
         */
        if (a64_ptr_index(p) == 2)
            a64_sub_sxtw(d, n, m);
        else
            a64_sub(d, n, m);
        return;
    case OP_mul:
        emit(0x1b007c00 | (m << 16) | (n << 5) | d);
        return;
    case OP_div:
        emit(0x1ac00c00 | (m << 16) | (n << 5) | d);
        return;
    case OP_mod:
        /* d = n % m. Do not put the quotient in d: register coalescing may make
         * d alias n, losing the minuend before MSUB reads it.
         */
        emit(0x1ac00c00 | (m << 16) | (n << 5) | A64_IP0);
        emit(0x1b008000 | (m << 16) | (n << 10) | (A64_IP0 << 5) | d);
        return;
    case OP_lshift:
        emit(0x1ac02000 | (m << 16) | (n << 5) | d);
        return;
    case OP_rshift:
        emit(0x1ac02800 | (m << 16) | (n << 5) | d);
        return;
    case OP_bit_and:
        emit(0x0a000000 | (m << 16) | (n << 5) | d);
        return;
    case OP_bit_or:
        emit(0x2a000000 | (m << 16) | (n << 5) | d);
        return;
    case OP_bit_xor:
        emit(0x4a000000 | (m << 16) | (n << 5) | d);
        return;
    case OP_negate:
        emit(0x4b0003e0 | (n << 16) | d);
        return;
    case OP_bit_not:
        emit(0x2a2003e0 | (n << 16) | d);
        return;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        emit((a64_cmp_wide(p) ? 0xeb00001f : 0x6b00001f) | (m << 16) |
             (n << 5));
        emit((a64_cmp_wide(p) ? 0x9a9f07e0 : 0x1a9f07e0) |
             ((a64_cond(p->op) ^ 1) << 12) | d);
        return;

    /* Width follows the operand, exactly as the comparisons above do. A pointer
     * must be tested whole, or one whose low word happens to be zero reads as
     * null. An int must not be: multiply, divide, shift and the bitwise
     * operations all use the W forms, which leave the upper half zeroed rather
     * than sign-extended, so only the low word is the value.
     */
    case OP_log_not:
        emit((p->src0_is_pointer ? 0xf100001f : 0x7100001f) | (n << 5));
        emit(0x1a9f07e0 | (1 << 12) | d);
        return;

    /* OP_trunc's src1 is the target width; OP_sign_ext's packs the source width
     * in its upper half (see promote_unchecked()). Decoding both the same way
     * made every promotion a plain move.
     */
    case OP_trunc:
        a64_extend(d, n, p->src1);
        return;
    case OP_sign_ext: {
        int src_size = (p->src1 >> 16) & 0xffff, dst_size = p->src1 & 0xffff;

        /* Widening to a pointer: the register already holds a full address, so
         * extending it from 32 bits would discard the upper half.
         */
        if (dst_size == PTR_SIZE)
            a64_mov(d, n);
        else
            a64_extend(d, n, src_size);
        return;
    }
    case OP_cast:
        a64_mov(d, n);
        return;
    case OP_branch:
        a64_cbnz(p->src0_is_pointer, n, p->then_bb->elf_offset);
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
            emit(0x94000000 |
                 (((f->bbs->elf_offset - elf_code->size) / 4) & 0x3ffffff));
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
        a64_mem(0, 8, A64_IP0, n, 0);
        return;
    }
    case OP_load_func:
        a64_mov(A64_IP0, n);
        return;
    case OP_indirect:
        emit(0xd63f0200);
        return;
    case OP_return:
        if (p->src0 >= 0 && n != 0)
            a64_mov(0, n);
        a64_mov_imm(A64_IP0, ALIGN_UP(p->src1, A64_STACK_ALIGN));
        a64_add(A64_SP, A64_SP, A64_IP0);
        emit(0xa8c14ff6); /* ldp x22, x19, [sp], #16 */
        emit(0xa8c157f4); /* ldp x20, x21, [sp], #16 */
        emit(0xa8c17bfd); /* ldp x29, x30, [sp], #16 */
        emit(0xd65f03c0); /* ret                     */
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
                      0x91000210 | ((got & 0xfff) << 10)); /* add x16, x16, # */
        elf_write_int(dynamic_sections.elf_plt,
                      0xf9400211); /* ldr x17, [x16] */
        elf_write_int(dynamic_sections.elf_plt, 0xd61f0220); /* br x17 */
    }
}

void code_generate(void)
{
    int global_frame = ALIGN_UP(GLOBAL_FUNC->stack_size, A64_STACK_ALIGN);

    /* At Linux process entry argc is at [sp] and argv begins at sp + 8; they
     * are loaded into x20 and x21. Those two are allocatable by the global
     * initialiser, so the values are parked in x23/x24 -- callee-saved, and
     * outside this backend's allocation set -- to survive it.
     */
    if (dynlink)
        a64_mov_sp(25);
    a64_mem(1, 8, 20, A64_SP, 0);
    emit(0x910023f5);
    a64_mov(23, 20);
    a64_mov(24, 21);
    if (dynlink) {
        a64_mov_imm(A64_IP0, A64_STACK_ALIGN);
        a64_sub(A64_SP, A64_SP, A64_IP0);
        a64_mem(0, 8, 23, A64_SP, 0);
        a64_mem(0, 8, 24, A64_SP, 8);
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
    a64_mem(0, 8, A64_GP, A64_IP0, 0);
    if (dynlink) {
        func_t *memset_func = find_func("memset");
        if (!memset_func)
            fatal("arm64 dynamic startup needs memset");
        a64_mov(0, A64_GP);
        a64_mov(1, A64_ZR);
        a64_mov_imm(2, global_frame);
        a64_bl_addr(dynamic_sections.elf_plt_start + memset_func->plt_offset);
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
        emit(0xd4000001);
        emit(0xd65f03c0);
    }
    for (ph2_ir_t *p = GLOBAL_FUNC->bbs->ph2_ir_list.head; p; p = p->next)
        emit_ph2_ir(p);
    if (MAIN_BB) {
        if (dynlink) {
            a64_mem(1, 8, 23, A64_SP, global_frame);
            a64_mem(1, 8, 24, A64_SP, global_frame + 8);
            a64_mov_imm(0, elf_code_start + MAIN_BB->elf_offset);
            a64_mov(1, 23);
            a64_mov(2, 24);
            a64_mov(3, A64_ZR);
            a64_mov(4, A64_ZR);
            a64_mov(5, A64_ZR);
            a64_mov(6, 25);
            a64_bl_addr(dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE);
            emit(0xd4200000); /* __libc_start_main does not return */
        } else {
            a64_mov(0, 23);
            a64_mov(1, 24);
            emit(0x94000000 |
                 (((MAIN_BB->elf_offset - elf_code->size) / 4) & 0x3ffffff));
            a64_mov_imm(8, 93);
            emit(0xd4000001);
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
