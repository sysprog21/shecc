/**
 * Allocate registers from IR. The linear-scan algorithm now expects a minimum
 * of 7 available registers (typical for RISC-style architectures).
 *
 * TODO: Implement the "-O level" option. This allocator now always drops the
 * dead variable and does NOT wrtie it back to the stack.
 */

void expire_regs(int i)
{
    int t;
    for (t = 0; t < REG_CNT; t++) {
        if (REG[t].var == NULL)
            continue;
        if (REG[t].var->liveness < i) {
            REG[t].var = NULL;
            REG[t].polluted = 0;
        }
    }
}

int find_in_regs(var_t *var)
{
    int i;
    for (i = 0; i < REG_CNT; i++)
        if (REG[i].var == var)
            return i;
    return -1;
}

int try_avl_reg()
{
    int i;
    for (i = 0; i < REG_CNT; i++)
        if (REG[i].var == NULL)
            return i;
    return -1;
}

/* Return the available register. Spill the content if needed */
int get_src_reg(func_t *fn, var_t *var, int reserved)
{
    ph2_ir_t *ph2_ir;
    int reg_idx, i, ofs, t = 0;

    reg_idx = find_in_regs(var);
    if (reg_idx > -1)
        return reg_idx;

    reg_idx = try_avl_reg();
    if (reg_idx > -1) {
        REG[reg_idx].var = var;
        REG[reg_idx].polluted = 0;

        if (var->is_global == 1)
            ph2_ir = add_ph2_ir(OP_global_load);
        else
            ph2_ir = add_ph2_ir(OP_load);
        ph2_ir->dest = reg_idx;
        ph2_ir->src0 = var->offset;
        return reg_idx;
    }

    for (i = 0; i < REG_CNT; i++) {
        if (reserved == i)
            continue;
        if (REG[i].var->liveness > t) {
            t = REG[i].var->liveness;
            reg_idx = i;
        }
    }

    /**
     * TODO: Estimate the cost of global spilling. It should spill itself if
     * it has the longest lifetime.
     */
    if (0 && var->liveness > REG[reg_idx].var->liveness) {
        ;
    } else {
        ofs = REG[reg_idx].var->offset;

        /* allocate space if the temporary var is going to be spilled */
        if (ofs == 0) {
            if (REG[reg_idx].var->is_global == 1) {
                ofs = FUNCS[0].stack_size;
                FUNCS[0].stack_size += 4;
            } else {
                ofs = fn->stack_size;
                fn->stack_size += 4;
            }
            REG[reg_idx].var->offset = ofs;
        }

        if (REG[reg_idx].polluted) {
            if (REG[reg_idx].var->is_global == 1)
                ph2_ir = add_ph2_ir(OP_global_store);
            else
                ph2_ir = add_ph2_ir(OP_store);
            ph2_ir->src0 = reg_idx;
            ph2_ir->src1 = ofs;
        }

        if (var->is_global == 1)
            ph2_ir = add_ph2_ir(OP_global_load);
        else
            ph2_ir = add_ph2_ir(OP_load);
        ph2_ir->dest = reg_idx;
        ph2_ir->src0 = var->offset;

        REG[reg_idx].var = var;
        REG[reg_idx].polluted = 0;

        return reg_idx;
    }
}

/* use the source register if it is going to be expired */
int get_dest_reg(func_t *fn, var_t *var, int pc, int src0, int src1)
{
    ph2_ir_t *ph2_ir;
    int i, ofs, reg_idx;
    int t = 0;

    reg_idx = find_in_regs(var);
    if (reg_idx > -1) {
        REG[reg_idx].polluted = 1;
        return reg_idx;
    }

    reg_idx = try_avl_reg();
    if (reg_idx > -1) {
        REG[reg_idx].var = var;
        REG[reg_idx].polluted = 1;
        return reg_idx;
    }

    if (src0 > -1)
        if (REG[src0].var->liveness == pc) {
            REG[src0].var = var;
            REG[src0].polluted = 1;
            return src0;
        }

    if (src1 > -1)
        if (REG[src1].var->liveness == pc) {
            REG[src1].var = var;
            REG[src1].polluted = 1;
            return src1;
        }

    for (i = 0; i < REG_CNT; i++) {
        if (REG[i].var->liveness > t) {
            t = REG[i].var->liveness;
            reg_idx = i;
        }
    }

    /**
     * TODO: Estimate the cost of global spilling. It should spill itself if
     * it has the longest lifetime.
     */
    if (0 && var->liveness > REG[reg_idx].var->liveness) {
        ;
    } else {
        ofs = REG[reg_idx].var->offset;

        /* allocate space if the temporary var is going to be spilled */
        if (ofs == 0) {
            if (REG[reg_idx].var->is_global == 1) {
                ofs = FUNCS[0].stack_size;
                FUNCS[0].stack_size += 4;
            } else {
                ofs = fn->stack_size;
                fn->stack_size += 4;
            }
            REG[reg_idx].var->offset = ofs;
        }

        if (REG[reg_idx].polluted) {
            if (REG[reg_idx].var->is_global == 1)
                ph2_ir = add_ph2_ir(OP_global_store);
            else
                ph2_ir = add_ph2_ir(OP_store);
            ph2_ir->src0 = reg_idx;
            ph2_ir->src1 = ofs;
        }

        REG[reg_idx].var = var;
        REG[reg_idx].polluted = 1;

        return reg_idx;
    }
}

/**
 * Do not store if the var is going to to be expired after current iteration.
 * Used in `OP_branch`.
 */
void spill_all_regs(func_t *fn, int pc)
{
    ph2_ir_t *ph2_ir;
    int i, ofs;

    for (i = 0; i < REG_CNT; i++) {
        if (REG[i].var == NULL)
            continue;

        if (REG[i].var->liveness == pc)
            continue;

        if (REG[i].polluted == 0) {
            REG[i].var = NULL;
            continue;
        }

        if (REG[i].var->is_global == 1)
            ph2_ir = add_ph2_ir(OP_global_store);
        else
            ph2_ir = add_ph2_ir(OP_store);

        ph2_ir->src0 = i;
        ofs = REG[i].var->offset;

        /* allocate space if the temporary var is going to be spilled */
        if (ofs == 0) {
            if (REG[i].var->is_global == 1) {
                ofs = FUNCS[0].stack_size;
                FUNCS[0].stack_size += 4;
            } else {
                ofs = fn->stack_size;
                fn->stack_size += 4;
            }
            REG[i].var->offset = ofs;
        }
        ph2_ir->src1 = ofs;

        REG[i].var = NULL;
        REG[i].polluted = 0;
    }
}

void reg_alloc()
{
    ph1_ir_t *ph1_ir;
    ph2_ir_t *ph2_ir;
    func_t *fn;
    int i, j, ofs;
    int reg_idx, reg_idx_src0, reg_idx_src1;

    int argument_idx = 0;
    int loop_end_idx = 0;

    /**
     * Make sure the var which is "write-after-read" to be written back to the
     * stack before next iteration starting.
     */
    int loop_lv[MAX_NESTING];
    int loop_lv_idx = 0;

    for (i = 0; i < global_ir_idx; i++) {
        ph1_ir = &GLOBAL_IR[i];

        /* TODO: Estimate the cost of global spilling. */
        if (ph1_ir->op == OP_allocat)
            set_var_liveout(ph1_ir->src0, 1 << 28);
        else if (ph1_ir->op == OP_assign)
            set_var_liveout(ph1_ir->src0, i);
        else if (ph1_ir->op != OP_load_constant)
            error("Unsupported operation in global scope");
    }

    for (i = 0; i < ph1_ir_idx; i++) {
        ph1_ir = &PH1_IR[i];

        switch (ph1_ir->op) {
        case OP_allocat:
            if (ph1_ir->src0->is_global == 1)
                error("Unknown global allocation in body statement");
            break;
        case OP_label:
            if (loop_end_idx == i) {
                loop_end_idx = loop_lv[--loop_lv_idx];
                break;
            }
            if (ph1_ir->src0->init_val != 0) {
                loop_lv[loop_lv_idx++] = loop_end_idx;
                loop_end_idx = ph1_ir->src0->init_val;
            }
            break;
        case OP_write:
            set_var_liveout(ph1_ir->dest, i);
            if (ph1_ir->src0->is_func == 0)
                set_var_liveout(ph1_ir->src0, i);
            break;
        case OP_branch:
            set_var_liveout(ph1_ir->dest, i);
            break;
        case OP_push:
        case OP_indirect:
            set_var_liveout(ph1_ir->src0, i);
            break;
        case OP_return:
            if (ph1_ir->src0)
                set_var_liveout(ph1_ir->src0, i);
            break;
        case OP_add:
        case OP_sub:
        case OP_mul:
        case OP_div:
        case OP_mod:
        case OP_eq:
        case OP_neq:
        case OP_gt:
        case OP_lt:
        case OP_geq:
        case OP_leq:
        case OP_bit_and:
        case OP_bit_or:
        case OP_bit_xor:
        case OP_log_and:
        case OP_log_or:
        case OP_rshift:
        case OP_lshift:
            set_var_liveout(ph1_ir->src0, i);
            set_var_liveout(ph1_ir->src1, i);
            if (loop_end_idx != 0) {
                ph1_ir->src0->in_loop = 1;
                ph1_ir->src1->in_loop = 1;
            }
            if (ph1_ir->dest->in_loop != 0)
                set_var_liveout(ph1_ir->dest, loop_end_idx);
            break;
        case OP_assign:
        case OP_read:
        case OP_address_of:
        case OP_bit_not:
        case OP_log_not:
        case OP_negate:
            set_var_liveout(ph1_ir->src0, i);
            if (loop_end_idx != 0)
                ph1_ir->src0->in_loop = 1;
            if (ph1_ir->dest->in_loop != 0)
                set_var_liveout(ph1_ir->dest, loop_end_idx);
            break;
        default:
            break;
        }
    }

    /* initiate register file */
    for (i = 0; i < REG_CNT; i++) {
        REG[i].var = NULL;
        REG[i].polluted = 0;
    }

    for (i = 0; i < global_ir_idx; i++) {
        ph1_ir = &GLOBAL_IR[i];
        fn = &FUNCS[0];

        if (ph1_ir->op == OP_allocat) {
            ph1_ir->src0->offset = fn->stack_size;

            if (ph1_ir->src0->array_size == 0) {
                if (ph1_ir->src0->is_ptr)
                    fn->stack_size += 4;
                else if (strcmp(ph1_ir->src0->type_name, "int") &&
                         strcmp(ph1_ir->src0->type_name, "char")) {
                    type_t *type = find_type(ph1_ir->src0->type_name);
                    fn->stack_size += type->size;
                } else
                    fn->stack_size += 4;
            } else {
                /* allocate a pointer that pointing to the first element */
                int sz = fn->stack_size;
                fn->stack_size += PTR_SIZE;

                reg_idx = get_dest_reg(fn, ph1_ir->src0, i, -1, -1);

                ph2_ir = add_ph2_ir(OP_global_address_of);
                ph2_ir->src0 = fn->stack_size;
                ph2_ir->dest = reg_idx;

                if (ph1_ir->src0->is_ptr)
                    fn->stack_size += PTR_SIZE * ph1_ir->src0->array_size;
                else {
                    type_t *type = find_type(ph1_ir->src0->type_name);
                    fn->stack_size += type->size * ph1_ir->src0->array_size;
                }

                ph2_ir = add_ph2_ir(OP_global_store);
                ph2_ir->src0 = reg_idx;
                ph2_ir->src1 = sz;
            }
        } else if (ph1_ir->op == OP_load_constant) {
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, -1, -1);
            ph2_ir = add_ph2_ir(OP_load_constant);
            ph2_ir->src0 = ph1_ir->dest->init_val;
            ph2_ir->dest = reg_idx;
        } else if (ph1_ir->op == OP_assign) {
            reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, reg_idx_src0, -1);
            ph2_ir = add_ph2_ir(OP_assign);
            ph2_ir->src0 = reg_idx_src0;
            ph2_ir->dest = reg_idx;

            ph2_ir = add_ph2_ir(OP_global_store);
            ofs = ph1_ir->dest->offset;
            ph2_ir->src0 = reg_idx;
            ph2_ir->src1 = ofs;
        } else
            error("Unsupported operation in global scope ");
    }

    /* jump to entry point after global statements */
    ph2_ir = add_ph2_ir(OP_jump);
    strcpy(ph2_ir->func_name, "main");

    for (i = 0; i < ph1_ir_idx; i++) {
        ph1_ir = &PH1_IR[i];

        expire_regs(i);

        switch (ph1_ir->op) {
        case OP_define:
            fn = find_func(ph1_ir->func_name);

            ph2_ir = add_ph2_ir(OP_define);
            strcpy(ph2_ir->func_name, ph1_ir->func_name);

            /* set arguments valid */
            for (j = 0; j < fn->num_params; j++) {
                REG[j].var = &fn->param_defs[j];
                REG[j].polluted = 1;
            }
            for (; j < REG_CNT; j++) {
                REG[j].var = NULL;
                REG[j].polluted = 0;
            }

            /* make sure all arguments are in stack */
            spill_all_regs(fn, -1);

            break;
        case OP_block_start:
        case OP_block_end:
            spill_all_regs(fn, -1);
            add_ph2_ir(ph1_ir->op);
            break;
        case OP_allocat:
            ph1_ir->src0->offset = fn->stack_size;

            if (ph1_ir->src0->is_global == 1)
                error("Invalid allocation in body statement");

            if (ph1_ir->src0->array_size == 0) {
                if (ph1_ir->src0->is_ptr) {
                    fn->stack_size += 4;
                } else if (strcmp(ph1_ir->src0->type_name, "int") &&
                           strcmp(ph1_ir->src0->type_name, "char")) {
                    type_t *type = find_type(ph1_ir->src0->type_name);
                    fn->stack_size += type->size;
                } else
                    fn->stack_size += 4;
            } else {
                int sz = fn->stack_size;
                fn->stack_size += PTR_SIZE;

                reg_idx = get_dest_reg(fn, ph1_ir->src0, i, -1, -1);

                ph2_ir = add_ph2_ir(OP_address_of);
                ph2_ir->src0 = fn->stack_size;
                ph2_ir->dest = reg_idx;

                if (ph1_ir->src0->is_ptr)
                    fn->stack_size += PTR_SIZE * ph1_ir->src0->array_size;
                else {
                    type_t *type = find_type(ph1_ir->src0->type_name);
                    fn->stack_size += type->size * ph1_ir->src0->array_size;
                }
                ph2_ir = add_ph2_ir(OP_store);
                ph2_ir->src0 = reg_idx;
                ph2_ir->src1 = sz;
            }
            break;
        case OP_load_constant:
        case OP_load_data_address:
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, -1, -1);
            ph2_ir = add_ph2_ir(ph1_ir->op);
            ph2_ir->src0 = ph1_ir->dest->init_val;
            ph2_ir->dest = reg_idx;
            break;
        case OP_address_of:
            ofs = ph1_ir->src0->offset;
            if (ofs == 0) {
                reg_idx = find_in_regs(ph1_ir->src0);
                if (reg_idx == -1)
                    error("Unexpected error");

                ph2_ir = add_ph2_ir(OP_store);
                if (ph1_ir->src0->is_global) {
                    ofs = FUNCS[0].stack_size;
                    FUNCS[0].stack_size += 4;
                } else {
                    ofs = fn->stack_size;
                    fn->stack_size += 4;
                }
                ph1_ir->src0->offset = ofs;
                ph2_ir->src0 = reg_idx;
                ph2_ir->src1 = ofs;

                REG[reg_idx].polluted = 0;
            }

            /**
             * Write the content back to the stack to prevent getting the
             * obsolete content when dereferencing.
             */
            reg_idx = find_in_regs(ph1_ir->src0);
            if (reg_idx > -1)
                if (REG[reg_idx].polluted) {
                    if (REG[reg_idx].var->is_global)
                        ph2_ir = add_ph2_ir(OP_global_store);
                    else
                        ph2_ir = add_ph2_ir(OP_store);
                    ph2_ir->src0 = reg_idx;
                    ph2_ir->src1 = ofs;
                }

            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, -1, -1);

            if (ph1_ir->src0->is_global)
                ph2_ir = add_ph2_ir(OP_global_address_of);
            else
                ph2_ir = add_ph2_ir(OP_address_of);
            ph2_ir->src0 = ofs;
            ph2_ir->dest = reg_idx;
            break;
        case OP_label:
            spill_all_regs(fn, -1);
            ph2_ir = add_ph2_ir(OP_label);
            strcpy(ph2_ir->func_name, ph1_ir->src0->var_name);
            break;
        case OP_branch:
            spill_all_regs(fn, i);

            reg_idx_src0 = get_src_reg(fn, ph1_ir->dest, -1);
            ph2_ir = add_ph2_ir(OP_branch);
            ph2_ir->src0 = reg_idx_src0;
            strcpy(ph2_ir->true_label, ph1_ir->src0->var_name);
            strcpy(ph2_ir->false_label, ph1_ir->src1->var_name);
            break;
        case OP_jump:
            spill_all_regs(fn, -1);

            ph2_ir = add_ph2_ir(OP_jump);
            strcpy(ph2_ir->func_name, ph1_ir->dest->var_name);
            break;
        case OP_push:
            if (argument_idx == 0)
                spill_all_regs(fn, -1);

            ofs = ph1_ir->src0->offset;
            if (ph1_ir->src0->is_global)
                ph2_ir = add_ph2_ir(OP_global_load);
            else
                ph2_ir = add_ph2_ir(OP_load);
            ph2_ir->src0 = ofs;
            ph2_ir->dest = argument_idx;

            argument_idx++;
            break;
        case OP_call:
            if (argument_idx == 0)
                spill_all_regs(fn, -1);

            ph2_ir = add_ph2_ir(OP_call);
            strcpy(ph2_ir->func_name, ph1_ir->func_name);

            argument_idx = 0;
            break;
        case OP_indirect:
            if (argument_idx == 0)
                spill_all_regs(fn, -1);

            ofs = ph1_ir->src0->offset;
            if (ph1_ir->src0->is_global)
                ph2_ir = add_ph2_ir(OP_global_load_func);
            else
                ph2_ir = add_ph2_ir(OP_load_func);
            ph2_ir->src0 = ofs;

            ph2_ir = add_ph2_ir(OP_indirect);

            argument_idx = 0;
            break;
        case OP_func_ret:
            /* pseudo instruction: r0 := r0 */
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, -1, -1);
            ph2_ir = add_ph2_ir(OP_assign);
            ph2_ir->src0 = 0;
            ph2_ir->dest = reg_idx;
            break;
        case OP_return:
            spill_all_regs(fn, -1);

            if (ph1_ir->src0)
                reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
            else
                reg_idx_src0 = -1;

            ph2_ir = add_ph2_ir(OP_return);
            ph2_ir->src0 = reg_idx_src0;
            break;
        case OP_read:
            reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, reg_idx_src0, -1);
            ph2_ir = add_ph2_ir(OP_read);
            ph2_ir->src0 = reg_idx_src0;
            ph2_ir->src1 = ph1_ir->size;
            ph2_ir->dest = reg_idx;
            break;
        case OP_write:
            if (ph1_ir->src0->is_func) {
                reg_idx_src0 = get_src_reg(fn, ph1_ir->dest, -1);
                ph2_ir = add_ph2_ir(OP_address_of_func);
                ph2_ir->src0 = reg_idx_src0;
                strcpy(ph2_ir->func_name, ph1_ir->src0->var_name);
            } else {
                /* make sure there is no obsolete content after storing */
                spill_all_regs(fn, -1);

                reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
                reg_idx_src1 = get_src_reg(fn, ph1_ir->dest, reg_idx_src0);
                ph2_ir = add_ph2_ir(OP_write);
                ph2_ir->src0 = reg_idx_src0;
                ph2_ir->src1 = reg_idx_src1;
                ph2_ir->dest = ph1_ir->size;
            }
            break;
        case OP_assign:
        case OP_negate:
        case OP_bit_not:
        case OP_log_not:
            reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
            reg_idx = get_dest_reg(fn, ph1_ir->dest, i, reg_idx_src0, -1);
            ph2_ir = add_ph2_ir(ph1_ir->op);
            ph2_ir->src0 = reg_idx_src0;
            ph2_ir->dest = reg_idx;
            break;
        case OP_add:
        case OP_sub:
        case OP_mul:
        case OP_div:
        case OP_mod:
        case OP_eq:
        case OP_neq:
        case OP_gt:
        case OP_lt:
        case OP_geq:
        case OP_leq:
        case OP_bit_and:
        case OP_bit_or:
        case OP_bit_xor:
        case OP_log_and:
        case OP_log_or:
        case OP_rshift:
        case OP_lshift:
            reg_idx_src0 = get_src_reg(fn, ph1_ir->src0, -1);
            reg_idx_src1 = get_src_reg(fn, ph1_ir->src1, reg_idx_src0);
            reg_idx =
                get_dest_reg(fn, ph1_ir->dest, i, reg_idx_src0, reg_idx_src1);
            ph2_ir = add_ph2_ir(ph1_ir->op);
            ph2_ir->src0 = reg_idx_src0;
            ph2_ir->src1 = reg_idx_src1;
            ph2_ir->dest = reg_idx;
            break;
        default:
            add_ph2_ir(ph1_ir->op);
            break;
        }

        if (ph1_ir->op == OP_jump || ph1_ir->op == OP_call ||
            ph1_ir->op == OP_indirect)
            for (j = 0; j < REG_CNT; j++) {
                REG[j].var = NULL;
                REG[j].polluted = 0;
            }
    }
}

/* not support "%c" in printf() yet */
void dump_ph2_ir()
{
    ph2_ir_t *ph2_ir;
    int i, rd, rs1, rs2;

    for (i = 0; i < ph2_ir_idx; i++) {
        ph2_ir = &PH2_IR[i];

        rd = ph2_ir->dest + 48;
        rs1 = ph2_ir->src0 + 48;
        rs2 = ph2_ir->src1 + 48;

        switch (ph2_ir->op) {
        case OP_define:
            printf("%s:", ph2_ir->func_name);
            break;
        case OP_block_start:
        case OP_block_end:
        case OP_allocat:
            continue;
        case OP_assign:
            printf("\t%%x%c = %%x%c", rd, rs1);
            break;
        case OP_load_constant:
            printf("\tli %%x%c, $%d", rd, ph2_ir->src0);
            break;
        case OP_load_data_address:
            printf("\t%%x%c = .data(%d)", rd, ph2_ir->src0);
            break;
        case OP_address_of:
            printf("\t%%x%c = %%sp + %d", rd, ph2_ir->src0);
            break;
        case OP_global_address_of:
            printf("\t%%x%c = %%gp + %d", rd, ph2_ir->src0);
            break;
        case OP_label:
            printf("%s:", ph2_ir->func_name);
            break;
        case OP_branch:
            printf("\tbr %%x%c, %s, %s", rs1, ph2_ir->true_label,
                   ph2_ir->false_label);
            break;
        case OP_jump:
            printf("\tj %s", ph2_ir->func_name);
            break;
        case OP_call:
            printf("\tcall @%s", ph2_ir->func_name);
            break;
        case OP_return:
            if (ph2_ir->src0 == -1)
                printf("\tret");
            else
                printf("\tret %%x%c", rs1);
            break;
        case OP_load:
            printf("\tload %%x%c, %d(sp)", rd, ph2_ir->src0);
            break;
        case OP_store:
            printf("\tstore %%x%c, %d(sp)", rs1, ph2_ir->src1);
            break;
        case OP_global_load:
            printf("\tload %%x%c, %d(gp)", rd, ph2_ir->src0);
            break;
        case OP_global_store:
            printf("\tstore %%x%c, %d(gp)", rs1, ph2_ir->src1);
            break;
        case OP_read:
            printf("\t%%x%c = (%%x%c)", rd, rs1);
            break;
        case OP_write:
            printf("\t(%%x%c) = %%x%c", rs2, rs1);
            break;
        case OP_address_of_func:
            printf("\t(%%x%c) = @%s", rs1, ph2_ir->func_name);
            break;
        case OP_load_func:
            printf("\tload %%t0, %d(sp)", ph2_ir->src0);
            break;
        case OP_global_load_func:
            printf("\tload %%t0, %d(gp)", ph2_ir->src0);
            break;
        case OP_indirect:
            printf("\tindirect call @(%%t0)");
            break;
        case OP_negate:
            printf("\tneg %%x%c, %%x%c", rd, rs1);
            break;
        case OP_add:
            printf("\t%%x%c = add %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_sub:
            printf("\t%%x%c = sub %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_mul:
            printf("\t%%x%c = mul %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_div:
            printf("\t%%x%c = div %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_mod:
            printf("\t%%x%c = mod %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_eq:
            printf("\t%%x%c = eq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_neq:
            printf("\t%%x%c = neq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_gt:
            printf("\t%%x%c = gt %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_lt:
            printf("\t%%x%c = lt %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_geq:
            printf("\t%%x%c = geq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_leq:
            printf("\t%%x%c = leq %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_and:
            printf("\t%%x%c = and %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_or:
            printf("\t%%x%c = or %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_bit_not:
            printf("\t%%x%c = not %%x%c", rd, rs1);
            break;
        case OP_bit_xor:
            printf("\t%%x%c = xor %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_and:
            printf("\t%%x%c = and %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_or:
            printf("\t%%x%c = or %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_log_not:
            printf("\t%%x%c = not %%x%c", rd, rs1);
            break;
        case OP_rshift:
            printf("\t%%x%c = rshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_lshift:
            printf("\t%%x%c = lshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        default:
            break;
        }
        printf("\n");
    }
}
