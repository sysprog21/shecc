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
    rv_ori = 24595 /* 0b0010011 + (6 << 12) */,
    rv_andi = 28691 /* 0b0010011 + (7 << 12) */,
    rv_slli = 4115 /* 0b0010011 + (1 << 12) */,
    rv_srli = 20499 /* 0b0010011 + (5 << 12) */,
    rv_srai = 1073762323 /* 0b0010011 + (5 << 12) + (0x20 << 25) */,
    rv_slti = 8211 /* 0b0010011 + (2 << 12) */,
    rv_sltiu = 12307 /* 0b0010011 + (3 << 12) */,
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
    rv_blt = 16483 /* 0b1100011 + (4 << 12) */,
    rv_bge = 20579 /* 0b1100011 + (5 << 12) */,
    rv_bltu = 24675 /* 0b1100011 + (6 << 12) */,
    rv_bgeu = 28771 /* 0b1100011 + (7 << 12) */,
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
    rv_div = 33570867 /* 0b0110011 + (1 << 25) + (4 << 12) */,
    rv_mod = 33579059 /* 0b0110011 + (1 << 25) + (6 << 12) */
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
        error("Invalid bit copy");

    v = imm >> i_start;
    v = v & ((2 << (i_end - i_start)) - 1);
    v = v << d_start;
    return v;
}

int rv_hi(int val)
{
    if ((val & (1 << 11)) != 0)
        return val + 4096;
    return val;
}

int rv_lo(int val)
{
    if ((val & (1 << 11)) != 0)
        return (val & 0xFFF) - 4096;
    return val & 0xFFF;
}

int rv_encode_R(rv_op op, rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return op + (rd << 7) + (rs1 << 15) + (rs2 << 20);
}

int rv_encode_I(rv_op op, rv_reg rd, rv_reg rs1, int imm)
{
    if (imm > 2047 || imm < -2048)
        error("Offset too large");

    if (imm < 0) {
        imm += 4096;
        imm &= (1 << 13) - 1;
    }
    return op + (rd << 7) + (rs1 << 15) + (imm << 20);
}

int rv_encode_S(rv_op op, rv_reg rs1, rv_reg rs2, int imm)
{
    if (imm > 2047 || imm < -2048)
        error("Offset too large");

    if (imm < 0) {
        imm += 4096;
        imm &= (1 << 13) - 1;
    }
    return op + (rs1 << 15) + (rs2 << 20) + rv_extract_bits(imm, 0, 4, 7, 11) +
           rv_extract_bits(imm, 5, 11, 25, 31);
}

int rv_encode_B(rv_op op, rv_reg rs1, rv_reg rs2, int imm)
{
    int sign = 0;

    /* 13 signed bits, with bit zero ignored */
    if (imm > 4095 || imm < -4096)
        error("Offset too large");

    if (imm < 0)
        sign = 1;

    return op + (rs1 << 15) + (rs2 << 20) + rv_extract_bits(imm, 11, 11, 7, 7) +
           rv_extract_bits(imm, 1, 4, 8, 11) +
           rv_extract_bits(imm, 5, 10, 25, 30) + (sign << 31);
}

int rv_encode_J(rv_op op, rv_reg rd, int imm)
{
    int sign = 0;

    if (imm < 0) {
        sign = 1;
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

int __add(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_add, rd, rs1, rs2);
}

int __sub(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_sub, rd, rs1, rs2);
}

int __xor(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_xor, rd, rs1, rs2);
}

int __or(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_or, rd, rs1, rs2);
}

int __and(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_and, rd, rs1, rs2);
}

int __sll(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_sll, rd, rs1, rs2);
}

int __srl(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_srl, rd, rs1, rs2);
}

int __sra(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_sra, rd, rs1, rs2);
}

int __slt(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_slt, rd, rs1, rs2);
}

int __sltu(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_sltu, rd, rs1, rs2);
}

int __addi(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_addi, rd, rs1, imm);
}

int __xori(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_xori, rd, rs1, imm);
}

int __ori(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_ori, rd, rs1, imm);
}

int __andi(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_andi, rd, rs1, imm);
}

int __slli(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_slli, rd, rs1, imm);
}

int __srli(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_srli, rd, rs1, imm);
}

int __srai(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_srai, rd, rs1, imm);
}

int __slti(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_slti, rd, rs1, imm);
}

int __sltiu(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_sltiu, rd, rs1, imm);
}

int __lb(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_lb, rd, rs1, imm);
}

int __lh(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_lh, rd, rs1, imm);
}

int __lw(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_lw, rd, rs1, imm);
}

int __lbu(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_lbu, rd, rs1, imm);
}

int __lhu(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_lhu, rd, rs1, imm);
}

int __sb(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_S(rv_sb, rs1, rd, imm);
}

int __sh(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_S(rv_sh, rs1, rd, imm);
}

int __sw(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_S(rv_sw, rs1, rd, imm);
}

int __beq(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_beq, rs1, rs2, imm);
}

int __bne(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_bne, rs1, rs2, imm);
}

int __blt(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_blt, rs1, rs2, imm);
}

int __bge(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_bge, rs1, rs2, imm);
}

int __bltu(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_bltu, rs1, rs2, imm);
}

int __bgeu(rv_reg rs1, rv_reg rs2, int imm)
{
    return rv_encode_B(rv_bgeu, rs1, rs2, imm);
}

int __jal(rv_reg rd, int imm)
{
    return rv_encode_J(rv_jal, rd, imm);
}

int __jalr(rv_reg rd, rv_reg rs1, int imm)
{
    return rv_encode_I(rv_jalr, rd, rs1, imm);
}

int __lui(rv_reg rd, int imm)
{
    return rv_encode_U(rv_lui, rd, imm);
}

int __auipc(rv_reg rd, int imm)
{
    return rv_encode_U(rv_auipc, rd, imm);
}

int __ecall()
{
    return rv_encode_I(rv_ecall, __zero, __zero, 0);
}

int __ebreak()
{
    return rv_encode_I(rv_ebreak, __zero, __zero, 1);
}

int __nop()
{
    return __addi(__zero, __zero, 0);
}

int __mul(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_mul, rd, rs1, rs2);
}

int __div(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_div, rd, rs1, rs2);
}

int __mod(rv_reg rd, rv_reg rs1, rv_reg rs2)
{
    return rv_encode_R(rv_mod, rd, rs1, rs2);
}
