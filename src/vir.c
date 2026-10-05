#include "vir.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    VIR_USE_EXACT,
    VIR_USE_USER,
    VIR_USE_EDGE,
    VIR_USE_EFFECT,
    VIR_USE_RETURN,
    VIR_USE_BRANCH,
} vir_use_owner_t;

typedef struct vir_dominance vir_dominance_t;

static const struct {
    const char *name;
    unsigned char width;
} vir_types[] = {
#define VIR_TYPE_ITEM(name, spelling, width) {spelling, width},
    VIR_TYPE_LIST
#undef VIR_TYPE_ITEM
};

static bool vir_has_block(const vir_function_t *func,
                          const vir_block_t *needle);
static bool vir_block_open(const vir_function_t *func,
                           const vir_block_t *block);
static int vir_block_index(const vir_function_t *func,
                           const vir_block_t *needle);
static bool vir_has_value(const vir_function_t *func,
                          const vir_value_t *needle);
static bool vir_has_incoming_edge(const vir_block_t *block,
                                  const vir_edge_t *needle);
static bool vir_has_outgoing_edge(const vir_block_t *block,
                                  const vir_edge_t *needle);
static bool vir_has_effect(const vir_block_t *block,
                           const vir_effect_t *needle);
static bool vir_value_dominates(const vir_function_t *func,
                                const vir_dominance_t *dominance,
                                const vir_value_t *value,
                                const vir_block_t *use_block,
                                int use_position);
static void vir_remove_use(vir_value_t *operand, vir_use_t *needle);
static void vir_remove_matching_use(vir_value_t *value,
                                    vir_use_owner_t kind,
                                    const void *owner,
                                    int operand);
static bool vir_value_dominates_effect(const vir_function_t *func,
                                       const vir_dominance_t *dominance,
                                       const vir_value_t *value,
                                       const vir_effect_t *effect);
static void vir_effect_append(vir_block_t *block, vir_effect_t *effect);
static void vir_repair_value_positions(vir_block_t *block);
static vir_effect_t *vir_new_effect(vir_function_t *func,
                                    vir_block_t *block,
                                    vir_effect_kind_t kind);

#define vir_remove_user_use(value, user, operand) \
    vir_remove_matching_use(value, VIR_USE_USER, user, operand)
#define vir_remove_return_use(value, block) \
    vir_remove_matching_use(value, VIR_USE_RETURN, block, -1)
#define vir_remove_branch_use(value, block) \
    vir_remove_matching_use(value, VIR_USE_BRANCH, block, -1)
#define vir_remove_effect_use(value, effect, operand) \
    vir_remove_matching_use(value, VIR_USE_EFFECT, effect, operand)

static bool vir_use_matches(const vir_use_t *use,
                            vir_use_owner_t kind,
                            const void *owner,
                            int operand)
{
    bool matches;

    switch (kind) {
    case VIR_USE_EXACT:
        matches = use == owner;
        break;
    case VIR_USE_USER:
        matches = use->user == owner;
        break;
    case VIR_USE_EDGE:
        matches = use->edge == owner;
        break;
    case VIR_USE_EFFECT:
        matches = use->effect == owner;
        break;
    case VIR_USE_RETURN:
        matches = use->return_block == owner;
        break;
    case VIR_USE_BRANCH:
        matches = use->branch_block == owner;
        break;
    default:
        matches = false;
        break;
    }
    return matches && (operand < 0 || use->operand == operand);
}

int vir_integer_type_width(vir_type_t type)
{
    return (unsigned int) type < sizeof(vir_types) / sizeof(*vir_types)
               ? vir_types[type].width
               : 0;
}

vir_type_t vir_integer_type_from_size(int bytes)
{
    for (vir_type_t type = VIR_TYPE_I8; type <= VIR_TYPE_I64; type++)
        if (vir_integer_type_width(type) / 8 == bytes)
            return type;
    return VIR_TYPE_VOID;
}

bool vir_type_is_scalar(vir_type_t type)
{
    return vir_integer_type_width(type) || type == VIR_TYPE_PTR;
}

bool vir_call_shape_valid(const vir_effect_t *effect, int max_args)
{
    return effect && effect->kind == VIR_EFFECT_CALL && max_args >= 0 &&
           (effect->result_type == VIR_TYPE_VOID ||
            vir_type_is_scalar(effect->result_type)) &&
           (effect->result ? effect->result->type == effect->result_type
                           : effect->result_type == VIR_TYPE_VOID) &&
           effect->arg_count >= 0 && effect->arg_count <= max_args &&
           (!effect->arg_count || (effect->args && effect->arg_uses));
}

bool vir_function_shape(const vir_function_t *func, vir_function_shape_t *shape)
{
    if (!func || !shape)
        return false;
    memset(shape, 0, sizeof(*shape));
    for (const vir_block_t *block = func->blocks; block; block = block->next) {
        shape->has_effects |= block->effects != NULL;
        for (const vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing) {
            if (edge->arg_count < 0)
                return false;
            if (edge->arg_count > shape->max_edge_args)
                shape->max_edge_args = edge->arg_count;
        }
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next)
            if (effect->kind == VIR_EFFECT_CALL) {
                if (effect->arg_count < 0)
                    return false;
                shape->has_calls = true;
                if (effect->arg_count > shape->max_call_args)
                    shape->max_call_args = effect->arg_count;
            }
        for (const vir_value_t *value = block->head; value; value = value->next)
            if (value->opcode == VIR_OP_STACK_ADDR) {
                if (value->address_slot >= INT_MAX)
                    return false;
                if (shape->root_count <= (int) value->address_slot)
                    shape->root_count = value->address_slot + 1;
            } else if (value->opcode == VIR_OP_GLOBAL_ADDR) {
                shape->has_globals = true;
            }
    }
    return true;
}

static bool vir_opt_level_valid(vir_opt_level_t level)
{
    return level >= VIR_OPT_O0 && level <= VIR_OPT_O2;
}

static int vir_type_width(const vir_function_t *func, vir_type_t type)
{
    int width = vir_integer_type_width(type);

    if (width)
        return width;
    return type == VIR_TYPE_PTR ? func->pointer_bits : 0;
}

static unsigned long long vir_width_mask(int width)
{
    return width == 64 ? ~0ULL : ((1ULL << width) - 1ULL);
}

static unsigned long long vir_fold_unary(vir_opcode_t opcode,
                                         int source_width,
                                         unsigned long long value)
{
    unsigned long long mask = vir_width_mask(source_width);

    if (opcode == VIR_OP_NEG)
        return (0ULL - value) & mask;
    if (opcode == VIR_OP_BITNOT)
        return ~value & mask;
    if (opcode != VIR_OP_SEXT)
        return value;
    value &= mask;
    if (source_width < 64 && (value & (1ULL << (source_width - 1))))
        value |= ~mask;
    return value;
}

static unsigned long long vir_fold_integer_cast(vir_opcode_t opcode,
                                                int source_width,
                                                int destination_width,
                                                unsigned long long value)
{
    if (opcode == VIR_OP_SEXT)
        return vir_fold_unary(opcode, source_width, value) &
               vir_width_mask(destination_width);
    return value & vir_width_mask(opcode == VIR_OP_TRUNC ? destination_width
                                                         : source_width);
}

static bool vir_fold_signed_div(int width,
                                unsigned long long left,
                                unsigned long long right,
                                bool remainder,
                                unsigned long long *result)
{
    unsigned long long mask = vir_width_mask(width);
    unsigned long long sign = 1ULL << (width - 1);
    unsigned long long left_bits = left & mask;
    unsigned long long right_bits = right & mask;
    bool left_negative = (left_bits & sign) != 0;
    bool right_negative = (right_bits & sign) != 0;
    unsigned long long left_magnitude;
    unsigned long long right_magnitude;
    unsigned long long magnitude;
    bool result_negative;

    if (!right_bits || (left_bits == sign && right_bits == mask))
        return false;
    left_magnitude = left_negative ? (0ULL - left_bits) & mask : left_bits;
    right_magnitude = right_negative ? (0ULL - right_bits) & mask : right_bits;
    magnitude = remainder ? left_magnitude % right_magnitude
                          : left_magnitude / right_magnitude;
    result_negative =
        remainder ? left_negative : left_negative != right_negative;
    *result = result_negative ? 0ULL - magnitude : magnitude;
    *result &= mask;
    return true;
}

static bool vir_fold_integer_shift(int width,
                                   vir_opcode_t opcode,
                                   unsigned long long value,
                                   unsigned long long count,
                                   unsigned long long *result)
{
    if (count >= (unsigned int) width)
        return false;
    *result = opcode == VIR_OP_SHL ? value << count : value >> count;
    if (opcode == VIR_OP_ASHR && count && (value & (1ULL << (width - 1)))) {
        unsigned long long mask = vir_width_mask(width);

        *result |= mask ^ (mask >> count);
    }
    return true;
}

static bool vir_fold_binary(const vir_function_t *func,
                            vir_opcode_t opcode,
                            vir_type_t result_type,
                            vir_type_t operand_type,
                            unsigned long long left,
                            unsigned long long right,
                            unsigned long long *result)
{
    int width = vir_type_width(func, result_type);
    int operand_width = vir_type_width(func, operand_type);
    unsigned long long mask;

    if (!width || !operand_width)
        return false;
    mask = vir_width_mask(operand_width);

    switch (opcode) {
    case VIR_OP_ADD:
        *result = left + right;
        break;
    case VIR_OP_SUB:
        *result = left - right;
        break;
    case VIR_OP_MUL:
        *result = left * right;
        break;
    case VIR_OP_SDIV:
    case VIR_OP_SREM:
        if (!vir_fold_signed_div(operand_width, left, right,
                                 opcode == VIR_OP_SREM, result))
            return false;
        break;
    case VIR_OP_UDIV:
    case VIR_OP_UREM:
        if (!right)
            return false;
        *result = opcode == VIR_OP_UDIV ? left / right : left % right;
        break;
    case VIR_OP_SHL:
    case VIR_OP_ASHR:
    case VIR_OP_LSHR:
        if (!vir_fold_integer_shift(operand_width, opcode, left, right, result))
            return false;
        break;
    case VIR_OP_BITAND:
        *result = left & right;
        break;
    case VIR_OP_BITOR:
        *result = left | right;
        break;
    case VIR_OP_BITXOR:
        *result = left ^ right;
        break;
    case VIR_OP_EQ:
        *result = left == right;
        break;
    case VIR_OP_SLT: {
        unsigned long long sign = 1ULL << (operand_width - 1);

        *result = ((left & mask) ^ sign) < ((right & mask) ^ sign);
        break;
    }
    case VIR_OP_ULT:
        *result = left < right;
        break;
    case VIR_OP_PTRADD:
        *result = left + right;
        break;
    default:
        return false;
    }
    *result &= vir_width_mask(width);
    return true;
}

bool vir_fold_i32_binary(vir_opcode_t opcode, int left, int right, int *result)
{
    unsigned long long folded;

    if (!vir_fold_binary(NULL, opcode, VIR_TYPE_I32, VIR_TYPE_I32,
                         (unsigned int) left, (unsigned int) right, &folded))
        return false;
    *result = (int) (unsigned int) folded;
    return true;
}

static unsigned long long vir_integer_mask(vir_type_t type)
{
    int width = vir_integer_type_width(type);
    return vir_width_mask(width);
}

static bool vir_valid_alignment(unsigned int alignment)
{
    return alignment && !(alignment & (alignment - 1));
}

static const char *vir_type_name(vir_type_t type)
{
    return (unsigned int) type < sizeof(vir_types) / sizeof(*vir_types)
               ? vir_types[type].name
               : "void";
}

static const char *vir_opcode_name(vir_opcode_t opcode)
{
    switch (opcode) {
#define VIR_OPCODE_ITEM(name, spelling) \
    case VIR_OP_##name:                 \
        return spelling;
        VIR_OPCODE_LIST
#undef VIR_OPCODE_ITEM
    default:
        return "invalid";
    }
}

#define VIR_OPCODE_ITEM(name, spelling) case VIR_OP_##name:
#define DEFINE_VIR_OPCODE_PREDICATE(name, opcodes) \
    static bool name(vir_opcode_t opcode)          \
    {                                              \
        switch (opcode) {                          \
            opcodes return true;                   \
        default:                                   \
            return false;                          \
        }                                          \
    }
DEFINE_VIR_OPCODE_PREDICATE(vir_is_pure_unary_opcode,
                            VIR_PURE_UNARY_OPCODE_LIST)
DEFINE_VIR_OPCODE_PREDICATE(vir_is_pure_binary_opcode,
                            VIR_PURE_BINARY_OPCODE_LIST)
#undef DEFINE_VIR_OPCODE_PREDICATE
#undef VIR_OPCODE_ITEM

static void *vir_arena_alloc(vir_arena_t *arena, int size)
{
    const int align = sizeof(void *);
    vir_arena_block_t *block;
    int capacity;

    if (size <= 0 || size > INT_MAX - align + 1 || arena->fail_after == 0)
        return NULL;
    if (arena->fail_after > 0)
        arena->fail_after--;
    size = (size + align - 1) & ~(align - 1);
    block = arena->head;
    if (!block || size > block->capacity - block->used) {
        capacity = arena->block_size;
        if (capacity < size)
            capacity = size;
        if (arena->capacity > INT_MAX - capacity)
            return NULL;
        block = malloc(sizeof(*block) + capacity - 1);
        if (!block)
            return NULL;
        block->next = arena->head;
        block->capacity = capacity;
        block->used = 0;
        arena->head = block;
        arena->capacity += capacity;
        if (arena->capacity > arena->peak_capacity)
            arena->peak_capacity = arena->capacity;
    }

    void *ptr = block->data + block->used;
    block->used += size;
    memset(ptr, 0, size);
    return ptr;
}

void vir_function_init(vir_function_t *func, int arena_block_size)
{
    memset(func, 0, sizeof(*func));
    func->arena.block_size = arena_block_size > 0 ? arena_block_size : 4096;
    func->arena.fail_after = -1;
}

int vir_function_set_pointer_bits(vir_function_t *func, int pointer_bits)
{
    if (!func || func->blocks || (pointer_bits != 32 && pointer_bits != 64))
        return 0;
    func->pointer_bits = pointer_bits;
    return 1;
}

void vir_function_release(vir_function_t *func)
{
    vir_arena_block_t *block = func->arena.head;
    while (block) {
        vir_arena_block_t *next = block->next;
        free(block);
        block = next;
    }
    memset(func, 0, sizeof(*func));
}

static void vir_collect_value_stats(const vir_value_t *value,
                                    vir_stats_t *stats)
{
    stats->values++;
    for (const vir_use_t *use = value->uses; use; use = use->next)
        stats->uses++;
}

void vir_collect_stats(const vir_function_t *func, vir_stats_t *stats)
{
    const vir_block_t *block;
    memset(stats, 0, sizeof(*stats));
    stats->arena_capacity = func->arena.capacity;
    stats->arena_peak_capacity = func->arena.peak_capacity;
    for (block = func->blocks; block; block = block->next) {
        const vir_value_t *value;
        const vir_edge_t *edge;
        const vir_effect_t *effect;
        stats->blocks++;
        for (value = block->params; value; value = value->param_next) {
            stats->params++;
            vir_collect_value_stats(value, stats);
        }
        for (value = block->head; value; value = value->next) {
            vir_collect_value_stats(value, stats);
        }
        for (edge = block->outgoing; edge; edge = edge->next_outgoing)
            stats->edges++;
        for (effect = block->effects; effect; effect = effect->next) {
            stats->effects++;
            stats->volatile_effects +=
                effect->kind == VIR_EFFECT_VOLATILE_LOAD ||
                effect->kind == VIR_EFFECT_VOLATILE_STORE;
        }
    }
}

vir_block_t *vir_block_create(vir_function_t *func)
{
    vir_block_t *block = vir_arena_alloc(&func->arena, sizeof(*block));
    if (!block)
        return NULL;
    block->id = func->next_block_id++;
    if (func->last_block)
        func->last_block->next = block;
    else
        func->blocks = block;
    func->last_block = block;
    return block;
}

static vir_value_t *vir_value_alloc(vir_function_t *func,
                                    vir_block_t *block,
                                    vir_type_t type)
{
    vir_value_t *value;

    if (!vir_has_block(func, block) || !vir_type_width(func, type))
        return NULL;
    value = vir_arena_alloc(&func->arena, sizeof(*value));
    if (!value)
        return NULL;
    value->id = func->next_value_id++;
    value->type = type;
    value->block = block;
    return value;
}

static vir_value_t *vir_new_value(vir_function_t *func,
                                  vir_block_t *block,
                                  vir_opcode_t opcode,
                                  vir_type_t type,
                                  int nr_ops)
{
    vir_value_t *value = vir_value_alloc(func, block, type);

    if (!value)
        return NULL;
    value->opcode = opcode;
    value->nr_ops = nr_ops;
    value->position = block->tail ? block->tail->position + 1 : 0;
    value->order = block->next_order++;
    if (block->tail)
        block->tail->next = value;
    else
        block->head = value;
    block->tail = value;
    return value;
}

static vir_value_t *vir_const(vir_function_t *func,
                              vir_block_t *block,
                              vir_type_t type,
                              unsigned long long constant)
{
    vir_value_t *value = vir_new_value(func, block, VIR_OP_CONST, type, 0);
    if (value)
        value->constant = constant & vir_width_mask(vir_type_width(func, type));
    return value;
}

vir_value_t *vir_const_i32(vir_function_t *func,
                           vir_block_t *block,
                           int constant)
{
    return vir_const(func, block, VIR_TYPE_I32,
                     (unsigned long long) (unsigned int) constant);
}

vir_value_t *vir_const_i1(vir_function_t *func,
                          vir_block_t *block,
                          int constant)
{
    return vir_const(func, block, VIR_TYPE_I1, !!constant);
}

vir_value_t *vir_const_int(vir_function_t *func,
                           vir_block_t *block,
                           vir_type_t type,
                           unsigned long long bits)
{
    return vir_const(func, block, type, bits);
}

vir_value_t *vir_const_ptr(vir_function_t *func,
                           vir_block_t *block,
                           unsigned long long bits)
{
    return vir_const(func, block, VIR_TYPE_PTR, bits);
}

static bool vir_address_root_conflicts(const vir_function_t *func,
                                       vir_address_kind_t kind,
                                       unsigned int slot,
                                       const char *name,
                                       unsigned int size,
                                       unsigned int alignment)
{
    const vir_block_t *block;

    for (block = func->blocks; block; block = block->next) {
        const vir_value_t *value;
        for (value = block->head; value; value = value->next) {
            bool same_root = kind == VIR_ADDRESS_STACK
                                 ? value->opcode == VIR_OP_STACK_ADDR &&
                                       value->address_slot == slot
                                 : value->opcode == VIR_OP_GLOBAL_ADDR &&
                                       value->address_name &&
                                       !strcmp(value->address_name, name);

            if (same_root && (value->address_size != size ||
                              value->address_alignment != alignment))
                return 1;
        }
    }
    return 0;
}

static vir_value_t *new_addr(vir_function_t *func,
                             vir_block_t *block,
                             vir_address_kind_t kind,
                             unsigned int slot,
                             const char *name,
                             unsigned int size,
                             unsigned int alignment)
{
    char *copy = NULL;
    vir_value_t *value;

    if (!vir_has_block(func, block) || !vir_type_width(func, VIR_TYPE_PTR) ||
        !size || !vir_valid_alignment(alignment) ||
        (kind == VIR_ADDRESS_GLOBAL && (!name || !*name)) ||
        vir_address_root_conflicts(func, kind, slot, name, size, alignment))
        return NULL;
    if (kind == VIR_ADDRESS_GLOBAL) {
        size_t length = strlen(name) + 1;

        if (length > (size_t) 0x7fffffff ||
            !(copy = vir_arena_alloc(&func->arena, (int) length)))
            return NULL;
        memcpy(copy, name, length);
    }
    value = vir_new_value(
        func, block,
        kind == VIR_ADDRESS_STACK ? VIR_OP_STACK_ADDR : VIR_OP_GLOBAL_ADDR,
        VIR_TYPE_PTR, 0);
    if (!value)
        return NULL;
    value->address_kind = kind;
    value->address_slot = slot;
    value->address_size = size;
    value->address_alignment = alignment;
    value->address_name = copy;
    return value;
}

vir_value_t *vir_stack_addr(vir_function_t *func,
                            vir_block_t *block,
                            unsigned int slot,
                            unsigned int size,
                            unsigned int alignment)
{
    return new_addr(func, block, VIR_ADDRESS_STACK, slot, NULL, size,
                    alignment);
}

vir_value_t *vir_global_addr(vir_function_t *func,
                             vir_block_t *block,
                             const char *name,
                             unsigned int size,
                             unsigned int alignment)
{
    return new_addr(func, block, VIR_ADDRESS_GLOBAL, 0, name, size, alignment);
}

vir_value_t *vir_function_addr(vir_function_t *func,
                               vir_block_t *block,
                               const char *name)
{
    vir_value_t *value;
    char *copy;
    size_t length;

    if (!vir_has_block(func, block) || !name || !*name)
        return NULL;
    length = strlen(name) + 1;
    if (length > INT_MAX ||
        !(copy = vir_arena_alloc(&func->arena, (int) length)))
        return NULL;
    memcpy(copy, name, length);
    value = vir_new_value(func, block, VIR_OP_FUNC_ADDR, VIR_TYPE_PTR, 0);
    if (value)
        value->address_name = copy;
    return value;
}

vir_value_t *vir_rodata_addr(vir_function_t *func,
                             vir_block_t *block,
                             unsigned int offset)
{
    vir_value_t *value;

    if (!vir_has_block(func, block) || !vir_type_width(func, VIR_TYPE_PTR))
        return NULL;
    value = vir_new_value(func, block, VIR_OP_RODATA_ADDR, VIR_TYPE_PTR, 0);
    if (value)
        value->constant = offset;
    return value;
}

static void vir_block_append_param(vir_block_t *block, vir_value_t *param)
{
    param->is_block_param = 1;
    param->opcode = VIR_OP_PARAM;
    param->block = block;
    if (block->last_param)
        block->last_param->param_next = param;
    else
        block->params = param;
    block->last_param = param;
    block->param_count++;
}

vir_value_t *vir_block_add_param(vir_function_t *func,
                                 vir_block_t *block,
                                 vir_type_t type)
{
    vir_value_t *value = vir_value_alloc(func, block, type);

    if (!value)
        return NULL;

    /* Its own opcode, so no "opcode == VIR_OP_CONST" test can mistake a block
     * parameter, whose constant field is zero, for the constant 0.
     */
    vir_block_append_param(block, value);
    return value;
}

static bool vir_values_owned(const vir_function_t *func,
                             vir_value_t *const *values,
                             int count)
{
    if (!func || count < 0 || (count && !values))
        return false;
    for (int i = 0; i < count; i++)
        if (!values[i] || !vir_has_value(func, values[i]))
            return false;
    return true;
}

static bool vir_edge_args_match(const vir_function_t *func,
                                vir_block_t *to,
                                vir_value_t **args,
                                int arg_count,
                                bool check_ownership)
{
    vir_value_t *param;
    int i = 0;

    if (!to || !vir_has_block(func, to) || arg_count < 0 ||
        arg_count != to->param_count || (arg_count && !args) ||
        (check_ownership && !vir_values_owned(func, args, arg_count)))
        return 0;
    param = to->params;
    for (i = 0; i < arg_count; i++) {
        if (!args[i] || !param || args[i]->type != param->type)
            return 0;
        param = param->param_next;
    }
    return 1;
}

static vir_edge_t *vir_edge_alloc(vir_function_t *func, int arg_count)
{
    vir_edge_t *edge = vir_arena_alloc(&func->arena, sizeof(*edge));

    if (!edge)
        return NULL;
    if (arg_count) {
        edge->args =
            vir_arena_alloc(&func->arena, sizeof(*edge->args) * arg_count);
        edge->arg_uses =
            vir_arena_alloc(&func->arena, sizeof(*edge->arg_uses) * arg_count);
        if (!edge->args || !edge->arg_uses)
            return NULL;
    }
    edge->arg_count = arg_count;
    return edge;
}

static vir_edge_t *vir_edge_prepare(vir_function_t *func,
                                    vir_block_t *from,
                                    vir_block_t *to,
                                    vir_value_t **args,
                                    int arg_count)
{
    vir_edge_t *edge;

    edge = vir_edge_alloc(func, arg_count);
    if (!edge)
        return NULL;
    edge->from = from;
    edge->to = to;
    if (arg_count)
        memcpy(edge->args, args, sizeof(*args) * arg_count);
    return edge;
}

static void vir_link_use(vir_value_t *value, vir_use_t *use)
{
    use->next = value->uses;
    value->uses = use;
}

static void vir_link_effect_use(vir_effect_t *effect,
                                vir_value_t *value,
                                vir_use_t *use,
                                int operand)
{
    use->effect = effect;
    use->operand = operand;
    vir_link_use(value, use);
}

static void vir_edge_link_uses(vir_edge_t *edge)
{
    for (int i = 0; i < edge->arg_count; i++) {
        vir_use_t *use = &edge->arg_uses[i];
        use->edge = edge;
        use->operand = i;
        vir_link_use(edge->args[i], use);
    }
}

static void vir_edge_replace_args(vir_edge_t *edge,
                                  vir_value_t **args,
                                  vir_use_t *uses,
                                  int count)
{
    int i;

    for (i = 0; i < edge->arg_count; i++)
        vir_remove_use(edge->args[i], &edge->arg_uses[i]);
    edge->args = args;
    edge->arg_uses = uses;
    edge->arg_count = count;
    vir_edge_link_uses(edge);
}

static void vir_edge_link(vir_edge_t *edge)
{
    edge->next_outgoing = edge->from->outgoing;
    edge->from->outgoing = edge;
    edge->next_incoming = edge->to->incoming;
    edge->to->incoming = edge;
    vir_edge_link_uses(edge);
}

static void vir_edge_remove_from_list(vir_edge_t **head,
                                      vir_edge_t *edge,
                                      bool outgoing)
{
    vir_edge_t **link = head;

    while (*link != edge)
        link = outgoing ? &(*link)->next_outgoing : &(*link)->next_incoming;
    *link = outgoing ? edge->next_outgoing : edge->next_incoming;
}

static bool vir_edge_is_linked(const vir_function_t *func,
                               const vir_edge_t *edge)
{
    return func && edge && vir_has_block(func, edge->from) &&
           vir_has_block(func, edge->to) &&
           vir_has_outgoing_edge(edge->from, edge) &&
           vir_has_incoming_edge(edge->to, edge);
}

vir_edge_t *vir_edge_create(vir_function_t *func,
                            vir_block_t *from,
                            vir_block_t *to,
                            vir_value_t **args,
                            int arg_count)
{
    vir_edge_t *edge;
    if (!vir_block_open(func, from) ||
        !vir_edge_args_match(func, to, args, arg_count, true))
        return NULL;
    edge = vir_edge_prepare(func, from, to, args, arg_count);
    if (edge) {
        vir_edge_link(edge);
        from->terminator = VIR_TERM_JUMP;
    }
    return edge;
}

static void vir_edge_unlink(vir_edge_t *edge)
{
    int i;

    vir_edge_remove_from_list(&edge->from->outgoing, edge, true);
    vir_edge_remove_from_list(&edge->to->incoming, edge, false);
    for (i = 0; i < edge->arg_count; i++)
        vir_remove_use(edge->args[i], &edge->arg_uses[i]);
}

int vir_edge_delete(vir_function_t *func, vir_edge_t *edge)
{
    vir_block_t *from;
    vir_edge_t *survivor = NULL;

    if (!vir_edge_is_linked(func, edge))
        return 0;
    from = edge->from;
    if (from->terminator == VIR_TERM_BRANCH) {
        if (!from->true_edge || !from->false_edge ||
            (edge != from->true_edge && edge != from->false_edge))
            return 0;
        survivor = edge == from->true_edge ? from->false_edge : from->true_edge;
        vir_remove_branch_use(from->branch_condition, from);
    } else if (from->terminator != VIR_TERM_JUMP || from->outgoing != edge ||
               edge->next_outgoing) {
        return 0;
    }
    vir_edge_unlink(edge);
    if (!survivor) {
        from->terminator = VIR_TERM_NONE;
        return 1;
    }
    /* Deleting one arm leaves the other edge as a jump. */
    from->terminator = VIR_TERM_JUMP;
    from->branch_condition = NULL;
    from->true_edge = NULL;
    from->false_edge = NULL;
    return from->outgoing == survivor && !survivor->next_outgoing;
}

int vir_edge_redirect(vir_function_t *func, vir_edge_t *edge, vir_block_t *to)
{
    if (!vir_edge_is_linked(func, edge) ||
        !vir_edge_args_match(func, to, edge->args, edge->arg_count, false))
        return 0;
    if (edge->to == to)
        return 1;
    vir_edge_remove_from_list(&edge->to->incoming, edge, false);
    edge->to = to;
    edge->next_incoming = to->incoming;
    to->incoming = edge;
    return 1;
}

vir_block_t *vir_edge_split(vir_function_t *func, vir_edge_t *edge)
{
    vir_block_t *split;
    vir_edge_t *forward;
    vir_value_t *target_param;
    int i;

    if (!vir_edge_is_linked(func, edge))
        return NULL;
    split = vir_arena_alloc(&func->arena, sizeof(*split));
    if (!split)
        return NULL;
    target_param = edge->to->params;
    for (i = 0; i < edge->arg_count; i++) {
        vir_value_t *param = vir_arena_alloc(&func->arena, sizeof(*param));
        if (!param || !target_param)
            return NULL;
        param->type = target_param->type;
        vir_block_append_param(split, param);
        target_param = target_param->param_next;
    }
    if (target_param)
        return NULL;
    forward = vir_edge_alloc(func, edge->arg_count);
    if (!forward)
        return NULL;
    split->id = func->next_block_id++;
    if (func->last_block)
        func->last_block->next = split;
    else
        func->blocks = split;
    func->last_block = split;
    for (target_param = split->params, i = 0; target_param;
         target_param = target_param->param_next, i++) {
        target_param->id = func->next_value_id++;
        forward->args[i] = target_param;
    }
    forward->from = split;
    forward->to = edge->to;
    vir_edge_link(forward);
    split->terminator = VIR_TERM_JUMP;
    if (!vir_edge_redirect(func, edge, split))
        return NULL;
    return split;
}

int vir_block_set_branch(vir_function_t *func,
                         vir_block_t *block,
                         vir_value_t *condition,
                         const vir_edge_args_t *true_desc,
                         const vir_edge_args_t *false_desc)
{
    vir_edge_t *true_edge;
    vir_edge_t *false_edge;
    vir_use_t *condition_use;
    if (!condition || !true_desc || !false_desc ||
        condition->type != VIR_TYPE_I1 || !vir_block_open(func, block))
        return 0;
    if (!vir_has_value(func, condition) ||
        !vir_edge_args_match(func, true_desc->to, true_desc->args,
                             true_desc->arg_count, true) ||
        !vir_edge_args_match(func, false_desc->to, false_desc->args,
                             false_desc->arg_count, true))
        return 0;
    true_edge = vir_edge_prepare(func, block, true_desc->to, true_desc->args,
                                 true_desc->arg_count);
    if (!true_edge)
        return 0;
    false_edge = vir_edge_prepare(func, block, false_desc->to, false_desc->args,
                                  false_desc->arg_count);
    if (!false_edge)
        return 0;
    condition_use = vir_arena_alloc(&func->arena, sizeof(*condition_use));
    if (!condition_use)
        return 0;
    vir_edge_link(true_edge);
    vir_edge_link(false_edge);
    block->terminator = VIR_TERM_BRANCH;
    block->branch_condition = condition;
    block->true_edge = true_edge;
    block->false_edge = false_edge;
    condition_use->branch_block = block;
    vir_link_use(condition, condition_use);
    return 1;
}

int vir_block_set_return(vir_function_t *func,
                         vir_block_t *block,
                         vir_value_t *value)
{
    vir_use_t *use;
    if (!vir_block_open(func, block) || (value && !vir_has_value(func, value)))
        return 0;
    /* A void function returns no value, so the return records no use. */
    if (!value) {
        block->terminator = VIR_TERM_RETURN;
        block->return_value = NULL;
        return 1;
    }
    use = vir_arena_alloc(&func->arena, sizeof(*use));
    if (!use)
        return 0;
    block->terminator = VIR_TERM_RETURN;
    block->return_value = value;
    use->return_block = block;
    vir_link_use(value, use);
    return 1;
}

vir_effect_t *vir_effect_create(vir_function_t *func,
                                vir_block_t *block,
                                vir_effect_kind_t kind)
{
    vir_effect_t *effect;
    if (!vir_block_open(func, block) ||
        (kind != VIR_EFFECT_CALL && kind != VIR_EFFECT_VOLATILE))
        return NULL;
    effect = vir_new_effect(func, block, kind);
    if (!effect)
        return NULL;
    vir_effect_append(block, effect);
    return effect;
}

static void vir_effect_append(vir_block_t *block, vir_effect_t *effect)
{
    effect->position =
        block->last_effect ? block->last_effect->position + 1 : 0;
    effect->order =
        effect->result ? effect->result->order : block->next_order++;
    if (block->last_effect)
        block->last_effect->next = effect;
    else
        block->effects = effect;
    block->last_effect = effect;
}

static vir_effect_t *vir_new_effect(vir_function_t *func,
                                    vir_block_t *block,
                                    vir_effect_kind_t kind)
{
    vir_effect_t *effect = vir_arena_alloc(&func->arena, sizeof(*effect));

    if (effect) {
        effect->kind = kind;
        effect->block = block;
    }
    return effect;
}

static void vir_link_effect_operand(vir_effect_t *effect,
                                    vir_value_t **slot,
                                    vir_use_t **use_slot,
                                    vir_value_t *value,
                                    vir_use_t *use,
                                    int operand)
{
    *slot = value;
    *use_slot = use;
    vir_link_effect_use(effect, value, use, operand);
}

static void vir_link_user_operand(vir_value_t *user,
                                  vir_value_t *operand,
                                  vir_use_t *use,
                                  int index)
{
    use->user = user;
    use->operand = index;
    vir_link_use(operand, use);
}

static vir_value_t *vir_user_operation(vir_function_t *func,
                                       vir_block_t *block,
                                       vir_opcode_t opcode,
                                       vir_type_t type,
                                       vir_value_t *op0,
                                       vir_value_t *op1)
{
    int nr_ops = op1 ? 2 : 1;
    vir_use_t *uses = vir_arena_alloc(&func->arena, sizeof(*uses) * nr_ops);
    vir_value_t *value;

    if (!uses || !(value = vir_new_value(func, block, opcode, type, nr_ops)))
        return NULL;
    value->op0 = op0;
    vir_link_user_operand(value, op0, &uses[0], 0);
    if (op1) {
        value->op1 = op1;
        vir_link_user_operand(value, op1, &uses[1], 1);
    }
    return value;
}

static vir_value_t *vir_load_impl(vir_function_t *func,
                                  vir_block_t *block,
                                  vir_value_t *address,
                                  vir_type_t type,
                                  bool is_volatile)
{
    vir_effect_t *effect;
    vir_use_t *use;
    vir_value_t *value;

    if (!vir_block_open(func, block) ||
        (address &&
         (!vir_has_value(func, address) || address->type != VIR_TYPE_PTR)) ||
        !vir_type_width(func, type))
        return NULL;
    effect = vir_new_effect(
        func, block, is_volatile ? VIR_EFFECT_VOLATILE_LOAD : VIR_EFFECT_LOAD);
    use = address ? vir_arena_alloc(&func->arena, sizeof(*use)) : NULL;
    if (!effect || (address && !use))
        return NULL;
    value = vir_new_value(func, block, VIR_OP_LOAD, type, 0);
    if (!value)
        return NULL;
    value->def_effect = effect;
    effect->result = value;
    if (address)
        vir_link_effect_operand(effect, &effect->address, &effect->address_use,
                                address, use, 0);
    vir_effect_append(block, effect);
    return value;
}

#define DEFINE_MEMORY_LOAD(name, is_volatile)                          \
    vir_value_t *name(vir_function_t *func, vir_block_t *block,        \
                      vir_value_t *address, vir_type_t type)           \
    {                                                                  \
        if (!address)                                                  \
            return NULL;                                               \
        return vir_load_impl(func, block, address, type, is_volatile); \
    }

static bool vir_store_link_operands(vir_function_t *func,
                                    vir_effect_t *effect,
                                    vir_value_t *address,
                                    vir_value_t *value)
{
    vir_use_t *uses = vir_arena_alloc(&func->arena, sizeof(*uses) * 2);

    if (!uses)
        return false;
    vir_link_effect_operand(effect, &effect->address, &effect->address_use,
                            address, &uses[0], 0);
    vir_link_effect_operand(effect, &effect->stored_value,
                            &effect->stored_value_use, value, &uses[1], 1);
    return true;
}

static vir_effect_t *vir_store_impl(vir_function_t *func,
                                    vir_block_t *block,
                                    vir_value_t *address,
                                    vir_value_t *value,
                                    bool is_volatile)
{
    vir_effect_t *effect;

    if (!vir_block_open(func, block) || (!!address != !!value) ||
        (address &&
         (!vir_has_value(func, address) || !vir_has_value(func, value) ||
          address->type != VIR_TYPE_PTR)) ||
        (value && !vir_type_width(func, value->type)))
        return NULL;
    effect = vir_new_effect(
        func, block,
        is_volatile ? VIR_EFFECT_VOLATILE_STORE : VIR_EFFECT_STORE);
    if (!effect ||
        (address && !vir_store_link_operands(func, effect, address, value)))
        return NULL;
    vir_effect_append(block, effect);
    return effect;
}

#define DEFINE_MEMORY_STORE(name, is_volatile)                           \
    vir_effect_t *name(vir_function_t *func, vir_block_t *block,         \
                       vir_value_t *address, vir_value_t *value)         \
    {                                                                    \
        if (!address || !value)                                          \
            return NULL;                                                 \
        return vir_store_impl(func, block, address, value, is_volatile); \
    }

DEFINE_MEMORY_LOAD(vir_load, false)
DEFINE_MEMORY_LOAD(vir_volatile_load, true)
DEFINE_MEMORY_STORE(vir_store, false)
DEFINE_MEMORY_STORE(vir_volatile_store, true)

#undef DEFINE_MEMORY_LOAD
#undef DEFINE_MEMORY_STORE

vir_value_t *vir_deferred_load(vir_function_t *func,
                               vir_block_t *block,
                               vir_type_t type,
                               bool is_volatile)
{
    return vir_load_impl(func, block, NULL, type, is_volatile);
}

vir_effect_t *vir_deferred_store(vir_function_t *func,
                                 vir_block_t *block,
                                 bool is_volatile)
{
    return vir_store_impl(func, block, NULL, NULL, is_volatile);
}

bool vir_deferred_load_resolve(vir_function_t *func,
                               vir_effect_t *effect,
                               vir_value_t *address)
{
    vir_use_t *use;

    if (!func || !effect || !address || !effect->block || !effect->result ||
        !vir_effect_is_load(effect->kind) || effect->address ||
        effect->address_use || !vir_has_block(func, effect->block) ||
        !vir_has_value(func, address) || address->type != VIR_TYPE_PTR ||
        effect->result->def_effect != effect ||
        !vir_value_dominates_effect(func, NULL, address, effect))
        return false;
    use = vir_arena_alloc(&func->arena, sizeof(*use));
    if (!use)
        return false;
    vir_link_effect_operand(effect, &effect->address, &effect->address_use,
                            address, use, 0);
    return true;
}

bool vir_deferred_store_resolve(vir_function_t *func,
                                vir_effect_t *effect,
                                vir_value_t *address,
                                vir_value_t *value)
{
    if (!func || !effect || !address || !value || !effect->block ||
        !vir_effect_is_store(effect->kind) || effect->address ||
        effect->stored_value || effect->address_use ||
        effect->stored_value_use || !vir_has_block(func, effect->block) ||
        !vir_has_value(func, address) || !vir_has_value(func, value) ||
        address->type != VIR_TYPE_PTR || !vir_type_width(func, value->type) ||
        !vir_value_dominates_effect(func, NULL, address, effect) ||
        !vir_value_dominates_effect(func, NULL, value, effect))
        return false;
    return vir_store_link_operands(func, effect, address, value);
}

static vir_effect_t *vir_call_impl(vir_function_t *func,
                                   vir_block_t *block,
                                   const char *callee,
                                   vir_value_t *callee_value,
                                   vir_value_t **args,
                                   int arg_count,
                                   vir_type_t result_type)
{
    char *name;
    size_t length = 0;
    vir_effect_t *effect;
    vir_use_t *callee_use = NULL;
    vir_value_t *result = NULL;
    int i;

    if (!vir_block_open(func, block) || (!!callee == !!callee_value) ||
        (callee && !*callee) ||
        (callee_value && (callee_value->type != VIR_TYPE_PTR ||
                          !vir_has_value(func, callee_value))) ||
        arg_count > INT_MAX / (int) sizeof(*effect->args) ||
        arg_count > INT_MAX / (int) sizeof(*effect->arg_uses) ||
        !vir_values_owned(func, args, arg_count) ||
        (result_type != VIR_TYPE_VOID && !vir_type_width(func, result_type)))
        return NULL;
    name = NULL;
    if (callee) {
        length = strlen(callee) + 1;
        if (length > (size_t) 0x7fffffff)
            return NULL;
        name = vir_arena_alloc(&func->arena, (int) length);
    }
    effect = vir_new_effect(func, block, VIR_EFFECT_CALL);
    if ((callee && !name) || !effect)
        return NULL;
    if (callee_value) {
        callee_use = vir_arena_alloc(&func->arena, sizeof(*callee_use));
        if (!callee_use)
            return NULL;
    }
    if (arg_count) {
        effect->args =
            vir_arena_alloc(&func->arena, sizeof(*effect->args) * arg_count);
        effect->arg_uses = vir_arena_alloc(
            &func->arena, sizeof(*effect->arg_uses) * arg_count);
        if (!effect->args || !effect->arg_uses)
            return NULL;
    }
    if (result_type != VIR_TYPE_VOID) {
        result = vir_new_value(func, block, VIR_OP_CALL, result_type, 0);
        if (!result)
            return NULL;
    }
    if (callee)
        memcpy(name, callee, length);
    effect->callee = name;
    effect->callee_value = callee_value;
    effect->callee_value_use = callee_use;
    effect->arg_count = arg_count;
    effect->result_type = result_type;
    effect->result = result;
    if (result)
        result->def_effect = effect;
    if (callee_use)
        vir_link_effect_use(effect, callee_value, callee_use, -1);
    for (i = 0; i < arg_count; i++) {
        effect->args[i] = args[i];
        vir_link_effect_use(effect, args[i], &effect->arg_uses[i], i);
    }
    vir_effect_append(block, effect);
    return effect;
}

vir_effect_t *vir_call(vir_function_t *func,
                       vir_block_t *block,
                       const char *callee,
                       vir_value_t **args,
                       int arg_count,
                       vir_type_t result_type)
{
    return vir_call_impl(func, block, callee, NULL, args, arg_count,
                         result_type);
}

vir_effect_t *vir_call_indirect(vir_function_t *func,
                                vir_block_t *block,
                                vir_value_t *callee,
                                vir_value_t **args,
                                int arg_count,
                                vir_type_t result_type)
{
    return vir_call_impl(func, block, NULL, callee, args, arg_count,
                         result_type);
}

static bool vir_call_abi_type_valid(const vir_function_t *func,
                                    vir_call_abi_type_t type,
                                    bool allow_void)
{
    if (type.type == VIR_TYPE_VOID)
        return allow_void && !type.is_unsigned && !type.is_bool;
    if (type.type == VIR_TYPE_PTR)
        return !type.is_unsigned && !type.is_bool &&
               vir_type_width(func, type.type) != 0;
    if (type.type < VIR_TYPE_I8 || type.type > VIR_TYPE_I64)
        return false;
    if (type.is_bool)
        return type.type == VIR_TYPE_I8 && !type.is_unsigned;
    return true;
}

static bool vir_call_abi_type_matches_value(vir_call_abi_type_t abi_type,
                                            vir_type_t value_type)
{
    return abi_type.is_bool
               ? abi_type.type == VIR_TYPE_I8 && value_type == VIR_TYPE_I1
               : abi_type.type == value_type;
}

static bool vir_call_signature_matches(const vir_function_t *func,
                                       const vir_effect_t *effect,
                                       const vir_call_signature_t *signature)
{
    int i;

    if (!func || !effect || !signature || effect->kind != VIR_EFFECT_CALL ||
        (!effect->callee_value && (!effect->callee || !*effect->callee)) ||
        !vir_has_block(func, effect->block) ||
        !vir_has_effect(effect->block, effect) ||
        (effect->callee_value && !vir_has_value(func, effect->callee_value)) ||
        (signature->va_start_slot != (unsigned int) -1 &&
         !signature->is_variadic) ||
        signature->param_count < 0 ||
        signature->param_count != effect->arg_count ||
        (signature->is_variadic &&
         (signature->fixed_param_count < 0 ||
          signature->fixed_param_count > signature->param_count)) ||
        (signature->param_count && !signature->params) ||
        signature->param_count > INT_MAX / (int) sizeof(*signature->params) ||
        !vir_values_owned(func, effect->args, effect->arg_count) ||
        !vir_call_abi_type_valid(func, signature->result, true) ||
        !vir_call_abi_type_matches_value(signature->result,
                                         effect->result_type))
        return false;
    for (i = 0; i < signature->param_count; i++)
        if (!vir_call_abi_type_valid(func, signature->params[i], false) ||
            !vir_call_abi_type_matches_value(signature->params[i],
                                             effect->args[i]->type))
            return false;
    return true;
}

bool vir_call_set_signature(vir_function_t *func,
                            vir_effect_t *effect,
                            const vir_call_signature_t *signature)
{
    vir_call_signature_t *copy;
    vir_call_abi_type_t *params = NULL;

    if (!effect || effect->signature ||
        !vir_call_signature_matches(func, effect, signature))
        return false;
    copy = vir_arena_alloc(&func->arena, sizeof(*copy));
    if (!copy)
        return false;
    if (signature->param_count) {
        params = vir_arena_alloc(
            &func->arena, (int) (sizeof(*params) * signature->param_count));
        if (!params)
            return false;
        memcpy(params, signature->params,
               sizeof(*params) * signature->param_count);
    }
    *copy = *signature;
    copy->params = params;
    copy->param_slots = NULL;
    copy->va_start_slot = (unsigned int) -1;
    effect->signature = copy;
    return true;
}

/* Operand and result type rules shared by construction and verification. */
static bool vir_binary_types_valid(const vir_function_t *func,
                                   vir_opcode_t opcode,
                                   vir_type_t result,
                                   vir_type_t left,
                                   vir_type_t right)
{
    if (!vir_is_pure_binary_opcode(opcode))
        return false;
    if (opcode == VIR_OP_PTRADD)
        return result == VIR_TYPE_PTR && left == VIR_TYPE_PTR &&
               vir_integer_type_width(right) == func->pointer_bits;
    if (vir_opcode_is_shift(opcode))
        return vir_type_is_i32_or_i64(left) && right == VIR_TYPE_I32 &&
               result == left;
    if (vir_opcode_is_compare(opcode))
        return left == right && result == VIR_TYPE_I1 &&
               (opcode == VIR_OP_EQ ? vir_type_width(func, left)
                                    : vir_type_is_i32_or_i64(left));
    return left == right && result == left && vir_type_is_i32_or_i64(result);
}

vir_value_t *vir_binary(vir_function_t *func,
                        vir_block_t *block,
                        vir_opcode_t opcode,
                        vir_type_t type,
                        vir_value_t *op0,
                        vir_value_t *op1)
{
    if (func && opcode == VIR_OP_PTRADD && func->pointer_bits == 64 && op1 &&
        op1->type == VIR_TYPE_I32)
        op1 = vir_sext(func, block, op1, VIR_TYPE_I64);
    if (!vir_has_block(func, block) || !vir_has_value(func, op0) ||
        !vir_has_value(func, op1) ||
        !vir_binary_types_valid(func, opcode, type, op0->type, op1->type))
        return NULL;
    return vir_user_operation(func, block, opcode, type, op0, op1);
}

static bool vir_integer_cast_valid(vir_opcode_t opcode,
                                   vir_type_t destination,
                                   vir_type_t source)
{
    int to = vir_integer_type_width(destination);
    int from = vir_integer_type_width(source);

    return to && from &&
           (opcode == VIR_OP_TRUNC
                ? to < from
                : (opcode == VIR_OP_ZEXT || opcode == VIR_OP_SEXT) &&
                      to > from);
}

/* Operand and result type rules shared by construction and verification. */
static bool vir_unary_types_valid(const vir_function_t *func,
                                  vir_opcode_t opcode,
                                  vir_type_t result,
                                  vir_type_t operand)
{
    if (opcode == VIR_OP_NEG || opcode == VIR_OP_BITNOT)
        return result == operand && vir_type_is_i32_or_i64(result);
    if (opcode == VIR_OP_ZEXT || opcode == VIR_OP_SEXT ||
        opcode == VIR_OP_TRUNC)
        return vir_integer_cast_valid(opcode, result, operand);
    if (opcode == VIR_OP_PTRTOINT)
        return operand == VIR_TYPE_PTR &&
               vir_integer_type_width(result) == func->pointer_bits;
    if (opcode == VIR_OP_INTTOPTR)
        return result == VIR_TYPE_PTR &&
               vir_integer_type_width(operand) == func->pointer_bits;
    return false;
}

static vir_value_t *vir_unary(vir_function_t *func,
                              vir_block_t *block,
                              vir_opcode_t opcode,
                              vir_type_t type,
                              vir_value_t *operand)
{
    if (!vir_has_block(func, block) || !vir_has_value(func, operand) ||
        !vir_is_pure_unary_opcode(opcode) ||
        !vir_unary_types_valid(func, opcode, type, operand->type))
        return NULL;
    return vir_user_operation(func, block, opcode, type, operand, NULL);
}

static bool vir_is_constant(const vir_value_t *value)
{
    return value && !value->is_block_param && value->nr_ops == 0 &&
           value->opcode == VIR_OP_CONST;
}

static bool vir_is_const_int(const vir_value_t *value,
                             unsigned long long constant)
{
    return vir_is_constant(value) && vir_type_is_i32_or_i64(value->type) &&
           value->constant == (constant & vir_integer_mask(value->type));
}

static bool vir_binary_operands_are_local(const vir_function_t *func,
                                          const vir_block_t *block,
                                          const vir_value_t *op0,
                                          const vir_value_t *op1)
{
    return block && op0 && op1 && vir_has_block(func, block) &&
           vir_has_value(func, op0) && vir_has_value(func, op1);
}

static bool vir_integer_operands_are_local(const vir_function_t *func,
                                           const vir_block_t *block,
                                           const vir_value_t *op0,
                                           const vir_value_t *op1)
{
    return vir_binary_operands_are_local(func, block, op0, op1) &&
           op0->type == op1->type && vir_type_is_i32_or_i64(op0->type);
}

static void vir_order_commutative(vir_value_t **op0, vir_value_t **op1)
{
    if ((*op1)->id < (*op0)->id) {
        vir_value_t *tmp = *op0;
        *op0 = *op1;
        *op1 = tmp;
    }
}

#define DEFINE_BINARY_WRAPPER(name, helper, opcode)             \
    vir_value_t *name(vir_function_t *func, vir_block_t *block, \
                      vir_value_t *op0, vir_value_t *op1)       \
    {                                                           \
        return helper(func, block, op0, op1, opcode);           \
    }

static vir_value_t *vir_integer_binary(vir_function_t *func,
                                       vir_block_t *block,
                                       vir_value_t *op0,
                                       vir_value_t *op1,
                                       vir_opcode_t opcode)
{
    unsigned long long result;
    bool division = vir_opcode_is_division(opcode);
    bool shift = vir_opcode_is_shift(opcode);

    if (!(shift ? vir_binary_operands_are_local(func, block, op0, op1) &&
                      (vir_type_is_i32_or_i64(op0->type) &&
                       op1->type == VIR_TYPE_I32)
                : vir_integer_operands_are_local(func, block, op0, op1)))
        return NULL;
    if (shift && vir_is_const_int(op1, 0))
        return op0;
    if (opcode == VIR_OP_SUB) {
        if (op0 == op1)
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op1, 0))
            return op0;
    } else if (opcode == VIR_OP_MUL) {
        if (vir_is_const_int(op0, 0) || vir_is_const_int(op1, 0))
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op0, 1))
            return op1;
        if (vir_is_const_int(op1, 1))
            return op0;
    } else if (opcode == VIR_OP_BITAND) {
        if (vir_is_const_int(op0, 0) || vir_is_const_int(op1, 0))
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op0, vir_integer_mask(op0->type)))
            return op1;
        if (vir_is_const_int(op1, vir_integer_mask(op1->type)))
            return op0;
    } else if (!division && !shift) {
        if (vir_is_const_int(op0, 0))
            return op1;
        if (vir_is_const_int(op1, 0))
            return op0;
    }
    if (vir_opcode_is_commutative(opcode))
        vir_order_commutative(&op0, &op1);
    if (vir_is_constant(op0) && vir_is_constant(op1)) {
        if (!vir_fold_binary(func, opcode, op0->type, op0->type, op0->constant,
                             op1->constant, &result))
            return division || shift ? vir_user_operation(func, block, opcode,
                                                          op0->type, op0, op1)
                                     : NULL;
        return vir_const_int(func, block, op0->type, result);
    }
    return vir_user_operation(func, block, opcode, op0->type, op0, op1);
}

DEFINE_BINARY_WRAPPER(vir_add, vir_integer_binary, VIR_OP_ADD)
DEFINE_BINARY_WRAPPER(vir_sub, vir_integer_binary, VIR_OP_SUB)
DEFINE_BINARY_WRAPPER(vir_mul, vir_integer_binary, VIR_OP_MUL)

vir_value_t *vir_div(vir_function_t *func,
                     vir_block_t *block,
                     vir_value_t *op0,
                     vir_value_t *op1,
                     bool is_unsigned,
                     bool remainder)
{
    vir_opcode_t opcode;

    opcode = remainder ? (is_unsigned ? VIR_OP_UREM : VIR_OP_SREM)
                       : (is_unsigned ? VIR_OP_UDIV : VIR_OP_SDIV);
    return vir_integer_binary(func, block, op0, op1, opcode);
}

DEFINE_BINARY_WRAPPER(vir_bitand, vir_integer_binary, VIR_OP_BITAND)
DEFINE_BINARY_WRAPPER(vir_bitor, vir_integer_binary, VIR_OP_BITOR)
DEFINE_BINARY_WRAPPER(vir_bitxor, vir_integer_binary, VIR_OP_BITXOR)
DEFINE_BINARY_WRAPPER(vir_shl, vir_integer_binary, VIR_OP_SHL)
DEFINE_BINARY_WRAPPER(vir_ashr, vir_integer_binary, VIR_OP_ASHR)
DEFINE_BINARY_WRAPPER(vir_lshr, vir_integer_binary, VIR_OP_LSHR)

static vir_value_t *vir_integer_unary(vir_function_t *func,
                                      vir_block_t *block,
                                      vir_value_t *operand,
                                      vir_type_t type,
                                      vir_opcode_t opcode)
{
    bool cast = opcode == VIR_OP_ZEXT || opcode == VIR_OP_SEXT ||
                opcode == VIR_OP_TRUNC;

    if (!operand || !vir_has_block(func, block) ||
        !vir_has_value(func, operand) ||
        (cast ? !vir_integer_cast_valid(opcode, type, operand->type)
              : (type != operand->type || !vir_type_is_i32_or_i64(type))))
        return NULL;
    if (!vir_is_constant(operand))
        return vir_unary(func, block, opcode, type, operand);
    if (cast)
        return vir_const_int(
            func, block, type,
            vir_fold_integer_cast(opcode, vir_integer_type_width(operand->type),
                                  vir_integer_type_width(type),
                                  operand->constant));
    return vir_const_int(func, block, type,
                         vir_fold_unary(opcode, vir_integer_type_width(type),
                                        operand->constant));
}

#define DEFINE_INTEGER_UNARY(name, opcode)                                \
    vir_value_t *name(vir_function_t *func, vir_block_t *block,           \
                      vir_value_t *operand)                               \
    {                                                                     \
        return vir_integer_unary(func, block, operand,                    \
                                 operand ? operand->type : VIR_TYPE_VOID, \
                                 opcode);                                 \
    }

DEFINE_INTEGER_UNARY(vir_neg, VIR_OP_NEG)
DEFINE_INTEGER_UNARY(vir_bitnot, VIR_OP_BITNOT)

#undef DEFINE_INTEGER_UNARY

static vir_value_t *vir_compare(vir_function_t *func,
                                vir_block_t *block,
                                vir_value_t *op0,
                                vir_value_t *op1,
                                vir_opcode_t opcode)
{
    unsigned long long bits0;
    unsigned long long bits1;

    if (opcode == VIR_OP_EQ) {
        if (!vir_binary_operands_are_local(func, block, op0, op1) ||
            op0->type != op1->type || !vir_type_width(func, op0->type))
            return NULL;
        if (op0 == op1)
            return vir_const_i1(func, block, 1);
        if (vir_opcode_is_commutative(opcode))
            vir_order_commutative(&op0, &op1);
    } else if (!vir_integer_operands_are_local(func, block, op0, op1)) {
        return NULL;
    }
    if (vir_is_constant(op0) && vir_is_constant(op1)) {
        bits0 = op0->constant;
        bits1 = op1->constant;
        if (!vir_fold_binary(func, opcode, VIR_TYPE_I1, op0->type, bits0, bits1,
                             &bits0))
            return NULL;
        return vir_const_i1(func, block, bits0);
    }
    return vir_user_operation(func, block, opcode, VIR_TYPE_I1, op0, op1);
}

DEFINE_BINARY_WRAPPER(vir_eq, vir_compare, VIR_OP_EQ)
DEFINE_BINARY_WRAPPER(vir_slt, vir_compare, VIR_OP_SLT)
DEFINE_BINARY_WRAPPER(vir_ult, vir_compare, VIR_OP_ULT)

#undef DEFINE_BINARY_WRAPPER

#define DEFINE_FIXED_CAST(name, cast, type)                     \
    vir_value_t *name(vir_function_t *func, vir_block_t *block, \
                      vir_value_t *operand)                     \
    {                                                           \
        return cast(func, block, operand, type);                \
    }

DEFINE_FIXED_CAST(vir_zext_i1, vir_zext, VIR_TYPE_I32)
DEFINE_FIXED_CAST(vir_trunc_i1, vir_trunc, VIR_TYPE_I1)

#undef DEFINE_FIXED_CAST

#define DEFINE_INTEGER_CAST(name, opcode)                             \
    vir_value_t *name(vir_function_t *func, vir_block_t *block,       \
                      vir_value_t *operand, vir_type_t type)          \
    {                                                                 \
        return vir_integer_unary(func, block, operand, type, opcode); \
    }

DEFINE_INTEGER_CAST(vir_zext, VIR_OP_ZEXT)
DEFINE_INTEGER_CAST(vir_sext, VIR_OP_SEXT)
DEFINE_INTEGER_CAST(vir_trunc, VIR_OP_TRUNC)

#undef DEFINE_INTEGER_CAST

static vir_value_t *vir_pointer_cast(vir_function_t *func,
                                     vir_block_t *block,
                                     vir_value_t *operand,
                                     vir_type_t type,
                                     vir_opcode_t opcode)
{
    bool to_integer = opcode == VIR_OP_PTRTOINT;

    if (!func || !operand ||
        (to_integer ? operand->type != VIR_TYPE_PTR : type != VIR_TYPE_PTR) ||
        vir_integer_type_width(to_integer ? type : operand->type) !=
            func->pointer_bits)
        return NULL;
    if (vir_is_constant(operand))
        return to_integer ? vir_const_int(func, block, type, operand->constant)
                          : vir_const_ptr(func, block, operand->constant);
    return vir_unary(func, block, opcode, type, operand);
}

vir_value_t *vir_ptrtoint(vir_function_t *func,
                          vir_block_t *block,
                          vir_value_t *operand,
                          vir_type_t type)
{
    return vir_pointer_cast(func, block, operand, type, VIR_OP_PTRTOINT);
}

vir_value_t *vir_inttoptr(vir_function_t *func,
                          vir_block_t *block,
                          vir_value_t *operand)
{
    return vir_pointer_cast(func, block, operand, VIR_TYPE_PTR,
                            VIR_OP_INTTOPTR);
}

vir_value_t *vir_ptradd(vir_function_t *func,
                        vir_block_t *block,
                        vir_value_t *base,
                        vir_value_t *byte_offset)
{
    if (func && func->pointer_bits == 64 && byte_offset &&
        byte_offset->type == VIR_TYPE_I32)
        byte_offset = vir_sext(func, block, byte_offset, VIR_TYPE_I64);
    if (!base || !byte_offset || !vir_has_block(func, block) ||
        !vir_has_value(func, base) || !vir_has_value(func, byte_offset) ||
        base->type != VIR_TYPE_PTR ||
        !vir_integer_type_width(byte_offset->type) ||
        vir_integer_type_width(byte_offset->type) != func->pointer_bits)
        return NULL;
    if (vir_is_constant(byte_offset) && byte_offset->constant == 0)
        return base;
    if (vir_is_constant(base) && vir_is_constant(byte_offset))
        return vir_const_ptr(func, block,
                             base->constant + byte_offset->constant);
    return vir_user_operation(func, block, VIR_OP_PTRADD, VIR_TYPE_PTR, base,
                              byte_offset);
}

static void vir_remove_use(vir_value_t *operand, vir_use_t *needle)
{
    vir_remove_matching_use(operand, VIR_USE_EXACT, needle, -1);
}

static void vir_remove_matching_use(vir_value_t *value,
                                    vir_use_owner_t kind,
                                    const void *owner,
                                    int operand)
{
    vir_use_t **link = &value->uses;

    while (*link && !vir_use_matches(*link, kind, owner, operand))
        link = &(*link)->next;
    if (*link)
        *link = (*link)->next;
}

static void vir_remove_effect_uses(vir_effect_t *effect)
{
    if (effect->address_use)
        vir_remove_effect_use(effect->address, effect, 0);
    if (effect->stored_value_use)
        vir_remove_effect_use(effect->stored_value, effect, 1);
    if (effect->callee_value_use)
        vir_remove_effect_use(effect->callee_value, effect, -1);
    effect->address_use = effect->stored_value_use = effect->callee_value_use =
        NULL;
    for (int operand = 0; operand < effect->arg_count; operand++)
        vir_remove_effect_use(effect->args[operand], effect, operand);
}

bool vir_effect_remove(vir_function_t *func, vir_effect_t *effect)
{
    vir_effect_t **link;
    vir_effect_t *previous = NULL;

    if (!func || !effect ||
        (effect->kind != VIR_EFFECT_STORE &&
         (effect->kind != VIR_EFFECT_LOAD || !effect->result ||
          effect->result->uses)) ||
        !vir_has_block(func, effect->block))
        return false;
    link = &effect->block->effects;
    while (*link && *link != effect) {
        previous = *link;
        link = &(*link)->next;
    }
    if (!*link)
        return false;
    if (effect->result) {
        vir_value_t **value_link = &effect->block->head;
        while (*value_link && *value_link != effect->result)
            value_link = &(*value_link)->next;
        if (!*value_link)
            return false;
        *value_link = effect->result->next;
        effect->result->next = NULL;
        effect->result->def_effect = NULL;
    }
    vir_remove_effect_uses(effect);
    *link = effect->next;
    if (effect->block->last_effect == effect)
        effect->block->last_effect = previous;
    effect->next = NULL;
    vir_repair_value_positions(effect->block);
    return true;
}

static void vir_set_use_slot(vir_value_t **slot,
                             vir_value_t *value,
                             vir_use_t *use)
{
    vir_remove_use(*slot, use);
    *slot = value;
    vir_link_use(value, use);
}

static vir_value_t **vir_use_value_slot(vir_use_t *use)
{
    if (use->user)
        return use->operand == 0 ? &use->user->op0 : &use->user->op1;
    if (use->edge)
        return &use->edge->args[use->operand];
    if (use->effect) {
        if (use->effect->kind == VIR_EFFECT_CALL)
            return use->operand < 0 ? &use->effect->callee_value
                                    : &use->effect->args[use->operand];
        return use->operand == 0 ? &use->effect->address
                                 : &use->effect->stored_value;
    }
    return use->return_block ? &use->return_block->return_value
                             : &use->branch_block->branch_condition;
}

vir_block_t *vir_use_block(const vir_use_t *use)
{
    return use->user           ? use->user->block
           : use->edge         ? use->edge->from
           : use->effect       ? use->effect->block
           : use->return_block ? use->return_block
                               : use->branch_block;
}

int vir_use_order(const vir_use_t *use)
{
    return use->user     ? use->user->order
           : use->effect ? use->effect->order
                         : vir_use_block(use)->next_order;
}

static bool vir_replacement_dominates_with_context(
    const vir_function_t *func,
    const vir_dominance_t *dominance,
    const vir_value_t *from,
    const vir_value_t *to)
{
    const vir_use_t *use;
    for (use = from->uses; use; use = use->next) {
        const vir_block_t *block;
        int position;
        if (!use->user && !use->edge && use->effect) {
            if (!vir_value_dominates_effect(func, dominance, to, use->effect))
                return 0;
            continue;
        }
        if (!(block = vir_use_block(use)))
            return 0;
        position = use->user     ? use->user->position
                   : block->tail ? block->tail->position + 1
                                 : 0;
        if (!vir_value_dominates(func, dominance, to, block, position))
            return 0;
    }
    return 1;
}

static bool vir_replacement_dominates(const vir_function_t *func,
                                      const vir_value_t *from,
                                      const vir_value_t *to)
{
    return vir_replacement_dominates_with_context(func, NULL, from, to);
}

static void vir_replace_all_uses_unchecked(vir_value_t *from, vir_value_t *to)
{
    while (from->uses) {
        vir_use_t *use = from->uses;
        vir_set_use_slot(vir_use_value_slot(use), to, use);
    }
}

int vir_replace_all_uses(vir_function_t *func,
                         vir_value_t *from,
                         vir_value_t *to)
{
    if (!func || !from || !to || !vir_has_value(func, from) ||
        !vir_has_value(func, to) || from->type != to->type)
        return 0;
    if (from == to)
        return 1;
    if (!vir_replacement_dominates(func, from, to))
        return 0;
    vir_replace_all_uses_unchecked(from, to);
    return 1;
}

static unsigned int vir_cse_hash(const vir_value_t *value)
{
    unsigned int hash = (unsigned int) value->opcode;
    hash = hash * 33u + (unsigned int) value->type;
    hash = hash * 33u + (unsigned int) value->op0->id;
    return hash * 33u + (unsigned int) value->op1->id;
}

static unsigned int vir_cse_capacity(const vir_function_t *func,
                                     const vir_block_t *only_block)
{
    int values = 0;
    unsigned int capacity = 1;
    const vir_block_t *block = only_block ? only_block : func->blocks;
    const vir_block_t *stop = only_block ? only_block->next : NULL;

    for (; block != stop; block = block->next)
        for (const vir_value_t *value = block->head; value; value = value->next)
            if (value->nr_ops == 2 && ++values > INT_MAX / 2)
                return 0;
    while (capacity < (unsigned int) values * 2u) {
        if (capacity > (unsigned int) INT_MAX / 2u)
            return 0;
        capacity <<= 1;
    }
    return capacity;
}

/* Look up each binary value of @block in the open-addressed @table and fold it
 * into an equal leader. Local CSE clears and reuses the table per block; GVN
 * keeps it across blocks and relies on vir_replace_all_uses() to refuse a
 * leader that does not dominate.
 */
static int vir_cse_scan_block(vir_function_t *func,
                              vir_block_t *block,
                              vir_value_t **table,
                              unsigned int capacity,
                              vir_cse_stats_t *stats)
{
    int replacements = 0;

    for (vir_value_t *value = block->head; value; value = value->next) {
        vir_value_t *leader;
        unsigned int slot;

        if (value->nr_ops != 2)
            continue;
        if (stats) {
            stats->values++;
            stats->lookups++;
        }
        slot = vir_cse_hash(value) & (capacity - 1);
        while ((leader = table[slot])) {
            if (stats)
                stats->probes++;
            if (leader->opcode == value->opcode &&
                leader->type == value->type && leader->op0 == value->op0 &&
                leader->op1 == value->op1)
                break;
            slot = (slot + 1) & (capacity - 1);
        }
        if (!leader) {
            table[slot] = value;
            continue;
        }
        if (stats)
            stats->hits++;
        if (value->uses && vir_replace_all_uses(func, value, leader)) {
            replacements++;
            if (stats)
                stats->replacements++;
        }
    }
    return replacements;
}

static int vir_cse_run(vir_function_t *func,
                       vir_opt_level_t opt_level,
                       vir_cse_stats_t *stats,
                       bool global)
{
    vir_value_t **table = NULL;
    int replacements = 0;
    unsigned int capacity = 0;

    if (stats)
        memset(stats, 0, sizeof(*stats));
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (global ? opt_level != VIR_OPT_O2 : opt_level == VIR_OPT_O0)
        return 0;
    if (global)
        capacity = vir_cse_capacity(func, NULL);
    else
        for (vir_block_t *block = func->blocks; block; block = block->next) {
            unsigned int block_capacity = vir_cse_capacity(func, block);

            if (block_capacity > capacity)
                capacity = block_capacity;
        }
    if (!capacity || !(table = calloc(capacity, sizeof(*table))))
        return 0;
    if (global && stats) {
        stats->tables++;
        stats->table_slots += (int) capacity;
    }
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        unsigned int block_capacity =
            global ? capacity : vir_cse_capacity(func, block);

        if (!block_capacity)
            continue;
        if (!global) {
            memset(table, 0, (size_t) block_capacity * sizeof(*table));
            if (stats) {
                stats->tables++;
                stats->table_slots += (int) block_capacity;
            }
        }
        replacements +=
            vir_cse_scan_block(func, block, table, block_capacity, stats);
    }
    free(table);
    return replacements;
}

int vir_local_cse_with_stats(vir_function_t *func,
                             vir_opt_level_t opt_level,
                             vir_cse_stats_t *stats)
{
    return vir_cse_run(func, opt_level, stats, false);
}

int vir_local_cse(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_local_cse_with_stats(func, opt_level, NULL);
}

int vir_gvn_with_stats(vir_function_t *func,
                       vir_opt_level_t opt_level,
                       vir_cse_stats_t *stats)
{
    return vir_cse_run(func, opt_level, stats, true);
}

static bool vir_value_is_dead(const vir_value_t *value)
{
    const vir_effect_t *effect = value->def_effect;

    /* Only unused pure values and non-volatile loads may be discarded. */
    return !value->uses && ((!value->is_block_param && !effect) ||
                            (effect && effect->kind == VIR_EFFECT_LOAD &&
                             effect->result == value));
}

static void vir_repair_value_positions(vir_block_t *block)
{
    vir_effect_t *effect;
    vir_value_t *value;
    int position = 0;
    int order = 0;

    block->tail = NULL;
    for (value = block->head; value; value = value->next) {
        value->position = position++;
        block->tail = value;
    }
    block->last_effect = NULL;
    for (position = 0, effect = block->effects; effect;
         position++, effect = effect->next) {
        effect->position = position;
        block->last_effect = effect;
    }
    effect = block->effects;
    value = block->head;
    while (value || effect) {
        if (!effect || (value && value->order < effect->order)) {
            value->order = order++;
            value = value->next;
        } else if (!value || effect->order < value->order) {
            effect->order = order++;
            effect = effect->next;
        } else {
            value->order = order;
            effect->order = order++;
            value = value->next;
            effect = effect->next;
        }
    }
    block->next_order = order;
}

/* Promote a nonescaping stack scalar used only by direct, nonvolatile memory
 * effects in one block.
 */
static bool vir_promote_private_stack(vir_function_t *func,
                                      vir_block_t *block,
                                      vir_value_t *address)
{
    vir_value_t *stored = NULL;
    vir_use_t *use;
    vir_effect_t *effect;
    vir_type_t access_type = VIR_TYPE_VOID;

    if (address->opcode != VIR_OP_STACK_ADDR || address->block != block ||
        address->address_kind != VIR_ADDRESS_STACK || !address->uses)
        return false;
    for (vir_block_t *owner = func->blocks; owner; owner = owner->next)
        for (vir_value_t *value = owner->head; value; value = value->next)
            if (value != address && value->opcode == VIR_OP_STACK_ADDR &&
                value->address_slot == address->address_slot)
                return false;
    for (use = address->uses; use; use = use->next) {
        effect = use->effect;
        if (use->user || use->edge || use->return_block || use->branch_block ||
            use->operand != 0 || !effect || effect->block != block ||
            effect->address != address ||
            (effect->kind != VIR_EFFECT_LOAD &&
             effect->kind != VIR_EFFECT_STORE))
            return false;
    }
    for (effect = block->effects; effect; effect = effect->next) {
        vir_value_t *value;

        if (effect->address != address)
            continue;
        if (effect->kind == VIR_EFFECT_STORE) {
            stored = effect->stored_value;
            value = stored;
        } else if (effect->kind == VIR_EFFECT_LOAD) {
            if (!stored)
                return false;
            value = effect->result;
        } else {
            return false;
        }
        vir_type_t type;

        if (!value)
            return false;
        type = value->type;
        if (!vir_type_width(func, type) ||
            (access_type != VIR_TYPE_VOID && access_type != type))
            return false;
        access_type = type;
    }
    if (!stored)
        return false;
    stored = NULL;
    for (effect = block->effects; effect; effect = effect->next) {
        if (effect->address != address)
            continue;
        if (effect->kind == VIR_EFFECT_STORE) {
            stored = effect->stored_value;
        } else if (!vir_replace_all_uses(func, effect->result, stored)) {
            return false;
        }
    }
    for (vir_effect_t **link = &block->effects; *link;) {
        effect = *link;
        if (effect->address == address && effect->kind == VIR_EFFECT_STORE) {
            vir_remove_effect_uses(effect);
            *link = effect->next;
        } else {
            link = &effect->next;
        }
    }
    vir_repair_value_positions(block);
    return true;
}

static void vir_promote_private_stack_values(vir_function_t *func)
{
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        for (vir_value_t *value = block->head; value; value = value->next)
            if (value->opcode == VIR_OP_STACK_ADDR)
                vir_promote_private_stack(func, block, value);
    }
}

/* An unused ordinary read cannot observe a store. Every other effect remains a
 * barrier unless it overwrites exactly the same typed location.
 */
static void vir_drop_overwritten_stores(vir_block_t *block)
{
    for (vir_effect_t **link = &block->effects; *link;) {
        vir_effect_t *effect = *link;
        if (effect->kind != VIR_EFFECT_STORE) {
            link = &effect->next;
            continue;
        }
        vir_effect_t *next = effect->next;
        while (next && next->kind == VIR_EFFECT_LOAD && next->result &&
               !next->result->uses)
            next = next->next;
        if (next && next->kind == VIR_EFFECT_STORE &&
            effect->address == next->address &&
            effect->stored_value->type == next->stored_value->type) {
            vir_remove_effect_uses(effect);
            *link = effect->next;
        } else {
            link = &effect->next;
        }
    }
    vir_repair_value_positions(block);
}

int vir_dce_with_stats(vir_function_t *func,
                       vir_opt_level_t opt_level,
                       int *passes,
                       int *values_scanned,
                       int *values_removed,
                       int *temporary_bytes,
                       int *temporary_peak_bytes)
{
    unsigned char *dead;
    vir_value_t **work;
    int capacity;
    int head = 0;
    int tail = 0;
    int removed = 0;
    int result = -1;

    if (passes)
        *passes = 0;
    if (values_scanned)
        *values_scanned = 0;
    if (values_removed)
        *values_removed = 0;
    if (temporary_bytes)
        *temporary_bytes = 0;
    if (temporary_peak_bytes)
        *temporary_peak_bytes = 0;
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    for (vir_block_t *block = func->blocks; block; block = block->next)
        vir_drop_overwritten_stores(block);
    capacity = func->next_value_id;
    if (capacity <= 0)
        return 0;
    if (capacity > INT_MAX / (int) (sizeof(*dead) + sizeof(*work)))
        return -1;
    dead = calloc((size_t) capacity, sizeof(*dead));
    work = malloc((size_t) capacity * sizeof(*work));
    if (!dead || !work)
        goto cleanup;
    if (temporary_bytes)
        *temporary_bytes = capacity * (int) (sizeof(*dead) + sizeof(*work));
    if (temporary_peak_bytes)
        *temporary_peak_bytes =
            capacity * (int) (sizeof(*dead) + sizeof(*work));
    if (passes)
        (*passes)++;
    for (vir_block_t *block = func->blocks; block; block = block->next)
        for (vir_value_t *value = block->head; value; value = value->next) {
            if (values_scanned)
                (*values_scanned)++;
            if (vir_value_is_dead(value)) {
                dead[value->id] = 1;
                work[tail++] = value;
            }
        }
    while (head < tail) {
        vir_value_t *value = work[head++];

        if (value->def_effect) {
            vir_effect_t *load = value->def_effect;
            vir_value_t *input = load->address;

            vir_remove_effect_uses(load);
            if (vir_value_is_dead(input) && !dead[input->id]) {
                dead[input->id] = 1;
                work[tail++] = input;
            }
            continue;
        }
        for (int operand = 0; operand < value->nr_ops; operand++) {
            vir_value_t *input = operand ? value->op1 : value->op0;

            vir_remove_user_use(input, value, operand);
            if (vir_value_is_dead(input) && !dead[input->id]) {
                dead[input->id] = 1;
                work[tail++] = input;
            }
        }
    }
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        vir_value_t **link = &block->head;
        vir_effect_t **effect_link = &block->effects;

        while (*link) {
            vir_value_t *value = *link;

            if (dead[value->id]) {
                *link = value->next;
                removed++;
            } else {
                link = &value->next;
            }
        }
        while (*effect_link) {
            vir_effect_t *effect = *effect_link;

            if (effect->kind == VIR_EFFECT_LOAD && effect->result &&
                dead[effect->result->id]) {
                *effect_link = effect->next;
            } else {
                effect_link = &effect->next;
            }
        }
        vir_repair_value_positions(block);
    }
    result = removed;
cleanup:
    free(dead);
    free(work);
    if (result >= 0 && values_removed)
        *values_removed = removed;
    return result;
}

int vir_dce(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_dce_with_stats(func, opt_level, NULL, NULL, NULL, NULL, NULL);
}

enum {
    VIR_SCCP_UNDEF,
    VIR_SCCP_CONST,
    VIR_SCCP_OVERDEF,
};

static bool vir_sccp_set(unsigned char *state,
                         unsigned long long *constant,
                         int id,
                         int next_state,
                         unsigned long long next_constant)
{
    if (state[id] == VIR_SCCP_OVERDEF)
        return false;
    if (next_state == VIR_SCCP_OVERDEF) {
        state[id] = VIR_SCCP_OVERDEF;
        return true;
    }
    if (state[id] == VIR_SCCP_UNDEF) {
        state[id] = VIR_SCCP_CONST;
        constant[id] = next_constant;
        return true;
    }
    if (constant[id] == next_constant)
        return false;
    state[id] = VIR_SCCP_OVERDEF;
    return true;
}

static bool vir_sccp_fold(const vir_function_t *func,
                          const vir_value_t *value,
                          unsigned long long left,
                          unsigned long long right,
                          unsigned long long *result)
{
    if (value->nr_ops == 1) {
        if (vir_is_pure_unary_opcode(value->opcode))
            *result = vir_fold_unary(
                value->opcode, vir_type_width(func, value->op0->type), left);
        else
            return false;
        *result &= vir_width_mask(vir_type_width(func, value->type));
        return true;
    }
    return vir_fold_binary(func, value->opcode, value->type, value->op0->type,
                           left, right, result);
}

static bool vir_sccp_value(const vir_function_t *func,
                           const vir_value_t *value,
                           unsigned char *state,
                           unsigned long long *constant)
{
    unsigned long long result;

    if (value->is_block_param)
        return false;
    if (vir_is_constant(value))
        return vir_sccp_set(state, constant, value->id, VIR_SCCP_CONST,
                            value->constant);
    if (value->def_effect || !value->nr_ops)
        return vir_sccp_set(state, constant, value->id, VIR_SCCP_OVERDEF, 0);
    if (state[value->op0->id] == VIR_SCCP_OVERDEF ||
        (value->nr_ops == 2 && state[value->op1->id] == VIR_SCCP_OVERDEF))
        return vir_sccp_set(state, constant, value->id, VIR_SCCP_OVERDEF, 0);
    if (state[value->op0->id] != VIR_SCCP_CONST ||
        (value->nr_ops == 2 && state[value->op1->id] != VIR_SCCP_CONST))
        return false;
    if (!vir_sccp_fold(func, value, constant[value->op0->id],
                       value->nr_ops == 2 ? constant[value->op1->id] : 0,
                       &result))
        return vir_sccp_set(state, constant, value->id, VIR_SCCP_OVERDEF, 0);
    return vir_sccp_set(state, constant, value->id, VIR_SCCP_CONST, result);
}

static int vir_sccp_edge_index(const vir_function_t *func,
                               const vir_edge_t *needle)
{
    int index = 0;

    for (const vir_block_t *block = func->blocks; block; block = block->next)
        for (const vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing, index++)
            if (edge == needle)
                return index;
    return -1;
}

typedef struct {
    unsigned char *queued_blocks;
    int *queue;
    int head;
    int tail;
    int count;
    int capacity;
} vir_sccp_worklist_t;

/* Entry/edge activation can enqueue inactive blocks; uses only requeue active
 * ones.
 */
static bool vir_sccp_queue_block(vir_sccp_worklist_t *worklist,
                                 const vir_function_t *func,
                                 const vir_block_t *block,
                                 bool require_executable)
{
    int index = vir_block_index(func, block);

    if (index < 0 || worklist->queued_blocks[index] == 2 ||
        (require_executable && worklist->queued_blocks[index] != 1) ||
        worklist->count >= worklist->capacity)
        return false;
    worklist->queued_blocks[index] = 2;
    worklist->queue[worklist->tail] = index;
    worklist->tail = (worklist->tail + 1) % worklist->capacity;
    worklist->count++;
    return true;
}

static void vir_sccp_schedule_uses(vir_sccp_worklist_t *worklist,
                                   const vir_function_t *func,
                                   const vir_value_t *value,
                                   const vir_block_t *current_block,
                                   const unsigned char *executable_edges)
{
    for (const vir_use_t *use = value->uses; use; use = use->next) {
        if (use->user && use->user->block != current_block)
            vir_sccp_queue_block(worklist, func, use->user->block, true);
        else if (use->branch_block && use->branch_block != current_block)
            vir_sccp_queue_block(worklist, func, use->branch_block, true);
        else if (use->edge) {
            int index = vir_sccp_edge_index(func, use->edge);

            if (index >= 0 && executable_edges[index])
                vir_sccp_queue_block(worklist, func, use->edge->to, true);
        }
    }
}

static bool vir_sccp_param(const vir_value_t *param,
                           int index,
                           const unsigned char *executable_edges,
                           const vir_function_t *func,
                           unsigned char *state,
                           unsigned long long *constant)
{
    const vir_edge_t *edge;
    int seen = 0;
    int next_state = VIR_SCCP_UNDEF;
    unsigned long long next_constant = 0;

    for (edge = param->block->incoming; edge; edge = edge->next_incoming) {
        int edge_index = vir_sccp_edge_index(func, edge);

        if (edge_index < 0)
            return false;
        if (!executable_edges[edge_index])
            continue;
        seen = 1;
        if (state[edge->args[index]->id] == VIR_SCCP_OVERDEF) {
            next_state = VIR_SCCP_OVERDEF;
            break;
        }
        if (state[edge->args[index]->id] != VIR_SCCP_CONST)
            continue;
        if (next_state == VIR_SCCP_UNDEF) {
            next_state = VIR_SCCP_CONST;
            next_constant = constant[edge->args[index]->id];
        } else if (next_constant != constant[edge->args[index]->id]) {
            next_state = VIR_SCCP_OVERDEF;
            break;
        }
    }

    /* Entry parameters have no incoming edge and therefore model unknown
     * function inputs. Treating them as undefined can make a loop header look
     * constant before its backedge becomes executable.
     */
    if (!seen)
        return vir_sccp_set(state, constant, param->id, VIR_SCCP_OVERDEF, 0);
    if (next_state == VIR_SCCP_UNDEF)
        return false;
    return vir_sccp_set(state, constant, param->id, next_state, next_constant);
}

static void vir_sccp_mark_edge(vir_sccp_worklist_t *worklist,
                               const vir_function_t *func,
                               const vir_edge_t *edge,
                               unsigned char *executable_edges)
{
    int edge_index = vir_sccp_edge_index(func, edge);

    if (edge_index < 0)
        return;
    if (!executable_edges[edge_index]) {
        executable_edges[edge_index] = 1;
        vir_sccp_queue_block(worklist, func, edge->to, false);
    }
}

static vir_value_t *vir_sccp_constant_before(vir_function_t *func,
                                             vir_value_t *value,
                                             unsigned long long constant)
{
    vir_block_t *block = value->block;
    vir_value_t *replacement =
        vir_const_int(func, block, value->type, constant);

    if (!replacement)
        return NULL;
    if (block->head != replacement) {
        vir_value_t *previous = block->head;

        while (previous->next != replacement)
            previous = previous->next;
        previous->next = NULL;
        block->tail = previous;
        replacement->next = block->head;
        block->head = replacement;
    }

    /* Insert before an effect-first block as well as before pure values; equal
     * orders are reserved for an effect and its own result value.
     */
    replacement->order = -1;
    vir_repair_value_positions(block);
    return replacement;
}

static bool vir_sccp_replace_constant(vir_function_t *func,
                                      vir_value_t *value,
                                      const unsigned char *state,
                                      const unsigned long long *constant,
                                      int *values_replaced)
{
    vir_value_t *replacement;

    if (state[value->id] != VIR_SCCP_CONST || !value->uses ||
        !(replacement =
              vir_sccp_constant_before(func, value, constant[value->id])) ||
        !vir_replace_all_uses(func, value, replacement))
        return false;
    if (values_replaced)
        (*values_replaced)++;
    return true;
}

static void vir_sccp_release(unsigned char *state,
                             unsigned long long *constant,
                             unsigned char *blocks,
                             unsigned char *edges,
                             int *work)
{
    free(state);
    free(constant);
    free(blocks);
    free(edges);
    free(work);
}

int vir_sccp_with_stats_and_pruning(vir_function_t *func,
                                    vir_opt_level_t opt_level,
                                    int *stats,
                                    bool prune_unreachable)
{
    unsigned char *state;
    unsigned char *executable_blocks;
    unsigned char *executable_edges;
    unsigned long long *constant;
    int *work;
    vir_sccp_worklist_t worklist;
    int capacity;
    int block_count = 0;
    int edge_count = 0;
    int edge_marker_count;
    int temporary;
    int changes = 0;
    int result = -1;
    int simplified = 0;
    int *passes = stats ? &stats[VIR_SCCP_STAT_PASSES] : NULL;
    int *values_scanned = stats ? &stats[VIR_SCCP_STAT_VALUES_SCANNED] : NULL;
    int *values_replaced = stats ? &stats[VIR_SCCP_STAT_VALUES_REPLACED] : NULL;
    int *branches_simplified =
        stats ? &stats[VIR_SCCP_STAT_BRANCHES_SIMPLIFIED] : NULL;
    int *blocks_removed = stats ? &stats[VIR_SCCP_STAT_BLOCKS_REMOVED] : NULL;
    int *temporary_bytes = stats ? &stats[VIR_SCCP_STAT_TEMPORARY_BYTES] : NULL;
    int *temporary_peak_bytes =
        stats ? &stats[VIR_SCCP_STAT_TEMPORARY_PEAK_BYTES] : NULL;

    if (stats)
        memset(stats, 0, VIR_SCCP_STAT_COUNT * sizeof(*stats));
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;

    /* Without values there is no lattice to solve and no branch to fold; an
     * empty void function is the usual case.
     */
    if (opt_level == VIR_OPT_O0 || !func->blocks || !func->next_value_id)
        return 0;
    vir_promote_private_stack_values(func);
    capacity = func->next_value_id;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        block_count++;
        for (vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing)
            edge_count++;
    }
    if (capacity <= 0 ||
        capacity > INT_MAX / (int) (sizeof(*state) + sizeof(*constant)))
        return -1;
    temporary = capacity * (int) (sizeof(*state) + sizeof(*constant));
    if (block_count > (INT_MAX - temporary) /
                          (int) (sizeof(*executable_blocks) + sizeof(*work)))
        return -1;
    temporary +=
        block_count * (int) (sizeof(*executable_blocks) + sizeof(*work));
    edge_marker_count = edge_count ? edge_count : 1;
    if (edge_marker_count >
        (INT_MAX - temporary) / (int) sizeof(*executable_edges))
        return -1;
    temporary += edge_marker_count * (int) sizeof(*executable_edges);
    state = calloc((size_t) capacity, sizeof(*state));
    constant = calloc((size_t) capacity, sizeof(*constant));
    executable_blocks =
        calloc((size_t) block_count, sizeof(*executable_blocks));
    executable_edges =
        calloc((size_t) edge_marker_count, sizeof(*executable_edges));
    work = malloc(sizeof(*work) * (size_t) block_count);
    if (!state || !constant || !executable_blocks || !executable_edges || !work)
        goto cleanup;
    worklist.queued_blocks = executable_blocks;
    worklist.queue = work;
    worklist.head = 0;
    worklist.tail = 0;
    worklist.count = 0;
    worklist.capacity = block_count;
    if (!vir_sccp_queue_block(&worklist, func, func->blocks, false))
        goto fail;
    while (worklist.count) {
        int block_index = worklist.queue[worklist.head];
        vir_block_t *block = func->blocks;

        worklist.head = (worklist.head + 1) % worklist.capacity;
        worklist.count--;
        for (int i = 0; i < block_index; i++)
            block = block->next;
        if (!block || worklist.queued_blocks[block_index] != 2)
            goto fail;
        worklist.queued_blocks[block_index] = 1;
        if (passes)
            (*passes)++;
        int param_index = 0;

        for (vir_value_t *value = block->params; value;
             value = value->param_next, param_index++) {
            if (vir_sccp_param(value, param_index, executable_edges, func,
                               state, constant))
                vir_sccp_schedule_uses(&worklist, func, value, block,
                                       executable_edges);
        }
        for (vir_value_t *value = block->head; value; value = value->next) {
            if (values_scanned)
                (*values_scanned)++;
            if (vir_sccp_value(func, value, state, constant))
                vir_sccp_schedule_uses(&worklist, func, value, block,
                                       executable_edges);
        }
        if (block->terminator == VIR_TERM_JUMP)
            vir_sccp_mark_edge(&worklist, func, block->outgoing,
                               executable_edges);
        else if (block->terminator == VIR_TERM_BRANCH) {
            int condition_state = state[block->branch_condition->id];

            if (condition_state == VIR_SCCP_CONST)
                vir_sccp_mark_edge(&worklist, func,
                                   constant[block->branch_condition->id]
                                       ? block->true_edge
                                       : block->false_edge,
                                   executable_edges);
            else if (condition_state == VIR_SCCP_OVERDEF) {
                vir_sccp_mark_edge(&worklist, func, block->true_edge,
                                   executable_edges);
                vir_sccp_mark_edge(&worklist, func, block->false_edge,
                                   executable_edges);
            }
        }
    }

    /* Rewrite conditional edges before replacing the condition value: the
     * replacement receives a fresh ID outside the temporary lattice.
     */
    for (vir_block_t *block = func->blocks; block; block = block->next)
        if (block->terminator == VIR_TERM_BRANCH &&
            state[block->branch_condition->id] == VIR_SCCP_CONST) {
            vir_edge_t *dead = constant[block->branch_condition->id]
                                   ? block->false_edge
                                   : block->true_edge;
            if (!vir_edge_delete(func, dead))
                goto fail;
            changes++;
            simplified++;
            if (branches_simplified)
                (*branches_simplified)++;
        }
    for (vir_block_t *block = func->blocks; block; block = block->next)
        for (vir_value_t *value = block->params; value;
             value = value->param_next)
            changes += vir_sccp_replace_constant(func, value, state, constant,
                                                 values_replaced);
    for (vir_block_t *block = func->blocks; block; block = block->next)
        for (vir_value_t *value = block->head; value; value = value->next)
            if (!vir_is_constant(value))
                changes += vir_sccp_replace_constant(func, value, state,
                                                     constant, values_replaced);
    if (simplified && prune_unreachable) {
        int before = block_count;

        if (!vir_remove_unreachable(func))
            goto fail;
        int after = vir_function_block_count(func);
        if (blocks_removed)
            *blocks_removed = before - after;
        changes += before - after;
    }
    result = changes;
    if (temporary_bytes)
        *temporary_bytes = temporary;
    if (temporary_peak_bytes)
        *temporary_peak_bytes = temporary;
    goto cleanup;

fail:
    result = -1;
cleanup:
    vir_sccp_release(state, constant, executable_blocks, executable_edges,
                     work);
    return result;
}

int vir_sccp(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_sccp_with_stats(func, opt_level, NULL);
}

int vir_sccp_with_stats(vir_function_t *func,
                        vir_opt_level_t opt_level,
                        int *stats)
{
    return vir_sccp_with_stats_and_pruning(func, opt_level, stats, true);
}

typedef struct {
    vir_edge_t *edge;
    vir_value_t **args;
    vir_use_t *uses;
    vir_value_t *inserted;
} vir_edge_resize_t;

typedef struct {
    vir_edge_resize_t *edges;
    int count;
    int old_count;
    int index;
    bool insert;
} vir_param_edit_t;

static void vir_param_edit_release(vir_param_edit_t *edit)
{
    free(edit->edges);
    memset(edit, 0, sizeof(*edit));
}

static bool vir_param_edit_prepare(vir_function_t *func,
                                   vir_block_t *block,
                                   vir_value_t *removed,
                                   int old_count,
                                   int index,
                                   bool insert,
                                   vir_param_edit_t *edit)
{
    vir_edge_t *edge;
    int new_count;
    int i;

    memset(edit, 0, sizeof(*edit));
    if (!vir_has_block(func, block) || old_count < 0 || index < 0 ||
        (insert ? index > old_count : index >= old_count) ||
        (!insert &&
         (!removed || removed->block != block || !removed->is_block_param)))
        return 0;
    if (!insert) {
        vir_value_t *param;
        for (param = block->params; param && param != removed;
             param = param->param_next)
            ;
        if (!param)
            return 0;
    }
    for (edge = block->incoming; edge; edge = edge->next_incoming)
        edit->count++;
    edit->edges =
        edit->count ? calloc((size_t) edit->count, sizeof(*edit->edges)) : NULL;
    if (edit->count && !edit->edges)
        return 0;
    edit->old_count = old_count;
    edit->index = index;
    edit->insert = insert;
    new_count = old_count + (insert ? 1 : -1);
    for (edge = block->incoming, i = 0; edge; edge = edge->next_incoming, i++) {
        edit->edges[i].edge = edge;
        if (!new_count)
            continue;
        edit->edges[i].args = vir_arena_alloc(
            &func->arena, sizeof(*edit->edges[i].args) * new_count);
        edit->edges[i].uses = vir_arena_alloc(
            &func->arena, sizeof(*edit->edges[i].uses) * new_count);
        if (!edit->edges[i].args || !edit->edges[i].uses)
            goto fail;
    }
    return 1;

fail:
    vir_param_edit_release(edit);
    return 0;
}

static void vir_param_edit_apply(vir_block_t *block,
                                 vir_value_t *param,
                                 vir_param_edit_t *edit)
{
    int new_count = edit->old_count + (edit->insert ? 1 : -1);
    int i;

    for (i = 0; i < edit->count; i++) {
        vir_edge_t *edge = edit->edges[i].edge;
        int source, destination = 0;

        for (source = 0; source < edge->arg_count; source++) {
            if (edit->insert && source == edit->index)
                edit->edges[i].args[destination++] = edit->edges[i].inserted;
            if (!edit->insert && source == edit->index)
                continue;
            edit->edges[i].args[destination++] = edge->args[source];
        }
        if (edit->insert && edit->index == edge->arg_count)
            edit->edges[i].args[destination++] = edit->edges[i].inserted;
        vir_edge_replace_args(edge, edit->edges[i].args, edit->edges[i].uses,
                              new_count);
    }
    if (edit->insert) {
        vir_block_append_param(block, param);
    } else {
        vir_value_t *cursor, *previous = NULL;
        for (cursor = block->params; cursor && cursor != param;
             cursor = cursor->param_next)
            previous = cursor;
        if (previous)
            previous->param_next = param->param_next;
        else
            block->params = param->param_next;
        if (block->last_param == param)
            block->last_param = previous;
        block->param_count--;
    }
}

static int vir_count_uses(const vir_value_t *value,
                          vir_use_owner_t kind,
                          const void *owner,
                          int operand)
{
    const vir_use_t *use;
    int count = 0;

    for (use = value->uses; use; use = use->next)
        if (vir_use_matches(use, kind, owner, operand))
            count++;
    return count;
}

#define vir_use_count(value, user, operand) \
    vir_count_uses(value, VIR_USE_USER, user, operand)
#define vir_edge_use_count(value, edge, operand) \
    vir_count_uses(value, VIR_USE_EDGE, edge, operand)
#define vir_effect_use_count(value, effect, operand) \
    vir_count_uses(value, VIR_USE_EFFECT, effect, operand)

#define DEFINE_BLOCK_LIST_CONTAINS(name, type, head, link)         \
    static bool name(const vir_block_t *block, const type *needle) \
    {                                                              \
        const type *item;                                          \
        for (item = block->head; item; item = item->link)          \
            if (item == needle)                                    \
                return true;                                       \
        return false;                                              \
    }

DEFINE_BLOCK_LIST_CONTAINS(vir_has_incoming_edge,
                           vir_edge_t,
                           incoming,
                           next_incoming)
DEFINE_BLOCK_LIST_CONTAINS(vir_has_outgoing_edge,
                           vir_edge_t,
                           outgoing,
                           next_outgoing)
DEFINE_BLOCK_LIST_CONTAINS(vir_has_effect, vir_effect_t, effects, next)

#undef DEFINE_BLOCK_LIST_CONTAINS

#define vir_return_use_count(value, block) \
    vir_count_uses(value, VIR_USE_RETURN, block, -1)
#define vir_branch_use_count(value, block) \
    vir_count_uses(value, VIR_USE_BRANCH, block, -1)

static int vir_block_index(const vir_function_t *func,
                           const vir_block_t *needle)
{
    const vir_block_t *block;
    int index = 0;

    if (!func || !needle)
        return -1;
    for (block = func->blocks; block; block = block->next, index++)
        if (block == needle)
            return index;
    return -1;
}

static bool vir_has_block(const vir_function_t *func, const vir_block_t *needle)
{
    return vir_block_index(func, needle) >= 0;
}

static bool vir_block_open(const vir_function_t *func, const vir_block_t *block)
{
    return vir_has_block(func, block) && block->terminator == VIR_TERM_NONE;
}

static int vir_count_values(const vir_function_t *func,
                            const vir_value_t *needle,
                            int id)
{
    const vir_block_t *block;
    const vir_value_t *value;
    int count = 0;

#define VIR_COUNT_VALUE_CHAIN(head, link)                 \
    for (value = (head); value; value = value->link)      \
        if (needle ? value == needle : value->id == id) { \
            if (needle)                                   \
                return 1;                                 \
            count++;                                      \
        }
    for (block = func->blocks; block; block = block->next) {
        VIR_COUNT_VALUE_CHAIN(block->params, param_next);
        VIR_COUNT_VALUE_CHAIN(block->head, next);
    }
#undef VIR_COUNT_VALUE_CHAIN
    return count;
}

static bool vir_has_value(const vir_function_t *func, const vir_value_t *needle)
{
    if (!needle)
        return false;
    return vir_count_values(func, needle, 0) != 0;
}

static int vir_value_id_count(const vir_function_t *func, int id)
{
    return vir_count_values(func, NULL, id);
}

#define VIR_LIST_HAS_CYCLE(type, head, link)       \
    do {                                           \
        const type *slow = (head), *fast = (head); \
        while (fast && fast->link) {               \
            slow = slow->link;                     \
            fast = fast->link->link;               \
            if (slow == fast)                      \
                return 0;                          \
        }                                          \
    } while (0)

static bool vir_lists_are_acyclic(const vir_function_t *func)
{
    const vir_block_t *block;
    VIR_LIST_HAS_CYCLE(vir_block_t, func->blocks, next);
    for (block = func->blocks; block; block = block->next) {
        const vir_value_t *value;
        VIR_LIST_HAS_CYCLE(vir_value_t, block->params, param_next);
        VIR_LIST_HAS_CYCLE(vir_value_t, block->head, next);
        VIR_LIST_HAS_CYCLE(vir_edge_t, block->incoming, next_incoming);
        VIR_LIST_HAS_CYCLE(vir_edge_t, block->outgoing, next_outgoing);
        VIR_LIST_HAS_CYCLE(vir_effect_t, block->effects, next);
        for (value = block->params; value; value = value->param_next)
            VIR_LIST_HAS_CYCLE(vir_use_t, value->uses, next);
        for (value = block->head; value; value = value->next)
            VIR_LIST_HAS_CYCLE(vir_use_t, value->uses, next);
    }
    return 1;
}

#undef VIR_LIST_HAS_CYCLE

int vir_function_block_count(const vir_function_t *func)
{
    const vir_block_t *block;
    int count = 0;
    for (block = func->blocks; block; block = block->next)
        count++;
    return count;
}

static bool vir_mark_reachable_blocks(const vir_function_t *func,
                                      const vir_block_t *blocked,
                                      unsigned char *reachable,
                                      int *work)
{
    int head = 0;
    int tail = 0;

    reachable[0] = 1;
    work[tail++] = 0;
    while (head < tail) {
        const vir_block_t *block = func->blocks;
        int index = work[head++];
        const vir_edge_t *edge;

        for (; index; index--)
            block = block->next;
        for (edge = block->outgoing; edge; edge = edge->next_outgoing) {
            int target;

            if (blocked && edge->to == blocked)
                continue;
            target = vir_block_index(func, edge->to);
            if (target < 0) {
                if (blocked)
                    continue;
                return false;
            }
            if (reachable[target])
                continue;
            reachable[target] = 1;
            work[tail++] = target;
        }
    }
    return true;
}

/* Non-verifier transformations use a bounded reachability query rather than the
 * old quadratic int matrix. The verifier itself builds a compact bitset matrix
 * once per function below.
 */
static bool vir_value_dominates_by_reachability(const vir_function_t *func,
                                                const vir_value_t *value,
                                                const vir_block_t *use_block,
                                                int count)
{
    unsigned char *reachable;
    int *work;
    int use_index;

    if (vir_block_index(func, value->block) < 0 ||
        (use_index = vir_block_index(func, use_block)) < 0 || count <= 0 ||
        count > INT_MAX / (int) sizeof(*work))
        return false;
    if (value->block == func->blocks)
        return true;
    reachable = calloc((size_t) count, sizeof(*reachable));
    work = malloc((size_t) count * sizeof(*work));
    if (!reachable || !work) {
        free(reachable);
        free(work);
        return false;
    }

    /* If the use is unreachable, no path can avoid the definition and the
     * result is vacuously true. The same walk also handles that case.
     */
    if (!vir_mark_reachable_blocks(func, value->block, reachable, work)) {
        free(reachable);
        free(work);
        return false;
    }
    use_index = !reachable[use_index];
    free(reachable);
    free(work);
    return use_index;
}

struct vir_dominance {
    vir_block_t **blocks;
    int *indices;
    unsigned long *dom;
    unsigned long *candidate;
    int count;
    int words;
    int id_count;
};

static void vir_dominance_release(vir_dominance_t *dominance)
{
    free(dominance->blocks);
    free(dominance->indices);
    free(dominance->dom);
    free(dominance->candidate);
    memset(dominance, 0, sizeof(*dominance));
}

static int vir_dominance_index(const vir_dominance_t *dominance,
                               const vir_block_t *block)
{
    int index;

    if (!block || block->id < 0 || block->id >= dominance->id_count)
        return -1;
    index = dominance->indices[block->id];
    return index >= 0 && dominance->blocks[index] == block ? index : -1;
}

static bool vir_dominance_block_dominates(const vir_dominance_t *dominance,
                                          const vir_block_t *dominator,
                                          const vir_block_t *block)
{
    int definition = vir_dominance_index(dominance, dominator);
    int use = vir_dominance_index(dominance, block);
    int word_bits = (int) (sizeof(unsigned long) * CHAR_BIT);

    return definition >= 0 && use >= 0 &&
           (dominance->dom[use * dominance->words + definition / word_bits] &
            (1UL << (definition % word_bits))) != 0;
}

static bool vir_dominance_init(const vir_function_t *func,
                               vir_dominance_t *dominance)
{
    const vir_block_t *block;
    int count = vir_function_block_count(func);
    int words;
    int i;

    memset(dominance, 0, sizeof(*dominance));
    if (count <= 0 || func->next_block_id <= 0 ||
        func->next_block_id > INT_MAX / (int) sizeof(*dominance->indices))
        return false;
    if (count > INT_MAX - (int) (sizeof(unsigned long) * CHAR_BIT) + 1)
        return false;
    words = (count + (int) (sizeof(unsigned long) * CHAR_BIT) - 1) /
            (int) (sizeof(unsigned long) * CHAR_BIT);
    if (words <= 0 || count > INT_MAX / words / (int) sizeof(*dominance->dom))
        return false;
    dominance->blocks = malloc((size_t) count * sizeof(*dominance->blocks));
    dominance->indices =
        malloc((size_t) func->next_block_id * sizeof(*dominance->indices));
    dominance->dom =
        calloc((size_t) count * (size_t) words, sizeof(*dominance->dom));
    dominance->candidate =
        malloc((size_t) words * sizeof(*dominance->candidate));
    if (!dominance->blocks || !dominance->indices || !dominance->dom ||
        !dominance->candidate) {
        vir_dominance_release(dominance);
        return false;
    }
    dominance->count = count;
    dominance->words = words;
    dominance->id_count = func->next_block_id;
    for (i = 0; i < dominance->id_count; i++)
        dominance->indices[i] = -1;
    for (block = func->blocks, i = 0; block; block = block->next, i++) {
        if (block->id < 0 || block->id >= dominance->id_count ||
            dominance->indices[block->id] >= 0) {
            vir_dominance_release(dominance);
            return false;
        }
        dominance->blocks[i] = (vir_block_t *) block;
        dominance->indices[block->id] = i;
    }

    for (i = 0; i < count * words; i++)
        dominance->dom[i] = ~0UL;
    for (i = 0; i < words; i++)
        dominance->dom[i] = 0;
    dominance->dom[0] = 1UL;
    for (;;) {
        bool changed = false;
        for (i = 1; i < count; i++) {
            const vir_edge_t *edge;
            vir_block_t *current = dominance->blocks[i];
            int have_predecessor = 0;
            int word;

            for (word = 0; word < words; word++)
                dominance->candidate[word] = ~0UL;
            for (edge = current->incoming; edge; edge = edge->next_incoming) {
                int predecessor = vir_dominance_index(dominance, edge->from);
                if (predecessor < 0) {
                    vir_dominance_release(dominance);
                    return false;
                }
                if (!have_predecessor) {
                    for (word = 0; word < words; word++)
                        dominance->candidate[word] =
                            dominance->dom[predecessor * words + word];
                    have_predecessor = 1;
                } else {
                    for (word = 0; word < words; word++)
                        dominance->candidate[word] &=
                            dominance->dom[predecessor * words + word];
                }
            }
            dominance
                ->candidate[i / (int) (sizeof(unsigned long) * CHAR_BIT)] |=
                1UL << (i % (int) (sizeof(unsigned long) * CHAR_BIT));
            for (word = 0; word < words; word++) {
                unsigned long *slot = &dominance->dom[i * words + word];
                if (*slot != dominance->candidate[word]) {
                    *slot = dominance->candidate[word];
                    changed = true;
                }
            }
        }
        if (!changed)
            break;
    }
    return true;
}

/* Drop loop-invariant and forwarding parameters without source SSA bindings. */
static int vir_simplify_params_with_context(vir_function_t *func,
                                            const vir_dominance_t *dominance)
{
    int changed = 0, previous;
    do {
        previous = changed;
        for (vir_block_t *block = func->blocks; block; block = block->next) {
            int index = 0;
            for (vir_value_t *param = block->params, *next; param;
                 param = next) {
                vir_value_t *candidate = NULL;
                bool trivial = true;
                next = param->param_next;
                for (vir_edge_t *edge = block->incoming; edge;
                     edge = edge->next_incoming) {
                    vir_value_t *value = edge->args[index];
                    if (value == param)
                        continue;
                    if (candidate && candidate != value)
                        trivial = false;
                    candidate = value;
                }
                if (!trivial || !candidate ||
                    !vir_replacement_dominates_with_context(func, dominance,
                                                            param, candidate)) {
                    index++;
                    continue;
                }
                vir_param_edit_t edit;
                if (!vir_param_edit_prepare(func, block, param,
                                            block->param_count, index, false,
                                            &edit))
                    return -1;
                vir_replace_all_uses_unchecked(param, candidate);
                vir_param_edit_apply(block, param, &edit);
                vir_param_edit_release(&edit);
                changed++;
            }
        }
    } while (changed != previous);
    return changed;
}

int vir_simplify_params(vir_function_t *func, vir_opt_level_t opt_level)
{
    vir_dominance_t dominance;
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    if (!vir_dominance_init(func, &dominance))
        return -1;
    int changed = vir_simplify_params_with_context(func, &dominance);
    vir_dominance_release(&dominance);
    return changed;
}

/* An affine offset is safe to recur only while every narrower intermediate
 * stays nonnegative and fits its signed type. Pointer-width arithmetic itself
 * is modular, so ILP32 does not need the LP64 narrowing proof.
 */
static bool vir_sr_affine(const vir_function_t *func,
                          vir_value_t *value,
                          vir_value_t *iv,
                          unsigned long long limit,
                          unsigned long long *scale,
                          unsigned long long *bias,
                          int depth)
{
    unsigned long long multiplier = 1, add = 0;
    if (value == iv) {
        *scale = 1;
        *bias = 0;
        return true;
    }
    if (!value || depth == 16)
        return false;
    if (value->opcode == VIR_OP_SEXT || value->opcode == VIR_OP_ZEXT) {
        if (!vir_sr_affine(func, value->op0, iv, limit, scale, bias, depth + 1))
            return false;
    } else {
        vir_value_t *operand = value->op0;
        vir_value_t *constant = value->op1;
        if ((value->opcode == VIR_OP_MUL || value->opcode == VIR_OP_ADD) &&
            vir_is_constant(operand)) {
            constant = operand;
            operand = value->op1;
        }
        if (!vir_is_constant(constant))
            return false;
        if (value->opcode == VIR_OP_MUL)
            multiplier = constant->constant;
        else if (value->opcode == VIR_OP_SHL && constant->constant < 31)
            multiplier = 1ULL << constant->constant;
        else if (value->opcode == VIR_OP_ADD)
            add = constant->constant;
        else
            return false;
        if (!multiplier || multiplier > INT_MAX || add > INT_MAX ||
            !vir_sr_affine(func, operand, iv, limit, scale, bias, depth + 1) ||
            *scale > 0x7fffffffffffffffULL / multiplier ||
            *bias > (0x7fffffffffffffffULL - add) / multiplier)
            return false;
        *scale *= multiplier;
        *bias = *bias * multiplier + add;
    }
    int bits = vir_integer_type_width(value->type);
    unsigned long long maximum =
        bits == 64 ? 0x7fffffffffffffffULL : (1ULL << (bits - 1)) - 1;
    return bits && (func->pointer_bits == 32 ||
                    (*bias <= maximum && *scale <= (maximum - *bias) / limit));
}

/* Root addresses are harmless to rematerialize even when first mentioned in the
 * loop. Other pure base expressions move only with invariant operands.
 */
static vir_value_t *vir_sr_base(vir_function_t *func,
                                const vir_dominance_t *dominance,
                                vir_block_t *pre,
                                vir_value_t *value,
                                int depth)
{
    if (vir_value_dominates(func, dominance, value, pre, INT_MAX))
        return value;
    if (depth == 16 || value->is_block_param || value->def_effect ||
        vir_opcode_may_trap(value->opcode))
        return NULL;
    if (value->opcode == VIR_OP_GLOBAL_ADDR ||
        value->opcode == VIR_OP_STACK_ADDR)
        for (vir_value_t *root = pre->head; root; root = root->next)
            if (root->opcode == value->opcode &&
                (value->opcode == VIR_OP_STACK_ADDR
                     ? root->address_slot == value->address_slot
                     : !strcmp(root->address_name, value->address_name)))
                return root;
    if (value->opcode == VIR_OP_GLOBAL_ADDR)
        return vir_global_addr(func, pre, value->address_name,
                               value->address_size, value->address_alignment);
    if (value->opcode == VIR_OP_STACK_ADDR)
        return vir_stack_addr(func, pre, value->address_slot,
                              value->address_size, value->address_alignment);
    if (value->opcode == VIR_OP_CONST)
        return vir_const(func, pre, value->type, value->constant);
    if (!value->nr_ops)
        return NULL;
    vir_value_t *left =
        vir_sr_base(func, dominance, pre, value->op0, depth + 1);
    vir_value_t *right = value->nr_ops == 2 ? vir_sr_base(func, dominance, pre,
                                                          value->op1, depth + 1)
                                            : NULL;
    return left && (value->nr_ops == 1 || right)
               ? vir_user_operation(func, pre, value->opcode, value->type, left,
                                    right)
               : NULL;
}

typedef struct {
    unsigned long long first;
    unsigned long long step;
} vir_sr_offset_t;

static int vir_sr_pointer(vir_function_t *func,
                          vir_block_t *header,
                          vir_edge_t *entry,
                          vir_edge_t *back,
                          vir_value_t *address,
                          vir_value_t *base,
                          const vir_sr_offset_t *offsets,
                          vir_value_t **pointer)
{
    int index = 0;
    unsigned long long first = offsets->first;
    for (vir_value_t *param = header->params; param;
         param = param->param_next, index++) {
        vir_value_t *initial = entry->args[index];
        vir_value_t *next = back->args[index];
        bool same_initial = !first ? initial == base
                                   : initial->opcode == VIR_OP_PTRADD &&
                                         initial->op0 == base &&
                                         vir_is_const_int(initial->op1, first);
        if (param->type == VIR_TYPE_PTR && same_initial &&
            next->opcode == VIR_OP_PTRADD && next->op0 == param &&
            vir_is_const_int(next->op1, offsets->step)) {
            if (!vir_replace_all_uses(func, address, param))
                return -1;
            *pointer = param;
            return 1;
        }
    }
    vir_param_edit_t edit;
    vir_type_t width = func->pointer_bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    if (!vir_param_edit_prepare(func, header, NULL, header->param_count,
                                header->param_count, true, &edit))
        return -1;
    vir_value_t *offset = vir_const_int(func, entry->from, width, first);
    vir_value_t *initial =
        offset ? vir_ptradd(func, entry->from, base, offset) : NULL;
    vir_value_t *step = vir_const_int(func, back->from, width, offsets->step);
    vir_value_t *param = vir_value_alloc(func, header, VIR_TYPE_PTR);
    vir_value_t *next =
        initial && step && param
            ? vir_user_operation(func, back->from, VIR_OP_PTRADD, VIR_TYPE_PTR,
                                 param, step)
            : NULL;
    if (!next) {
        vir_param_edit_release(&edit);
        return -1;
    }
    for (int i = 0; i < edit.count; i++)
        edit.edges[i].inserted = edit.edges[i].edge == entry ? initial : next;
    vir_param_edit_apply(header, param, &edit);
    vir_param_edit_release(&edit);
    if (!vir_replace_all_uses(func, address, param))
        return -1;
    *pointer = param;
    return 1;
}

/* Frontend boolean conversions may invert the comparison feeding a branch. */
static vir_value_t *vir_sr_condition(vir_value_t *value, bool *positive)
{
    for (int depth = 0; value && depth < 16; depth++) {
        if (value->opcode == VIR_OP_ZEXT && value->op0->type == VIR_TYPE_I1) {
            value = value->op0;
        } else if (value->opcode == VIR_OP_EQ &&
                   ((vir_is_constant(value->op0) && !value->op0->constant) ||
                    (vir_is_constant(value->op1) && !value->op1->constant))) {
            value = vir_is_constant(value->op0) ? value->op1 : value->op0;
            *positive = !*positive;
        } else {
            return value;
        }
    }
    return NULL;
}

/* A unique latch incremented by one under a signed i < bound test cannot wrap
 * before exiting. Literal bounds additionally allow a pointer equality exit
 * test and retirement of an otherwise unused counter recurrence.
 */
int vir_strength_reduce(vir_function_t *func, vir_opt_level_t opt_level)
{
    vir_dominance_t dominance;
    int changed = 0;
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    if (vir_dce(func, opt_level) < 0 || !vir_dominance_init(func, &dominance))
        return -1;
    if (vir_simplify_params_with_context(func, &dominance) < 0) {
        vir_dominance_release(&dominance);
        return -1;
    }
    for (vir_block_t *header = func->blocks; header; header = header->next) {
        vir_edge_t *entry = NULL, *back = NULL;
        bool positive = true;
        vir_value_t *branch = header->branch_condition;
        vir_value_t *condition = vir_sr_condition(branch, &positive);
        if (header->terminator != VIR_TERM_BRANCH || !condition ||
            condition->opcode != VIR_OP_SLT ||
            condition->op0->type != VIR_TYPE_I32)
            continue;
        for (vir_edge_t *edge = header->incoming; edge;
             edge = edge->next_incoming) {
            if (vir_dominance_block_dominates(&dominance, header, edge->from)) {
                if (back) {
                    back = NULL;
                    break;
                }
                back = edge;
            } else {
                if (entry) {
                    entry = NULL;
                    break;
                }
                entry = edge;
            }
        }
        vir_value_t *iv = condition->op0;
        vir_value_t *limit = condition->op1;
        bool inclusive = vir_is_constant(iv) && limit->is_block_param;
        if (inclusive) {
            iv = condition->op1;
            limit = condition->op0;
            positive = !positive;
        }
        vir_edge_t *body_edge =
            positive ? header->true_edge : header->false_edge;
        if (!entry || !back || entry->from->terminator != VIR_TERM_JUMP ||
            back->from->terminator != VIR_TERM_JUMP || back->from == header ||
            !vir_dominance_block_dominates(&dominance, body_edge->to,
                                           back->from) ||
            (!vir_is_constant(limit) &&
             vir_dominance_block_dominates(&dominance, header, limit->block)))
            continue;
        int index = 0;
        for (vir_value_t *param = header->params; param && param != iv;
             param = param->param_next)
            index++;
        if (!iv->is_block_param || iv->block != header ||
            index == header->param_count)
            continue;
        vir_value_t *initial = entry->args[index];
        vir_value_t *increment = back->args[index];
        if (!vir_is_constant(initial) || initial->constant > INT_MAX ||
            increment->opcode != VIR_OP_ADD ||
            !((increment->op0 == iv && vir_is_const_int(increment->op1, 1)) ||
              (increment->op1 == iv && vir_is_const_int(increment->op0, 1))))
            continue;
        unsigned long long bound = vir_is_constant(limit)
                                       ? limit->constant + (inclusive ? 1 : 0)
                                       : INT_MAX;
        if (!bound || bound > INT_MAX || initial->constant >= bound)
            continue;
        vir_value_t *walk = NULL, *walk_base = NULL;
        unsigned long long walk_scale = 0, walk_bias = 0;
        for (vir_block_t *block = func->blocks; block; block = block->next) {
            if (!vir_dominance_block_dominates(&dominance, header, block) ||
                !vir_dominance_block_dominates(&dominance, block, back->from))
                continue;
            for (vir_value_t *address = block->head, *next; address;
                 address = next) {
                next = address->next;
                unsigned long long scale, bias;
                if (address->opcode != VIR_OP_PTRADD || !address->uses ||
                    !vir_sr_affine(func, address->op1, iv, bound, &scale, &bias,
                                   0))
                    continue;
                vir_value_t *pointer;
                vir_value_t *base =
                    vir_sr_base(func, &dominance, entry->from, address->op0, 0);
                if (!base)
                    continue;
                vir_sr_offset_t offsets = {
                    (initial->constant * scale + bias) &
                        vir_width_mask(func->pointer_bits),
                    scale};
                if (vir_sr_pointer(func, header, entry, back, address, base,
                                   &offsets, &pointer) < 0) {
                    changed = -1;
                    goto done;
                }
                changed++;
                walk = pointer;
                walk_base = base;
                walk_scale = scale;
                walk_bias = bias;
            }
        }
        if (!walk || !vir_is_constant(limit) ||
            (bound - initial->constant) >
                vir_width_mask(func->pointer_bits) / walk_scale ||
            !branch->uses || branch->uses->next ||
            branch->uses->branch_block != header)
            continue;

        /* The equality endpoint works even when the concrete pointer wraps: the
         * bounded span cannot complete a full address-space revolution.
         */
        vir_type_t width =
            func->pointer_bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_value_t *offset = vir_const_int(func, entry->from, width,
                                            bound * walk_scale + walk_bias);
        vir_value_t *end =
            offset ? vir_ptradd(func, entry->from, walk_base, offset) : NULL;
        vir_value_t *equal = end ? vir_eq(func, header, walk, end) : NULL;
        vir_value_t *zero = vir_const_i1(func, header, 0);
        vir_value_t *test =
            positive
                ? (equal && zero ? vir_eq(func, header, equal, zero) : NULL)
                : equal;
        if (!test || !vir_replace_all_uses(func, branch, test)) {
            changed = -1;
            goto done;
        }
        if (vir_dce(func, opt_level) < 0) {
            changed = -1;
            goto done;
        }
        if (iv->uses && !iv->uses->next && iv->uses->user == increment &&
            increment->uses && !increment->uses->next &&
            increment->uses->edge == back &&
            increment->uses->operand == index) {
            vir_param_edit_t edit;
            if (!vir_param_edit_prepare(func, header, iv, header->param_count,
                                        index, false, &edit)) {
                changed = -1;
                goto done;
            }
            vir_replace_all_uses_unchecked(iv, initial);
            vir_param_edit_apply(header, iv, &edit);
            vir_param_edit_release(&edit);
        }
    }
done:
    vir_dominance_release(&dominance);
    return changed;
}

static bool vir_value_dominates_effect(const vir_function_t *func,
                                       const vir_dominance_t *dominance,
                                       const vir_value_t *value,
                                       const vir_effect_t *effect)
{
    vir_value_t **pending;
    unsigned char *seen;
    int count;
    int pending_count = 0;
    int position;
    bool dominates = true;

    if (!func || !value || !effect || !effect->block)
        return false;
    position = effect->block->tail ? effect->block->tail->position + 1 : 0;
    if (value->def_effect) {
        if (value->def_effect->block == effect->block)
            return value->def_effect->position < effect->position;
        return vir_value_dominates(func, dominance, value, effect->block,
                                   position);
    }
    if (value->block == effect->block &&
        (value->is_block_param || value->order <= effect->order))
        return true;
    if (value->is_block_param || value->nr_ops == 0)
        return vir_value_dominates(func, dominance, value, effect->block,
                                   position);
    count = func->next_value_id;
    if (value->id < 0 || value->id >= count ||
        count > INT_MAX / (int) sizeof(*pending))
        return false;
    seen = calloc((size_t) count, sizeof(*seen));
    pending = malloc((size_t) count * sizeof(*pending));
    if (!seen || !pending) {
        free(seen);
        free(pending);
        return false;
    }
    pending[pending_count++] = (vir_value_t *) value;
    seen[value->id] = 1;
    while (pending_count && dominates) {
        const vir_value_t *current = pending[--pending_count];

        if (current->def_effect) {
            if (current->def_effect->block == effect->block)
                dominates = current->def_effect->position < effect->position;
            else
                dominates = vir_value_dominates(func, dominance, current,
                                                effect->block, position);
            continue;
        }
        if (current->block != effect->block &&
            !vir_value_dominates(func, dominance, current, effect->block,
                                 position)) {
            dominates = false;
            continue;
        }
        if (current->is_block_param || (current->block == effect->block &&
                                        current->order <= effect->order))
            continue;
        if (current->nr_ops < 0 || current->nr_ops > 2) {
            dominates = false;
            continue;
        }
        for (int i = 0; i < current->nr_ops; i++) {
            const vir_value_t *operand = i ? current->op1 : current->op0;

            if (!operand || operand->id < 0 || operand->id >= count) {
                dominates = false;
                break;
            }
            if (!seen[operand->id]) {
                seen[operand->id] = 1;
                pending[pending_count++] = (vir_value_t *) operand;
            }
        }
    }
    free(pending);
    free(seen);
    return dominates;
}

static int vir_block_id_count(const vir_function_t *func, int id)
{
    const vir_block_t *block;
    int count = 0;
    for (block = func->blocks; block; block = block->next)
        if (block->id == id)
            count++;
    return count;
}

static bool vir_value_uses_are_dead(const vir_function_t *func,
                                    const vir_value_t *value,
                                    const unsigned char *reachable)
{
    const vir_use_t *use;
    for (use = value->uses; use; use = use->next) {
        int index = vir_block_index(func, vir_use_block(use));

        if (index < 0 || reachable[index])
            return 0;
    }
    return 1;
}

int vir_remove_unreachable(vir_function_t *func)
{
    vir_block_t *block;
    vir_block_t **link;
    unsigned char *reachable;
    int *work;
    int count;
    int i;

    if (!func || !func->blocks)
        return 0;
    count = vir_function_block_count(func);
    if (count > INT_MAX / (int) sizeof(*reachable) ||
        count > INT_MAX / (int) sizeof(*work))
        return 0;
    reachable = calloc((size_t) count, sizeof(*reachable));
    work = malloc(sizeof(*work) * (size_t) count);
    if (!reachable || !work) {
        free(reachable);
        free(work);
        return 0;
    }
    if (!vir_mark_reachable_blocks(func, NULL, reachable, work)) {
        free(reachable);
        free(work);
        return 0;
    }
    for (block = func->blocks, i = 0; block; block = block->next, i++) {
        const vir_value_t *value;
        if (reachable[i])
            continue;
        for (value = block->params; value; value = value->param_next)
            if (!vir_value_uses_are_dead(func, value, reachable))
                goto fail;
        for (value = block->head; value; value = value->next)
            if (!vir_value_uses_are_dead(func, value, reachable))
                goto fail;
    }
    for (block = func->blocks, i = 0; block; block = block->next, i++) {
        vir_value_t *value;
        if (reachable[i])
            continue;
        while (block->outgoing)
            vir_edge_unlink(block->outgoing);
        if (block->terminator == VIR_TERM_RETURN && block->return_value)
            vir_remove_return_use(block->return_value, block);
        else if (block->terminator == VIR_TERM_BRANCH)
            vir_remove_branch_use(block->branch_condition, block);
        {
            vir_effect_t *effect;
            for (effect = block->effects; effect; effect = effect->next)
                vir_remove_effect_uses(effect);
        }
        for (value = block->head; value; value = value->next)
            for (int operand = 0; operand < value->nr_ops; operand++)
                vir_remove_user_use(operand ? value->op1 : value->op0, value,
                                    operand);
    }
    link = &func->blocks;
    func->last_block = NULL;
    for (i = 0; *link;) {
        block = *link;
        if (!reachable[i])
            *link = block->next;
        else {
            func->last_block = block;
            link = &block->next;
        }
        i++;
    }
    free(reachable);
    free(work);
    return 1;

fail:
    free(reachable);
    free(work);
    return 0;
}

static bool vir_is_empty_jump_forwarder(const vir_function_t *func,
                                        const vir_block_t *block)
{
    const vir_edge_t *edge;

    return block != func->blocks && block->incoming && !block->params &&
           !block->head && !block->effects &&
           block->terminator == VIR_TERM_JUMP && (edge = block->outgoing) &&
           !edge->next_outgoing && !edge->arg_count && edge->to != block;
}

/* Replace jumps into a return-only parameter block with direct returns. */
static int vir_inline_return_param(vir_function_t *func, vir_block_t *block)
{
    vir_value_t *result;
    vir_use_t *use;
    vir_edge_t *edge;
    int index = 0;

    if (block == func->blocks || block->terminator != VIR_TERM_RETURN ||
        block->head || block->effects || !(result = block->return_value) ||
        !result->is_block_param || result->block != block)
        return 0;
    for (vir_value_t *param = block->params; param != result;
         param = param->param_next) {
        if (!param)
            return 0;
        index++;
    }
    for (edge = block->incoming; edge; edge = edge->next_incoming)
        if (!edge->from || edge->from->terminator != VIR_TERM_JUMP ||
            edge->from->outgoing != edge || edge->next_outgoing ||
            edge->arg_count != block->param_count || !edge->args ||
            index >= edge->arg_count)
            return 0;
    if (!block->incoming)
        return 0;
    for (edge = block->incoming; edge;) {
        vir_edge_t *next = edge->next_incoming;
        vir_block_t *from = edge->from;
        vir_value_t *value = edge->args[index];

        if (!(use = vir_arena_alloc(&func->arena, sizeof(*use)))) {
            return -1;
        }
        if (!vir_edge_delete(func, edge))
            return -1;
        from->terminator = VIR_TERM_RETURN;
        from->return_value = value;
        use->return_block = from;
        vir_link_use(value, use);
        edge = next;
    }
    vir_remove_return_use(result, block);
    return 1;
}

/* A branch on "c == true" is a branch on c, and one on "c == false" is a branch
 * on c with its edges swapped. The frontend spells "x != 0" as (x == 0) ==
 * false, so this leaves one compare per test, which a backend fuses with its
 * jump, and DCE drops the outer compare once the branch was its only use.
 * Equality operands are ordered by id, so the constant may be either one.
 */
static bool vir_fold_branch_condition(vir_block_t *block)
{
    vir_value_t *condition = block->branch_condition;
    vir_value_t *operand;
    vir_value_t *constant;
    vir_use_t *use;

    if (block->terminator != VIR_TERM_BRANCH || !condition ||
        condition->opcode != VIR_OP_EQ || !condition->op0 || !condition->op1)
        return false;
    operand = condition->op0;
    constant = condition->op1;
    if (operand->opcode == VIR_OP_CONST) {
        operand = condition->op1;
        constant = condition->op0;
    }
    if (constant->opcode != VIR_OP_CONST || operand->opcode == VIR_OP_CONST)
        return false;
    if (operand->type != VIR_TYPE_I1) {
        /* C promotes comparison results to int before testing them again. Zero
         * extension preserves the boolean values zero and one.
         */
        if (operand->opcode != VIR_OP_ZEXT || !operand->op0 ||
            operand->op0->type != VIR_TYPE_I1 || constant->constant > 1)
            return false;
        operand = operand->op0;
    }
    for (use = condition->uses; use && use->branch_block != block;
         use = use->next)
        ;
    if (!use)
        return false;
    vir_set_use_slot(&block->branch_condition, operand, use);
    if (!constant->constant) {
        vir_edge_t *edge = block->true_edge;

        block->true_edge = block->false_edge;
        block->false_edge = edge;
    }
    return true;
}

int vir_simplify_cfg_with_pruning(vir_function_t *func,
                                  vir_opt_level_t opt_level,
                                  bool prune_unreachable)
{
    int changes = 0;
    bool changed;

    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    /* Folding a condition changes no block, so it is not counted. */
    for (vir_block_t *block = func->blocks; block; block = block->next)
        while (vir_fold_branch_condition(block))
            ;
    do {
        vir_block_t *block;

        changed = false;
        for (block = func->blocks; block; block = block->next) {
            vir_edge_t *edge;
            vir_block_t *target;
            int inlined;

            inlined = vir_inline_return_param(func, block);
            if (inlined < 0)
                return -1;
            if (inlined) {
                changes++;
                changed = true;
                break;
            }

            if (!vir_is_empty_jump_forwarder(func, block))
                continue;
            target = block->outgoing->to;
            for (edge = block->incoming; edge;) {
                vir_edge_t *next = edge->next_incoming;

                if (!vir_edge_redirect(func, edge, target))
                    return -1;
                edge = next;
            }
            if (block->incoming)
                return -1;
            changes++;
            changed = true;
            break;
        }
    } while (changed);
    if (changes && prune_unreachable && !vir_remove_unreachable(func))
        return -1;
    return changes;
}

int vir_simplify_cfg(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_simplify_cfg_with_pruning(func, opt_level, true);
}

/* Dominance is queried by reachability only; no analysis state is retained in
 * the IR or allocated as an all-pairs matrix.
 */
static bool vir_value_dominates(const vir_function_t *func,
                                const vir_dominance_t *dominance,
                                const vir_value_t *value,
                                const vir_block_t *use_block,
                                int use_position)
{
    if (!value || !use_block || (!dominance && (!func || !func->blocks)))
        return false;
    if (value->block == use_block)
        return value->is_block_param || value->position < use_position;
    if (dominance)
        return vir_dominance_block_dominates(dominance, value->block,
                                             use_block);
    return vir_value_dominates_by_reachability(func, value, use_block,
                                               vir_function_block_count(func));
}

static bool vir_licm_add_temporary_bytes(size_t *bytes, size_t amount)
{
    if (amount > (size_t) INT_MAX - *bytes)
        return false;
    *bytes += amount;
    return true;
}

static void vir_licm_mark_loop(const vir_block_t *header,
                               const vir_block_t *latch,
                               const vir_dominance_t *dominance,
                               unsigned char *in_loop,
                               int *work,
                               int *value_blocks)
{
    int head = 0;
    int tail = 0;
    int header_index = vir_dominance_index(dominance, header);
    int latch_index = vir_dominance_index(dominance, latch);

    if (header_index < 0 || latch_index < 0)
        return;
    if (!in_loop[header_index]) {
        in_loop[header_index] = 1;
        if (dominance->blocks[header_index]->head)
            (*value_blocks)++;
    }
    if (!in_loop[latch_index]) {
        in_loop[latch_index] = 1;
        if (dominance->blocks[latch_index]->head)
            (*value_blocks)++;
        work[tail++] = latch_index;
    }
    while (head < tail) {
        const vir_edge_t *edge;
        const vir_block_t *block = dominance->blocks[work[head++]];

        if (block == header)
            continue;
        for (edge = block->incoming; edge; edge = edge->next_incoming) {
            int pred = vir_dominance_index(dominance, edge->from);

            if (pred < 0 || in_loop[pred])
                continue;
            in_loop[pred] = 1;
            if (dominance->blocks[pred]->head)
                (*value_blocks)++;
            work[tail++] = pred;
        }
    }
}

static vir_block_t *vir_licm_preheader(const vir_block_t *header,
                                       const vir_dominance_t *dominance,
                                       const unsigned char *in_loop)
{
    const vir_edge_t *edge;
    vir_block_t *preheader = NULL;

    for (edge = header->incoming; edge; edge = edge->next_incoming) {
        int pred = vir_dominance_index(dominance, edge->from);

        if (pred < 0)
            return NULL;
        if (!in_loop[pred]) {
            if (preheader)
                return NULL;
            preheader = edge->from;
        }
    }
    return preheader;
}

static bool vir_licm_value_is_candidate(const vir_value_t *value)
{
    return !value->is_block_param && !value->def_effect && value->nr_ops &&
           !vir_opcode_may_trap(value->opcode);
}

#define VIR_LICM_EAGER_VALUE_BLOCK_LIMIT 128

/* Return an indexed candidate's number of loop-local operands; -1 excludes
 * values that LICM cannot move. Reverse uses release dependents as operands
 * move.
 */
static int vir_licm_value_loop_dependencies(vir_value_t *value,
                                            int value_count,
                                            const vir_dominance_t *dominance,
                                            const unsigned char *in_loop)
{
    int operand;
    int dependencies = 0;

    if (value->id < 0 || value->id >= value_count ||
        !vir_licm_value_is_candidate(value))
        return -1;
    for (operand = 0; operand < value->nr_ops; operand++) {
        vir_value_t *input = operand ? value->op1 : value->op0;
        int index = vir_dominance_index(dominance, input->block);

        if (index < 0)
            return -1;
        if (in_loop[index])
            dependencies++;
    }
    return dependencies;
}

/* Large sparse loops should go straight to the value worklist. Sample a
 * bounded, evenly spaced set of value-bearing blocks before selecting the eager
 * scan, so one ready root cannot make a long dependency chain look dense. At
 * least half of the sampled candidates must be ready. Inspect no more than the
 * fixed value limit in each selected block.
 */
static bool vir_licm_readiness_is_dense(vir_block_t **blocks,
                                        int count,
                                        int value_blocks,
                                        int value_count,
                                        const vir_dominance_t *dominance,
                                        const unsigned char *in_loop,
                                        int *sampled_values)
{
    int ordinal = 0;
    int sample_index = 0;
    int sample_ordinal = 0;
    int candidates = 0;
    int ready = 0;

    if (sampled_values)
        *sampled_values = 0;

    for (int i = 0; i < count; i++) {
        vir_value_t *value;

        if (!in_loop[i] || !blocks[i]->head)
            continue;
        if (ordinal == sample_ordinal) {
            int inspected = 0;

            for (value = blocks[i]->head;
                 value && inspected < VIR_LICM_READINESS_SAMPLE_VALUE_LIMIT;
                 value = value->next, inspected++) {
                int dependencies;

                if (sampled_values)
                    (*sampled_values)++;
                dependencies = vir_licm_value_loop_dependencies(
                    value, value_count, dominance, in_loop);
                if (dependencies < 0)
                    continue;
                candidates++;
                if (!dependencies)
                    ready++;
            }
            if (++sample_index == VIR_LICM_READINESS_SAMPLE_BLOCK_LIMIT)
                break;
            sample_ordinal =
                (int) ((uint64_t) sample_index * (uint64_t) (value_blocks - 1) /
                       (VIR_LICM_READINESS_SAMPLE_BLOCK_LIMIT - 1));
        }
        ordinal++;
    }
    return candidates > 0 && ready >= candidates / 2 + candidates % 2;
}

static void vir_licm_move_value(vir_block_t *from,
                                vir_block_t *to,
                                vir_value_t *value)
{
    vir_value_t **link = &from->head;

    while (*link != value)
        link = &(*link)->next;
    *link = value->next;
    value->next = NULL;
    if (to->tail)
        to->tail->next = value;
    else
        to->head = value;
    to->tail = value;
    value->block = to;
}

static void vir_licm_release(unsigned char *in_loop,
                             int *work,
                             int *pending,
                             vir_value_t **ready,
                             vir_dominance_t *dominance)
{
    free(in_loop);
    free(work);
    free(pending);
    free(ready);
    vir_dominance_release(dominance);
}

/* LICM borrows VIR's bitset dominance context for this pass only; the analysis
 * is released on every exit and is never stored in the IR.
 */
int vir_licm_with_stats(vir_function_t *func,
                        vir_opt_level_t opt_level,
                        int *stats)
{
    vir_dominance_t dominance;
    vir_block_t **blocks;
    unsigned char *in_loop;
    int *work;
    int *pending = NULL;
    vir_value_t **ready = NULL;
    int count;
    int value_count;
    int bytes;
    size_t temporary_bytes = 0;
    int hoisted = 0;
    int header_index;

    if (stats)
        memset(stats, 0, VIR_LICM_STAT_COUNT * sizeof(*stats));
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    memset(&dominance, 0, sizeof(dominance));
    if (opt_level == VIR_OPT_O0 || !(count = vir_function_block_count(func)))
        return 0;
    if (count > INT_MAX / (int) sizeof(*blocks) ||
        count > INT_MAX / (int) sizeof(*in_loop) ||
        count > INT_MAX / (int) sizeof(*work))
        return -1;
    if (!vir_dominance_init(func, &dominance))
        return -1;
    count = dominance.count;
    value_count = func->next_value_id;
    blocks = dominance.blocks;
    if (!vir_licm_add_temporary_bytes(
            &temporary_bytes, (size_t) count * sizeof(*dominance.blocks)) ||
        !vir_licm_add_temporary_bytes(
            &temporary_bytes,
            (size_t) dominance.id_count * sizeof(*dominance.indices)) ||
        !vir_licm_add_temporary_bytes(
            &temporary_bytes, (size_t) count * (size_t) dominance.words *
                                  sizeof(*dominance.dom)) ||
        !vir_licm_add_temporary_bytes(
            &temporary_bytes,
            (size_t) dominance.words * sizeof(*dominance.candidate)) ||
        !vir_licm_add_temporary_bytes(&temporary_bytes,
                                      (size_t) count * sizeof(*in_loop)) ||
        !vir_licm_add_temporary_bytes(&temporary_bytes,
                                      (size_t) count * sizeof(*work))) {
        vir_dominance_release(&dominance);
        return -1;
    }
    bytes = (int) temporary_bytes;
    in_loop = calloc(count, sizeof(*in_loop));
    work = malloc(count * sizeof(*work));
    if (!in_loop || !work) {
        vir_licm_release(in_loop, work, pending, ready, &dominance);
        return -1;
    }
    if (stats)
        stats[VIR_LICM_STAT_TEMPORARY_BYTES] = bytes;
    for (header_index = 0; header_index < count; header_index++) {
        vir_block_t *header = blocks[header_index];
        const vir_edge_t *edge;
        bool has_backedge = false;
        int value_blocks = 0;

        memset(in_loop, 0, count * sizeof(*in_loop));
        for (edge = header->incoming; edge; edge = edge->next_incoming) {
            int latch_index = vir_dominance_index(&dominance, edge->from);

            if (latch_index < 0 ||
                !vir_dominance_block_dominates(&dominance, header, edge->from))
                continue;
            vir_licm_mark_loop(header, edge->from, &dominance, in_loop, work,
                               &value_blocks);
            has_backedge = true;
        }
        if (!has_backedge)
            continue;
        vir_block_t *preheader =
            vir_licm_preheader(header, &dominance, in_loop);
        int ready_count = 0;
        int ready_head = 0;

        if (!preheader)
            continue;
        if (stats)
            stats[VIR_LICM_STAT_LOOPS]++;

        /* Concentrated loop values are cheap to try in source order. Hoist
         * ready values immediately, avoiding the value-ID queues when this pass
         * drains the candidates or finds no ready root.
         */
        bool eager_scan = value_blocks <= VIR_LICM_EAGER_VALUE_BLOCK_LIMIT;

        if (!eager_scan) {
            int sampled_values = 0;

            eager_scan = vir_licm_readiness_is_dense(
                blocks, count, value_blocks, value_count, &dominance, in_loop,
                stats ? &sampled_values : NULL);
            if (stats)
                stats[VIR_LICM_STAT_READINESS_SAMPLE_VALUES] += sampled_values;
            if (stats && eager_scan)
                stats[VIR_LICM_STAT_READINESS_EAGER_ADMISSIONS]++;
        }
        if (eager_scan) {
            int candidate_count = 0;
            int fast_hoisted = 0;

            for (int i = 0; i < count; i++) {
                vir_value_t *value;

                if (!in_loop[i])
                    continue;
                value = blocks[i]->head;
                while (value) {
                    vir_value_t *next = value->next;
                    int dependencies = vir_licm_value_loop_dependencies(
                        value, value_count, &dominance, in_loop);

                    if (dependencies >= 0) {
                        candidate_count++;
                        if (!dependencies) {
                            vir_licm_move_value(blocks[i], preheader, value);
                            hoisted++;
                            fast_hoisted++;
                        }
                    }
                    value = next;
                }
            }

            if (!candidate_count || fast_hoisted == candidate_count) {
                if (fast_hoisted) {
                    for (int i = 0; i < count; i++)
                        if (in_loop[i])
                            vir_repair_value_positions(blocks[i]);
                    vir_repair_value_positions(preheader);
                }
                continue;
            }
            if (!fast_hoisted)
                continue;
        }
        if (!pending && value_count > 0) {
            if (value_count > INT_MAX / (int) sizeof(*pending) ||
                value_count > INT_MAX / (int) sizeof(*ready) ||
                !vir_licm_add_temporary_bytes(
                    &temporary_bytes,
                    (size_t) value_count * sizeof(*pending)) ||
                !vir_licm_add_temporary_bytes(
                    &temporary_bytes, (size_t) value_count * sizeof(*ready))) {
                vir_licm_release(in_loop, work, pending, ready, &dominance);
                return -1;
            }
            pending = malloc((size_t) value_count * sizeof(*pending));
            ready = malloc((size_t) value_count * sizeof(*ready));
            if (!pending || !ready) {
                vir_licm_release(in_loop, work, pending, ready, &dominance);
                return -1;
            }
            bytes = (int) temporary_bytes;
            if (stats)
                stats[VIR_LICM_STAT_TEMPORARY_BYTES] = bytes;
            if (stats)
                stats[VIR_LICM_STAT_VALUE_WORKLIST_ALLOCS]++;
        }
        for (int i = 0; i < count; i++) {
            vir_value_t *value;

            if (!in_loop[i])
                continue;
            for (value = blocks[i]->head; value; value = value->next) {
                int dependencies;

                if (value->id < 0 || value->id >= value_count)
                    continue;
                dependencies = vir_licm_value_loop_dependencies(
                    value, value_count, &dominance, in_loop);
                pending[value->id] = dependencies;
                if (dependencies == 0)
                    ready[ready_count++] = value;
            }
        }
        while (ready_head < ready_count) {
            vir_value_t *value = ready[ready_head++];
            vir_use_t *use;

            vir_licm_move_value(value->block, preheader, value);
            hoisted++;
            for (use = value->uses; use; use = use->next) {
                vir_value_t *user = use->user;
                int user_block;

                if (!user || user->id < 0 || user->id >= value_count)
                    continue;
                user_block = vir_dominance_index(&dominance, user->block);
                if (user_block < 0 || !in_loop[user_block])
                    continue;
                if (pending[user->id] <= 0)
                    continue;
                if (--pending[user->id] == 0)
                    ready[ready_count++] = user;
            }
        }
        for (int i = 0; i < count; i++)
            if (in_loop[i])
                vir_repair_value_positions(blocks[i]);
        vir_repair_value_positions(preheader);
    }
    vir_licm_release(in_loop, work, pending, ready, &dominance);
    if (stats)
        stats[VIR_LICM_STAT_VALUES_HOISTED] = hoisted;
    return hoisted;
}

int vir_licm(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_licm_with_stats(func, opt_level, NULL);
}

static bool vir_verify_reverse_uses(const vir_function_t *func,
                                    const vir_value_t *value)
{
    const vir_use_t *use;
    for (use = value->uses; use; use = use->next) {
        if (!!use->user + !!use->edge + !!use->effect + !!use->return_block +
                !!use->branch_block !=
            1)
            return 0;
        if (use->user) {
            if (!vir_has_value(func, use->user) ||
                (use->operand != 0 && use->operand != 1) ||
                (use->operand == 0 ? use->user->op0 : use->user->op1) != value)
                return 0;
        } else if (use->edge) {
            if (!vir_has_block(func, use->edge->from) ||
                !vir_has_block(func, use->edge->to) ||
                !vir_has_outgoing_edge(use->edge->from, use->edge) ||
                use->operand < 0 || use->operand >= use->edge->arg_count ||
                use->edge->args[use->operand] != value)
                return 0;
        } else if (use->effect) {
            const vir_effect_t *effect = use->effect;
            const vir_value_t *effect_value;
            if (!vir_has_block(func, effect->block) ||
                !vir_has_effect(effect->block, effect))
                return 0;
            if (effect->kind == VIR_EFFECT_CALL) {
                if (use->operand == -1) {
                    if (effect->callee_value_use != use ||
                        effect->callee_value_use->effect != effect ||
                        effect->callee_value_use->operand != -1)
                        return 0;
                    effect_value = effect->callee_value;
                } else {
                    if (use->operand < 0 || use->operand >= effect->arg_count ||
                        !effect->args || !effect->arg_uses ||
                        effect->arg_uses[use->operand].effect != effect ||
                        effect->arg_uses[use->operand].operand != use->operand)
                        return 0;
                    effect_value = effect->args[use->operand];
                }
            } else {
                if (use->operand != 0 && use->operand != 1)
                    return 0;
                effect_value =
                    use->operand == 0 ? effect->address : effect->stored_value;
            }
            if (effect_value != value)
                return 0;
        } else if (use->return_block) {
            if (!vir_has_block(func, use->return_block) ||
                use->return_block->terminator != VIR_TERM_RETURN ||
                use->return_block->return_value != value)
                return 0;
        } else if (!vir_has_block(func, use->branch_block) ||
                   use->branch_block->terminator != VIR_TERM_BRANCH ||
                   use->branch_block->branch_condition != value) {
            return 0;
        }
    }
    return 1;
}

static int vir_verify_error(char **error, char *message)
{
    if (error)
        *error = message;
    return 0;
}

static int vir_verify_invalid_value(char **error)
{
    return vir_verify_error(error, "invalid value/use structure");
}

static int vir_verify_invalid_order(char **error)
{
    return vir_verify_error(error, "invalid block order");
}

static bool vir_has_no_address_metadata(const vir_value_t *value)
{
    return value->address_kind == VIR_ADDRESS_NONE && !value->address_slot &&
           !value->address_size && !value->address_alignment &&
           !value->address_name;
}

static bool vir_effect_has_call_metadata(const vir_effect_t *effect)
{
    return effect->callee || effect->callee_value || effect->signature ||
           effect->callee_value_use || effect->args || effect->arg_uses ||
           effect->arg_count || effect->result_type;
}

static bool vir_verify_address_value(const vir_value_t *value)
{
    if (value->type != VIR_TYPE_PTR || value->constant || value->op0 ||
        value->op1 || value->def_effect || !value->address_size ||
        !vir_valid_alignment(value->address_alignment))
        return 0;
    if (value->opcode == VIR_OP_STACK_ADDR)
        return value->address_kind == VIR_ADDRESS_STACK && !value->address_name;
    return value->opcode == VIR_OP_GLOBAL_ADDR &&
           value->address_kind == VIR_ADDRESS_GLOBAL && !value->address_slot &&
           value->address_name && *value->address_name;
}

static bool vir_verify_effect_operand(const vir_function_t *func,
                                      const vir_dominance_t *dominance,
                                      const vir_effect_t *effect,
                                      const vir_value_t *value,
                                      const vir_use_t *use,
                                      int operand)
{
    return value && vir_has_value(func, value) && use &&
           use->effect == effect && use->operand == operand &&
           vir_value_dominates_effect(func, dominance, value, effect) &&
           vir_effect_use_count(value, effect, operand) == 1;
}

static bool vir_verify_value_operands(const vir_function_t *func,
                                      const vir_dominance_t *dominance,
                                      const vir_value_t *value)
{
    if (value->nr_ops < 1 || value->nr_ops > 2)
        return false;
    for (int i = 0; i < value->nr_ops; i++) {
        const vir_value_t *operand = i ? value->op1 : value->op0;

        if (!operand || !vir_has_value(func, operand) ||
            !vir_type_width(func, operand->type) ||
            vir_use_count(operand, value, i) != 1 ||
            !vir_value_dominates(func, dominance, operand, value->block,
                                 value->position))
            return false;
    }
    return vir_type_width(func, value->type) &&
           (value->nr_ops == 1
                ? vir_unary_types_valid(func, value->opcode, value->type,
                                        value->op0->type)
                : vir_binary_types_valid(func, value->opcode, value->type,
                                         value->op0->type, value->op1->type));
}

static int vir_verify_block(const vir_function_t *func,
                            const vir_dominance_t *dominance,
                            const vir_block_t *block,
                            char **error)
{
    const vir_value_t *value;
    const vir_edge_t *edge;
    const vir_effect_t *effect;
    int position = 0;
    int terminator_position;
    int param_count = 0;
    if (block->id < 0 || vir_block_id_count(func, block->id) != 1)
        return vir_verify_invalid_order(error);
    for (value = block->params; value; value = value->param_next) {
        if (value->block != block || !value->is_block_param || value->nr_ops ||
            !vir_type_width(func, value->type) || value->id < 0 ||
            !vir_has_no_address_metadata(value) ||
            vir_value_id_count(func, value->id) != 1)
            return vir_verify_invalid_value(error);
        if (!vir_verify_reverse_uses(func, value))
            return vir_verify_invalid_value(error);
        param_count++;
    }
    if ((block->params == NULL) != (block->last_param == NULL) ||
        (block->last_param && block->last_param->param_next) ||
        block->param_count != param_count)
        return vir_verify_invalid_order(error);
    for (value = block->head; value; value = value->next) {
        if (value->block != block || !vir_has_block(func, value->block) ||
            value->is_block_param || value->position != position ||
            value->id < 0 || value->order < 0 ||
            value->order >= block->next_order)
            return vir_verify_invalid_value(error);
        position++;
        if (vir_value_id_count(func, value->id) != 1)
            return vir_verify_invalid_value(error);
        if (value->opcode != VIR_OP_STACK_ADDR &&
            value->opcode != VIR_OP_GLOBAL_ADDR &&
            value->opcode != VIR_OP_FUNC_ADDR &&
            !vir_has_no_address_metadata(value))
            return vir_verify_invalid_value(error);
        if (value->nr_ops == 0) {
            if (value->opcode == VIR_OP_CONST) {
                if (!vir_type_width(func, value->type) ||
                    value->constant !=
                        (value->constant &
                         vir_width_mask(vir_type_width(func, value->type))))
                    return vir_verify_invalid_value(error);
            } else if (value->opcode == VIR_OP_FUNC_ADDR) {
                if (value->type != VIR_TYPE_PTR || value->def_effect ||
                    value->constant || value->address_kind ||
                    value->address_slot || value->address_size ||
                    value->address_alignment || !value->address_name ||
                    !*value->address_name)
                    return vir_verify_invalid_value(error);
            } else if (value->opcode == VIR_OP_RODATA_ADDR) {
                if (value->type != VIR_TYPE_PTR || value->def_effect)
                    return vir_verify_invalid_value(error);
            } else if (value->opcode == VIR_OP_STACK_ADDR ||
                       value->opcode == VIR_OP_GLOBAL_ADDR) {
                if (!vir_verify_address_value(value) ||
                    vir_address_root_conflicts(
                        func, value->address_kind, value->address_slot,
                        value->address_name, value->address_size,
                        value->address_alignment))
                    return vir_verify_invalid_value(error);
            } else if ((value->opcode != VIR_OP_LOAD &&
                        value->opcode != VIR_OP_CALL) ||
                       !value->def_effect ||
                       (value->opcode == VIR_OP_LOAD
                            ? value->def_effect->kind != VIR_EFFECT_LOAD &&
                                  value->def_effect->kind !=
                                      VIR_EFFECT_VOLATILE_LOAD
                            : value->def_effect->kind != VIR_EFFECT_CALL) ||
                       value->def_effect->block != block ||
                       value->def_effect->result != value ||
                       (value->opcode == VIR_OP_LOAD &&
                        (!value->def_effect->address ||
                         value->def_effect->address->type != VIR_TYPE_PTR)) ||
                       (value->opcode == VIR_OP_CALL &&
                        value->def_effect->result_type != value->type) ||
                       !vir_type_width(func, value->type)) {
                return vir_verify_invalid_value(error);
            }
        } else if (!vir_verify_value_operands(func, dominance, value)) {
            return vir_verify_invalid_value(error);
        }
        if (!vir_verify_reverse_uses(func, value))
            return vir_verify_invalid_value(error);
    }
    if (block->tail && block->tail->next)
        return vir_verify_invalid_order(error);
    {
        const vir_value_t *ordered_value = block->head;
        const vir_effect_t *ordered_effect = block->effects;
        int order = 0;

        while (ordered_value || ordered_effect) {
            bool paired = ordered_value && ordered_effect &&
                          ordered_value->order == ordered_effect->order;

            if (paired) {
                if (ordered_value->order != order ||
                    ordered_value->def_effect != ordered_effect ||
                    ordered_effect->result != ordered_value)
                    return vir_verify_invalid_order(error);
                ordered_value = ordered_value->next;
                ordered_effect = ordered_effect->next;
            } else if (ordered_effect &&
                       (!ordered_value ||
                        ordered_effect->order < ordered_value->order)) {
                if (ordered_effect->order != order || ordered_effect->result)
                    return vir_verify_invalid_order(error);
                ordered_effect = ordered_effect->next;
            } else {
                if (ordered_value->order != order || ordered_value->def_effect)
                    return vir_verify_invalid_order(error);
                ordered_value = ordered_value->next;
            }
            order++;
        }
        if (order != block->next_order)
            return vir_verify_invalid_order(error);
    }
    terminator_position = position;
    position = 0;
    for (effect = block->effects; effect; effect = effect->next) {
        if (effect->block != block || effect->position != position++ ||
            effect->order < 0 || effect->order >= block->next_order)
            return vir_verify_invalid_value(error);
        if (vir_effect_is_load(effect->kind)) {
            if (!effect->address || !effect->result || effect->stored_value ||
                effect->stored_value_use ||
                vir_effect_has_call_metadata(effect) ||
                !vir_has_value(func, effect->result) ||
                effect->address->type != VIR_TYPE_PTR ||
                effect->result->def_effect != effect ||
                !vir_verify_effect_operand(func, dominance, effect,
                                           effect->address, effect->address_use,
                                           0))
                return vir_verify_invalid_value(error);
        } else if (vir_effect_is_store(effect->kind)) {
            if (!effect->address || !effect->stored_value || effect->result ||
                vir_effect_has_call_metadata(effect) ||
                effect->address->type != VIR_TYPE_PTR ||
                !vir_verify_effect_operand(func, dominance, effect,
                                           effect->address, effect->address_use,
                                           0) ||
                !vir_verify_effect_operand(func, dominance, effect,
                                           effect->stored_value,
                                           effect->stored_value_use, 1))
                return vir_verify_invalid_value(error);
        } else if (effect->kind == VIR_EFFECT_CALL) {
            int operand;
            if (effect->address || effect->stored_value ||
                effect->address_use || effect->stored_value_use)
                return vir_verify_invalid_value(error);
            if (vir_effect_has_call_metadata(effect) || effect->result) {
                if ((effect->callee != NULL) ==
                        (effect->callee_value != NULL) ||
                    (effect->callee && !*effect->callee) ||
                    !vir_call_shape_valid(effect, INT_MAX) ||
                    (effect->result_type != VIR_TYPE_VOID &&
                     !vir_type_width(func, effect->result_type)) ||
                    (effect->result && effect->result->def_effect != effect))
                    return vir_verify_invalid_value(error);
                if (effect->callee_value) {
                    if (effect->callee_value->type != VIR_TYPE_PTR ||
                        !vir_verify_effect_operand(
                            func, dominance, effect, effect->callee_value,
                            effect->callee_value_use, -1))
                        return vir_verify_error(error, "invalid call target");
                } else if (effect->callee_value_use) {
                    return vir_verify_invalid_value(error);
                }
                if (effect->signature && !vir_call_signature_matches(
                                             func, effect, effect->signature))
                    return vir_verify_error(error,
                                            "invalid indirect call signature");
                for (operand = 0; operand < effect->arg_count; operand++)
                    if (!vir_verify_effect_operand(
                            func, dominance, effect, effect->args[operand],
                            &effect->arg_uses[operand], operand))
                        return vir_verify_invalid_value(error);
            }
        } else if (effect->kind == VIR_EFFECT_VOLATILE) {
            if (effect->address || effect->stored_value || effect->result ||
                effect->address_use || effect->stored_value_use ||
                vir_effect_has_call_metadata(effect))
                return vir_verify_invalid_value(error);
        } else {
            return vir_verify_invalid_value(error);
        }
    }
    if ((block->effects == NULL) != (block->last_effect == NULL) ||
        (block->last_effect && block->last_effect->next))
        return vir_verify_invalid_order(error);
    if (block->terminator == VIR_TERM_NONE)
        return vir_verify_invalid_order(error);
    if (block->terminator == VIR_TERM_JUMP &&
        (!block->outgoing || block->outgoing->next_outgoing ||
         block->return_value || block->branch_condition || block->true_edge ||
         block->false_edge))
        return vir_verify_invalid_order(error);
    if (block->terminator == VIR_TERM_BRANCH) {
        if (!block->branch_condition ||
            !vir_has_value(func, block->branch_condition) ||
            block->branch_condition->type != VIR_TYPE_I1 || !block->true_edge ||
            !block->false_edge || block->true_edge == block->false_edge ||
            block->true_edge->from != block ||
            block->false_edge->from != block ||
            !vir_has_outgoing_edge(block, block->true_edge) ||
            !vir_has_outgoing_edge(block, block->false_edge) ||
            !block->outgoing || !block->outgoing->next_outgoing ||
            block->outgoing->next_outgoing->next_outgoing ||
            vir_branch_use_count(block->branch_condition, block) != 1 ||
            !vir_value_dominates(func, dominance, block->branch_condition,
                                 block, terminator_position))
            return vir_verify_invalid_value(error);
    }
    if (block->outgoing && block->terminator != VIR_TERM_JUMP &&
        block->terminator != VIR_TERM_BRANCH)
        return vir_verify_invalid_order(error);
    if (block->terminator == VIR_TERM_RETURN &&
        (block->outgoing ||
         (block->return_value &&
          (!vir_has_value(func, block->return_value) ||
           vir_return_use_count(block->return_value, block) != 1 ||
           !vir_value_dominates(func, dominance, block->return_value, block,
                                terminator_position)))))
        return vir_verify_invalid_value(error);
    for (edge = block->outgoing; edge; edge = edge->next_outgoing) {
        int i;
        vir_value_t *param;
        if (edge->from != block || !vir_has_block(func, edge->to) ||
            !vir_has_incoming_edge(edge->to, edge) ||
            edge->arg_count != edge->to->param_count)
            return vir_verify_invalid_order(error);
        param = edge->to->params;
        for (i = 0; i < edge->arg_count; i++) {
            if (!edge->args[i] || !vir_has_value(func, edge->args[i]) ||
                !param || edge->args[i]->type != param->type ||
                vir_edge_use_count(edge->args[i], edge, i) != 1 ||
                !vir_value_dominates(func, dominance, edge->args[i], block,
                                     terminator_position))
                return vir_verify_invalid_value(error);
            param = param->param_next;
        }
    }
    for (edge = block->incoming; edge; edge = edge->next_incoming)
        if (edge->to != block || !vir_has_block(func, edge->from) ||
            !vir_has_outgoing_edge(edge->from, edge))
            return vir_verify_invalid_order(error);
    return 1;
}

int vir_verify(const vir_function_t *func, char **error)
{
    const vir_block_t *block;
    vir_dominance_t dominance;

    if (!func || !func->blocks || !vir_lists_are_acyclic(func))
        return vir_verify_invalid_order(error);
    if (!vir_dominance_init(func, &dominance))
        return vir_verify_invalid_order(error);
    for (block = func->blocks; block; block = block->next)
        if (!vir_verify_block(func, &dominance, block, error)) {
            vir_dominance_release(&dominance);
            return 0;
        }
    vir_dominance_release(&dominance);
    if (error)
        *error = NULL;
    return 1;
}

typedef struct vir_ssa_binding {
    vir_block_t *block;
    unsigned int variable;
    vir_value_t *value;
    vir_value_t *param;
    struct vir_ssa_binding *next;
} vir_ssa_binding_t;

typedef struct vir_ssa_block_state {
    vir_block_t *block;
    int sealed;
    struct vir_ssa_block_state *next;
} vir_ssa_block_state_t;

struct vir_ssa {
    vir_function_t *func;
    vir_ssa_binding_t *bindings;
    vir_ssa_block_state_t *blocks;
};

bool vir_ssa_discard_variable(vir_ssa_t *ssa, unsigned int variable)
{
    vir_ssa_binding_t **link;

    if (!ssa)
        return false;
    for (vir_ssa_binding_t *binding = ssa->bindings; binding;
         binding = binding->next)
        if (binding->variable == variable && binding->param &&
            (binding->param->uses || binding->block->incoming))
            return false;
    link = &ssa->bindings;
    while (*link) {
        vir_ssa_binding_t *binding = *link;
        if (binding->variable != variable) {
            link = &binding->next;
            continue;
        }
        if (binding->param) {
            int index = 0;
            vir_param_edit_t edit;
            for (vir_value_t *param = binding->block->params;
                 param && param != binding->param; param = param->param_next)
                index++;
            if (!vir_param_edit_prepare(
                    ssa->func, binding->block, binding->param,
                    binding->block->param_count, index, false, &edit))
                return false;
            vir_param_edit_apply(binding->block, binding->param, &edit);
            vir_param_edit_release(&edit);
        }
        *link = binding->next;
        free(binding);
    }
    return true;
}

static vir_ssa_binding_t *vir_ssa_find_binding(const vir_ssa_t *ssa,
                                               const vir_block_t *block,
                                               unsigned int variable,
                                               const vir_value_t *param)
{
    vir_ssa_binding_t *binding;
    for (binding = ssa->bindings; binding; binding = binding->next)
        if (binding->block == block &&
            (param ? binding->param == param : binding->variable == variable))
            return binding;
    return NULL;
}

static vir_ssa_block_state_t *vir_ssa_block_state(vir_ssa_t *ssa,
                                                  vir_block_t *block,
                                                  int create)
{
    vir_ssa_block_state_t *state;
    for (state = ssa->blocks; state; state = state->next)
        if (state->block == block)
            return state;
    if (!create)
        return NULL;
    state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;
    state->block = block;
    state->next = ssa->blocks;
    ssa->blocks = state;
    return state;
}

static bool vir_ssa_is_sealed(vir_ssa_t *ssa, vir_block_t *block)
{
    vir_ssa_block_state_t *state = vir_ssa_block_state(ssa, block, 0);

    return state && state->sealed;
}

vir_ssa_t *vir_ssa_create(vir_function_t *func)
{
    vir_ssa_t *ssa;
    if (!func)
        return NULL;
    ssa = calloc(1, sizeof(*ssa));
    if (ssa)
        ssa->func = func;
    return ssa;
}

void vir_ssa_release(vir_ssa_t *ssa)
{
    vir_ssa_binding_t *binding;
    vir_ssa_block_state_t *state;
    if (!ssa)
        return;
    while ((binding = ssa->bindings)) {
        ssa->bindings = binding->next;
        free(binding);
    }
    while ((state = ssa->blocks)) {
        ssa->blocks = state->next;
        free(state);
    }
    free(ssa);
}

int vir_ssa_write(vir_ssa_t *ssa,
                  vir_block_t *block,
                  unsigned int variable,
                  vir_value_t *value)
{
    vir_ssa_binding_t *binding;
    if (!ssa || !value || !vir_has_block(ssa->func, block) ||
        !vir_has_value(ssa->func, value))
        return 0;
    if (!vir_value_dominates(ssa->func, NULL, value, block,
                             block->tail ? block->tail->position + 1 : 0))
        return 0;
    binding = vir_ssa_find_binding(ssa, block, variable, NULL);
    if (!binding) {
        binding = calloc(1, sizeof(*binding));
        if (!binding)
            return 0;
        binding->block = block;
        binding->variable = variable;
        binding->next = ssa->bindings;
        ssa->bindings = binding;
    }
    binding->value = value;
    return 1;
}

vir_value_t *vir_ssa_read(const vir_ssa_t *ssa,
                          const vir_block_t *block,
                          unsigned int variable)
{
    vir_ssa_binding_t *binding;

    if (!ssa || !vir_has_block(ssa->func, block))
        return NULL;
    binding = vir_ssa_find_binding(ssa, block, variable, NULL);
    return binding ? binding->value : NULL;
}

bool vir_ssa_is_param(const vir_ssa_t *ssa,
                      const vir_block_t *block,
                      unsigned int variable)
{
    vir_ssa_binding_t *binding;

    if (!ssa || !vir_has_block(ssa->func, block))
        return 0;
    binding = vir_ssa_find_binding(ssa, block, variable, NULL);
    return binding && binding->param && binding->value == binding->param;
}

static vir_value_t *vir_ssa_read_sealed_impl(vir_ssa_t *ssa,
                                             vir_block_t *block,
                                             unsigned int variable,
                                             unsigned char *visited)
{
    vir_ssa_binding_t *binding;
    vir_value_t *value;
    int index;

    if (!vir_has_block(ssa->func, block))
        return NULL;
    index = vir_block_index(ssa->func, block);
    if (index < 0 || visited[index])
        return NULL;
    visited[index] = 1;
    binding = vir_ssa_find_binding(ssa, block, variable, NULL);
    if (binding)
        return binding->value;
    if (!vir_ssa_is_sealed(ssa, block) || !block->incoming ||
        block->incoming->next_incoming)
        return NULL;
    value =
        vir_ssa_read_sealed_impl(ssa, block->incoming->from, variable, visited);
    if (!value || !vir_ssa_write(ssa, block, variable, value))
        return NULL;
    return value;
}

vir_value_t *vir_ssa_read_sealed(vir_ssa_t *ssa,
                                 vir_block_t *block,
                                 unsigned int variable)
{
    unsigned char *visited;
    vir_value_t *value;
    int count;

    if (!ssa || !block)
        return NULL;
    count = vir_function_block_count(ssa->func);
    visited = calloc((size_t) count, sizeof(*visited));
    if (!visited)
        return NULL;
    value = vir_ssa_read_sealed_impl(ssa, block, variable, visited);
    free(visited);
    return value;
}

vir_value_t *vir_ssa_predeclare(vir_ssa_t *ssa,
                                vir_block_t *block,
                                unsigned int variable,
                                vir_type_t type)
{
    vir_ssa_binding_t *binding;
    vir_ssa_block_state_t *state;
    vir_value_t *value;
    vir_param_edit_t edit = {0};
    vir_edge_t *edge;
    int i = 0;

    if (!ssa || !vir_has_block(ssa->func, block) ||
        !vir_type_width(ssa->func, type) || vir_ssa_read(ssa, block, variable))
        return NULL;
    state = vir_ssa_block_state(ssa, block, 1);
    if (!state || state->sealed)
        return NULL;
    binding = calloc(1, sizeof(*binding));
    if (!binding)
        return NULL;
    if (!block->incoming) {
        value = vir_block_add_param(ssa->func, block, type);
        if (!value)
            goto fail;
    } else {
        value = vir_arena_alloc(&ssa->func->arena, sizeof(*value));
        if (!value ||
            !vir_param_edit_prepare(ssa->func, block, NULL, block->param_count,
                                    block->param_count, true, &edit))
            goto fail;
        value->type = type;
        for (edge = block->incoming; edge; edge = edge->next_incoming, i++) {
            vir_value_t *incoming = vir_ssa_read(ssa, edge->from, variable);
            if (!incoming || incoming->type != type ||
                edge->arg_count != block->param_count)
                goto fail;
            edit.edges[i].inserted = incoming;
        }
        value->id = ssa->func->next_value_id++;
        vir_param_edit_apply(block, value, &edit);
    }
    binding->block = block;
    binding->variable = variable;
    binding->value = value;
    binding->param = value;
    binding->next = ssa->bindings;
    ssa->bindings = binding;
    vir_param_edit_release(&edit);
    return value;

fail:
    vir_param_edit_release(&edit);
    free(binding);
    return NULL;
}

int vir_ssa_seal(vir_ssa_t *ssa, vir_block_t *block)
{
    typedef struct {
        vir_value_t *param;
        vir_value_t *candidate;
        vir_ssa_binding_t *binding;
        vir_param_edit_t edit;
        int index;
    } vir_ssa_trivial_param_t;
    vir_ssa_block_state_t *state;
    vir_value_t *param;
    vir_ssa_trivial_param_t *trivial_params;
    int param_count;
    int trivial_count = 0;
    int index;
    int i;

    if (!ssa || !vir_has_block(ssa->func, block))
        return 0;
    state = vir_ssa_block_state(ssa, block, 1);
    if (!state || state->sealed)
        return 0;
    param_count = block->param_count;
    trivial_params = param_count
                         ? calloc((size_t) param_count, sizeof(*trivial_params))
                         : NULL;
    if (param_count && !trivial_params)
        return 0;
    for (param = block->params, index = 0; param;
         param = param->param_next, index++) {
        vir_value_t *candidate = NULL;
        vir_edge_t *edge;
        int trivial = 1;

        if (!vir_ssa_find_binding(ssa, block, 0, param))
            goto fail;

        /* A back edge that passes the parameter to itself adds no value, so a
         * loop-invariant parameter is trivial too: its only other incoming
         * value is the one it always holds.
         */
        for (edge = block->incoming; edge; edge = edge->next_incoming) {
            if (edge->arg_count != param_count)
                goto fail;
            if (edge->args[index] == param)
                continue;
            if (!candidate)
                candidate = edge->args[index];
            else if (candidate != edge->args[index])
                trivial = 0;
        }
        if (candidate && candidate != param && trivial) {
            if (!vir_replacement_dominates(ssa->func, param, candidate))
                goto fail;
            trivial_params[trivial_count].param = param;
            trivial_params[trivial_count].candidate = candidate;
            trivial_params[trivial_count].binding =
                vir_ssa_find_binding(ssa, block, 0, param);
            trivial_params[trivial_count].index = index;
            trivial_count++;
        }
    }
    for (i = 0; i < trivial_count; i++)
        if (!vir_param_edit_prepare(
                ssa->func, block, trivial_params[i].param, param_count - i,
                trivial_params[i].index - i, false, &trivial_params[i].edit))
            goto fail;
    for (i = 0; i < trivial_count; i++) {
        vir_replace_all_uses_unchecked(trivial_params[i].param,
                                       trivial_params[i].candidate);
        vir_param_edit_apply(block, trivial_params[i].param,
                             &trivial_params[i].edit);
        trivial_params[i].binding->value = trivial_params[i].candidate;
        trivial_params[i].binding->param = NULL;
    }
    for (i = 0; i < trivial_count; i++)
        vir_param_edit_release(&trivial_params[i].edit);
    free(trivial_params);
    state->sealed = 1;
    return 1;

fail:
    for (i = 0; i < trivial_count; i++)
        vir_param_edit_release(&trivial_params[i].edit);
    free(trivial_params);
    return 0;
}

static bool vir_ssa_edge_args(const vir_ssa_t *ssa,
                              const vir_block_t *from,
                              const vir_block_t *to,
                              vir_value_t ***args)
{
    int i = 0;

    if (!args)
        return false;
    *args = NULL;
    if (!ssa || !from || !to || to->param_count < 0)
        return false;
    if (to->param_count &&
        !(*args = calloc((size_t) to->param_count, sizeof(**args))))
        return false;
    for (const vir_value_t *param = to->params; param;
         param = param->param_next) {
        vir_ssa_binding_t *binding = vir_ssa_find_binding(ssa, to, 0, param);

        if (i >= to->param_count || !binding ||
            !((*args)[i] = vir_ssa_read(ssa, from, binding->variable)))
            goto fail;
        i++;
    }
    if (i == to->param_count)
        return true;
fail:
    free(*args);
    *args = NULL;
    return false;
}

vir_edge_t *vir_ssa_jump(vir_ssa_t *ssa, vir_block_t *from, vir_block_t *to)
{
    vir_value_t **args;
    int count = to ? to->param_count : 0;
    vir_edge_t *edge;
    if (!ssa || !from || !to || !vir_has_block(ssa->func, from) ||
        !vir_has_block(ssa->func, to) || vir_ssa_is_sealed(ssa, to))
        return NULL;
    if (!vir_ssa_edge_args(ssa, from, to, &args))
        return NULL;
    edge = vir_edge_create(ssa->func, from, to, args, count);
    free(args);
    return edge;
}

int vir_ssa_branch(vir_ssa_t *ssa,
                   vir_block_t *from,
                   vir_value_t *condition,
                   vir_block_t *true_to,
                   vir_block_t *false_to)
{
    vir_value_t **true_args = NULL;
    vir_value_t **false_args = NULL;
    int true_count;
    int false_count;
    int result = 0;
    if (!ssa || !from || !condition || !true_to || !false_to ||
        !vir_has_block(ssa->func, from) || !vir_has_block(ssa->func, true_to) ||
        !vir_has_block(ssa->func, false_to) ||
        vir_ssa_is_sealed(ssa, true_to) || vir_ssa_is_sealed(ssa, false_to))
        return 0;
    true_count = true_to->param_count;
    false_count = false_to->param_count;
    if (!vir_ssa_edge_args(ssa, from, true_to, &true_args) ||
        !vir_ssa_edge_args(ssa, from, false_to, &false_args))
        goto out;
    {
        vir_edge_args_t t = {true_to, true_args, true_count};
        vir_edge_args_t f = {false_to, false_args, false_count};
        result = vir_block_set_branch(ssa->func, from, condition, &t, &f);
    }
out:
    free(true_args);
    free(false_args);
    return result;
}

static void vir_print_call_abi_type(const vir_call_abi_type_t *type, FILE *out)
{
    fprintf(out, "%s%s", vir_type_name(type->type),
            type->is_bool       ? ".bool"
            : type->is_unsigned ? ".unsigned"
                                : "");
}

static void vir_print_values(vir_value_t *const *values, int count, FILE *out)
{
    int i;

    for (i = 0; i < count; i++)
        fprintf(out, "%s%%d%d", i ? ", " : "", values[i]->id);
}

static void vir_print_call(const vir_effect_t *effect, FILE *out)
{
    if (effect->result)
        fprintf(out, "  %%d%d = call.%s ", effect->result->id,
                vir_type_name(effect->result->type));
    else
        fprintf(out, "  call%s ", effect->callee_value ? "_indirect" : "");
    if (effect->callee_value)
        fprintf(out, "%%d%d(", effect->callee_value->id);
    else
        fprintf(out, "@%s(", effect->callee);
    vir_print_values(effect->args, effect->arg_count, out);
    fputc(')', out);
    if (effect->signature) {
        fprintf(out, " sig(");
        for (int i = 0; i < effect->signature->param_count; i++) {
            fprintf(out, "%s", i ? ", " : "");
            vir_print_call_abi_type(&effect->signature->params[i], out);
        }
        fprintf(out, ")->");
        vir_print_call_abi_type(&effect->signature->result, out);
    }
    fputc('\n', out);
}

static void vir_print_value(const vir_value_t *value, FILE *out)
{
    if (value->opcode == VIR_OP_CONST)
        fprintf(out, "  %%d%d = const.%s %llu\n", value->id,
                vir_type_name(value->type), value->constant);
    else if (value->opcode == VIR_OP_STACK_ADDR)
        fprintf(out, "  %%d%d = stackaddr %u, %u, align %u\n", value->id,
                value->address_slot, value->address_size,
                value->address_alignment);
    else if (value->opcode == VIR_OP_GLOBAL_ADDR)
        fprintf(out, "  %%d%d = globaladdr @%s, %u, align %u\n", value->id,
                value->address_name, value->address_size,
                value->address_alignment);
    else if (value->opcode == VIR_OP_FUNC_ADDR)
        fprintf(out, "  %%d%d = funcaddr @%s\n", value->id,
                value->address_name);
    else if (value->opcode == VIR_OP_RODATA_ADDR)
        fprintf(out, "  %%d%d = rodataaddr %llu\n", value->id, value->constant);
    else if (value->opcode == VIR_OP_LOAD && value->def_effect->address)
        fprintf(out, "  %%d%d = %s.%s %%d%d\n", value->id,
                value->def_effect->kind == VIR_EFFECT_VOLATILE_LOAD
                    ? "volatile.load"
                    : "load",
                vir_type_name(value->type), value->def_effect->address->id);
    else if (value->opcode == VIR_OP_CALL)
        vir_print_call(value->def_effect, out);
    else if (value->opcode == VIR_OP_LOAD)
        fprintf(out, "  %%d%d = load.%s <unresolved>\n", value->id,
                vir_type_name(value->type));
    else if (value->nr_ops == 1)
        fprintf(out, "  %%d%d = %s.%s.%s %%d%d\n", value->id,
                vir_opcode_name(value->opcode), vir_type_name(value->op0->type),
                vir_type_name(value->type), value->op0->id);
    else
        fprintf(out, "  %%d%d = %s %%d%d, %%d%d\n", value->id,
                vir_opcode_name(value->opcode), value->op0->id, value->op1->id);
}

static void vir_print_effect(const vir_effect_t *effect, FILE *out)
{
    if (vir_effect_is_store(effect->kind) && effect->address)
        fprintf(out, "  %s %%d%d, %%d%d\n",
                effect->kind == VIR_EFFECT_VOLATILE_STORE ? "volatile.store"
                                                          : "store",
                effect->address->id, effect->stored_value->id);
    else if (effect->kind == VIR_EFFECT_CALL && !effect->result &&
             (effect->callee || effect->callee_value))
        vir_print_call(effect, out);
    else
        fprintf(out, "  effect.%s\n",
                effect->kind == VIR_EFFECT_STORE            ? "store"
                : effect->kind == VIR_EFFECT_CALL           ? "call"
                : effect->kind == VIR_EFFECT_VOLATILE_STORE ? "volatile.store"
                                                            : "volatile");
}

void vir_print(const vir_function_t *func, FILE *out)
{
    const vir_block_t *block;
    for (block = func->blocks; block; block = block->next) {
        const vir_value_t *value;
        const vir_edge_t *edge;
        const vir_effect_t *effect;
        fprintf(out, "b%d", block->id);
        if (block->params) {
            const vir_value_t *param;
            int first = 1;
            fprintf(out, "(");
            for (param = block->params; param; param = param->param_next) {
                fprintf(out, "%s%%d%d:%s", first ? "" : ", ", param->id,
                        vir_type_name(param->type));
                first = 0;
            }
            fprintf(out, ")");
        }
        fprintf(out, ":\n");
        value = block->head;
        effect = block->effects;
        while (value || effect) {
            if (value && effect && effect->result == value) {
                vir_print_value(value, out);
                value = value->next;
                effect = effect->next;
            } else if (!value || (effect && effect->order < value->order)) {
                vir_print_effect(effect, out);
                effect = effect->next;
            } else {
                vir_print_value(value, out);
                value = value->next;
            }
        }
        if (block->terminator == VIR_TERM_BRANCH) {
            fprintf(out, "  branch %%d%d, b%d(", block->branch_condition->id,
                    block->true_edge->to->id);
            vir_print_values(block->true_edge->args,
                             block->true_edge->arg_count, out);
            fprintf(out, "), b%d(", block->false_edge->to->id);
            vir_print_values(block->false_edge->args,
                             block->false_edge->arg_count, out);
            fprintf(out, ")\n");
        } else {
            for (edge = block->outgoing; edge; edge = edge->next_outgoing) {
                fprintf(out, "  jump b%d(", edge->to->id);
                vir_print_values(edge->args, edge->arg_count, out);
                fprintf(out, ")\n");
            }
        }
        if (block->terminator == VIR_TERM_RETURN && block->return_value)
            fprintf(out, "  return %%d%d\n", block->return_value->id);
        else if (block->terminator == VIR_TERM_RETURN)
            fprintf(out, "  return\n");
    }
}
