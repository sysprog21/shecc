/* Translate IR to target machine code */

#include "arm.c"

void emit(int code)
{
    elf_write_code_int(code);
}

void code_generate()
{
    ph2_ir_t *ph2_ir;
    func_t *fn;
    int i, ofs, global_stack_size, elf_data_start;
    int rd, rn, rm;
    int block_lv = 0;

    add_label("__syscall", 44);

    /* calculate the offset of labels */
    elf_code_idx = 96;

    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];

        switch (ph2_ir->op) {
        case OP_define:
            fn = find_func(ph2_ir->func_name);
            add_label(ph2_ir->func_name, elf_code_idx);
            elf_code_idx += 16;
            break;
        case OP_block_start:
            block_lv++;
            break;
        case OP_block_end:
            /* handle the function with the implicit return */
            --block_lv;
            if (block_lv != 0)
                break;
            if (!strcmp(fn->return_def.type_name, "void"))
                elf_code_idx += 20;
            break;
        case OP_load:
        case OP_load_func:
        case OP_global_load:
        case OP_global_load_func:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255)
                elf_code_idx += 16;
            else
                elf_code_idx += 4;
            break;
        case OP_store:
        case OP_global_store:
            if (ph2_ir->src1 < 0)
                abort();
            else if (ph2_ir->src1 > 255)
                elf_code_idx += 16;
            else
                elf_code_idx += 4;
            break;
        case OP_address_of:
        case OP_global_address_of:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255)
                elf_code_idx += 12;
            else
                elf_code_idx += 4;
            break;
        case OP_label:
            add_label(ph2_ir->func_name, elf_code_idx);
            break;
        case OP_jump:
            if (!strcmp(ph2_ir->func_name, "main"))
                elf_code_idx += 24;
            else
                elf_code_idx += 4;
            break;
        case OP_load_constant:
            if (ph2_ir->src0 < 0)
                elf_code_idx += 12;
            else if (ph2_ir->src0 > 255)
                elf_code_idx += 8;
            else
                elf_code_idx += 4;
            break;
        case OP_assign:
            if (ph2_ir->dest != ph2_ir->src0)
                elf_code_idx += 4;
            break;
        case OP_call:
        case OP_read:
        case OP_write:
        case OP_negate:
        case OP_add:
        case OP_sub:
        case OP_mul:
        case OP_div:
        case OP_bit_and:
        case OP_bit_or:
        case OP_bit_xor:
        case OP_bit_not:
        case OP_rshift:
        case OP_lshift:
        case OP_indirect:
            elf_code_idx += 4;
            break;
        case OP_load_data_address:
            elf_code_idx += 8;
            break;
        case OP_mod:
        case OP_eq:
        case OP_neq:
        case OP_gt:
        case OP_lt:
        case OP_geq:
        case OP_leq:
        case OP_log_or:
        case OP_log_not:
        case OP_address_of_func:
            elf_code_idx += 12;
            break;
        case OP_branch:
        case OP_return:
            elf_code_idx += 24;
            break;
        case OP_log_and:
            elf_code_idx += 28;
            break;
        default:
            break;
        }
    }

    elf_data_start = elf_code_start + elf_code_idx;
    global_stack_size = FUNCS[0].stack_size;
    block_lv = 0;
    elf_code_idx = 0;

    /* insert entry, exit point and syscall manually */
    elf_add_symbol("__start", strlen("__start"), elf_code_idx);
    emit(__movw(__AL, __r8, global_stack_size));
    emit(__movt(__AL, __r8, global_stack_size));
    emit(__sub_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r12, __sp));
    emit(__bl(__AL, 96 - elf_code_idx));

    elf_add_symbol("__exit", strlen("__exit"), elf_code_idx);
    emit(__movw(__AL, __r8, global_stack_size));
    emit(__movt(__AL, __r8, global_stack_size));
    emit(__add_r(__AL, __sp, __sp, __r8));
    emit(__mov_r(__AL, __r0, __r0));
    emit(__mov_i(__AL, __r7, 1));
    emit(__svc());

    elf_add_symbol("__syscall", strlen("__syscall"), elf_code_idx);
    emit(__sw(__AL, __lr, __sp, -4));
    emit(__add_i(__AL, __sp, __sp, -4));
    emit(__mov_r(__AL, __r7, __r0));
    emit(__mov_r(__AL, __r0, __r1));
    emit(__mov_r(__AL, __r1, __r2));
    emit(__mov_r(__AL, __r2, __r3));
    emit(__mov_r(__AL, __r3, __r4));
    emit(__mov_r(__AL, __r4, __r5));
    emit(__mov_r(__AL, __r5, __r6));
    emit(__svc());
    emit(__add_i(__AL, __sp, __sp, 4));
    emit(__lw(__AL, __lr, __sp, -4));
    emit(__mov_r(__AL, __pc, __lr));

    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];

        rd = ph2_ir->dest;
        rn = ph2_ir->src0;
        rm = ph2_ir->src1;

        switch (ph2_ir->op) {
        case OP_define:
            fn = find_func(ph2_ir->func_name);
            emit(__sw(__AL, __lr, __sp, -4));
            emit(__movw(__AL, __r8, fn->stack_size + 4));
            emit(__movt(__AL, __r8, fn->stack_size + 4));
            emit(__sub_r(__AL, __sp, __sp, __r8));
            break;
        case OP_block_start:
            block_lv++;
            break;
        case OP_block_end:
            --block_lv;
            if (block_lv != 0)
                break;
            if (!strcmp(fn->return_def.type_name, "void")) {
                emit(__movw(__AL, __r8, fn->stack_size + 4));
                emit(__movt(__AL, __r8, fn->stack_size + 4));
                emit(__add_r(__AL, __sp, __sp, __r8));
                emit(__lw(__AL, __lr, __sp, -4));
                emit(__mov_r(__AL, __pc, __lr));
            }
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
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, rd, __sp, __r8));
            } else
                emit(__add_i(__AL, rd, __sp, ph2_ir->src0));
            break;
        case OP_global_address_of:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, rd, __r12, __r8));
            } else
                emit(__add_i(__AL, rd, __r12, ph2_ir->src0));
            break;
        case OP_assign:
            if (rd != rn)
                emit(__mov_r(__AL, rd, rn));
            break;
        case OP_label:
            break;
        case OP_branch:
            /* calculate the absolute address for jumping */
            ofs = find_label_offset(ph2_ir->false_label);
            emit(__movw(__AL, __r8, ofs + elf_code_start));
            emit(__movt(__AL, __r8, ofs + elf_code_start));
            emit(__teq(rn));
            emit(__b(__NE, 8));
            emit(__blx(__AL, __r8));

            ofs = find_label_offset(ph2_ir->true_label);
            emit(__b(__AL, ofs - elf_code_idx));
            break;
        case OP_jump:
            if (!strcmp(ph2_ir->func_name, "main")) {
                emit(__movw(__AL, __r8, global_stack_size));
                emit(__movt(__AL, __r8, global_stack_size));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__lw(__AL, __r0, __r8, 0));
                emit(__add_i(__AL, __r1, __r8, 4));
            }

            ofs = find_label_offset(ph2_ir->func_name);
            emit(__b(__AL, ofs - elf_code_idx));
            break;
        case OP_call:
            ofs = find_label_offset(ph2_ir->func_name);
            emit(__bl(__AL, ofs - elf_code_idx));
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                emit(__mov_r(__AL, __r0, __r0));
            else
                emit(__mov_r(__AL, __r0, rn));
            emit(__movw(__AL, __r8, fn->stack_size + 4));
            emit(__movt(__AL, __r8, fn->stack_size + 4));
            emit(__add_r(__AL, __sp, __sp, __r8));
            emit(__lw(__AL, __lr, __sp, -4));
            emit(__mov_r(__AL, __pc, __lr));
            break;
        case OP_load:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__lw(__AL, rd, __r8, 0));
            } else
                emit(__lw(__AL, rd, __sp, ph2_ir->src0));
            break;
        case OP_store:
            if (ph2_ir->src1 < 0)
                abort();
            else if (ph2_ir->src1 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src1));
                emit(__movt(__AL, __r8, ph2_ir->src1));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__sw(__AL, rn, __r8, 0));
            } else
                emit(__sw(__AL, rn, __sp, ph2_ir->src1));
            break;
        case OP_load_func:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __sp, __r8));
                emit(__lw(__AL, __r8, __r8, 0));
            } else
                emit(__lw(__AL, __r8, __sp, ph2_ir->src0));
            break;
        case OP_global_load:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __r12, __r8));
                emit(__lw(__AL, rd, __r8, 0));
            } else
                emit(__lw(__AL, rd, __r12, ph2_ir->src0));
            break;
        case OP_global_store:
            if (ph2_ir->src1 < 0)
                abort();
            else if (ph2_ir->src1 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src1));
                emit(__movt(__AL, __r8, ph2_ir->src1));
                emit(__add_r(__AL, __r8, __r12, __r8));
                emit(__sw(__AL, rn, __r8, 0));
            } else
                emit(__sw(__AL, rn, __r12, ph2_ir->src1));
            break;
        case OP_global_load_func:
            if (ph2_ir->src0 < 0)
                abort();
            else if (ph2_ir->src0 > 255) {
                emit(__movw(__AL, __r8, ph2_ir->src0));
                emit(__movt(__AL, __r8, ph2_ir->src0));
                emit(__add_r(__AL, __r8, __r12, __r8));
                emit(__lw(__AL, __r8, __r8, 0));
            } else
                emit(__lw(__AL, __r8, __r12, ph2_ir->src0));
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
                emit(__sb(__AL, rn, rm, 0));
            else if (ph2_ir->dest == 4)
                emit(__sw(__AL, rn, rm, 0));
            else
                abort();
            break;
        case OP_address_of_func:
            /* calculate the absolute address for jumping */
            ofs = find_label_offset(ph2_ir->func_name);
            emit(__movw(__AL, __r8, ofs + elf_code_start));
            emit(__movt(__AL, __r8, ofs + elf_code_start));
            emit(__sw(__AL, __r8, rn, 0));
            break;
        case OP_indirect:
            emit(__blx(__AL, __r8));
            break;
        case OP_negate:
            emit(__rsb_i(__AL, rd, 0, rn));
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
        case OP_eq:
        case OP_neq:
        case OP_lt:
        case OP_leq:
        case OP_gt:
        case OP_geq:
            emit(__cmp_r(__AL, rn, rm));
            emit(__zero(rd));
            emit(__mov_i(arm_get_cond(ph2_ir->op), rd, 1));
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
        case OP_bit_not:
            emit(__mvn_r(__AL, rd, rn));
            break;
        case OP_log_and:
            emit(__teq(rn));
            emit(__mov_i(__NE, __r8, 1));
            emit(__mov_i(__EQ, __r8, 0));
            emit(__teq(rm));
            emit(__mov_i(__NE, rd, 1));
            emit(__mov_i(__EQ, rd, 0));
            emit(__and_r(__AL, rd, __r8, rd));
            break;
        case OP_log_or:
            emit(__or_r(__AL, rd, rn, rm));
            emit(__teq(rd));
            emit(__mov_i(__NE, rd, 1));
            break;
        case OP_log_not:
            emit(__teq(rn));
            emit(__mov_i(__NE, rd, 0));
            emit(__mov_i(__EQ, rd, 1));
            break;
        case OP_rshift:
            emit(__srl(__AL, rd, rn, rm));
            break;
        case OP_lshift:
            emit(__sll(__AL, rd, rn, rm));
            break;
        default:
            abort();
            break;
        }
    }
}
