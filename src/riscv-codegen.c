/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */
#include "defs.h"
#include "globals.c"
#include "riscv.c"

#define RV32_ALIGNMENT 16

static bool rv_count_only;
static int rv_counted_words;

void emit_ph2_ir(ph2_ir_t *ph2_ir);

/* a0-a7 hold virtual registers 0-7; s3-s5 hold the remaining allocatable
 * registers and are saved when used. s2 stages indirect callees, while s0/s1
 * belong to startup argument handling. The backend does not use other saved
 * registers; ra and sp are handled by the function prologue and epilogue.
 */

/* The machine register for IR register @ir_reg. The first eight are the
 * argument registers a0-a7; the rest are s3-s5, which a callee preserves, so a
 * value pinned there survives a call. s0 and s1 belong to the entry code and s2
 * holds an indirect call's target.
 */
int rv_reg_of(int ir_reg)
{
    return ir_reg < 8 ? ir_reg + __a0 : __s3 + ir_reg - 8;
}

/* The callee-saved registers @func writes, and so must save on entry and
 * restore on return: how many of s3-s5, which are handed out in order so the
 * highest one used decides, and in bit 8 whether it stages an indirect call's
 * target in s2.
 */
int rv_saved_regs(func_t *func)
{
    int top = func_highest_used_reg(func, 8);
    int s2 = 0;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
            if (ir->dest_hi > top)
                top = ir->dest_hi;
            if (ir->src0_hi > top)
                top = ir->src0_hi;
            if (ir->src1_hi > top)
                top = ir->src1_hi;
            if (ir->op == OP_load_func)
                s2 = 1 << 8;
        }
    }
    if (top >= REG_CNT)
        top = REG_CNT - 1;
    return (top - 7) | s2;
}

/* How many words rv_saved_regs()'s @saved occupies in the frame. */
int rv_saved_words(int saved)
{
    return (saved & 0xff) + (saved >> 8);
}

/* The frame a function allocates: its locals, ra, and the saved registers, kept
 * 16-byte aligned.
 */
int rv_frame_bytes(int stack_size, int saved)
{
    return ALIGN_UP(stack_size + 4 + rv_saved_words(saved) * 4, RV32_ALIGNMENT);
}

/* The register saved in frame word @i: s3 onwards, then s2. */
int rv_saved_reg(int saved, int i)
{
    return i < (saved & 0xff) ? __s3 + i : __s2;
}

static int rv_count_ph2_ir_words(ph2_ir_t *ph2_ir)
{
    bool saved_count_only = rv_count_only;
    int saved_counted_words = rv_counted_words;
    char *saved_fatal_function_context = fatal_function_context;

    rv_count_only = true;
    rv_counted_words = 0;
    emit_ph2_ir(ph2_ir);
    int words = rv_counted_words;

    rv_count_only = saved_count_only;
    rv_counted_words = saved_counted_words;
    fatal_function_context = saved_fatal_function_context;
    return words;
}

void update_elf_offset(ph2_ir_t *ph2_ir)
{
    if (ph2_ir->op != OP_allocat)
        elf_offset += rv_count_ph2_ir_words(ph2_ir) * 4;
}

static ph2_frame_layout_t rv_frame_layout(func_t *func, ph2_ir_t *define)
{
    int saved = rv_saved_regs(func);

    define->src2 = saved;
    return (ph2_frame_layout_t) {rv_frame_bytes(func->stack_size, saved),
                                 rv_count_ph2_ir_words(define) * 4, saved,
                                 true};
}

void cfg_flatten(void)
{
    func_t *func;
    ph2_ir_prepare(false);

    if (dynlink) {
        /* When using dynamic linking, 26 instructions are generated at the
         * program entry point to perform the following operations:
         * - prepare arguments and call __libc_start_main()
         * - preserve a0 ('argc'), a1 ('argv') and sp.
         * - allocate and clear a global stack, then jump to global init
         *   function.
         */
        elf_offset = 104;
    } else {
        /* Under static linking, "__syscall" must be generated to allow the
         * program to invoke system calls.
         *
         * "__syscall" consists of 9 instructions, preceded by 7 initial
         * instructions because the global-init call is an AUIPC/JALR pair.
         * Consequently, "__syscall" starts at byte 28 and the subsequent
         * function (GLOBAL_FUNC) starts at byte 64.
         */
        func = find_func("__syscall");
        func->bbs->elf_offset = 28;
        elf_offset = 64;
    }

    GLOBAL_FUNC->bbs->elf_offset = elf_offset;

    ph2_ir_visit_globals(update_elf_offset);

    /* Prepare argc/argv and reach main through a range-independent call.
     * code_generate() emits this only when there is a main, so a translation
     * unit without one must not be charged for it either: every function after
     * would be placed 28 bytes from where it is emitted.
     */
    if (MAIN_BB)
        elf_offset += dynlink ? 48 : 28;

    ph2_ir_flatten_functions(rv_frame_layout, update_elf_offset);
}

void emit(int code)
{
    if (rv_count_only) {
        if (rv_counted_words < INT_MAX)
            rv_counted_words++;
    } else
        elf_write_int(elf_code, code);
}

static void emit_rv32_split_address(bool pc_relative,
                                    rv_reg rd,
                                    rv_reg base,
                                    int offset)
{
    int hi = rv_hi(offset), lo = rv_lo(offset);

    emit(pc_relative ? __auipc(base, hi) : __lui(base, hi));
    emit(pc_relative ? __jalr(rd, base, lo) : __addi(rd, base, lo));
}

static rv_reg rv_memory_base(rv_reg base, int offset, int pair, int *disp)
{
    *disp = offset;
    if (offset >= -2048 && offset <= 2047 - pair * 4)
        return base;
    emit_rv32_split_address(false, __t0, __t0, offset);
    emit(__add(__t0, base, __t0));
    *disp = 0;
    return __t0;
}

static void rv_load(rv_reg dest,
                    rv_reg base,
                    int offset,
                    int size,
                    bool is_unsigned)
{
    if (size == 1)
        emit(is_unsigned ? __lbu(dest, base, offset)
                         : __lb(dest, base, offset));
    else if (size == 2)
        emit(is_unsigned ? __lhu(dest, base, offset)
                         : __lh(dest, base, offset));
    else
        emit(__lw(dest, base, offset));
}

static void rv_store(rv_reg src, rv_reg base, int offset, int size)
{
    if (size == 1)
        emit(__sb(src, base, offset));
    else if (size == 2)
        emit(__sh(src, base, offset));
    else
        emit(__sw(src, base, offset));
}

static int riscv_bitwise_insn(opcode_t op,
                              rv_reg dest,
                              rv_reg left,
                              rv_reg right)
{
    switch (op) {
    case OP_bit_and:
        return __and(dest, left, right);
    case OP_bit_or:
        return __or(dest, left, right);
    case OP_bit_xor:
        return __xor(dest, left, right);
    default:
        fatal("unsupported RISC-V bitwise operation");
    }
    return 0;
}

void emit_ph2_ir(ph2_ir_t *ph2_ir)
{
    func_t *func;
    int rd = rv_reg_of(ph2_ir->dest);
    int rs1 = rv_reg_of(ph2_ir->src0);
    int rs2 = rv_reg_of(ph2_ir->src1);
    int rd_hi = rv_reg_of(ph2_ir->dest_hi);
    int rs1_hi = rv_reg_of(ph2_ir->src0_hi);
    int rs2_hi = rv_reg_of(ph2_ir->src1_hi);
    int ofs;

    /* Prepare the variables to reuse the same code for the instruction sequence
     * of
     * 1. division and modulo.
     * 2. load and store operations.
     * 3. address-of operations.
     */
    rv_reg interm, divisor_mask = __t1;

    switch (ph2_ir->op) {
    case OP_define:
        fatal_function_context = ph2_ir->func_name;
        ofs = rv_frame_bytes(ph2_ir->src0, ph2_ir->src2);

        /* Lower sp before saving below the frame's top: there is no red zone,
         * and a signal delivered in between would overwrite anything stored
         * below sp. t1 holds the old sp to address the saves from.
         */
        emit_rv32_split_address(false, __t0, __t0, ofs);
        emit(__addi(__t1, __sp, 0));
        emit(__sub(__sp, __sp, __t0));
        emit(__sw(__ra, __t1, -4));
        for (int i = 0; i < rv_saved_words(ph2_ir->src2); i++)
            emit(__sw(rv_saved_reg(ph2_ir->src2, i), __t1, -8 - i * 4));
        return;
    case OP_load_constant:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit_rv32_split_address(false, rd, rd, ph2_ir->src0);

        } else
            emit(__addi(rd, __zero, ph2_ir->src0));
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047) {
                emit_rv32_split_address(false, rd_hi, rd_hi, ph2_ir->src1);
            } else
                emit(__addi(rd_hi, __zero, ph2_ir->src1));
        }
        return;
    case OP_address_of:
    case OP_global_address_of:
        interm = ph2_ir->op == OP_address_of ? __sp : __gp;
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit_rv32_split_address(false, __t0, __t0, ph2_ir->src0);
            emit(__add(rd, interm, __t0));
        } else
            emit(__addi(rd, interm, ph2_ir->src0));
        return;
    case OP_assign:
        if (PH2_PAIR_DEST_SRC0(ph2_ir) && rd == rs1_hi) {
            /* A pair moved one register over, as a slot round trip collapsed
             * into a move can leave it: the low destination is the high source,
             * so move the high word first, through t0 when the two registers
             * trade places.
             */
            if (rd_hi == rs1) {
                emit(__addi(__t0, rs1, 0));
                emit(__addi(rd, rs1_hi, 0));
                emit(__addi(rd_hi, __t0, 0));
            } else {
                emit(__addi(rd_hi, rs1_hi, 0));
                emit(__addi(rd, rs1, 0));
            }
            return;
        }
        emit(__addi(rd, rs1, 0));
        if (PH2_PAIR_DEST_SRC0(ph2_ir) && ph2_ir->dest_hi != ph2_ir->src0_hi)
            emit(__addi(rd_hi, rs1_hi, 0));
        return;
    case OP_load:
    case OP_global_load:
        interm = ph2_ir->op == OP_load ? __sp : __gp;
        if (ph2_ir->dest_hi >= 0) {
            int disp;
            rv_reg base = rv_memory_base(interm, ph2_ir->src0, 1, &disp);

            rv_load(rd, base, disp, 4, true);
            rv_load(rd_hi, base, disp + 4, 4, true);
            return;
        }
        {
            int disp;
            rv_reg base = rv_memory_base(interm, ph2_ir->src0, 0, &disp);

            rv_load(rd, base, disp, ph2_ir->size_bytes, ph2_ir->is_unsigned);
        }
        return;
    case OP_store:
    case OP_global_store:
        interm = ph2_ir->op == OP_store ? __sp : __gp;
        if (ph2_ir->src0_hi >= 0) {
            int disp;
            rv_reg base = rv_memory_base(interm, ph2_ir->src1, 1, &disp);

            rv_store(rs1, base, disp, 4);
            rv_store(rs1_hi, base, disp + 4, 4);
            return;
        }
        {
            int disp;
            rv_reg base = rv_memory_base(interm, ph2_ir->src1, 0, &disp);

            rv_store(rs1, base, disp, ph2_ir->size_bytes);
        }
        return;
    case OP_read:
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src1 != 8)
                fatal("unsupported RISC-V pair load width");
#define RV_WORD_LOAD(dst, base, offset) emit(__lw(dst, base, offset))
            EMIT_PAIR_LOAD(RV_WORD_LOAD, rd, rd_hi, rs1);
#undef RV_WORD_LOAD
            return;
        }
        if (ph2_ir->src1 != 1 && ph2_ir->src1 != 2 && ph2_ir->src1 != 4)
            fatal("unsupported RISC-V load width");
        rv_load(rd, rs1, 0, ph2_ir->src1, ph2_ir->is_unsigned);
        return;
    case OP_write:
        if (ph2_ir->src1_hi >= 0) {
            if (ph2_ir->dest != 8)
                fatal("unsupported RISC-V pair store width");
            emit(__sw(rs2, rs1, 0));
            emit(__sw(rs2_hi, rs1, 4));
            return;
        }
        if (ph2_ir->dest != 1 && ph2_ir->dest != 2 && ph2_ir->dest != 4)
            fatal("unsupported RISC-V store width");
        rv_store(rs2, rs1, 0, ph2_ir->dest);
        return;
    case OP_branch:
        ofs = elf_code_start + ph2_ir->then_bb->elf_offset;
        emit_rv32_split_address(false, __t0, __t0, ofs);

        /* A pair is true when either word is nonzero. */
        if (ph2_ir->src0_hi >= 0) {
            emit(__or(__t1, rs1, rs1_hi));
            rs1 = __t1;
        }
        emit(__beq(rs1, __zero, 8));
        emit(__jalr(__zero, __t0, 0));
        ofs = elf_code_start + ph2_ir->else_bb->elf_offset;
        emit_rv32_split_address(false, __t0, __t0, ofs);
        emit(__jalr(__zero, __t0, 0));
        return;
    case OP_jump:
        ofs = ph2_ir->next_bb->elf_offset - elf_code->size;
        emit_rv32_split_address(true, __zero, __t0, ofs);
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        if (func->bbs) {
            ofs = func->bbs->elf_offset - elf_code->size;
            emit_rv32_split_address(true, __ra, __ra, ofs);
            return;
        } else if (dynlink) {
            /* The PLT follows the code and read-only data, so a large image
             * puts it beyond the 1 MiB a JAL reaches. Call it PC-relative
             * through AUIPC and JALR, which reach anywhere.
             */
            ofs = (dynamic_sections.elf_plt_start + func->plt_offset) -
                  (elf_code_start + elf_code->size);
            emit_rv32_split_address(true, __ra, __ra, ofs);
            return;
        } else {
            printf("The '%s' function is not implemented\n", ph2_ir->func_name);
            fflush(stdout); /* see fatal() */
            abort();
        }
    case OP_load_data_address:
        emit_rv32_split_address(false, rd, rd, elf_data_start + ph2_ir->src0);
        return;
    case OP_load_rodata_address:
        emit_rv32_split_address(false, rd, rd, elf_rodata_start + ph2_ir->src0);
        return;
    case OP_address_of_func:
        ofs = function_entry_address(ph2_ir->func_name);
        emit_rv32_split_address(false, __t0, __t0, ofs);
        emit(__sw(__t0, rs1, 0));
        return;
    case OP_load_func:
        emit(__addi(__s2, rs1, 0));
        return;
    case OP_indirect:
        emit(__jalr(__ra, __s2, 0));
        return;
    case OP_return:
        if (ph2_ir->src0 == -1)
            emit(__addi(__zero, __zero, 0));
        else
            emit(__addi(__a0, rs1, 0));
        if (ph2_ir->src0_hi >= 0)
            emit(__addi(__a1, rs1_hi, 0));
        ofs = rv_frame_bytes(ph2_ir->src1, ph2_ir->src2);
        /* Reload the saved registers while sp still covers them. */
        emit_rv32_split_address(false, __t0, __t0, ofs);
        emit(__add(__t1, __sp, __t0));
        emit(__lw(__ra, __t1, -4));
        for (int i = 0; i < rv_saved_words(ph2_ir->src2); i++)
            emit(__lw(rv_saved_reg(ph2_ir->src2, i), __t1, -8 - i * 4));
        emit(__addi(__sp, __t1, 0));
        emit(__jalr(__zero, __ra, 0));
        return;
    case OP_add:
        if (PH2_PAIR_BINARY(ph2_ir)) {
            /* Preserve the original low words until carry is known, including
             * an in-place destination and x + x using the same register pair.
             */
            emit(__add(__t0, rs1, rs2));
            emit(__sltu(__t1, __t0, rs1));
            emit(__add(rd_hi, rs1_hi, rs2_hi));
            emit(__add(rd_hi, rd_hi, __t1));
            emit(__addi(rd, __t0, 0));
        } else
            emit(__add(rd, rs1, rs2));
        return;
    case OP_sub:
        if (PH2_PAIR_BINARY(ph2_ir))
            emit(__sltu(__t0, rs1, rs2));
        emit(__sub(rd, rs1, rs2));
        if (PH2_PAIR_BINARY(ph2_ir)) {
            emit(__sub(rd_hi, rs1_hi, rs2_hi));
            emit(__sub(rd_hi, rd_hi, __t0));
        }
        return;
    case OP_mul:
        if (hard_mul_div) {
            if (PH2_PAIR_BINARY(ph2_ir)) {
                emit(__mul(__t0, rs1, rs2_hi));
                emit(__mul(__t1, rs1_hi, rs2));
                emit(__mulhu(__t2, rs1, rs2));
                emit(__mul(rd, rs1, rs2));
                emit(__add(rd_hi, __t2, __t0));
                emit(__add(rd_hi, rd_hi, __t1));
            } else
                emit(__mul(rd, rs1, rs2));
        } else {
            if (PH2_PAIR_BINARY(ph2_ir)) {
                /* Multiply two 64-bit pairs modulo 2^64 without the M
                 * extension. Keep the product in the destination pair, so t0-t3
                 * can hold the shifting multiplicand and multiplier; the loop
                 * ends once both multiplier words are zero.
                 */
                emit(__addi(__t0, rs1, 0));
                emit(__addi(__t1, rs1_hi, 0));
                emit(__addi(__t2, rs2, 0));
                emit(__addi(__t3, rs2_hi, 0));
                emit(__addi(rd, __zero, 0));
                emit(__addi(rd_hi, __zero, 0));
                emit(__andi(__t4, __t2, 1));
                emit(__sub(__t4, __zero, __t4));
                emit(__and(__t5, __t0, __t4));
                emit(__and(__t4, __t1, __t4));
                emit(__add(__t5, rd, __t5));
                emit(__sltu(__t6, __t5, rd));
                emit(__add(rd_hi, rd_hi, __t4));
                emit(__add(rd_hi, rd_hi, __t6));
                emit(__addi(rd, __t5, 0));
                emit(__srli(__t6, __t0, 31));
                emit(__slli(__t0, __t0, 1));
                emit(__slli(__t1, __t1, 1));
                emit(__or(__t1, __t1, __t6));
                emit(__slli(__t6, __t3, 31));
                emit(__srli(__t2, __t2, 1));
                emit(__or(__t2, __t2, __t6));
                emit(__srli(__t3, __t3, 1));
                emit(__or(__t6, __t2, __t3));
                emit(__bne(__t6, __zero, -72));
                return;
            }
            emit(__addi(__t0, __zero, 0));
            emit(__addi(__t1, __zero, 0));
            emit(__addi(__t3, rs1, 0));
            emit(__addi(__t4, rs2, 0));
            emit(__beq(__t3, __zero, 32));
            emit(__beq(__t4, __zero, 28));
            emit(__andi(__t1, __t4, 1));
            emit(__beq(__t1, __zero, 8));
            emit(__add(__t0, __t0, __t3));
            emit(__slli(__t3, __t3, 1));
            emit(__srli(__t4, __t4, 1));
            emit(__jal(__zero, -28));
            emit(__addi(rd, __t0, 0));
        }
        return;
    case OP_div:
    case OP_mod:
        if (PH2_PAIR_BINARY(ph2_ir)) {
            bool is_unsigned =
                ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned;

            /* 64-bit restoring division. t0:t1 is the remainder, t2:t3 is the
             * shifting dividend, t4:t5 is the divisor, and rd:rd_hi accumulates
             * the quotient. Two stack words hold the incoming dividend bit and
             * the iteration counter while t6 carries the remainder/borrow; this
             * avoids borrowing operand registers, because an SSA destination
             * may legally coalesce with a dying operand pair. Signed inputs
             * reserve two additional words for their sign masks and are
             * converted to magnitudes before entering the same loop.
             */
            emit(__addi(__sp, __sp, is_unsigned ? -8 : -16));
            emit(__addi(__t0, __zero, 0));
            emit(__addi(__t1, __zero, 0));
            if (is_unsigned) {
                emit(__addi(__t2, rs1, 0));
                emit(__addi(__t3, rs1_hi, 0));
                emit(__addi(__t4, rs2, 0));
                emit(__addi(__t5, rs2_hi, 0));
            } else {
                /* x ^ sign + -sign is abs(x), including INT64_MIN's
                 * representable unsigned magnitude.
                 */
                emit(__srai(__t6, rs1_hi, 31));
                emit(__sw(__t6, __sp, 8));
                emit(__xor(__t2, rs1, __t6));
                emit(__xor(__t3, rs1_hi, __t6));
                emit(__sub(__t6, __zero, __t6));
                emit(__add(__t2, __t2, __t6));
                emit(__sltu(__t6, __t2, __t6));
                emit(__add(__t3, __t3, __t6));
                emit(__srai(__t6, rs2_hi, 31));
                emit(__sw(__t6, __sp, 12));
                emit(__xor(__t4, rs2, __t6));
                emit(__xor(__t5, rs2_hi, __t6));
                emit(__sub(__t6, __zero, __t6));
                emit(__add(__t4, __t4, __t6));
                emit(__sltu(__t6, __t4, __t6));
                emit(__add(__t5, __t5, __t6));
            }
            emit(__addi(rd, __zero, 0));
            emit(__addi(rd_hi, __zero, 0));
            emit(__addi(__t6, __zero, 64));
            emit(__sw(__t6, __sp, 4));

            /* Bring down one dividend bit, shift the quotient, then subtract if
             * the two-word remainder is at least the divisor.
             */
            emit(__srli(__t6, __t3, 31));
            emit(__sw(__t6, __sp, 0));
            emit(__srli(__t6, __t0, 31));
            emit(__slli(__t0, __t0, 1));
            emit(__slli(__t1, __t1, 1));
            emit(__or(__t1, __t1, __t6));
            emit(__lw(__t6, __sp, 0));
            emit(__or(__t0, __t0, __t6));
            emit(__srli(__t6, __t2, 31));
            emit(__slli(__t2, __t2, 1));
            emit(__slli(__t3, __t3, 1));
            emit(__or(__t3, __t3, __t6));
            emit(__srli(__t6, rd, 31));
            emit(__slli(rd, rd, 1));
            emit(__slli(rd_hi, rd_hi, 1));
            emit(__or(rd_hi, rd_hi, __t6));
            emit(__bltu(__t1, __t5, 32));
            emit(__bltu(__t5, __t1, 8));
            emit(__bltu(__t0, __t4, 24));
            emit(__sltu(__t6, __t0, __t4));
            emit(__sub(__t0, __t0, __t4));
            emit(__sub(__t1, __t1, __t5));
            emit(__sub(__t1, __t1, __t6));
            emit(__addi(rd, rd, 1));
            emit(__lw(__t6, __sp, 4));
            emit(__addi(__t6, __t6, -1));
            emit(__sw(__t6, __sp, 4));
            emit(__bne(__t6, __zero, -108));

            if (ph2_ir->op == OP_mod) {
                emit(__addi(rd, __t0, 0));
                emit(__addi(rd_hi, __t1, 0));
            }
            if (!is_unsigned) {
                /* A quotient is negative for unlike operand signs; a remainder
                 * follows the dividend's sign.
                 */
                emit(__lw(__t6, __sp, 8));
                if (ph2_ir->op == OP_div) {
                    emit(__lw(__t0, __sp, 12));
                    emit(__xor(__t6, __t6, __t0));
                }
                emit(__xor(rd, rd, __t6));
                emit(__xor(rd_hi, rd_hi, __t6));
                emit(__sub(__t6, __zero, __t6));
                emit(__add(rd, rd, __t6));
                emit(__sltu(__t6, rd, __t6));
                emit(__add(rd_hi, rd_hi, __t6));
            }
            emit(__addi(__sp, __sp, is_unsigned ? 8 : 16));
            return;
        }
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                emit(ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned
                         ? __divu(rd, rs1, rs2)
                         : __div(rd, rs1, rs2));
            else
                emit(ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned
                         ? __modu(rd, rs1, rs2)
                         : __mod(rd, rs1, rs2));
            return;
        }
        interm = __t0;
        /* div/mod emulation */
        if (ph2_ir->op == OP_mod) {
            /* If the requested operation is modulo, the result will be stored
             * in __t2. The sign of the divisor is irrelevant for determining
             * the result's sign.
             */
            interm = __t2;
            divisor_mask = __zero;
        }

        /* Obtain absolute values of the dividend and divisor. Unsigned values
         * already are magnitudes; keep the same instruction count as the signed
         * path because the fixed branch displacements below depend on it.
         */
        emit(__addi(__t2, rs1, 0));
        emit(__addi(__t3, rs2, 0));
        if (ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned) {
            emit(__addi(__t0, __zero, 0));
            emit(__addi(__t2, __t2, 0));
            emit(__addi(__t2, __t2, 0));
            emit(__addi(__t1, __zero, 0));
            emit(__addi(__t3, __t3, 0));
            emit(__addi(__t3, __t3, 0));
            emit(__addi(__t5, __zero, 0));
        } else {
            emit(__srai(__t0, __t2, 31));
            emit(__add(__t2, __t2, __t0));
            emit(__xor(__t2, __t2, __t0));
            emit(__srai(__t1, __t3, 31));
            emit(__add(__t3, __t3, __t1));
            emit(__xor(__t3, __t3, __t1));
            emit(__xor(__t5, __t0, divisor_mask));
        }
        /* Unsigned integer division */
        emit(__addi(__t0, __zero, 0));
        emit(__addi(__t1, __zero, 1));
        emit(__beq(__t3, __zero, 60));
        emit(__beq(__t2, __zero, 56));
        emit(__beq(__t2, __t3, 28));
        emit(__bltu(__t2, __t3, 24));

        /* Stop scaling before the divisor's high bit would wrap to zero.
         * Without this guard a high-bit unsigned dividend can loop forever
         * after the next left shift turns both the divisor and quotient bit
         * marker into zero.
         */
        emit(__slt(__t4, __t3, __zero));
        emit(__bne(__t4, __zero, 16));
        emit(__slli(__t3, __t3, 1));
        emit(__slli(__t1, __t1, 1));
        emit(__jal(__zero, -24));
        emit(__bltu(__t2, __t3, 12));
        emit(__sub(__t2, __t2, __t3));
        emit(__add(__t0, __t0, __t1));
        emit(__srli(__t1, __t1, 1));
        emit(__srli(__t3, __t3, 1));
        emit(__bne(__t1, __zero, -20));
        emit(__addi(rd, interm, 0));
        /* Handle the correct sign for the quotient or remainder */
        emit(__beq(__t5, __zero, 8));
        emit(__sub(rd, __zero, rd));
        return;
    case OP_lshift:
        if (PH2_PAIR_DEST_SRC0(ph2_ir)) {
            emit(__addi(__t0, __zero, 32));
            emit(__bltu(rs2, __t0, 20));
            emit(__sub(__t1, rs2, __t0));
            emit(__sll(rd_hi, rs1, __t1));
            emit(__addi(rd, __zero, 0));
            emit(__jal(__zero, 32));

            /* The bits crossing into the high word are the low word shifted
             * right by 32 - n. RV32 takes a shift amount modulo 32, so a shift
             * by zero did not shift at all and ORed the whole low word in.
             * Shift by 1 and then by 31 - n, which stays in range.
             */
            emit(__addi(__t1, __zero, 31));
            emit(__srli(__t2, rs1, 1));
            emit(__sll(rd, rs1, rs2));
            emit(__sll(rd_hi, rs1_hi, rs2));
            emit(__sub(__t1, __t1, rs2));
            emit(__srl(__t2, __t2, __t1));
            emit(__or(rd_hi, rd_hi, __t2));
            return;
        }
        emit(__sll(rd, rs1, rs2));
        return;
    case OP_rshift:
        if (PH2_PAIR_DEST_SRC0(ph2_ir)) {
            bool is_unsigned = ph2_ir->src0_is_unsigned;

            emit(__addi(__t0, __zero, 32));
            emit(__bltu(rs2, __t0, 20));
            emit(__sub(__t1, rs2, __t0));
            emit(is_unsigned ? __srl(rd, rs1_hi, __t1)
                             : __sra(rd, rs1_hi, __t1));
            emit(is_unsigned ? __addi(rd_hi, __zero, 0)
                             : __srai(rd_hi, rs1_hi, 31));
            emit(__jal(__zero, 32));

            /* As for a left shift, bring the high word's bits across with two
             * in-range shifts, by 1 and by 31 - n.
             */
            emit(__addi(__t1, __zero, 31));
            emit(__slli(__t2, rs1_hi, 1));

            /* The low word takes the high word's bits in, never its own sign:
             * the sign lives in the high word.
             */
            emit(__srl(rd, rs1, rs2));
            emit(__sub(__t1, __t1, rs2));
            emit(__sll(__t2, __t2, __t1));
            emit(__or(rd, rd, __t2));
            emit(is_unsigned ? __srl(rd_hi, rs1_hi, rs2)
                             : __sra(rd_hi, rs1_hi, rs2));
            return;
        }
        emit(ph2_ir->src0_is_unsigned ? __srl(rd, rs1, rs2)
                                      : __sra(rd, rs1, rs2));
        return;
    case OP_eq:
        if (ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0) {
            emit(__xor(__t0, rs1, rs2));
            emit(__xor(rd, rs1_hi, rs2_hi));
            emit(__or(rd, rd, __t0));
            emit(__sltu(rd, __zero, rd));
            emit(__xori(rd, rd, 1));
            return;
        }
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        emit(__xori(rd, rd, 1));
        return;
    case OP_neq:
        if (ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0) {
            emit(__xor(__t0, rs1, rs2));
            emit(__xor(rd, rs1_hi, rs2_hi));
            emit(__or(rd, rd, __t0));
            emit(__sltu(rd, __zero, rd));
            return;
        }
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        return;
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        if (ph2_ir->src0_hi >= 0 && ph2_ir->src1_hi >= 0) {
            bool unsigned_cmp =
                ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned;
            bool reverse = ph2_ir->op == OP_gt || ph2_ir->op == OP_leq;
            bool invert = ph2_ir->op == OP_geq || ph2_ir->op == OP_leq;

            emit(unsigned_cmp ? __sltu(__t0, reverse ? rs2_hi : rs1_hi,
                                       reverse ? rs1_hi : rs2_hi)
                              : __slt(__t0, reverse ? rs2_hi : rs1_hi,
                                      reverse ? rs1_hi : rs2_hi));
            emit(__xor(__t1, rs1_hi, rs2_hi));
            emit(__sltu(__t1, __zero, __t1));
            emit(__xori(__t1, __t1, 1));
            emit(__sltu(rd, reverse ? rs2 : rs1, reverse ? rs1 : rs2));
            emit(__and(rd, rd, __t1));
            emit(__or(rd, rd, __t0));
            if (invert)
                emit(__xori(rd, rd, 1));
            return;
        }
        if (ph2_ir->op == OP_gt || ph2_ir->op == OP_leq)
            emit((ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned)
                     ? __sltu(rd, rs2, rs1)
                     : __slt(rd, rs2, rs1));
        else
            emit((ph2_ir->src0_is_unsigned || ph2_ir->src1_is_unsigned)
                     ? __sltu(rd, rs1, rs2)
                     : __slt(rd, rs1, rs2));
        if (ph2_ir->op == OP_geq || ph2_ir->op == OP_leq)
            emit(__xori(rd, rd, 1));
        return;
    case OP_negate:
        emit(__sub(rd, __zero, rs1));
        if (PH2_PAIR_DEST_SRC0(ph2_ir)) {
            emit(__sltu(__t0, __zero, rs1));
            emit(__sub(rd_hi, __zero, rs1_hi));
            emit(__sub(rd_hi, rd_hi, __t0));
        }
        return;
    case OP_bit_not:
        emit(__xori(rd, rs1, -1));
        if (PH2_PAIR_DEST_SRC0(ph2_ir))
            emit(__xori(rd_hi, rs1_hi, -1));
        return;
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
        emit(riscv_bitwise_insn(ph2_ir->op, rd, rs1, rs2));
        if (PH2_PAIR_BINARY(ph2_ir))
            emit(riscv_bitwise_insn(ph2_ir->op, rd_hi, rs1_hi, rs2_hi));
        return;
    case OP_log_not:
        if (ph2_ir->src0_hi >= 0) {
            emit(__or(__t0, rs1, rs1_hi));
            rs1 = __t0;
        }
        emit(__sltu(rd, __zero, rs1));
        emit(__xori(rd, rd, 1));
        return;
    case OP_trunc:
        if (ph2_ir->is_unsigned && (ph2_ir->src1 == 1 || ph2_ir->src1 == 2)) {
            int shift = ph2_ir->src1 == 1 ? 24 : 16;

            emit(__slli(rd, rs1, shift));
            emit(__srli(rd, rd, shift));
        } else if (ph2_ir->src1 == 1) {
            emit(__slli(rd, rs1, 24));
            emit(__srai(rd, rd, 24));
        } else if (ph2_ir->src1 == 2) {
            emit(__slli(rd, rs1, 16));
            emit(__srai(rd, rd, 16));
        } else if (ph2_ir->src1 == 4) {
            /* No truncation needed for 32-bit values */
            emit(__add(rd, rs1, __zero));
        } else {
            fatal("Unsupported truncation operation with invalid target size");
        }
        return;
    case OP_sign_ext: {
        /* The upper 16 bits of src1 hold the source size. */
        int source_size = (ph2_ir->src1 >> 16) & 0xFFFF;

        if (ph2_ir->dest_hi >= 0 && ph2_ir->src0_hi < 0) {
            if (source_size == 1) {
                if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer)
                    emit(__andi(rd, rs1, 0xFF));
                else {
                    emit(__slli(rd, rs1, 24));
                    emit(__srai(rd, rd, 24));
                }
            } else if (source_size == 2) {
                emit(__slli(rd, rs1, 16));
                emit(ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer
                         ? __srli(rd, rd, 16)
                         : __srai(rd, rd, 16));
            } else
                emit(__addi(rd, rs1, 0));
            if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer)
                emit(__addi(rd_hi, __zero, 0));
            else
                emit(__srai(rd_hi, rd, 31));
            return;
        }

        /* The shifts act on the whole 32-bit register, so the source's top bit
         * has to reach bit 31 whatever the target width: "unsigned short x = c"
         * with c a negative signed char shifted by only 8, leaving 0x94 for
         * -108.
         */
        int shift_amount = (4 - source_size) * 8;

        if (source_size == 2) {
            /* Sign extend from short to word (16-bit shift) For 16-bit sign
             * extension, use only shift operations since 0xFFFF is too large
             * for RISC-V immediate field
             */
            emit(__slli(rd, rs1, shift_amount));
            emit(ph2_ir->src0_is_unsigned ? __srli(rd, rd, shift_amount)
                                          : __srai(rd, rd, shift_amount));
        } else {
            /* Fallback for other sizes */
            emit(__andi(rd, rs1, 0xFF));
            emit(__slli(rd, rd, shift_amount));
            emit(ph2_ir->src0_is_unsigned ? __srli(rd, rd, shift_amount)
                                          : __srai(rd, rd, shift_amount));
        }
        return;
    }
    case OP_cast:
        /* Widen scalars into the paired representation, or preserve both halves
         * when casting between two direct wide scalar types.
         */
        emit(__addi(rd, rs1, 0));
        if (ph2_ir->dest_hi >= 0) {
            if (ph2_ir->src0_hi >= 0) {
                if (ph2_ir->dest_hi != ph2_ir->src0_hi)
                    emit(__addi(rd_hi, rs1_hi, 0));
            } else if (ph2_ir->src0_is_unsigned || ph2_ir->src0_is_pointer)
                emit(__addi(rd_hi, __zero, 0));
            else
                emit(__srai(rd_hi, rs1, 31));
        }
        return;
    default:
        fatal("Unknown opcode");
    }
}

void plt_generate(void);
void code_generate(void)
{
    int global_stack_size;
    int ofs;

    if (dynlink) {
        plt_generate();

        /* - Initial stack layout when the program starts:
         *
         *      +----------------+ (high address)
         *      | ...            |
         *      +----------------+
         *      | argv[argc - 1] |
         *      +----------------+
         *      | ...            |
         *      +----------------+
         *      | argv[0]        |
         *      +----------------+
         *      | argc           |
         *      +----------------+ <- sp points to this location.
         *
         * - At the program entry point, it must call __libc_start_main()
         *   under dynamic linking. The function prototype is as follows:
         *
         *   int __libc_start_main(int (*main) (int, char **, char **),
         *                         int argc, char **argv,
         *                         void (*init) (void),
         *                         void (*fini) (void),
         *                         void (*rtld_fini) (void),
         *                         void (*stack_end));
         *
         * Currently, to execute a dynamically linked program with the minimal
         * effort required, we perform the following call: ->
         * __libc_start_main(main_wrapper, argc, argv, NULL,
         *                      NULL, NULL, stack_end)
         */
        emit_rv32_split_address(false, __a0, __a0, elf_code_start + 40);
        emit(__lw(__a1, __sp, 0));
        emit(__addi(__a2, __sp, 4));
        emit(__addi(__a3, __zero, 0));
        emit(__addi(__a4, __zero, 0));
        emit(__addi(__a5, __zero, 0));
        emit(__addi(__a6, __sp, 0));

        /* Call __libc_start_main() via PLT[1], PC-relative through AUIPC and
         * JALR: the PLT may lie beyond the 1 MiB a JAL reaches.
         */
        ofs = (dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE) -
              (elf_code_start + elf_code->size);
        emit_rv32_split_address(true, __ra, __ra, ofs);

        /* The main wrapper is located here under the dynamic linking mode
         *
         * Use s0 and s1 registers to temporarily store 'argc' and 'argv', while
         * preserving ra on the stack.
         *
         * After the main function completes its execution, it must use the
         * original content of ra to transfer control back to
         * __libc_start_main().
         */
        emit(__addi(__sp, __sp, -12));
        emit(__sw(__ra, __sp, 8));
        emit(__sw(__s1, __sp, 4));   /* callee-saved */
        emit(__sw(__s0, __sp, 0));   /* callee-saved */
        emit(__addi(__s0, __a0, 0)); /* argc */
        emit(__addi(__s1, __a1, 0)); /* argv */
        global_stack_size =
            ALIGN_UP(GLOBAL_FUNC->stack_size, RV32_ALIGNMENT) + 4;
    } else {
        /* When using static linking, the starting address of the main wrapper
         * is here.
         *
         * Save original sp in s0 first.
         */
        global_stack_size = ALIGN_UP(GLOBAL_FUNC->stack_size, RV32_ALIGNMENT);
        emit(__addi(__s0, __sp, 0));
    }

    /* Next, the main wrapper performs:
     *   1. allocate global stack
     *   2. jump to global init function
     *   3. call the main function
     */
    emit_rv32_split_address(false, __t0, __t0, global_stack_size);
    emit(__sub(__sp, __sp, __t0));
    emit(__addi(__gp, __sp, 0)); /* Set up global pointer */

    /* A static image runs first on untouched stack memory, which reads as zero,
     * but a dynamic one follows the loader and glibc's startup over the same
     * memory. Clear the global stack so a global with no initializer starts at
     * zero there too, storing from the top word down; t0 still holds 'ofs', a
     * nonzero multiple of four. No libc call is involved, so this holds with
     * --no-libc as well.
     */
    if (dynlink) {
        emit(__addi(__t0, __t0, -4));
        emit(__add(__t1, __gp, __t0));
        emit(__sw(__zero, __t1, 0));
        emit(__bne(__t0, __zero, -12));
    }
    ofs = GLOBAL_FUNC->bbs->elf_offset - elf_code->size;
    emit_rv32_split_address(true, __ra, __ra, ofs);

    if (!dynlink) {
        /* syscall trampoline for __syscall */
        emit(__addi(__a7, __a0, 0));
        emit(__addi(__a0, __a1, 0));
        emit(__addi(__a1, __a2, 0));
        emit(__addi(__a2, __a3, 0));
        emit(__addi(__a3, __a4, 0));
        emit(__addi(__a4, __a5, 0));
        emit(__addi(__a5, __a6, 0));
        emit(__ecall());
        emit(__jalr(__zero, __ra, 0));
    }

    ph2_ir_visit_globals(emit_ph2_ir);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    if (MAIN_BB) {
        if (dynlink) {
            emit(__addi(__a0, __s0, 0));
            emit(__addi(__a1, __s1, 0));
            ofs = MAIN_BB->elf_offset - elf_code->size;
            emit_rv32_split_address(true, __ra, __ra, ofs);

            /* - Restore sp, s0 and s1.
             * - Transfer control back to __libc_start_main() using
             *   the preserved ra.
             */
            emit_rv32_split_address(false, __t0, __t0, global_stack_size);
            emit(__add(__sp, __sp, __t0));
            emit(__lw(__ra, __sp, 8));
            emit(__lw(__s1, __sp, 4));
            emit(__lw(__s0, __sp, 0));
            emit(__addi(__sp, __sp, 12));
            emit(__jalr(__zero, __ra, 0));
        } else {
            /* use original sp saved in s0 to get argc/argv */
            emit(__addi(__t0, __s0, 0));
            emit(__lw(__a0, __t0, 0));
            emit(__addi(__a1, __t0, 4));
            ofs = MAIN_BB->elf_offset - elf_code->size;
            emit_rv32_split_address(true, __ra, __ra, ofs);

            /* exit with main's return value in a0 */
            emit(__addi(__a7, __zero, 93));
            emit(__ecall());
        }
    }

    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir_t *ph2_ir = PH2_IR_FLATTEN[i];
        emit_ph2_ir(ph2_ir);
    }
}

void plt_generate(void)
{
    int addr_of_plt = dynamic_sections.elf_plt_start;
    int addr_of_got = dynamic_sections.elf_got_start;
    int end = dynamic_sections.plt_size - PLT_FIXUP_SIZE;
    int ofs, pcrel_hi, pcrel_lo;

    ofs = addr_of_got - addr_of_plt;
    pcrel_hi = ofs & ~0xFFF;
    pcrel_lo = ofs & 0xFFF;
    if (pcrel_lo > 2047) {
        pcrel_hi += 0x1000;
        pcrel_lo -= 0x1000;
    }

    /* Accroding the RISC-V ABI specification, the first PLT entry should
     * contains the following instructions:
     *
     * 1: auipc t2, %pcrel_hi(.got)
     *    sub    t1, t1, t3
     *    lw     t3, %pcrel_lo(1b)(t2)
     *    addi   t1, t1 -(PLT0_SIZE + 12)    # PLT0_SIZE is 32 bytes.
     *    addi   t0, t2, %pcrel_lo(1b)
     *    srli   t1, t1, log2(16 / PTRSIZE)  # PTRSIZE is 4 bytes.
     *    lw     t0, PTRSIZE(t0)
     *    jr     t3
     *
     * +-----------------------------------+-----------------------+
     * | Instruction                       | Contents of registers |
     * +-----------------------------------+-----------------------+
     * | auipc  t2, %pcrel_hi(.got)        | t0: <dont' care>      |
     * |                                   | t1: &PLT[N] + 12      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: &PLT[0]           |
     * +-----------------------------------+-----------------------+
     * | sub    t1, t1, t3                 | t0: <dont' care>      |
     * |                                   | t1: (N - 1) * 16 +    |
     * |                                   |     32 + 12           |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: &PLT[0]           |
     * +-----------------------------------+-----------------------+
     * | lw     t3, %pcrel_lo(1b)(t2)      | t0: <dont' care>      |
     * |                                   | t1: (N - 1) * 16 +    |
     * |                                   |     32 + 12           |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | addi   t1, t1 -(PLT0_SIZE + 12)   | t0: <dont'care>       |
     * |                                   | t1: (N - 1) * 16      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | addi   t0, t2, %pcrel_lo(1b)      | t0: &GOT[0]           |
     * |                                   | t1: (N - 1) * 16      |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | srli   t1, t1, log2(16 / PTRSIZE) | t0: &GOT[0]           |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | lw     t0, PTRSIZE(t0)            | t0: GOT[1]            |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     * | jr     t3                         | t0: GOT[1]            |
     * |                                   | t1: (N - 1) * 4       |
     * |                                   | t2: %pcrel_hi(.got)   |
     * |                                   | t3: GOT[0]            |
     * +-----------------------------------+-----------------------+
     *
     * Note:
     * - N >= 1, and it means the N-th external function.
     * - &PLT[N] - &PLT[0] = size of PLT[0] + size of several PLT stubs.
     *                     = 32             + (N - 1) * 16
     */
    elf_write_int(dynamic_sections.elf_plt, __auipc(__t2, pcrel_hi));
    elf_write_int(dynamic_sections.elf_plt, __sub(__t1, __t1, __t3));
    elf_write_int(dynamic_sections.elf_plt, __lw(__t3, __t2, pcrel_lo));
    elf_write_int(dynamic_sections.elf_plt, __addi(__t1, __t1, -44));
    elf_write_int(dynamic_sections.elf_plt, __addi(__t0, __t2, pcrel_lo));
    elf_write_int(dynamic_sections.elf_plt, __srli(__t1, __t1, 2));
    elf_write_int(dynamic_sections.elf_plt, __lw(__t0, __t0, 4));
    elf_write_int(dynamic_sections.elf_plt, __jalr(__zero, __t3, 0));
    for (int i = 0; i * PLT_ENT_SIZE < end; i++) {
        /* elf_generate() ensures that the .got section is placed a higher
         * memory address than the plt section. As a result, 'ofs' must always
         * be positive.
         *
         * addr_of_plt: the starting address of PLT[N]. (N >= 1) addr_of_got:
         * the starting address of GOT[N + 1].
         */
        addr_of_plt =
            dynamic_sections.elf_plt_start + PLT_FIXUP_SIZE + PLT_ENT_SIZE * i;
        addr_of_got = dynamic_sections.elf_got_start + PTR_SIZE * (i + 2);
        ofs = addr_of_got - addr_of_plt;

        /* In RISC-V ABI, a PLT stub takes up 4 instructions to load GOT[N + 2]:
         *
         * 1: auipc t3, %pcrel_hi(function@.got)
         *    lw     t3, %pcrel_lo(1b)(t3)
         *    jalr   t1, t3
         *    nop
         *
         * Each PLT stub uses auipc and lw instructions to perform a PC-relative
         * addressing to obtain GOT[N + 1], and then perform an unconditional
         * jump.
         *
         * +-------------------------------------+----------------------------+
         * | Instruction                         | Contents of registers      |
         * +-------------------------------------+----------------------------+
         * | auipc  t3, %pcrel_hi(function@.got) | t1: <dont'care>            |
         * |                                     | t3: pcrel_hi(function%got) |
         * +-------------------------------------+----------------------------+
         * | lw     t3, %pcrel_lo(1b)(t3)        | t1: <dont'care>            |
         * |                                     | t3: GOT[N + 1]             |
         * +-------------------------------------+----------------------------+
         * | jalr   t1, t3                       | t1: addr of nop            |
         * |                                     | t3: GOT[N + 1]             |
         * +-------------------------------------+----------------------------+
         */
        pcrel_hi = ofs & ~0xFFF;
        pcrel_lo = ofs & 0xFFF;
        if (pcrel_lo > 2047) {
            pcrel_hi += 0x1000;
            pcrel_lo -= 0x1000;
        }

        elf_write_int(dynamic_sections.elf_plt, __auipc(__t3, pcrel_hi));
        elf_write_int(dynamic_sections.elf_plt, __lw(__t3, __t3, pcrel_lo));
        elf_write_int(dynamic_sections.elf_plt, __jalr(__t1, __t3, 0));
        elf_write_int(dynamic_sections.elf_plt, __addi(__zero, __zero, 0));
    }
}
