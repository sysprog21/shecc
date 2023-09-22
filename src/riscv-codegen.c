/* Translate IR to target machine code */

#include "riscv.c"

void emit(int code)
{
    elf_write_code_int(code);
}

void code_generate()
{
    ph2_ir_t *ph2_ir;
    func_t *fn;
    int i, ofs, global_stack_size, elf_data_start;
    int rd, rs1, rs2;
    int block_lv = 0;

    add_label("__syscall", 44);

    /* calculate offset of labels */
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
            /* handle function with implicit return */
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
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
                elf_code_idx += 16;
            else
                elf_code_idx += 4;
            break;
        case OP_store:
        case OP_global_store:
            if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047)
                elf_code_idx += 16;
            else
                elf_code_idx += 4;
            break;
        case OP_address_of:
        case OP_global_address_of:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
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
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047)
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
        case OP_mod:
        case OP_gt:
        case OP_lt:
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
        case OP_neq:
        case OP_geq:
        case OP_leq:
        case OP_log_or:
        case OP_log_not:
            elf_code_idx += 8;
            break;
        case OP_eq:
        case OP_address_of_func:
            elf_code_idx += 12;
            break;
        case OP_log_and:
            elf_code_idx += 16;
            break;
        case OP_branch:
            elf_code_idx += 20;
            break;
        case OP_return:
            elf_code_idx += 24;
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
    emit(__lui(__t0, rv_hi(global_stack_size)));
    emit(__addi(__t0, __t0, rv_lo(global_stack_size)));
    emit(__sub(__sp, __sp, __t0));
    emit(__addi(__gp, __sp, 0));
    emit(__jal(__ra, 96 - elf_code_idx));

    elf_add_symbol("__exit", strlen("__exit"), elf_code_idx);
    emit(__lui(__t0, rv_hi(global_stack_size)));
    emit(__addi(__t0, __t0, rv_lo(global_stack_size)));
    emit(__add(__sp, __sp, __t0));
    emit(__addi(__a0, __a0, 0));
    emit(__addi(__a7, __zero, 93));
    emit(__ecall());

    elf_add_symbol("__syscall", strlen("__syscall"), elf_code_idx);
    emit(__addi(__sp, __sp, -4));
    emit(__sw(__ra, __sp, 0));
    emit(__addi(__a7, __a0, 0));
    emit(__addi(__a0, __a1, 0));
    emit(__addi(__a1, __a2, 0));
    emit(__addi(__a2, __a3, 0));
    emit(__addi(__a3, __a4, 0));
    emit(__addi(__a4, __a5, 0));
    emit(__addi(__a5, __a6, 0));
    emit(__ecall());
    emit(__lw(__ra, __sp, 0));
    emit(__addi(__sp, __sp, 4));
    emit(__jalr(__zero, __ra, 0));

    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];

        rd = ph2_ir->dest + 10;
        rs1 = ph2_ir->src0 + 10;
        rs2 = ph2_ir->src1 + 10;

        switch (ph2_ir->op) {
        case OP_define:
            fn = find_func(ph2_ir->func_name);
            emit(__sw(__ra, __sp, -4));
            emit(__lui(__t0, rv_hi(fn->stack_size + 4)));
            emit(__addi(__t0, __t0, rv_lo(fn->stack_size + 4)));
            emit(__sub(__sp, __sp, __t0));
            break;
        case OP_block_start:
            block_lv++;
            break;
        case OP_block_end:
            --block_lv;
            if (block_lv != 0)
                break;
            if (!strcmp(fn->return_def.type_name, "void")) {
                emit(__lui(__t0, rv_hi(fn->stack_size + 4)));
                emit(__addi(__t0, __t0, rv_lo(fn->stack_size + 4)));
                emit(__add(__sp, __sp, __t0));
                emit(__lw(__ra, __sp, -4));
                emit(__jalr(__zero, __ra, 0));
            }
            break;
        case OP_load_constant:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(rd, rv_hi(ph2_ir->src0)));
                emit(__addi(rd, rd, rv_lo(ph2_ir->src0)));
            } else
                emit(__addi(rd, __zero, ph2_ir->src0));
            break;
        case OP_load_data_address:
            emit(__lui(rd, rv_hi(ph2_ir->src0 + elf_data_start)));
            emit(__addi(rd, rd, rv_lo(ph2_ir->src0 + elf_data_start)));
            break;
        case OP_address_of:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(rd, __sp, __t0));
            } else
                emit(__addi(rd, __sp, ph2_ir->src0));
            break;
        case OP_global_address_of:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(rd, __gp, __t0));
            } else
                emit(__addi(rd, __gp, ph2_ir->src0));
            break;
        case OP_assign:
            if (ph2_ir->dest != ph2_ir->src0)
                emit(__addi(rd, rs1, 0));
            break;
        case OP_label:
            break;
        case OP_branch:
            /* calculate the absolute address for jumping */
            ofs = find_label_offset(ph2_ir->false_label);
            emit(__lui(__t0, rv_hi(ofs + elf_code_start)));
            emit(__addi(__t0, __t0, rv_lo(ofs + elf_code_start)));
            emit(__bne(rs1, __zero, 8));
            emit(__jalr(__zero, __t0, 0));

            ofs = find_label_offset(ph2_ir->true_label);
            emit(__jal(__zero, ofs - elf_code_idx));
            break;
        case OP_jump:
            if (!strcmp(ph2_ir->func_name, "main")) {
                emit(__lui(__t0, rv_hi(global_stack_size)));
                emit(__addi(__t0, __t0, rv_lo(global_stack_size)));
                emit(__add(__t0, __sp, __t0));
                emit(__lw(__a0, __t0, 0));
                emit(__addi(__a1, __t0, 4));
            }

            ofs = find_label_offset(ph2_ir->func_name);
            emit(__jal(__zero, ofs - elf_code_idx));
            break;
        case OP_call:
            ofs = find_label_offset(ph2_ir->func_name);
            emit(__jal(__ra, ofs - elf_code_idx));
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                emit(__addi(__zero, __zero, 0));
            else
                emit(__addi(__a0, rs1, 0));
            emit(__lui(__t0, rv_hi(fn->stack_size + 4)));
            emit(__addi(__t0, __t0, rv_lo(fn->stack_size + 4)));
            emit(__add(__sp, __sp, __t0));
            emit(__lw(__ra, __sp, -4));
            emit(__jalr(__zero, __ra, 0));
            break;
        case OP_load:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(__t0, __sp, __t0));
                emit(__lw(rd, __t0, 0));
            } else
                emit(__lw(rd, __sp, ph2_ir->src0));
            break;
        case OP_store:
            if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src1)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src1)));
                emit(__add(__t0, __sp, __t0));
                emit(__sw(rs1, __t0, 0));
            } else
                emit(__sw(rs1, __sp, ph2_ir->src1));
            break;
        case OP_load_func:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(__t0, __sp, __t0));
                emit(__lw(__t0, __t0, 0));
            } else
                emit(__lw(__t0, __sp, ph2_ir->src0));
            break;
        case OP_global_load:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(__t0, __gp, __t0));
                emit(__lw(rd, __t0, 0));
            } else
                emit(__lw(rd, __gp, ph2_ir->src0));
            break;
        case OP_global_store:
            if (ph2_ir->src1 < -2048 || ph2_ir->src1 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src1)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src1)));
                emit(__add(__t0, __gp, __t0));
                emit(__sw(rs1, __t0, 0));
            } else
                emit(__sw(rs1, __gp, ph2_ir->src1));
            break;
        case OP_global_load_func:
            if (ph2_ir->src0 < -2048 || ph2_ir->src0 > 2047) {
                emit(__lui(__t0, rv_hi(ph2_ir->src0)));
                emit(__addi(__t0, __t0, rv_lo(ph2_ir->src0)));
                emit(__add(__t0, __gp, __t0));
                emit(__lw(__t0, __t0, 0));
            } else
                emit(__lw(__t0, __gp, ph2_ir->src0));
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
                emit(__sb(rs1, rs2, 0));
            else if (ph2_ir->dest == 4)
                emit(__sw(rs1, rs2, 0));
            else
                abort();
            break;
        case OP_address_of_func:
            /* calculate the absolute address for jumping */
            ofs = find_label_offset(ph2_ir->func_name);
            emit(__lui(__t0, rv_hi(ofs + elf_code_start)));
            emit(__addi(__t0, __t0, rv_lo(ofs + elf_code_start)));
            emit(__sw(__t0, rs1, 0));
            break;
        case OP_indirect:
            emit(__jalr(__ra, __t0, 0));
            break;
        case OP_negate:
            emit(__sub(rd, __zero, rs1));
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
        case OP_lt:
            emit(__slt(rd, rs1, rs2));
            break;
        case OP_geq:
            emit(__slt(rd, rs1, rs2));
            emit(__xori(rd, rd, 1));
            break;
        case OP_leq:
            emit(__slt(rd, rs2, rs1));
            emit(__xori(rd, rd, 1));
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
        case OP_bit_not:
            emit(__xori(rd, rs1, -1));
            break;
        case OP_log_and:
            emit(__sltu(__t0, __zero, rs1));
            emit(__sub(__t0, __zero, __t0));
            emit(__and(__t0, __t0, rs2));
            emit(__sltu(rd, __zero, __t0));
            break;
        case OP_log_or:
            emit(__or(rd, rs1, rs2));
            emit(__sltu(rd, __zero, rd));
            break;
        case OP_log_not:
            emit(__sltu(rd, __zero, rs1));
            emit(__xori(rd, rd, 1));
            break;
        case OP_rshift:
            emit(__sra(rd, rs1, rs2));
            break;
        case OP_lshift:
            emit(__sll(rd, rs1, rs2));
            break;
        default:
            abort();
            break;
        }
    }
}
