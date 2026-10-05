#include "vir-lower.h"

typedef struct {
    int slot;
    int reg;
    int high;
    bool dirty;
    int end;
    int fixed;
} vir_location_t;

typedef struct {
    const vir_function_t *function;
    vir_location_t *locations;
    basic_block_t **blocks;
    basic_block_t *block;
    basic_block_t *tail;
    int owners[REG_CNT];
    unsigned int locked;
    unsigned int pinned_mask;
    vir_value_t *pins[REG_CNT];
    int pin_count;
    int frame;
    int copy_slot;
    int position;
    int *block_positions;
    int (*global_offset)(const char *, void *);
    void *global_context;
} vir_lower_t;

static int vir_machine_width(vir_lower_t *lower, vir_type_t type)
{
    int bits = type == VIR_TYPE_PTR ? lower->function->pointer_bits
                                    : vir_integer_type_width(type);
    return bits < 8 ? 1 : bits / 8;
}

static bool vir_machine_pair(vir_lower_t *lower, vir_value_t *value)
{
    return PTR_SIZE == 4 && vir_machine_width(lower, value->type) == 8;
}

static ph2_ir_t *vir_machine_insn(vir_lower_t *lower, opcode_t opcode)
{
    return bb_add_ph2_ir(lower->block, opcode);
}

static basic_block_t *vir_machine_block(vir_lower_t *lower)
{
    basic_block_t *block = arena_alloc_bb();
    block->elf_offset = -1;
    block->ph2_base = -1;
    if (lower->tail)
        lower->tail->rpo_next = block;
    lower->tail = block;
    return block;
}

static void vir_machine_spill(vir_lower_t *lower, int reg)
{
    int id = lower->owners[reg];
    vir_location_t *location;
    ph2_ir_t *instruction;

    if (id < 0)
        return;
    location = &lower->locations[id];
    if (location->dirty && location->end >= lower->position) {
        instruction = vir_machine_insn(lower, OP_store);
        instruction->src0 = location->reg;
        instruction->src0_hi = location->high;
        instruction->src1 = location->slot;
        instruction->size_bytes = location->high < 0 ? PTR_SIZE : 8;
        location->dirty = false;
    }
    lower->owners[location->reg] = -1;
    if (location->high >= 0)
        lower->owners[location->high] = -1;
    location->reg = -1;
    location->high = -1;
}

static void vir_machine_flush(vir_lower_t *lower)
{
    for (int reg = 0; reg < REG_CNT; reg++)
        vir_machine_spill(lower, reg);
}

static void vir_machine_expire(vir_lower_t *lower)
{
    for (int reg = 0; reg < REG_CNT; reg++)
        if (lower->owners[reg] >= 0 &&
            lower->locations[lower->owners[reg]].end < lower->position)
            vir_machine_spill(lower, reg);
}

static int vir_machine_claim(vir_lower_t *lower, vir_value_t *value)
{
    vir_location_t *location = &lower->locations[value->id];
    int words = vir_machine_pair(lower, value) ? 2 : 1;
    int selected = -1;

    if (location->reg >= 0)
        return location->reg;
    for (int reg = 0; reg + words <= REG_CNT; reg++) {
        if (words == 2 && (reg & 1))
            continue;
        unsigned int mask = ((1u << words) - 1) << reg;
        if ((lower->locked | lower->pinned_mask) & mask)
            continue;
        if (selected < 0)
            selected = reg;
        if (lower->owners[reg] < 0 &&
            (words == 1 || lower->owners[reg + 1] < 0)) {
            selected = reg;
            break;
        }
    }
    if (selected < 0)
        fatal("VIR: insufficient machine registers");
    for (int word = 0; word < words; word++)
        vir_machine_spill(lower, selected + word);
    location->reg = selected;
    location->high = words == 2 ? selected + 1 : -1;
    for (int word = 0; word < words; word++)
        lower->owners[selected + word] = value->id;
    return selected;
}

static int vir_machine_read(vir_lower_t *lower, vir_value_t *value)
{
    vir_location_t *location = &lower->locations[value->id];
    int reg = location->reg;
    ph2_ir_t *instruction;

    if (reg < 0) {
        reg = vir_machine_claim(lower, value);
        instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = reg;
        instruction->dest_hi = location->high;
        instruction->src0 = location->slot;
        instruction->size_bytes = vir_machine_width(lower, value->type);

        /* VIR values are bitvectors. Signed operations declare signedness at
         * their consuming instruction, while reloads preserve all bits.
         */
        instruction->is_unsigned = true;
    }
    lower->locked |= 1u << reg;
    if (location->high >= 0)
        lower->locked |= 1u << location->high;
    return reg;
}

static void vir_machine_result(vir_lower_t *lower,
                               ph2_ir_t *instruction,
                               vir_value_t *value)
{
    instruction->dest = vir_machine_claim(lower, value);
    instruction->dest_hi = lower->locations[value->id].high;
    instruction->size_bytes = vir_machine_width(lower, value->type);
    instruction->is_pointer = value->type == VIR_TYPE_PTR;
    lower->locations[value->id].dirty = true;
}

static void vir_machine_normalize(vir_lower_t *lower, vir_value_t *value)
{
    ph2_ir_t *instruction;
    if (value->type == VIR_TYPE_I1) {
        int result = lower->locations[value->id].reg;
        lower->locked |= 1u << result;
        int scratch = 0;
        while ((lower->locked | lower->pinned_mask) & (1u << scratch))
            scratch++;
        vir_machine_spill(lower, scratch);
        instruction = vir_machine_insn(lower, OP_load_constant);
        instruction->dest = scratch;
        instruction->src0 = 1;
        instruction->size_bytes = 4;
        instruction = vir_machine_insn(lower, OP_bit_and);
        instruction->src0 = result;
        instruction->src1 = scratch;
        vir_machine_result(lower, instruction, value);
    } else if (value->type == VIR_TYPE_I8 || value->type == VIR_TYPE_I16) {
        instruction = vir_machine_insn(lower, OP_trunc);
        instruction->src0 = lower->locations[value->id].reg;
        instruction->src1 = vir_machine_width(lower, value->type);
        vir_machine_result(lower, instruction, value);
        instruction->is_unsigned = true;
    }
}

static opcode_t vir_machine_opcode(vir_opcode_t opcode)
{
    switch (opcode) {
    case VIR_OP_ADD:
    case VIR_OP_PTRADD:
        return OP_add;
    case VIR_OP_SUB:
        return OP_sub;
    case VIR_OP_MUL:
        return OP_mul;
    case VIR_OP_SDIV:
    case VIR_OP_UDIV:
        return OP_div;
    case VIR_OP_SREM:
    case VIR_OP_UREM:
        return OP_mod;
    case VIR_OP_SHL:
        return OP_lshift;
    case VIR_OP_ASHR:
    case VIR_OP_LSHR:
        return OP_rshift;
    case VIR_OP_BITAND:
        return OP_bit_and;
    case VIR_OP_BITOR:
        return OP_bit_or;
    case VIR_OP_BITXOR:
        return OP_bit_xor;
    case VIR_OP_EQ:
        return OP_eq;
    case VIR_OP_SLT:
    case VIR_OP_ULT:
        return OP_lt;
    case VIR_OP_NEG:
        return OP_negate;
    case VIR_OP_BITNOT:
        return OP_bit_not;
    case VIR_OP_TRUNC:
        return OP_trunc;
    case VIR_OP_SEXT:
    case VIR_OP_ZEXT:
        return OP_sign_ext;
    case VIR_OP_PTRTOINT:
    case VIR_OP_INTTOPTR:
        return OP_cast;
    default:
        return OP_load_constant;
    }
}

static bool vir_machine_unsigned(vir_opcode_t opcode)
{
    return opcode == VIR_OP_UDIV || opcode == VIR_OP_UREM ||
           opcode == VIR_OP_LSHR || opcode == VIR_OP_ULT ||
           opcode == VIR_OP_ZEXT;
}

static void vir_machine_value(vir_lower_t *lower, vir_value_t *value)
{
    ph2_ir_t *instruction;
    int first = -1, second = -1;

    if (value->is_block_param || value->def_effect)
        return;
    lower->locked = 0;
    vir_machine_expire(lower);
    if (value->op0)
        first = vir_machine_read(lower, value->op0);
    if (value->op1)
        second = vir_machine_read(lower, value->op1);
    int first_high = value->op0 ? lower->locations[value->op0->id].high : -1;
    int second_high = value->op1 ? lower->locations[value->op1->id].high : -1;

    /* Reuse a dying input of the same width: no move or spill is needed to
     * preserve it, and pair operations keep both halves in their original
     * order.
     */
    if (value->op0 && lower->locations[value->op0->id].fixed < 0 &&
        lower->locations[value->id].fixed < 0 &&
        lower->locations[value->op0->id].end == lower->position &&
        vir_machine_width(lower, value->op0->type) ==
            vir_machine_width(lower, value->type)) {
        vir_location_t *old = &lower->locations[value->op0->id];
        vir_location_t *result = &lower->locations[value->id];
        result->reg = first;
        result->high = first_high;
        old->reg = old->high = -1;
        old->dirty = false;
        lower->owners[first] = value->id;
        if (first_high >= 0)
            lower->owners[first_high] = value->id;
    }
    /* Claim before appending the operation: eviction must precede the write. */
    vir_machine_claim(lower, value);
    instruction = vir_machine_insn(lower, vir_machine_opcode(value->opcode));
    vir_machine_result(lower, instruction, value);
    if (first >= 0) {
        instruction->src0 = first;
        instruction->src0_hi = first_high;
        instruction->src0_is_pointer = value->op0->type == VIR_TYPE_PTR;
    }
    if (second >= 0) {
        instruction->src1 = second;
        instruction->src1_hi = second_high;
        instruction->src1_is_pointer = value->op1->type == VIR_TYPE_PTR;
    }
    if (value->opcode == VIR_OP_EQ || value->opcode == VIR_OP_SLT ||
        value->opcode == VIR_OP_ULT)
        instruction->size_bytes = vir_machine_width(lower, value->op0->type);
    instruction->is_unsigned = vir_machine_unsigned(value->opcode);
    instruction->src0_is_unsigned = instruction->is_unsigned;
    instruction->src1_is_unsigned = instruction->is_unsigned;
    if (value->opcode == VIR_OP_CONST) {
        instruction->src0 = (int) value->constant;
        instruction->src1 = (int) (value->constant >> 32);
        instruction->is_unsigned = true;
    } else if (value->opcode == VIR_OP_STACK_ADDR) {
        instruction->op = OP_address_of;
        instruction->src0 = lower->locations[value->id].slot;
    } else if (value->opcode == VIR_OP_GLOBAL_ADDR) {
        instruction->op = OP_global_address_of;
        instruction->src0 =
            lower->global_offset(value->address_name, lower->global_context);
    } else if (value->opcode == VIR_OP_FUNC_ADDR) {
        /* Existing PH2 function relocation writes through an address. Reuse it
         * without constructing a source variable or source instruction.
         */
        instruction->op = OP_address_of;
        instruction->src0 = lower->locations[value->id].slot;
        instruction = vir_machine_insn(lower, OP_address_of_func);
        instruction->src0 = lower->locations[value->id].reg;
        instruction->func_name = intern_string((char *) value->address_name);
        instruction = vir_machine_insn(lower, OP_load);
        vir_machine_result(lower, instruction, value);
        instruction->src0 = lower->locations[value->id].slot;
        lower->locations[value->id].dirty = false;
    } else if (value->opcode == VIR_OP_RODATA_ADDR) {
        instruction->op = OP_load_rodata_address;
        instruction->src0 = (int) value->constant;
    } else if (value->opcode == VIR_OP_TRUNC) {
        instruction->src1 = vir_machine_width(lower, value->type);
        instruction->is_unsigned = true;
    } else if (value->opcode == VIR_OP_ZEXT || value->opcode == VIR_OP_SEXT) {
        instruction->src1 = (vir_machine_width(lower, value->op0->type) << 16) |
                            vir_machine_width(lower, value->type);
    }
    if ((value->opcode == VIR_OP_SEXT || value->opcode == VIR_OP_ZEXT) &&
        value->op0->type == VIR_TYPE_I1) {
        instruction->op = OP_cast;
        instruction->src0_is_unsigned = true;
        instruction->is_unsigned = true;
        if (value->opcode == VIR_OP_SEXT) {
            instruction = vir_machine_insn(lower, OP_negate);
            instruction->src0 = lower->locations[value->id].reg;
            instruction->src0_hi = lower->locations[value->id].high;
            vir_machine_result(lower, instruction, value);
        }
    }
    if (value->opcode == VIR_OP_SEXT &&
        vir_machine_width(lower, value->type) < 4) {
        instruction = vir_machine_insn(lower, OP_trunc);
        instruction->src0 = lower->locations[value->id].reg;
        instruction->src1 = vir_machine_width(lower, value->type);
        vir_machine_result(lower, instruction, value);
        instruction->is_unsigned = true;
    }
    if (value->opcode == VIR_OP_TRUNC && value->type == VIR_TYPE_I1)
        vir_machine_normalize(lower, value);
    lower->locked = 0;
}

/* A shared register is safe only when neither definition can overwrite a
 * still-used instance of the other value. Re-entering its defining block starts
 * a new SSA instance; the initial block needs instruction ordering.
 */
static bool vir_machine_interferes(vir_lower_t *lower,
                                   vir_value_t *writer,
                                   vir_value_t *survivor,
                                   int *seen,
                                   vir_block_t **work)
{
    memset(seen, 0, lower->function->next_block_id * sizeof(int));
    int pending = 0;
    const vir_block_t *block = writer->block;
    int start = writer->is_block_param ? -1 : writer->order;
    bool initial = true;
    while (block) {
        int stop = block->next_order + 1;
        if (block == survivor->block) {
            int definition = survivor->is_block_param ? -1 : survivor->order;
            if (!initial || definition > start)
                stop = definition;
        }
        for (vir_use_t *use = survivor->uses; use; use = use->next) {
            const vir_block_t *used = vir_use_block(use);
            int order = vir_use_order(use);
            if (used == block && order > start && order < stop)
                return true;
        }
        if (stop > block->next_order)
            for (vir_edge_t *edge = block->outgoing; edge;
                 edge = edge->next_outgoing)
                if (!seen[edge->to->id]) {
                    seen[edge->to->id] = 1;
                    work[pending++] = edge->to;
                }
        block = pending ? work[--pending] : NULL;
        initial = false;
        start = -1;
    }
    return false;
}

static bool vir_machine_dominated(vir_lower_t *lower,
                                  vir_block_t *header,
                                  vir_block_t *target,
                                  int *seen,
                                  vir_block_t **work)
{
    memset(seen, 0, lower->function->next_block_id * sizeof(int));
    int pending = 0;
    vir_block_t *entry = lower->function->blocks;
    if (entry != header) {
        seen[entry->id] = 1;
        work[pending++] = entry;
    }
    while (pending) {
        const vir_block_t *block = work[--pending];
        if (block == target)
            return false;
        for (vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing)
            if (edge->to != header && !seen[edge->to->id]) {
                seen[edge->to->id] = 1;
                work[pending++] = edge->to;
            }
    }
    return true;
}

/* Build conservative intervals from actual SSA uses and predecessor paths. Back
 * edges extend intervals through the loop, while short-lived values in a
 * straight block reuse the same homes. Buckets avoid sorting by source IDs.
 */
static bool vir_machine_slots(vir_lower_t *lower)
{
    const vir_function_t *function = lower->function;
    int count = function->next_value_id ? function->next_value_id : 1;
    int positions = 0;
    int slots = 0, free_count = 0;
    vir_value_t **values = calloc(count, sizeof(vir_value_t *));
    int *seen = calloc(function->next_block_id, sizeof(int));
    vir_block_t **work =
        malloc(function->next_block_id * sizeof(vir_block_t *));
    int *free_slots = malloc(count * sizeof(int));
    int *next_start = malloc(count * sizeof(int));
    int *next_end = malloc(count * sizeof(int));
    int *heads = NULL, *ends = NULL;
    bool success = false;

    lower->block_positions = calloc(function->next_block_id, sizeof(int));
    if (!values || !seen || !work || !free_slots || !next_start || !next_end ||
        !lower->block_positions)
        goto done;
    for (vir_block_t *block = function->blocks; block; block = block->next) {
        lower->block_positions[block->id] = positions;
        positions += block->next_order + 1;
        for (vir_value_t *value = block->params; value;
             value = value->param_next)
            values[value->id] = value;
        for (vir_value_t *value = block->head; value; value = value->next)
            values[value->id] = value;
    }
    heads = malloc(positions * sizeof(int));
    ends = malloc((positions + 1) * sizeof(int));
    if (!heads || !ends)
        goto done;
    for (int position = 0; position < positions; position++)
        heads[position] = ends[position] = -1;
    ends[positions] = -1;
    for (int id = 0; id < count; id++) {
        vir_value_t *value = values[id];
        if (!value)
            continue;
        int start = lower->block_positions[value->block->id] +
                    (value->is_block_param ? 0 : value->order);
        int end = start;
        int pending = 0;
        for (vir_use_t *use = value->uses; use; use = use->next) {
            vir_block_t *block = vir_use_block(use);
            int used = lower->block_positions[block->id] + vir_use_order(use);
            if (used > end)
                end = used;
            if (used < start)
                start = used;
            if (block != value->block && seen[block->id] != id + 1) {
                seen[block->id] = id + 1;
                work[pending++] = block;
            }
        }
        while (pending) {
            const vir_block_t *block = work[--pending];
            int begin = lower->block_positions[block->id];
            int finish = begin + block->next_order;
            if (block != value->block && begin < start)
                start = begin;
            if (finish > end)
                end = finish;
            if (block == value->block)
                continue;
            for (vir_edge_t *edge = block->incoming; edge;
                 edge = edge->next_incoming)
                if (seen[edge->from->id] != id + 1) {
                    seen[edge->from->id] = id + 1;
                    work[pending++] = edge->from;
                }
        }
        lower->locations[id].end = end;
        next_start[id] = heads[start];
        heads[start] = id;
        next_end[id] = ends[end + 1];
        ends[end + 1] = id;
    }

    /* A few canonical callee registers remove loop-edge traffic without
     * changing the conservative homes or local scratch allocator.
     */
    int base = REG_CNT - CALLEE_SAVED_REGS;
    int limit = REG_CNT - 1;
    if (limit - base > REG_CNT - 6)
        limit = base + REG_CNT - 6;
    for (int reg = base; reg < limit;) {
        vir_value_t *best = NULL;
        int best_score = 0;
        for (int id = 0; id < count; id++) {
            vir_value_t *value = values[id];
            if (!value || lower->locations[id].fixed >= 0 ||
                value->opcode == VIR_OP_CALL ||
                reg + (vir_machine_pair(lower, value) ? 2 : 1) > limit)
                continue;
            int score = 0;
            for (vir_use_t *use = value->uses; use; use = use->next) {
                const vir_block_t *block = vir_use_block(use);
                if (block != value->block)
                    score += 16;
            }
            if (!score)
                continue;
            if (value->is_block_param)
                for (vir_edge_t *edge = value->block->incoming; edge;
                     edge = edge->next_incoming) {
                    int span = lower->block_positions[edge->from->id] -
                               lower->block_positions[value->block->id];
                    if (span >= 0)
                        score += 100000 + 65536 / (span + 1);
                }
            if (score > best_score) {
                best = value;
                best_score = score;
            }
        }
        if (!best)
            break;
        vir_location_t *location = &lower->locations[best->id];
        int words = vir_machine_pair(lower, best) ? 2 : 1;
        location->fixed = location->reg = reg;
        location->high = words == 2 ? reg + 1 : -1;
        lower->pinned_mask |= ((1u << words) - 1) << reg;
        lower->pins[lower->pin_count++] = best;
        reg += words;
    }

    /* Phi affinity extends a pin through values feeding the same family.
     * Checking both directions against every member prevents alternate branch
     * results from overwriting one another before their outgoing copies.
     */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int id = 0; id < count; id++) {
            vir_value_t *value = values[id];
            if (!value || !value->uses || lower->locations[id].fixed >= 0 ||
                value->def_effect)
                continue;
            int fixed = -1;
            bool safe = true;
            for (vir_use_t *use = value->uses; use; use = use->next) {
                if (!use->edge)
                    continue;
                vir_value_t *param = use->edge->to->params;
                for (int index = 0; param && index < use->operand; index++)
                    param = param->param_next;
                if (!param || param->type != value->type ||
                    lower->locations[param->id].fixed < 0) {
                    safe = false;
                    break;
                }
                int destination = lower->locations[param->id].fixed;
                if (fixed >= 0 && fixed != destination) {
                    safe = false;
                    break;
                }
                fixed = destination;
            }
            if (!safe)
                continue;
            vir_value_t *root = NULL;
            for (int index = 0; index < lower->pin_count; index++)
                if (lower->locations[lower->pins[index]->id].fixed == fixed)
                    root = lower->pins[index];
            if (!root || !root->is_block_param ||
                !vir_machine_dominated(lower, root->block, value->block, seen,
                                       work))
                continue;
            for (int other = 0; other < count && safe; other++)
                if (values[other] && lower->locations[other].fixed == fixed)
                    safe = !vir_machine_interferes(lower, value, values[other],
                                                   seen, work) &&
                           !vir_machine_interferes(lower, values[other], value,
                                                   seen, work);
            if (!safe)
                continue;
            lower->locations[id].fixed = lower->locations[id].reg = fixed;
            lower->locations[id].high =
                vir_machine_pair(lower, value) ? fixed + 1 : -1;
            changed = true;
        }
    }
    for (int position = 0; position < positions; position++) {
        for (int id = ends[position]; id >= 0; id = next_end[id])
            free_slots[free_count++] =
                (lower->locations[id].slot - lower->frame) / 8;
        for (int id = heads[position]; id >= 0; id = next_start[id]) {
            int slot = free_count ? free_slots[--free_count] : slots++;
            lower->locations[id].slot = lower->frame + slot * 8;
        }
    }
    lower->frame += slots * 8;
    success = true;
done:
    free(values);
    free(seen);
    free(work);
    free(free_slots);
    free(next_start);
    free(next_end);
    free(heads);
    free(ends);
    return success;
}

static int vir_machine_arg_start(vir_lower_t *lower,
                                 int cursor,
                                 vir_value_t *value,
                                 bool variadic)
{
    if (vir_machine_pair(lower, value) &&
        (ELF_MACHINE != 0xf3 || variadic || cursor >= MAX_ARGS_IN_REG))
        return ALIGN_UP(cursor, 2);
    return cursor;
}

static void vir_machine_pin_parameters(vir_lower_t *lower)
{
    for (int index = 0; index < lower->pin_count; index++) {
        vir_value_t *value = lower->pins[index];
        if (!value->is_block_param || value->block != lower->function->blocks)
            continue;
        vir_location_t *location = &lower->locations[value->id];
        ph2_ir_t *instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = location->fixed;
        instruction->dest_hi = location->high;
        instruction->src0 = location->slot;
        instruction->size_bytes = vir_machine_width(lower, value->type);
        instruction->is_unsigned = true;
    }
}

static void vir_machine_call(vir_lower_t *lower, vir_effect_t *effect)
{
    int cursor = 0;
    ph2_ir_t *instruction;

    /* Make every live value recoverable before argument registers are reused.
     * Staging reads directly from homes, so pair moves cannot overwrite another
     * argument that has yet to be loaded.
     */
    vir_machine_flush(lower);
    for (int index = -1; index < effect->arg_count; index++) {
        vir_value_t *argument =
            index < 0 ? effect->callee_value : effect->args[index];
        if (!argument)
            continue;
        vir_location_t *location = &lower->locations[argument->id];
        if (location->fixed < 0)
            continue;
        instruction = vir_machine_insn(lower, OP_store);
        instruction->src0 = location->fixed;
        instruction->src0_hi = location->high;
        instruction->src1 = location->slot;
        instruction->size_bytes = vir_machine_width(lower, argument->type);
    }

    /* Stack arguments precede register arguments, so no staging scratch value
     * can overwrite an argument already placed for the call.
     */
    for (int pass = 0; pass < 2; pass++) {
        cursor = 0;
        for (int index = 0; index < effect->arg_count; index++) {
            vir_value_t *argument = effect->args[index];
            int words = vir_machine_pair(lower, argument) ? 2 : 1;
            cursor = vir_machine_arg_start(
                lower, cursor, argument,
                effect->signature->is_variadic &&
                    index >= effect->signature->fixed_param_count);
            for (int word = 0; word < words; word++) {
                int abi_word = cursor + word;
                bool on_stack = abi_word >= MAX_ARGS_IN_REG;
                int reg = on_stack ? REG_CNT - 1 : abi_word;
                if (on_stack != (pass == 0))
                    continue;
                instruction = vir_machine_insn(lower, OP_load);
                instruction->dest = reg;
                instruction->src0 =
                    lower->locations[argument->id].slot + word * PTR_SIZE;
                instruction->size_bytes =
                    words == 2 ? 4 : vir_machine_width(lower, argument->type);
                instruction->is_unsigned =
                    effect->signature->params[index].is_unsigned || words == 2;
                if (on_stack) {
                    instruction = vir_machine_insn(lower, OP_store);
                    instruction->src0 = reg;
                    instruction->src1 = (abi_word - MAX_ARGS_IN_REG) * PTR_SIZE;
                    instruction->size_bytes = PTR_SIZE;
                }
            }
            cursor += words;
        }
    }
    if (effect->callee_value) {
        instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = REG_CNT - 1;
        instruction->src0 = lower->locations[effect->callee_value->id].slot;
        instruction = vir_machine_insn(lower, OP_load_func);
        instruction->src0 = REG_CNT - 1;
    }
    if (effect->callee_value)
        vir_machine_insn(lower, OP_indirect);
    else {
        instruction = vir_machine_insn(lower, OP_call);
        instruction->func_name = intern_string((char *) effect->callee);
    }
    if (effect->result) {
        vir_location_t *location = &lower->locations[effect->result->id];
        location->reg = 0;
        location->high = vir_machine_pair(lower, effect->result) ? 1 : -1;
        location->dirty = true;
        lower->owners[0] = effect->result->id;
        if (location->high >= 0)
            lower->owners[1] = effect->result->id;
        lower->locked = 0;
        vir_machine_normalize(lower, effect->result);
        lower->locked = 0;
    }
}

static void vir_machine_effect(vir_lower_t *lower, vir_effect_t *effect)
{
    ph2_ir_t *instruction;
    int address, stored = -1;

    if (effect->kind == VIR_EFFECT_CALL) {
        vir_machine_call(lower, effect);
        return;
    }
    if (!effect->address)
        return;
    lower->locked = 0;
    address = vir_machine_read(lower, effect->address);
    if (effect->stored_value)
        stored = vir_machine_read(lower, effect->stored_value);
    if (effect->result)
        vir_machine_claim(lower, effect->result);
    instruction = vir_machine_insn(lower, effect->result ? OP_read : OP_write);
    instruction->src0 = address;
    instruction->is_volatile = effect->kind == VIR_EFFECT_VOLATILE_LOAD ||
                               effect->kind == VIR_EFFECT_VOLATILE_STORE;
    if (effect->result) {
        vir_machine_result(lower, instruction, effect->result);
        instruction->src1 = vir_machine_width(lower, effect->result->type);
        instruction->is_unsigned = true;
    } else {
        instruction->src1 = stored;
        instruction->src1_hi = lower->locations[effect->stored_value->id].high;
        instruction->size_bytes =
            vir_machine_width(lower, effect->stored_value->type);
        instruction->dest = instruction->size_bytes;
    }
    lower->locked = 0;
}

/* Copy a bitvector between a register (or pair) and its stack home. */
static void vir_machine_copy(vir_lower_t *lower,
                             vir_value_t *value,
                             int from,
                             int source,
                             int to,
                             int destination)
{
    bool pair = vir_machine_pair(lower, value);
    if (from < 0) {
        ph2_ir_t *instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = to < 0 ? 0 : to;
        instruction->dest_hi = pair ? instruction->dest + 1 : -1;
        instruction->src0 = source;
        instruction->size_bytes = vir_machine_width(lower, value->type);
        instruction->is_unsigned = true;
        if (to >= 0)
            return;
        from = 0;
    }
    ph2_ir_t *instruction =
        vir_machine_insn(lower, to < 0 ? OP_store : OP_assign);
    instruction->src0 = from;
    instruction->src0_hi = pair ? from + 1 : -1;
    instruction->size_bytes = vir_machine_width(lower, value->type);
    instruction->is_unsigned = true;
    if (to < 0)
        instruction->src1 = destination;
    else {
        instruction->dest = to;
        instruction->dest_hi = pair ? to + 1 : -1;
    }
}

/* Parallel register copies use a scratch outside every live source. */
static void vir_machine_parallel(vir_lower_t *lower,
                                 vir_edge_t *edge,
                                 bool resident,
                                 bool snapshot)
{
    vir_value_t *source[REG_CNT];
    int dest[REG_CNT], from[REG_CNT], slot[REG_CNT], count = 0;
    vir_value_t *parameter = edge->to->params;
    for (int index = 0; index < edge->arg_count;
         index++, parameter = parameter->param_next) {
        vir_value_t *argument = edge->args[index];
        vir_location_t *location = &lower->locations[argument->id];
        int target = lower->locations[parameter->id].fixed;
        int reg = snapshot ? -1 : resident ? location->reg : location->fixed;
        if (target < 0 || target == reg)
            continue;
        source[count] = argument;
        dest[count] = target;
        from[count] = reg;
        slot[count++] =
            snapshot ? lower->copy_slot + index * 8 : location->slot;
    }
    while (count) {
        int ready = -1;
        for (int index = 0; index < count; index++) {
            bool blocked = false;
            for (int other = 0; other < count; other++)
                if (from[other] == dest[index])
                    blocked = true;
            if (!blocked) {
                ready = index;
                break;
            }
        }
        if (ready < 0) {
            int reg = from[0], scratch = 0;
            unsigned int occupied = lower->pinned_mask;
            for (int index = 0; index < count; index++)
                if (from[index] >= 0)
                    occupied |=
                        ((1u
                          << (vir_machine_pair(lower, source[index]) ? 2 : 1)) -
                         1)
                        << from[index];
            int words = vir_machine_pair(lower, source[0]) ? 2 : 1;
            while (occupied & (((1u << words) - 1) << scratch))
                scratch++;
            vir_machine_copy(lower, source[0], reg, -1, scratch, -1);
            for (int index = 0; index < count; index++)
                if (from[index] == reg)
                    from[index] = scratch;
            continue;
        }
        vir_machine_copy(lower, source[ready], from[ready], slot[ready],
                         dest[ready], -1);
        count--;
        source[ready] = source[count];
        dest[ready] = dest[count];
        from[ready] = from[count];
        slot[ready] = slot[count];
    }
}

/* Only overlapping memory destinations need snapshots. All-register edges
 * consume the predecessor's resident values without roundtrips through homes.
 */
static basic_block_t *vir_machine_edge(vir_lower_t *lower, vir_edge_t *edge)
{
    if (!edge->arg_count)
        return lower->blocks[edge->to->id];
    bool resident = true, overlap = false, inplace = true;
    int argument_index = 0;
    for (vir_value_t *parameter = edge->to->params; parameter;
         parameter = parameter->param_next, argument_index++) {
        vir_location_t *target = &lower->locations[parameter->id];
        if (target->fixed >= 0) {
            if (lower->locations[edge->args[argument_index]->id].reg !=
                target->fixed)
                inplace = false;
            continue;
        }
        resident = inplace = false;
        int width = vir_machine_width(lower, parameter->type);
        for (int index = 0; index < edge->arg_count; index++) {
            vir_value_t *argument = edge->args[index];
            vir_location_t *source = &lower->locations[argument->id];
            if (source->fixed < 0 &&
                target->slot <
                    source->slot + vir_machine_width(lower, argument->type) &&
                source->slot < target->slot + width)
                overlap = true;
        }
    }
    if (inplace)
        return lower->blocks[edge->to->id];
    basic_block_t *saved = lower->block;
    basic_block_t *copy = vir_machine_block(lower);
    lower->block = copy;
    if (overlap)
        for (int index = 0; index < edge->arg_count; index++) {
            vir_value_t *argument = edge->args[index];
            vir_location_t *source = &lower->locations[argument->id];
            vir_machine_copy(lower, argument, source->fixed, source->slot, -1,
                             lower->copy_slot + index * 8);
        }
    vir_value_t *parameter = edge->to->params;
    for (int index = 0; index < edge->arg_count;
         index++, parameter = parameter->param_next) {
        if (lower->locations[parameter->id].fixed >= 0)
            continue;
        vir_value_t *argument = edge->args[index];
        vir_location_t *source = &lower->locations[argument->id];
        vir_machine_copy(lower, argument, overlap ? -1 : source->fixed,
                         overlap ? lower->copy_slot + index * 8 : source->slot,
                         -1, lower->locations[parameter->id].slot);
    }
    vir_machine_parallel(lower, edge, resident, overlap);
    ph2_ir_t *instruction = vir_machine_insn(lower, OP_jump);
    instruction->next_bb = lower->blocks[edge->to->id];
    lower->block = saved;
    return copy;
}

static void vir_machine_edge_temporaries(vir_lower_t *lower, vir_block_t *block)
{
    for (int reg = 0; reg < REG_CNT; reg++) {
        int id = lower->owners[reg];
        if (id < 0)
            continue;
        vir_value_t *value = NULL;
        for (vir_edge_t *edge = block->outgoing; edge && !value;
             edge = edge->next_outgoing)
            for (int index = 0; index < edge->arg_count; index++)
                if (edge->args[index]->id == id)
                    value = edge->args[index];
        if (!value)
            continue;
        bool resident = true;
        for (vir_use_t *use = value->uses; use; use = use->next) {
            if (!use->edge || use->edge->from != block) {
                resident = false;
                break;
            }
            for (vir_value_t *parameter = use->edge->to->params; parameter;
                 parameter = parameter->param_next)
                if (lower->locations[parameter->id].fixed < 0)
                    resident = false;
        }
        if (resident)
            lower->locations[id].dirty = false;
    }
}

/* Machine edges deliberately bypass parser capture. Register homes are flushed
 * at every edge; predecessor metadata serves layout and backend analyses.
 */
static void vir_machine_connect(basic_block_t *from,
                                basic_block_t *to,
                                bb_connection_type_t type)
{
    if (!to)
        return;
    if (to->prev_idx == to->prev_cap)
        to->prev = arena_grow(BB_ARENA, (char *) to->prev, &to->prev_cap,
                              sizeof(bb_connection_t), 4, MAX_BB_PRED,
                              "Too many machine predecessors");
    to->prev[to->prev_idx].bb = from;
    to->prev[to->prev_idx++].type = type;
    if (type == NEXT)
        from->next = to;
    else if (type == THEN)
        from->then_ = to;
    else
        from->else_ = to;
}

static bool vir_machine_layout(vir_machine_function_t *output)
{
    int count = output->block_count;
    basic_block_t **blocks = malloc(count * sizeof(*blocks));
    basic_block_t **stack = malloc(count * sizeof(*stack));
    basic_block_t **post = malloc(count * sizeof(*post));
    if (!blocks || !stack || !post) {
        free(blocks);
        free(stack);
        free(post);
        return false;
    }
    int index = 0;
    for (basic_block_t *block = output->entry; block; block = block->rpo_next) {
        blocks[index++] = block;
        block->rpo = -1;
        ph2_ir_t *tail = block->ph2_ir_list.tail;
        if (tail && tail->op == OP_jump)
            vir_machine_connect(block, tail->next_bb, NEXT);
        else if (tail && tail->op == OP_branch) {
            vir_machine_connect(block, tail->then_bb, THEN);
            vir_machine_connect(block, tail->else_bb, ELSE);
        }
    }
    basic_block_t *last = NULL;
    index = 0;
    for (int root = 0; root < count; root++) {
        if (blocks[root]->rpo != -1)
            continue;
        int depth = 1, used = 0;
        stack[0] = blocks[root];
        stack[0]->rpo = -2;
        while (depth) {
            basic_block_t *block = stack[depth - 1];
            basic_block_t *next = block->next;
            if (!next || next->rpo != -1)
                next = block->then_;
            if (!next || next->rpo != -1)
                next = block->else_;
            if (next && next->rpo == -1) {
                next->rpo = -2;
                stack[depth++] = next;
            } else {
                post[used++] = block;
                depth--;
            }
        }
        while (used) {
            basic_block_t *block = post[--used];
            if (last)
                last->rpo_next = block;
            else
                output->entry = block;
            block->rpo = index++;
            last = block;
        }
    }
    if (last)
        last->rpo_next = NULL;
    free(blocks);
    free(stack);
    free(post);
    return true;
}

bool vir_lower_machine(const vir_function_t *function,
                       const vir_call_signature_t *signature,
                       int (*global_offset)(const char *, void *),
                       void *global_context,
                       vir_machine_function_t *output)
{
    vir_lower_t lower;
    int outgoing = 0, copy_count = 0, variadic_base = -1, object_end = 0;
    vir_value_t *parameter;
    int cursor = 0;

    if (!function || !function->blocks || !signature || !output ||
        function->pointer_bits != PTR_SIZE * 8 ||
        function->blocks->param_count != signature->param_count ||
        (signature->va_start_slot != (unsigned int) -1 &&
         !signature->is_variadic))
        return false;
    memset(&lower, 0, sizeof(lower));
    lower.function = function;
    lower.global_offset = global_offset;
    lower.global_context = global_context;
    lower.locations =
        calloc(function->next_value_id ? function->next_value_id : 1,
               sizeof(vir_location_t));
    lower.blocks = calloc(function->next_block_id, sizeof(basic_block_t *));
    if (!lower.locations || !lower.blocks) {
        free(lower.locations);
        free(lower.blocks);
        return false;
    }
    for (int reg = 0; reg < REG_CNT; reg++)
        lower.owners[reg] = -1;
    for (vir_block_t *block = function->blocks; block; block = block->next) {
        if (block->incoming && block->param_count > copy_count)
            copy_count = block->param_count;
        for (vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            if (effect->kind != VIR_EFFECT_CALL)
                continue;
            cursor = 0;
            for (int index = 0; index < effect->arg_count; index++) {
                cursor = vir_machine_arg_start(
                    &lower, cursor, effect->args[index],
                    effect->signature && effect->signature->is_variadic &&
                        index >= effect->signature->fixed_param_count);
                cursor += vir_machine_pair(&lower, effect->args[index]) ? 2 : 1;
            }
            if (cursor > outgoing)
                outgoing = cursor;

            /* ABI signedness and the variadic boundary belong to VIR, so
             * lowering never guesses them from source function metadata.
             */
            if (!effect->signature ||
                effect->signature->param_count != effect->arg_count)
                goto unsupported;
        }
        for (vir_value_t *value = block->head; value; value = value->next)
            if ((value->opcode == VIR_OP_GLOBAL_ADDR && !global_offset) ||
                (value->opcode == VIR_OP_LOAD && !value->def_effect) ||
                (value->opcode == VIR_OP_CALL && !value->def_effect))
                goto unsupported;
    }
    lower.frame = outgoing > MAX_ARGS_IN_REG
                      ? (outgoing - MAX_ARGS_IN_REG) * PTR_SIZE
                      : 0;
    lower.frame = ALIGN_UP(lower.frame, 8);
    lower.copy_slot = lower.frame;
    lower.frame += copy_count * 8;
    if (signature->is_variadic) {
        variadic_base = lower.frame;
        lower.frame += (PTR_SIZE == 4 ? 2 * MAX_PARAMS : MAX_PARAMS) * PTR_SIZE;
    }
    for (int index = 0; index < function->next_value_id; index++) {
        lower.locations[index].fixed = -1;
        lower.locations[index].slot = -1;
        lower.locations[index].reg = -1;
        lower.locations[index].high = -1;
    }
    if (!vir_machine_slots(&lower))
        goto unsupported;
    /* Object storage is separate from each address value's spill home. */
    int *objects = malloc(
        (function->next_value_id ? function->next_value_id : 1) * sizeof(int));
    if (!objects)
        goto unsupported;
    for (int index = 0; index < function->next_value_id; index++)
        objects[index] = -1;
    if (signature->is_variadic) {
        cursor = 0;
        parameter = function->blocks->params;
        for (int index = 0; index < signature->param_count; index++) {
            cursor = vir_machine_arg_start(&lower, cursor, parameter, false);
            unsigned int slot = signature->param_slots
                                    ? signature->param_slots[index]
                                    : (unsigned int) -1;
            if (slot != (unsigned int) -1) {
                if (slot >= (unsigned int) function->next_value_id) {
                    free(objects);
                    goto unsupported;
                }
                objects[slot] = variadic_base + cursor * PTR_SIZE;
            }
            cursor += vir_machine_pair(&lower, parameter) ? 2 : 1;
            parameter = parameter->param_next;
        }
        if (signature->va_start_slot != (unsigned int) -1) {
            if (signature->va_start_slot >=
                (unsigned int) function->next_value_id) {
                free(objects);
                goto unsupported;
            }
            objects[signature->va_start_slot] =
                variadic_base + cursor * PTR_SIZE;
        }
    }
    for (vir_block_t *block = function->blocks; block; block = block->next) {
        lower.blocks[block->id] = vir_machine_block(&lower);
        for (vir_value_t *value = block->head; value; value = value->next) {
            if (value->opcode != VIR_OP_STACK_ADDR)
                continue;
            if (value->address_slot >= (unsigned int) function->next_value_id) {
                free(objects);
                goto unsupported;
            }
            if (objects[value->address_slot] < 0) {
                lower.frame = ALIGN_UP(lower.frame, value->address_alignment);
                objects[value->address_slot] = lower.frame;
                lower.frame += value->address_size;
                object_end = lower.frame;
            }
        }
    }
    output->entry = lower.blocks[function->blocks->id];
    lower.block = output->entry;
    if (signature->is_variadic) {
        int words = PTR_SIZE == 4 ? 2 * MAX_PARAMS : MAX_PARAMS;
        for (int word = 0; word < words; word++) {
            int reg = word < MAX_ARGS_IN_REG ? word : REG_CNT - 1;
            ph2_ir_t *instruction;
            if (word >= MAX_ARGS_IN_REG) {
                instruction = vir_machine_insn(&lower, OP_load);
                instruction->dest = reg;
                instruction->src0 = (word - MAX_ARGS_IN_REG) * PTR_SIZE;
                instruction->ofs_based_on_stack_top = true;
                instruction->size_bytes = PTR_SIZE;
            }
            instruction = vir_machine_insn(&lower, OP_store);
            instruction->src0 = reg;
            instruction->src1 = variadic_base + word * PTR_SIZE;
            instruction->size_bytes = PTR_SIZE;
        }
    }
    parameter = function->blocks->params;
    cursor = 0;
    while (parameter) {
        int words = vir_machine_pair(&lower, parameter) ? 2 : 1;
        cursor = vir_machine_arg_start(&lower, cursor, parameter, false);
        for (int word = 0; word < words; word++) {
            int abi_word = cursor + word;
            int reg = abi_word < MAX_ARGS_IN_REG ? abi_word : REG_CNT - 1;
            ph2_ir_t *instruction;
            if (abi_word >= MAX_ARGS_IN_REG) {
                instruction = vir_machine_insn(&lower, OP_load);
                instruction->dest = reg;
                instruction->src0 = (abi_word - MAX_ARGS_IN_REG) * PTR_SIZE;
                instruction->ofs_based_on_stack_top = true;
                instruction->size_bytes = PTR_SIZE;
            }
            instruction = vir_machine_insn(&lower, OP_store);
            instruction->src0 = reg;
            instruction->src1 =
                lower.locations[parameter->id].slot + word * PTR_SIZE;
            instruction->size_bytes =
                words == 2 ? 4 : vir_machine_width(&lower, parameter->type);
        }
        cursor += words;
        parameter = parameter->param_next;
    }
    vir_machine_pin_parameters(&lower);
    for (vir_block_t *block = function->blocks; block; block = block->next) {
        vir_value_t *value = block->head;
        vir_effect_t *effect = block->effects;
        lower.block = lower.blocks[block->id];
        while (value || effect) {
            if (value && (!effect || value->order < effect->order)) {
                lower.position =
                    lower.block_positions[block->id] + value->order;
                vir_machine_value(&lower, value);
                if (value->opcode == VIR_OP_STACK_ADDR)
                    lower.block->ph2_ir_list.tail->src0 =
                        objects[value->address_slot];
                value = value->next;
            } else {
                lower.position =
                    lower.block_positions[block->id] + effect->order;
                vir_machine_expire(&lower);
                vir_machine_effect(&lower, effect);
                effect = effect->next;
            }
        }
        lower.position = lower.block_positions[block->id] + block->next_order;
        ph2_ir_t *instruction;
        if (block->terminator == VIR_TERM_RETURN) {
            lower.locked = 0;
            int returned = block->return_value
                               ? vir_machine_read(&lower, block->return_value)
                               : -1;
            instruction = vir_machine_insn(&lower, OP_return);
            instruction->src0 = returned;
            if (block->return_value) {
                instruction->src0_hi =
                    lower.locations[block->return_value->id].high;
                instruction->size_bytes =
                    vir_machine_width(&lower, block->return_value->type);
                instruction->src0_is_unsigned = signature->result.is_unsigned;
            }
            /* A return has no successor to observe dirty homes. */
            for (int reg = 0; reg < REG_CNT; reg++) {
                int id = lower.owners[reg];
                if (id >= 0) {
                    lower.locations[id].reg = -1;
                    lower.locations[id].high = -1;
                    lower.locations[id].dirty = false;
                    lower.owners[reg] = -1;
                }
            }
        } else if (block->terminator == VIR_TERM_JUMP) {
            basic_block_t *target = vir_machine_edge(&lower, block->outgoing);
            vir_machine_edge_temporaries(&lower, block);
            vir_machine_flush(&lower);
            instruction = vir_machine_insn(&lower, OP_jump);
            instruction->next_bb = target;
        } else if (block->terminator == VIR_TERM_BRANCH) {
            lower.locked = 0;
            int condition = vir_machine_read(&lower, block->branch_condition);
            int condition_high =
                lower.locations[block->branch_condition->id].high;
            basic_block_t *yes = vir_machine_edge(&lower, block->true_edge);
            basic_block_t *no = vir_machine_edge(&lower, block->false_edge);
            vir_machine_edge_temporaries(&lower, block);
            vir_use_t *use = block->branch_condition->uses;
            if (use && !use->next && use->branch_block == block)
                lower.locations[block->branch_condition->id].dirty = false;
            vir_machine_flush(&lower);
            instruction = vir_machine_insn(&lower, OP_branch);
            instruction->src0 = condition;
            instruction->src0_hi = condition_high;
            instruction->size_bytes =
                vir_machine_width(&lower, block->branch_condition->type);
            instruction->then_bb = yes;
            instruction->else_bb = no;
        }
        lower.locked = 0;
    }
    output->stack_size = object_end;
    output->block_count = 0;
    for (basic_block_t *block = output->entry; block; block = block->rpo_next) {
        output->block_count++;
        for (ph2_ir_t *instruction = block->ph2_ir_list.head; instruction;
             instruction = instruction->next) {
            int accessed = 0;
            if (instruction->op == OP_load &&
                !instruction->ofs_based_on_stack_top)
                accessed = instruction->src0 + instruction->size_bytes;
            else if (instruction->op == OP_store)
                accessed = instruction->src1 + instruction->size_bytes;
            else if (instruction->op == OP_address_of)
                accessed = instruction->src0 + PTR_SIZE;
            if (accessed > output->stack_size)
                output->stack_size = accessed;
        }
    }
    if (!vir_machine_layout(output)) {
        free(objects);
        goto unsupported;
    }
    output->stack_size = ALIGN_UP(output->stack_size, PTR_SIZE);
    for (basic_block_t *block = output->entry; block; block = block->rpo_next)
        for (ph2_ir_t *instruction = block->ph2_ir_list.head; instruction;
             instruction = instruction->next)
            if (instruction->op == OP_return)
                instruction->src1 = output->stack_size;
    free(objects);
    free(lower.locations);
    free(lower.blocks);
    free(lower.block_positions);
    return true;
unsupported:
    free(lower.locations);
    free(lower.blocks);
    free(lower.block_positions);
    return false;
}
