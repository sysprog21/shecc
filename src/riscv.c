/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* RISC-V instruction encoding */

/* opcodes */
typedef enum {
    /* R type */
    rv_add = 51 /* 0b110011 + (0 << 12) */,
    rv_sub = 1073741875 /* 0b110011 + (0 << 12) + (0x20 << 25) */,
    rv_xor = 16435 /* 0b110011 + (4 << 12) */,
    rv_or = 24627 /* 0b110011 + (6 << 12) */,
    rv_and = 28723 /* 0b110011 + (7 << 12) */,
    rv_sll = 4147 /* 0b110011 + (1 << 12) */,
    rv_srl = 20531 /* 0b110011 + (5 << 12) */,
    rv_sra = 1073762355 /* 0b110011 + (5 << 12) + (0x20 << 25) */,
    rv_slt = 8243 /* 0b110011 + (2 << 12) */,
    rv_sltu = 12339 /* 0b110011 + (3 << 12) */,
    /* I type */
    rv_addi = 19 /* 0b0010011 */,
    rv_xori = 16403 /* 0b0010011 + (4 << 12) */,
    rv_andi = 28691 /* 0b0010011 + (7 << 12) */,
    rv_slli = 4115 /* 0b0010011 + (1 << 12) */,
    rv_srli = 20499 /* 0b0010011 + (5 << 12) */,
    rv_srai = 1073762323 /* 0b0010011 + (5 << 12) + (0x20 << 25) */,
    /* load/store */
    rv_lb = 3 /* 0b11 */,
    rv_lh = 4099 /* 0b11 + (1 << 12) */,
    rv_lw = 8195 /* 0b11 + (2 << 12) */,
    rv_lbu = 16387 /* 0b11 + (4 << 12) */,
    rv_lhu = 20483 /* 0b11 + (5 << 12) */,
    rv_sb = 35 /* 0b0100011 */,
    rv_sh = 4131 /* 0b0100011 + (1 << 12) */,
    rv_sw = 8227 /* 0b0100011 + (2 << 12) */,
    /* branch */
    rv_beq = 99 /* 0b1100011 */,
    rv_bne = 4195 /* 0b1100011 + (1 << 12) */,
    rv_bltu = 24675 /* 0b1100011 + (6 << 12) */,
    /* jumps */
    rv_jal = 111 /* 0b1101111 */,
    rv_jalr = 103 /* 0b1100111 */,
    /* misc */
    rv_lui = 55 /* 0b0110111 */,
    rv_auipc = 23 /* 0b0010111 */,
    rv_ecall = 115 /* 0b1110011 */,
    rv_ebreak = 1048691 /* 0b1110011 + (1 << 20) */,
    /* m */
    rv_mul = 33554483 /* 0b0110011 + (1 << 25) */,
    rv_mulhu = 33566771 /* rv_mul + (3 << 12): unsigned high half */,
    rv_div = 33570867 /* 0b0110011 + (1 << 25) + (4 << 12) */,
    rv_divu = 33574963 /* 0b0110011 + (1 << 25) + (5 << 12) */,
    rv_mod = 33579059 /* 0b0110011 + (1 << 25) + (6 << 12) */,
    rv_modu = 33583155 /* 0b0110011 + (1 << 25) + (7 << 12) */
} rv_op;

/* registers */
typedef enum {
    __zero = 0,
    __ra = 1,
    __sp = 2,
    __gp = 3,
    __tp = 4,
    __t0 = 5,
    __t1 = 6,
    __t2 = 7,
    __s0 = 8,
    __s1 = 9,
    __a0 = 10,
    __a1 = 11,
    __a2 = 12,
    __a3 = 13,
    __a4 = 14,
    __a5 = 15,
    __a6 = 16,
    __a7 = 17,
    __s2 = 18,
    __s3 = 19,
    __s4 = 20,
    __s5 = 21,
    __s6 = 22,
    __s7 = 23,
    __s8 = 24,
    __s9 = 25,
    __s10 = 26,
    __s11 = 27,
    __t3 = 28,
    __t4 = 29,
    __t5 = 30,
    __t6 = 31
} rv_reg;

int rv_extract_bits(int imm, int i_start, int i_end, int d_start, int d_end)
{
    int v;

    if (d_end - d_start != i_end - i_start || i_start > i_end ||
        d_start > d_end)
        fatal("Invalid bit copy");

    v = imm >> i_start;
    v &= ((2 << (i_end - i_start)) - 1);
    v <<= d_start;
    return v;
}

int rv_hi(int val)
{
    return val + ((val & (1 << 11)) << 1);
}

int rv_lo(int val)
{
    return (val & 0xFFF) - ((val & (1 << 11)) << 1);
}

int rv_encode_R(rv_op op, rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return op + (rd << 7) + (rs1 << 15) + (rs2 << 20);
}

static int rv_imm12(int imm)
{
    if (imm > 2047 || imm < -2048)
        fatal("Offset too large");
    return imm < 0 ? imm + 4096 : imm;
}

int rv_encode_I(rv_op op, rv_reg rd, rv_reg rs1, int imm)
{
    imm = rv_imm12(imm);
    return op + (rd << 7) + (rs1 << 15) + (imm << 20);
}

int rv_encode_S(rv_op op, rv_reg rs1, rv_reg rs2, int imm)
{
    imm = rv_imm12(imm);
    return op + (rs1 << 15) + (rs2 << 20) + rv_extract_bits(imm, 0, 4, 7, 11) +
           rv_extract_bits(imm, 5, 11, 25, 31);
}

int rv_encode_B(rv_op op, rv_reg rs1, rv_reg rs2, int imm)
{
    bool sign = false;

    /* 13 signed bits, with bit zero ignored */
    if (imm > 4095 || imm < -4096)
        fatal("Offset too large");

    if (imm < 0)
        sign = true;

    return op + (rs1 << 15) + (rs2 << 20) + rv_extract_bits(imm, 11, 11, 7, 7) +
           rv_extract_bits(imm, 1, 4, 8, 11) +
           rv_extract_bits(imm, 5, 10, 25, 30) + (sign << 31);
}

int rv_encode_J(rv_op op, rv_reg rd, int imm)
{
    bool sign = false;

    /* valid jump range: -1MB to 1MB */
    if (imm > 1048575 || imm < -1048576)
        fatal("Offset too large");

    if (imm < 0) {
        sign = true;
        imm = -imm;
        imm = (1 << 21) - imm;
    }
    return op + (rd << 7) + rv_extract_bits(imm, 1, 10, 21, 30) +
           rv_extract_bits(imm, 11, 11, 20, 20) +
           rv_extract_bits(imm, 12, 19, 12, 19) + (sign << 31);
}

int rv_encode_U(rv_op op, rv_reg rd, int imm)
{
    return op + (rd << 7) + rv_extract_bits(imm, 12, 31, 12, 31);
}

/* Typed wrappers share the encoding-format argument order. */
#define RV_INSN_R(name, op)                     \
    int name(rv_reg rd, rv_reg rs1, rv_reg rs2) \
    {                                           \
        return rv_encode_R(op, rd, rs1, rs2);   \
    }
#define RV_INSN_I(name, op)                   \
    int name(rv_reg rd, rv_reg rs1, int imm)  \
    {                                         \
        return rv_encode_I(op, rd, rs1, imm); \
    }
#define RV_INSN_S(name, op)                   \
    int name(rv_reg rd, rv_reg rs1, int imm)  \
    {                                         \
        return rv_encode_S(op, rs1, rd, imm); \
    }
#define RV_INSN_B(name, op)                    \
    int name(rv_reg rs1, rv_reg rs2, int imm)  \
    {                                          \
        return rv_encode_B(op, rs1, rs2, imm); \
    }
#define RV_INSN_U(name, op)              \
    int name(rv_reg rd, int imm)         \
    {                                    \
        return rv_encode_U(op, rd, imm); \
    }
RV_INSN_R(__add, rv_add)
RV_INSN_R(__sub, rv_sub)
RV_INSN_R(__mulhu, rv_mulhu)
RV_INSN_R(__xor, rv_xor)
RV_INSN_R(__or, rv_or)
RV_INSN_R(__and, rv_and)
RV_INSN_R(__sll, rv_sll)
RV_INSN_R(__srl, rv_srl)
RV_INSN_R(__sra, rv_sra)
RV_INSN_R(__slt, rv_slt)
RV_INSN_R(__sltu, rv_sltu)
RV_INSN_I(__addi, rv_addi)
RV_INSN_I(__xori, rv_xori)
RV_INSN_I(__andi, rv_andi)
RV_INSN_I(__slli, rv_slli)
RV_INSN_I(__srli, rv_srli)
RV_INSN_I(__srai, rv_srai)
RV_INSN_I(__lb, rv_lb)
RV_INSN_I(__lh, rv_lh)
RV_INSN_I(__lbu, rv_lbu)
RV_INSN_I(__lhu, rv_lhu)
RV_INSN_I(__lw, rv_lw)
RV_INSN_S(__sb, rv_sb)
RV_INSN_S(__sh, rv_sh)
RV_INSN_S(__sw, rv_sw)
RV_INSN_B(__beq, rv_beq)
RV_INSN_B(__bne, rv_bne)
RV_INSN_B(__bltu, rv_bltu)

int __jal(rv_reg rd, int imm)
{
    return rv_encode_J(rv_jal, rd, imm);
}
RV_INSN_I(__jalr, rv_jalr)
RV_INSN_U(__lui, rv_lui)
RV_INSN_U(__auipc, rv_auipc)

int __ecall(void)
{
    return rv_encode_I(rv_ecall, __zero, __zero, 0);
}

RV_INSN_R(__mul, rv_mul)
RV_INSN_R(__div, rv_div)
RV_INSN_R(__divu, rv_divu)
RV_INSN_R(__mod, rv_mod)
RV_INSN_R(__modu, rv_modu)

#undef RV_INSN_R
#undef RV_INSN_I
#undef RV_INSN_S
#undef RV_INSN_B
#undef RV_INSN_U
