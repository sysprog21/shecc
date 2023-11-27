/* Translate IR to target machine code */

#include "arm.c"

void emit(int code)
{
    elf_write_code_int(code);
}

void cfg_flatten()
{
    func_t *func = find_func("__syscall");
    func->fn->bbs->elf_offset = 44; /* offset of start + exit in codegen */

    elf_offset = 80; /* offset of start + exit + syscall in codegen */
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
                elf_offset += 16;

            ph2_ir_t *insn;
            for (insn = bb->ph2_ir_list.head; insn; insn = insn->next) {
                switch (insn->op) {
                case OP_load_constant:
                    flatten_ir = add_ph2_ir(OP_load_constant);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                    /**
                     * ARMv7 uses 12 bits to encode immediate value, but the
                     * higher 4 bits are for rotation. See A5.2.4 "Modified
                     * immediate constants in ARM instructions" in ARMv7-A
                     * manual.
                     */
                    if (flatten_ir->src0 < 0)
                        elf_offset += 12;
                    else if (flatten_ir->src0 > 255)
                        elf_offset += 8;
                    else
                        elf_offset += 4;

                    break;
                case OP_assign:
                    if (insn->dest != insn->src0) {
                        flatten_ir = add_ph2_ir(OP_assign);
                        memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                        elf_offset += 4;
                    }
                    break;
                case OP_address_of:
                    flatten_ir = add_ph2_ir(OP_address_of);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                    /**
                     * ARMv7 uses 12 bits to encode immediate value, but the
                     * higher 4 bits are for rotation. See A5.2.4 "Modified
                     * immediate constants in ARM instructions" in ARMv7-A
                     * manual.
                     */
                    if (flatten_ir->src0 > 255)
                        elf_offset += 12;
                    else
                        elf_offset += 4;

                    break;
                case OP_load:
                case OP_global_load:
                    flatten_ir = add_ph2_ir(OP_address_of);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                    /**
                     * ARMv7 straight uses 12 bits to encode the offset of
                     * load instruction (no rotation).
                     */
                    if (flatten_ir->src0 > 4095)
                        elf_offset += 16;
                    else
                        elf_offset += 4;

                    break;
                case OP_store:
                case OP_global_store:
                    flatten_ir = add_ph2_ir(OP_address_of);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));

                    /**
                     * ARMv7 straight uses 12 bits to encode the offset of
                     * store instruction (no rotation).
                     */
                    if (flatten_ir->src1 > 4095)
                        elf_offset += 16;
                    else
                        elf_offset += 4;

                    break;
                case OP_global_address_of:
                case OP_read:
                case OP_write:
                case OP_jump:
                case OP_call:
                case OP_load_func:
                case OP_indirect:
                case OP_add:
                case OP_sub:
                case OP_mul:
                case OP_div:
                case OP_lshift:
                case OP_rshift:
                case OP_bit_and:
                case OP_bit_or:
                case OP_bit_xor:
                case OP_negate:
                case OP_log_and:
                case OP_bit_not:
                    flatten_ir = add_ph2_ir(insn->op);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 4;
                    break;
                case OP_load_data_address:
                    flatten_ir = add_ph2_ir(OP_load_data_address);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 8;
                    break;
                case OP_address_of_func:
                case OP_mod:
                case OP_eq:
                case OP_neq:
                case OP_gt:
                case OP_lt:
                case OP_geq:
                case OP_leq:
                case OP_log_not:
                case OP_log_or:
                    flatten_ir = add_ph2_ir(insn->op);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 12;
                    break;
                case OP_branch:
                    flatten_ir = add_ph2_ir(OP_branch);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    elf_offset += 24;
                    break;
                case OP_return:
                    flatten_ir = add_ph2_ir(OP_return);
                    memcpy(flatten_ir, insn, sizeof(ph2_ir_t));
                    flatten_ir->src1 = bb->belong_to->func->stack_size;
                    elf_offset += 24;
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
         ph2_ir = ph2_ir->next) {
        int rd = ph2_ir->dest;
        int rn = ph2_ir->src0;
        int rm = ph2_ir->src1;

        switch (ph2_ir->op) {
        case OP_load_constant:
            emit(__mov_i(__AL, rd, ph2_ir->src0));
            break;
        case OP_global_address_of:
            emit(__add_i(__AL, rd, __r12, ph2_ir->src0));
            break;
        case OP_assign:
            emit(__mov_r(__AL, rd, rn));
            break;
        case OP_global_store:
            emit(__sw(__AL, rn, __r12, ph2_ir->src1));
            break;
        default:
            printf("Unknown opcode\n");
            abort();
        }
    }
    /* jump to main */
    emit(__b(__AL, MAIN_BB->elf_offset - elf_code_idx));

    int i;
    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir_t *ph2_ir = &PH2_IR[i];
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
            break;
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
            break;
        case OP_load_data_address:
            emit(__movw(__AL, rd, ph2_ir->src0 + elf_data_start));
            emit(__movt(__AL, rd, ph2_ir->src0 + elf_data_start));
            break;
        case OP_address_of:
            if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, rd, __sp, __r8));
            } else
                emit(__add_i(__AL, rd, __sp, ph2_ir->src0));
            break;
        case OP_assign:
            emit(__mov_r(__AL, rd, rn));
            break;
        case OP_load:
            if (ph2_ir->src0 > 4095) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__lw(__AL, rd, __r8, 0));
            } else
                emit(__lw(__AL, rd, __sp, ph2_ir->src0));
            break;
        case OP_store:
            if (ph2_ir->src1 > 4095) {
                emit(__movw(__AL, __r8, ph2_ir->src1));
                emit(__movt(__AL, __r8, ph2_ir->src1));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__sw(__AL, rn, __r8, 0));
            } else
                emit(__sw(__AL, rn, __sp, ph2_ir->src1));
            break;
        case OP_global_load:
            if (ph2_ir->src0 > 4095) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __r12, __r8));
                emit(__lw(__AL, rd, __r8, 0));
            } else
                emit(__lw(__AL, rd, __r12, ph2_ir->src0));
            break;
        case OP_global_store:
            if (ph2_ir->src1 > 4095) {
                emit(__movw(__AL, __r8, ph2_ir->src1));
                emit(__movt(__AL, __r8, ph2_ir->src1));
                emit(__add_r(__AL, __r8, __r12, __r8));
                emit(__sw(__AL, rn, __r8, 0));
            } else
                emit(__sw(__AL, rn, __r12, ph2_ir->src1));
            break;
        case OP_read:
            if (ph2_ir->src1 == 1)
                emit(__lb(__AL, rd, rn, 0));
            else if (ph2_ir->src1 == 4)
                emit(__lw(__AL, rd, rn, 0));
            else
                abort();
            break;
        case OP_write:
            if (ph2_ir->dest == 1)
                emit(__sb(__AL, rm, rn, 0));
            else if (ph2_ir->dest == 4)
                emit(__sw(__AL, rm, rn, 0));
            else
                abort();
            break;
        case OP_branch:
            ofs = ph2_ir->else_bb->elf_offset;
            emit(__movw(__AL, __r8, ofs + elf_code_start));
            emit(__movt(__AL, __r8, ofs + elf_code_start));
            emit(__teq(rn));
            emit(__b(__NE, 8));
            emit(__blx(__AL, __r8));

            ofs = ph2_ir->then_bb->elf_offset;
            emit(__b(__AL, ofs - elf_code_idx));
            break;
        case OP_jump:
            emit(__b(__AL, ph2_ir->next_bb->elf_offset - elf_code_idx));
            break;
        case OP_call:
            emit(__bl(__AL, find_func(ph2_ir->func_name)->fn->bbs->elf_offset -
                                elf_code_idx));
            break;
        case OP_address_of_func:
            ofs = elf_code_start +
                  find_func(ph2_ir->func_name)->fn->bbs->elf_offset;
            emit(__movw(__AL, __r8, ofs));
            emit(__movt(__AL, __r8, ofs));
            emit(__sw(__AL, __r8, rn, 0));
            break;
        case OP_load_func:
            emit(__mov_r(__AL, __r8, rn));
            break;
        case OP_indirect:
            emit(__blx(__AL, __r8));
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                emit(__mov_r(__AL, __r0, __r0));
            else
                emit(__mov_r(__AL, __r0, rn));
            emit(__movw(__AL, __r8, ph2_ir->src1 + 4));
            emit(__movt(__AL, __r8, ph2_ir->src1 + 4));
            emit(__add_r(__AL, __sp, __sp, __r8));
            emit(__lw(__AL, __lr, __sp, -4));
            emit(__mov_r(__AL, __pc, __lr));
            break;
        case OP_add:
            emit(__add_r(__AL, rd, rn, rm));
            break;
        case OP_sub:
            emit(__sub_r(__AL, rd, rn, rm));
            break;
        case OP_mul:
            emit(__mul(__AL, rd, rn, rm));
            break;
        case OP_div:
            emit(__div(__AL, rd, rm, rn));
            break;
        case OP_mod:
            emit(__div(__AL, __r8, rm, rn));
            emit(__mul(__AL, __r8, rm, __r8));
            emit(__sub_r(__AL, rd, rn, __r8));
            break;
        case OP_lshift:
            emit(__sll(__AL, rd, rn, rm));
            break;
        case OP_rshift:
            emit(__srl(__AL, rd, rn, rm));
            break;
        case OP_eq:
        case OP_neq:
        case OP_gt:
        case OP_lt:
        case OP_geq:
        case OP_leq:
            emit(__cmp_r(__AL, rn, rm));
            emit(__zero(rd));
            emit(__mov_i(arm_get_cond(ph2_ir->op), rd, 1));
            break;
        case OP_negate:
            emit(__rsb_i(__AL, rd, 0, rn));
            break;
        case OP_bit_not:
            emit(__mvn_r(__AL, rd, rn));
            break;
        case OP_bit_and:
            emit(__and_r(__AL, rd, rn, rm));
            break;
        case OP_bit_or:
            emit(__or_r(__AL, rd, rn, rm));
            break;
        case OP_bit_xor:
            emit(__eor_r(__AL, rd, rn, rm));
            break;
        case OP_log_not:
            emit(__teq(rn));
            emit(__mov_i(__NE, rd, 0));
            emit(__mov_i(__EQ, rd, 1));
            break;
        case OP_log_and:
            /* FIXME: bad logical-and instruction */
            emit(__and_r(__AL, rd, rn, rm));
            break;
        case OP_log_or:
            emit(__or_r(__AL, rd, rn, rm));
            emit(__teq(rd));
            emit(__mov_i(__NE, rd, 1));
            break;
        default:
            printf("Unknown opcode\n");
            abort();
        }
    }
}
