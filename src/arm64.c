/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* AArch64 instruction encoding */

/* Identifier naming conventions
 *   - suffix _insn : returns one encoded 32-bit instruction word, which the
 *                    code generator hands to emit().
 *   - prefix A64_  : architectural register numbers and condition codes.
 *
 * Every AArch64 instruction is one fixed-width word, so, as in src/arm.c and
 * src/riscv.c, each helper here returns that word rather than writing it. The
 * @sf argument selects the 64-bit X form when true and the 32-bit W form when
 * false; it is bit 31 of every data-processing encoding below.
 *
 * What belongs here is anything whose arguments are purely architectural --
 * register numbers, immediates, displacements, widths. Anything that reads
 * shecc's IR, the register allocator's state or a basic block belongs in
 * src/arm64-codegen.c, as do the range checks, which report through fatal().
 */

#include "defs.h"

/* Register 31 is SP or the zero register depending on the instruction. */
#define A64_SP 31
#define A64_ZR 31

/* IP0 and IP1, the intra-procedure-call scratch registers the linker may use
 * between instructions. The frame pointer and link register are x29 and x30.
 */
#define A64_IP0 16
#define A64_IP1 17
#define A64_FP 29
#define A64_LR 30

typedef enum {
    A64_EQ = 0,
    A64_NE = 1,
    A64_HS = 2, /* unsigned >= */
    A64_LO = 3, /* unsigned < */
    A64_HI = 8, /* unsigned > */
    A64_LS = 9, /* unsigned <= */
    A64_GE = 10,
    A64_LT = 11,
    A64_GT = 12,
    A64_LE = 13
} a64_cond_t;

int a64_sf(bool sf)
{
    return sf ? 0x80000000 : 0;
}

/* MOVZ/MOVN/MOVK place a 16-bit immediate at bit 16 * @hw. */
int a64_movz_insn(bool sf, int rd, int imm16, int hw)
{
    return a64_sf(sf) | 0x52800000 | (hw << 21) | ((imm16 & 0xffff) << 5) | rd;
}

int a64_movn_insn(bool sf, int rd, int imm16, int hw)
{
    return a64_sf(sf) | 0x12800000 | (hw << 21) | ((imm16 & 0xffff) << 5) | rd;
}

int a64_movk_insn(bool sf, int rd, int imm16, int hw)
{
    return a64_sf(sf) | 0x72800000 | (hw << 21) | ((imm16 & 0xffff) << 5) | rd;
}

/* MOV between general registers is ORR with the zero register. SP is not a
 * general register there, so a move involving SP is ADD #0 instead.
 */
int a64_mov_insn(bool sf, int rd, int rm)
{
    return a64_sf(sf) | 0x2a0003e0 | (rm << 16) | rd;
}

int a64_add_imm_insn(bool sf, int rd, int rn, int imm12)
{
    return a64_sf(sf) | 0x11000000 | ((imm12 & 0xfff) << 10) | (rn << 5) | rd;
}

int a64_sub_imm_insn(bool sf, int rd, int rn, int imm12)
{
    return a64_sf(sf) | 0x51000000 | ((imm12 & 0xfff) << 10) | (rn << 5) | rd;
}

/* CMP #imm is SUBS into the zero register. */
int a64_cmp_imm_insn(bool sf, int rn, int imm12)
{
    return a64_sf(sf) | 0x7100001f | ((imm12 & 0xfff) << 10) | (rn << 5);
}

int a64_add_reg_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x0b000000 | (rm << 16) | (rn << 5) | rd;
}

int a64_sub_reg_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x4b000000 | (rm << 16) | (rn << 5) | rd;
}

/* CMP Rn, Rm is SUBS into the zero register. */
int a64_cmp_reg_insn(bool sf, int rn, int rm)
{
    return a64_sf(sf) | 0x6b00001f | (rm << 16) | (rn << 5);
}

/* The extended-register forms of 64-bit ADD and SUB. UXTX is the plain form
 * that may name SP; SXTW widens a W register operand as it is added.
 */
#define A64_EXT_SXTW 6
#define A64_EXT_UXTX 3

int a64_add_ext_insn(int rd, int rn, int rm, int option)
{
    return 0x8b200000 | (rm << 16) | (option << 13) | (rn << 5) | rd;
}

int a64_sub_ext_insn(int rd, int rn, int rm, int option)
{
    return 0xcb200000 | (rm << 16) | (option << 13) | (rn << 5) | rd;
}

/* NEG and MVN are SUB and ORN with the zero register as first operand. */
int a64_neg_insn(bool sf, int rd, int rm)
{
    return a64_sf(sf) | 0x4b0003e0 | (rm << 16) | rd;
}

int a64_mvn_insn(bool sf, int rd, int rm)
{
    return a64_sf(sf) | 0x2a2003e0 | (rm << 16) | rd;
}

int a64_and_reg_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x0a000000 | (rm << 16) | (rn << 5) | rd;
}

int a64_orr_reg_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x2a000000 | (rm << 16) | (rn << 5) | rd;
}

int a64_eor_reg_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x4a000000 | (rm << 16) | (rn << 5) | rd;
}

/* MUL is MADD with the zero register as addend; MSUB computes Ra - Rn * Rm. */
int a64_mul_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1b007c00 | (rm << 16) | (rn << 5) | rd;
}

int a64_msub_insn(bool sf, int rd, int rn, int rm, int ra)
{
    return a64_sf(sf) | 0x1b008000 | (rm << 16) | (ra << 10) | (rn << 5) | rd;
}

int a64_udiv_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1ac00800 | (rm << 16) | (rn << 5) | rd;
}

int a64_sdiv_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1ac00c00 | (rm << 16) | (rn << 5) | rd;
}

int a64_lslv_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1ac02000 | (rm << 16) | (rn << 5) | rd;
}

int a64_lsrv_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1ac02400 | (rm << 16) | (rn << 5) | rd;
}

int a64_asrv_insn(bool sf, int rd, int rn, int rm)
{
    return a64_sf(sf) | 0x1ac02800 | (rm << 16) | (rn << 5) | rd;
}

/* SXTB/SXTH/SXTW and UXTB/UXTH/UXTW into an X register are SBFM and UBFM taking
 * the low @bits bits.
 */
int a64_sext_insn(int rd, int rn, int bits)
{
    return 0x93400000 | ((bits - 1) << 10) | (rn << 5) | rd;
}

int a64_zext_insn(int rd, int rn, int bits)
{
    return 0xd3400000 | ((bits - 1) << 10) | (rn << 5) | rd;
}

/* CSET Rd, cond is CSINC with the zero register and the inverted condition. */
int a64_cset_insn(bool sf, int rd, a64_cond_t cond)
{
    return a64_sf(sf) | 0x1a9f07e0 | ((cond ^ 1) << 12) | rd;
}

/* Base opcode of the unscaled load/store of @size bytes. Size lives in bits
 * 31:30 and the operation in 23:22, where a load picks the sign-extending form
 * for every width narrower than a doubleword -- values sit sign-extended in
 * their X register, so a narrow load must widen the same way whichever
 * addressing form carries it. The scaled form is this plus bit 24, which is why
 * one table serves both and they can no longer disagree. @size must be 1, 2, 4
 * or 8.
 */
int a64_mem_op(bool load, int size)
{
    /* Unsigned, so a doubleword's size field shifted into bits 31:30 does not
     * overflow a signed int.
     */
    unsigned int sz = size == 8 ? 3 : size == 4 ? 2 : size == 2 ? 1 : 0;

    if (!load)
        return 0x38000000 | (sz << 30);
    return 0x38000000 | (sz << 30) | ((size == 8 ? 1 : 2) << 22);
}

/* LDUR/STUR: a signed 9-bit byte offset. */
int a64_mem_unscaled_insn(bool load, int size, int rt, int rn, int ofs)
{
    return a64_mem_op(load, size) | ((ofs & 0x1ff) << 12) | (rn << 5) | rt;
}

/* LDR/STR with an unsigned 12-bit offset scaled by the access width. */
int a64_mem_scaled_insn(bool load, int size, int rt, int rn, int ofs)
{
    return a64_mem_op(load, size) | (1 << 24) | ((ofs / size) << 10) |
           (rn << 5) | rt;
}

/* LDR/STR post-indexed: access [Rn], then add the signed 9-bit @ofs to Rn. */
int a64_mem_post_insn(bool load, int size, int rt, int rn, int ofs)
{
    return a64_mem_op(load, size) | 0x400 | ((ofs & 0x1ff) << 12) | (rn << 5) |
           rt;
}

/* How many instructions a64_mem() in the code generator spends on an access of
 * @size bytes at @ofs: one when either immediate form reaches it, and otherwise
 * MOVZ/MOVK, ADD and the access through the scratch register.
 */
int a64_mem_count(int size, int ofs)
{
    if (ofs >= 0 && ofs <= 4095 * size && !(ofs % size))
        return 1;
    if (ofs >= -256 && ofs <= 255)
        return 1;
    return 4;
}

/* STP pre-indexed and LDP post-indexed on X registers. The 7-bit offset is in
 * units of eight bytes.
 */
int a64_stp_pre_insn(int rt, int rt2, int rn, int ofs)
{
    return 0xa9800000 | (((ofs / 8) & 0x7f) << 15) | (rt2 << 10) | (rn << 5) |
           rt;
}

int a64_ldp_post_insn(int rt, int rt2, int rn, int ofs)
{
    return 0xa8c00000 | (((ofs / 8) & 0x7f) << 15) | (rt2 << 10) | (rn << 5) |
           rt;
}

/* Branch displacements are in instructions, relative to the branch itself. */
int a64_b_insn(int disp)
{
    return 0x14000000 | (disp & 0x3ffffff);
}

int a64_bl_insn(int disp)
{
    return 0x94000000 | (disp & 0x3ffffff);
}

int a64_cbz_insn(bool sf, int rt, int disp)
{
    return a64_sf(sf) | 0x34000000 | ((disp & 0x7ffff) << 5) | rt;
}

int a64_cbnz_insn(bool sf, int rt, int disp)
{
    return a64_sf(sf) | 0x35000000 | ((disp & 0x7ffff) << 5) | rt;
}

int a64_br_insn(int rn)
{
    return 0xd61f0000 | (rn << 5);
}

int a64_blr_insn(int rn)
{
    return 0xd63f0000 | (rn << 5);
}

int a64_ret_insn(void)
{
    return 0xd65f0000 | (A64_LR << 5);
}

int a64_svc_insn(int imm16)
{
    return 0xd4000001 | ((imm16 & 0xffff) << 5);
}

int a64_brk_insn(int imm16)
{
    return 0xd4200000 | ((imm16 & 0xffff) << 5);
}

/* ADRP: the page of the target, @pages 4 KiB pages from the page of the pc. */
int a64_adrp_pages_insn(int rd, int pages)
{
    return 0x90000000 | ((pages & 3) << 29) | (((pages >> 2) & 0x7ffff) << 5) |
           rd;
}
