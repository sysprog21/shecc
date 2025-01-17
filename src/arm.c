/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* ARMv7-A instruction encoding */

/* Identifier naming conventions
 *   - prefix arm_ : Arm instruction encoding.
 *   - prefix __ : mnemonic symbols for Arm instruction, condition code,
 *                 registers, etc.
 *
 * An example of usage in src/codegen.c: (unconditional jump)
 *
 *         +---------------- write specified instruction into ELF
 *         |
 *      emit(__b(__AL, ofs));
 *             |    |   |
 *             |    |   +--- to PC-relative expression
 *             |    +------- always
 *             +------------ branch
 *
 * Machine-level "b" instructions have restricted ranges from the address of
 * the current instruction.
 */

/* opcode */
typedef enum {
    arm_and = 0,
    arm_eor = 1,
    arm_sub = 2,
    arm_rsb = 3,
    arm_add = 4,
    arm_ldm = 9,
    arm_teq = 9,
    arm_cmp = 10,
    arm_orr = 12,
    arm_mov = 13,
    arm_mvn = 15,
    arm_stmdb = 16
} arm_op_t;

/* Condition code
 * Reference:
 * https://community.arm.com/developer/ip-products/processors/b/processors-ip-blog/posts/condition-codes-1-condition-flags-and-codes
 */
typedef enum {
    __EQ = 0,  /* Equal */
    __NE = 1,  /* Not equal */
    __CS = 2,  /* Unsigned higher or same */
    __CC = 3,  /* Unsigned lower */
    __LS = 9,  /* Unsigned lower or same */
    __GE = 10, /* Signed greater than or equal */
    __LT = 11, /* Signed less than */
    __GT = 12, /* Signed greater than */
    __LE = 13, /* Signed less than or equal */
    __AL = 14  /* Always executed */
} arm_cond_t;

/* Registers */
typedef enum {
    __r0 = 0,
    __r1 = 1,
    __r2 = 2,
    __r3 = 3,
    __r4 = 4,
    __r5 = 5,
    __r6 = 6,
    __r7 = 7,
    __r8 = 8,
    __r9 = 9,
    __r10 = 10,
    __r11 = 11,
    __r12 = 12,
    __sp = 13, /* stack pointer, r13 */
    __lr = 14, /* link register, r14 */
    __pc = 15  /* program counter, r15 */
} arm_reg;

typedef enum {
    logic_ls = 0, /* Logical left shift */
    logic_rs = 1, /* Logical right shift */
    arith_rs = 2, /* Arithmetic right shift */
    rotat_rs = 3  /* Rotate right shift */
} shift_type;

arm_cond_t arm_get_cond(opcode_t op)
{
    switch (op) {
    case OP_eq:
        return __EQ;
    case OP_neq:
        return __NE;
    case OP_lt:
        return __LT;
    case OP_geq:
        return __GE;
    case OP_gt:
        return __GT;
    case OP_leq:
        return __LE;
    default:
        error("Unsupported condition IR opcode");
    }
    return __AL;
}

int arm_extract_bits(int imm, int i_start, int i_end, int d_start, int d_end)
{
    if (((d_end - d_start) != (i_end - i_start)) || (i_start > i_end) ||
        (d_start > d_end))
        error("Invalid bit copy");

    int v = imm >> i_start;
    v &= ((2 << (i_end - i_start)) - 1);
    v <<= d_start;
    return v;
}

int arm_encode(arm_cond_t cond, int opcode, int rn, int rd, int op2)
{
    return (cond << 28) + (opcode << 20) + (rn << 16) + (rd << 12) + op2;
}

int __svc()
{
    return arm_encode(__AL, 240, 0, 0, 0);
}

int __mov(arm_cond_t cond, int io, int opcode, int s, int rn, int rd, int op2)
{
    int shift = 0;
    if (op2 > 255) {
        shift = 16; /* full rotation */
        while ((op2 & 3) == 0) {
            /* we can shift by two bits */
            op2 >>= 2;
            shift -= 1;
        }
        if (op2 > 255)
            /* value spans more than 8 bits */
            error("Unable to represent value");
    }
    return arm_encode(cond, s + (opcode << 1) + (io << 5), rn, rd,
                      (shift << 8) + (op2 & 255));
}

int __and_r(arm_cond_t cond, arm_reg rd, arm_reg rs, arm_reg rm)
{
    return __mov(cond, 0, arm_and, 0, rs, rd, rm);
}

int __or_r(arm_cond_t cond, arm_reg rd, arm_reg rs, arm_reg rm)
{
    return __mov(cond, 0, arm_orr, 0, rs, rd, rm);
}

int __eor_r(arm_cond_t cond, arm_reg rd, arm_reg rs, arm_reg rm)
{
    return __mov(cond, 0, arm_eor, 0, rs, rd, rm);
}

int __mvn_r(arm_cond_t cond, arm_reg rd, arm_reg rm)
{
    return __mov(cond, 0, arm_mvn, 0, 0, rd, rm);
}

int __movw(arm_cond_t cond, arm_reg rd, int imm)
{
    return arm_encode(cond, 48, 0, rd, 0) +
           arm_extract_bits(imm, 0, 11, 0, 11) +
           arm_extract_bits(imm, 12, 15, 16, 19);
}

int __movt(arm_cond_t cond, arm_reg rd, int imm)
{
    imm >>= 16;
    return arm_encode(cond, 52, 0, rd, 0) +
           arm_extract_bits(imm, 0, 11, 0, 11) +
           arm_extract_bits(imm, 12, 15, 16, 19);
}

int __mov_i(arm_cond_t cond, arm_reg rd, int imm)
{
    return __mov(cond, 1, arm_mov, 0, 0, rd, imm);
}

int __mov_r(arm_cond_t cond, arm_reg rd, arm_reg rs)
{
    return __mov(cond, 0, arm_mov, 0, 0, rd, rs);
}

int __srl(arm_cond_t cond, arm_reg rd, arm_reg rm, arm_reg rs)
{
    return arm_encode(cond, 0 + (arm_mov << 1) + (0 << 5), 0, rd,
                      rm + (1 << 4) + (1 << 5) + (rs << 8));
}

int __srl_amt(arm_cond_t cond,
              int s,
              shift_type shift,
              arm_reg rd,
              arm_reg rm,
              int amt)
{
    return arm_encode(cond, s + (arm_mov << 1) + (0 << 5), 0, rd,
                      rm + (0 << 4) + (shift << 5) + (amt << 7));
}

int __sll(arm_cond_t cond, arm_reg rd, arm_reg rm, arm_reg rs)
{
    return arm_encode(cond, 0 + (arm_mov << 1) + (0 << 5), 0, rd,
                      rm + (1 << 4) + (0 << 5) + (rs << 8));
}

int __sll_amt(arm_cond_t cond,
              int s,
              shift_type shift,
              arm_reg rd,
              arm_reg rm,
              int amt)
{
    return arm_encode(cond, s + (arm_mov << 1) + (0 << 5), 0, rd,
                      rm + (0 << 4) + (shift << 5) + (amt << 7));
}

int __sra(arm_cond_t cond, arm_reg rd, arm_reg rm, arm_reg rs)
{
    return arm_encode(cond, 0 + (arm_mov << 1) + (0 << 5), 0, rd,
                      rm + (5 << 4) + (rs << 8));
}

int __add_i(arm_cond_t cond, arm_reg rd, arm_reg rs, int imm)
{
    if (imm >= 0)
        return __mov(cond, 1, arm_add, 0, rs, rd, imm);
    return __mov(cond, 1, arm_sub, 0, rs, rd, -imm);
}

int __add_r(arm_cond_t cond, arm_reg rd, arm_reg rs, arm_reg ro)
{
    return __mov(cond, 0, arm_add, 0, rs, rd, ro);
}

int __sub_r(arm_cond_t cond, arm_reg rd, arm_reg rs, arm_reg ro)
{
    return __mov(cond, 0, arm_sub, 0, rs, rd, ro);
}

int __zero(int rd)
{
    return __mov_i(__AL, rd, 0);
}

int arm_transfer(arm_cond_t cond,
                 int l,
                 int size,
                 arm_reg rn,
                 arm_reg rd,
                 int ofs)
{
    int opcode = 64 + 16 + 8 + l;
    if (size == 1)
        opcode += 4;
    if (ofs < 0) {
        opcode -= 8;
        ofs = -ofs;
    }
    return arm_encode(cond, opcode, rn, rd, ofs & 4095);
}

int __lw(arm_cond_t cond, arm_reg rd, arm_reg rn, int ofs)
{
    return arm_transfer(cond, 1, 4, rn, rd, ofs);
}

int __lb(arm_cond_t cond, arm_reg rd, arm_reg rn, int ofs)
{
    return arm_transfer(cond, 1, 1, rn, rd, ofs);
}

int __sw(arm_cond_t cond, arm_reg rd, arm_reg rn, int ofs)
{
    return arm_transfer(cond, 0, 4, rn, rd, ofs);
}

int __sb(arm_cond_t cond, arm_reg rd, arm_reg rn, int ofs)
{
    return arm_transfer(cond, 0, 1, rn, rd, ofs);
}

int __stmdb(arm_cond_t cond, int w, arm_reg rn, int reg_list)
{
    return arm_encode(cond, arm_stmdb + (0x2 << 6) + (w << 1), rn, 0, reg_list);
}

int __ldm(arm_cond_t cond, int w, arm_reg rn, int reg_list)
{
    return arm_encode(cond, arm_ldm + (0x2 << 6) + (w << 1), rn, 0, reg_list);
}

int __b(arm_cond_t cond, int ofs)
{
    int o = (ofs - 8) >> 2;
    return arm_encode(cond, 160, 0, 0, 0) + (o & 16777215);
}

int __bl(arm_cond_t cond, int ofs)
{
    int o = (ofs - 8) >> 2;
    return arm_encode(cond, 176, 0, 0, 0) + (o & 16777215);
}

int __blx(arm_cond_t cond, arm_reg rd)
{
    return arm_encode(cond, 18, 15, 15, rd + 3888);
}

int __mul(arm_cond_t cond, arm_reg rd, arm_reg r1, arm_reg r2)
{
    return arm_encode(cond, 0, rd, 0, (r1 << 8) + 144 + r2);
}

int __div(arm_cond_t cond, arm_reg rd, arm_reg r1, arm_reg r2)
{
    return arm_encode(cond, 113, rd, 15, (r1 << 8) + 16 + r2);
}

int __rsb_i(arm_cond_t cond, arm_reg rd, int imm, arm_reg rn)
{
    return __mov(cond, 1, arm_rsb, 0, rn, rd, imm);
}

int __cmp_r(arm_cond_t cond, arm_reg r1, arm_reg r2)
{
    return __mov(cond, 0, arm_cmp, 1, r1, 0, r2);
}

int __cmp_i(arm_cond_t cond, arm_reg rn, int imm)
{
    return __mov(cond, 1, arm_cmp, 1, rn, 0, imm);
}

int __teq(arm_reg rd)
{
    return __mov(__AL, 1, arm_teq, 1, rd, 0, 0);
}
