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

/* A value is pointer-like if it is a pointer itself or its type is.
 * On LP64 targets these occupy PTR_SIZE bytes rather than 4.
 */
bool is_pointer_like(var_t *v)
{
    return v && (v->ptr_level > 0 || (v->type && v->type->ptr_level > 0));
}

/* Width of the value a local's frame slot actually holds.
 *
 * Slots are pointer-sized, but a scalar occupies only its low bytes. Reading
 * the whole slot back would pick up stale bytes whenever the variable was
 * written through a pointer, so loads must use the declared width. Anything
 * not a known scalar -- pointers, arrays, aggregates, functions -- keeps a
 * full pointer in its slot and is reported as such.
 */
int var_slot_size(var_t *v)
{
    if (!v || v->ptr_level || v->is_func || v->array_size)
        return PTR_SIZE;
    /* Only a variable whose address escaped can have its slot written behind
     * the allocator's back, at the pointee's width rather than the slot's.
     * Everything else is written and read through this same path, so the
     * full slot is always valid and the wider access is safe.
     *
     * Narrowing these to the declared width does not work: an SSA temporary
     * carries the type of the expression that made it, which for pointer
     * arithmetic is still int, and a four-byte slot truncates the address.
     */
    if (!v->address_taken)
        return PTR_SIZE;
    if (!v->type)
        return PTR_SIZE;
    /* Classify by the stored width rather than by type identity: an enum is
     * a distinct type_t that still stores as a 4-byte int, and a store
     * through an enum pointer writes only those 4 bytes. Reading the slot
     * any wider then picks up whatever the stack happened to hold above it.
     */
    if (v->type->base_type == TYPE_struct)
        return PTR_SIZE;
    if (v->type->size == 1 || v->type->size == 2 || v->type->size == 4)
        return v->type->size;
    return PTR_SIZE;
}

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

bool aggregate_has_function_pointer_seen(type_t *type,
                                         type_t **seen,
                                         int seen_count)
{
    if (!type)
        return false;

    for (int i = 0; i < seen_count; i++)
        if (seen[i] == type)
            return false;

    if (seen_count >= MAX_TYPES)
        return false;
    seen[seen_count++] = type;

    if (type->base_type == TYPE_typedef && type->base_struct)
        type = type->base_struct;

    for (int i = 0; i < type->num_fields; i++) {
        var_t *field = &type->fields[i];
        if (field->is_func)
            return true;

        if (!field->ptr_level && field->type && !field->type->ptr_level &&
            (field->type->base_type == TYPE_struct ||
             field->type->base_type == TYPE_union ||
             field->type->base_type == TYPE_typedef) &&
            aggregate_has_function_pointer_seen(field->type, seen, seen_count))
            return true;
    }

    return false;
}

bool aggregate_has_function_pointer(type_t *type)
{
    type_t *seen[MAX_TYPES];
    return aggregate_has_function_pointer_seen(type, seen, 0);
}

/* Whether @var appears in @list. */
bool var_list_holds(var_list_t *list, var_t *var)
{
    for (int i = 0; i < list->size; i++) {
        if (list->elements[i] == var)
            return true;
    }
    return false;
}

bool check_live_out(basic_block_t *bb, var_t *var)
{
    return var_list_holds(&bb->live_out, var);
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
    n->ofs_based_on_stack_top = false;
    n->size_bytes = PTR_SIZE; /* default to the full slot; see add_ph2_ir */
    n->is_pointer = false;

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
/* Slots reg_alloc() hands out inside the current function, in allocation order
 * and therefore ascending by offset: both places that call slot_var_track()
 * assign var->offset = stack_size and then grow the frame. Only these are
 * candidates for the two cleanups below; every other address in the frame
 * belongs to something a pointer may legally reach.
 *
 * The parallel arrays are indexed by position in slot_vars and are filled once
 * per function by slot_scan().
 */
var_t *slot_vars[MAX_LOCALS];
int slot_var_count;
char slot_private[MAX_LOCALS];
char slot_stores[MAX_LOCALS]; /* saturates at 2: only "exactly one" matters */
char slot_loads[MAX_LOCALS];
basic_block_t *slot_home[MAX_LOCALS];

void slot_var_track(var_t *var)
{
    if (slot_var_count < MAX_LOCALS)
        slot_vars[slot_var_count++] = var;
}

/* Whether the slot's address cannot have escaped, so that the stores and loads
 * naming it are the only accesses to it. address_taken covers &var; array_size
 * and has_backing_storage cover aggregates whose interior is reached by
 * pointer arithmetic. A store through a pointer is invisible to these scans,
 * so anything else in the frame is left alone.
 */
bool slot_is_private(var_t *var)
{
    if (var->address_taken || var->array_size || var->has_backing_storage)
        return false;
    if (var->is_global || var->ofs_based_on_stack_top)
        return false;
    return true;
}

/* Position of the slot at @offset in slot_vars, or -1 when the offset does not
 * name one. The array is sorted, so this is a binary search.
 */
int slot_lookup(int offset)
{
    int lo = 0, hi = slot_var_count - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int at = slot_vars[mid]->offset;

        if (at == offset)
            return mid;
        if (at < offset)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

/* Tally the accesses to every tracked slot in one walk of the function.
 *
 * Both cleanups below need to know how often a slot is stored and loaded.
 * Asking that question per slot means re-walking the whole function once for
 * each of them, which is quadratic and, on shecc's own larger functions, was
 * the single most expensive thing the compiler did.
 */
void slot_scan(func_t *func)
{
    for (int i = 0; i < slot_var_count; i++) {
        slot_private[i] = slot_is_private(slot_vars[i]);
        slot_stores[i] = 0;
        slot_loads[i] = 0;
        slot_home[i] = NULL;
    }

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = ir->next) {
            if (ir->ofs_based_on_stack_top)
                continue;
            if (ir->op == OP_store) {
                int i = slot_lookup(ir->src1);
                if (i < 0)
                    continue;
                if (slot_stores[i] < 2)
                    slot_stores[i]++;
                slot_home[i] = bb;
            } else if (ir->op == OP_load) {
                int i = slot_lookup(ir->src0);
                if (i < 0)
                    continue;
                if (slot_loads[i] < 2)
                    slot_loads[i]++;
                if (slot_home[i] != bb)
                    slot_home[i] = NULL;
            }
        }
    }
}

/* Unlink @ir, whose predecessor in @bb's list is @prev (NULL at the head). */
void ph2_list_remove(basic_block_t *bb, ph2_ir_t *prev, ph2_ir_t *ir)
{
    if (prev)
        prev->next = ir->next;
    else
        bb->ph2_ir_list.head = ir->next;
    if (bb->ph2_ir_list.tail == ir)
        bb->ph2_ir_list.tail = prev;
}

/* Whether @ir leaves @reg holding something other than what it held before. */
bool ph2_writes_reg(ph2_ir_t *ir, int reg)
{
    switch (ir->op) {
    case OP_store:
    case OP_global_store:
    case OP_write:
    case OP_branch:
    case OP_jump:
    case OP_return:
    case OP_push:
        return false;
    case OP_call:
    case OP_indirect:
        return true; /* the call clobbers the caller-saved registers */
    default:
        return ir->dest == reg;
    }
}

/* Collapse a value that goes out to a stack slot and comes straight back.
 *
 * Unwinding a phi writes the value to the phi's own slot, and the next
 * instruction reads it back to copy it into the variable's slot -- a round trip
 * through memory that also hides the "s = s + x" pattern from
 * try_fold_alu_to_slot(), which would otherwise emit a single add to the slot.
 * When the slot is written once and read once in the same block, and the source
 * register survives in between, the load is just a register move and the store
 * is dead.
 */
void collapse_slot_roundtrip(func_t *func)
{
    for (int i = 0; i < slot_var_count; i++) {
        if (!slot_private[i] || slot_stores[i] != 1 || slot_loads[i] != 1)
            continue;

        basic_block_t *home = slot_home[i];
        if (!home)
            continue;

        int offset = slot_vars[i]->offset;
        ph2_ir_t *store = NULL, *load = NULL, *prev_store = NULL,
                 *prev_load = NULL, *prev = NULL;
        bool ok = false;

        /* The store has to come first, and its register has to still hold the
         * value when the load would have run. Both are unique in the function,
         * so the first of each seen here is the one.
         */
        for (ph2_ir_t *ir = home->ph2_ir_list.head; ir; ir = ir->next) {
            if (!ir->ofs_based_on_stack_top) {
                if (!store && ir->op == OP_store && ir->src1 == offset) {
                    store = ir;
                    prev_store = prev;
                    prev = ir;
                    continue;
                }
                if (ir->op == OP_load && ir->src0 == offset) {
                    load = ir;
                    prev_load = prev;
                    ok = store != NULL;
                    break;
                }
            }
            if (store && ph2_writes_reg(ir, store->src0))
                break;
            prev = ir;
        }
        if (!ok)
            continue;

        load->op = OP_assign;
        load->src0 = store->src0;

        /* Removing the store first would leave prev_load stale when the store
         * is the load's predecessor, so drop the later one first.
         */
        if (load->dest == load->src0)
            ph2_list_remove(home, prev_load, load);
        ph2_list_remove(home, prev_store, store);
    }
}

/* Drop stores to a slot that nothing in the function ever loads.
 *
 * Unwinding a phi copies the value into the phi's slot, and the source SSA
 * temp is written to a slot of its own that no one reads -- one wasted store
 * per phi operand, on every iteration of a loop.
 */
void dead_store_elim(func_t *func)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        ph2_ir_t *prev = NULL, *next;
        for (ph2_ir_t *ir = bb->ph2_ir_list.head; ir; ir = next) {
            next = ir->next;
            if (ir->op == OP_store && !ir->ofs_based_on_stack_top) {
                int i = slot_lookup(ir->src1);
                if (i >= 0 && slot_private[i] && !slot_loads[i]) {
                    ph2_list_remove(bb, prev, ir);
                    continue;
                }
            }
            prev = ir;
        }
    }
}

/* Write the register back to the variable's stack slot, allocating the slot on
 * first use.  The register keeps holding the value; only memory is made
 * coherent, so callers decide separately whether to drop the association.
 */
/* Reserve @var's stack slot in @func's frame.
 *
 * A variable whose address escapes is reached through a pointer that carries
 * no information about what it points to, so give it the alignment any object
 * type could need rather than only its own. Slots stay in increasing offset
 * order, which slot_lookup()'s binary search relies on.
 */
void alloc_var_slot(func_t *func, var_t *var)
{
    if (var->address_taken)
        func->stack_size = ALIGN_UP(func->stack_size, 16);
    var->offset = func->stack_size;
    var->space_is_allocated = true;
    func->stack_size += PTR_SIZE;
    slot_var_track(var);
}

void store_var(basic_block_t *bb, var_t *var, int idx)
{
    if (!var->space_is_allocated) {
        alloc_var_slot(bb->belong_to, var);
    }
    ph2_ir_t *ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_store)
                                  : bb_add_ph2_ir(bb, OP_store);
    ir->src0 = idx;
    ir->src1 = var->offset;
    ir->ofs_based_on_stack_top = var->ofs_based_on_stack_top;
    ir->is_pointer = is_pointer_like(var);
    ir->size_bytes = var_slot_size(var);
    REGS[idx].polluted = 0;
}

void spill_var(basic_block_t *bb, var_t *var, int idx)
{
    if (REGS[idx].polluted)
        store_var(bb, var, idx);
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
    } else if (var->is_global && var->array_size) {
        /* A global array's address is fixed for the life of the program, and
         * the initialiser recorded where its storage sits. Computing it beats
         * loading back the pointer that was stored there: the address is an
         * addition rather than a memory read, so it does not have to wait on
         * the data cache on every trip round a loop.
         */
        ir = bb_add_ph2_ir(bb, OP_global_address_of);
        ir->src0 = var->init_val;
    } else {
        ir = var->is_global ? bb_add_ph2_ir(bb, OP_global_load)
                            : bb_add_ph2_ir(bb, OP_load);
        ir->src0 = var->offset;
        ir->ofs_based_on_stack_top = var->ofs_based_on_stack_top;
    }

    ir->dest = idx;
    ir->is_pointer = is_pointer_like(var);
    ir->size_bytes = var_slot_size(var);
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

/* Set while lowering the run of OP_push that stages a call's arguments. The
 * pushed values have to stay put until the call consumes them, and that
 * liveness is recorded on var->consumed rather than in the block's remaining
 * instructions, so no register may be reused for anything here.
 */
bool is_pushing_args;

/* Whether @var is read again before the end of @bb.
 *
 * var->consumed is the last use anywhere in the function, which says nothing
 * about this block: a value whose only remaining reader sits on a path this
 * block does not reach is dead here, and every value that outlives the block
 * is in its live-out set. Asking the block directly is what makes the two
 * arms of an if-else able to reuse the same register.
 */
bool var_read_later_in_bb(basic_block_t *bb, insn_t *from, var_t *var)
{
    for (insn_t *insn = from; insn; insn = insn->next) {
        if (insn->rs1 == var || insn->rs2 == var)
            return true;
    }
    return false;
}

/* x86 ALU instructions are two-operand: "rd = rs1 OP rs2" is emitted as
 * "MOV rd, rs1" followed by "OP rd, rs2", and the MOV disappears entirely when
 * rd already names rs1's register. So when a source operand is dead after this
 * instruction, its register is the best possible home for the destination.
 *
 * Only a plain local qualifies. A global or an address-taken variable is
 * reachable through memory, so its register cannot simply be repurposed even
 * once the SSA name is dead.
 */
int coalesce_candidate(basic_block_t *bb, insn_t *insn, int reg)
{
    if (reg < 0 || reg >= REG_CNT)
        return -1;

    if (!insn)
        return -1;

    /* Restricted to the pure ALU forms, whose lowering is exactly
     * "MOV rd, rs1; OP rd, rs2". Everything else -- loads, calls, address
     * arithmetic the backend folds into addressing modes -- has its own
     * register expectations and is left alone.
     */
    switch (insn->opcode) {
    case OP_add:
    case OP_sub:
    case OP_mul:
    case OP_bit_and:
    case OP_bit_or:
    case OP_bit_xor:
    case OP_lshift:
    case OP_rshift:
    case OP_negate:
    case OP_bit_not:
        break;
    default:
        return -1;
    }

    if (is_pushing_args)
        return -1;

    var_t *v = REGS[reg].var;
    if (!v || v->is_global || v->address_taken)
        return -1;
    if (check_live_out(bb, v))
        return -1;
    if (var_read_later_in_bb(bb, insn->next, v))
        return -1;
    return reg;
}

int prepare_dest(basic_block_t *bb,
                 insn_t *insn,
                 var_t *var,
                 int operand_0,
                 int operand_1)
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

    /* Reuse a dying source operand's register so the two-operand lowering
     * needs no MOV. Taking a free register instead would cost one on every
     * such instruction.
     */
    i = coalesce_candidate(bb, insn, operand_0);
    if (i < 0)
        i = coalesce_candidate(bb, insn, operand_1);
    if (i > -1) {
        vreg_clear_phys(REGS[i].var);
        REGS[i].var = var;
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

/* Count the predecessors still wired to 'bb'.  bb_disconnect() leaves holes in
 * prev[], so prev_idx is only a high-water mark and the entries must be
 * counted rather than trusted.
 */
int bb_pred_count(basic_block_t *bb)
{
    int n = 0;
    for (int i = 0; i < bb->prev_idx; i++) {
        if (bb->prev[i].bb)
            n++;
    }
    return n;
}

/* End a block the way spill_live_out() does -- every live-out variable written
 * here is flushed to its stack slot, so memory is coherent for every path out
 * of 'bb' -- but keep the register associations instead of dropping them.  What
 * survives the boundary is decided per successor by bb_export_regs().
 */
void spill_live_out_keep(basic_block_t *bb)
{
    for (int i = 0; i < REG_CNT; i++) {
        var_t *var = REGS[i].var;
        if (!var)
            continue;
        if (!check_live_out(bb, var)) {
            vreg_clear_phys(var);
            REGS[i].var = NULL;
            REGS[i].polluted = 0;
            continue;
        }
        if (REGS[i].polluted)
            store_var(bb, var, i);
    }
}

/* Hand the register file to the successor when it is the only way out of 'bb'
 * and the only way in to the successor: control then reaches it with exactly
 * these registers, so it can read them in place rather than reloading from the
 * slots just written.
 *
 * Only the fall-through edge qualifies. A branch target is emitted wherever the
 * backend's linear walk puts it, and a block the walk misses is re-emitted
 * later -- that second copy is reached with unrelated registers, so a register
 * file handed across a branch edge does not hold for every path that runs the
 * block's code. The fall-through successor is always emitted contiguously.
 */
void bb_export_regs(basic_block_t *bb)
{
    basic_block_t *succ = bb->next;

    if (!succ || bb->then_ || bb->else_)
        return;
    /* The successor must also be the block reg_alloc() visits next, so that the
     * file it inherits is the one just built here.
     */
    if (succ != bb->rpo_next || succ->has_entry_regs)
        return;
    if (bb_pred_count(succ) != 1)
        return;

    for (int i = 0; i < REG_CNT; i++)
        succ->entry_regs[i] = REGS[i].var;
    succ->has_entry_regs = true;
}

/* Install the register file this block starts with: the predecessor's file when
 * it handed one over, an empty one otherwise.  Everything handed over has just
 * been written back, so nothing is polluted on entry.
 */
void load_entry_regs(basic_block_t *bb)
{
    for (int i = 0; i < REG_CNT; i++) {
        var_t *var = bb->has_entry_regs ? bb->entry_regs[i] : NULL;

        if (REGS[i].var && REGS[i].var != var)
            vreg_clear_phys(REGS[i].var);
        REGS[i].var = var;
        REGS[i].polluted = 0;
        vreg_map_to_phys(var, i);
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

/* Return whether extra arguments are pushed onto stack. */
bool abi_lower_call_args(basic_block_t *bb, insn_t *insn)
{
    int num_of_args = 0;
    int stack_args = 0;
    while (insn && insn->opcode == OP_push) {
        num_of_args += 1;
        insn = insn->next;
    }

    if (num_of_args <= MAX_ARGS_IN_REG)
        return false;

    insn = insn->prev;
    stack_args = num_of_args - MAX_ARGS_IN_REG;
    while (stack_args) {
        load_var(bb, insn->rs1, MAX_ARGS_IN_REG - 1);
        ph2_ir_t *ir = bb_add_ph2_ir(bb, OP_store);
        ir->src0 = MAX_ARGS_IN_REG - 1;
        /* One pointer-sized slot per stack-passed argument. */
        ir->src1 = (stack_args - 1) * PTR_SIZE;
        stack_args -= 1;
        insn = insn->prev;
    }
    REGS[MAX_ARGS_IN_REG - 1].var = NULL;
    return true;
}

/* The half-open range of instruction indices in @bb over which @var holds a
 * value, written into *lo and *hi. Returns false when @var is not live
 * anywhere in @bb.
 *
 * A value live on entry starts before the first instruction; one defined here
 * starts just after the instruction that writes it, so that a value read by
 * that same instruction has already ended. It ends at its last read, or past
 * the last instruction when it is live on exit.
 */
bool var_range_in_bb(basic_block_t *bb, var_t *var, int *lo, int *hi)
{
    bool live_in = var_list_holds(&bb->live_in, var);

    /* A value the block neither receives nor writes cannot be live anywhere
     * in it: any read would have made it live on entry, and anything live on
     * exit without being written here is live on entry too. Settling that
     * from the liveness sets keeps the walk below off the blocks -- the large
     * majority -- where the variable never appears, which is what stops this
     * pass from costing a quarter of the compiler.
     */
    if (!live_in && !var_check_killed(var, bb))
        return false;

    bool live_out = var_list_holds(&bb->live_out, var);
    int def = -1, last_read = -1, n = 0;

    for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
        if (insn->rs1 == var || insn->rs2 == var)
            last_read = n;
        if (insn->rd == var && def < 0)
            def = n;
        n++;
    }

    if (live_in || def < 0)
        *lo = -1;
    else
        *lo = def + 1;

    if (live_out)
        *hi = n;
    else if (last_read >= 0)
        *hi = last_read;
    else
        *hi = *lo;
    return true;
}

/* Whether @a and @b are ever live at the same point in @func, and so cannot
 * share one stack slot.
 */
bool vars_interfere(func_t *func, var_t *a, var_t *b)
{
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        int alo, ahi, blo, bhi;

        if (!var_range_in_bb(bb, a, &alo, &ahi))
            continue;
        if (!var_range_in_bb(bb, b, &blo, &bhi))
            continue;
        if (alo <= bhi && blo <= ahi)
            return true;
    }
    return false;
}

/* Give a phi operand the same stack slot as the phi it feeds.
 *
 * Leaving SSA turns each phi into a store of the operand into the phi's slot,
 * one per predecessor. The operand is a value of its own, so it is spilled to
 * a slot of its own first, and the phi store then reads it back -- a load and
 * a store on every path into the join, and on every iteration of a loop. When
 * the two never hold values at the same time they can share one slot: the
 * operand is written straight into the phi's slot, and the copy the phi would
 * make is a load and store of the same address, which the peephole drops.
 */
/* The variables this pass has already placed, so a newcomer can be checked
 * against everything sharing the slot it is about to join rather than against
 * one member of it.
 */
var_t *phi_slot_vars[MAX_LOCALS];
int phi_slot_count;

void phi_slot_record(var_t *var)
{
    /* Dropping a member silently would leave later joiners checked against
     * only part of the group sharing their slot, so overflow has to stop the
     * compile rather than quietly produce an unsound placement.
     */
    if (phi_slot_count >= MAX_LOCALS)
        fatal("Too many coalesced phi slots");
    phi_slot_vars[phi_slot_count++] = var;
}

/* Whether @v is live at the same time as anything already sharing @offset. */
bool phi_slot_conflicts(func_t *func, int offset, var_t *v)
{
    /* Blocks are the outer loop so that @v's range in each is worked out once
     * rather than once per variable compared against; @v occupies few blocks,
     * and the rest are skipped without looking at the group at all.
     */
    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        int vlo, vhi;

        if (!var_range_in_bb(bb, v, &vlo, &vhi))
            continue;

        for (int i = 0; i < phi_slot_count; i++) {
            var_t *other = phi_slot_vars[i];
            int olo, ohi;

            if (other == v || other->offset != offset)
                continue;
            if (!var_range_in_bb(bb, other, &olo, &ohi))
                continue;
            if (vlo <= ohi && olo <= vhi)
                return true;
        }
    }
    return false;
}

bool phi_slot_compatible(var_t *d, var_t *s)
{
    if (!slot_is_private(d) || !slot_is_private(s))
        return false;
    if (var_slot_size(d) != var_slot_size(s))
        return false;
    if (is_pointer_like(d) != is_pointer_like(s))
        return false;
    return d->ofs_based_on_stack_top == s->ofs_based_on_stack_top;
}

/* Move everything sharing @from onto @to, once no member of one group is ever
 * live at the same time as a member of the other. Chained phis reach here as
 * two groups that were built independently -- "x = phi(...)" placed with its
 * own operands before "y = phi(x, ...)" was seen -- and merging them is what
 * puts a whole loop-carried variable in one slot.
 */
bool phi_slot_merge(func_t *func, int to, int from)
{
    if (to == from)
        return true;

    for (int i = 0; i < phi_slot_count; i++) {
        if (phi_slot_vars[i]->offset != to)
            continue;
        for (int j = 0; j < phi_slot_count; j++) {
            if (phi_slot_vars[j]->offset != from)
                continue;
            if (vars_interfere(func, phi_slot_vars[i], phi_slot_vars[j]))
                return false;
        }
    }

    for (int j = 0; j < phi_slot_count; j++) {
        if (phi_slot_vars[j]->offset == from)
            phi_slot_vars[j]->offset = to;
    }
    return true;
}

void coalesce_phi_slots(func_t *func)
{
    phi_slot_count = 0;

    for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
        for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
            if (insn->opcode != OP_unwound_phi)
                continue;

            var_t *d = insn->rd, *s = insn->rs1;
            if (!d || !s || d == s)
                continue;
            if (!phi_slot_compatible(d, s))
                continue;

            if (d->space_is_allocated && s->space_is_allocated) {
                phi_slot_merge(func, d->offset, s->offset);
                continue;
            }

            /* Exactly one side has a slot by now, or neither does. Chained
             * phis are met operand-first, so it may be the operand that is
             * already placed; whichever side has the slot is the one joined,
             * and when neither does the phi takes a fresh slot for the
             * operand to join.
             */
            var_t *have, *want;
            if (s->space_is_allocated) {
                have = s;
                want = d;
            } else {
                if (!d->space_is_allocated) {
                    alloc_var_slot(func, d);
                    phi_slot_record(d);
                }
                have = d;
                want = s;
            }

            if (phi_slot_conflicts(func, have->offset, want))
                continue;
            want->offset = have->offset;
            want->space_is_allocated = true;
            phi_slot_record(want);
        }
    }
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

        /* Global initializers carry no liveness information, so no operand
         * register may be reused as a destination here.
         */

        switch (global_insn->opcode) {
        case OP_allocat:
            if (global_insn->rd->array_size) {
                /* Original scheme: pointer slot + backing region. Cache the
                 * base offset of the backing region into init_val so later
                 * global initializers can address elements without loading
                 * the pointer.
                 */
                global_insn->rd->offset = GLOBAL_FUNC->stack_size;
                global_insn->rd->space_is_allocated = true;
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

                dest = prepare_dest(GLOBAL_FUNC->bbs, NULL, global_insn->rd, -1,
                                    -1);
                ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, OP_global_address_of);
                ir->src0 = src0;
                ir->dest = dest;
                ir->is_pointer = true;
                ir->size_bytes = PTR_SIZE;
                spill_var(GLOBAL_FUNC->bbs, global_insn->rd, dest);
            } else {
                global_insn->rd->offset = GLOBAL_FUNC->stack_size;
                global_insn->rd->space_is_allocated = true;
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
                    GLOBAL_FUNC->stack_size += PTR_SIZE;
            }
            break;
        case OP_load_constant:
        case OP_load_data_address:
        case OP_load_rodata_address:
            dest =
                prepare_dest(GLOBAL_FUNC->bbs, NULL, global_insn->rd, -1, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, global_insn->opcode);
            ir->src0 = global_insn->rd->init_val;
            ir->dest = dest;
            break;
        case OP_assign:
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            dest =
                prepare_dest(GLOBAL_FUNC->bbs, NULL, global_insn->rd, src0, -1);
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
                global_insn->rd->space_is_allocated = true;
                global_insn->rd->is_global = true;
                break;
            }
            /* Fallback: generate an add */
            int src1;
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            src1 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs2, src0);
            dest = prepare_dest(GLOBAL_FUNC->bbs, NULL, global_insn->rd, src0,
                                src1);
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
        case OP_trunc:
        case OP_sign_ext:
        case OP_cast:
            /* A narrowing initializer such as "char g[] = {65, 66}" reaches
             * the global block as a conversion, so it has to be lowered here
             * exactly as it is inside a function.
             */
            src0 = prepare_operand(GLOBAL_FUNC->bbs, global_insn->rs1, -1);
            dest =
                prepare_dest(GLOBAL_FUNC->bbs, NULL, global_insn->rd, src0, -1);
            ir = bb_add_ph2_ir(GLOBAL_FUNC->bbs, global_insn->opcode);
            ir->src0 = src0;
            ir->src1 = global_insn->sz;
            ir->dest = dest;
            break;
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

        slot_var_count = 0;
        coalesce_phi_slots(func);

        /* set arguments available */
        int args_in_reg = func->num_params < MAX_ARGS_IN_REG ? func->num_params
                                                             : MAX_ARGS_IN_REG;
        for (int i = 0; i < args_in_reg; i++) {
            REGS[i].var = var_subscript0(&func->param_defs[i]);
            REGS[i].polluted = 1;
        }

        /* variadic function implementation */
        if (func->va_args) {
            /* When encountering a variadic function, allocate space for all
             * arguments on the local stack to ensure their addresses are
             * contiguous.
             */
            for (int i = 0; i < MAX_PARAMS; i++) {
                ph2_ir_t *ir;
                int src0 = i;

                if (i >= MAX_ARGS_IN_REG) {
                    /* Callee should access caller's stack to obtain the
                     * extra arguments.
                     */
                    ir = bb_add_ph2_ir(func->bbs, OP_load);
                    ir->dest = MAX_ARGS_IN_REG;
                    ir->src0 = (i - MAX_ARGS_IN_REG) * PTR_SIZE;
                    ir->ofs_based_on_stack_top = true;
                    src0 = MAX_ARGS_IN_REG;
                }

                if (i < args_in_reg) {
                    var_t *param = var_subscript0(&func->param_defs[i]);
                    param->offset = func->stack_size;
                    param->space_is_allocated = true;
                }

                ir = bb_add_ph2_ir(func->bbs, OP_store);
                ir->src0 = src0;
                ir->src1 = func->stack_size;
                func->stack_size += PTR_SIZE;
            }
        } else {
            /* If the number of function arguments is fixed, the extra arguments
             * are directly placed in the caller's stack space instead of the
             * callee's.
             *
             *     +---------->  +---------------+
             *     |             | local vars    |
             *     |             +---------------+
             *     |             | extra arg 4   |
             *     |             +---------------+ <-- sp + stack_size + 12
             *  caller's space   | extra arg 3   |
             *     |             +---------------+ <-- sp + stack_size + 8
             *     |             | extra arg 2   |
             *     |             +---------------+ <-- sp + stack_size + 4
             *     |             | extra arg 1   |
             *     +---------->  +---------------+ <-- sp + stack_size
             *     |             | local vars    |
             *     |             +---------------+ <-- sp + 16
             *  callee's space   | Next callee's |
             *     |             | additional    |
             *     |             | arguments     |
             *     +---------->  +---------------+ <-- sp
             *
             * Note that:
             * - For the Arm architecture, extra arg1 ~ argX correspond to
             *   arg5 ~ arg(X + 4).
             * - For the RISC-V architecture, extra arg1 ~ argX correspond to
             *   arg9 ~ arg(X + 8).
             *
             * If any instruction use one of these additional arguments, it
             * inherits 'offset' and 'ofs_based_on_stack_top'. When calling
             * cfg_flatten(), the operand's offset will be recalculated by
             * adding the function's stack size.
             */
            for (int i = MAX_ARGS_IN_REG; i < func->num_params; i++) {
                var_t *param = var_subscript0(&func->param_defs[i]);
                param->offset = (i - MAX_ARGS_IN_REG) * PTR_SIZE;
                param->space_is_allocated = true;
                param->ofs_based_on_stack_top = true;
            }
        }

        for (basic_block_t *bb = func->bbs; bb; bb = bb->rpo_next) {
            bool handle_abi = false, args_on_stack = false;

            is_pushing_args = false;
            int args = 0;

            bb->visited++;

            /* The entry block starts with the incoming arguments already in
             * their registers; every other block takes what its predecessor
             * handed over, or nothing.
             */
            if (bb != func->bbs)
                load_entry_regs(bb);

            for (insn_t *insn = bb->insn_list.head; insn; insn = insn->next) {
                func_t *callee_func;
                ph2_ir_t *ir;
                int dest, src0, src1;
                int sz, clear_reg;

                refresh(bb, insn);

                switch (insn->opcode) {
                case OP_unwound_phi:
                    track_var_use(insn->rs1, insn->idx);

                    if (!insn->rd->space_is_allocated)
                        alloc_var_slot(bb->belong_to, insn->rd);

                    /* Sharing a slot with the phi turns the copy into a write
                     * of the operand into the place it already lives. Only a
                     * register the block has changed still needs storing --
                     * and reading the slot back first, as the general path
                     * would, is a load whose value goes straight home again.
                     */
                    if (insn->rs1->space_is_allocated &&
                        insn->rs1->offset == insn->rd->offset &&
                        insn->rs1->ofs_based_on_stack_top ==
                            insn->rd->ofs_based_on_stack_top) {
                        int held = find_in_regs(insn->rs1);

                        if (held < 0 || !REGS[held].polluted)
                            break; /* the slot already holds the value */
                        store_var(bb, insn->rs1, held);
                        break;
                    }

                    src0 = prepare_operand(bb, insn->rs1, -1);
                    ir = bb_add_ph2_ir(bb, OP_store);
                    ir->src0 = src0;
                    ir->src1 = insn->rd->offset;
                    ir->ofs_based_on_stack_top =
                        insn->rd->ofs_based_on_stack_top;
                    ir->is_pointer = is_pointer_like(insn->rd);
                    ir->size_bytes = var_slot_size(insn->rd);
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
                    insn->rd->space_is_allocated = true;
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

                    if (!insn->rd->is_global &&
                        aggregate_has_function_pointer(insn->rd->type)) {
                        insn->rd->has_backing_storage = true;
                    }

                    dest = prepare_dest(bb, insn, insn->rd, -1, -1);
                    ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = src0;
                    ir->dest = dest;
                    ir->ofs_based_on_stack_top =
                        insn->rd->ofs_based_on_stack_top;

                    /* For arrays, store the base address just like global
                     * arrays do
                     */
                    if (insn->rd->array_size)
                        spill_var(bb, insn->rd, dest);
                    break;
                case OP_load_constant:
                case OP_load_data_address:
                case OP_load_rodata_address:
                    dest = prepare_dest(bb, insn, insn->rd, -1, -1);
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

                    /* OP_allocat puts a local aggregate's spill slot before
                     * its backing storage. &aggregate must name the backing
                     * storage, not the spill slot.
                     *
                     * FIXME: This does not support aggregate parameter for now.
                     */
                    bool is_pointer =
                        insn->rs1->ptr_level ||
                        (insn->rs1->type && insn->rs1->type->ptr_level);
                    if (!insn->rs1->is_global && !is_pointer &&
                        aggregate_has_function_pointer(insn->rs1->type)) {
                        if (!insn->rs1->has_backing_storage) {
                            insn->rs1->offset = func->stack_size;
                            insn->rs1->space_is_allocated = true;
                            insn->rs1->ofs_based_on_stack_top = false;
                            func->stack_size += PTR_SIZE;
                            if (insn->rs1->ptr_level)
                                sz = PTR_SIZE;
                            else
                                sz = insn->rs1->type->size;
                            if (insn->rs1->array_size)
                                func->stack_size +=
                                    align_size(insn->rs1->array_size * sz);
                            else
                                func->stack_size += align_size(sz);
                            insn->rs1->has_backing_storage = true;
                        }

                        dest = prepare_dest(bb, insn, insn->rd, -1, -1);
                        ir = bb_add_ph2_ir(bb, OP_address_of);
                        ir->src0 = insn->rs1->offset + PTR_SIZE;
                        ir->dest = dest;
                        ir->ofs_based_on_stack_top =
                            insn->rs1->ofs_based_on_stack_top;
                        break;
                    }

                    /* make sure variable is on stack */
                    if (!insn->rs1->space_is_allocated) {
                        alloc_var_slot(bb->belong_to, insn->rs1);

                        for (int i = 0; i < REG_CNT; i++)
                            if (REGS[i].var == insn->rs1) {
                                ir = bb_add_ph2_ir(bb, OP_store);
                                ir->src0 = i;
                                ir->src1 = insn->rs1->offset;
                                ir->ofs_based_on_stack_top =
                                    insn->rs1->ofs_based_on_stack_top;
                                /* Clear stale register tracking */
                                REGS[i].var = NULL;
                            }
                    }

                    dest = prepare_dest(bb, insn, insn->rd, -1, -1);
                    if (insn->rs1->is_global ||
                        insn->opcode == OP_global_address_of)
                        ir = bb_add_ph2_ir(bb, OP_global_address_of);
                    else
                        ir = bb_add_ph2_ir(bb, OP_address_of);
                    ir->src0 = insn->rs1->offset;
                    ir->dest = dest;
                    ir->ofs_based_on_stack_top =
                        insn->rs1->ofs_based_on_stack_top;
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
                    dest = prepare_dest(bb, insn, insn->rd, src0, -1);
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
                    dest = prepare_dest(bb, insn, insn->rd, src0, -1);
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
                        if (dynlink) {
                            func_t *target_fn = find_func(ir->func_name);
                            if (target_fn)
                                target_fn->is_used = true;
                        }
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
                    if (!handle_abi) {
                        args_on_stack = abi_lower_call_args(bb, insn);
                        handle_abi = true;
                    }

                    if (args_on_stack && args >= MAX_ARGS_IN_REG)
                        break;

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

                    if (dynlink)
                        callee_func->is_used = true;

                    ir = bb_add_ph2_ir(bb, OP_call);
                    strcpy(ir->func_name, insn->str);

                    is_pushing_args = false;
                    args = 0;
                    handle_abi = false;

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
                    handle_abi = false;

                    for (int i = 0; i < REG_CNT; i++)
                        REGS[i].var = NULL;
                    break;
                case OP_func_ret:
                    dest = prepare_dest(bb, insn, insn->rd, -1, -1);
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
                    dest = prepare_dest(bb, insn, insn->rd, src0, src1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src0 = src0;
                    ir->src1 = src1;
                    ir->dest = dest;
                    /* Record whether the result is an address. On LP64 an
                     * int-typed result has to wrap at 32 bits, while a
                     * pointer must keep all 64. The backend cannot tell the
                     * two apart without this.
                     */
                    ir->is_pointer = is_pointer_like(insn->rd) ||
                                     is_pointer_like(insn->rs1) ||
                                     is_pointer_like(insn->rs2);
                    break;
                case OP_negate:
                case OP_bit_not:
                case OP_log_not:
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    dest = prepare_dest(bb, insn, insn->rd, src0, -1);
                    ir = bb_add_ph2_ir(bb, insn->opcode);
                    ir->src0 = src0;
                    ir->dest = dest;
                    break;
                case OP_trunc:
                case OP_sign_ext:
                case OP_cast:
                    src0 = prepare_operand(bb, insn->rs1, -1);
                    dest = prepare_dest(bb, insn, insn->rd, src0, -1);
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

            if (bb->next) {
                spill_live_out_keep(bb);
                bb_export_regs(bb);
            }

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
        for (int i = 0; i < func->exit->prev_idx; i++) {
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

        slot_scan(func);
        collapse_slot_roundtrip(func);
        dead_store_elim(func);
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
