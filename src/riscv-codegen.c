/* Translate IR to target machine code */

#include "riscv.c"

void emit(int code)
{
    elf_write_code_int(code);
}

void cfg_flatten()
{
    func_t *func = find_func("__syscall");
    func->fn->bbs->elf_offset = 32; /* offset of start + exit in codegen */

    elf_offset = 68; /* offset of start + exit + syscall in codegen */
    GLOBAL_FUNC.fn->bbs->elf_offset = elf_offset;

    ph2_ir_t *ph2_ir;
    for (ph2_ir = GLOBAL_FUNC.fn->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next) {
        switch (ph2_ir->op) {
        case OP_load_constant:
        case OP_global_address_of:
        case OP_assign:
        case OP_global_store:
            elf_offset += 4;
            break;
        default:
            printf("Unknown opcode\n");
            abort();
        }
    }
    /* jump to main */
    elf_offset += 4;

    fn_t *fn;
    for (fn = FUNC_LIST.head; fn; fn = fn->next) {
        ph2_ir_t *flatten_ir;

        /* reserve stack */
        flatten_ir = add_ph2_ir(OP_define);
        flatten_ir->src0 = fn->func->stack_size;

        basic_block_t *bb;
        for (bb = fn->bbs; bb; bb = bb->rpo_next) {
            bb->elf_offset = elf_offset;

            if (bb == fn->bbs)
                /* save ra, sp */
                elf_offset += 8;

            ph2_ir_t *insn;
            for (insn = bb->ph2_ir_list.head; insn; insn = insn->next) {
                switch (insn->op) {
                case OP_load_constant:
                    flatten_ir = add_ph2_ir(OP_load_constant);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                    /* RISC-V uses 12 bits to encode immediate value */
                    if (flatten_ir->src0 < 2048 && flatten_ir->src0 > -2047)
                        elf_offset += 4;
                    else
                        elf_offset += 8;

                    break;
                case OP_assign:
                    if (insn->dest != insn->src0) {
                        flatten_ir = add_ph2_ir(OP_assign);
                        memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                        elf_offset += 4;
                    }
                    break;
                case OP_address_of:
                case OP_global_address_of:
                case OP_read:
                case OP_write:
                case OP_load:
                case OP_store:
                case OP_global_load:
                case OP_global_store:
                case OP_jump:
                case OP_call:
                case OP_load_func:
                case OP_indirect:
                case OP_add:
                case OP_sub:
                case OP_mul:
                case OP_div:
                case OP_mod:
                case OP_lshift:
                case OP_rshift:
                case OP_gt:
                case OP_lt:
                case OP_bit_and:
                case OP_bit_or:
                case OP_bit_xor:
                case OP_negate:
                case OP_log_and:
                case OP_bit_not:
                    /* TODO: if the offset of store/load is more than 12 bits,
                     * use compounded instructions */
                    flatten_ir = add_ph2_ir(insn->op);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 4;
                    break;
                case OP_load_data_address:
                case OP_branch:
                case OP_neq:
                case OP_geq:
                case OP_leq:
                case OP_log_not:
                case OP_log_or:
                    flatten_ir = add_ph2_ir(insn->op);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 8;
                    break;
                case OP_address_of_func:
                case OP_eq:
                    flatten_ir = add_ph2_ir(OP_eq);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 12;
                    break;
                case OP_return:
                    flatten_ir = add_ph2_ir(OP_return);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    /* restore sp */
                    flatten_ir->src1 = bb->belong_to->func->stack_size;
                    elf_offset += 16;
                    break;
                default:
                    printf("Unknown opcode\n");
                    abort();
                }
            }
        }
    }
}

void code_generate()
{
    int elf_data_start = elf_code_start + elf_offset;

    /* start */
    emit(__addi(__sp, __sp, -GLOBAL_FUNC.stack_size));
    emit(__addi(__gp, __sp, 0));
    emit(__jal(__ra, GLOBAL_FUNC.fn->bbs->elf_offset - elf_code_idx));

    /* exit */
    emit(__addi(__gp, __gp, GLOBAL_FUNC.stack_size));
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
         ph2_ir = ph2_ir->next) {
        int rd = ph2_ir->dest + 10;
        int rs1 = ph2_ir->src0 + 10;
        int rs2 = ph2_ir->src1 + 10;

        switch (ph2_ir->op) {
        case OP_load_constant:
            emit(__addi(rd, __zero, ph2_ir->src0));
            break;
        case OP_global_address_of:
            emit(__addi(rd, __gp, ph2_ir->src0));
            break;
        case OP_assign:
            emit(__addi(rd, rs1, 0));
            break;
        case OP_global_store:
            emit(__sw(rs1, __gp, ph2_ir->src1));
            break;
        default:
            printf("Unknown opcode\n");
            abort();
        }
    }
    /* jump to main */
    emit(__jal(__zero, MAIN_BB->elf_offset - elf_code_idx));

    int i;
    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir_t *ph2_ir = &PH2_IR[i];
        int rd = ph2_ir->dest + 10;
        int rs1 = ph2_ir->src0 + 10;
        int rs2 = ph2_ir->src1 + 10;
        int ofs;

        switch (ph2_ir->op) {
        case OP_define:
            emit(__addi(__sp, __sp, -ph2_ir->src0 - 4));
            emit(__sw(__ra, __sp, 0));
            break;
        case OP_load_constant:
            if (ph2_ir->src0 < 2048 && ph2_ir->src0 > -2047)
                emit(__addi(rd, __zero, ph2_ir->src0));
            else {
                emit(__lui(rd, rv_hi(ph2_ir->src0)));
                emit(__addi(rd, rd, rv_lo(ph2_ir->src0)));
            }
            break;
        case OP_load_data_address:
            emit(__lui(rd, rv_hi(elf_data_start + ph2_ir->src0)));
            emit(__addi(rd, rd, rv_lo(elf_data_start + ph2_ir->src0)));
            break;
        case OP_address_of:
            emit(__addi(rd, __sp, ph2_ir->src0));
            break;
        case OP_assign:
            emit(__addi(rd, rs1, 0));
            break;
        case OP_load:
            emit(__lw(rd, __sp, ph2_ir->src0));
            break;
        case OP_store:
            emit(__sw(rs1, __sp, ph2_ir->src1));
            break;
        case OP_global_load:
            emit(__lw(rd, __gp, ph2_ir->src0));
            break;
        case OP_global_store:
            emit(__sw(rs1, __gp, ph2_ir->src1));
            break;
        case OP_read:
            if (ph2_ir->src1 == 1)
                emit(__lb(rd, rs1, 0));
            else if (ph2_ir->src1 == 4)
                emit(__lw(rd, rs1, 0));
            else
                abort();
            break;
        case OP_write:
            if (ph2_ir->dest == 1)
                emit(__sb(rs2, rs1, 0));
            else if (ph2_ir->dest == 4)
                emit(__sw(rs2, rs1, 0));
            else
                abort();
            break;
        case OP_branch:
            emit(
                __bne(rs1, __zero, ph2_ir->then_bb->elf_offset - elf_code_idx));
            emit(__jal(__zero, ph2_ir->else_bb->elf_offset - elf_code_idx));
            break;
        case OP_jump:
            emit(__jal(__zero, ph2_ir->next_bb->elf_offset - elf_code_idx));
            break;
        case OP_call:
            emit(__jal(__ra, find_func(ph2_ir->func_name)->fn->bbs->elf_offset -
                                 elf_code_idx));
            break;
        case OP_address_of_func:
            ofs = elf_code_start +
                  find_func(ph2_ir->func_name)->fn->bbs->elf_offset;
            emit(__lui(__t0, rv_hi(ofs)));
            emit(__addi(__t0, __t0, rv_lo(ofs)));
            emit(__sw(__t0, rs1, 0));
            break;
        case OP_load_func:
            emit(__addi(__t0, rs1, 0));
            break;
        case OP_indirect:
            emit(__jalr(__ra, __t0, 0));
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                emit(__addi(__zero, __zero, 0));
            else
                emit(__addi(__a0, rs1, 0));
            emit(__lw(__ra, __sp, 0));
            emit(__addi(__sp, __sp, ph2_ir->src1 + 4));
            emit(__jalr(__zero, __ra, 0));
            break;
        case OP_add:
            emit(__add(rd, rs1, rs2));
            break;
        case OP_sub:
            emit(__sub(rd, rs1, rs2));
            break;
        case OP_mul:
            emit(__mul(rd, rs1, rs2));
            break;
        case OP_div:
            emit(__div(rd, rs1, rs2));
            break;
        case OP_mod:
            emit(__mod(rd, rs1, rs2));
            break;
        case OP_lshift:
            emit(__sll(rd, rs1, rs2));
            break;
        case OP_rshift:
            emit(__sra(rd, rs1, rs2));
            break;
        case OP_eq:
            emit(__sub(rd, rs1, rs2));
            emit(__sltu(rd, __zero, rd));
            emit(__xori(rd, rd, 1));
            break;
        case OP_neq:
            emit(__sub(rd, rs1, rs2));
            emit(__sltu(rd, __zero, rd));
            break;
        case OP_gt:
            emit(__slt(rd, rs2, rs1));
            break;
        case OP_geq:
            emit(__slt(rd, rs1, rs2));
            emit(__xori(rd, rd, 1));
            break;
        case OP_lt:
            emit(__slt(rd, rs1, rs2));
            break;
        case OP_leq:
            emit(__slt(rd, rs2, rs1));
            emit(__xori(rd, rd, 1));
            break;
        case OP_negate:
            emit(__sub(rd, __zero, rs1));
            break;
        case OP_bit_not:
            emit(__xori(rd, rs1, -1));
            break;
        case OP_bit_and:
            emit(__and(rd, rs1, rs2));
            break;
        case OP_bit_or:
            emit(__or(rd, rs1, rs2));
            break;
        case OP_bit_xor:
            emit(__xor(rd, rs1, rs2));
            break;
        case OP_log_not:
            emit(__sltu(rd, __zero, rs1));
            emit(__xori(rd, rd, 1));
            break;
        case OP_log_and:
            /* FIXME: bad logical-and instruction */
            emit(__and(rd, rs1, rs2));
            break;
        case OP_log_or:
            emit(__or(rd, rs1, rs2));
            emit(__sltu(rd, __zero, rd));
            break;
        default:
            printf("Unknown opcode\n");
            abort();
        }
    }
}
