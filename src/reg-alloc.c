/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the
 * file "LICENSE" for information on usage and redistribution of this file.
 */

/* Allocate registers from IR. The linear-scan algorithm now expects a minimum
 * of 7 available registers (typical for RISC-style architectures).
 *
 * TODO: Implement "-O level" optimization control. Currently the allocator
 * always performs dead variable elimination without writing back to stack.
 */
#include "defs.h"
#include "globals.c"

void vreg_map_to_phys(var_t *var, int phys_reg)
{
    if (var)
        var->phys_reg = phys_reg;
}

int vreg_get_phys(var_t *var)
{
    if (var)
        return var->phys_reg;
    return -1;
}

void vreg_clear_phys(var_t *var)
{
    if (var)
        var->phys_reg = -1;
}

/* Aligns size to nearest multiple of 4, this meets ARMv7's alignment
 * requirement.
 *
 * This function should be called whenever handling with user-defined type's
 * size.
 */
int align_size(int i)
{
    return i <= 4 ? 4 : (i + 3) & ~3;
}

bool check_live_out(basic_block_t *bb, var_t *var)
{
    for (int i = 0; i < bb->live_out.size; i++) {
        if (bb->live_out.elements[i] == var)
            return true;
    }
    return false;
}

void track_var_use(var_t *var, int insn_idx)
{
    if (!var)
        return;

    var->use_count++;

    if (var->first_use < 0)
        var->first_use = insn_idx;

    var->last_use = insn_idx;
}

void refresh(basic_block_t *bb, insn_t *insn)
{
    for (int i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (check_live_out(bb, REGS[i].var))
            continue;
        if (REGS[i].var->consumed < insn->idx) {
            vreg_clear_phys(REGS[i].var);
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
        }
    }
}

ph2_ir_t *bb_add_ph2_ir(basic_block_t *bb, opcode_t op)
{
    ph2_ir_t *n = arena_alloc(BB_ARENA, sizeof(ph2_ir_t));
    n->op = op;
    /* Initialize all fields explicitly */
    n->next = NULL;            /* well-formed singly linked list */
    n->is_branch_detached = 0; /* arch-lowering will set for branches */
    n->src0 = 0;
    n->src1 = 0;
    n->dest = 0;
    n->func_name[0] = '\0';
    n->next_bb = NULL;
    n->then_bb = NULL;
    n->else_bb = NULL;

    if (!bb->ph2_ir_list.head)
        bb->ph2_ir_list.head = n;
    else
        bb->ph2_ir_list.tail->next = n;

    bb->ph2_ir_list.tail = n;
    return n;
}

/* Calculate the cost of spilling a variable from a register.
 * Higher cost means the variable is more valuable to keep in a register.
 * The cost is computed based on multiple factors that affect performance.
 */
int calculate_spill_cost(var_t *var, basic_block_t *bb, int current_idx)
{
    int cost = 0;

    /* Variables that are live-out of the basic block must be spilled anyway,
     * so give them a high cost to prefer spilling them over others
     */
    if (check_live_out(bb, var))
        cost += 1000;

    /* Variables that will be used soon should have higher cost.
     * The closer the next use, the higher the penalty for spilling
     */
    if (var->consumed > current_idx) {
        int distance = var->consumed - current_idx;
        if (distance < 10)
            cost += 100 - distance * 10; /* Max 100 points for immediate use */
    }

    /* Frequently used variables should stay in registers.
     * Each use adds 5 points to the cost
     */
    if (var->use_count > 0)
        cost += var->use_count * 5;

    /* Variables inside loops are accessed repeatedly, so they should have much
     * higher priority to stay in registers (200 points per level)
     */
    if (var->loop_depth > 0)
        cost += var->loop_depth * 200;

    /* Constants can be easily reloaded, so prefer spilling them by reducing
     * their cost
     */
    if (var->is_const)
        cost -= 50;

    /* Variables with long live ranges may benefit from spilling to free up
     * registers for other variables
     */
    if (var->first_use >= 0 && var->last_use >= 0) {
        int range_length = var->last_use - var->first_use;
        if (range_length > 100)
            cost += 20; /* Small penalty for very long live ranges */
    }

    return cost;
}

int find_best_spill(basic_block_t *bb,
                    int current_idx,
                    int avoid_reg1,
                    int avoid_reg2)
{
    int best_reg = -1;
    int min_cost = 99999;

    for (int i = 0; i < REG_CNT; i++) {
        if (i == avoid_reg1 || i == avoid_reg2)
            continue;

        if (!REGS[i].var)
            continue;

        int cost = calculate_spill_cost(REGS[i].var, bb, current_idx);

        if (cost < min_cost) {
            min_cost = cost;
            best_reg = i;
        }
    }

    return best_reg;
}

/* Priority of spilling:
 * - live_out variable
 * - farthest local variable
 */
void spill_var(basic_block_t *bb, var_t *var, int idx)
{
    if (!REGS[idx].polluted) {
        REGS[idx].var = NULL;
        vreg_clear_phys(var);
        return;
    }

    if (!var->offset) {
        var->offset = bb->belong_to->stack_size;
        bb->belong_to->stack_size += 4;
    }
    ph2_ir_t *ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_store)
                                  : bb_add_ph2_ir(bb, OP_store);
    ir->src0 = idx;
    ir->src1 = var->offset;
    REGS[idx].var = NULL;
    REGS[idx].polluted = 0;
    vreg_clear_phys(var);
}

/* Return the index of register for given variable. Otherwise, return -1. */
int find_in_regs(var_t *var)
{
    for (int i = 0; i < REG_CNT; i++) {
        if (REGS[i].var == var)
            return i;
    }
    return -1;
}

void load_var(basic_block_t *bb, var_t *var, int idx)
{
    ph2_ir_t *ir;

    /* Load constants directly, others from memory */
    if (var->is_const) {
        ir = bb_add_ph2_ir(bb, OP_load_constant);
        ir->src0 = var->init_val;
    } else {
        ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_load)
                            : bb_add_ph2_ir(bb, OP_load);
        ir->src0 = var->offset;
    }

    ir->dest = idx;
    REGS[idx].var = var;
    REGS[idx].polluted = 0;
    vreg_map_to_phys(var, idx);
}

int prepare_operand(basic_block_t *bb, var_t *var, int operand_0)
{
    /* Check VReg mapping first for O(1) lookup */
    int phys_reg = vreg_get_phys(var);
    if (phys_reg >= 0 && phys_reg < REG_CNT && REGS[phys_reg].var == var)
        return phys_reg;

    /* Force reload for address-taken variables (may be modified via pointer) */
    int i = find_in_regs(var);
    if (i > -1 && !var->address_taken) {
        vreg_map_to_phys(var, i);
        return i;
    }

    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var) {
            load_var(bb, var, i);
            vreg_map_to_phys(var, i);
            return i;
        }
    }

    int spilled = find_best_spill(
        bb, bb->insn_list.tail ? bb->insn_list.tail->idx : 0, operand_0, -1);

    if (spilled < 0) {
        for (i = 0; i < REG_CNT; i++) {
            if (i != operand_0 && REGS[i].var) {
                spilled = i;
                break;
            }
        }
    }

    if (REGS[spilled].var)
        vreg_clear_phys(REGS[spilled].var);

    spill_var(bb, REGS[spilled].var, spilled);
    load_var(bb, var, spilled);
    vreg_map_to_phys(var, spilled);

    return spilled;
}

int prepare_dest(basic_block_t *bb, var_t *var, int operand_0, int operand_1)
{
    int phys_reg = vreg_get_phys(var);
    if (phys_reg >= 0 && phys_reg < REG_CNT && REGS[phys_reg].var == var) {
        REGS[phys_reg].polluted = 1;
        return phys_reg;
    }

    int i = find_in_regs(var);
    if (i > -1) {
        REGS[i].polluted = 1;
        vreg_map_to_phys(var, i);
        return i;
    }

    for (i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var) {
            REGS[i].var = var;
            REGS[i].polluted = 1;
            vreg_map_to_phys(var, i);
            return i;
        }
    }

    int spilled =
        find_best_spill(bb, bb->insn_list.tail ? bb->insn_list.tail->idx : 0,
                        operand_0, operand_1);

    if (spilled < 0) {
        for (i = 0; i < REG_CNT; i++) {
            if (i != operand_0 && i != operand_1 && REGS[i].var) {
                spilled = i;
                break;
            }
        }
    }

    if (REGS[spilled].var)
        vreg_clear_phys(REGS[spilled].var);

    spill_var(bb, REGS[spilled].var, spilled);
    REGS[spilled].var = var;
    REGS[spilled].polluted = 1;
    vreg_map_to_phys(var, spilled);

    return spilled;
}

void spill_alive(basic_block_t *bb, insn_t *insn)
{
    /* Spill all locals on pointer writes (conservative aliasing handling) */
    if (insn && insn->opcode == OP_write) {
        for (int i = 0; i < REG_CNT; i++) {
            if (REGS[i].var && !REGS[i].var->is_global)
                spill_var(bb, REGS[i].var, i);
        }
        return;
    }

    /* Standard spilling for non-pointer operations */
    for (int i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (check_live_out(bb, REGS[i].var)) {
            spill_var(bb, REGS[i].var, i);
            continue;
        }
        if (REGS[i].var->consumed > insn->idx) {
            spill_var(bb, REGS[i].var, i);
            continue;
        }
    }
}

void spill_live_out(basic_block_t *bb)
{
    for (int i = 0; i < REG_CNT; i++) {
        if (!REGS[i].var)
            continue;
        if (!check_live_out(bb, REGS[i].var)) {
            vreg_clear_phys(REGS[i].var);
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
            continue;
        }
        if (!var_check_killed(REGS[i].var, bb)) {
            vreg_clear_phys(REGS[i].var);
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
            continue;
        }
        spill_var(bb, REGS[i].var, i);
    }
}

/* The operand of 'OP_push' should not been killed until function called. */
void extend_liveness(basic_block_t *bb, insn_t *insn, var_t *var, int offset)
{
    if (check_live_out(bb, var))
        return;
    if (insn->idx + offset > var->consumed)
        var->consumed = insn->idx + offset;
}

void reg_alloc(void)
{
    /* TODO: Add proper .bss and .data section support for uninitialized /
     * initialized globals
     */
    for (insn_t *global_insn = GLOBAL_FUNC->bbs->insn_list.head; global_insn;
         global_insn = global_insn->next) {
        ph2_ir_t *ir;
        int dest, src0;

        switch (global_insn->opcode) {
        case OP_allocat:
            if (global_insn->rd->array_size) {
                /* Original scheme: pointer slot + backing region. Cache the
                 * base offset of the backing region into init_val so later
                 * global initializers can address elements without loading
                 * the pointer.
                 */
                global_insn->rd->offset = GLOBAL_FUNC->stack_size;
                GLOBAL_FUNC->stack_size += PTR_SIZE;
                src0 = GLOBAL_FUNC->stack_size; /* base of backing region */

                /* Stash base offset for this array variable */
                global_insn->rd->init_val = src0;

                if (global_insn->rd->ptr_level)
                    GLOBAL_FUNC->stack_size +=
                        align_size(PTR_SIZE * global_insn->rd->array_size);
                else {
                    GLOBAL_FUNC->stack_size +=
                        align_size(global_insn->rd->array_size *
                                   global_insn->rd->type->size);
                }

                dest = prepare_dest(GLOBAL_FUNC->bbs, global_insn->rd, -1, -1);
                ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_global_address_of);
                ir->src0 = src0;
                ir->dest = dest;
                spill_var(GLOBAL_FUNC->bbs, global_insn->rd, dest);
            } else {
                global_insn->rd->offset = GLOBAL_FUNC->stack_size;
                if (global_insn->rd->ptr_level)
                    GLOBAL_FUNC->stack_size += PTR_SIZE;
                else if (global_insn->rd->type != TY_int &&
                         global_insn->rd->type != TY_short &&
                         global_insn->rd->type != TY_char &&
                         global_insn->rd->type != TY_bool) {
                    GLOBAL_FUNC->stack_size +=
                        align_size(global_insn->rd->type->size);
                } else
                    /* 'char' is aligned to one byte for the convenience */
                    GLOBAL_FUNC->stack_size += 4;
            }
            break;
        case OP_load_constant:
        case OP_load_data_address:
        case OP_load_rodata_address:
            dest = prepare_dest(GLOBAL_FUNC->bbs, global_insn->rd, -1, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, global_insn->opcode);
            ir->src0 = global_insn->rd->init_val;
            ir->dest = dest;
            break;
        case OP_assign:
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            dest = prepare_dest(GLOBAL_FUNC->bbs, global_insn->rd, src0, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_assign);
            ir->src0 = src0;
            ir->dest = dest;
            spill_var(GLOBAL_FUNC->bbs, global_insn->rd, dest);
            /* release the unused constant number in register manually */
            REGS[src0].polluted = 0;
            vreg_clear_phys(REGS[src0].var);
            REGS[src0].var = NULL;
            break;
        case OP_add: {
            /* Special-case address computation for globals: if rs1 is a global
             * base and rs2 is a constant, propagate absolute offset to rd so
             * OP_write can fold into OP_global_store.
             */
            if (global_insn->rs1 && global_insn->rs1->is_global &&
                global_insn->rs2) {
                int base_off = global_insn->rs1->offset;
                /* For global arrays, use backing-region base cached in init_val
                 */
                if (global_insn->rs1->array_size > 0)
                    base_off = global_insn->rs1->init_val;
                global_insn->rd->offset = base_off + global_insn->rs2->init_val;
                global_insn->rd->is_global = true;
                break;
            }
            /* Fallback: generate an add */
            int src1;
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            src1 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs2, src0);
            dest = prepare_dest(GLOBAL_FUNC->bbs, global_insn->rd, src0, src1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_add);
            ir->src0 = src0;
            ir->src1 = src1;
            ir->dest = dest;
            break;
        }
        case OP_write: {
            /* Fold (addr, val) where addr carries GP-relative offset */
            if (global_insn->rs1 && (global_insn->rs1->is_global)) {
                int vreg =
                    prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs2, -1);
                ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_global_store);
                ir->src0 = vreg;
                /* For array variables used as base, store to the backing
                 * region's base offset (cached in init_val).
                 */
                int base_off = global_insn->rs1->offset;
                if (global_insn->rs1->array_size > 0)
                    base_off = global_insn->rs1->init_val;
                ir->src1 = base_off;
                break;
            }
            /* Fallback generic write */
            int src1;
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            src1 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs2, src0);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_write);
            ir->src0 = src0;
            ir->src1 = src1;
            ir->dest = global_insn->sz;
            break;
        }
        default:
            printf("Unsupported global operation: %d\n", global_insn->opcode);
            abort();
        }
    }

    for (func_t *func = FUNC_LIST.head; func; func = func->next) {
        /* Skip function declarations without bodies */
        if (!func->bbs)
            continue;

        func->visited++;

        if (!strcmp(func->return_def.var_name, "main"))
            MAIN_BB = func->bbs;

        for (int i = 0; i < REG_CNT; i++)
            REGS[i].var = NULL;

        /* set arguments available */
        for (int i = 0; i < func->num_params; i++) {
            REGS[i].var = func->param_defs[i].subscripts[0];
            REGS[i].polluted = 1;
        }

        /* variadic function implementation */
        if (func->va_args) {
            for (int i = 0; i < MAX_PARAMS; i++) {
                ph2_ir_t *ir = bb_add_ph2_ir(func->bbs, OP_store);

                if (i < func->num_params)
                    func->param_defs[i].subscripts[0]->offset =
                        func->stack_size;

                ir->src0 = i;
                ir->src1 = func->stack_size;
                func->stack_size += 4;
            }
        }

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bool is_pushing_args = false;
            int args = 0;

            bb->visited++;

            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                func_t *callee_func;
                ph2_ir_t *ir;
                int dest, src0, src1;
                int sz, clear_reg;

                refresh(bb, insn);

                switch (insn->opcode) {
                case OP_unwound_phi:
                    track_var_use(insn->rs1, insn->idx);
                    src0 = prepare_operand(bb, insn->rs1, -1);

                    if (!insn->rd->offset) {
                        insn->rd->offset = bb->belong_to->stack_size;
                        bb->belong_to->stack_size += 4;
                    }

                    ir = bb_add_ph2_ir(bb, OP_store);
                    ir->src0 = src0;
                    ir->src1 = insn->rd->offset;
                    break;
                case OP_allocat:
                    if ((insn->rd->type == TY_void ||
                         insn->rd->type == TY_int ||
                         insn->rd->type == TY_short ||
                         insn->rd->type == TY_char ||
                         insn->rd->type == TY_bool) &&
                        insn->rd->array_size == 0)
                        break;

                    insn->rd->offset = func->stack_size;
                    func->stack_size += PTR_SIZE;
                    src0 = func->stack_size;

                    if (insn->rd->ptr_level)
                        sz = PTR_SIZE;
                    else {
                        sz = insn->rd->type->size;
                    }

                    if (insn->rd->array_size)
                        func->stack_size +=
                            align_size(insn->rd->array_size * sz);
                    else
                        func->stack_size += align_size(sz);

                    dest = prepare_dest(bb, insn->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = src0;
                    ir->dest = dest;

                    /* For arrays, store the base address just like global
                     * arrays do
                     */
                    if (insn->rd->array_size)
                        spill_var(bb, insn->rd, dest);
                    break;
                case OP_load_constant:
                case OP_load_data_address:
                case OP_load_rodata_address:
                    dest = prepare_dest(bb, insn->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src0 = insn->rd->init_val;
                    ir->dest = dest;

                    /* store global variable immediately after assignment */
                    if (insn->rd->is_global) {
                        ir = bb_add_ph2_ir(bb, OP_global_store);
                        ir->src0 = dest;
                        ir->src1 = insn->rd->offset;
                        REGS[dest].polluted = 0;
                    }

                    break;
                case OP_address_of:
                case OP_global_address_of:
                    /* Mark variable as address-taken, disable constant
                     * optimization
                     */
                    insn->rs1->address_taken = true;
                    insn->rs1->is_const = false;

                    /* make sure variable is on stack */
                    if (!insn->rs1->offset) {
                        insn->rs1->offset = bb->belong_to->stack_size;
                        bb->belong_to->stack_size += 4;

                        for (int i = 0; i < REG_CNT; i++)
                            if (REGS[i].var == insn->rs1) {
                                ir = bb_add_ph2_ir(bb, OP_store);
                                ir->src0 = i;
                                ir->src1 = insn->rs1->offset;
                                /* Clear stale register tracking */
                                REGS[i].var = NULL;
                            }
                    }

                    dest = prepare_dest(bb, insn->rd, -1, -1);
                    if (insn->rs1->is_global ||
                        insn->opcode == OP_global_address_of)
                        ir = bb_add_ph2_ir(bb, OP_global_address_of);
                    else
                        ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = insn->rs1->offset;
                    ir->dest = dest;
                    break;
                case OP_assign:
                    if (insn->rd->consumed == -1)
                        break;

                    track_var_use(insn->rs1, insn->idx);
                    src0 = find_in_regs(insn->rs1);

                    /* If operand is loaded from stack, clear the original slot
                     * after moving.
                     */
                    if (src0 > -1)
                        clear_reg = 0;
                    else {
                        clear_reg = 1;
                        src0 = prepare_operand(bb, insn->rs1, -1);
                    }
                    dest = prepare_dest(bb, insn->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, OP_assign);
                    ir->src0 = src0;
                    ir->dest = dest;

                    /* store global variable immediately after assignment */
                    if (insn->rd->is_global) {
                        ir = bb_add_ph2_ir(bb, OP_global_store);
                        ir->src0 = dest;
                        ir->src1 = insn->rd->offset;
                        REGS[dest].polluted = 0;
                    }

                    if (clear_reg) {
                        vreg_clear_phys(REGS[src0].var);
                        REGS[src0].var = NULL;
                    }

                    break;
                case OP_read:
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    dest = prepare_dest(bb, insn->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, OP_read);
                    ir->src0 = src0;
                    ir->src1 = insn->sz;
                    ir->dest = dest;
                    break;
                case OP_write:
                    if (insn->rs2->is_func) {
                        src0 = prepare_operand(bb, insn->rs1, -1);
                        ir = bb_add_ph2_ir(bb, OP_address_of_func);
                        ir->src0 = src0;
                        strcpy(ir->func_name, insn->rs2->var_name);
                    } else {
                        /* FIXME: Register content becomes stale after store
                         * operation. Current workaround causes redundant
                         * spilling - need better register invalidation
                         * strategy.
                         */
                        spill_alive(bb, insn);
                        src0 = prepare_operand(bb, insn->rs1, -1);
                        src1 = prepare_operand(bb, insn->rs2, src0);
                        ir = bb_add_ph2_ir(bb, OP_write);
                        ir->src0 = src0;
                        ir->src1 = src1;
                        ir->dest = insn->sz;
                    }
                    break;
                case OP_branch:
                    src0 = prepare_operand(bb, insn->rs1, -1);

                    /* REGS[src0].var had been set to NULL, but the actual
                     * content is still holded in the register.
                     */
                    spill_live_out(bb);

                    ir = bb_add_ph2_ir(bb, OP_branch);
                    ir->src0 = src0;
                    ir->then_bb = bb->then_;
                    ir->else_bb = bb->else_;
                    break;
                case OP_push:
                    extend_liveness(bb, insn, insn->rs1, insn->sz);

                    if (!is_pushing_args) {
                        spill_alive(bb, insn);
                        is_pushing_args = true;
                    }

                    src0 = prepare_operand(bb, insn->rs1, -1);
                    ir = bb_add_ph2_ir(bb, OP_assign);
                    ir->src0 = src0;
                    ir->dest = args++;
                    REGS[ir->dest].var = insn->rs1;
                    REGS[ir->dest].polluted = 0;
                    break;
                case OP_call:
                    callee_func = find_func(insn->str);
                    if (!callee_func->num_params)
                        spill_alive(bb, insn);

                    ir = bb_add_ph2_ir(bb, OP_call);
                    strcpy(ir->func_name, insn->str);

                    is_pushing_args = false;
                    args = 0;

                    for (int i = 0; i < REG_CNT; i++)
                        REGS[i].var = NULL;

                    break;
                case OP_indirect:
                    if (!args)
                        spill_alive(bb, insn);

                    src0 = prepare_operand(bb, insn->rs1, -1);
                    ir = bb_add_ph2_ir(bb, OP_load_func);
                    ir->src0 = src0;

                    bb_add_ph2_ir(bb, OP_indirect);

                    is_pushing_args = false;
                    args = 0;
                    break;
                case OP_func_ret:
                    dest = prepare_dest(bb, insn->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_assign);
                    ir->src0 = 0;
                    ir->dest = dest;
                    break;
                case OP_return:
                    if (insn->rs1)
                        src0 = prepare_operand(bb, insn->rs1, -1);
                    else
                        src0 = -1;

                    ir = bb_add_ph2_ir(bb, OP_return);
                    ir->src0 = src0;
                    break;
                case OP_add:
                case OP_sub:
                case OP_mul:
                case OP_div:
                case OP_mod:
                case OP_lshift:
                case OP_rshift:
                case OP_eq:
                case OP_neq:
                case OP_gt:
                case OP_geq:
                case OP_lt:
                case OP_leq:
                case OP_bit_and:
                case OP_bit_or:
                case OP_bit_xor:
                    track_var_use(insn->rs1, insn->idx);
                    track_var_use(insn->rs2, insn->idx);
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    src1 = prepare_operand(bb, insn->rs2, src0);
                    dest = prepare_dest(bb, insn->rd, src0, src1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src0 = src0;
                    ir->src1 = src1;
                    ir->dest = dest;
                    break;
                case OP_negate:
                case OP_bit_not:
                case OP_log_not:
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    dest = prepare_dest(bb, insn->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src0 = src0;
                    ir->dest = dest;
                    break;
                case OP_trunc:
                case OP_sign_ext:
                case OP_cast:
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    dest = prepare_dest(bb, insn->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src1 = insn->sz;
                    ir->src0 = src0;
                    ir->dest = dest;
                    break;
                default:
                    printf("Unknown opcode\n");
                    abort();
                }
            }

            if (bb->next)
                spill_live_out(bb);

            if (bb == func->exit)
                continue;

            /* append jump instruction for the normal block only */
            if (!bb->next)
                continue;

            if (bb->next == func->exit)
                continue;

            /* jump to the beginning of loop or over the else block */
            if (bb->next->visited == func->visited ||
                bb->next->rpo != bb->rpo + 1) {
                ph2_ir_t *ir = bb_add_ph2_ir(bb, OP_jump);
                ir->next_bb = bb->next;
            }
        }

        /* handle implicit return */
        for (int i = 0; i < MAX_BB_PRED; i++) {
            basic_block_t *bb = func->exit->prev[i].bb;
            if (!bb)
                continue;

            if (func->return_def.type != TY_void)
                continue;

            if (bb->insn_list.tail)
                if (bb->insn_list.tail->opcode == OP_return)
                    continue;

            ph2_ir_t *ir = bb_add_ph2_ir(bb, OP_return);
            ir->src0 = -1;
        }
    }
}

void dump_ph2_ir(void)
{
    for (int i = 0; i < ph2_ir_idx; i++) {
        ph2_ir_t *ph2_ir = PH2_IR_FLATTEN[i];

        const int rd = ph2_ir->dest + 48;
        const int rs1 = ph2_ir->src0 + 48;
        const int rs2 = ph2_ir->src1 + 48;

        switch (ph2_ir->op) {
        case OP_define:
            printf("%s:", ph2_ir->func_name);
            break;
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
        case OP_load_rodata_address:
            printf("\t%%x%c = .rodata(%d)", rd, ph2_ir->src0);
            break;
        case OP_address_of:
            printf("\t%%x%c = %%sp + %d", rd, ph2_ir->src0);
            break;
        case OP_global_address_of:
            printf("\t%%x%c = %%gp + %d", rd, ph2_ir->src0);
            break;
        case OP_branch:
            printf("\tbr %%x%c", rs1);
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
            printf("\t(%%x%c) = %%x%c", rs1, rs2);
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
        case OP_log_not:
            printf("\t%%x%c = not %%x%c", rd, rs1);
            break;
        case OP_rshift:
            printf("\t%%x%c = rshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_lshift:
            printf("\t%%x%c = lshift %%x%c, %%x%c", rd, rs1, rs2);
            break;
        case OP_trunc:
            printf("\t%%x%c = trunc %%x%c, %d", rd, rs1, ph2_ir->src1);
            break;
        case OP_sign_ext:
            printf("\t%%x%c = sign_ext %%x%c, %d", rd, rs1, ph2_ir->src1);
            break;
        case OP_cast:
            printf("\t%%x%c = cast %%x%c", rd, rs1);
            break;
        default:
            break;
        }
        printf("\n");
    }
}
