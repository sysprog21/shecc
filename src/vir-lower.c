#include "vir-lower.h"

typedef struct {
    vir_value_t *value;
    int slot;
    int reg;
    int high;
    bool dirty;
    bool rematerializable;
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
    int frame;
    int copy_slot;
    int transfer_slot;
    int position;
    int *block_positions;
    int *live_seen;
    vir_block_t **live_work;
    /* Last mark vir_machine_interferes() left in a block-indexed seen array. */
    int live_stamp;
    const vir_dominance_t *dominance;
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

static bool vir_machine_literal(const vir_value_t *value)
{
    return value->opcode == VIR_OP_CONST ||
           value->opcode == VIR_OP_GLOBAL_ADDR ||
           value->opcode == VIR_OP_RODATA_ADDR;
}

/* ABI and edge transfers can consume homes without going through read(). */
static bool vir_machine_rematerializable(const vir_value_t *value)
{
    if (!vir_machine_literal(value))
        return false;
    for (const vir_use_t *use = value->uses; use; use = use->next)
        if (use->edge || (use->effect && use->effect->kind == VIR_EFFECT_CALL))
            return false;
    return true;
}

static void vir_machine_materialize(vir_lower_t *lower,
                                    const vir_value_t *value,
                                    ph2_ir_t *instruction)
{
    instruction->is_pointer = value->type == VIR_TYPE_PTR;
    if (value->opcode == VIR_OP_CONST) {
        instruction->op = OP_load_constant;
        instruction->src0 = (int) value->constant;
        instruction->src1 = (int) (value->constant >> 32);
        instruction->is_unsigned = true;
    } else if (value->opcode == VIR_OP_GLOBAL_ADDR) {
        instruction->op = OP_global_address_of;
        instruction->src0 =
            lower->global_offset(value->address_name, lower->global_context);
    } else {
        instruction->op = OP_load_rodata_address;
        instruction->src0 = (int) value->constant;
    }
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
        if (location->rematerializable)
            vir_machine_materialize(lower, value, instruction);
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
    lower->locations[value->id].dirty =
        !lower->locations[value->id].rematerializable;
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
#define MAP(vir, machine) \
    case VIR_OP_##vir:    \
        return OP_##machine
    switch (opcode) {
        MAP(ADD, add);
        MAP(PTRADD, add);
        MAP(SUB, sub);
        MAP(MUL, mul);
        MAP(SDIV, div);
        MAP(UDIV, div);
        MAP(SREM, mod);
        MAP(UREM, mod);
        MAP(SHL, lshift);
        MAP(ASHR, rshift);
        MAP(LSHR, rshift);
        MAP(BITAND, bit_and);
        MAP(BITOR, bit_or);
        MAP(BITXOR, bit_xor);
        MAP(EQ, eq);
        MAP(SLT, lt);
        MAP(ULT, lt);
        MAP(NEG, negate);
        MAP(BITNOT, bit_not);
        MAP(TRUNC, trunc);
        MAP(SEXT, sign_ext);
        MAP(ZEXT, sign_ext);
        MAP(PTRTOINT, cast);
        MAP(INTTOPTR, cast);
    default:
        return OP_load_constant;
    }
#undef MAP
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
    vir_value_t *op0 = value->op0, *op1 = value->op1;
    bool negate = value->opcode == VIR_OP_EQ &&
                  value->block->branch_condition != value &&
                  op0->type == VIR_TYPE_I1 &&
                  ((vir_is_constant(op0) && !op0->constant) ||
                   (vir_is_constant(op1) && !op1->constant));
    if (negate) {
        op0 = vir_is_constant(op0) && !op0->constant ? op1 : op0;
        op1 = NULL;
    }

    /* Keep literals in the immediate operand without changing the value graph.
     */
    if (op0 && op1 && op0->opcode == VIR_OP_CONST &&
        op1->opcode != VIR_OP_CONST &&
        (value->opcode == VIR_OP_ADD || value->opcode == VIR_OP_MUL ||
         value->opcode == VIR_OP_BITAND || value->opcode == VIR_OP_BITOR ||
         value->opcode == VIR_OP_BITXOR)) {
        op0 = value->op1;
        op1 = value->op0;
    }

    if (value->is_block_param || value->def_effect)
        return;
    lower->locked = 0;
    vir_machine_expire(lower);
    if (op0)
        first = vir_machine_read(lower, op0);
    if (op1)
        second = vir_machine_read(lower, op1);
    int first_high = op0 ? lower->locations[op0->id].high : -1;
    int second_high = op1 ? lower->locations[op1->id].high : -1;

    /* Reuse a dying input of the same width: no move or spill is needed to
     * preserve it, and pair operations keep both halves in their original
     * order.
     */
    if (op0 && lower->locations[op0->id].fixed < 0 &&
        lower->locations[value->id].fixed < 0 &&
        lower->locations[op0->id].end == lower->position &&
        vir_machine_width(lower, op0->type) ==
            vir_machine_width(lower, value->type)) {
        vir_location_t *old = &lower->locations[op0->id];
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
    instruction = vir_machine_insn(
        lower, negate ? OP_log_not : vir_machine_opcode(value->opcode));
    vir_machine_result(lower, instruction, value);
    if (first >= 0) {
        instruction->src0 = first;
        instruction->src0_hi = first_high;
        instruction->src0_is_pointer = op0->type == VIR_TYPE_PTR;
    }
    if (second >= 0) {
        instruction->src1 = second;
        instruction->src1_hi = second_high;
        instruction->src1_is_pointer = op1->type == VIR_TYPE_PTR;
    }
    if (value->opcode == VIR_OP_EQ || value->opcode == VIR_OP_SLT ||
        value->opcode == VIR_OP_ULT)
        instruction->size_bytes = vir_machine_width(lower, value->op0->type);
    instruction->is_unsigned = vir_machine_unsigned(value->opcode);
    instruction->src0_is_unsigned = instruction->is_unsigned;
    instruction->src1_is_unsigned = instruction->is_unsigned;
    if (vir_machine_literal(value)) {
        vir_machine_materialize(lower, value, instruction);
    } else if (value->opcode == VIR_OP_STACK_ADDR) {
        instruction->op = OP_address_of;
        instruction->src0 = lower->locations[value->id].slot;
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
 *
 * Rather than walking forward from the writer, walk backward from the
 * survivor's uses to the blocks it is live into, stopping at its definition.
 * The writer interferes when it reaches one of them, and the backward walk is
 * bounded by the survivor's live range instead of everything the writer
 * reaches. @seen holds a fresh mark per call, so it is not cleared each time.
 */
static bool vir_machine_interferes(vir_lower_t *lower,
                                   vir_value_t *writer,
                                   vir_value_t *survivor,
                                   int *seen,
                                   vir_block_t **work)
{
    const vir_block_t *first = writer->block;
    int start = writer->is_block_param ? -1 : writer->order;
    int definition = survivor->is_block_param ? -1 : survivor->order;
    bool defined_later = first == survivor->block && definition > start;
    int first_stop = defined_later ? definition : first->next_order + 1;
    int pending = 0;

    /* Fresh arrays start zeroed, so any mark from 2 up is unused. */
    if (lower->live_stamp < 2 || lower->live_stamp == INT_MAX) {
        memset(seen, 0, lower->function->next_block_id * sizeof(int));
        lower->live_stamp = 1;
    }
    int mark = ++lower->live_stamp;
    for (vir_use_t *use = survivor->uses; use; use = use->next) {
        vir_block_t *used = vir_use_block(use);
        int order = vir_use_order(use);
        int stop = used == survivor->block ? definition : used->next_order + 1;
        if (used == first && order > start && order < first_stop)
            return true;
        if (order > -1 && order < stop && seen[used->id] != mark) {
            seen[used->id] = mark;
            work[pending++] = used;
        }
    }

    /* The writer's own block continues past its end unless the survivor is
     * defined later in it.
     */
    if (defined_later)
        return false;
    while (pending) {
        const vir_block_t *block = work[--pending];
        for (vir_edge_t *edge = block->incoming; edge;
             edge = edge->next_incoming) {
            vir_block_t *from = edge->from;
            if (from == first)
                return true;
            if (from != survivor->block && seen[from->id] != mark) {
                seen[from->id] = mark;
                work[pending++] = from;
            }
        }
    }
    return false;
}

/* A direct sole-predecessor successor can inherit unmodified register owners.
 */
static void vir_machine_flush_successor(vir_lower_t *lower,
                                        vir_block_t *block,
                                        const vir_dominance_t *dom)
{
    vir_block_t *next = block->next;
    bool direct = next && !next->param_count && next->incoming &&
                  !next->incoming->next_incoming &&
                  next->incoming->from == block;
    for (vir_edge_t *edge = block->outgoing; edge; edge = edge->next_outgoing)
        direct = direct && !edge->arg_count;
    for (int reg = 0; reg < REG_CNT; reg++) {
        int id = lower->owners[reg];
        if (id < 0 || lower->locations[id].reg != reg)
            continue;
        vir_value_t *value = lower->locations[id].value;
        bool retain = direct && value != block->branch_condition;
        for (vir_use_t *use = value->uses; retain && use; use = use->next) {
            vir_block_t *used = vir_use_block(use);
            if (used == value->block && lower->block_positions[used->id] <=
                                            lower->block_positions[block->id])
                continue;
            retain = vir_dominance_block_dominates(dom, next, used);
        }
        if (!retain) {
            vir_value_t boundary = {0};
            boundary.block = block;
            boundary.order = block->next_order;
            bool argument = false;
            for (vir_use_t *use = value->uses; use; use = use->next)
                argument |= use->edge && use->edge->from == block;
            if (!value->is_block_param && !argument &&
                value != block->branch_condition &&
                !vir_machine_interferes(lower, &boundary, value,
                                        lower->live_seen, lower->live_work))
                lower->locations[id].dirty = false;
            vir_machine_spill(lower, reg);
        }
    }
}

/* Every value sharing physical lanes must survive either definition safely.
 * @pinned lists the IDs of the @count values given a fixed register.
 */
static bool vir_machine_pin_safe(vir_lower_t *lower,
                                 vir_value_t *value,
                                 int fixed,
                                 const int *pinned,
                                 int count,
                                 int *seen,
                                 vir_block_t **work)
{
    unsigned int lanes = (vir_machine_pair(lower, value) ? 3u : 1u) << fixed;
    for (int i = 0; i < count; i++) {
        vir_value_t *other = lower->locations[pinned[i]].value;
        int reg = lower->locations[pinned[i]].fixed;
        if (!(lanes & ((vir_machine_pair(lower, other) ? 3u : 1u) << reg)))
            continue;
        if (vir_machine_interferes(lower, value, other, seen, work) ||
            vir_machine_interferes(lower, other, value, seen, work))
            return false;
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
    int *heads = NULL, *ends = NULL, *pinned = NULL;
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
        free_slots[id] = 0;
        if (!value)
            continue;
        lower->locations[id].value = value;
        lower->locations[id].rematerializable =
            vir_machine_rematerializable(value);
        int score = 0, ordinary_uses = 0;
        int start = lower->block_positions[value->block->id] +
                    (value->is_block_param ? 0 : value->order);
        int end = start, last_local_use = value->order;
        int pending = 0;
        for (vir_use_t *use = value->uses; use; use = use->next) {
            ordinary_uses += !use->edge;
            vir_block_t *block = vir_use_block(use);
            int used = lower->block_positions[block->id] + vir_use_order(use);
            if (block != value->block)
                score += 16;
            else if (vir_use_order(use) > last_local_use)
                last_local_use = vir_use_order(use);
            if (used > end)
                end = used;
            if (used < start)
                start = used;
            if (block != value->block && seen[block->id] != id + 1) {
                seen[block->id] = id + 1;
                work[pending++] = block;
            }
        }
        if (value->opcode == VIR_OP_CALL)
            for (vir_effect_t *effect = value->block->effects; effect;
                 effect = effect->next)
                if (effect->kind == VIR_EFFECT_CALL &&
                    effect->order > value->order &&
                    effect->order < last_local_use)
                    score += 16;
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
        if (score && value->is_block_param)
            for (vir_edge_t *edge = value->block->incoming; edge;
                 edge = edge->next_incoming) {
                int span = lower->block_positions[edge->from->id] -
                           lower->block_positions[value->block->id];
                if (span >= 0) {
                    long long weighted =
                        score +
                        (100000LL + 65536 / (span + 1)) * (ordinary_uses + 1);
                    score = weighted > INT_MAX ? INT_MAX : (int) weighted;
                }
            }
        free_slots[id] = score;
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

    /* Reuse the future home freelist for immutable pin scores. Adding fixed
     * values can only make a failed lane assignment less available.
     */
    int pinned_count = 0;
    pinned = malloc(count * sizeof(int));
    if (!pinned)
        goto done;
    while (true) {
        vir_value_t *best = NULL;
        int best_score = 0, best_reg = -1;
        for (int id = 0; id < count; id++) {
            vir_value_t *value = values[id];
            if (!value || lower->locations[id].fixed >= 0)
                continue;
            int score = free_slots[id];
            if (score <= best_score)
                continue;
            int words = vir_machine_pair(lower, value) ? 2 : 1;
            int reg;
            for (reg = base; reg + words <= limit; reg++)
                if (vir_machine_pin_safe(lower, value, reg, pinned,
                                         pinned_count, seen, work)) {
                    best = value;
                    best_score = score;
                    best_reg = reg;
                    break;
                }
            if (reg + words > limit)
                free_slots[id] = 0;
        }
        if (!best)
            break;
        vir_location_t *location = &lower->locations[best->id];
        int words = vir_machine_pair(lower, best) ? 2 : 1;
        location->fixed = location->reg = best_reg;
        location->high = words == 2 ? best_reg + 1 : -1;
        pinned[pinned_count++] = best->id;
        lower->pinned_mask |= ((1u << words) - 1) << best_reg;

        /* Phi affinity extends a pin through values feeding the same family.
         * Checking both directions against every member prevents alternate
         * branch results from overwriting one another before their outgoing
         * copies.
         */
        bool changed = best->is_block_param;
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
                        lower->locations[param->id].fixed < 0 ||
                        !vir_dominance_block_dominates(
                            lower->dominance, param->block, value->block)) {
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
                if (!safe || fixed < 0 ||
                    !vir_machine_pin_safe(lower, value, fixed, pinned,
                                          pinned_count, seen, work))
                    continue;
                lower->locations[id].fixed = lower->locations[id].reg = fixed;
                lower->locations[id].high =
                    vir_machine_pair(lower, value) ? fixed + 1 : -1;
                pinned[pinned_count++] = id;
                changed = true;
            }
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
    if (success) {
        lower->live_seen = seen;
        lower->live_work = work;
    } else {
        free(seen);
        free(work);
    }
    free(free_slots);
    free(next_start);
    free(next_end);
    free(heads);
    free(ends);
    free(pinned);
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

/* Word-sized transfers keep pair lanes in the same dependency graph as scalars.
 */
typedef struct {
    int from, slot, to, width;
    bool is_unsigned, abi;
} vir_transfer_t;

static void vir_machine_transfer(vir_lower_t *lower, vir_transfer_t *copy)
{
    if (copy->from == copy->to && (!copy->abi || copy->width >= PTR_SIZE))
        return;
    opcode_t op = copy->from < 0 ? OP_load : OP_assign;
    if (copy->from >= 0 && copy->abi && copy->width < PTR_SIZE)
        op = OP_sign_ext;
    ph2_ir_t *instruction = vir_machine_insn(lower, op);
    instruction->dest = copy->to;
    instruction->src0 = copy->from < 0 ? copy->slot : copy->from;
    instruction->size_bytes = copy->width;
    instruction->is_unsigned = copy->is_unsigned;
    if (op == OP_sign_ext) {
        instruction->src1 = (copy->width << 16) | PTR_SIZE;
        instruction->src0_is_unsigned = copy->is_unsigned;
    }
}

/* Save one source word to break cycles without clobbering completed copies. */
static void vir_machine_transfer_save(vir_lower_t *lower,
                                      vir_transfer_t *copies,
                                      int count,
                                      int reg,
                                      int slot)
{
    ph2_ir_t *instruction = vir_machine_insn(lower, OP_store);
    instruction->src0 = reg;
    instruction->src1 = slot;
    instruction->size_bytes = PTR_SIZE;
    for (int index = 0; index < count; index++)
        if (copies[index].from == reg) {
            copies[index].from = -1;
            copies[index].slot = slot;
        }
}

static void vir_machine_parallel(vir_lower_t *lower,
                                 vir_transfer_t *copies,
                                 int count)
{
    while (count) {
        int ready = -1;
        for (int index = 0; index < count; index++) {
            bool blocked = false;
            for (int other = 0; other < count; other++)
                if (other != index && copies[other].from == copies[index].to)
                    blocked = true;
            if (!blocked) {
                ready = index;
                break;
            }
        }
        if (ready < 0) {
            vir_machine_transfer_save(lower, copies, count, copies[0].from,
                                      lower->transfer_slot);
            continue;
        }
        vir_machine_transfer(lower, &copies[ready]);
        copies[ready] = copies[--count];
    }
}

static void vir_machine_call(vir_lower_t *lower, vir_effect_t *effect)
{
    int cursor = 0, count = 0;
    vir_transfer_t *copies = calloc(effect->arg_count + 1, 2 * sizeof(*copies));
    if (!copies)
        fatal("VIR: cannot allocate call transfers");
    ph2_ir_t *instruction;

    /* Capture residents before flushing invalidates their allocator locations.
     */
    for (int index = 0; index < effect->arg_count; index++) {
        vir_value_t *argument = effect->args[index];
        vir_location_t *location = &lower->locations[argument->id];
        int words = vir_machine_pair(lower, argument) ? 2 : 1;
        cursor = vir_machine_arg_start(
            lower, cursor, argument,
            effect->signature->is_variadic &&
                index >= effect->signature->fixed_param_count);
        for (int word = 0; word < words; word++) {
            int abi_word = cursor + word;
            copies[count++] = (vir_transfer_t) {
                location->reg < 0 ? -1 : location->reg + word,
                location->slot + word * PTR_SIZE,
                abi_word < MAX_ARGS_IN_REG ? abi_word
                                           : -1 - (abi_word - MAX_ARGS_IN_REG),
                words == 2 ? 4 : vir_machine_width(lower, argument->type),
                effect->signature->params[index].is_unsigned || words == 2,
                true};
        }
        cursor += words;
    }
    if (effect->callee_value) {
        vir_location_t *location = &lower->locations[effect->callee_value->id];
        copies[count++] = (vir_transfer_t) {
            location->reg, location->slot, REG_CNT - 1, PTR_SIZE, true, false};
    }
    for (int reg = 0; reg < REG_CNT; reg++) {
        int id = lower->owners[reg];
        if (id >= 0 && lower->locations[id].end <= lower->position)
            lower->locations[id].dirty = false;
        vir_machine_spill(lower, reg);
    }

    /* Stack arguments precede register copies. Protect any resident in the
     * stack staging scratch, including a saved indirect callee.
     */
    bool stack = false;
    for (int index = 0; index < count; index++)
        stack |= copies[index].to < 0;
    if (stack)
        for (int index = 0; index < count; index++)
            if (copies[index].from == REG_CNT - 1) {
                vir_machine_transfer_save(lower, copies, count, REG_CNT - 1,
                                          copies[index].slot);
                break;
            }
    int registers = 0;
    for (int index = 0; index < count; index++) {
        vir_transfer_t copy = copies[index];
        if (copy.to >= 0) {
            copies[registers++] = copy;
            continue;
        }
        int destination = (-1 - copy.to) * PTR_SIZE;
        copy.to = REG_CNT - 1;
        vir_machine_transfer(lower, &copy);
        instruction = vir_machine_insn(lower, OP_store);
        instruction->src0 = copy.to;
        instruction->src1 = destination;
        instruction->size_bytes = PTR_SIZE;
    }
    vir_machine_parallel(lower, copies, registers);
    free(copies);
    if (effect->callee_value) {
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
        if (location->fixed >= 0) {
            int words = vir_machine_pair(lower, effect->result) ? 2 : 1;
            for (int word = 0; word < words; word++) {
                vir_transfer_t copy = {location->reg + word,
                                       location->slot,
                                       location->fixed + word,
                                       words == 2 ? 4 : PTR_SIZE,
                                       true,
                                       false};
                vir_machine_transfer(lower, &copy);
                lower->owners[location->reg + word] = -1;
            }
            location->reg = location->fixed;
            location->high = words == 2 ? location->fixed + 1 : -1;
        }
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

/* Copy a bitvector from a register (or pair) or home to another home. */
static void vir_machine_copy(vir_lower_t *lower,
                             vir_value_t *value,
                             int from,
                             int source,
                             int destination)
{
    bool pair = vir_machine_pair(lower, value);
    if (from < 0) {
        ph2_ir_t *instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = 0;
        instruction->dest_hi = pair ? instruction->dest + 1 : -1;
        instruction->src0 = source;
        instruction->size_bytes = vir_machine_width(lower, value->type);
        instruction->is_unsigned = true;
        from = 0;
    }
    ph2_ir_t *instruction = vir_machine_insn(lower, OP_store);
    instruction->src0 = from;
    instruction->src0_hi = pair ? from + 1 : -1;
    instruction->size_bytes = vir_machine_width(lower, value->type);
    instruction->is_unsigned = true;
    instruction->src1 = destination;
}

static void vir_machine_edge_registers(vir_lower_t *lower,
                                       vir_edge_t *edge,
                                       bool resident,
                                       bool snapshot)
{
    vir_transfer_t copies[REG_CNT];
    int count = 0;
    vir_value_t *parameter = edge->to->params;
    for (int index = 0; index < edge->arg_count;
         index++, parameter = parameter->param_next) {
        vir_value_t *argument = edge->args[index];
        vir_location_t *location = &lower->locations[argument->id];
        int target = lower->locations[parameter->id].fixed;
        int reg = snapshot ? -1 : resident ? location->reg : location->fixed;
        if (target < 0 || target == reg)
            continue;
        int words = vir_machine_pair(lower, argument) ? 2 : 1;
        for (int word = 0; word < words; word++)
            copies[count++] = (vir_transfer_t) {
                reg < 0 ? -1 : reg + word,
                (snapshot ? lower->copy_slot + index * 8 : location->slot) +
                    word * PTR_SIZE,
                target + word,
                words == 2 ? 4 : vir_machine_width(lower, argument->type),
                true,
                false};
    }
    vir_machine_parallel(lower, copies, count);
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
            vir_machine_copy(lower, argument, source->fixed, source->slot,
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
                         lower->locations[parameter->id].slot);
    }
    vir_machine_edge_registers(lower, edge, resident, overlap);
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

static void vir_machine_save_argument(vir_lower_t *lower,
                                      int word,
                                      int slot,
                                      int width)
{
    int reg = word < MAX_ARGS_IN_REG ? word : REG_CNT - 1;
    ph2_ir_t *instruction;
    if (word >= MAX_ARGS_IN_REG) {
        instruction = vir_machine_insn(lower, OP_load);
        instruction->dest = reg;
        instruction->src0 = (word - MAX_ARGS_IN_REG) * PTR_SIZE;
        instruction->ofs_based_on_stack_top = true;
        instruction->size_bytes = PTR_SIZE;
    }
    instruction = vir_machine_insn(lower, OP_store);
    instruction->src0 = reg;
    instruction->src1 = slot;
    instruction->size_bytes = width;
}

/* Keep incoming words resident when they already satisfy the allocator's pair
 * alignment. Narrow and split arguments retain their canonical home path.
 */
static void vir_machine_parameters(vir_lower_t *lower)
{
    int cursor = 0;
    for (vir_value_t *value = lower->function->blocks->params; value;
         value = value->param_next) {
        vir_location_t *location = &lower->locations[value->id];
        int words = vir_machine_pair(lower, value) ? 2 : 1;
        int width = vir_machine_width(lower, value->type);
        cursor = vir_machine_arg_start(lower, cursor, value, false);
        int source = cursor;
        cursor += words;
        if (!value->uses)
            continue;
        bool resident = width >= 4 && cursor <= MAX_ARGS_IN_REG &&
                        (words == 1 || !(source & 1));
        if (!resident)
            for (int word = 0; word < words; word++)
                vir_machine_save_argument(lower, source + word,
                                          location->slot + word * PTR_SIZE,
                                          words == 2 ? 4 : width);
        if (location->fixed >= 0) {
            ph2_ir_t *instruction =
                vir_machine_insn(lower, resident ? OP_assign : OP_load);
            instruction->dest = location->fixed;
            instruction->dest_hi = location->high;
            instruction->src0 = resident ? source : location->slot;
            instruction->src0_hi = resident && words == 2 ? source + 1 : -1;
            instruction->size_bytes = width;
            instruction->is_pointer = value->type == VIR_TYPE_PTR;
            instruction->is_unsigned = true;
        } else if (resident) {
            location->reg = source;
            location->high = words == 2 ? source + 1 : -1;
            location->dirty = true;
            for (int word = 0; word < words; word++)
                lower->owners[source + word] = value->id;
        }
    }
}

bool vir_lower_machine(const vir_function_t *function,
                       const vir_call_signature_t *signature,
                       int (*global_offset)(const char *, void *),
                       void *global_context,
                       vir_machine_function_t *output)
{
    vir_lower_t lower;
    vir_dominance_t dominance = {0};
    int outgoing = 0, copy_count = 0, variadic_base = -1, object_end = 0;
    vir_value_t *parameter;
    int cursor = 0;
    int *objects = NULL;
    bool success = false;

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
    if (!lower.locations || !lower.blocks)
        goto unsupported;
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
    lower.transfer_slot = lower.frame;
    if (copy_count || outgoing)
        lower.frame += 8;
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
    if (!vir_dominance_init(function, &dominance))
        goto unsupported;
    lower.dominance = &dominance;
    if (!vir_machine_slots(&lower))
        goto unsupported;
    /* Object storage is separate from each address value's spill home. */
    objects = malloc((function->next_value_id ? function->next_value_id : 1) *
                     sizeof(int));
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
                if (slot >= (unsigned int) function->next_value_id)
                    goto unsupported;
                objects[slot] = variadic_base + cursor * PTR_SIZE;
            }
            cursor += vir_machine_pair(&lower, parameter) ? 2 : 1;
            parameter = parameter->param_next;
        }
        if (signature->va_start_slot != (unsigned int) -1) {
            if (signature->va_start_slot >=
                (unsigned int) function->next_value_id)
                goto unsupported;
            objects[signature->va_start_slot] =
                variadic_base + cursor * PTR_SIZE;
        }
    }
    for (vir_block_t *block = function->blocks; block; block = block->next) {
        lower.blocks[block->id] = vir_machine_block(&lower);
        for (vir_value_t *value = block->head; value; value = value->next) {
            if (value->opcode != VIR_OP_STACK_ADDR)
                continue;
            if (value->address_slot >= (unsigned int) function->next_value_id)
                goto unsupported;
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
        for (int word = 0; word < words; word++)
            vir_machine_save_argument(
                &lower, word, variadic_base + word * PTR_SIZE, PTR_SIZE);
    }
    vir_machine_parameters(&lower);
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
                if (id >= 0)
                    lower.locations[id].dirty = false;
            }
            vir_machine_flush(&lower);
        } else if (block->terminator == VIR_TERM_JUMP) {
            basic_block_t *target = vir_machine_edge(&lower, block->outgoing);
            vir_machine_edge_temporaries(&lower, block);
            vir_machine_flush_successor(&lower, block, &dominance);
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
            vir_machine_flush_successor(&lower, block, &dominance);
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
    if (!vir_machine_layout(output))
        goto unsupported;
    output->stack_size = ALIGN_UP(output->stack_size, PTR_SIZE);
    for (basic_block_t *block = output->entry; block; block = block->rpo_next)
        for (ph2_ir_t *instruction = block->ph2_ir_list.head; instruction;
             instruction = instruction->next)
            if (instruction->op == OP_return)
                instruction->src1 = output->stack_size;
    success = true;
unsupported:
    free(lower.live_seen);
    free(lower.live_work);
    vir_dominance_release(&dominance);
    free(objects);
    free(lower.locations);
    free(lower.blocks);
    free(lower.block_positions);
    return success;
}
