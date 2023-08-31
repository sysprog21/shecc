/* Translate IR to target machine code */

#include "arm.c"

/* Compute stack space needed for function's parameters */
void size_func(func_t *fn)
{
    int s = 0, i;

    /* parameters are turned into local variables */
    for (i = 0; i < fn->num_params; i++) {
        s += size_var(&fn->param_defs[i]);
        fn->param_defs[i].offset = s; /* stack offset */
    }

    /* align to 16 bytes */
    if ((s & 15) > 0)
        s = (s - (s & 15)) + 16;
    if (s > 2047)
        error("Local stack size exceeded");

    fn->params_size = s;
}

/* Return stack size required after block local variables */
int size_block(block_t *blk)
{
    int size = 0, i, offset;

    /* our offset starts from parent's offset */
    if (!blk->parent)
        offset = blk->func ? blk->func->params_size : 0;
    else
        offset = size_block(blk->parent);

    /* declared locals */
    for (i = 0; i < blk->next_local; i++) {
        int vs = size_var(&blk->locals[i]);
        /* look up value off stack */
        blk->locals[i].offset = size + offset + vs;
        size += vs;
    }

    /* align to 16 bytes */
    if ((size & 15) > 0)
        size = (size - (size & 15)) + 16;
    if (size > 2047)
        error("Local stack size exceeded");

    /* save in block for stack allocation */
    blk->locals_size = size;
    return size + offset;
}

/* Compute stack necessary sizes for all functions */
void size_funcs(int data_start)
{
    block_t *blk;
    int i;

    /* size functions */
    for (i = 0; i < funcs_idx; i++)
        size_func(&FUNCS[i]);

    /* size blocks excl. global block */
    for (i = 1; i < blocks_idx; i++)
        size_block(&BLOCKS[i]);

    /* allocate data for globals, in block 0 */
    blk = &BLOCKS[0];
    for (i = 0; i < blk->next_local; i++) {
        blk->locals[i].offset = elf_data_idx; /* set offset in data section */
        elf_add_symbol(blk->locals[i].var_name, strlen(blk->locals[i].var_name),
                       data_start + elf_data_idx);
        /* TODO: add .bss section */
        if (!strcmp(blk->locals[i].type_name, "int") &&
            blk->locals[i].init_val != 0)
            elf_write_data_int(blk->locals[i].init_val);
        else
            elf_data_idx += size_var(&blk->locals[i]);
    }
}

/* Return expected binary length of an IR instruction in bytes */
int get_code_length(ir_instr_t *ii)
{
    opcode_t op = ii->op;

    switch (op) {
    case OP_func_extry: {
        func_t *fn = find_func(ii->str_param1);
        return 16 + (fn->num_params << 2);
    }
    case OP_call:
    case OP_indirect:
        return ii->param_no ? 8 : 4;
    case OP_load_constant:
        return (ii->int_param1 >= 0 && ii->int_param1 < 256) ? 4 : 8;
    case OP_block_start:
    case OP_block_end: {
        block_t *blk = &BLOCKS[ii->int_param1];
        return (blk->next_local > 0) ? 4 : 0;
    }
    case OP_eq:
    case OP_neq:
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return 12;
    case OP_syscall:
        return 32;
    case OP_func_exit:
        return 16;
    case OP_exit:
    case OP_mod:
    case OP_log_not:
        return 12;
    case OP_load_data_address:
    case OP_jz:
    case OP_jnz:
    case OP_push:
    case OP_pop:
    case OP_address_of:
    case OP_start:
        return 8;
    case OP_jump:
    case OP_return:
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_div:
    case OP_read:
    case OP_write:
    case OP_log_or:
    case OP_log_and:
    case OP_bit_or:
    case OP_bit_and:
    case OP_bit_xor:
    case OP_bit_not:
    case OP_negate:
    case OP_lshift:
    case OP_rshift:
        return 4;
    case OP_label:
        return 0;
    default:
        error("Unsupported IR opcode");
    }
    return 0;
}

void emit(int code)
{
    elf_write_code_int(code);
}

/* Compute total binary code length based on IR opcode */
int total_code_length()
{
    int code_len = 0, i;
    for (i = 0; i < ir_idx; i++) {
        IR[i].code_offset = code_len;
        IR[i].op_len = get_code_length(&IR[i]);
        code_len += IR[i].op_len;
    }
    return code_len;
}

void code_generate()
{
    int stack_size = 0, i;
    block_t *blk = NULL;
    int _c_block_level = 0;

    int code_start = elf_code_start; /* ELF headers size */
    int data_start = total_code_length();
    size_funcs(code_start + data_start);

    for (i = 0; i < ir_idx; i++) {
        var_t *var;
        func_t *fn;

        ir_instr_t *ii = &IR[i];
        opcode_t op = ii->op;
        int pc = elf_code_idx;
        int ofs, val;
        int dest_reg = ii->param_no;
        int OP_reg = ii->int_param1;

        switch (op) {
        case OP_load_data_address:
            /* lookup address of a constant in data section */
            ofs = data_start + ii->int_param1;
            ofs += code_start;
            emit(__movw(__AL, dest_reg, ofs));
            emit(__movt(__AL, dest_reg, ofs));
            DUMP_IR("    x%d := &data[%d]", dest_reg, ii->int_param1);
            break;
        case OP_load_constant:
            /* load numeric constant */
            val = ii->int_param1;
            if (val >= 0 && val < 256) {
                emit(__mov_i(__AL, dest_reg, val));
            } else {
                emit(__movw(__AL, dest_reg, val));
                emit(__movt(__AL, dest_reg, val));
            }
            DUMP_IR("    x%d := %d", dest_reg, ii->int_param1);
            break;
        case OP_address_of:
            /* lookup address of a variable */
            var = find_global_var(ii->str_param1);
            if (var) {
                ofs = data_start + var->offset;
                /* need to find the variable offset in data section, absolute */
                ofs += code_start;

                emit(__movw(__AL, dest_reg, ofs));
                emit(__movt(__AL, dest_reg, ofs));
            } else {
                /* need to find the variable offset on stack, i.e. from r11 */
                var = find_local_var(ii->str_param1, blk);
                if (var) {
                    int offset = -var->offset;
                    emit(__add_i(__AL, dest_reg, __r11, offset & 255));
                    emit(__add_i(__AL, dest_reg, dest_reg,
                                 offset - (offset & 255)));
                } else {
                    /* is it function address? */
                    fn = find_func(ii->str_param1);
                    if (fn) {
                        int jump_instr_index = fn->entry_point;
                        ir_instr_t *jump_instr = &IR[jump_instr_index];
                        /* load code offset into variable */
                        ofs = code_start + jump_instr->code_offset;
                        emit(__movw(__AL, dest_reg, ofs));
                        emit(__movt(__AL, dest_reg, ofs));
                    } else
                        error("Undefined identifier");
                }
            }
            DUMP_IR("    x%d = &%s", dest_reg, ii->str_param1);
            break;
        case OP_read:
            /* read (dereference) memory address */
            switch (ii->int_param2) {
            case 4:
                emit(__lw(__AL, dest_reg, OP_reg, 0));
                break;
            case 1:
                emit(__lb(__AL, dest_reg, OP_reg, 0));
                break;
            default:
                error("Unsupported word size");
            }
            DUMP_IR("    x%d = *x%d (%d)", dest_reg, OP_reg, ii->int_param2);
            break;
        case OP_write:
            /* write at memory address */
            switch (ii->int_param2) {
            case 4:
                emit(__sw(__AL, dest_reg, OP_reg, 0));
                break;
            case 1:
                emit(__sb(__AL, dest_reg, OP_reg, 0));
                break;
            default:
                error("Unsupported word size");
            }
            DUMP_IR("    *x%d = x%d (%d)", OP_reg, dest_reg, ii->int_param2);
            break;
        case OP_jump: {
            /* unconditional jump to an IR-index */
            int jump_instr_index = ii->int_param1;
            ir_instr_t *jump_instr = &IR[jump_instr_index];
            int jump_location = jump_instr->code_offset;
            ofs = jump_location - pc;

            emit(__b(__AL, ofs));
            DUMP_IR("    goto %d", ii->int_param1);
        } break;
        case OP_return: {
            /* jump to function exit */
            func_t *fd = find_func(ii->str_param1);
            int jump_instr_index = fd->exit_point;
            ir_instr_t *jump_instr = &IR[jump_instr_index];
            int jump_location = jump_instr->code_offset;
            ofs = jump_location - pc;

            emit(__b(__AL, ofs));
            DUMP_IR("    return (from %s)", ii->str_param1);
        } break;
        case OP_call: {
            /* function call */
            int jump_instr_index;
            ir_instr_t *jump_instr;
            int jump_location;

            /* need to find offset */
            fn = find_func(ii->str_param1);
            jump_instr_index = fn->entry_point;
            jump_instr = &IR[jump_instr_index];
            jump_location = jump_instr->code_offset;
            ofs = jump_location - pc;

            emit(__bl(__AL, ofs));
            if (dest_reg != __r0)
                emit(__mov_r(__AL, dest_reg, __r0));
            DUMP_IR("    x%d := %s() @ %d", dest_reg, ii->str_param1,
                    fn->entry_point);
        } break;
        case OP_indirect:
            /* indirect call with function pointer.
             * address in OP_reg, result in dest_reg
             */
            emit(__blx(__AL, OP_reg));
            if (dest_reg != __r0)
                emit(__mov_r(__AL, dest_reg, __r0));
            DUMP_IR("    x%d := x%d()", dest_reg, OP_reg);
            break;
        case OP_push:
            /* 16 aligned although we only need 4 */
            emit(__add_i(__AL, __sp, __sp, -16));
            emit(__sw(__AL, dest_reg, __sp, 0));
            DUMP_IR("    push x%d", dest_reg);
            break;
        case OP_pop:
            emit(__lw(__AL, dest_reg, __sp, 0));
            /* 16 aligned although we only need 4 */
            emit(__add_i(__AL, __sp, __sp, 16));
            DUMP_IR("    pop x%d", dest_reg);
            break;
        case OP_func_exit:
            /* restore previous frame */
            emit(__add_i(__AL, __sp, __r11, 16));
            emit(__lw(__AL, __lr, __sp, -8));
            emit(__lw(__AL, __r11, __sp, -4));
            emit(__mov_r(__AL, __pc, __lr));
            fn = NULL;
            DUMP_IR("    exit %s", ii->str_param1);
            break;
        case OP_add:
            emit(__add_r(__AL, dest_reg, dest_reg, OP_reg));

            DUMP_IR("    x%d += x%d", dest_reg, OP_reg);
            break;
        case OP_sub:
            emit(__sub_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d -= x%d", dest_reg, OP_reg);
            break;
        case OP_mul:
            emit(__mul(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d *= x%d", dest_reg, OP_reg);
            break;
        case OP_div:
            emit(__div(__AL, dest_reg, OP_reg, dest_reg));
            DUMP_IR("    x%d /= x%d", dest_reg, OP_reg);
            break;
        case OP_mod:
            emit(__div(__AL, OP_reg + 1, OP_reg, dest_reg));
            emit(__mul(__AL, OP_reg, OP_reg, OP_reg + 1));
            emit(__sub_r(__AL, dest_reg, dest_reg, OP_reg));
            /* TODO: support percent-sign character (%) in format string */
            DUMP_IR("    x%d = x%d mod x%d", dest_reg, dest_reg, OP_reg);
            break;
        case OP_negate:
            emit(__rsb_i(__AL, dest_reg, 0, dest_reg));
            DUMP_IR("    -x%d", dest_reg);
            break;
        case OP_label:
            if (ii->str_param1)
                /* TODO: lazy evaluation */
                if (strlen(ii->str_param1) > 0)
                    elf_add_symbol(ii->str_param1, strlen(ii->str_param1),
                                   code_start + pc);
            DUMP_IR("%4d:", i);
            break;
        case OP_eq:
        case OP_neq:
        case OP_lt:
        case OP_leq:
        case OP_gt:
        case OP_geq:
            /* we want 1/nonzero if equ, 0 otherwise */
            emit(__cmp_r(__AL, dest_reg, OP_reg));
            emit(__zero(dest_reg));
            emit(__mov_i(arm_get_cond(op), dest_reg, 1));

            DUMP_IR(op == OP_eq    ? "    x%d == x%d ?"
                    : op == OP_neq ? "    x%d != x%d ?"
                    : op == OP_lt  ? "    x%d < x%d ?"
                    : op == OP_geq ? "    x%d >= x%d ?"
                    : op == OP_gt  ? "    x%d > x%d ?"
                    : op == OP_leq ? "    x%d <= x%d ?"
                                   : "    x%d ?? x%d ?",
                    dest_reg, OP_reg);
            break;
        case OP_log_and:
            /* we assume both have to be 1, they can not be just nonzero */
            emit(__and_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d &&= x%d", dest_reg, OP_reg);
            break;
        case OP_log_or:
            emit(__or_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d ||= x%d", dest_reg, OP_reg);
            break;
        case OP_bit_and:
            emit(__and_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d &= x%d", dest_reg, OP_reg);
            break;
        case OP_bit_or:
            emit(__or_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d |= x%d", dest_reg, OP_reg);
            break;
        case OP_bit_xor:
            emit(__eor_r(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d ^= x%d", dest_reg, OP_reg);
            break;
        case OP_bit_not:
            emit(__mvn_r(__AL, dest_reg, dest_reg));
            DUMP_IR("    x%d ~= x%d", dest_reg, dest_reg);
            break;
        case OP_lshift:
            emit(__sll(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d <<= x%d", dest_reg, OP_reg);
            break;
        case OP_rshift:
            emit(__srl(__AL, dest_reg, dest_reg, OP_reg));
            DUMP_IR("    x%d >>= x%d", dest_reg, OP_reg);
            break;
        case OP_log_not:
            /* 1 if zero, 0 if nonzero */
            emit(__teq(dest_reg));
            emit(__mov_i(__NE, dest_reg, 0));
            emit(__mov_i(__EQ, dest_reg, 1));
            DUMP_IR("    !x%d", dest_reg);
            break;
        case OP_jz:
        case OP_jnz: {
            /* conditional jumps to IR-index */
            int jump_instr_index = ii->int_param1;
            ir_instr_t *jump_instr = &IR[jump_instr_index];
            int jump_location = jump_instr->code_offset;
            ofs = jump_location - pc - 4;

            emit(__teq(dest_reg));
            if (op == OP_jz) {
                emit(__b(__EQ, ofs));
                DUMP_IR("    if false then goto %d", ii->int_param1);
            } else {
                emit(__b(__NE, ofs));
                DUMP_IR("    if true then goto %d", ii->int_param1);
            }
        } break;
        case OP_block_start:
            blk = &BLOCKS[ii->int_param1];
            if (blk->next_local > 0) {
                /* reserve stack space for locals */
                emit(__add_i(__AL, __sp, __sp, -blk->locals_size));
                stack_size += blk->locals_size;
            }
            DUMP_IR("    {");
            _c_block_level++;
            break;
        case OP_block_end:
            blk = &BLOCKS[ii->int_param1]; /* should not be necessary */
            if (blk->next_local > 0) {
                /* remove stack space for locals */
                emit(__add_i(__AL, __sp, __sp, blk->locals_size));
                stack_size -= blk->locals_size;
            }
            /* blk is current block */
            blk = blk->parent;
            DUMP_IR("}");
            _c_block_level--;
            break;
        case OP_func_extry: {
            int pn, ps;
            fn = find_func(ii->str_param1);
            ps = fn->params_size;

            /* add to symbol table */
            elf_add_symbol(ii->str_param1, strlen(ii->str_param1),
                           code_start + pc);

            /* create stack space for params and parent frame */
            emit(__add_i(__AL, __sp, __sp, -16 - ps));
            emit(__sw(__AL, __r11, __sp, 12 + ps));
            emit(__sw(__AL, __lr, __sp, 8 + ps));
            emit(__add_i(__AL, __r11, __sp, ps));
            stack_size = ps;

            /* push parameters on stack */
            for (pn = 0; pn < fn->num_params; pn++) {
                emit(__sw(__AL, __r0 + pn, __r11, -fn->param_defs[pn].offset));
            }
            DUMP_IR("%s:", ii->str_param1);
        } break;
        case OP_start:
            emit(__lw(__AL, __r0, __sp, 0));    /* argc */
            emit(__add_i(__AL, __r1, __sp, 4)); /* argv */
            DUMP_IR("    start");
            break;
        case OP_syscall:
            /* Linux Arm/EABI syscalls are invoked using a software interrupt.
             * The function arguments go in registers R0-R6, the syscall
             * number in register R7.
             * See https://man7.org/linux/man-pages/man2/syscall.2.html
             */
            emit(__mov_r(__AL, __r7, __r0));
            emit(__mov_r(__AL, __r0, __r1));
            emit(__mov_r(__AL, __r1, __r2));
            emit(__mov_r(__AL, __r2, __r3));
            emit(__mov_r(__AL, __r3, __r4));
            emit(__mov_r(__AL, __r4, __r5));
            emit(__mov_r(__AL, __r5, __r6));
            emit(__svc());
            DUMP_IR("    syscall");
            break;
        case OP_exit:
            /* syscall for 'exit' */
            emit(__mov_r(__AL, __r0, OP_reg));
            emit(__mov_i(__AL, __r7, 1));
            emit(__svc());
            DUMP_IR("    exit");
            break;
        default:
            error("Unsupported IR opcode");
        }
    }
}
