#include "vir.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct vir_dominance vir_dominance_t;

static int vir_dominance_index(const vir_dominance_t *dominance,
                               const vir_block_t *block);

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
static bool vir_mark_reachable_blocks(const vir_function_t *func,
                                      const vir_block_t *blocked,
                                      unsigned char *reachable,
                                      int *work);
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
    const void *held = use;

    if (kind != VIR_USE_EXACT)
        held = use->kind == kind ? use->owner : NULL;
    return held == owner && (operand < 0 || use->operand == operand);
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

#define VIR_ARENA_GROWTH_LIMIT (256 * 1024)

static void vir_arena_release(vir_arena_t *arena)
{
    vir_arena_block_t *block = arena->head;
    while (block) {
        vir_arena_block_t *next = block->next;
        free(block);
        block = next;
    }
}

/* Allocation for a caller that writes every byte itself. */
static void *vir_arena_alloc_raw(vir_arena_t *arena, int size)
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
        /* Grow with the arena, up to a cap, so a large graph takes a handful of
         * blocks rather than one per block_size bytes.
         */
        capacity = arena->capacity < VIR_ARENA_GROWTH_LIMIT
                       ? arena->capacity
                       : VIR_ARENA_GROWTH_LIMIT;
        if (capacity < arena->block_size)
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
    return ptr;
}

/* Every allocation comes back zero-filled. */
static void *vir_arena_alloc(vir_arena_t *arena, int size)
{
    void *ptr = vir_arena_alloc_raw(arena, size);

    if (ptr)
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
    free(func->block_table);
    vir_arena_release(&func->arena);
    memset(func, 0, sizeof(*func));
}

/* Graph compaction. Removing values, effects, edges and blocks unlinks them but
 * leaves their bytes in the arena, so a finished graph carries every object its
 * construction and optimization ever made. vir_function_compact() copies the
 * live graph into a fresh arena and releases the old one.
 *
 * Copying keeps ids, positions, construction order and the order of every list,
 * use lists included, so passes and lowering see the same graph. While a copy
 * is made, the old object forwards to its copy through one of its own links: a
 * value through @member, an effect through @next and an edge through
 * @next_outgoing. Blocks forward by their index in the block table.
 */
typedef struct {
    vir_function_t *func;
    vir_arena_t *arena;
    vir_block_t **blocks;
    /* By value id, the copy's operand uses, indexed by operand. */
    vir_use_t **operand_uses;
} vir_compact_t;

static bool vir_root_index_insert(vir_function_t *func, vir_value_t *value);

static vir_value_t *vir_compact_value(const vir_value_t *value)
{
    return value ? value->member : NULL;
}

static vir_effect_t *vir_compact_effect(const vir_effect_t *effect)
{
    return effect ? effect->next : NULL;
}

static vir_edge_t *vir_compact_edge(const vir_edge_t *edge)
{
    return edge ? edge->next_outgoing : NULL;
}

static vir_block_t *vir_compact_block(const vir_compact_t *compact,
                                      const vir_block_t *block)
{
    return block ? compact->blocks[block->index] : NULL;
}

static void *vir_compact_bytes(vir_compact_t *compact,
                               const void *from,
                               int size)
{
    void *copy;

    if (!from || size <= 0)
        return NULL;
    copy = vir_arena_alloc_raw(compact->arena, size);
    if (copy)
        memcpy(copy, from, (size_t) size);
    return copy;
}

/* A copy of @name, or NULL when there is none or memory runs out. */
static char *vir_compact_string(vir_compact_t *compact, const char *name)
{
    size_t length;

    if (!name)
        return NULL;
    length = strlen(name) + 1;
    return length > INT_MAX ? NULL
                            : vir_compact_bytes(compact, name, (int) length);
}

/* Copy one value and point the old one at the copy. The copy keeps old links
 * until vir_compact_link_value() translates them.
 */
static vir_value_t *vir_compact_copy_value(vir_compact_t *compact,
                                           vir_value_t *value)
{
    vir_value_t *copy = vir_compact_bytes(compact, value, sizeof(*value));

    if (!copy)
        return NULL;
    if (value->addr) {
        copy->addr =
            vir_compact_bytes(compact, value->addr, sizeof(*value->addr));
        if (!copy->addr ||
            (value->addr->name && !(copy->addr->name = vir_compact_string(
                                        compact, value->addr->name))))
            return NULL;
        copy->addr->root_next = NULL;
    }
    /* Operand uses are found again by their operand index. */
    if (value->op0) {
        compact->operand_uses[value->id] = vir_arena_alloc(
            compact->arena, (int) sizeof(vir_use_t) * (value->op1 ? 2 : 1));
        if (!compact->operand_uses[value->id])
            return NULL;
    }
    value->member = copy;
    return copy;
}

static vir_effect_t *vir_compact_copy_effect(vir_compact_t *compact,
                                             vir_effect_t *effect)
{
    vir_effect_t *copy = vir_compact_bytes(compact, effect, sizeof(*effect));
    int count = effect->arg_count;

    if (!copy || (effect->callee && !(copy->callee = vir_compact_string(
                                          compact, effect->callee))))
        return NULL;
    if (effect->address_use && !(copy->address_use = vir_arena_alloc(
                                     compact->arena, sizeof(vir_use_t))))
        return NULL;
    if (effect->stored_value_use && !(copy->stored_value_use = vir_arena_alloc(
                                          compact->arena, sizeof(vir_use_t))))
        return NULL;
    if (effect->callee_value_use && !(copy->callee_value_use = vir_arena_alloc(
                                          compact->arena, sizeof(vir_use_t))))
        return NULL;
    copy->args = NULL;
    copy->arg_uses = NULL;
    if (effect->args && count &&
        !(copy->args = vir_compact_bytes(compact, effect->args,
                                         (int) sizeof(*copy->args) * count)))
        return NULL;
    if (effect->arg_uses && count &&
        !(copy->arg_uses = vir_arena_alloc(
              compact->arena, (int) sizeof(*copy->arg_uses) * count)))
        return NULL;
    if (effect->signature) {
        const vir_call_signature_t *signature = effect->signature;
        vir_call_signature_t *sig =
            vir_compact_bytes(compact, signature, sizeof(*signature));

        if (!sig)
            return NULL;
        sig->params = NULL;
        sig->param_slots = NULL;
        if (signature->param_count &&
            !(sig->params = vir_compact_bytes(
                  compact, signature->params,
                  (int) sizeof(*signature->params) * signature->param_count)))
            return NULL;
        if (signature->param_slots && signature->param_count &&
            !(sig->param_slots =
                  vir_compact_bytes(compact, signature->param_slots,
                                    (int) sizeof(*signature->param_slots) *
                                        signature->param_count)))
            return NULL;
        copy->signature = sig;
    }
    effect->next = copy;
    return copy;
}

static vir_edge_t *vir_compact_copy_edge(vir_compact_t *compact,
                                         vir_edge_t *edge)
{
    vir_edge_t *copy = vir_compact_bytes(compact, edge, sizeof(*edge));

    if (!copy)
        return NULL;
    copy->args = NULL;
    copy->arg_uses = NULL;
    if (edge->arg_count) {
        copy->args = vir_compact_bytes(
            compact, edge->args, (int) sizeof(*edge->args) * edge->arg_count);
        copy->arg_uses = vir_arena_alloc(
            compact->arena, (int) sizeof(*edge->arg_uses) * edge->arg_count);
        if (!copy->args || !copy->arg_uses)
            return NULL;
    }
    edge->next_outgoing = copy;
    return copy;
}

/* The copy of the use @use of an old value, or a new use for a terminator. */
static vir_use_t *vir_compact_use(vir_compact_t *compact, const vir_use_t *use)
{
    vir_effect_t *effect;
    vir_edge_t *edge;
    vir_value_t *user;

    switch (use->kind) {
    case VIR_USE_USER:
        user = use->owner;
        return &compact->operand_uses[user->id][use->operand];
    case VIR_USE_EDGE:
        edge = vir_compact_edge(use->owner);
        return &edge->arg_uses[use->operand];
    case VIR_USE_EFFECT:
        effect = vir_compact_effect(use->owner);
        if (effect->kind == VIR_EFFECT_CALL)
            return use->operand < 0 ? effect->callee_value_use
                                    : &effect->arg_uses[use->operand];
        return use->operand == 0 ? effect->address_use
                                 : effect->stored_value_use;
    default:
        return vir_arena_alloc(compact->arena, sizeof(vir_use_t));
    }
}

static void *vir_compact_owner(vir_compact_t *compact, const vir_use_t *use)
{
    switch (use->kind) {
    case VIR_USE_USER:
        return vir_compact_value(use->owner);
    case VIR_USE_EDGE:
        return vir_compact_edge(use->owner);
    case VIR_USE_EFFECT:
        return vir_compact_effect(use->owner);
    default:
        return vir_compact_block(compact, use->owner);
    }
}

/* Translate the copy's links and rebuild its use list in the old order. The
 * copy's @uses still heads the old list, whose records are left untouched.
 */
static bool vir_compact_link_value(vir_compact_t *compact, vir_value_t *copy)
{
    vir_use_t **tail = &copy->uses;
    const vir_use_t *use = copy->uses;

    copy->op0 = vir_compact_value(copy->op0);
    copy->op1 = vir_compact_value(copy->op1);
    copy->block = vir_compact_block(compact, copy->block);
    copy->next = vir_compact_value(copy->next);
    copy->param_next = vir_compact_value(copy->param_next);
    copy->def_effect = vir_compact_effect(copy->def_effect);
    copy->member = copy;
    for (; use; use = use->next) {
        vir_use_t *fresh = vir_compact_use(compact, use);

        if (!fresh)
            return false;
        fresh->kind = use->kind;
        fresh->owner = vir_compact_owner(compact, use);
        fresh->operand = use->operand;
        *tail = fresh;
        tail = &fresh->next;
    }
    *tail = NULL;
    return !copy->addr || copy->opcode == VIR_OP_FUNC_ADDR ||
           vir_root_index_insert(compact->func, copy);
}

static void vir_compact_link_effect(vir_compact_t *compact, vir_effect_t *copy)
{
    copy->block = vir_compact_block(compact, copy->block);
    copy->address = vir_compact_value(copy->address);
    copy->stored_value = vir_compact_value(copy->stored_value);
    copy->result = vir_compact_value(copy->result);
    copy->callee_value = vir_compact_value(copy->callee_value);
    copy->next = vir_compact_effect(copy->next);
    if (copy->args)
        for (int i = 0; i < copy->arg_count; i++)
            copy->args[i] = vir_compact_value(copy->args[i]);
}

static void vir_compact_link_edge(vir_compact_t *compact, vir_edge_t *copy)
{
    copy->from = vir_compact_block(compact, copy->from);
    copy->to = vir_compact_block(compact, copy->to);
    copy->next_outgoing = vir_compact_edge(copy->next_outgoing);
    copy->next_incoming = vir_compact_edge(copy->next_incoming);
    for (int i = 0; i < copy->arg_count; i++)
        copy->args[i] = vir_compact_value(copy->args[i]);
}

static bool vir_compact_copy_block(vir_compact_t *compact, vir_block_t *block)
{
    vir_value_t *value, *next;
    vir_effect_t *effect, *next_effect;
    vir_edge_t *edge, *next_edge;

    for (value = block->params; value; value = value->param_next)
        if (!vir_compact_copy_value(compact, value))
            return false;
    for (value = block->head; value; value = next) {
        next = value->next;
        if (!vir_compact_copy_value(compact, value))
            return false;
    }
    for (effect = block->effects; effect; effect = next_effect) {
        next_effect = effect->next;
        if (!vir_compact_copy_effect(compact, effect))
            return false;
    }
    for (edge = block->outgoing; edge; edge = next_edge) {
        next_edge = edge->next_outgoing;
        if (!vir_compact_copy_edge(compact, edge))
            return false;
    }
    return true;
}

/* Once copied, the old lists can no longer be walked: their links forward to
 * copies. The copies are walked instead, translating each old link they hold.
 */
static bool vir_compact_link_block(vir_compact_t *compact, vir_block_t *copy)
{
    vir_value_t *value;
    vir_effect_t *effect;
    vir_edge_t *edge;

    copy->params = vir_compact_value(copy->params);
    copy->last_param = vir_compact_value(copy->last_param);
    copy->head = vir_compact_value(copy->head);
    copy->tail = vir_compact_value(copy->tail);
    copy->effects = vir_compact_effect(copy->effects);
    copy->last_effect = vir_compact_effect(copy->last_effect);
    copy->incoming = vir_compact_edge(copy->incoming);
    copy->outgoing = vir_compact_edge(copy->outgoing);
    copy->return_value = vir_compact_value(copy->return_value);
    copy->branch_condition = vir_compact_value(copy->branch_condition);
    copy->true_edge = vir_compact_edge(copy->true_edge);
    copy->false_edge = vir_compact_edge(copy->false_edge);
    copy->next = vir_compact_block(compact, copy->next);

    /* Linking a copy translates its own next link, so each walk advances on
     * links that already point at copies.
     */
    for (value = copy->params; value; value = value->param_next)
        if (!vir_compact_link_value(compact, value))
            return false;
    for (value = copy->head; value; value = value->next)
        if (!vir_compact_link_value(compact, value))
            return false;
    for (effect = copy->effects; effect; effect = effect->next)
        vir_compact_link_effect(compact, effect);
    for (edge = copy->outgoing; edge; edge = edge->next_outgoing)
        vir_compact_link_edge(compact, edge);
    return true;
}

static int vir_compact_round(size_t bytes)
{
    return (int) ((bytes + sizeof(void *) - 1) & ~(sizeof(void *) - 1));
}

/* Bytes the copy will take, so that it lands in one arena block of that size
 * rather than in blocks grown with spare room at their ends.
 */
static long vir_compact_size(const vir_function_t *func)
{
    long bytes = 0;
    int roots = 0, capacity = 64;

    for (int i = 0; i < func->block_count; i++) {
        const vir_block_t *block = func->block_table[i];

        bytes += vir_compact_round(sizeof(*block));
        for (int list = 0; list < 2; list++)
            for (const vir_value_t *value = list ? block->head : block->params;
                 value; value = list ? value->next : value->param_next) {
                bytes += vir_compact_round(sizeof(*value));
                if (value->op0)
                    bytes += vir_compact_round(sizeof(vir_use_t) *
                                               (value->op1 ? 2 : 1));
                if (value->addr) {
                    bytes += vir_compact_round(sizeof(*value->addr));
                    if (value->addr->name)
                        bytes +=
                            vir_compact_round(strlen(value->addr->name) + 1);
                    roots += value->opcode != VIR_OP_FUNC_ADDR;
                }
            }
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            bytes +=
                vir_compact_round(sizeof(*effect)) +
                3 * vir_compact_round(sizeof(vir_use_t)) +
                vir_compact_round((sizeof(vir_value_t *) + sizeof(vir_use_t)) *
                                  effect->arg_count);
            if (effect->callee)
                bytes += vir_compact_round(strlen(effect->callee) + 1);
            if (effect->signature)
                bytes += vir_compact_round(sizeof(*effect->signature)) +
                         vir_compact_round(
                             (sizeof(*effect->signature->params) +
                              sizeof(*effect->signature->param_slots)) *
                             effect->signature->param_count);
        }
        for (const vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing)
            bytes +=
                vir_compact_round(sizeof(*edge)) +
                vir_compact_round((sizeof(vir_value_t *) + sizeof(vir_use_t)) *
                                  edge->arg_count);
        if (block->return_value || block->branch_condition)
            bytes += vir_compact_round(sizeof(vir_use_t));
    }
    /* The root index doubles as it fills, leaving each smaller table behind. */
    while (capacity / 2 <= roots)
        capacity *= 2;
    return bytes + 2L * capacity * (long) sizeof(vir_value_t *);
}

bool vir_function_compact(vir_function_t *func)
{
    vir_function_t fresh = *func;
    vir_compact_t compact;
    bool ok;
    long size;
    int i;

    if (!func->blocks)
        return true;
    size = vir_compact_size(func);
    memset(&fresh.arena, 0, sizeof(fresh.arena));
    fresh.arena.block_size = size < INT_MAX ? (int) size : INT_MAX;
    fresh.arena.fail_after = func->arena.fail_after;
    fresh.root_table = NULL;
    fresh.root_capacity = 0;
    fresh.root_count = 0;
    compact.func = &fresh;
    compact.arena = &fresh.arena;
    compact.blocks = calloc((size_t) func->block_count, sizeof(vir_block_t *));
    compact.operand_uses =
        calloc((size_t) func->next_value_id + 1, sizeof(vir_use_t *));
    ok = compact.blocks && compact.operand_uses &&
         vir_root_index_insert(&fresh, NULL);
    for (i = 0; ok && i < func->block_count; i++)
        ok = (compact.blocks[i] = vir_compact_bytes(
                  &compact, func->block_table[i], sizeof(vir_block_t))) != NULL;
    for (i = 0; ok && i < func->block_count; i++)
        ok = vir_compact_copy_block(&compact, func->block_table[i]);
    /* Past this point the old graph is no longer intact. */
    for (i = 0; ok && i < func->block_count; i++)
        ok = vir_compact_link_block(&compact, compact.blocks[i]);
    if (ok) {
        for (i = 0; i < func->block_count; i++)
            fresh.block_table[i] = compact.blocks[i];
        fresh.blocks = compact.blocks[0];
        fresh.last_block = compact.blocks[func->block_count - 1];
        fresh.arena.block_size = func->arena.block_size;
        vir_arena_release(&func->arena);
        *func = fresh;
    } else {
        vir_arena_release(&fresh.arena);
    }
    free(compact.blocks);
    free(compact.operand_uses);
    return ok;
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

/* Link @block after the function's last block. */
static bool vir_block_link(vir_function_t *func, vir_block_t *block)
{
    if (func->block_count == func->block_capacity) {
        if (func->block_capacity >= INT_MAX / (int) sizeof(block) / 2)
            return false;
        int capacity = func->block_capacity ? func->block_capacity * 2 : 16;
        vir_block_t **table = malloc((size_t) capacity * sizeof(*table));
        if (!table)
            return false;
        if (func->block_table)
            memcpy(table, func->block_table,
                   (size_t) func->block_count * sizeof(*table));
        free(func->block_table);
        func->block_table = table;
        func->block_capacity = capacity;
    }
    block->index = func->block_count;
    func->block_table[func->block_count++] = block;
    block->id = func->next_block_id++;
    if (func->last_block)
        func->last_block->next = block;
    else
        func->blocks = block;
    func->last_block = block;
    return true;
}

vir_block_t *vir_block_create(vir_function_t *func)
{
    vir_block_t *block = vir_arena_alloc(&func->arena, sizeof(*block));
    return block && vir_block_link(func, block) ? block : NULL;
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
    value->member = value;
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

static unsigned int vir_root_hash(vir_address_kind_t kind,
                                  unsigned int slot,
                                  const char *name)
{
    unsigned int hash = 2166136261u;

    if (kind != VIR_ADDRESS_STACK) {
        for (; name && *name; name++)
            hash = (hash ^ (unsigned char) *name) * 16777619u;
        return hash;
    }
    return slot * 2654435761u;
}

static unsigned int vir_root_bucket(vir_address_kind_t kind,
                                    unsigned int slot,
                                    const char *name,
                                    int capacity)
{
    return vir_root_hash(kind, slot, name) & (unsigned int) (capacity - 1);
}

/* Address values are indexed when created; a root's slot or name never changes
 * afterwards (only its size may be refined), so the bucket stays valid. The
 * table lives in the function's arena: a separate heap table would cost a whole
 * page per function under shecc's own malloc. A grown table leaves the old one
 * behind, at most doubling its footprint.
 */
static bool vir_root_index_insert(vir_function_t *func, vir_value_t *value)
{
    if (func->root_count >= func->root_capacity / 2) {
        int capacity = func->root_capacity ? func->root_capacity * 2 : 64;
        vir_value_t **table;

        if (capacity > INT_MAX / (int) sizeof(*table) / 2)
            return false;
        table = vir_arena_alloc(&func->arena, capacity * (int) sizeof(*table));
        if (!table)
            return false;
        memset(table, 0, (size_t) capacity * sizeof(*table));
        for (int i = 0; i < func->root_capacity; i++) {
            vir_value_t *entry = func->root_table[i];
            while (entry) {
                vir_value_t *next = entry->addr->root_next;
                unsigned int bucket =
                    vir_root_bucket(entry->addr->kind, entry->addr->slot,
                                    entry->addr->name, capacity);
                entry->addr->root_next = table[bucket];
                table[bucket] = entry;
                entry = next;
            }
        }
        func->root_table = table;
        func->root_capacity = capacity;
    }
    if (value) {
        unsigned int bucket =
            vir_root_bucket(value->addr->kind, value->addr->slot,
                            value->addr->name, func->root_capacity);
        value->addr->root_next = func->root_table[bucket];
        func->root_table[bucket] = value;
        func->root_count++;
    }
    return true;
}

/* Whether @value sits in the value list of a block of @func. Removal clears
 * @member, so only stale block metadata needs the full scan.
 */
static bool vir_value_listed(const vir_function_t *func,
                             const vir_value_t *value)
{
    return value->member == value && vir_has_value(func, value);
}

static bool vir_address_root_conflicts(const vir_function_t *func,
                                       vir_address_kind_t kind,
                                       unsigned int slot,
                                       const char *name,
                                       unsigned int size,
                                       unsigned int alignment)
{
    if (!func->root_capacity)
        return 0;
    unsigned int bucket =
        vir_root_bucket(kind, slot, name, func->root_capacity);
    for (const vir_value_t *value = func->root_table[bucket]; value;
         value = value->addr->root_next) {
        bool same_root = kind == VIR_ADDRESS_STACK
                             ? value->opcode == VIR_OP_STACK_ADDR &&
                                   value->addr->slot == slot
                             : value->opcode == VIR_OP_GLOBAL_ADDR &&
                                   value->addr->name &&
                                   !strcmp(value->addr->name, name);

        if (same_root &&
            (value->addr->size != size ||
             value->addr->alignment != alignment) &&
            vir_value_listed(func, value))
            return 1;
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
    vir_address_t *addr;
    vir_value_t *value;

    if (!vir_has_block(func, block) || !vir_type_width(func, VIR_TYPE_PTR) ||
        !size || !vir_valid_alignment(alignment) ||
        (kind == VIR_ADDRESS_GLOBAL && (!name || !*name)) ||
        vir_address_root_conflicts(func, kind, slot, name, size, alignment) ||
        !vir_root_index_insert(func, NULL))
        return NULL;
    if (kind == VIR_ADDRESS_GLOBAL) {
        size_t length = strlen(name) + 1;

        if (length > (size_t) 0x7fffffff ||
            !(copy = vir_arena_alloc(&func->arena, (int) length)))
            return NULL;
        memcpy(copy, name, length);
    }
    if (!(addr = vir_arena_alloc(&func->arena, sizeof(*addr))))
        return NULL;
    value = vir_new_value(
        func, block,
        kind == VIR_ADDRESS_STACK ? VIR_OP_STACK_ADDR : VIR_OP_GLOBAL_ADDR,
        VIR_TYPE_PTR, 0);
    if (!value)
        return NULL;
    addr->kind = kind;
    addr->slot = slot;
    addr->size = size;
    addr->alignment = alignment;
    addr->name = copy;
    value->addr = addr;
    vir_root_index_insert(func, value);
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
    vir_address_t *addr;
    char *copy;
    size_t length;

    if (!vir_has_block(func, block) || !name || !*name)
        return NULL;
    length = strlen(name) + 1;
    if (length > INT_MAX ||
        !(copy = vir_arena_alloc(&func->arena, (int) length)) ||
        !(addr = vir_arena_alloc(&func->arena, sizeof(*addr))))
        return NULL;
    memcpy(copy, name, length);
    addr->name = copy;
    value = vir_new_value(func, block, VIR_OP_FUNC_ADDR, VIR_TYPE_PTR, 0);
    if (value)
        value->addr = addr;
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
    param->member = param;
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
    vir_use_set_owner(use, VIR_USE_EFFECT, effect);
    use->operand = operand;
    vir_link_use(value, use);
}

static void vir_edge_link_uses(vir_edge_t *edge)
{
    for (int i = 0; i < edge->arg_count; i++) {
        vir_use_t *use = &edge->arg_uses[i];
        vir_use_set_owner(use, VIR_USE_EDGE, edge);
        use->operand = i;
        vir_link_use(edge->args[i], use);
    }
}

static void vir_edge_unlink_uses(vir_edge_t *edge)
{
    for (int i = 0; i < edge->arg_count; i++)
        vir_remove_use(edge->args[i], &edge->arg_uses[i]);
}

static void vir_edge_replace_args(vir_edge_t *edge,
                                  vir_value_t **args,
                                  vir_use_t *uses,
                                  int count)
{
    vir_edge_unlink_uses(edge);
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
    vir_edge_remove_from_list(&edge->from->outgoing, edge, true);
    vir_edge_remove_from_list(&edge->to->incoming, edge, false);
    vir_edge_unlink_uses(edge);
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
    if (!vir_block_link(func, split))
        return NULL;
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
    vir_use_set_owner(condition_use, VIR_USE_BRANCH, block);
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
    vir_use_set_owner(use, VIR_USE_RETURN, block);
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
    vir_use_set_owner(use, VIR_USE_USER, user);
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

    if (!vir_has_block(func, block) ||
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
        if (!address || !vir_block_open(func, block))                  \
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

vir_effect_t *vir_deferred_store(vir_function_t *func,
                                 vir_block_t *block,
                                 bool is_volatile)
{
    return vir_store_impl(func, block, NULL, NULL, is_volatile);
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

static vir_value_t *vir_normalize_bitwise(vir_function_t *func,
                                          vir_block_t *block,
                                          vir_value_t *op0,
                                          vir_value_t *op1,
                                          vir_opcode_t opcode)
{
    /* Normalize bitwise identities before allocating a value. Operands remain
     * in the graph, so discarding a result cannot discard memory effects.
     */
    if (opcode == VIR_OP_BITAND || opcode == VIR_OP_BITOR ||
        opcode == VIR_OP_BITXOR) {
        if (op0 == op1)
            return opcode == VIR_OP_BITXOR
                       ? vir_const_int(func, block, op0->type, 0)
                       : op0;
        if ((op0->opcode == VIR_OP_BITNOT && op0->op0 == op1) ||
            (op1->opcode == VIR_OP_BITNOT && op1->op0 == op0))
            return vir_const_int(
                func, block, op0->type,
                opcode == VIR_OP_BITAND ? 0 : vir_integer_mask(op0->type));
        if (opcode == VIR_OP_BITOR || opcode == VIR_OP_BITXOR) {
            vir_value_t *other = NULL;
            if (vir_is_const_int(op0, vir_integer_mask(op0->type)))
                other = op1;
            else if (vir_is_const_int(op1, vir_integer_mask(op1->type)))
                other = op0;
            if (other)
                return opcode == VIR_OP_BITOR
                           ? vir_const_int(func, block, op0->type,
                                           vir_integer_mask(op0->type))
                           : vir_bitnot(func, block, other);
        }
        if (opcode != VIR_OP_BITXOR) {
            if (op0->opcode == opcode && (op0->op0 == op1 || op0->op1 == op1))
                return op0;
            if (op1->opcode == opcode && (op1->op0 == op0 || op1->op1 == op0))
                return op1;
            vir_opcode_t dual =
                opcode == VIR_OP_BITAND ? VIR_OP_BITOR : VIR_OP_BITAND;
            if (op0->opcode == dual && (op0->op0 == op1 || op0->op1 == op1))
                return op1;
            if (op1->opcode == dual && (op1->op0 == op0 || op1->op1 == op0))
                return op0;
        }
    }
    return NULL;
}

/* Identities and constant folds that construction applies at every level. */
static vir_value_t *vir_normalize_integer(vir_function_t *func,
                                          vir_block_t *block,
                                          vir_value_t *op0,
                                          vir_value_t *op1,
                                          vir_opcode_t opcode)
{
    if (opcode == VIR_OP_ADD || opcode == VIR_OP_BITOR ||
        opcode == VIR_OP_BITXOR) {
        if (vir_is_const_int(op0, 0))
            return op1;
        if (vir_is_const_int(op1, 0))
            return op0;
    }
    if (opcode == VIR_OP_SUB) {
        if (op0 == op1)
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op1, 0))
            return op0;
    }
    if (opcode == VIR_OP_BITAND) {
        if (vir_is_const_int(op0, 0) || vir_is_const_int(op1, 0))
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op0, vir_integer_mask(op0->type)))
            return op1;
        if (vir_is_const_int(op1, vir_integer_mask(op0->type)))
            return op0;
    }
    if (vir_opcode_is_shift(opcode) && vir_is_const_int(op1, 0))
        return op0;
    if (opcode == VIR_OP_MUL) {
        if (vir_is_const_int(op0, 0) || vir_is_const_int(op1, 0))
            return vir_const_int(func, block, op0->type, 0);
        if (vir_is_const_int(op0, 1))
            return op1;
        if (vir_is_const_int(op1, 1))
            return op0;
    }
    unsigned long long result;
    if (vir_is_constant(op0) && vir_is_constant(op1) &&
        vir_fold_binary(func, opcode, op0->type, op0->type, op0->constant,
                        op1->constant, &result))
        return vir_const_int(func, block, op0->type, result);
    return NULL;
}

/* Rewrite multiplication, division and remainder by a power of two as shifts
 * and masks. Only vir_normalize() applies it, so construction keeps the
 * arithmetic shape the frontend asked for.
 */
static vir_value_t *vir_strength_reduce_integer(vir_function_t *func,
                                                vir_block_t *block,
                                                vir_value_t *op0,
                                                vir_value_t *op1,
                                                vir_opcode_t opcode)
{
    if (opcode != VIR_OP_MUL && !vir_opcode_is_division(opcode))
        return NULL;
    if (opcode == VIR_OP_MUL && vir_is_constant(op0)) {
        vir_value_t *swap = op0;
        op0 = op1;
        op1 = swap;
    }
    if (!vir_is_constant(op1))
        return NULL;
    unsigned long long divisor = op1->constant;
    int width = vir_integer_type_width(op0->type);
    if (!divisor || (divisor & (divisor - 1)))
        return NULL;
    bool remainder = opcode == VIR_OP_SREM || opcode == VIR_OP_UREM;
    bool signed_division = opcode == VIR_OP_SDIV || opcode == VIR_OP_SREM;
    if (signed_division && (divisor >> (width - 1)))
        return NULL;
    if (divisor == 1)
        return remainder ? vir_const_int(func, block, op0->type, 0) : op0;
    int shift = 0;
    for (unsigned long long bits = divisor; bits > 1; bits >>= 1)
        shift++;
    vir_value_t *count = vir_const_i32(func, block, shift);
    if (!count)
        return NULL;
    if (opcode == VIR_OP_MUL)
        return vir_shl(func, block, op0, count);
    if (opcode == VIR_OP_UDIV)
        return vir_lshr(func, block, op0, count);
    vir_value_t *mask = vir_const_int(func, block, op0->type, divisor - 1);
    if (!mask)
        return NULL;
    if (opcode == VIR_OP_UREM)
        return vir_bitand(func, block, op0, mask);

    /* Signed quotients truncate toward zero. Bias negative dividends by d-1
     * before the arithmetic shift; their addition cannot overflow the type.
     * Remainders retain the dividend's sign via x - quotient*d.
     */
    vir_value_t *sign_count = vir_const_i32(func, block, width - 1);
    vir_value_t *sign =
        sign_count ? vir_ashr(func, block, op0, sign_count) : NULL;
    vir_value_t *bias = sign ? vir_bitand(func, block, sign, mask) : NULL;
    vir_value_t *biased = bias ? vir_add(func, block, op0, bias) : NULL;
    vir_value_t *quotient =
        biased ? vir_ashr(func, block, biased, count) : NULL;
    if (!quotient || !remainder)
        return quotient;
    vir_value_t *product = vir_shl(func, block, quotient, count);
    return product ? vir_sub(func, block, op0, product) : NULL;
}

static vir_value_t *vir_integer_binary(vir_function_t *func,
                                       vir_block_t *block,
                                       vir_value_t *op0,
                                       vir_value_t *op1,
                                       vir_opcode_t opcode);

/* The operations that associate with a constant: op(op(x, c1), c2) is
 * op(x, c1 op c2), and two shifts the same way make one shift by the sum while
 * it stays below the width. This shortens the chain a field offset or a mask
 * built in steps would otherwise keep.
 */
static vir_value_t *vir_normalize_chain(vir_function_t *func,
                                        vir_block_t *block,
                                        vir_value_t *op0,
                                        vir_value_t *op1,
                                        vir_opcode_t opcode)
{
    vir_value_t *constant = NULL, *inner = NULL, *inner_constant = NULL;
    vir_value_t *x;
    unsigned long long combined;
    int width = vir_integer_type_width(op0->type);

    if (opcode == VIR_OP_SHL || opcode == VIR_OP_ASHR ||
        opcode == VIR_OP_LSHR) {
        if (!vir_is_constant(op1) || op0->opcode != opcode || op0->def_effect ||
            !vir_is_constant(op0->op1) ||
            op0->op1->constant >= (unsigned long long) width ||
            op1->constant >= (unsigned long long) width ||
            op0->op1->constant + op1->constant >= (unsigned long long) width)
            return NULL;
        vir_value_t *count = vir_const_i32(
            func, block, (int) (op0->op1->constant + op1->constant));
        return count ? vir_integer_binary(func, block, op0->op0, count, opcode)
                     : NULL;
    }
    if (opcode != VIR_OP_ADD && opcode != VIR_OP_MUL &&
        opcode != VIR_OP_BITAND && opcode != VIR_OP_BITOR &&
        opcode != VIR_OP_BITXOR)
        return NULL;
    if (vir_is_constant(op1) && !vir_is_constant(op0)) {
        constant = op1;
        inner = op0;
    } else if (vir_is_constant(op0) && !vir_is_constant(op1)) {
        constant = op0;
        inner = op1;
    }
    if (!constant || inner->opcode != opcode || inner->def_effect)
        return NULL;
    if (vir_is_constant(inner->op1) && !vir_is_constant(inner->op0)) {
        inner_constant = inner->op1;
        x = inner->op0;
    } else if (vir_is_constant(inner->op0) && !vir_is_constant(inner->op1)) {
        inner_constant = inner->op0;
        x = inner->op1;
    } else {
        return NULL;
    }
    if (!vir_fold_binary(func, opcode, op0->type, op0->type,
                         inner_constant->constant, constant->constant,
                         &combined))
        return NULL;
    constant = vir_const_int(func, block, op0->type, combined);
    return constant ? vir_integer_binary(func, block, x, constant, opcode)
                    : NULL;
}

static vir_value_t *vir_normalize_binary(vir_function_t *func,
                                         vir_block_t *block,
                                         vir_value_t *op0,
                                         vir_value_t *op1,
                                         vir_opcode_t opcode)
{
    vir_value_t *normalized =
        vir_normalize_bitwise(func, block, op0, op1, opcode);
    if (!normalized)
        normalized = vir_normalize_integer(func, block, op0, op1, opcode);
    return normalized ? normalized
                      : vir_normalize_chain(func, block, op0, op1, opcode);
}

static vir_value_t *vir_integer_unary(vir_function_t *func,
                                      vir_block_t *block,
                                      vir_value_t *operand,
                                      vir_type_t type,
                                      vir_opcode_t opcode);

/* A cast of a cast is one cast, or none: trunc(zext(y)) is y when it returns to
 * y's width, a narrower trunc(y) when it goes below it, and zext(y) or sext(y)
 * when it stops between. Extending a zero extension by zeros or by sign bits
 * is the same zero extension.
 */
static vir_value_t *vir_cast_chain(vir_function_t *func,
                                   vir_block_t *block,
                                   vir_opcode_t opcode,
                                   vir_type_t type,
                                   vir_value_t *inner)
{
    vir_value_t *y = inner->op0;
    int from, to;

    if (inner->def_effect || inner->nr_ops != 1 ||
        (inner->opcode != VIR_OP_ZEXT && inner->opcode != VIR_OP_SEXT &&
         inner->opcode != VIR_OP_TRUNC))
        return NULL;
    from = vir_integer_type_width(y->type);
    to = vir_integer_type_width(type);
    if (!from || !to)
        return NULL;
    if (opcode == VIR_OP_TRUNC) {
        if (inner->opcode == VIR_OP_TRUNC)
            return vir_integer_unary(func, block, y, type, VIR_OP_TRUNC);
        if (to == from)
            return y;
        return vir_integer_unary(func, block, y, type,
                                 to < from ? VIR_OP_TRUNC : inner->opcode);
    }
    if (inner->opcode == VIR_OP_TRUNC)
        return NULL;
    if (inner->opcode == opcode ||
        (opcode == VIR_OP_SEXT && inner->opcode == VIR_OP_ZEXT))
        return vir_integer_unary(func, block, y, type, inner->opcode);
    return NULL;
}

/* NEG and BITNOT undo themselves: op(op(x)) is x. */
static vir_value_t *vir_unary_involution(vir_opcode_t opcode,
                                         const vir_value_t *operand)
{
    return (opcode == VIR_OP_NEG || opcode == VIR_OP_BITNOT) &&
                   operand->opcode == opcode
               ? operand->op0
               : NULL;
}

static vir_value_t *vir_integer_binary(vir_function_t *func,
                                       vir_block_t *block,
                                       vir_value_t *op0,
                                       vir_value_t *op1,
                                       vir_opcode_t opcode)
{
    bool division = vir_opcode_is_division(opcode);
    bool shift = vir_opcode_is_shift(opcode);

    if (!(shift ? vir_binary_operands_are_local(func, block, op0, op1) &&
                      (vir_type_is_i32_or_i64(op0->type) &&
                       op1->type == VIR_TYPE_I32)
                : vir_integer_operands_are_local(func, block, op0, op1)))
        return NULL;
    vir_value_t *normalized =
        vir_normalize_binary(func, block, op0, op1, opcode);
    if (normalized)
        return normalized;
    if (vir_opcode_is_commutative(opcode))
        vir_order_commutative(&op0, &op1);

    /* Normalization already folded every foldable constant pair; only a
     * trapping division or out-of-range shift keeps its explicit operation.
     */
    if (vir_is_constant(op0) && vir_is_constant(op1) && !division && !shift)
        return NULL;
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
        !vir_unary_types_valid(func, opcode, type, operand->type))
        return NULL;
    vir_value_t *involution = vir_unary_involution(opcode, operand);
    if (involution)
        return involution;
    if (cast && !vir_is_constant(operand)) {
        vir_value_t *chained =
            vir_cast_chain(func, block, opcode, type, operand);
        if (chained)
            return chained;
    }
    if (!vir_is_constant(operand))
        return vir_user_operation(func, block, opcode, type, operand, NULL);
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
    } else if (op0 == op1 ||
               (opcode == VIR_OP_ULT && vir_is_const_int(op1, 0))) {
        /* Nothing is below itself, and nothing is unsigned-below zero. */
        return vir_const_i1(func, block, 0);
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

/* ptradd(ptradd(p, c1), c2) is ptradd(p, c1 + c2), the one displacement an
 * access can take.
 */
static vir_value_t *vir_ptradd_fold(vir_function_t *func,
                                    vir_block_t *block,
                                    vir_value_t *base,
                                    vir_value_t *byte_offset)
{
    if (base->opcode != VIR_OP_PTRADD || !vir_is_constant(base->op1) ||
        !vir_is_constant(byte_offset) || base->op1->type != byte_offset->type)
        return NULL;
    vir_value_t *sum =
        vir_const_int(func, block, byte_offset->type,
                      base->op1->constant + byte_offset->constant);
    return sum ? vir_ptradd(func, block, base->op0, sum) : NULL;
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
    vir_value_t *folded = vir_ptradd_fold(func, block, base, byte_offset);
    if (folded)
        return folded;
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

/* Unlink EFFECT, its result value and its operand uses from its block. The
 * caller has already dropped every use of the result.
 */
static bool vir_effect_unlink(vir_effect_t *effect)
{
    vir_effect_t **link = &effect->block->effects;
    while (*link && *link != effect)
        link = &(*link)->next;
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
        effect->result->member = NULL;
        effect->result->def_effect = NULL;
    }
    vir_remove_effect_uses(effect);
    *link = effect->next;
    effect->next = NULL;
    vir_repair_value_positions(effect->block);
    return true;
}

bool vir_effect_remove(vir_function_t *func, vir_effect_t *effect)
{
    if (!func || !effect ||
        (effect->kind != VIR_EFFECT_STORE &&
         (effect->kind != VIR_EFFECT_LOAD || !effect->result ||
          effect->result->uses)) ||
        !vir_has_block(func, effect->block))
        return false;
    return vir_effect_unlink(effect);
}

static void vir_set_use_slot(vir_value_t **slot,
                             vir_value_t *value,
                             vir_use_t *use)
{
    vir_remove_use(*slot, use);
    *slot = value;
    vir_link_use(value, use);
}

static vir_value_t **vir_use_value_slot(const vir_use_t *use)
{
    vir_value_t *user;
    vir_edge_t *edge;
    vir_effect_t *effect;
    vir_block_t *block;

    switch (use->kind) {
    case VIR_USE_USER:
        user = use->owner;
        return use->operand == 0 ? &user->op0 : &user->op1;
    case VIR_USE_EDGE:
        edge = use->owner;
        return &edge->args[use->operand];
    case VIR_USE_EFFECT:
        effect = use->owner;
        if (effect->kind == VIR_EFFECT_CALL)
            return use->operand < 0 ? &effect->callee_value
                                    : &effect->args[use->operand];
        return use->operand == 0 ? &effect->address : &effect->stored_value;
    case VIR_USE_RETURN:
        block = use->owner;
        return &block->return_value;
    default:
        block = use->owner;
        return &block->branch_condition;
    }
}

vir_block_t *vir_use_block(const vir_use_t *use)
{
    switch (use->kind) {
    case VIR_USE_USER:
        return ((vir_value_t *) use->owner)->block;
    case VIR_USE_EDGE:
        return ((vir_edge_t *) use->owner)->from;
    case VIR_USE_EFFECT:
        return ((vir_effect_t *) use->owner)->block;
    case VIR_USE_RETURN:
    case VIR_USE_BRANCH:
        return use->owner;
    default:
        return NULL;
    }
}

int vir_use_order(const vir_use_t *use)
{
    switch (use->kind) {
    case VIR_USE_USER:
        return ((vir_value_t *) use->owner)->order;
    case VIR_USE_EFFECT:
        return ((vir_effect_t *) use->owner)->order;
    default:
        return vir_use_block(use)->next_order;
    }
}

static bool vir_replacement_dominates_with_context(
    const vir_function_t *func,
    const vir_dominance_t *dominance,
    const vir_value_t *from,
    const vir_value_t *to)
{
    const vir_use_t *use;

    /* Without a dominance tree, every use in another block asks the same
     * question: is it unreachable once the definition's block is removed? Walk
     * the CFG once for all of them rather than once per use.
     */
    unsigned char *reachable = NULL;
    int *work = NULL;
    bool dominates = true;
    for (use = from->uses; use && dominates; use = use->next) {
        const vir_block_t *block;
        int position;
        if (vir_use_effect(use)) {
            dominates = vir_value_dominates_effect(func, dominance, to,
                                                   vir_use_effect(use));
            continue;
        }
        if (!(block = vir_use_block(use))) {
            dominates = false;
            continue;
        }
        if (!dominance && func && func->blocks && block != to->block) {
            int index = vir_block_index(func, block);
            if (vir_block_index(func, to->block) < 0 || index < 0)
                dominates = false;
            else if (to->block == func->blocks)
                continue;
            else if (!reachable) {
                reachable =
                    calloc((size_t) func->block_count, sizeof(*reachable));
                work = malloc((size_t) func->block_count * sizeof(*work));
                dominates =
                    reachable && work &&
                    vir_mark_reachable_blocks(func, to->block, reachable, work);
            }
            dominates = dominates && !reachable[index];
            continue;
        }
        position = vir_use_user(use) ? vir_use_user(use)->position
                   : block->tail     ? block->tail->position + 1
                                     : 0;
        dominates = vir_value_dominates(func, dominance, to, block, position);
    }
    free(reachable);
    free(work);
    return dominates;
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

/* What CSE treats as an operand's identity. Constants are not uniqued when
 * they are built, so with @fold two equal ones stand for the same operand;
 * every other value is itself.
 */
static unsigned int vir_cse_operand_key(const vir_value_t *operand, bool fold)
{
    if (fold && operand->opcode == VIR_OP_CONST)
        return ((unsigned int) operand->constant ^
                (unsigned int) (operand->constant >> 32)) *
                   2654435761u +
               (unsigned int) operand->type;
    return (unsigned int) operand->id;
}

/* Two constants that hold the same bits, though built as separate values. */
static bool vir_const_same(const vir_value_t *a, const vir_value_t *b)
{
    return a->opcode == VIR_OP_CONST && b->opcode == VIR_OP_CONST &&
           a->type == b->type && a->constant == b->constant;
}

static bool vir_cse_operand_equal(const vir_value_t *a,
                                  const vir_value_t *b,
                                  bool fold)
{
    return a == b || (fold && vir_const_same(a, b));
}

/* Pure values worth numbering: binary and unary operations, and the address of
 * a stack object, global, string or function, which the backend would
 * otherwise rebuild at every use. Loads and calls carry an effect and constants
 * are cheap to rematerialize, so neither takes part.
 */
static bool vir_cse_candidate(const vir_value_t *value)
{
    if (value->def_effect)
        return false;
    /* A constant displacement folds into the access that uses it: the x64 and
     * AArch64 emitters take it as an addressing-mode offset (see sib_take and
     * addr_fold_disp in x64-codegen.c), so a shared copy would only take a
     * register.
     */
    if (value->opcode == VIR_OP_PTRADD && value->op1->opcode == VIR_OP_CONST)
        return false;
    if (value->nr_ops)
        return true;
    return value->opcode == VIR_OP_STACK_ADDR ||
           value->opcode == VIR_OP_GLOBAL_ADDR ||
           value->opcode == VIR_OP_RODATA_ADDR ||
           value->opcode == VIR_OP_FUNC_ADDR;
}

static unsigned int vir_cse_hash(const vir_value_t *value, bool fold)
{
    unsigned int hash = (unsigned int) value->opcode;
    hash = hash * 33u + (unsigned int) value->type;
    if (value->nr_ops == 2) {
        unsigned int left = vir_cse_operand_key(value->op0, fold);
        unsigned int right = vir_cse_operand_key(value->op1, fold);
        /* Commutative operands may sit in either order. */
        if (vir_opcode_is_commutative(value->opcode))
            return hash * 33u + (left + right) * 31u + (left ^ right);
        return (hash * 33u + left) * 33u + right;
    }
    if (value->nr_ops == 1)
        return hash * 33u + vir_cse_operand_key(value->op0, fold);
    if (value->opcode == VIR_OP_STACK_ADDR)
        return hash * 33u + value->addr->slot;
    if (value->opcode == VIR_OP_RODATA_ADDR)
        return hash * 33u + (unsigned int) value->constant;
    for (const char *name = value->addr->name; *name; name++)
        hash = hash * 33u + (unsigned char) *name;
    return hash;
}

static bool vir_cse_equal(const vir_value_t *a, const vir_value_t *b, bool fold)
{
    if (a->opcode != b->opcode || a->type != b->type || a->nr_ops != b->nr_ops)
        return false;
    if (a->nr_ops == 2)
        return (vir_cse_operand_equal(a->op0, b->op0, fold) &&
                vir_cse_operand_equal(a->op1, b->op1, fold)) ||
               (vir_opcode_is_commutative(a->opcode) &&
                vir_cse_operand_equal(a->op0, b->op1, fold) &&
                vir_cse_operand_equal(a->op1, b->op0, fold));
    if (a->nr_ops == 1)
        return vir_cse_operand_equal(a->op0, b->op0, fold);
    if (a->opcode == VIR_OP_STACK_ADDR)
        return a->addr->slot == b->addr->slot;
    if (a->opcode == VIR_OP_RODATA_ADDR)
        return a->constant == b->constant;
    return a->addr->name == b->addr->name ||
           !strcmp(a->addr->name, b->addr->name);
}

/* Local CSE sizes each block's table from its order counter, which bounds the
 * values it holds: one scan per block per run was a measurable share of a
 * self-compile. The table size only changes probe order, not the leaders. The
 * power-of-two table size keeping @count entries at most half full, or 0 when
 * it would not fit an int.
 */
static unsigned int vir_cse_table_size(int count)
{
    unsigned int capacity = 1;
    if (count > INT_MAX / 2)
        return 0;
    while (capacity < (unsigned int) count * 2u) {
        if (capacity > (unsigned int) INT_MAX / 2u)
            return 0;
        capacity <<= 1;
    }
    return capacity;
}

static unsigned int vir_cse_block_capacity(const vir_block_t *block)
{
    return vir_cse_table_size(block->next_order);
}

static unsigned int vir_cse_capacity(const vir_function_t *func)
{
    int values = 0;

    for (const vir_block_t *block = func->blocks; block; block = block->next)
        for (const vir_value_t *value = block->head; value; value = value->next)
            if (vir_cse_candidate(value) && ++values > INT_MAX / 2)
                return 0;
    return vir_cse_table_size(values);
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
                              vir_cse_stats_t *stats,
                              bool fold)
{
    int replacements = 0;

    for (vir_value_t *value = block->head; value; value = value->next) {
        vir_value_t *leader;
        unsigned int slot;

        if (!vir_cse_candidate(value))
            continue;
        if (stats) {
            stats->values++;
            stats->lookups++;
        }
        slot = vir_cse_hash(value, fold) & (capacity - 1);
        while ((leader = table[slot])) {
            if (stats)
                stats->probes++;
            if (vir_cse_equal(leader, value, fold))
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
        capacity = vir_cse_capacity(func);
    else
        for (vir_block_t *block = func->blocks; block; block = block->next) {
            unsigned int block_capacity = vir_cse_block_capacity(block);

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
            global ? capacity : vir_cse_block_capacity(block);

        if (!block_capacity)
            continue;
        if (!global) {
            memset(table, 0, (size_t) block_capacity * sizeof(*table));
            if (stats) {
                stats->tables++;
                stats->table_slots += (int) block_capacity;
            }
        }
        replacements += vir_cse_scan_block(func, block, table, block_capacity,
                                           stats, !global);
    }
    free(table);
    return replacements;
}

/* Equivalent pure address trees do not require new SSA identities. */
static bool vir_same_address(const vir_value_t *a,
                             const vir_value_t *b,
                             int depth)
{
    if (a == b)
        return true;
    if (!a || !b || !depth || a->opcode != b->opcode || a->type != b->type ||
        a->def_effect || b->def_effect || a->is_block_param ||
        b->is_block_param)
        return false;
    if (a->opcode == VIR_OP_CONST)
        return a->constant == b->constant;
    return a->nr_ops && a->nr_ops == b->nr_ops &&
           vir_same_address(a->op0, b->op0, depth - 1) &&
           (a->nr_ops == 1 || vir_same_address(a->op1, b->op1, depth - 1));
}

/* Memory forwarding.
 *
 * A load that reads what an earlier load or store on every path to it already
 * produced is that value. The walk follows each chain of blocks with a single
 * predecessor, so the available values on entry to a block are exactly those at
 * the end of its predecessor, and whatever defined one dominates the load.
 * Everything else starts empty: a join or loop header has paths this walk has
 * not seen.
 *
 * What a store may overwrite is decided from the address alone. An address is a
 * root object (stack slot, global or string) plus an offset, or an opaque
 * pointer plus an offset. Two different roots are different objects, so
 * accesses through them never overlap. A pointer that came from anywhere else
 * can only reach a stack slot whose address left the function (stored, passed,
 * compared or cast). Strings are read-only and survive every store.
 *
 * A call forgets everything but strings, even slots it cannot reach: carrying a
 * value across a call needs a callee-saved register or a spill, and measured on
 * a self-compile that cost more than loading it again.
 */
#define VIR_MEM_ENTRIES 32
/* Frames with more stack slots than this skip the analysis. */
#define VIR_MEM_MAX_SLOTS 65536

typedef enum {
    VIR_MEM_ANY,    /* Unknown extent: may overlap anything. */
    VIR_MEM_OPAQUE, /* Pointer of unknown origin plus offset. */
    VIR_MEM_STACK,
    VIR_MEM_GLOBAL,
    VIR_MEM_RODATA,
} vir_mem_kind_t;

typedef enum {
    VIR_MEM_DISJOINT,
    VIR_MEM_SAME,
    VIR_MEM_OVERLAP,
} vir_mem_relation_t;

typedef struct {
    vir_mem_kind_t kind;
    const vir_value_t *base; /* Opaque pointer the offset applies to. */
    const char *name;        /* Global. */
    unsigned int slot;       /* Stack. */
    int offset;
    bool variable; /* A non-constant amount was added: offset is unknown. */
    unsigned int size;
} vir_mem_loc_t;

typedef struct {
    vir_mem_loc_t loc;
    const vir_value_t *address;
    vir_value_t *value;
} vir_mem_entry_t;

typedef struct {
    int count;
    vir_mem_entry_t entries[VIR_MEM_ENTRIES];
} vir_mem_state_t;

typedef struct {
    const vir_function_t *func;
    /* One flag per stack slot: its address is used for more than loading and
     * storing through it, so a call or an unknown pointer may reach it.
     */
    unsigned char *escaped;
    unsigned int slots;
} vir_mem_t;

/* Whether the address held by @value, or something computed from it by pointer
 * arithmetic, is used for anything but the address of a load or store.
 */
static bool vir_mem_address_escapes(const vir_value_t *value, int depth)
{
    for (const vir_use_t *use = value->uses; use; use = use->next) {
        const vir_effect_t *effect = vir_use_effect(use);
        const vir_value_t *user = vir_use_user(use);
        if (effect) {
            if (use->operand != 0 || effect->address != value ||
                effect->kind == VIR_EFFECT_CALL)
                return true;
        } else if (user && user->opcode == VIR_OP_PTRADD && use->operand == 0 &&
                   depth < 8) {
            if (vir_mem_address_escapes(user, depth + 1))
                return true;
        } else {
            return true;
        }
    }
    return false;
}

/* Every stack address the function holds is in the root table, so finding them
 * does not take a walk over all its values.
 */
static bool vir_mem_init(vir_mem_t *mem, const vir_function_t *func)
{
    unsigned int slots = 0;

    mem->func = func;
    mem->escaped = NULL;
    mem->slots = 0;
    for (int i = 0; i < func->root_capacity; i++)
        for (const vir_value_t *value = func->root_table[i]; value;
             value = value->addr->root_next)
            if (value->opcode == VIR_OP_STACK_ADDR &&
                vir_value_listed(func, value) && value->addr->slot >= slots)
                slots = value->addr->slot + 1;
    if (!slots)
        return true;
    if (slots > VIR_MEM_MAX_SLOTS || !(mem->escaped = calloc(slots, 1)))
        return false;
    mem->slots = slots;
    for (int i = 0; i < func->root_capacity; i++)
        for (const vir_value_t *value = func->root_table[i]; value;
             value = value->addr->root_next)
            if (value->opcode == VIR_OP_STACK_ADDR &&
                vir_value_listed(func, value) &&
                vir_mem_address_escapes(value, 0))
                mem->escaped[value->addr->slot] = 1;
    return true;
}

/* How many plain loads and stores @func holds. */
static void vir_count_memory_effects(const vir_function_t *func,
                                     int *loads,
                                     int *stores)
{
    if (loads)
        *loads = 0;
    if (stores)
        *stores = 0;
    for (const vir_block_t *block = func->blocks; block; block = block->next)
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            if (effect->kind == VIR_EFFECT_LOAD && loads)
                ++*loads;
            else if (effect->kind == VIR_EFFECT_STORE && stores)
                ++*stores;
        }
}

static bool vir_mem_slot_escaped(const vir_mem_t *mem, unsigned int slot)
{
    return slot >= mem->slots || mem->escaped[slot];
}

static void vir_mem_locate(const vir_mem_t *mem,
                           const vir_value_t *address,
                           unsigned int size,
                           vir_mem_loc_t *loc)
{
    const vir_value_t *base = address;
    int offset = 0;
    bool variable = false;

    for (int depth = 0; depth < 16 && base->opcode == VIR_OP_PTRADD; depth++) {
        const vir_value_t *step = base->op1;
        unsigned long long bits = step->constant;
        if (step->opcode != VIR_OP_CONST) {
            variable = true;
        } else if (mem->func->pointer_bits == 32 || !(bits >> 31) ||
                   (bits >> 31) == 0x1ffffffffULL) {
            int delta = (int) (unsigned int) bits;
            if (delta > 0x3fffffff - offset || delta < -0x3fffffff - offset)
                variable = true;
            else
                offset += delta;
        } else {
            variable = true;
        }
        base = base->op0;
    }
    loc->kind = VIR_MEM_OPAQUE;
    loc->base = base;
    loc->name = NULL;
    loc->slot = 0;
    loc->offset = offset;
    loc->variable = variable;
    loc->size = size;
    if (base->opcode == VIR_OP_STACK_ADDR ||
        base->opcode == VIR_OP_GLOBAL_ADDR) {
        /* A constant offset outside the object says nothing about what it
         * reaches.
         */
        if (!variable &&
            (offset < 0 || (unsigned int) offset > base->addr->size ||
             size > base->addr->size - (unsigned int) offset)) {
            loc->kind = VIR_MEM_ANY;
        } else if (base->opcode == VIR_OP_STACK_ADDR) {
            loc->kind = VIR_MEM_STACK;
            loc->slot = base->addr->slot;
        } else {
            loc->kind = VIR_MEM_GLOBAL;
            loc->name = base->addr->name;
        }
    } else if (base->opcode == VIR_OP_RODATA_ADDR) {
        loc->kind = VIR_MEM_RODATA;
        if (base->constant > 0x3fffffffULL)
            loc->variable = true;
        else
            loc->offset = offset + (int) base->constant;
    }
}

/* Whether two locations of the same kind sit in the same object: the same
 * stack slot, the same global, the same string area, or off the same unknown
 * pointer.
 */
static bool vir_mem_same_root(const vir_mem_loc_t *a, const vir_mem_loc_t *b)
{
    if (a->kind == VIR_MEM_STACK)
        return a->slot == b->slot;
    if (a->kind == VIR_MEM_GLOBAL)
        return a->name == b->name || !strcmp(a->name, b->name);
    if (a->kind == VIR_MEM_OPAQUE)
        return a->base == b->base;
    return true;
}

static vir_mem_relation_t vir_mem_relation(const vir_mem_t *mem,
                                           const vir_mem_loc_t *a,
                                           const vir_mem_loc_t *b)
{
    if (a->kind == VIR_MEM_ANY || b->kind == VIR_MEM_ANY)
        return VIR_MEM_OVERLAP;
    if ((a->kind == VIR_MEM_RODATA) != (b->kind == VIR_MEM_RODATA))
        return VIR_MEM_DISJOINT;
    if (a->kind != b->kind) {
        /* Stack slots and globals are objects of their own; only a pointer of
         * unknown origin can reach into either, and into a slot only when the
         * slot's address got out.
         */
        const vir_mem_loc_t *other;
        if (a->kind != VIR_MEM_OPAQUE && b->kind != VIR_MEM_OPAQUE)
            return VIR_MEM_DISJOINT;
        other = a->kind == VIR_MEM_OPAQUE ? b : a;
        return other->kind == VIR_MEM_STACK &&
                       !vir_mem_slot_escaped(mem, other->slot)
                   ? VIR_MEM_DISJOINT
                   : VIR_MEM_OVERLAP;
    }
    if (!vir_mem_same_root(a, b))
        return a->kind == VIR_MEM_OPAQUE ? VIR_MEM_OVERLAP : VIR_MEM_DISJOINT;
    if (a->variable || b->variable)
        return VIR_MEM_OVERLAP;
    if (a->offset + (int) a->size <= b->offset ||
        b->offset + (int) b->size <= a->offset)
        return VIR_MEM_DISJOINT;
    return a->offset == b->offset && a->size == b->size ? VIR_MEM_SAME
                                                        : VIR_MEM_OVERLAP;
}

static void vir_mem_drop(vir_mem_state_t *state, int index)
{
    state->entries[index] = state->entries[--state->count];
}

/* Forget whatever a store to @loc may have changed. */
static void vir_mem_kill(const vir_mem_t *mem,
                         vir_mem_state_t *state,
                         const vir_mem_loc_t *loc)
{
    for (int i = 0; i < state->count;)
        if (vir_mem_relation(mem, &state->entries[i].loc, loc) !=
            VIR_MEM_DISJOINT)
            vir_mem_drop(state, i);
        else
            i++;
}

static void vir_mem_kill_call(vir_mem_state_t *state)
{
    for (int i = 0; i < state->count;) {
        const vir_mem_loc_t *loc = &state->entries[i].loc;
        if (loc->kind == VIR_MEM_RODATA)
            i++;
        else
            vir_mem_drop(state, i);
    }
}

static void vir_mem_record(vir_mem_state_t *state,
                           const vir_mem_loc_t *loc,
                           const vir_value_t *address,
                           vir_value_t *value)
{
    vir_mem_entry_t *entry;
    if (state->count == VIR_MEM_ENTRIES)
        vir_mem_drop(state, 0);
    entry = &state->entries[state->count++];
    entry->loc = *loc;
    entry->address = address;
    entry->value = value;
}

static unsigned int vir_mem_access_size(const vir_function_t *func,
                                        vir_type_t type)
{
    return (unsigned int) (vir_type_width(func, type) + 7) / 8;
}

/* The earlier value that @loc, accessed as @type, is known to hold. */
static vir_value_t *vir_mem_available(const vir_mem_t *mem,
                                      const vir_mem_state_t *state,
                                      const vir_value_t *address,
                                      const vir_mem_loc_t *loc,
                                      vir_type_t type)
{
    for (int i = state->count - 1; i >= 0; i--) {
        const vir_mem_entry_t *entry = &state->entries[i];
        vir_mem_relation_t relation;
        if (entry->value->type != type)
            continue;
        if (entry->address == address)
            return entry->value;
        relation = vir_mem_relation(mem, &entry->loc, loc);
        if (relation == VIR_MEM_SAME ||
            (relation == VIR_MEM_OVERLAP &&
             vir_same_address(entry->address, address, 8)))
            return entry->value;
    }
    return NULL;
}

static int vir_mem_scan_block(const vir_mem_t *mem,
                              const vir_block_t *block,
                              vir_mem_state_t *state)
{
    int changes = 0;

    for (vir_effect_t *effect = block->effects; effect; effect = effect->next) {
        vir_mem_loc_t loc;
        vir_value_t *known;
        vir_type_t type;
        switch (effect->kind) {
        case VIR_EFFECT_LOAD:
            type = effect->result->type;
            vir_mem_locate(mem, effect->address,
                           vir_mem_access_size(mem->func, type), &loc);
            known = vir_mem_available(mem, state, effect->address, &loc, type);
            if (known && known != effect->result) {
                if (effect->result->uses) {
                    vir_replace_all_uses_unchecked(effect->result, known);
                    changes++;
                }
            } else if (!known) {
                vir_mem_record(state, &loc, effect->address, effect->result);
            }
            break;
        case VIR_EFFECT_STORE:
            type = effect->stored_value->type;
            vir_mem_locate(mem, effect->address,
                           vir_mem_access_size(mem->func, type), &loc);
            vir_mem_kill(mem, state, &loc);
            vir_mem_record(state, &loc, effect->address, effect->stored_value);
            break;
        case VIR_EFFECT_CALL:
            vir_mem_kill_call(state);
            break;
        default:
            state->count = 0;
            break;
        }
    }
    return changes;
}

static int vir_forward_memory(vir_function_t *func)
{
    vir_mem_t mem;
    int blocks = func->block_count;
    int slots = blocks < 16 ? blocks : 16;
    int changes = 0;
    unsigned char *visited = NULL;
    vir_mem_state_t *states = NULL;
    vir_block_t **work = NULL;
    int *owner;
    int *spare;
    int loads, stores;

    vir_count_memory_effects(func, &loads, &stores);
    /* A load needs an earlier access to take its value from. */
    if (blocks <= 0 || !loads || loads + stores < 2)
        return 0;
    /* The walk indexes its tables by block position. */
    for (const vir_block_t *block = func->blocks; block; block = block->next)
        if (vir_block_index(func, block) < 0)
            return 0;
    if (!vir_mem_init(&mem, func))
        return 0;
    states = malloc(sizeof(*states) * (size_t) slots);
    work = malloc(sizeof(*work) * (size_t) slots +
                  2 * sizeof(*owner) * (size_t) slots + (size_t) blocks);
    if (!states || !work)
        goto done;
    owner = (int *) (work + slots);
    spare = owner + slots;
    visited = (unsigned char *) (spare + slots);
    memset(visited, 0, (size_t) blocks);
    /* Phase 0 starts a walk at every block that is not the only way into its
     * successor; phase 1 sweeps up what those could not reach (a state pool
     * that ran dry, or a cycle of single-entry blocks).
     */
    for (int phase = 0; phase < 2; phase++) {
        for (vir_block_t *root = func->blocks; root; root = root->next) {
            int depth = 1;
            int spares = 0;
            bool single = root != func->blocks && root->incoming &&
                          !root->incoming->next_incoming;
            if (visited[root->index] || (!phase && single))
                continue;
            for (int i = 1; i < slots; i++)
                spare[spares++] = i;
            states[0].count = 0;
            work[0] = root;
            owner[0] = 0;
            while (depth) {
                vir_block_t *block = work[--depth];
                int mine = owner[depth];
                vir_block_t *child[2];
                int children = 0;
                bool handed = false;
                visited[block->index] = 1;
                changes += vir_mem_scan_block(&mem, block, &states[mine]);
                for (vir_edge_t *edge = block->outgoing; edge && children < 2;
                     edge = edge->next_outgoing) {
                    vir_block_t *to = edge->to;
                    if (to != func->blocks && !visited[to->index] &&
                        to->incoming && !to->incoming->next_incoming)
                        child[children++] = to;
                }
                for (int i = 0; i < children; i++) {
                    int slot = mine;
                    if (i + 1 < children) {
                        if (!spares)
                            continue;
                        slot = spare[--spares];
                        states[slot].count = states[mine].count;
                        memcpy(states[slot].entries, states[mine].entries,
                               (size_t) states[mine].count *
                                   sizeof(states[mine].entries[0]));
                    } else {
                        handed = true;
                    }
                    work[depth] = child[i];
                    owner[depth++] = slot;
                }
                if (!handed)
                    spare[spares++] = mine;
            }
        }
    }
done:
    free(states);
    free(work);
    free(mem.escaped);
    return changes;
}

int vir_local_cse_with_stats(vir_function_t *func,
                             vir_opt_level_t opt_level,
                             vir_cse_stats_t *stats)
{
    int changes = vir_cse_run(func, opt_level, stats, false);
    if (changes < 0 || opt_level == VIR_OPT_O0)
        return changes;
    int loads = vir_forward_memory(func);
    if (stats)
        stats->replacements += loads;
    return changes + loads;
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

/* Move the values a builder appended after OLD_TAIL in front of the value at
 * *LINK, giving them its order so they keep its place among the effects.
 * Return the link that now holds that value.
 */
static vir_value_t **vir_move_appended_before(vir_block_t *block,
                                              vir_value_t *old_tail,
                                              vir_value_t **link)
{
    vir_value_t *value = *link;
    vir_value_t *first = old_tail->next;
    vir_value_t *last = block->tail;
    old_tail->next = NULL;
    last->next = value;
    *link = first;
    for (vir_value_t *inserted = first; inserted != value;
         inserted = inserted->next)
        inserted->order = value->order;
    vir_repair_value_positions(block);
    return &last->next;
}

/* SSA parameter replacement and CSE expose identities after construction. Reuse
 * the builders' rules, placing any fresh pure replacement before its original
 * definition. Every replacement is then either new and in front of the value or
 * one of its transitive operands, so it dominates all existing uses.
 */
int vir_normalize(vir_function_t *func, vir_opt_level_t opt_level)
{
    int changed = 0;
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        for (vir_value_t **link = &block->head; *link; link = &(*link)->next) {
            vir_value_t *value = *link;
            if (!value->uses || value->def_effect)
                continue;
            vir_value_t *replacement = NULL;
            vir_value_t *old_tail = block->tail;
            if (value->nr_ops == 2 && vir_type_is_i32_or_i64(value->type)) {
                replacement = vir_normalize_binary(func, block, value->op0,
                                                   value->op1, value->opcode);
                if (!replacement)
                    replacement = vir_strength_reduce_integer(
                        func, block, value->op0, value->op1, value->opcode);
            } else if (value->nr_ops == 1) {
                replacement = vir_unary_involution(value->opcode, value->op0);
                if (!replacement && (value->opcode == VIR_OP_ZEXT ||
                                     value->opcode == VIR_OP_SEXT ||
                                     value->opcode == VIR_OP_TRUNC))
                    replacement = vir_cast_chain(func, block, value->opcode,
                                                 value->type, value->op0);
            } else if (value->opcode == VIR_OP_PTRADD) {
                replacement =
                    vir_ptradd_fold(func, block, value->op0, value->op1);
            }
            if (!replacement || replacement->type != value->type)
                continue;
            if (block->tail != old_tail)
                link = vir_move_appended_before(block, old_tail, link);
            vir_replace_all_uses_unchecked(value, replacement);
            changed++;
        }
    }
    return changed;
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

/* Delay nontrapping pointer arithmetic until old operands have been consumed,
 * so an outgoing pointer can reuse their register without changing effects.
 */
static bool vir_sink_pointer_adds(vir_block_t *block)
{
    bool changed = false;
    vir_effect_t *barrier = block->effects;
    for (vir_value_t **link = &block->head; *link; link = &(*link)->next) {
        vir_value_t *value = *link;
        if (value->opcode != VIR_OP_PTRADD)
            continue;
        int limit = block->next_order;
        for (vir_use_t *use = value->uses; use; use = use->next)
            if (vir_use_block(use) == block && vir_use_order(use) < limit)
                limit = vir_use_order(use);

        /* The value walk only advances, even after sinking a value. Keep the
         * first following barrier rather than rescanning all earlier effects.
         */
        while (barrier && (barrier->order <= value->order ||
                           barrier->kind == VIR_EFFECT_LOAD))
            barrier = barrier->next;
        if (barrier && barrier->order < limit)
            limit = barrier->order;
        if (limit <= value->order + 1)
            continue;
        vir_value_t *after = NULL;
        for (int operand = 0; operand < value->nr_ops; operand++) {
            vir_value_t *input = operand ? value->op1 : value->op0;
            for (vir_use_t *use = input->uses; use; use = use->next) {
                vir_value_t *user = vir_use_user(use);
                if (!user && vir_use_effect(use))
                    user = vir_use_effect(use)->result;
                if (!user || user->block != block ||
                    user->order <= value->order || user->order >= limit)
                    continue;
                if (!after || user->order > after->order)
                    after = user;
            }
        }
        if (!after)
            continue;
        *link = value->next;
        int order = after->order;
        for (vir_value_t *node = block->head; node; node = node->next)
            if (node->order > order)
                node->order++;
        for (vir_effect_t *effect = block->effects; effect;
             effect = effect->next)
            if (effect->order > order)
                effect->order++;
        block->next_order++;
        value->order = order + 1;
        value->next = after->next;
        after->next = value;
        link = &after->next;
        changed = true;
    }
    return changed;
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
        address->addr->kind != VIR_ADDRESS_STACK || !address->uses)
        return false;
    for (vir_block_t *owner = func->blocks; owner; owner = owner->next)
        for (vir_value_t *value = owner->head; value; value = value->next)
            if (value != address && value->opcode == VIR_OP_STACK_ADDR &&
                value->addr->slot == address->addr->slot)
                return false;
    for (use = address->uses; use; use = use->next) {
        effect = vir_use_effect(use);
        if (use->operand != 0 || !effect || effect->block != block ||
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

/* Whether a store to @later covers every byte of the one to @earlier. */
static bool vir_mem_covers(const vir_value_t *later_address,
                           const vir_mem_loc_t *later,
                           const vir_value_t *earlier_address,
                           const vir_mem_loc_t *earlier)
{
    if (later->size >= earlier->size &&
        (later_address == earlier_address ||
         vir_same_address(later_address, earlier_address, 8)))
        return true;
    if (later->kind == VIR_MEM_ANY || later->kind != earlier->kind ||
        later->variable || earlier->variable)
        return false;
    if (!vir_mem_same_root(later, earlier))
        return false;
    return later->offset <= earlier->offset &&
           later->offset + (int) later->size >=
               earlier->offset + (int) earlier->size;
}

/* How far past a store the search for its overwriting store goes. */
#define VIR_DEAD_STORE_WINDOW 64

/* Whether nothing can read the bytes @store writes before a later store in its
 * block overwrites all of them. A read of a location that may overlap, a
 * volatile access, and a call that can reach the location end the search; so
 * does the end of the block, because other blocks are not followed.
 */
static bool vir_store_overwritten(const vir_mem_t *mem,
                                  const vir_effect_t *store,
                                  const vir_mem_loc_t *loc)
{
    int window = VIR_DEAD_STORE_WINDOW;

    for (const vir_effect_t *effect = store->next; effect && window--;
         effect = effect->next) {
        vir_mem_loc_t other;
        switch (effect->kind) {
        case VIR_EFFECT_LOAD:
            if (!effect->result->uses)
                break;
            vir_mem_locate(mem, effect->address,
                           vir_mem_access_size(mem->func, effect->result->type),
                           &other);
            if (vir_mem_relation(mem, loc, &other) != VIR_MEM_DISJOINT)
                return false;
            break;
        case VIR_EFFECT_STORE:
            vir_mem_locate(
                mem, effect->address,
                vir_mem_access_size(mem->func, effect->stored_value->type),
                &other);
            if (vir_mem_covers(effect->address, &other, store->address, loc))
                return true;
            break;
        case VIR_EFFECT_CALL:
            if (loc->kind != VIR_MEM_STACK ||
                vir_mem_slot_escaped(mem, loc->slot))
                return false;
            break;
        default:
            return false;
        }
    }
    return false;
}

/* Remove stores nothing can observe: ones a later store in the block
 * overwrites, and every store to a stack slot whose address stays inside the
 * function and that nothing reads. With @overwrite false only the second kind
 * is looked for: the round after unused loads went cannot find new overwritten
 * stores.
 */
static int vir_eliminate_dead_stores(vir_function_t *func, bool overwrite)
{
    vir_mem_t mem;
    unsigned char *read = NULL;
    bool read_any = false;
    int removed = 0;
    int stores;

    vir_count_memory_effects(func, NULL, &stores);
    if (!stores || !vir_mem_init(&mem, func))
        return 0;
    if (mem.slots && !(read = calloc(mem.slots, 1))) {
        free(mem.escaped);
        return 0;
    }
    /* Without a stack slot no store can be dead for want of a reader. */
    for (vir_block_t *block = mem.slots ? func->blocks : NULL; block;
         block = block->next)
        for (vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            vir_mem_loc_t loc;
            if (effect->kind == VIR_EFFECT_STORE ||
                effect->kind == VIR_EFFECT_CALL)
                continue;
            if (effect->kind == VIR_EFFECT_VOLATILE) {
                read_any = true;
                continue;
            }
            /* Loads read the slot, and so does a volatile store: it may be
             * watched from outside.
             */
            vir_mem_locate(
                &mem, effect->address,
                vir_mem_access_size(func, vir_effect_is_load(effect->kind)
                                              ? effect->result->type
                                              : effect->stored_value->type),
                &loc);
            if (loc.kind == VIR_MEM_STACK && loc.slot < mem.slots)
                read[loc.slot] = 1;
            else if (loc.kind != VIR_MEM_OPAQUE && loc.kind != VIR_MEM_GLOBAL &&
                     loc.kind != VIR_MEM_RODATA)
                read_any = true;
        }
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        bool changed = false;
        int pending = 0;
        for (const vir_effect_t *e = block->effects; e; e = e->next)
            if (e->kind == VIR_EFFECT_STORE)
                pending++;
        for (vir_effect_t **link = &block->effects; *link;) {
            vir_effect_t *effect = *link;
            vir_mem_loc_t loc;
            bool dead = false;
            if (effect->kind == VIR_EFFECT_STORE) {
                pending--;
                vir_mem_locate(
                    &mem, effect->address,
                    vir_mem_access_size(func, effect->stored_value->type),
                    &loc);
                dead = (loc.kind == VIR_MEM_STACK &&
                        !vir_mem_slot_escaped(&mem, loc.slot) && !read_any &&
                        !read[loc.slot]) ||
                       (overwrite && pending &&
                        vir_store_overwritten(&mem, effect, &loc));
            }
            if (dead) {
                vir_remove_effect_uses(effect);
                *link = effect->next;
                changed = true;
                removed++;
            } else {
                link = &effect->next;
            }
        }
        if (changed)
            vir_repair_value_positions(block);
    }
    free(read);
    free(mem.escaped);
    return removed;
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
    int stores;
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
    stores = vir_eliminate_dead_stores(func, true);
    capacity = func->next_value_id;
    if (capacity <= 0)
        return stores;
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
        bool changed = false;

        while (*link) {
            vir_value_t *value = *link;

            if (dead[value->id]) {
                *link = value->next;
                value->member = NULL;
                removed++;
                changed = true;
            } else {
                link = &value->next;
            }
        }
        while (*effect_link) {
            vir_effect_t *effect = *effect_link;

            if (effect->kind == VIR_EFFECT_LOAD && effect->result &&
                dead[effect->result->id]) {
                *effect_link = effect->next;
                changed = true;
            } else {
                effect_link = &effect->next;
            }
        }
        changed |= vir_sink_pointer_adds(block);
        if (changed)
            vir_repair_value_positions(block);
    }
    /* Unused loads kept their slots read; with them gone, stores to those
     * slots may be dead too.
     */
    if (removed)
        stores += vir_eliminate_dead_stores(func, false);
    result = removed + stores;
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
        if (vir_use_user(use) && vir_use_user(use)->block != current_block)
            vir_sccp_queue_block(worklist, func, vir_use_user(use)->block,
                                 true);
        else if (vir_use_branch_block(use) &&
                 vir_use_branch_block(use) != current_block)
            vir_sccp_queue_block(worklist, func, vir_use_branch_block(use),
                                 true);
        else if (vir_use_edge(use)) {
            int index = vir_use_edge(use)->sccp_index;

            if (index >= 0 && executable_edges[index])
                vir_sccp_queue_block(worklist, func, vir_use_edge(use)->to,
                                     true);
        }
    }
}

static bool vir_sccp_param(const vir_value_t *param,
                           int index,
                           const unsigned char *executable_edges,
                           unsigned char *state,
                           unsigned long long *constant)
{
    const vir_edge_t *edge;
    int seen = 0;
    int next_state = VIR_SCCP_UNDEF;
    unsigned long long next_constant = 0;

    for (edge = param->block->incoming; edge; edge = edge->next_incoming) {
        int edge_index = edge->sccp_index;

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
    int edge_index = edge->sccp_index;

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

    /* The solver never changes the CFG, so this edge numbering holds until the
     * edges are rewritten after it.
     */
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        block_count++;
        for (vir_edge_t *edge = block->outgoing; edge;
             edge = edge->next_outgoing)
            edge->sccp_index = edge_count++;
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
        vir_block_t *block = func->block_table[block_index];

        worklist.head = (worklist.head + 1) % worklist.capacity;
        worklist.count--;
        if (worklist.queued_blocks[block_index] != 2)
            goto fail;
        worklist.queued_blocks[block_index] = 1;
        if (passes)
            (*passes)++;
        int param_index = 0;

        for (vir_value_t *value = block->params; value;
             value = value->param_next, param_index++) {
            if (vir_sccp_param(value, param_index, executable_edges, state,
                               constant))
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

/* Only an insertion allocates new arrays; a removal compacts the edge's own,
 * which sealing does for every trivial parameter it drops.
 */
typedef struct {
    vir_edge_t *edge;
    vir_value_t **args;
    vir_use_t *uses;
    vir_value_t *inserted;
} vir_edge_resize_t;

/* Most blocks have few predecessors, so their resize records fit inline and an
 * edit needs no allocation of its own. An edit must not move once prepared.
 */
#define VIR_PARAM_EDIT_INLINE 4

typedef struct {
    vir_edge_resize_t *edges;
    vir_edge_resize_t inline_edges[VIR_PARAM_EDIT_INLINE];
    int count;
    int old_count;
    int index;
    bool insert;
} vir_param_edit_t;

static void vir_param_edit_release(vir_param_edit_t *edit)
{
    if (edit->edges != edit->inline_edges)
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
    if (edit->count <= VIR_PARAM_EDIT_INLINE)
        edit->edges = edit->inline_edges;
    else if (!(edit->edges =
                   calloc((size_t) edit->count, sizeof(*edit->edges))))
        return 0;
    edit->old_count = old_count;
    edit->index = index;
    edit->insert = insert;
    new_count = old_count + 1;
    for (edge = block->incoming, i = 0; edge; edge = edge->next_incoming, i++) {
        edit->edges[i].edge = edge;
        if (!insert)
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
        vir_value_t **args = edit->edges[i].args;
        int source, destination = 0;

        /* A removal shifts the arguments down within the edge's own arrays, so
         * their uses are unlinked first and relinked in index order, as
         * vir_edge_replace_args does.
         */
        if (!edit->insert) {
            vir_edge_unlink_uses(edge);
            for (source = edit->index + 1; source < edge->arg_count; source++)
                edge->args[source - 1] = edge->args[source];
            edge->arg_count = new_count;
            vir_edge_link_uses(edge);
            continue;
        }
        for (source = 0; source < edge->arg_count; source++) {
            if (source == edit->index)
                args[destination++] = edit->edges[i].inserted;
            args[destination++] = edge->args[source];
        }
        if (edit->index == edge->arg_count)
            args[destination++] = edit->edges[i].inserted;
        vir_edge_replace_args(edge, args, edit->edges[i].uses, new_count);
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
        param->member = NULL;
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
    if (!func || !needle || needle->index < 0 ||
        needle->index >= func->block_count ||
        func->block_table[needle->index] != needle)
        return -1;
    return needle->index;
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

static bool vir_has_value_with_context(const vir_function_t *func,
                                       const vir_dominance_t *dominance,
                                       const vir_value_t *needle)
{
    if (!needle)
        return false;

    /* A value still linked into a block of this function answers in O(1): only
     * vir.c links and unlinks values, and it keeps @member in step. Keep the
     * full scan as a fallback for detached values, struct copies and malformed
     * ownership metadata.
     */
    if (needle->member == needle &&
        (dominance ? vir_dominance_index(dominance, needle->block) >= 0
                   : vir_has_block(func, needle->block)))
        return true;
    return vir_count_values(func, needle, 0) != 0;
}

static bool vir_has_value(const vir_function_t *func, const vir_value_t *needle)
{
    return vir_has_value_with_context(func, NULL, needle);
}

static bool vir_verify_value_id(const vir_function_t *func,
                                unsigned char *seen,
                                int id)
{
    if (id < 0)
        return false;
    return id < func->next_value_id ? !seen[id]++
                                    : vir_count_values(func, NULL, id) == 1;
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
    return func->block_count;
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
        const vir_block_t *block = func->block_table[work[head++]];
        const vir_edge_t *edge;

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

/* Whether the entry reaches @block. Anything that stops the walk counts as
 * reachable, so a caller relaxing a check for dead code stays strict.
 */
static bool vir_block_reachable(const vir_function_t *func,
                                const vir_block_t *block)
{
    int index = vir_block_index(func, block);
    unsigned char *reachable =
        calloc((size_t) func->block_count, sizeof(*reachable));
    int *work = malloc((size_t) func->block_count * sizeof(*work));
    bool result = index < 0 || !reachable || !work ||
                  !vir_mark_reachable_blocks(func, NULL, reachable, work) ||
                  reachable[index];

    free(reachable);
    free(work);
    return result;
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

    /* If the use is unreachable, no path can avoid the definition and the
     * result is vacuously true. The same walk also handles that case.
     */
    use_index =
        reachable && work &&
        vir_mark_reachable_blocks(func, value->block, reachable, work) &&
        !reachable[use_index];
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
    int *order, *post, *stack;
    vir_edge_t **cursor;
    unsigned char *seen;

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

    /* The maximal fixed point does not depend on the order blocks are visited
     * in, but how many rounds it takes does: reverse postorder from the entry
     * settles a reducible graph in two. Blocks the walk misses follow in list
     * order.
     */
    cursor =
        malloc((size_t) count * (sizeof(*cursor) + 3 * sizeof(*order) + 1));
    if (!cursor) {
        vir_dominance_release(dominance);
        return false;
    }
    order = (int *) (cursor + count);
    post = order + count;
    stack = post + count;
    seen = (unsigned char *) (stack + count);
    memset(seen, 0, (size_t) count);
    {
        int depth = 0, posted = 0, visits = 0;
        seen[0] = 1;
        stack[0] = 0;
        cursor[0] = dominance->blocks[0]->outgoing;
        while (depth >= 0) {
            vir_edge_t *edge = cursor[depth];
            if (edge) {
                int to = vir_dominance_index(dominance, edge->to);
                cursor[depth] = edge->next_outgoing;
                if (to >= 0 && !seen[to]) {
                    seen[to] = 1;
                    depth++;
                    stack[depth] = to;
                    cursor[depth] = dominance->blocks[to]->outgoing;
                }
            } else {
                post[posted++] = stack[depth--];
            }
        }
        while (posted)
            order[visits++] = post[--posted];
        for (i = 0; i < count; i++)
            if (!seen[i])
                order[visits++] = i;
    }
    for (;;) {
        bool changed = false;
        for (int visit = 0; visit < count; visit++) {
            const vir_edge_t *edge;
            vir_block_t *current;
            int have_predecessor = 0;
            int word;

            i = order[visit];
            if (i == 0)
                continue;
            current = dominance->blocks[i];

            for (word = 0; word < words; word++)
                dominance->candidate[word] = ~0UL;
            for (edge = current->incoming; edge; edge = edge->next_incoming) {
                int predecessor = vir_dominance_index(dominance, edge->from);
                if (predecessor < 0) {
                    free(cursor);
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
    free(cursor);
    return true;
}

/* The one value @param, the @index-th parameter of @block, receives on every
 * incoming edge apart from itself, or NULL when the edges disagree.
 */
static vir_value_t *vir_param_trivial_value(const vir_block_t *block,
                                            const vir_value_t *param,
                                            int index)
{
    vir_value_t *candidate = NULL;

    for (vir_edge_t *edge = block->incoming; edge; edge = edge->next_incoming) {
        vir_value_t *value = edge->args[index];
        if (value == param)
            continue;
        if (candidate && candidate != value)
            return NULL;
        candidate = value;
    }
    return candidate;
}

/* Drop loop-invariant and forwarding parameters without source SSA bindings.
 * The entry block's parameters are the function's arguments, whose caller
 * supplies a value no edge shows, so they always stay.
 */
static int vir_simplify_params_with_context(vir_function_t *func,
                                            const vir_dominance_t *dominance)
{
    int changed = 0, previous;
    do {
        previous = changed;
        for (vir_block_t *block = func->blocks->next; block;
             block = block->next) {
            int index = 0;
            for (vir_value_t *param = block->params, *next; param;
                 param = next) {
                vir_value_t *candidate =
                    vir_param_trivial_value(block, param, index);
                next = param->param_next;
                if (!candidate || !vir_replacement_dominates_with_context(
                                      func, dominance, param, candidate)) {
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

/* Dominance is built only once some parameter has a single incoming value. */
int vir_simplify_params(vir_function_t *func, vir_opt_level_t opt_level)
{
    vir_dominance_t dominance;
    int changed;

    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0 || !func->blocks)
        return 0;
    for (vir_block_t *block = func->blocks->next; block; block = block->next) {
        int index = 0;
        for (vir_value_t *param = block->params; param;
             param = param->param_next, index++)
            if (vir_param_trivial_value(block, param, index))
                goto simplify;
    }
    return 0;
simplify:
    if (!vir_dominance_init(func, &dominance))
        return -1;
    changed = vir_simplify_params_with_context(func, &dominance);
    vir_dominance_release(&dominance);
    return changed;
}

/* Inlining limits in VIR values. With a main, only functions reachable from it
 * are emitted, so any callee whose calls are all inlined leaves the image,
 * static or not. Linkage still predicts the cost: a static function is a helper
 * with a few local callers, so it may hold up to VIR_INLINE_STATIC_LIMIT, while
 * an extern one tends to have callers that are not inlined and keep it, so it
 * is copied only when tiny, or into a loop, where the call costs every
 * iteration. Measured on a self-compile, raising the extern limits or dropping
 * the static one both made the generated compiler larger and slower. One caller
 * absorbs at most VIR_INLINE_CALLER_BUDGET cloned values.
 */
#define VIR_INLINE_STATIC_LIMIT 40
#define VIR_INLINE_EXTERN_LIMIT 6
#define VIR_INLINE_LOOP_LIMIT 16
#define VIR_INLINE_CALLER_BUDGET 400
#define VIR_INLINE_MAX_BLOCKS 16
#define VIR_INLINE_MAX_ARGS 16

/* The value count of @callee if it can replace @call and holds at most @limit
 * values, else -1: a leaf with no stack objects whose parameters and returns
 * match the call's types exactly, so no ABI conversion is skipped.
 */
static int vir_inline_size(const vir_function_t *callee,
                           const vir_effect_t *call,
                           int limit)
{
    const vir_block_t *entry = callee->blocks;
    int values = 0;
    if (!entry || callee->block_count > VIR_INLINE_MAX_BLOCKS ||
        entry->param_count != call->arg_count ||
        (call->signature &&
         (call->signature->is_variadic || call->signature->result.is_bool)))
        return -1;
    int index = 0;
    for (const vir_value_t *param = entry->params; param;
         param = param->param_next, index++)
        if (param->type != call->args[index]->type ||
            (call->signature && call->signature->params[index].is_bool))
            return -1;
    for (const vir_block_t *block = entry; block; block = block->next) {
        if (block->param_count > VIR_INLINE_MAX_ARGS ||
            block->terminator == VIR_TERM_NONE ||
            (block->terminator == VIR_TERM_RETURN && call->result &&
             (!block->return_value ||
              block->return_value->type != call->result_type)))
            return -1;
        for (const vir_effect_t *effect = block->effects; effect;
             effect = effect->next)
            if ((effect->kind != VIR_EFFECT_LOAD &&
                 effect->kind != VIR_EFFECT_VOLATILE_LOAD &&
                 effect->kind != VIR_EFFECT_STORE &&
                 effect->kind != VIR_EFFECT_VOLATILE_STORE) ||
                (vir_effect_is_store(effect->kind) && !effect->stored_value))
                return -1;
        for (const vir_value_t *value = block->head; value; value = value->next)
            if (value->opcode == VIR_OP_STACK_ADDR ||
                value->opcode == VIR_OP_CALL || value->opcode == VIR_OP_PARAM ||
                ++values > limit)
                return -1;
    }
    return values;
}

/* Copy the pure @value into @block over the already mapped operands. */
static vir_value_t *vir_clone_value(vir_function_t *func,
                                    vir_block_t *block,
                                    const vir_value_t *value,
                                    vir_value_t *op0,
                                    vir_value_t *op1)
{
    switch (value->opcode) {
    case VIR_OP_CONST:
        return vir_const_int(func, block, value->type, value->constant);
    case VIR_OP_GLOBAL_ADDR:
        return vir_global_addr(func, block, value->addr->name,
                               value->addr->size, value->addr->alignment);
    case VIR_OP_RODATA_ADDR:
        return vir_rodata_addr(func, block, (unsigned int) value->constant);
    case VIR_OP_FUNC_ADDR:
        return vir_function_addr(func, block, value->addr->name);
    default:
        if (value->nr_ops < 1 || value->nr_ops > 2 || !op0 ||
            (value->nr_ops == 2 && !op1))
            return NULL;
        return vir_user_operation(func, block, value->opcode, value->type, op0,
                                  value->nr_ops == 2 ? op1 : NULL);
    }
}

/* Clone one callee value whose operands are already mapped. */
static vir_value_t *vir_inline_value(vir_function_t *caller,
                                     vir_block_t *block,
                                     const vir_value_t *value,
                                     vir_value_t **map)
{
    return vir_clone_value(caller, block, value,
                           value->op0 ? map[value->op0->id] : NULL,
                           value->op1 ? map[value->op1->id] : NULL);
}

static bool vir_inline_effect(vir_function_t *caller,
                              vir_block_t *block,
                              const vir_effect_t *effect,
                              vir_value_t **map)
{
    vir_value_t *address = effect->address ? map[effect->address->id] : NULL;
    if (!address)
        return false;
    if (vir_effect_is_load(effect->kind)) {
        vir_value_t *value =
            vir_load_impl(caller, block, address, effect->result->type,
                          effect->kind == VIR_EFFECT_VOLATILE_LOAD);
        map[effect->result->id] = value;
        return value != NULL;
    }
    vir_value_t *stored = map[effect->stored_value->id];
    if (!stored)
        return false;
    return (effect->kind == VIR_EFFECT_VOLATILE_STORE
                ? vir_volatile_store(caller, block, address, stored)
                : vir_store(caller, block, address, stored)) != NULL;
}

/* Clone @source's values and effects into @block in their shared order. */
static bool vir_inline_block_body(vir_function_t *caller,
                                  vir_block_t *block,
                                  const vir_block_t *source,
                                  vir_value_t **map)
{
    const vir_value_t *value = source->head;
    const vir_effect_t *effect = source->effects;
    while (value || effect) {
        if (value && value->def_effect) {
            value = value->next;
            continue;
        }
        if (effect && (!value || effect->order <= value->order)) {
            if (!vir_inline_effect(caller, block, effect, map))
                return false;
            effect = effect->next;
            continue;
        }
        if (!(map[value->id] = vir_inline_value(caller, block, value, map)))
            return false;
        value = value->next;
    }
    return true;
}

/* An edge's arguments mapped into the caller; parameter counts are bounded. */
static void vir_inline_edge_args(const vir_edge_t *edge,
                                 vir_value_t **map,
                                 vir_value_t **args)
{
    for (int i = 0; i < edge->arg_count; i++)
        args[i] = map[edge->args[i]->id];
}

/* Move everything @call's block holds after @call -- values, effects and the
 * terminator with its edges -- into a new block, and return it. The caller
 * unlinks @call next, which repairs @call's block.
 */
static vir_block_t *vir_inline_split(vir_function_t *caller, vir_effect_t *call)
{
    vir_block_t *block = call->block, *rest = vir_block_create(caller);
    if (!rest)
        return NULL;
    vir_value_t **link = &block->head;
    while (*link && (*link)->order <= call->order)
        link = &(*link)->next;
    rest->head = *link;
    *link = NULL;
    for (vir_value_t *value = rest->head; value; value = value->next)
        value->block = rest;
    rest->effects = call->next;
    call->next = NULL;
    for (vir_effect_t *effect = rest->effects; effect; effect = effect->next)
        effect->block = rest;

    /* The terminator fields of vir_block_t, which move as one. */
    rest->terminator = block->terminator;
    rest->return_value = block->return_value;
    rest->branch_condition = block->branch_condition;
    rest->true_edge = block->true_edge;
    rest->false_edge = block->false_edge;
    rest->outgoing = block->outgoing;
    for (vir_edge_t *edge = rest->outgoing; edge; edge = edge->next_outgoing)
        edge->from = rest;
    vir_value_t *owned[2] = {rest->return_value, rest->branch_condition};
    vir_use_owner_t kinds[2] = {VIR_USE_RETURN, VIR_USE_BRANCH};
    for (int i = 0; i < 2; i++)
        for (vir_use_t *use = owned[i] ? owned[i]->uses : NULL; use;
             use = use->next)
            if (vir_use_matches(use, kinds[i], block, -1))
                vir_use_set_owner(use, kinds[i], rest);
    block->terminator = VIR_TERM_NONE;
    block->return_value = NULL;
    block->branch_condition = NULL;
    block->true_edge = NULL;
    block->false_edge = NULL;
    block->outgoing = NULL;
    vir_repair_value_positions(rest);
    return rest;
}

/* Store the indices of the callee blocks reachable from its entry in reverse
 * post-order, which reaches every dominator of a block before the block, so
 * every operand is cloned before its users; returns how many.
 */
static int vir_inline_order(const vir_function_t *callee,
                            int *order,
                            unsigned char *seen)
{
    vir_block_t *stack[VIR_INLINE_MAX_BLOCKS];
    vir_edge_t *cursor[VIR_INLINE_MAX_BLOCKS];
    int depth = 0, count = callee->block_count;
    stack[depth] = callee->blocks;
    cursor[depth++] = callee->blocks->outgoing;
    seen[callee->blocks->index] = 1;
    while (depth) {
        vir_edge_t *edge = cursor[depth - 1];
        if (!edge) {
            order[--count] = stack[--depth]->index;
            continue;
        }
        cursor[depth - 1] = edge->next_outgoing;
        if (!seen[edge->to->index]) {
            seen[edge->to->index] = 1;
            stack[depth] = edge->to;
            cursor[depth++] = edge->to->outgoing;
        }
    }
    /* Shift the reachable blocks to the front. */
    int reachable = callee->block_count - count;
    for (int i = 0; i < reachable; i++)
        order[i] = order[count + i];
    return reachable;
}

/* Replace @call with a copy of @callee and return the block now holding what
 * followed the call, or NULL. The call's block jumps to the cloned entry with
 * the arguments, and every cloned return jumps to that block, whose parameter
 * carries the result.
 */
static vir_block_t *vir_inline_call(vir_function_t *caller,
                                    vir_effect_t *call,
                                    const vir_function_t *callee)
{
    int order[VIR_INLINE_MAX_BLOCKS];
    unsigned char reachable[VIR_INLINE_MAX_BLOCKS] = {0};
    vir_block_t *clone_of[VIR_INLINE_MAX_BLOCKS] = {0};
    vir_value_t *args[VIR_INLINE_MAX_ARGS], *other[VIR_INLINE_MAX_ARGS];
    vir_block_t *rest = NULL, *result_block = NULL;
    vir_value_t *result = NULL;
    vir_value_t **map =
        calloc((size_t) callee->next_value_id + 1, sizeof(*map));
    if (!map)
        goto done;
    int count = vir_inline_order(callee, order, reachable);

    /* Blocks and parameters first: edges and operands may name any of them. */
    for (int i = 0; i < count; i++) {
        const vir_block_t *source = callee->block_table[order[i]];
        if (!(clone_of[order[i]] = vir_block_create(caller)))
            goto done;
        for (vir_value_t *param = source->params; param;
             param = param->param_next)
            if (!(map[param->id] = vir_block_add_param(
                      caller, clone_of[order[i]], param->type)))
                goto done;
    }
    for (int i = 0; i < count; i++)
        if (!vir_inline_block_body(caller, clone_of[order[i]],
                                   callee->block_table[order[i]], map))
            goto done;

    /* Splice the clone in, then give each cloned block its terminator. */
    if (!(rest = vir_inline_split(caller, call)))
        goto done;
    if (call->result && call->result->uses) {
        if (!(result = vir_block_add_param(caller, rest, call->result_type)))
            goto done;
        vir_replace_all_uses_unchecked(call->result, result);
    }
    if (!vir_edge_create(caller, call->block, clone_of[callee->blocks->index],
                         call->args, call->arg_count) ||
        !vir_effect_unlink(call))
        goto done;
    for (int i = 0; i < count; i++) {
        const vir_block_t *source = callee->block_table[order[i]];
        vir_block_t *block = clone_of[order[i]];
        if (source->terminator == VIR_TERM_RETURN) {
            vir_value_t *value = result ? map[source->return_value->id] : NULL;
            if (!vir_edge_create(caller, block, rest, value ? &value : NULL,
                                 value ? 1 : 0))
                goto done;
            continue;
        }
        const vir_edge_t *taken = source->terminator == VIR_TERM_JUMP
                                      ? source->outgoing
                                      : source->true_edge;
        vir_inline_edge_args(taken, map, args);
        if (source->terminator == VIR_TERM_JUMP) {
            if (!vir_edge_create(caller, block, clone_of[taken->to->index],
                                 args, taken->arg_count))
                goto done;
            continue;
        }
        const vir_edge_t *other_edge = source->false_edge;
        vir_inline_edge_args(other_edge, map, other);
        vir_edge_args_t true_edge = {clone_of[taken->to->index], args,
                                     taken->arg_count};
        vir_edge_args_t false_edge = {clone_of[other_edge->to->index], other,
                                      other_edge->arg_count};
        if (!vir_block_set_branch(caller, block,
                                  map[source->branch_condition->id], &true_edge,
                                  &false_edge))
            goto done;
    }
    result_block = rest;
done:
    free(map);
    return result_block;
}

/* Mark the blocks that lie on a cycle of @func: Tarjan's strongly connected
 * components, iterative so a long CFG cannot exhaust the stack. LICM's natural
 * loops need dominance, which is not built yet here, and would miss the
 * irreducible cycles a goto can make.
 */
static bool vir_inline_cycles(const vir_function_t *func, unsigned char *cyclic)
{
    int count = func->block_count;
    int *index = malloc(sizeof(int) * (size_t) count * 3);
    vir_edge_t **cursor = malloc(sizeof(*cursor) * (size_t) count);
    vir_block_t **stack = malloc(sizeof(*stack) * (size_t) count * 2);
    if (!index || !cursor || !stack) {
        free(index);
        free(cursor);
        free(stack);
        return false;
    }
    int *low = index + count, *on_stack = low + count;
    vir_block_t **walk = stack + count;
    int next = 0, top = 0;
    for (int i = 0; i < count; i++) {
        index[i] = -1;
        on_stack[i] = 0;
        cyclic[i] = 0;
    }
    for (int root = 0; root < count; root++) {
        if (index[root] >= 0)
            continue;
        int depth = 0;
        walk[depth++] = func->block_table[root];
        index[root] = low[root] = next++;
        stack[top++] = func->block_table[root];
        on_stack[root] = 1;
        cursor[root] = func->block_table[root]->outgoing;
        while (depth) {
            vir_block_t *block = walk[depth - 1];
            vir_edge_t *edge = cursor[block->index];
            if (edge) {
                cursor[block->index] = edge->next_outgoing;
                int to = edge->to->index;
                if (to == block->index)
                    cyclic[to] = 1;
                if (index[to] < 0) {
                    index[to] = low[to] = next++;
                    stack[top++] = edge->to;
                    on_stack[to] = 1;
                    cursor[to] = edge->to->outgoing;
                    walk[depth++] = edge->to;
                } else if (on_stack[to] && index[to] < low[block->index]) {
                    low[block->index] = index[to];
                }
                continue;
            }
            depth--;
            if (depth && low[block->index] < low[walk[depth - 1]->index])
                low[walk[depth - 1]->index] = low[block->index];
            if (low[block->index] != index[block->index])
                continue;
            int first = top;
            do
                on_stack[stack[--first]->index] = 0;
            while (stack[first] != block);
            if (top - first > 1)
                for (int k = first; k < top; k++)
                    cyclic[stack[k]->index] = 1;
            top = first;
        }
    }
    free(index);
    free(cursor);
    free(stack);
    return true;
}

int vir_inline_calls(vir_function_t *caller,
                     vir_inline_lookup_t lookup,
                     void *context,
                     vir_opt_level_t opt_level)
{
    if (!caller || !lookup || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level != VIR_OPT_O2)
        return 0;
    int inlined = 0, budget = VIR_INLINE_CALLER_BUDGET;
    int original = caller->block_count;

    /* Loop membership decides only an extern callee too large to copy anywhere,
     * so it is computed for the first such candidate. Inlining only appends
     * blocks: cloned ones hold no call, and the rest of a split block is
     * rescanned in its place.
     */
    unsigned char *cyclic = NULL;
    for (int index = 0; index < original; index++) {
        vir_block_t *block = caller->block_table[index];
        for (vir_effect_t *effect = block->effects; effect;) {
            if (effect->kind != VIR_EFFECT_CALL || !effect->callee) {
                effect = effect->next;
                continue;
            }
            bool is_static = false;
            const vir_function_t *callee =
                lookup(effect->callee, context, &is_static);
            int cap =
                is_static ? VIR_INLINE_STATIC_LIMIT : VIR_INLINE_LOOP_LIMIT;
            if (cap > budget)
                cap = budget;
            int size = callee && callee != caller &&
                               callee->pointer_bits == caller->pointer_bits
                           ? vir_inline_size(callee, effect, cap)
                           : -1;
            if (size > VIR_INLINE_EXTERN_LIMIT && !is_static) {
                if (!cyclic) {
                    /* Sized for the blocks earlier inlines appended too. */
                    cyclic = calloc((size_t) caller->block_count + 1, 1);
                    if (!cyclic || !vir_inline_cycles(caller, cyclic)) {
                        free(cyclic);
                        return -1;
                    }
                }
                if (!cyclic[index])
                    size = -1;
            }
            if (size < 0) {
                effect = effect->next;
                continue;
            }
            vir_block_t *rest = vir_inline_call(caller, effect, callee);
            if (!rest) {
                free(cyclic);
                return -1;
            }
            budget -= size;
            inlined++;
            effect = rest->effects;
        }
    }
    free(cyclic);
    return inlined;
}

/* vir_tail_recursion_type() has already proven every effect is a nonvariadic
 * self call with matching arguments; check only how its result is returned.
 */
static vir_effect_t *vir_tail_call(vir_block_t *block,
                                   vir_type_t type,
                                   vir_value_t **addend)
{
    vir_value_t *returned = block->return_value;
    vir_effect_t *call = block->last_effect;
    *addend = NULL;
    if (block->terminator != VIR_TERM_RETURN || !returned || !call ||
        !call->result || call->result_type != type ||
        (call->signature && call->signature->result.is_bool))
        return NULL;
    vir_value_t *result = call->result;
    if (!result->uses || result->uses->next)
        return NULL;
    if (returned == result)
        return vir_use_return_block(result->uses) == block ? call : NULL;
    if (returned->opcode != VIR_OP_ADD || returned->type != type ||
        vir_use_user(result->uses) != returned || !returned->uses ||
        returned->uses->next || vir_use_return_block(returned->uses) != block)
        return NULL;
    *addend = returned->op0 == result ? returned->op1 : returned->op0;
    return call;
}

static void vir_tail_unlink_value(vir_block_t *block, vir_value_t *value)
{
    for (int operand = 0; operand < value->nr_ops; operand++)
        vir_remove_user_use(operand ? value->op1 : value->op0, value, operand);
    vir_value_t **link = &block->head;
    while (*link != value)
        link = &(*link)->next;
    *link = value->next;
    value->next = NULL;
    value->member = NULL;
}

/* Return the common i32/i64 result type when FUNC may become a loop, or
 * VIR_TYPE_VOID when a parameter, value, effect or return rules it out.
 */
static vir_type_t vir_tail_recursion_type(const vir_function_t *func,
                                          const char *name)
{
    const vir_block_t *header = func->blocks;
    int parameters = header->param_count;
    vir_type_t type = VIR_TYPE_VOID;
    bool self_tail = false;
    /* Most functions end in no self call at all; say so before any scan. */
    for (const vir_block_t *block = header; block && !self_tail;
         block = block->next) {
        const vir_effect_t *call = block->last_effect;
        self_tail = block->terminator == VIR_TERM_RETURN && call &&
                    call->kind == VIR_EFFECT_CALL && call->callee &&
                    !strcmp(call->callee, name);
    }
    if (!self_tail)
        return VIR_TYPE_VOID;
    for (vir_value_t *param = header->params; param; param = param->param_next)
        if (!vir_type_is_i32_or_i64(param->type))
            return VIR_TYPE_VOID;
    for (const vir_block_t *block = header; block; block = block->next) {
        for (vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            if (effect->kind != VIR_EFFECT_CALL || !effect->callee ||
                strcmp(effect->callee, name) ||
                effect->arg_count != parameters ||
                (effect->signature && effect->signature->is_variadic))
                return VIR_TYPE_VOID;
            int index = 0;
            for (vir_value_t *param = header->params; param;
                 param = param->param_next, index++)
                if (effect->args[index]->type != param->type)
                    return VIR_TYPE_VOID;
        }
        for (vir_value_t *value = block->head; value; value = value->next)
            if (value->opcode == VIR_OP_STACK_ADDR ||
                vir_opcode_may_trap(value->opcode))
                return VIR_TYPE_VOID;
        if (block->terminator == VIR_TERM_RETURN) {
            if (!block->return_value ||
                !vir_type_is_i32_or_i64(block->return_value->type) ||
                (type != VIR_TYPE_VOID && type != block->return_value->type))
                return VIR_TYPE_VOID;
            type = block->return_value->type;
        }
    }
    return type;
}

/* Turn direct and additive tail recursion into a loop. Requiring scalar word
 * parameters and no effects other than self calls avoids escaped stack objects,
 * narrowing ABI conversions, memory ordering, and external function behavior.
 */
int vir_eliminate_tail_recursion(vir_function_t *func,
                                 const char *name,
                                 vir_opt_level_t opt_level)
{
    if (!func || !name || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0 || !func->blocks || func->blocks->incoming)
        return 0;
    vir_type_t type = vir_tail_recursion_type(func, name);
    if (type == VIR_TYPE_VOID)
        return 0;
    vir_block_t *header = func->blocks;
    int parameters = header->param_count;
    int candidates = 0;
    for (vir_block_t *block = header; block; block = block->next) {
        vir_value_t *addend;
        if (vir_tail_call(block, type, &addend))
            candidates++;
    }
    if (!candidates || parameters >= INT_MAX / (int) sizeof(vir_value_t *) - 1)
        return 0;
    vir_value_t **args = malloc((size_t) (parameters + 1) * sizeof(*args));
    if (!args)
        return -1;
    int result = -1;
    vir_block_t *last = func->last_block;
    vir_block_t *entry = vir_block_create(func);
    vir_value_t *accumulator = vir_block_add_param(func, header, type);
    if (!entry || !accumulator)
        goto fail;
    int index = 0;
    for (vir_value_t *param = header->params; param != accumulator;
         param = param->param_next, index++) {
        args[index] = vir_block_add_param(func, entry, param->type);
        if (!args[index])
            goto fail;
    }
    args[parameters] = vir_const_int(func, entry, type, 0);
    if (!args[parameters])
        goto fail;

    /* The new ABI entry initializes the accumulator; the original entry is now
     * the recurrence header and receives every recursive argument in SSA.
     */
    last->next = NULL;
    entry->next = header;
    func->blocks = entry;
    func->last_block = last;
    for (int i = func->block_count - 1; i > 0; i--)
        func->block_table[i] = func->block_table[i - 1];
    func->block_table[0] = entry;
    for (int i = 0; i < func->block_count; i++)
        func->block_table[i]->index = i;
    if (!vir_edge_create(func, entry, header, args, parameters + 1))
        goto fail;
    for (vir_block_t *block = header; block; block = block->next) {
        if (block->terminator != VIR_TERM_RETURN)
            continue;
        vir_value_t *addend;
        vir_effect_t *call = vir_tail_call(block, type, &addend);
        vir_value_t *returned = block->return_value;
        vir_value_t *next = call && !addend ? accumulator
                                            : vir_add(func, block, accumulator,
                                                      call ? addend : returned);
        if (!next)
            goto fail;
        vir_remove_return_use(returned, block);
        block->return_value = NULL;
        block->terminator = VIR_TERM_NONE;
        if (!call) {
            if (!vir_block_set_return(func, block, next))
                goto fail;
            continue;
        }
        for (int i = 0; i < parameters; i++)
            args[i] = call->args[i];
        args[parameters] = next;
        if (addend)
            vir_tail_unlink_value(block, returned);
        if (!vir_effect_unlink(call) ||
            !vir_edge_create(func, block, header, args, parameters + 1))
            goto fail;
    }
    result = candidates;
fail:
    free(args);
    return result;
}

/* Coefficients in the result type's modular integer domain. */
typedef struct {
    unsigned long long accumulator, index, constant;
} vir_affine_form_t;

static bool vir_affine_form(const vir_value_t *value,
                            const vir_value_t *iv,
                            const vir_value_t *accumulator,
                            vir_affine_form_t *form,
                            int depth,
                            int *budget)
{
    vir_affine_form_t left = {0}, right = {0};
    if (!value || depth == 32 || --*budget < 0 || value->type != iv->type)
        return false;
    memset(form, 0, sizeof(*form));
    if (value == iv)
        form->index = 1;
    else if (value == accumulator)
        form->accumulator = 1;
    else if (vir_is_constant(value))
        form->constant = value->constant;
    else {
        vir_opcode_t opcode = value->opcode;
        if (value->is_block_param || value->def_effect ||
            (opcode != VIR_OP_ADD && opcode != VIR_OP_SUB &&
             opcode != VIR_OP_MUL && opcode != VIR_OP_SHL &&
             opcode != VIR_OP_NEG) ||
            !vir_affine_form(value->op0, iv, accumulator, &left, depth + 1,
                             budget))
            return false;
        /* NEG is 0 - x and SHL by k is MUL by 2^k in the modular domain. */
        if (opcode == VIR_OP_NEG) {
            right = left;
            memset(&left, 0, sizeof(left));
            opcode = VIR_OP_SUB;
        } else if (opcode == VIR_OP_SHL) {
            if (!vir_is_constant(value->op1) ||
                value->op1->constant >=
                    (unsigned int) vir_integer_type_width(iv->type))
                return false;
            right.constant = 1ULL << value->op1->constant;
            opcode = VIR_OP_MUL;
        } else if (!vir_affine_form(value->op1, iv, accumulator, &right,
                                    depth + 1, budget))
            return false;
        if (opcode == VIR_OP_MUL) {
            if (!left.accumulator && !left.index) {
                vir_affine_form_t swap = left;
                left = right;
                right = swap;
            }
            if (right.accumulator || right.index)
                return false;
            form->accumulator = left.accumulator * right.constant;
            form->index = left.index * right.constant;
            form->constant = left.constant * right.constant;
        } else {
            bool subtract = opcode == VIR_OP_SUB;
            form->accumulator =
                left.accumulator +
                (subtract ? -right.accumulator : right.accumulator);
            form->index = left.index + (subtract ? -right.index : right.index);
            form->constant =
                left.constant + (subtract ? -right.constant : right.constant);
        }
    }
    unsigned long long mask = vir_integer_mask(iv->type);
    form->accumulator &= mask;
    form->index &= mask;
    form->constant &= mask;
    return true;
}

/* Accept a single-entry straight-line loop with no observable body effects.
 * Values produced in its body must not escape the loop: only header parameters
 * have a well-defined value when the body is replaced by its summary.
 */
static vir_edge_t *vir_affine_loop_body(vir_block_t *header,
                                        unsigned char *members,
                                        int block_count)
{
    vir_block_t *block = header->true_edge->to;
    if (block == header)
        return NULL;
    members[header->index] = 1;
    for (;;) {
        if (members[block->index] || block->effects || block->params ||
            block->terminator != VIR_TERM_JUMP || !block->outgoing ||
            block->outgoing->next_outgoing || !block->incoming ||
            block->incoming->next_incoming)
            return NULL;
        members[block->index] = 1;
        if (block->outgoing->to == header)
            break;
        if (block->outgoing->arg_count)
            return NULL;
        block = block->outgoing->to;
    }
    for (vir_block_t *body = header->true_edge->to; body != header;
         body = body->outgoing->to) {
        for (vir_value_t *value = body->head; value; value = value->next) {
            if (vir_opcode_is_division(value->opcode))
                return NULL;
            for (vir_use_t *use = value->uses; use; use = use->next) {
                vir_block_t *user = vir_use_block(use);
                if (!user || user->index < 0 || user->index >= block_count ||
                    !members[user->index])
                    return NULL;
            }
        }
    }
    return block->outgoing;
}

/* A loop header carrying two parameters, entered once and from one back edge,
 * whose only control is its branch.
 */
static bool vir_two_param_loop_header(const vir_function_t *func,
                                      const vir_block_t *header)
{
    return header != func->blocks && header->param_count == 2 &&
           !header->effects && header->terminator == VIR_TERM_BRANCH &&
           header->incoming && header->incoming->next_incoming &&
           !header->incoming->next_incoming->next_incoming;
}

static int vir_summarize_affine_loop(vir_function_t *func,
                                     vir_block_t *header,
                                     unsigned char *members)
{
    if (!vir_two_param_loop_header(func, header))
        return 0;
    for (vir_value_t *value = header->head; value; value = value->next)
        if (vir_opcode_may_trap(value->opcode))
            return 0;
    vir_value_t *condition = header->branch_condition;
    if ((condition->opcode != VIR_OP_ULT && condition->opcode != VIR_OP_SLT) ||
        !vir_is_constant(condition->op1))
        return 0;
    vir_value_t *iv = condition->op0;
    if (!iv->is_block_param || iv->block != header ||
        !vir_type_is_i32_or_i64(iv->type))
        return 0;
    int index = header->params == iv ? 0 : 1;
    vir_value_t *accumulator = index ? header->params : iv->param_next;
    if (accumulator->type != iv->type)
        return 0;
    vir_edge_t *back = vir_affine_loop_body(header, members, func->block_count);
    bool leaves = back && !members[header->false_edge->to->index];
    /* Clear only the marked chain, so each candidate costs its body size. */
    members[header->index] = 0;
    for (vir_block_t *block = header->true_edge->to; members[block->index];
         block = block->outgoing->to)
        members[block->index] = 0;
    if (!leaves)
        return 0;

    /* The header has exactly two incoming edges and only the body's last block
     * reaches it, so the other edge enters from outside the loop.
     */
    vir_edge_t *entry =
        header->incoming == back ? back->next_incoming : header->incoming;
    if (!vir_is_constant(entry->args[index]))
        return 0;
    vir_affine_form_t increment, recurrence;
    int budget = 256;
    if (!vir_affine_form(back->args[index], iv, accumulator, &increment, 0,
                         &budget) ||
        increment.accumulator || increment.index != 1 || !increment.constant ||
        !vir_affine_form(back->args[1 - index], iv, accumulator, &recurrence, 0,
                         &budget))
        return 0;
    unsigned long long first = entry->args[index]->constant;
    unsigned long long limit = condition->op1->constant;
    unsigned long long step = increment.constant;
    unsigned long long mask = vir_integer_mask(iv->type);
    unsigned long long maximum =
        condition->opcode == VIR_OP_SLT ? mask >> 1 : mask;
    if (first > maximum || limit > maximum || first >= limit)
        return 0;
    unsigned long long trips = (limit - first - 1) / step + 1;
    if (trips > (maximum - first) / step)
        return 0;

    /* Exponentiate (a,i) -> (A*a+B*i+C,i+D). Composition is exact modulo the
     * integer width, including overflow in the accumulator. The separate
     * trip-count proof above rules out wrapping the controlling induction.
     */
    unsigned long long a = 1, b = 0, c = 0, d = 0;
    unsigned long long aa = recurrence.accumulator, bb = recurrence.index;
    unsigned long long cc = recurrence.constant, dd = step;
    for (unsigned long long remaining = trips; remaining; remaining >>= 1) {
        if (remaining & 1) {
            c = (aa * c + bb * d + cc) & mask;
            b = (aa * b + bb) & mask;
            a = (aa * a) & mask;
            d = (d + dd) & mask;
        }
        cc = (aa * cc + bb * dd + cc) & mask;
        bb = (aa * bb + bb) & mask;
        aa = (aa * aa) & mask;
        dd = (dd + dd) & mask;
    }
    vir_block_t *pre = entry->from;
    vir_value_t *scale = vir_const_int(func, pre, iv->type, a);
    vir_value_t *bias = vir_const_int(func, pre, iv->type, b * first + c);
    vir_value_t *final_iv =
        vir_const_int(func, pre, iv->type, first + trips * step);
    vir_value_t *product =
        scale ? vir_mul(func, pre, entry->args[1 - index], scale) : NULL;
    vir_value_t *final_acc =
        product && bias ? vir_add(func, pre, product, bias) : NULL;
    if (!final_acc || !final_iv)
        return -1;
    vir_set_use_slot(&entry->args[index], final_iv, &entry->arg_uses[index]);
    vir_set_use_slot(&entry->args[1 - index], final_acc,
                     &entry->arg_uses[1 - index]);
    return vir_edge_delete(func, header->true_edge) ? 1 : -1;
}

int vir_summarize_affine_loops(vir_function_t *func, vir_opt_level_t opt_level)
{
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0 || !func->blocks)
        return 0;
    unsigned char *members =
        calloc((size_t) func->block_count, sizeof(*members));
    if (!members)
        return -1;
    int changed = 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        int result = vir_summarize_affine_loop(func, block, members);
        if (result < 0) {
            changed = -1;
            break;
        }
        changed += result;
    }
    free(members);
    if (changed > 0 && !vir_remove_unreachable(func))
        return -1;
    return changed;
}

/* Split a pointer address into a base and a constant byte offset. */
static const vir_value_t *vir_unroll_base(const vir_value_t *pointer,
                                          long long *offset,
                                          int pointer_bits)
{
    *offset = 0;
    if (pointer->opcode == VIR_OP_PTRADD && vir_is_constant(pointer->op1)) {
        unsigned long long bits = pointer->op1->constant;
        if (pointer_bits < 64 && (bits >> (pointer_bits - 1) & 1))
            bits |= ~0ULL << pointer_bits;
        *offset = (long long) bits;
        return pointer->op0;
    }
    return pointer;
}

static vir_value_t *vir_sr_condition(vir_value_t *value, bool *positive);

/* Unroll one counted pointer reduction by two into a second accumulator. The
 * header carries an accumulator and a pointer and exits when the pointer equals
 * a loop-invariant end; the body's only effect is one ordinary load through the
 * pointer, which it adds to the accumulator before stepping the pointer by a
 * constant. Start and end must share a base and lie an even number of steps
 * apart, so no remainder iteration is needed, and modular addition makes the
 * regrouping exact. The accumulator may only be read after the loop, where the
 * two partial sums are added.
 */
static int vir_unroll_reduction(vir_function_t *func,
                                vir_block_t *header,
                                unsigned char *members)
{
    if (!vir_two_param_loop_header(func, header) ||
        header->true_edge->arg_count || header->false_edge->arg_count)
        return 0;
    bool positive = true;
    vir_value_t *condition =
        vir_sr_condition(header->branch_condition, &positive);
    if (!condition || condition->opcode != VIR_OP_EQ)
        return 0;
    int index =
        condition->op0->is_block_param && condition->op0->block == header ? 0
                                                                          : 1;
    vir_value_t *pointer = index ? condition->op1 : condition->op0;
    vir_value_t *end = index ? condition->op0 : condition->op1;
    if (!pointer->is_block_param || pointer->block != header ||
        pointer->type != VIR_TYPE_PTR || end->type != VIR_TYPE_PTR)
        return 0;
    int pointer_index = header->params == pointer ? 0 : 1;
    vir_value_t *accumulator =
        pointer_index ? header->params : pointer->param_next;
    if (!vir_type_is_i32_or_i64(accumulator->type))
        return 0;

    /* The body: a straight chain from the edge taken while the pointer has not
     * reached the end back to the header.
     */
    vir_block_t *after =
        positive ? header->true_edge->to : header->false_edge->to;
    vir_block_t *start =
        positive ? header->false_edge->to : header->true_edge->to;
    vir_block_t *block = start;
    vir_effect_t *load = NULL;
    if (start == header)
        return 0;
    members[header->index] = 1;
    int length = 0;
    for (;;) {
        if (members[block->index] || block->params || ++length > 8 ||
            block->terminator != VIR_TERM_JUMP || !block->outgoing ||
            block->outgoing->next_outgoing || !block->incoming ||
            block->incoming->next_incoming)
            return 0;
        members[block->index] = 1;
        for (vir_effect_t *effect = block->effects; effect;
             effect = effect->next) {
            if (load || effect->kind != VIR_EFFECT_LOAD ||
                effect->address != pointer || !effect->result ||
                effect->result->type != accumulator->type)
                return 0;
            load = effect;
        }
        if (block->outgoing->to != header && block->outgoing->arg_count)
            return 0;
        if (block->outgoing->to == header)
            break;
        block = block->outgoing->to;
    }
    if (!load || members[after->index] || after->incoming->next_incoming ||
        members[end->block->index])
        return 0;
    vir_edge_t *back = block->outgoing;
    vir_edge_t *entry =
        header->incoming == back ? back->next_incoming : header->incoming;
    vir_value_t *sum = back->args[1 - pointer_index];
    vir_value_t *next = back->args[pointer_index];
    long long step;
    if (sum->opcode != VIR_OP_ADD || sum->type != accumulator->type ||
        !((sum->op0 == accumulator && sum->op1 == load->result) ||
          (sum->op1 == accumulator && sum->op0 == load->result)) ||
        vir_unroll_base(next, &step, func->pointer_bits) != pointer || !step)
        return 0;

    /* Body values may leave the loop only through the back edge, and the
     * accumulator only after it, through the exit.
     */
    for (vir_block_t *body = start; body != header; body = body->outgoing->to)
        for (vir_value_t *value = body->head; value; value = value->next)
            for (vir_use_t *use = value->uses; use; use = use->next)
                if (!members[vir_use_block(use)->index])
                    return 0;
    for (vir_use_t *use = accumulator->uses; use; use = use->next)
        if (vir_use_user(use) != sum && vir_use_block(use) != after)
            return 0;

    /* The unrolled loop skips every other pointer value; nothing else in the
     * loop may observe one. After the loop it is the end either way.
     */
    for (vir_use_t *use = pointer->uses; use; use = use->next)
        if (vir_use_user(use) != condition && vir_use_user(use) != next &&
            vir_use_effect(use) != load && vir_use_block(use) != after)
            return 0;

    /* An even trip count between constant offsets from one base. */
    long long first, last;
    const vir_value_t *base =
        vir_unroll_base(entry->args[pointer_index], &first, func->pointer_bits);
    if (vir_unroll_base(end, &last, func->pointer_bits) != base)
        return 0;
    unsigned long long step_bits = (unsigned long long) step;
    long long distance = last - first;
    if (step == LLONG_MIN || distance % step || distance / step < 0 ||
        (distance / step) % 2)
        return 0;

    vir_param_edit_t edit;
    if (!vir_param_edit_prepare(func, header, NULL, header->param_count,
                                header->param_count, true, &edit))
        return -1;
    vir_type_t width = func->pointer_bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    vir_block_t *at = load->block;
    vir_value_t *second = vir_value_alloc(func, header, accumulator->type);
    vir_value_t *zero = vir_const_int(func, entry->from, accumulator->type, 0);
    vir_value_t *offset = vir_const_int(func, at, width, step_bits);
    vir_value_t *address =
        offset ? vir_ptradd(func, at, pointer, offset) : NULL;

    /* The public builders take effects only while a block is open; the second
     * load joins the first in a body block that already ends in its jump.
     */
    vir_value_t *value =
        address ? vir_load_impl(func, at, address, accumulator->type, false)
                : NULL;
    /* The new parameter joins the header only when the edit applies. */
    vir_value_t *second_sum =
        value && second ? vir_user_operation(func, at, VIR_OP_ADD,
                                             accumulator->type, second, value)
                        : NULL;
    vir_value_t *double_step =
        vir_const_int(func, back->from, width, step_bits * 2);
    vir_value_t *advanced =
        double_step ? vir_ptradd(func, back->from, pointer, double_step) : NULL;
    if (!zero || !second_sum || !advanced) {
        vir_param_edit_release(&edit);
        return -1;
    }
    for (int i = 0; i < edit.count; i++)
        edit.edges[i].inserted =
            edit.edges[i].edge == entry ? zero : second_sum;
    vir_param_edit_apply(header, second, &edit);
    vir_param_edit_release(&edit);
    vir_set_use_slot(&back->args[pointer_index], advanced,
                     &back->arg_uses[pointer_index]);

    /* Combine the partial sums at the top of the block after the loop, ahead of
     * its effects too: a store or call there may read the result before any
     * value of the block is defined.
     */
    vir_value_t *old_tail = after->tail;
    vir_value_t *total = vir_add(func, after, accumulator, second);
    if (!total)
        return -1;
    if (old_tail) {
        old_tail->next = NULL;
        total->next = after->head;
        after->head = total;
    }
    total->order = -1;
    vir_repair_value_positions(after);
    for (vir_use_t *use = accumulator->uses, *following; use; use = following) {
        following = use->next;
        if (vir_use_user(use) != sum && vir_use_user(use) != total)
            vir_set_use_slot(vir_use_value_slot(use), total, use);
    }
    return 1;
}

int vir_unroll_reductions(vir_function_t *func, vir_opt_level_t opt_level)
{
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level != VIR_OPT_O2 || !func->blocks)
        return 0;
    unsigned char *members =
        calloc((size_t) func->block_count, sizeof(*members));
    if (!members)
        return -1;
    int changed = 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        int result = vir_unroll_reduction(func, block, members);

        /* A candidate marks its header before any body block, so most blocks
         * leave nothing to clear.
         */
        if (members[block->index])
            memset(members, 0, (size_t) func->block_count);
        if (result < 0) {
            changed = -1;
            break;
        }
        changed += result;
    }
    free(members);
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
                     ? root->addr->slot == value->addr->slot
                     : !strcmp(root->addr->name, value->addr->name)))
                return root;
    if (value->opcode == VIR_OP_GLOBAL_ADDR)
        return vir_global_addr(func, pre, value->addr->name, value->addr->size,
                               value->addr->alignment);
    if (value->opcode == VIR_OP_STACK_ADDR)
        return vir_stack_addr(func, pre, value->addr->slot, value->addr->size,
                              value->addr->alignment);
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

    /* A runtime first offset, and a constant byte bias applied to the pointer
     * it gives: the subscript's own address is then shared, and the bias folds
     * into an addressing displacement.
     */
    vir_value_t *dynamic_first;
    unsigned long long dynamic_bias;
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
        if (offsets->dynamic_first) {
            vir_value_t *scaled = initial;
            if (offsets->dynamic_bias)
                scaled = initial->opcode == VIR_OP_PTRADD &&
                                 vir_is_const_int(initial->op1,
                                                  offsets->dynamic_bias)
                             ? initial->op0
                             : NULL;
            same_initial = scaled && scaled->opcode == VIR_OP_PTRADD &&
                           scaled->op0 == base &&
                           scaled->op1 == offsets->dynamic_first;
        }
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
    vir_value_t *offset = offsets->dynamic_first
                              ? offsets->dynamic_first
                              : vir_const_int(func, entry->from, width, first);
    vir_value_t *initial = NULL;
    /* The subscript usually computed this address already, before the loop. */
    if (offsets->dynamic_first)
        for (vir_value_t *value = entry->from->head; value && !initial;
             value = value->next)
            if (value->opcode == VIR_OP_PTRADD && value->op0 == base &&
                vir_same_address(value->op1, offsets->dynamic_first, 4))
                initial = value;
    if (!initial && offset)
        initial = vir_ptradd(func, entry->from, base, offset);
    if (initial && offsets->dynamic_first && offsets->dynamic_bias) {
        vir_value_t *bias =
            vir_const_int(func, entry->from, width, offsets->dynamic_bias);
        initial = bias ? vir_ptradd(func, entry->from, initial, bias) : NULL;
    }
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

/* Accept only widened unsigned indices with a common constant stride. */
static bool vir_sr_descending_offset(vir_value_t *offset,
                                     vir_value_t *iv,
                                     unsigned long long *stride,
                                     bool *previous)
{
    *stride = 1;
    if (offset->opcode == VIR_OP_MUL || offset->opcode == VIR_OP_SHL) {
        vir_value_t *constant = offset->op1;
        vir_value_t *index = offset->op0;
        if (offset->opcode == VIR_OP_MUL && vir_is_constant(index)) {
            constant = index;
            index = offset->op1;
        }
        if (!vir_is_constant(constant))
            return false;
        *stride =
            offset->opcode == VIR_OP_SHL
                ? (constant->constant < 31 ? 1ULL << constant->constant : 0)
                : constant->constant;
        offset = index;
    }
    if (!*stride || *stride > INT_MAX || offset->opcode != VIR_OP_ZEXT ||
        offset->type != VIR_TYPE_I64 || offset->op0->type != VIR_TYPE_I32)
        return false;
    offset = offset->op0;
    *previous = offset != iv;
    return offset == iv || (offset->opcode == VIR_OP_SUB && offset->op0 == iv &&
                            vir_is_const_int(offset->op1, 1));
}

typedef struct {
    const vir_dominance_t *dominance;
    vir_block_t *body;
    vir_value_t *iv, *condition, *increment, *base;
    vir_edge_t *back;
    int index;
    unsigned long long stride;
} vir_sr_descending_t;

static bool vir_sr_descending_uses(vir_sr_descending_t *loop,
                                   vir_value_t *value,
                                   int depth)
{
    if (depth == 8)
        return false;
    for (vir_use_t *use = value->uses; use; use = use->next) {
        if (vir_use_user(use) == loop->condition)
            continue;
        if (value == loop->increment && vir_use_edge(use) == loop->back &&
            use->operand == loop->index)
            continue;
        vir_value_t *user = vir_use_user(use);
        if (!user)
            return false;
        if (user->opcode == VIR_OP_PTRADD) {
            unsigned long long stride;
            bool previous;
            if (!vir_sr_descending_offset(user->op1, loop->iv, &stride,
                                          &previous) ||
                (previous && !vir_dominance_block_dominates(
                                 loop->dominance, loop->body, user->block)) ||
                (loop->base &&
                 (loop->base != user->op0 || loop->stride != stride)))
                return false;
            loop->base = user->op0;
            loop->stride = stride;
        } else if ((user->opcode != VIR_OP_SUB && user->opcode != VIR_OP_ZEXT &&
                    user->opcode != VIR_OP_MUL && user->opcode != VIR_OP_SHL) ||
                   !vir_sr_descending_uses(loop, user, depth + 1))
            return false;
    }
    return true;
}

/* Rebind operand @index of @user to @value. Fails only when the operand's use
 * list has lost the use, which the verifier would report.
 */
static bool vir_rebind_operand(vir_value_t *user, int index, vir_value_t *value)
{
    vir_value_t **slot = index ? &user->op1 : &user->op0;
    vir_use_t *use = (*slot)->uses;
    while (use && (vir_use_user(use) != user || use->operand != index))
        use = use->next;
    if (!use)
        return false;
    vir_set_use_slot(slot, value, use);
    return true;
}

/* Retarget the existing address without changing its effect ordering. */
static vir_value_t *vir_sr_derived_pointer(vir_function_t *func,
                                           vir_block_t *pre,
                                           vir_value_t *address,
                                           vir_value_t *pointer,
                                           unsigned long long bias)
{
    if (!(bias & vir_width_mask(func->pointer_bits)))
        return pointer;
    vir_type_t width = func->pointer_bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
    vir_value_t *delta = vir_const_int(func, pre, width, bias);
    if (!delta)
        return NULL;
    if (!vir_rebind_operand(address, 0, pointer) ||
        !vir_rebind_operand(address, 1, delta))
        return NULL;
    return address;
}

/* A value computed in @block itself, as opposed to one that is ready on entry
 * (a parameter, or a definition in a dominating block).
 */
static bool vir_reassociate_local(const vir_value_t *value,
                                  const vir_block_t *block)
{
    return !value->is_block_param && value->block == block &&
           !vir_is_constant(value);
}

/* Rewrite (local + ready) + C as local + (ready + C) when the inner sum has no
 * other use. Integer addition wraps, so any grouping yields the same value; the
 * constant then joins the operand that is ready on block entry, shortening a
 * chain that runs through the local operand (a loop-carried recurrence such as
 * a = a * k + i + C) by one addition.
 */
int vir_reassociate(vir_function_t *func, vir_opt_level_t opt_level)
{
    int changed = 0;
    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        for (vir_value_t *outer = block->head; outer; outer = outer->next) {
            if (outer->opcode != VIR_OP_ADD ||
                (outer->type != VIR_TYPE_I32 && outer->type != VIR_TYPE_I64))
                continue;
            int constant_index = vir_is_constant(outer->op1)   ? 1
                                 : vir_is_constant(outer->op0) ? 0
                                                               : -1;
            if (constant_index < 0)
                continue;
            vir_value_t *constant = constant_index ? outer->op1 : outer->op0;
            vir_value_t *inner = constant_index ? outer->op0 : outer->op1;
            if (inner->opcode != VIR_OP_ADD || inner->type != outer->type ||
                inner->block != block || !inner->uses || inner->uses->next ||
                vir_use_user(inner->uses) != outer)
                continue;
            vir_value_t *local = inner->op0, *ready = inner->op1;
            if (!vir_reassociate_local(local, block)) {
                local = inner->op1;
                ready = inner->op0;
            }
            if (!vir_reassociate_local(local, block) ||
                vir_reassociate_local(ready, block) || vir_is_constant(ready))
                continue;

            /* inner becomes ready + C, so it must follow C: move it to just
             * before outer. It is pure and its operands precede it already, and
             * it precedes its user outer, so one walk finds both.
             */
            vir_value_t **link = &block->head;
            while (*link != inner)
                link = &(*link)->next;
            *link = inner->next;
            while (*link != outer)
                link = &(*link)->next;
            inner->next = outer;
            *link = inner;
            inner->order = outer->order;
            vir_repair_value_positions(block);
            vir_value_t *first = ready, *second = constant;
            vir_order_commutative(&first, &second);
            vir_rebind_operand(inner, 0, first);
            vir_rebind_operand(inner, 1, second);
            first = local;
            second = inner;
            vir_order_commutative(&first, &second);
            vir_rebind_operand(outer, 0, first);
            vir_rebind_operand(outer, 1, second);
            changed++;
        }
    }
    return changed;
}

/* Reduce proven bounded ascending or widened unsigned descending counters to
 * pointer recurrences; retire counters when a pointer endpoint suffices.
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
    int cfg = vir_simplify_cfg(func, opt_level);
    if (cfg < 0) {
        vir_dominance_release(&dominance);
        return -1;
    }
    if (cfg) {
        vir_dominance_release(&dominance);
        if (!vir_dominance_init(func, &dominance))
            return -1;
    }
    for (vir_block_t *header = func->blocks; header; header = header->next) {
        vir_edge_t *entry = NULL, *back = NULL;
        bool positive = true;
        vir_value_t *branch = header->branch_condition;
        vir_value_t *condition = vir_sr_condition(branch, &positive);
        if (header->terminator != VIR_TERM_BRANCH || !condition ||
            (condition->opcode != VIR_OP_SLT &&
             condition->opcode != VIR_OP_ULT) ||
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
        bool descending = func->pointer_bits == 64 &&
                          condition->opcode == VIR_OP_ULT &&
                          vir_is_const_int(iv, 0) && limit->is_block_param;
        bool inclusive = vir_is_constant(iv) && limit->is_block_param;
        if (inclusive) {
            iv = condition->op1;
            limit = condition->op0;
            if (!descending)
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
        if (descending) {
            if (body_edge->to->incoming != body_edge ||
                body_edge->next_incoming || increment->opcode != VIR_OP_SUB ||
                increment->op0 != iv || !vir_is_const_int(increment->op1, 1) ||
                condition != branch)
                continue;
        } else if (!vir_is_constant(initial) || initial->constant > INT_MAX ||
                   increment->opcode != VIR_OP_ADD ||
                   !((increment->op0 == iv &&
                      vir_is_const_int(increment->op1, 1)) ||
                     (increment->op1 == iv &&
                      vir_is_const_int(increment->op0, 1))))
            continue;
        vir_sr_descending_t down = {&dominance, body_edge->to, iv,
                                    condition,  increment,     NULL,
                                    back,       index,         0};
        if (descending && (!vir_sr_descending_uses(&down, iv, 0) || !down.base))
            continue;
        bool unsigned_counter = condition->opcode == VIR_OP_ULT;
        if (unsigned_counter && !descending) {
            bool counter_only = vir_is_constant(limit) && increment->uses &&
                                !increment->uses->next &&
                                vir_use_edge(increment->uses) == back &&
                                increment->uses->operand == index;
            for (vir_use_t *use = iv->uses; use; use = use->next)
                counter_only &= vir_use_user(use) == condition ||
                                vir_use_user(use) == increment;
            if (!counter_only)
                continue;
        }
        unsigned long long bound = vir_is_constant(limit)
                                       ? limit->constant + (inclusive ? 1 : 0)
                                       : INT_MAX;
        if (!descending &&
            (!bound || bound > INT_MAX || initial->constant >= bound))
            continue;
        vir_value_t *walk = NULL, *walk_base = NULL;
        unsigned long long walk_scale = 0, walk_bias = 0;
        for (vir_block_t *block = func->blocks; block; block = block->next) {
            if (!vir_dominance_block_dominates(&dominance, header, block) ||
                (!descending &&
                 !vir_dominance_block_dominates(&dominance, block, back->from)))
                continue;
            for (vir_value_t *address = block->head, *next; address;
                 address = next) {
                next = address->next;
                unsigned long long scale, bias;
                if (address->opcode != VIR_OP_PTRADD || !address->uses)
                    continue;
                if (descending) {
                    bool previous;
                    if (!vir_sr_descending_offset(address->op1, iv, &scale,
                                                  &previous))
                        continue;
                    bias = previous ? 0ULL - scale : 0;
                } else if (!vir_sr_affine(func, address->op1, iv, bound, &scale,
                                          &bias, 0))
                    continue;
                vir_value_t *pointer;
                vir_value_t *base =
                    vir_sr_base(func, &dominance, entry->from, address->op0, 0);
                if (!base)
                    continue;
                if (walk && base == walk_base && scale == walk_scale) {
                    vir_value_t *derived =
                        bias == walk_bias
                            ? walk
                            : vir_sr_derived_pointer(func, entry->from, address,
                                                     walk, bias - walk_bias);
                    if (!derived ||
                        !vir_replace_all_uses(func, address, derived)) {
                        changed = -1;
                        goto done;
                    }
                    changed++;
                    continue;
                }
                vir_value_t *dynamic_first = NULL;
                if (descending) {
                    /* Scale the index exactly as normalization rewrote the
                     * subscript, so vir_sr_pointer() finds the subscript's own
                     * address in the entry block; the bias is applied to it.
                     */
                    vir_value_t *widened =
                        vir_zext(func, entry->from, initial, VIR_TYPE_I64);
                    vir_value_t *factor =
                        vir_const_int(func, entry->from, VIR_TYPE_I64, scale);
                    dynamic_first = widened && factor
                                        ? vir_strength_reduce_integer(
                                              func, entry->from, widened,
                                              factor, VIR_OP_MUL)
                                        : NULL;
                    if (!dynamic_first && widened && factor)
                        dynamic_first =
                            vir_mul(func, entry->from, widened, factor);
                    if (!dynamic_first) {
                        changed = -1;
                        goto done;
                    }
                }
                vir_sr_offset_t offsets = {
                    descending ? 0
                               : (initial->constant * scale + bias) &
                                     vir_width_mask(func->pointer_bits),
                    descending ? 0ULL - scale : scale, dynamic_first,
                    descending ? bias : 0};
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
        if (!walk && unsigned_counter && !descending) {
            int slot = 0;
            for (vir_value_t *param = header->params; param;
                 param = param->param_next, slot++) {
                vir_value_t *next = back->args[slot];
                if (param->type != VIR_TYPE_PTR ||
                    next->opcode != VIR_OP_PTRADD || next->op0 != param ||
                    !vir_is_constant(next->op1) || !next->op1->constant ||
                    next->op1->constant > INT_MAX)
                    continue;
                walk = param;
                walk_base = entry->args[slot];
                walk_scale = next->op1->constant;
                walk_bias = 0ULL - initial->constant * walk_scale;
                break;
            }
        }
        if (!walk || !vir_is_constant(limit) ||
            (!descending &&
             (bound - initial->constant) >
                 vir_width_mask(func->pointer_bits) / walk_scale) ||
            !branch->uses || branch->uses->next ||
            vir_use_branch_block(branch->uses) != header)
            continue;
        if (unsigned_counter && !descending)
            changed++;

        /* The equality endpoint works even when the concrete pointer wraps: the
         * bounded span cannot complete a full address-space revolution.
         */
        vir_type_t width =
            func->pointer_bits == 64 ? VIR_TYPE_I64 : VIR_TYPE_I32;
        vir_value_t *offset =
            vir_const_int(func, entry->from, width,
                          (descending ? 0 : bound * walk_scale) + walk_bias);
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
        if (iv->uses && !iv->uses->next &&
            vir_use_user(iv->uses) == increment && increment->uses &&
            !increment->uses->next && vir_use_edge(increment->uses) == back &&
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
    if (!reachable || !work ||
        !vir_mark_reachable_blocks(func, NULL, reachable, work))
        goto fail;
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
    func->block_count = 0;
    for (i = 0; *link;) {
        block = *link;
        if (!reachable[i])
            *link = block->next;
        else {
            func->last_block = block;
            block->index = func->block_count;
            func->block_table[func->block_count++] = block;
            link = &block->next;
        }
        i++;
    }
    i = 1;
    goto cleanup;
fail:
    i = 0;
cleanup:
    free(reachable);
    free(work);
    return i;
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
        vir_use_set_owner(use, VIR_USE_RETURN, from);
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
    for (use = condition->uses; use && vir_use_branch_block(use) != block;
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

/* A branch whose two edges reach the same block with the same arguments goes
 * there either way, so it is a jump, and DCE can drop its condition.
 */
static int vir_fold_same_target_branch(vir_function_t *func, vir_block_t *block)
{
    vir_edge_t *taken = block->true_edge, *other = block->false_edge;

    if (block->terminator != VIR_TERM_BRANCH || taken->to != other->to ||
        taken->arg_count != other->arg_count)
        return 0;
    for (int i = 0; i < taken->arg_count; i++)
        if (taken->args[i] != other->args[i])
            return 0;
    return vir_edge_delete(func, other) ? 1 : -1;
}

/* An incoming constant can select a branch without materializing its boolean
 * parameter. Keep the bypassed computations local and successors argument-free.
 */
static int vir_thread_constant_edge(vir_function_t *func, vir_block_t *block)
{
    vir_value_t *param = block->branch_condition;
    vir_value_t *constant = NULL;
    int index = 0;

    if (block == func->blocks || block->effects ||
        block->terminator != VIR_TERM_BRANCH || !param ||
        block->true_edge->arg_count || block->false_edge->arg_count)
        return 0;
    if (param->opcode == VIR_OP_EQ) {
        constant = param->op1;
        param = param->op0;
        if (param->opcode == VIR_OP_CONST) {
            vir_value_t *swap = param;
            param = constant;
            constant = swap;
        }
        if (constant->opcode != VIR_OP_CONST)
            return 0;
    } else if (param->type != VIR_TYPE_I1) {
        return 0;
    }
    if (!param->is_block_param || param->block != block)
        return 0;
    for (vir_value_t *value = block->params; value != param;
         value = value->param_next)
        index++;
    for (int list = 0; list < 2; list++)
        for (vir_value_t *value = list ? block->head : block->params; value;
             value = list ? value->next : value->param_next) {
            if (list && value->opcode != VIR_OP_CONST &&
                value->opcode != VIR_OP_EQ)
                return 0;
            for (vir_use_t *use = value->uses; use; use = use->next)
                if (vir_use_block(use) != block)
                    return 0;
        }
    for (vir_edge_t *edge = block->incoming; edge; edge = edge->next_incoming) {
        vir_value_t *actual = edge->args[index];
        if (actual->opcode != VIR_OP_CONST)
            continue;
        bool truth = constant ? actual->constant == constant->constant
                              : actual->constant != 0;
        vir_block_t *target =
            (truth ? block->true_edge : block->false_edge)->to;
        if (target == block || target == edge->from || target == func->blocks)
            continue;
        vir_edge_replace_args(edge, NULL, NULL, 0);
        return vir_edge_redirect(func, edge, target) ? 1 : -1;
    }
    return 0;
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

            /* Folding only drops an edge, so the scan can go on in place. */
            inlined = vir_fold_same_target_branch(func, block);
            if (inlined < 0)
                return -1;
            if (inlined) {
                changes++;
                changed = true;
            }
            inlined = vir_thread_constant_edge(func, block);
            if (!inlined)
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

/* Return duplication.
 *
 * A short block that only computes a result and returns it, entered by a jump
 * that passes a constant, is copied into that jump's block: the copy returns
 * straight away and, with the constant in place of the parameter, usually
 * folds to a constant. This is the shape of `return a && b` and `return x !=
 * y`, where each arm used to set a flag and join just to test it.
 */
#define VIR_RETURN_DUP_VALUES 6
#define VIR_RETURN_DUP_PARAMS 4

static bool vir_return_dup_candidate(const vir_function_t *func,
                                     const vir_block_t *block)
{
    int count = 0;

    if (block == func->blocks || block->terminator != VIR_TERM_RETURN ||
        block->effects || !block->params ||
        block->param_count > VIR_RETURN_DUP_PARAMS)
        return false;
    for (const vir_value_t *value = block->head; value; value = value->next) {
        if (++count > VIR_RETURN_DUP_VALUES || value->def_effect)
            return false;
        if (value->opcode == VIR_OP_STACK_ADDR ||
            (!value->nr_ops && value->opcode != VIR_OP_CONST &&
             value->opcode != VIR_OP_GLOBAL_ADDR &&
             value->opcode != VIR_OP_RODATA_ADDR &&
             value->opcode != VIR_OP_FUNC_ADDR))
            return false;
    }
    return true;
}

/* Position of @param among the parameters of @block, or -1. */
static int vir_param_index(const vir_block_t *block, const vir_value_t *param)
{
    int index = 0;

    for (const vir_value_t *p = block->params; p; p = p->param_next, index++)
        if (p == param)
            return index;
    return -1;
}

/* What @operand, a value of @block, stands for in the copy. Values from
 * outside the block already dominate every block that jumps into it.
 */
static vir_value_t *vir_return_dup_map(const vir_block_t *block,
                                       vir_value_t *operand,
                                       vir_value_t **args,
                                       vir_value_t **source,
                                       vir_value_t **copy,
                                       int count)
{
    if (operand->block != block)
        return operand;
    if (operand->is_block_param) {
        int index = vir_param_index(block, operand);
        return index < 0 ? NULL : args[index];
    }
    for (int index = 0; index < count; index++)
        if (source[index] == operand)
            return copy[index];
    return NULL;
}

/* Whether @operand is a constant once the block's parameters take @args:
 * constants themselves, parameters given one, and values computed from such.
 */
static bool vir_return_dup_known(const vir_block_t *block,
                                 const vir_value_t *operand,
                                 vir_value_t **args,
                                 bool *known,
                                 vir_value_t **seen,
                                 int count)
{
    if (operand->block != block)
        return operand->opcode == VIR_OP_CONST;
    if (operand->is_block_param) {
        int index = vir_param_index(block, operand);
        return index >= 0 && args[index]->opcode == VIR_OP_CONST;
    }
    for (int index = 0; index < count; index++)
        if (seen[index] == operand)
            return known[index];
    return false;
}

/* Whether the copy returns a constant, so SCCP will leave only the return. */
static bool vir_return_dup_folds(const vir_block_t *block, vir_value_t **args)
{
    bool known[VIR_RETURN_DUP_VALUES];
    vir_value_t *seen[VIR_RETURN_DUP_VALUES];
    int count = 0;

    for (const vir_value_t *value = block->head; value; value = value->next) {
        bool folds = value->opcode == VIR_OP_CONST || value->nr_ops > 0;
        for (int i = 0; folds && i < value->nr_ops; i++)
            folds = vir_return_dup_known(block, i ? value->op1 : value->op0,
                                         args, known, seen, count);
        known[count] = folds;
        seen[count++] = (vir_value_t *) value;
    }
    return !block->return_value ||
           vir_return_dup_known(block, block->return_value, args, known, seen,
                                count);
}

/* 1 when @edge now returns, 0 when it was left alone, -1 when the graph could
 * not be completed. Values are copied before the edge goes, so the only step
 * that can fail afterwards is attaching the return.
 */
static int vir_return_dup_edge(vir_function_t *func,
                               vir_block_t *block,
                               vir_edge_t *edge)
{
    vir_block_t *from = edge->from;
    vir_value_t *args[VIR_RETURN_DUP_PARAMS];
    vir_value_t *source[VIR_RETURN_DUP_VALUES];
    vir_value_t *copy[VIR_RETURN_DUP_VALUES];
    vir_value_t *result = NULL;
    int params = block->param_count;
    int count = 0;
    int constants = 0;

    if (from == block || from->terminator != VIR_TERM_JUMP ||
        from->outgoing != edge || edge->next_outgoing ||
        edge->arg_count != params)
        return 0;
    for (int i = 0; i < params; i++) {
        args[i] = edge->args[i];
        if (args[i]->opcode == VIR_OP_CONST)
            constants++;
    }
    for (const vir_value_t *value = block->head; value; value = value->next)
        count++;
    /* A copy that cannot fold would only grow the code. */
    if (!constants || (count > 2 && !vir_return_dup_folds(block, args)))
        return 0;
    count = 0;
    for (vir_value_t *value = block->head; value; value = value->next) {
        vir_value_t *op0 = value->op0
                               ? vir_return_dup_map(block, value->op0, args,
                                                    source, copy, count)
                               : NULL;
        vir_value_t *op1 = value->op1
                               ? vir_return_dup_map(block, value->op1, args,
                                                    source, copy, count)
                               : NULL;
        vir_value_t *clone = vir_clone_value(func, from, value, op0, op1);
        /* Failing here leaves only unused values, which DCE removes. */
        if (!clone)
            return 0;
        source[count] = value;
        copy[count++] = clone;
    }
    if (block->return_value) {
        result = vir_return_dup_map(block, block->return_value, args, source,
                                    copy, count);
        if (!result)
            return 0;
    }
    if (!vir_edge_delete(func, edge))
        return 0;
    return vir_block_set_return(func, from, result) ? 1 : -1;
}

/* Range fusion.
 *
 * `x < lo || x > hi` reaching one block, then one more, is two compares and two
 * branches. Whether x lies in [lo, hi] is one unsigned compare of x - lo with
 * hi - lo + 1, as long as lo <= hi and the range is not the whole type.
 */
#define VIR_RANGE_ARGS 8

/* Take apart `x < k` (a lower bound at k) or `k < x` (an upper bound at k). */
static bool vir_range_bound(const vir_value_t *condition,
                            vir_value_t **x,
                            unsigned long long *bound,
                            bool *lower)
{
    if (condition->def_effect || condition->nr_ops != 2 ||
        (condition->opcode != VIR_OP_SLT && condition->opcode != VIR_OP_ULT) ||
        !vir_type_is_i32_or_i64(condition->op0->type))
        return false;
    if (vir_is_constant(condition->op1) && !vir_is_constant(condition->op0)) {
        *x = condition->op0;
        *bound = condition->op1->constant;
        *lower = true;
        return true;
    }
    if (vir_is_constant(condition->op0) && !vir_is_constant(condition->op1)) {
        *x = condition->op1;
        *bound = condition->op0->constant;
        *lower = false;
        return true;
    }
    return false;
}

/* Whether every use of the values of @block is inside it, so that the block
 * can disappear without anything else noticing.
 */
static bool vir_range_block_private(const vir_block_t *block)
{
    for (const vir_value_t *value = block->head; value; value = value->next) {
        if (value->def_effect)
            return false;
        for (const vir_use_t *use = value->uses; use; use = use->next) {
            const vir_value_t *user = vir_use_user(use);
            if (user ? user->block != block
                     : vir_use_branch_block(use) != block)
                return false;
        }
    }
    return true;
}

static bool vir_range_same_args(const vir_edge_t *a, const vir_edge_t *b)
{
    if (a->arg_count != b->arg_count || a->arg_count > VIR_RANGE_ARGS)
        return false;
    for (int i = 0; i < a->arg_count; i++)
        if (a->args[i] != b->args[i] && !vir_const_same(a->args[i], b->args[i]))
            return false;
    return true;
}

/* 1 when the branch of @a and the one of the block after it became one, 0 when
 * the shape does not match, -1 when the graph could not be completed.
 */
static int vir_fuse_range(vir_function_t *func, vir_block_t *a)
{
    vir_block_t *b;
    vir_edge_t *out_a, *in_b;
    vir_edge_args_t yes, no;
    vir_value_t *x, *y, *low_const, *diff, *size_const, *inside;
    vir_value_t *out_args[VIR_RANGE_ARGS], *in_args[VIR_RANGE_ARGS];
    unsigned long long bound_a, bound_b, lo, hi, size, mask;
    bool lower_a, lower_b;
    vir_type_t type;
    int width;

    if (a->terminator != VIR_TERM_BRANCH || !a->true_edge || !a->false_edge ||
        !vir_range_bound(a->branch_condition, &x, &bound_a, &lower_a))
        return 0;
    b = a->false_edge->to;
    if (b == a || b == func->blocks || !b->incoming ||
        b->incoming->next_incoming || b->params || b->effects ||
        b->terminator != VIR_TERM_BRANCH || !b->true_edge || !b->false_edge ||
        !vir_range_bound(b->branch_condition, &y, &bound_b, &lower_b) ||
        y != x || lower_a == lower_b ||
        a->branch_condition->opcode != b->branch_condition->opcode ||
        a->true_edge->to != b->true_edge->to || a->true_edge->to == a ||
        a->true_edge->to == b || b->false_edge->to == a ||
        b->false_edge->to == b ||
        !vir_range_same_args(a->true_edge, b->true_edge) ||
        b->false_edge->arg_count > VIR_RANGE_ARGS ||
        !vir_range_block_private(b))
        return 0;
    type = x->type;
    width = vir_integer_type_width(type);
    mask = vir_width_mask(width);
    lo = lower_a ? bound_a : bound_b;
    hi = lower_a ? bound_b : bound_a;
    size = (hi - lo + 1) & mask;
    if (!size)
        return 0;
    if (a->branch_condition->opcode == VIR_OP_ULT
            ? lo > hi
            : ((lo ^ (1ULL << (width - 1))) > (hi ^ (1ULL << (width - 1)))))
        return 0;
    out_a = a->true_edge;
    in_b = b->false_edge;
    for (int i = 0; i < out_a->arg_count; i++)
        out_args[i] = out_a->args[i];
    for (int i = 0; i < in_b->arg_count; i++)
        in_args[i] = in_b->args[i];
    yes.to = in_b->to;
    yes.args = in_args;
    yes.arg_count = in_b->arg_count;
    no.to = out_a->to;
    no.args = out_args;
    no.arg_count = out_a->arg_count;

    low_const = vir_const_int(func, a, type, lo);
    diff = low_const ? vir_sub(func, a, x, low_const) : NULL;
    size_const = diff ? vir_const_int(func, a, type, size) : NULL;
    inside = size_const ? vir_ult(func, a, diff, size_const) : NULL;
    if (!inside)
        return 0;
    if (!vir_edge_delete(func, out_a) || !vir_edge_delete(func, a->outgoing))
        return -1;
    return vir_block_set_branch(func, a, inside, &yes, &no) ? 1 : -1;
}

int vir_fuse_range_tests(vir_function_t *func, vir_opt_level_t opt_level)
{
    int changed = 0;

    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level != VIR_OPT_O2)
        return 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        int status = vir_fuse_range(func, block);
        if (status < 0)
            return -1;
        changed += status;
    }
    return changed;
}

int vir_duplicate_returns(vir_function_t *func, vir_opt_level_t opt_level)
{
    int changed = 0;

    if (!func || !vir_opt_level_valid(opt_level))
        return -1;
    if (opt_level == VIR_OPT_O0)
        return 0;
    for (vir_block_t *block = func->blocks; block; block = block->next) {
        if (!vir_return_dup_candidate(func, block))
            continue;
        for (vir_edge_t *edge = block->incoming, *next; edge; edge = next) {
            next = edge->next_incoming;
            int status = vir_return_dup_edge(func, block, edge);
            if (status < 0)
                return -1;
            changed += status;
        }
    }
    return changed;
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
    unsigned char *in_loop = NULL;
    int *work = NULL;
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
                                      (size_t) count * sizeof(*work)))
        goto fail;
    bytes = (int) temporary_bytes;
    in_loop = calloc(count, sizeof(*in_loop));
    work = malloc(count * sizeof(*work));
    if (!in_loop || !work)
        goto fail;
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
                    &temporary_bytes, (size_t) value_count * sizeof(*ready)))
                goto fail;
            pending = malloc((size_t) value_count * sizeof(*pending));
            ready = malloc((size_t) value_count * sizeof(*ready));
            if (!pending || !ready)
                goto fail;
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
                vir_value_t *user = vir_use_user(use);
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
    if (stats)
        stats[VIR_LICM_STAT_VALUES_HOISTED] = hoisted;
    goto cleanup;
fail:
    hoisted = -1;
cleanup:
    vir_licm_release(in_loop, work, pending, ready, &dominance);
    return hoisted;
}

int vir_licm(vir_function_t *func, vir_opt_level_t opt_level)
{
    return vir_licm_with_stats(func, opt_level, NULL);
}

static bool vir_verify_reverse_uses(const vir_function_t *func,
                                    const vir_dominance_t *dominance,
                                    const vir_value_t *value)
{
    const vir_use_t *use;
    for (use = value->uses; use; use = use->next) {
        if (use->kind < VIR_USE_USER || use->kind > VIR_USE_BRANCH ||
            !use->owner)
            return 0;
        if (vir_use_user(use)) {
            if (!vir_has_value_with_context(func, dominance,
                                            vir_use_user(use)) ||
                (use->operand != 0 && use->operand != 1))
                return 0;
        } else if (vir_use_edge(use)) {
            if (vir_dominance_index(dominance, vir_use_edge(use)->from) < 0 ||
                vir_dominance_index(dominance, vir_use_edge(use)->to) < 0 ||
                !vir_has_outgoing_edge(vir_use_edge(use)->from,
                                       vir_use_edge(use)) ||
                use->operand < 0 ||
                use->operand >= vir_use_edge(use)->arg_count)
                return 0;
        } else if (vir_use_effect(use)) {
            const vir_effect_t *effect = vir_use_effect(use);
            if (vir_dominance_index(dominance, effect->block) < 0 ||
                !vir_has_effect(effect->block, effect))
                return 0;
            if (effect->kind == VIR_EFFECT_CALL) {
                if (use->operand == -1) {
                    if (effect->callee_value_use != use ||
                        vir_use_effect(effect->callee_value_use) != effect ||
                        effect->callee_value_use->operand != -1)
                        return 0;
                } else {
                    if (use->operand < 0 || use->operand >= effect->arg_count ||
                        !effect->args || !effect->arg_uses ||
                        vir_use_effect(&effect->arg_uses[use->operand]) !=
                            effect ||
                        effect->arg_uses[use->operand].operand != use->operand)
                        return 0;
                }
            } else {
                if (use->operand != 0 && use->operand != 1)
                    return 0;
            }
        } else if (vir_use_return_block(use)) {
            if (vir_dominance_index(dominance, vir_use_return_block(use)) < 0 ||
                vir_use_return_block(use)->terminator != VIR_TERM_RETURN)
                return 0;
        } else if (vir_dominance_index(dominance, vir_use_branch_block(use)) <
                       0 ||
                   vir_use_branch_block(use)->terminator != VIR_TERM_BRANCH) {
            return 0;
        }
        if (*vir_use_value_slot(use) != value)
            return 0;
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
    return !value->addr;
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
        value->op1 || value->def_effect || !value->addr || !value->addr->size ||
        !vir_valid_alignment(value->addr->alignment))
        return 0;
    if (value->opcode == VIR_OP_STACK_ADDR)
        return value->addr->kind == VIR_ADDRESS_STACK && !value->addr->name;
    return value->opcode == VIR_OP_GLOBAL_ADDR &&
           value->addr->kind == VIR_ADDRESS_GLOBAL && !value->addr->slot &&
           value->addr->name && *value->addr->name;
}

static bool vir_verify_effect_operand(const vir_function_t *func,
                                      const vir_dominance_t *dominance,
                                      const vir_effect_t *effect,
                                      const vir_value_t *value,
                                      const vir_use_t *use,
                                      int operand)
{
    return value && vir_has_value_with_context(func, dominance, value) && use &&
           vir_use_effect(use) == effect && use->operand == operand &&
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

        if (!operand || !vir_has_value_with_context(func, dominance, operand) ||
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
                            unsigned char *seen,
                            char **error)
{
    const vir_value_t *value;
    const vir_edge_t *edge;
    const vir_effect_t *effect;
    int position = 0;
    int terminator_position;
    int param_count = 0;
    if (vir_dominance_index(dominance, block) < 0)
        return vir_verify_invalid_order(error);
    for (value = block->params; value; value = value->param_next) {
        if (value->block != block || !value->is_block_param || value->nr_ops ||
            !vir_type_width(func, value->type) || value->id < 0 ||
            !vir_has_no_address_metadata(value) ||
            !vir_verify_value_id(func, seen, value->id))
            return vir_verify_invalid_value(error);
        if (!vir_verify_reverse_uses(func, dominance, value))
            return vir_verify_invalid_value(error);
        param_count++;
    }
    if ((block->params == NULL) != (block->last_param == NULL) ||
        (block->last_param && block->last_param->param_next) ||
        block->param_count != param_count)
        return vir_verify_invalid_order(error);
    for (value = block->head; value; value = value->next) {
        if (value->block != block || value->is_block_param ||
            value->position != position || value->id < 0 || value->order < 0 ||
            value->order >= block->next_order)
            return vir_verify_invalid_value(error);
        position++;
        if (!vir_verify_value_id(func, seen, value->id))
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
                    value->constant || !value->addr || value->addr->kind ||
                    value->addr->slot || value->addr->size ||
                    value->addr->alignment || !value->addr->name ||
                    !*value->addr->name)
                    return vir_verify_invalid_value(error);
            } else if (value->opcode == VIR_OP_RODATA_ADDR) {
                if (value->type != VIR_TYPE_PTR || value->def_effect)
                    return vir_verify_invalid_value(error);
            } else if (value->opcode == VIR_OP_STACK_ADDR ||
                       value->opcode == VIR_OP_GLOBAL_ADDR) {
                if (!vir_verify_address_value(value) ||
                    vir_address_root_conflicts(
                        func, value->addr->kind, value->addr->slot,
                        value->addr->name, value->addr->size,
                        value->addr->alignment))
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
        if (!vir_verify_reverse_uses(func, dominance, value))
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
                !vir_has_value_with_context(func, dominance, effect->result) ||
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
            !vir_has_value_with_context(func, dominance,
                                        block->branch_condition) ||
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
          (!vir_has_value_with_context(func, dominance, block->return_value) ||
           vir_return_use_count(block->return_value, block) != 1 ||
           !vir_value_dominates(func, dominance, block->return_value, block,
                                terminator_position)))))
        return vir_verify_invalid_value(error);
    for (edge = block->outgoing; edge; edge = edge->next_outgoing) {
        int i;
        vir_value_t *param;
        if (edge->from != block ||
            vir_dominance_index(dominance, edge->to) < 0 ||
            !vir_has_incoming_edge(edge->to, edge) ||
            edge->arg_count != edge->to->param_count)
            return vir_verify_invalid_order(error);
        param = edge->to->params;
        for (i = 0; i < edge->arg_count; i++) {
            if (!edge->args[i] ||
                !vir_has_value_with_context(func, dominance, edge->args[i]) ||
                !param || edge->args[i]->type != param->type ||
                vir_edge_use_count(edge->args[i], edge, i) != 1 ||
                !vir_value_dominates(func, dominance, edge->args[i], block,
                                     terminator_position))
                return vir_verify_invalid_value(error);
            param = param->param_next;
        }
    }
    for (edge = block->incoming; edge; edge = edge->next_incoming)
        if (edge->to != block ||
            vir_dominance_index(dominance, edge->from) < 0 ||
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
    unsigned char *seen =
        calloc(func->next_value_id > 0 ? (size_t) func->next_value_id : 1,
               sizeof(*seen));
    if (!seen) {
        vir_dominance_release(&dominance);
        return vir_verify_invalid_value(error);
    }
    int ok = 1;
    for (block = func->blocks; block && ok; block = block->next)
        ok = vir_verify_block(func, &dominance, block, seen, error);
    free(seen);
    vir_dominance_release(&dominance);
    if (!ok)
        return 0;
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
    struct vir_ssa_binding *block_next;
    /* Chains of the (block, variable) and parameter hash indexes. */
    struct vir_ssa_binding *variable_next;
    struct vir_ssa_binding *param_next;
} vir_ssa_binding_t;

/* Open hash of bindings: a lookup no longer walks every binding of a block. */
typedef struct {
    vir_ssa_binding_t **slots;
    int capacity;
    int count;
} vir_ssa_index_t;

typedef struct vir_ssa_block_state {
    vir_block_t *block;
    vir_ssa_binding_t *bindings;
    int sealed;
} vir_ssa_block_state_t;

/* Bindings, block states and per-edge scratch live in @arena until the builder
 * is released; a discarded binding waits on @spare for reuse.
 */
struct vir_ssa {
    vir_function_t *func;
    vir_arena_t arena;
    vir_ssa_binding_t *bindings;
    vir_ssa_binding_t *spare;
    vir_ssa_block_state_t **blocks;
    int block_capacity;
    vir_ssa_index_t by_variable;
    vir_ssa_index_t by_param;
    /* Set if an index could not grow; lookups then walk the block lists. */
    int unindexed;
};

static vir_ssa_binding_t *vir_ssa_new_binding(vir_ssa_t *ssa)
{
    vir_ssa_binding_t *binding = ssa->spare;
    if (!binding)
        return vir_arena_alloc(&ssa->arena, sizeof(*binding));
    ssa->spare = binding->next;
    memset(binding, 0, sizeof(*binding));
    return binding;
}

static void vir_ssa_drop_binding(vir_ssa_t *ssa, vir_ssa_binding_t *binding)
{
    binding->next = ssa->spare;
    ssa->spare = binding;
}

static vir_ssa_block_state_t *vir_ssa_find_block(const vir_ssa_t *ssa,
                                                 const vir_block_t *block)
{
    if (!block || block->id < 0 || block->id >= ssa->block_capacity)
        return NULL;
    vir_ssa_block_state_t *state = ssa->blocks[block->id];
    return state && state->block == block ? state : NULL;
}

/* The by-parameter index hashes @param alone; the by-variable index hashes
 * @block and @variable.
 */
static unsigned int vir_ssa_hash(const vir_block_t *block,
                                 unsigned int variable,
                                 const vir_value_t *param)
{
    return param ? (unsigned int) param->id * 2654435761u
                 : (unsigned int) block->id * 2654435761u ^ variable * 40503u;
}

static unsigned int vir_ssa_key(const vir_ssa_binding_t *binding, bool by_param)
{
    return vir_ssa_hash(binding->block, binding->variable,
                        by_param ? binding->param : NULL);
}

static vir_ssa_binding_t **vir_ssa_chain(vir_ssa_binding_t *binding,
                                         bool by_param)
{
    return by_param ? &binding->param_next : &binding->variable_next;
}

static void vir_ssa_index_insert(vir_ssa_t *ssa,
                                 vir_ssa_binding_t *binding,
                                 bool by_param)
{
    vir_ssa_index_t *index = by_param ? &ssa->by_param : &ssa->by_variable;

    if (ssa->unindexed)
        return;
    if (index->count >= index->capacity / 2) {
        int capacity = index->capacity ? index->capacity * 2 : 256;
        vir_ssa_binding_t **slots =
            capacity <= INT_MAX / (int) sizeof(*slots) / 2
                ? calloc((size_t) capacity, sizeof(*slots))
                : NULL;
        if (!slots) {
            ssa->unindexed = 1;
            return;
        }
        for (int i = 0; i < index->capacity; i++) {
            vir_ssa_binding_t *entry = index->slots[i];
            while (entry) {
                vir_ssa_binding_t *next = *vir_ssa_chain(entry, by_param);
                unsigned int bucket = vir_ssa_key(entry, by_param) &
                                      (unsigned int) (capacity - 1);
                *vir_ssa_chain(entry, by_param) = slots[bucket];
                slots[bucket] = entry;
                entry = next;
            }
        }
        free(index->slots);
        index->slots = slots;
        index->capacity = capacity;
    }
    unsigned int bucket =
        vir_ssa_key(binding, by_param) & (unsigned int) (index->capacity - 1);
    *vir_ssa_chain(binding, by_param) = index->slots[bucket];
    index->slots[bucket] = binding;
    index->count++;
}

static void vir_ssa_index_remove(vir_ssa_t *ssa,
                                 vir_ssa_binding_t *binding,
                                 bool by_param)
{
    vir_ssa_index_t *index = by_param ? &ssa->by_param : &ssa->by_variable;

    if (ssa->unindexed || !index->capacity)
        return;
    vir_ssa_binding_t **link =
        &index->slots[vir_ssa_key(binding, by_param) &
                      (unsigned int) (index->capacity - 1)];
    while (*link && *link != binding)
        link = vir_ssa_chain(*link, by_param);
    if (*link) {
        *link = *vir_ssa_chain(binding, by_param);
        index->count--;
    }
}

static void vir_ssa_link_binding(vir_ssa_t *ssa,
                                 vir_ssa_block_state_t *state,
                                 vir_ssa_binding_t *binding)
{
    binding->next = ssa->bindings;
    ssa->bindings = binding;
    binding->block_next = state->bindings;
    state->bindings = binding;
    vir_ssa_index_insert(ssa, binding, false);
    if (binding->param)
        vir_ssa_index_insert(ssa, binding, true);
}

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
        vir_ssa_block_state_t *state = vir_ssa_find_block(ssa, binding->block);
        vir_ssa_binding_t **local = &state->bindings;
        while (*local != binding)
            local = &(*local)->block_next;
        *local = binding->block_next;
        *link = binding->next;
        vir_ssa_index_remove(ssa, binding, false);
        if (binding->param)
            vir_ssa_index_remove(ssa, binding, true);
        vir_ssa_drop_binding(ssa, binding);
    }
    return true;
}

static vir_ssa_binding_t *vir_ssa_find_binding(const vir_ssa_t *ssa,
                                               const vir_block_t *block,
                                               unsigned int variable,
                                               const vir_value_t *param)
{
    if (!ssa->unindexed) {
        const vir_ssa_index_t *index =
            param ? &ssa->by_param : &ssa->by_variable;
        unsigned int hash;

        if (!index->capacity || !block)
            return NULL;
        hash = vir_ssa_hash(block, variable, param);
        for (vir_ssa_binding_t *binding =
                 index->slots[hash & (unsigned int) (index->capacity - 1)];
             binding;
             binding = param ? binding->param_next : binding->variable_next)
            if (binding->block == block &&
                (param ? binding->param == param
                       : binding->variable == variable))
                return binding;
        return NULL;
    }
    vir_ssa_block_state_t *state = vir_ssa_find_block(ssa, block);
    for (vir_ssa_binding_t *binding = state ? state->bindings : NULL; binding;
         binding = binding->block_next)
        if (param ? binding->param == param : binding->variable == variable)
            return binding;
    return NULL;
}

static vir_ssa_block_state_t *vir_ssa_block_state(vir_ssa_t *ssa,
                                                  vir_block_t *block,
                                                  int create)
{
    vir_ssa_block_state_t *state = vir_ssa_find_block(ssa, block);
    if (state || !create)
        return state;
    if (!block || block->id < 0 ||
        block->id >= INT_MAX / (int) sizeof(*ssa->blocks) / 2)
        return NULL;
    if (block->id >= ssa->block_capacity) {
        int capacity = (block->id + 1) * 2;
        vir_ssa_block_state_t **blocks =
            calloc((size_t) capacity, sizeof(*blocks));
        if (!blocks)
            return NULL;
        if (ssa->blocks)
            memcpy(blocks, ssa->blocks,
                   (size_t) ssa->block_capacity * sizeof(*blocks));
        free(ssa->blocks);
        ssa->blocks = blocks;
        ssa->block_capacity = capacity;
    }
    if (ssa->blocks[block->id])
        return NULL;
    state = vir_arena_alloc(&ssa->arena, sizeof(*state));
    if (!state)
        return NULL;
    state->block = block;
    ssa->blocks[block->id] = state;
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
    if (ssa) {
        ssa->func = func;
        ssa->arena.block_size = VIR_ARENA_BLOCK_SIZE;
        ssa->arena.fail_after = -1;
    }
    return ssa;
}

void vir_ssa_release(vir_ssa_t *ssa)
{
    if (!ssa)
        return;
    vir_arena_release(&ssa->arena);
    free(ssa->blocks);
    free(ssa->by_variable.slots);
    free(ssa->by_param.slots);
    free(ssa);
}

/* The dominance check in vir_ssa_write walks the whole function. A block whose
 * only predecessor already sees @value is dominated by that value's block, so
 * forwarding along such an edge skips the walk.
 */
int vir_ssa_forward(vir_ssa_t *ssa,
                    vir_block_t *block,
                    unsigned int variable,
                    vir_value_t *value)
{
    vir_ssa_binding_t *binding;
    if (!ssa || !value || !vir_has_block(ssa->func, block) ||
        !vir_has_value(ssa->func, value))
        return 0;
    binding = vir_ssa_find_binding(ssa, block, variable, NULL);
    if (!binding) {
        vir_ssa_block_state_t *state = vir_ssa_block_state(ssa, block, 1);
        if (!state)
            return 0;
        binding = vir_ssa_new_binding(ssa);
        if (!binding)
            return 0;
        binding->block = block;
        binding->variable = variable;
        vir_ssa_link_binding(ssa, state, binding);
    }
    binding->value = value;
    return 1;
}

int vir_ssa_write(vir_ssa_t *ssa,
                  vir_block_t *block,
                  unsigned int variable,
                  vir_value_t *value)
{
    if (!ssa || !value || !vir_has_block(ssa->func, block) ||
        !vir_has_value(ssa->func, value) ||
        !vir_value_dominates(ssa->func, NULL, value, block,
                             block->tail ? block->tail->position + 1 : 0))
        return 0;
    return vir_ssa_forward(ssa, block, variable, value);
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
    if (!value || !vir_ssa_forward(ssa, block, variable, value))
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
    binding = vir_ssa_new_binding(ssa);
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
    vir_ssa_link_binding(ssa, state, binding);
    vir_param_edit_release(&edit);
    return value;

fail:
    vir_param_edit_release(&edit);
    vir_ssa_drop_binding(ssa, binding);
    return NULL;
}

static int vir_ssa_seal_with_context(vir_ssa_t *ssa,
                                     vir_block_t *block,
                                     const vir_dominance_t *dominance)
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
    int reachable = -1;
    int index;
    int i;

    if (!ssa || (dominance ? vir_dominance_index(dominance, block) < 0
                           : !vir_has_block(ssa->func, block)))
        return 0;
    state = vir_ssa_block_state(ssa, block, 1);
    if (!state || state->sealed)
        return 0;
    param_count = block->param_count;
    trivial_params =
        param_count
            ? vir_arena_alloc(&ssa->arena,
                              param_count * (int) sizeof(*trivial_params))
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

        /* In reachable code the one other incoming value always dominates the
         * block, and a frontend that breaks that is caught here. A cycle cut
         * off from the entry can fail it; reachability removes that cycle
         * later, so its parameter is simply kept.
         */
        if (candidate && candidate != param && trivial) {
            if (!vir_replacement_dominates_with_context(ssa->func, dominance,
                                                        param, candidate)) {
                if (reachable < 0)
                    reachable = vir_block_reachable(ssa->func, block);
                if (reachable)
                    goto fail;
                continue;
            }
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
        vir_ssa_index_remove(ssa, trivial_params[i].binding, true);
        trivial_params[i].binding->param = NULL;
    }
    for (i = 0; i < trivial_count; i++)
        vir_param_edit_release(&trivial_params[i].edit);
    state->sealed = 1;
    return 1;

fail:
    for (i = 0; i < trivial_count; i++)
        vir_param_edit_release(&trivial_params[i].edit);
    return 0;
}

int vir_ssa_seal(vir_ssa_t *ssa, vir_block_t *block)
{
    return vir_ssa_seal_with_context(ssa, block, NULL);
}

/* Sealing removes parameters, not CFG edges, so one dominance context serves
 * the whole suffix of blocks. Keep the single-block API for incremental CFGs.
 */
int vir_ssa_seal_blocks(vir_ssa_t *ssa, vir_block_t *first)
{
    vir_dominance_t dominance;
    if (!ssa || !vir_dominance_init(ssa->func, &dominance))
        return 0;
    int ok = 1;
    for (vir_block_t *block = first; block && ok; block = block->next)
        ok = vir_ssa_seal_with_context(ssa, block, &dominance);
    vir_dominance_release(&dominance);
    return ok;
}

static bool vir_ssa_edge_args(vir_ssa_t *ssa,
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
        !(*args = vir_arena_alloc(&ssa->arena,
                                  to->param_count * (int) sizeof(**args))))
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
    *args = NULL;
    return false;
}

vir_edge_t *vir_ssa_jump(vir_ssa_t *ssa, vir_block_t *from, vir_block_t *to)
{
    vir_value_t **args;
    int count = to ? to->param_count : 0;
    if (!ssa || !from || !to || !vir_has_block(ssa->func, from) ||
        !vir_has_block(ssa->func, to) || vir_ssa_is_sealed(ssa, to))
        return NULL;
    if (!vir_ssa_edge_args(ssa, from, to, &args))
        return NULL;
    return vir_edge_create(ssa->func, from, to, args, count);
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
                value->addr->slot, value->addr->size, value->addr->alignment);
    else if (value->opcode == VIR_OP_GLOBAL_ADDR)
        fprintf(out, "  %%d%d = globaladdr @%s, %u, align %u\n", value->id,
                value->addr->name, value->addr->size, value->addr->alignment);
    else if (value->opcode == VIR_OP_FUNC_ADDR)
        fprintf(out, "  %%d%d = funcaddr @%s\n", value->id, value->addr->name);
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
