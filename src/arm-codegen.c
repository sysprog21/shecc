/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* Translate IR to target machine code */

#include "arm.c"

void update_elf_offset(ph2_ir_t *ph2_ir)
{
    switch (ph2_ir->op) {
    case OP_load_constant:
        /* ARMv7 uses 12 bits to encode immediate value, but the higher 4 bits
         * are for rotation. See A5.2.4 "Modified immediate constants in ARM
         * instructions" in ARMv7-A manual.
         */
        if (ph2_ir->src0 < 0)
            elf_offset += 12;
        else if (ph2_ir->src0 > 255)
            elf_offset += 8;
        else
            elf_offset += 4;
        return;
    case OP_address_of:
    case OP_global_address_of:
        /* ARMv7 uses 12 bits to encode immediate value, but the higher 4 bits
         * are for rotation. See A5.2.4 "Modified immediate constants in ARM
         * instructions" in ARMv7-A manual.
         */
        if (ph2_ir->src0 > 255)
            elf_offset += 12;
        else if (ph2_ir->src0 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_assign:
        if (ph2_ir->dest != ph2_ir->src0)
            elf_offset += 4;
        return;
    case OP_load:
    case OP_global_load:
        /* ARMv7 straight uses 12 bits to encode the offset of load instruction
         * (no rotation).
         */
        if (ph2_ir->src0 > 4095)
            elf_offset += 16;
        else if (ph2_ir->src0 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_store:
    case OP_global_store:
        /* ARMv7 straight uses 12 bits to encode the offset of store instruction
         * (no rotation).
         */
        if (ph2_ir->src1 > 4095)
            elf_offset += 16;
        else if (ph2_ir->src1 >= 0)
            elf_offset += 4;
        else
            abort();
        return;
    case OP_read:
    case OP_write:
    case OP_jump:
    case OP_call:
    case OP_load_func:
    case OP_indirect:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_lshift:
    case OP_rshift:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_negate:
    case OP_log_and:
    case OP_bit_not:
        elf_offset += 4;
        return;
    case OP_div:
        if (hard_mul_div) {
            elf_offset += 4;
        } else {
            elf_offset += 104;
        }
        return;
    case OP_mod:
        if (hard_mul_div) {
            elf_offset += 12;
        } else {
            elf_offset += 104;
        }
        return;
    case OP_load_data_address:
        elf_offset += 8;
        return;
    case OP_address_of_func:
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
    case OP_log_not:
    case OP_log_or:
        elf_offset += 12;
        return;
    case OP_branch:
        if (ph2_ir->is_branch_detached)
            elf_offset += 12;
        else
            elf_offset += 8;
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
    func->fn->bbs->elf_offset = 44; /* offset of start + exit in codegen */

    elf_offset = 80; /* offset of start + exit + syscall in codegen */
    GLOBAL_FUNC.fn->bbs->elf_offset = elf_offset;

    for (ph2_ir_t *ph2_ir = GLOBAL_FUNC.fn->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next) {
        update_elf_offset(ph2_ir);
    }

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    elf_offset += 24;

    for (fn_t *fn = FUNC_LIST.head; fn; fn = fn->next) {
        ph2_ir_t *flatten_ir;

        /* reserve stack */
        flatten_ir = add_ph2_ir(OP_define);
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

                if (insn->op == OP_branch) {
                    /* In SSA, we index 'else_bb' first, and then 'then_bb' */
                    if (insn->else_bb != bb->rpo_next)
                        flatten_ir->is_branch_detached = true;
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
    int rd = ph2_ir->dest;
    int rn = ph2_ir->src0;
    int rm = ph2_ir->src1;
    int ofs;

    switch (ph2_ir->op) {
    case OP_define:
        emit(__sw(__AL, __lr, __sp, -4));
        emit(__movw(__AL, __r8, ph2_ir->src0 + 4));
        emit(__movt(__AL, __r8, ph2_ir->src0 + 4));
        emit(__sub_r(__AL, __sp, __sp, __r8));
        return;
    case OP_load_constant:
        if (ph2_ir->src0 < 0) {
            emit(__movw(__AL, __r8, -ph2_ir->src0));
            emit(__movt(__AL, __r8, -ph2_ir->src0));
            emit(__rsb_i(__AL, rd, 0, __r8));
        } else if (ph2_ir->src0 > 255) {
            emit(__movw(__AL, rd, ph2_ir->src0));
            emit(__movt(__AL, rd, ph2_ir->src0));
        } else
            emit(__mov_i(__AL, rd, ph2_ir->src0));
        return;
    case OP_address_of:
        if (ph2_ir->src0 > 255) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, rd, __sp, __r8));
        } else
            emit(__add_i(__AL, rd, __sp, ph2_ir->src0));
        return;
    case OP_global_address_of:
        if (ph2_ir->src0 > 255) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, rd, __r12, __r8));
        } else
            emit(__add_i(__AL, rd, __r12, ph2_ir->src0));
        return;
    case OP_assign:
        emit(__mov_r(__AL, rd, rn));
        return;
    case OP_load:
        if (ph2_ir->src0 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, __r8, __sp, __r8));
            emit(__lw(__AL, rd, __r8, 0));
        } else
            emit(__lw(__AL, rd, __sp, ph2_ir->src0));
        return;
    case OP_store:
        if (ph2_ir->src1 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src1));
            emit(__movt(__AL, __r8, ph2_ir->src1));
            emit(__add_r(__AL, __r8, __sp, __r8));
            emit(__sw(__AL, rn, __r8, 0));
        } else
            emit(__sw(__AL, rn, __sp, ph2_ir->src1));
        return;
    case OP_global_load:
        if (ph2_ir->src0 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src0));
            emit(__movt(__AL, __r8, ph2_ir->src0));
            emit(__add_r(__AL, __r8, __r12, __r8));
            emit(__lw(__AL, rd, __r8, 0));
        } else
            emit(__lw(__AL, rd, __r12, ph2_ir->src0));
        return;
    case OP_global_store:
        if (ph2_ir->src1 > 4095) {
            emit(__movw(__AL, __r8, ph2_ir->src1));
            emit(__movt(__AL, __r8, ph2_ir->src1));
            emit(__add_r(__AL, __r8, __r12, __r8));
            emit(__sw(__AL, rn, __r8, 0));
        } else
            emit(__sw(__AL, rn, __r12, ph2_ir->src1));
        return;
    case OP_read:
        if (ph2_ir->src1 == 1)
            emit(__lb(__AL, rd, rn, 0));
        else if (ph2_ir->src1 == 4)
            emit(__lw(__AL, rd, rn, 0));
        else
            abort();
        return;
    case OP_write:
        if (ph2_ir->dest == 1)
            emit(__sb(__AL, rm, rn, 0));
        else if (ph2_ir->dest == 4)
            emit(__sw(__AL, rm, rn, 0));
        else
            abort();
        return;
    case OP_branch:
        emit(__teq(rn));
        if (ph2_ir->is_branch_detached) {
            emit(__b(__NE, 8));
            emit(__b(__AL, ph2_ir->else_bb->elf_offset - elf_code_idx));
        } else
            emit(__b(__NE, ph2_ir->then_bb->elf_offset - elf_code_idx));
        return;
    case OP_jump:
        emit(__b(__AL, ph2_ir->next_bb->elf_offset - elf_code_idx));
        return;
    case OP_call:
        func = find_func(ph2_ir->func_name);
        emit(__bl(__AL, func->fn->bbs->elf_offset - elf_code_idx));
        return;
    case OP_load_data_address:
        emit(__movw(__AL, rd, ph2_ir->src0 + elf_data_start));
        emit(__movt(__AL, rd, ph2_ir->src0 + elf_data_start));
        return;
    case OP_address_of_func:
        func = find_func(ph2_ir->func_name);
        ofs = elf_code_start + func->fn->bbs->elf_offset;
        emit(__movw(__AL, __r8, ofs));
        emit(__movt(__AL, __r8, ofs));
        emit(__sw(__AL, __r8, rn, 0));
        return;
    case OP_load_func:
        emit(__mov_r(__AL, __r8, rn));
        return;
    case OP_indirect:
        emit(__blx(__AL, __r8));
        return;
    case OP_return:
        if (ph2_ir->src0 == -1)
            emit(__mov_r(__AL, __r0, __r0));
        else
            emit(__mov_r(__AL, __r0, rn));
        emit(__movw(__AL, __r8, ph2_ir->src1 + 4));
        emit(__movt(__AL, __r8, ph2_ir->src1 + 4));
        emit(__add_r(__AL, __sp, __sp, __r8));
        emit(__lw(__AL, __lr, __sp, -4));
        emit(__blx(__AL, __lr));
        return;
    case OP_add:
        emit(__add_r(__AL, rd, rn, rm));
        return;
    case OP_sub:
        emit(__sub_r(__AL, rd, rn, rm));
        return;
    case OP_mul:
        emit(__mul(__AL, rd, rn, rm));
        return;
    case OP_div:
        if (hard_mul_div) {
            emit(__div(__AL, rd, rm, rn));
        } else {
            /* Obtain absoulte values of dividend and divisor */
            emit(__srl_amt(__AL, 0, arith_rs, __r8, rn, 31));
            emit(__add_r(__AL, rn, rn, __r8));
            emit(__eor_r(__AL, rn, rn, __r8));
            emit(__srl_amt(__AL, 0, arith_rs, __r9, rm, 31));
            emit(__add_r(__AL, rm, rm, __r9));
            emit(__eor_r(__AL, rm, rm, __r9));
            emit(__eor_r(__AL, __r10, __r8, __r9));
            /* Unsigned integer division */
            emit(__zero(__r9));
            emit(__mov_i(__AL, __r8, 1));
            emit(__cmp_i(__AL, rm, 0));
            emit(__b(__EQ, 52));
            emit(__cmp_i(__AL, rn, 0));
            emit(__b(__EQ, 44));
            emit(__cmp_r(__AL, rm, rn));
            emit(__sll_amt(__CC, 0, logic_ls, rm, rm, 1));
            emit(__sll_amt(__CC, 0, logic_ls, __r8, __r8, 1));
            emit(__b(__CC, -12));
            emit(__cmp_r(__AL, rn, rm));
            emit(__sub_r(__CS, rn, rn, rm));
            emit(__add_r(__CS, __r9, __r9, __r8));
            emit(__srl_amt(__AL, 1, logic_rs, __r8, __r8, 1));
            emit(__srl_amt(__CC, 0, logic_rs, rm, rm, 1));
            emit(__b(__CC, -20));
            emit(__mov_r(__AL, rd, __r9));
            /* Handle the correct sign for quotient */
            emit(__cmp_i(__AL, __r10, 0));
            emit(__rsb_i(__NE, rd, 0, rd));
        }
        return;
    case OP_mod:
        if (hard_mul_div) {
            emit(__div(__AL, __r8, rm, rn));
            emit(__mul(__AL, __r8, rm, __r8));
            emit(__sub_r(__AL, rd, rn, __r8));
        } else {
            /* Obtain absoulte values of dividend and divisor */
            emit(__srl_amt(__AL, 0, arith_rs, __r8, rn, 31));
            emit(__add_r(__AL, rn, rn, __r8));
            emit(__eor_r(__AL, rn, rn, __r8));
            emit(__srl_amt(__AL, 0, arith_rs, __r9, rm, 31));
            emit(__add_r(__AL, rm, rm, __r9));
            emit(__eor_r(__AL, rm, rm, __r9));
            emit(__mov_r(__AL, __r10, __r8));
            /* Unsigned integer division */
            emit(__zero(__r9));
            emit(__mov_i(__AL, __r8, 1));
            emit(__cmp_i(__AL, rm, 0));
            emit(__b(__EQ, 52));
            emit(__cmp_i(__AL, rn, 0));
            emit(__b(__EQ, 44));
            emit(__cmp_r(__AL, rm, rn));
            emit(__sll_amt(__CC, 0, logic_ls, rm, rm, 1));
            emit(__sll_amt(__CC, 0, logic_ls, __r8, __r8, 1));
            emit(__b(__CC, -12));
            emit(__cmp_r(__AL, rn, rm));
            emit(__sub_r(__CS, rn, rn, rm));
            emit(__add_r(__CS, __r9, __r9, __r8));
            emit(__srl_amt(__AL, 1, logic_rs, __r8, __r8, 1));
            emit(__srl_amt(__CC, 0, logic_rs, rm, rm, 1));
            emit(__b(__CC, -20));
            emit(__mov_r(__AL, rd, rn));
            /* Handle the correct sign for remainder */
            emit(__cmp_i(__AL, __r10, 0));
            emit(__rsb_i(__NE, rd, 0, rd));
        }
        return;
    case OP_lshift:
        emit(__sll(__AL, rd, rn, rm));
        return;
    case OP_rshift:
        emit(__srl(__AL, rd, rn, rm));
        return;
    case OP_eq:
    case OP_neq:
    case OP_gt:
    case OP_lt:
    case OP_geq:
    case OP_leq:
        emit(__cmp_r(__AL, rn, rm));
        emit(__zero(rd));
        emit(__mov_i(arm_get_cond(ph2_ir->op), rd, 1));
        return;
    case OP_negate:
        emit(__rsb_i(__AL, rd, 0, rn));
        return;
    case OP_bit_not:
        emit(__mvn_r(__AL, rd, rn));
        return;
    case OP_bit_and:
        emit(__and_r(__AL, rd, rn, rm));
        return;
    case OP_bit_or:
        emit(__or_r(__AL, rd, rn, rm));
        return;
    case OP_bit_xor:
        emit(__eor_r(__AL, rd, rn, rm));
        return;
    case OP_log_not:
        emit(__teq(rn));
        emit(__mov_i(__NE, rd, 0));
        emit(__mov_i(__EQ, rd, 1));
        return;
    case OP_log_and:
        /* FIXME: bad logical-and instruction */
        emit(__and_r(__AL, rd, rn, rm));
        return;
    case OP_log_or:
        emit(__or_r(__AL, rd, rn, rm));
        emit(__teq(rd));
        emit(__mov_i(__NE, rd, 1));
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
    emit(__movw(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__movt(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__sub_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r12, __sp));
    emit(__bl(__AL, GLOBAL_FUNC.fn->bbs->elf_offset - elf_code_idx));

    /* exit */
    emit(__movw(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__movt(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__add_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r0, __r0));
    emit(__mov_i(__AL, __r7, 1));
    emit(__svc());

    /* syscall */
    emit(__mov_r(__AL, __r7, __r0));
    emit(__mov_r(__AL, __r0, __r1));
    emit(__mov_r(__AL, __r1, __r2));
    emit(__mov_r(__AL, __r2, __r3));
    emit(__mov_r(__AL, __r3, __r4));
    emit(__mov_r(__AL, __r4, __r5));
    emit(__mov_r(__AL, __r5, __r6));
    emit(__svc());
    emit(__mov_r(__AL, __pc, __lr));

    ph2_ir_t *ph2_ir;
    for (ph2_ir = GLOBAL_FUNC.fn->bbs->ph2_ir_list.head; ph2_ir;
         ph2_ir = ph2_ir->next)
        emit_ph2_ir(ph2_ir);

    /* prepare 'argc' and 'argv', then proceed to 'main' function */
    emit(__movw(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__movt(__AL, __r8, GLOBAL_FUNC.stack_size));
    emit(__add_r(__AL, __r8, __r12, __r8));
    emit(__lw(__AL, __r0, __r8, 0));
    emit(__add_i(__AL, __r1, __r8, 4));
    emit(__b(__AL, MAIN_BB->elf_offset - elf_code_idx));

    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];
        emit_ph2_ir(ph2_ir);
    }
}
