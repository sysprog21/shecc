/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */

#include "riscv.c"

void update_elf_offset(ph2_ir_t *ph2_ir)
{
    switch (ph2_ir->op) {
    case OP_load_constant:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_address_of:
    case OP_global_address_of:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 12;
        else
            elf_offset += 4;
        return;
    case OP_assign:
        elf_offset += 4;
        return;
    case OP_load:
    case OP_global_load:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
            elf_offset += 16;
        else
            elf_offset += 4;
        return;
    case OP_store:
    case OP_global_store:
        if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047)
            elf_offset += 16;
        else
            elf_offset += 4;
        return;
    case OP_read:
    case OP_write:
    case OP_jump:
    case OP_call:
    case OP_load_func:
    case OP_indirect:
    case OP_add:
    case OP_sub:
    case OP_lshift:
    case OP_rshift:
    case OP_gt:
    case OP_lt:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_bit_not:
        elf_offset += 4;
        return;
    case OP_mul:
        if (hard_mul_div)
            elf_offset += 4;
        else
            elf_offset += 52;
        return;
    case OP_div:
    case OP_mod:
        if (hard_mul_div)
            elf_offset += 4;
        else
            elf_offset += 108;
        return;
    case OP_load_data_address:
    case OP_neq:
    case OP_geq:
    case OP_leq:
    case OP_log_not:
        elf_offset += 8;
        return;
    case OP_address_of_func:
    case OP_eq:
        elf_offset += 12;
        return;
    case OP_branch:
        elf_offset += 20;
        return;
    case OP_return:
        elf_offset += 24;
        return;
    default:
        printf("Unknown opcode\n");
        abort();
    }
}

void cfg_flatten()
{
    func_t *func = find_func("__syscall");
    func->fn->bbs->elf_offset = 48; /* offset of start + exit in codegen */

    elf_offset = 84; /* offset of start + exit + syscall in codegen */
    GLOBAL_FUNC.fn->bbs->elf_offset = elf_offset;

    for (ph2_ir_t *ph2_ir = GLOBAL_FUNC.fn->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next) {
        update_elf_offset(ph2_ir);
    }

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    elf_offset += 24;

    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        /* reserve stack */
        ph2_ir_t *flatten_ir = add_ph2_ir(OP_define);
        flatten_ir->src0 = fn->func->stack_size;

        for (basic_block_t *bb = fn->bbs; bb; bb = bb->rpo_next) {
            bb->elf_offset = elf_offset;

            if (bb == fn->bbs) {
                /* save ra, sp */
                elf_offset += 16;
            }

            for (ph2_ir_t *insn = bb->ph2_ir_list.head; insn;
                 insn = insn->next) {
                flatten_ir = add_ph2_ir(OP_generic);
                memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                if (insn->op == OP_return) {
                    /* restore sp */
                    flatten_ir->src1 = bb->belong_to->func->stack_size;
                }

                update_elf_offset(flatten_ir);
            }
        }
    }
}

void emit(int code)
{
    elf_write_code_int(code);
}

void emit_ph2_ir(ph2_ir_t *ph2_ir)
{
    func_t *func;
    int rd = ph2_ir->dest + 10;
    int rs1 = ph2_ir->src0 + 10;
    int rs2 = ph2_ir->src1 + 10;
    int ofs;

    /* Prepare the variables to reuse the same code for
     * the instruction sequence of
     * 1. division and modulo.
     * 2. load and store operations.
     * 3. address-of operations.
     */
    rv_reg interm, divisor_mask = __t1;

    switch (ph2_ir->op) {
    case OP_define:
        emit(__lui(__t0, rv_hi(ph2_ir->src0 + 4)));
        emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0 + 4)));
        emit(__sub(__sp, __sp, __t0));
        emit(__sw(__ra, __sp, 0));
        return;
    case OP_load_constant:
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(rd, rv_hi(ph2_ir->src0)));
            emit(__addi(rd, rd, rv_lo(ph2_ir->src0)));

        } else
            emit(__addi(rd, __zero, ph2_ir->src0));
        return;
    case OP_address_of:
    case OP_global_address_of:
        interm = ph2_ir->op == OP_address_of ? __sp : __gp;
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src0)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
            emit(__add(rd, interm, __t0));
        } else
            emit(__addi(rd, interm, ph2_ir->src0));
        return;
    case OP_assign:
        emit(__addi(rd, rs1, 0));
        return;
    case OP_load:
    case OP_global_load:
        interm = ph2_ir->op == OP_load ? __sp : __gp;
        if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src0)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
            emit(__add(__t0, interm, __t0));
            emit(__lw(rd, __t0, 0));
        } else
            emit(__lw(rd, interm, ph2_ir->src0));
        return;
    case OP_store:
    case OP_global_store:
        interm = ph2_ir->op == OP_store ? __sp : __gp;
        if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047) {
            emit(__lui(__t0, rv_hi(ph2_ir->src1)));
            emit(__addi(__t0, __t0, rv_lo(ph2_ir->src1)));
            emit(__add(__t0, interm, __t0));
            emit(__sw(rs1, __t0, 0));
        } else
            emit(__sw(rs1, interm, ph2_ir->src1));
        return;
    case OP_read:
        if (ph2_ir->src1 == 1)
            emit(__lb(rd, rs1, 0));
        else if (ph2_ir->src1 == 4)
            emit(__lw(rd, rs1, 0));
        else
            abort();
        return;
    case OP_write:
        if (ph2_ir->dest == 1)
            emit(__sb(rs2, rs1, 0));
        else if (ph2_ir->dest == 4)
            emit(__sw(rs2, rs1, 0));
        else
            abort();
        return;
    case OP_branch:
        ofs = elf_code_start + ph2_ir->then_bb->elf_offset;
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__beq(rs1, __zero, 8));
        emit(__jalr(__zero, __t0, 0));
        emit(__jal(__zero, ph2_ir->else_bb->elf_offset - elf_code_idx));
        return;
    case OP_jump:
        emit(__jal(__zero, ph2_ir->next_bb->elf_offset - elf_code_idx));
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        emit(__jal(__ra, func->fn->bbs->elf_offset - elf_code_idx));
        return;
    case OP_load_data_address:
        emit(__lui(rd, rv_hi(elf_data_start + ph2_ir->src0)));
        emit(__addi(rd, rd, rv_lo(elf_data_start + ph2_ir->src0)));
        return;
    case OP_address_of_func:
        func = find_func(ph2_ir->func_name);
        ofs = elf_code_start + func->fn->bbs->elf_offset;
        emit(__lui(__t0, rv_hi(ofs)));
        emit(__addi(__t0, __t0, rv_lo(ofs)));
        emit(__sw(__t0, rs1, 0));
        return;
    case OP_load_func:
        emit(__addi(__t0, rs1, 0));
        return;
    case OP_indirect:
        emit(__jalr(__ra, __t0, 0));
        return;
    case OP_return:
        if (ph2_ir->src0 == -1)
            emit(__addi(__zero, __zero, 0));
        else
            emit(__addi(__a0, rs1, 0));
        emit(__lw(__ra, __sp, 0));
        emit(__lui(__t0, rv_hi(ph2_ir->src1 + 4)));
        emit(__addi(__t0, __t0, rv_lo(ph2_ir->src1 + 4)));
        emit(__add(__sp, __sp, __t0));
        emit(__jalr(__zero, __ra, 0));
        return;
    case OP_add:
        emit(__add(rd, rs1, rs2));
        return;
    case OP_sub:
        emit(__sub(rd, rs1, rs2));
        return;
    case OP_mul:
        if (hard_mul_div)
            emit(__mul(rd, rs1, rs2));
        else {
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
        if (hard_mul_div) {
            if (ph2_ir->op == OP_div)
                emit(__div(rd, rs1, rs2));
            else
                emit(__mod(rd, rs1, rs2));
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
        /* Obtain absolute values of the dividend and divisor */
        emit(__addi(__t2, rs1, 0));
        emit(__addi(__t3, rs2, 0));
        emit(__srai(__t0, __t2, 31));
        emit(__add(__t2, __t2, __t0));
        emit(__xor(__t2, __t2, __t0));
        emit(__srai(__t1, __t3, 31));
        emit(__add(__t3, __t3, __t1));
        emit(__xor(__t3, __t3, __t1));
        emit(__xor(__t5, __t0, divisor_mask));
        /* Unsigned integer division */
        emit(__addi(__t0, __zero, 0));
        emit(__addi(__t1, __zero, 1));
        emit(__beq(__t3, __zero, 52));
        emit(__beq(__t2, __zero, 48));
        emit(__beq(__t2, __t3, 20));
        emit(__bltu(__t2, __t3, 16));
        emit(__slli(__t3, __t3, 1));
        emit(__slli(__t1, __t1, 1));
        emit(__jal(__zero, -16));
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
        emit(__sll(rd, rs1, rs2));
        return;
    case OP_rshift:
        emit(__sra(rd, rs1, rs2));
        return;
    case OP_eq:
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        emit(__xori(rd, rd, 1));
        return;
    case OP_neq:
        emit(__sub(rd, rs1, rs2));
        emit(__sltu(rd, __zero, rd));
        return;
    case OP_gt:
        emit(__slt(rd, rs2, rs1));
        return;
    case OP_geq:
        emit(__slt(rd, rs1, rs2));
        emit(__xori(rd, rd, 1));
        return;
    case OP_lt:
        emit(__slt(rd, rs1, rs2));
        return;
    case OP_leq:
        emit(__slt(rd, rs2, rs1));
        emit(__xori(rd, rd, 1));
        return;
    case OP_negate:
        emit(__sub(rd, __zero, rs1));
        return;
    case OP_bit_not:
        emit(__xori(rd, rs1, -1));
        return;
    case OP_bit_and:
        emit(__and(rd, rs1, rs2));
        return;
    case OP_bit_or:
        emit(__or(rd, rs1, rs2));
        return;
    case OP_bit_xor:
        emit(__xor(rd, rs1, rs2));
        return;
    case OP_log_not:
        emit(__sltu(rd, __zero, rs1));
        emit(__xori(rd, rd, 1));
        return;
    default:
        printf("Unknown opcode\n");
        abort();
    }
}

void code_generate()
{
    elf_data_start = elf_code_start + elf_offset;

    /* start */
    emit(__lui(__t0, rv_hi(GLOBAL_FUNC.stack_size)));
    emit(__addi(__t0, __t0, rv_lo(GLOBAL_FUNC.stack_size)));
    emit(__sub(__sp, __sp, __t0));
    emit(__addi(__gp, __sp, 0));
    emit(__jal(__ra, GLOBAL_FUNC.fn->bbs->elf_offset - elf_code_idx));

    /* exit */
    emit(__lui(__t0, rv_hi(GLOBAL_FUNC.stack_size)));
    emit(__addi(__t0, __t0, rv_lo(GLOBAL_FUNC.stack_size)));
    emit(__add(__gp, __gp, __t0));
    emit(__addi(__sp, __gp, 0));
    emit(__addi(__a0, __a0, 0));
    emit(__addi(__a7, __zero, 93));
    emit(__ecall());

    /* syscall */
    emit(__addi(__a7, __a0, 0));
    emit(__addi(__a0, __a1, 0));
    emit(__addi(__a1, __a2, 0));
    emit(__addi(__a2, __a3, 0));
    emit(__addi(__a3, __a4, 0));
    emit(__addi(__a4, __a5, 0));
    emit(__addi(__a5, __a6, 0));
    emit(__ecall());
    emit(__jalr(__zero, __ra, 0));

    ph2_ir_t *ph2_ir;
    for (ph2_ir = GLOBAL_FUNC.fn->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next)
        emit_ph2_ir(ph2_ir);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    emit(__lui(__t0, rv_hi(GLOBAL_FUNC.stack_size)));
    emit(__addi(__t0, __t0, rv_lo(GLOBAL_FUNC.stack_size)));
    emit(__add(__t0, __gp, __t0));
    emit(__lw(__a0, __t0, 0));
    emit(__addi(__a1, __t0, 4));
    emit(__jal(__zero, MAIN_BB->elf_offset - elf_code_idx));

    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];
        emit_ph2_ir(ph2_ir);
    }
}
