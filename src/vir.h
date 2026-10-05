/*
 * Typed value IR shared by the frontend, optimizers, and machine lowering.
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

/* Keep the compact type/opcode vocabulary in one place. The enum values,
 * verifier classification, and stable textual spelling are all derived from
 * these X-macro lists so adding an operation cannot leave one view stale.
 */
/* clang-format off */
/* Stage0 does not support multi-line macro continuations. */
#define VIR_TYPE_LIST VIR_TYPE_ITEM(VOID, "void", 0) VIR_TYPE_ITEM(I1, "i1", 1) VIR_TYPE_ITEM(I8, "i8", 8) VIR_TYPE_ITEM(I16, "i16", 16) VIR_TYPE_ITEM(I32, "i32", 32) VIR_TYPE_ITEM(I64, "i64", 64) VIR_TYPE_ITEM(PTR, "ptr", 0)
#define VIR_PURE_UNARY_OPCODE_LIST VIR_OPCODE_ITEM(NEG, "neg") VIR_OPCODE_ITEM(BITNOT, "bitnot.i32") VIR_OPCODE_ITEM(ZEXT, "zext") VIR_OPCODE_ITEM(SEXT, "sext") VIR_OPCODE_ITEM(TRUNC, "trunc") VIR_OPCODE_ITEM(PTRTOINT, "ptrtoint") VIR_OPCODE_ITEM(INTTOPTR, "inttoptr")
#define VIR_PURE_BINARY_OPCODE_LIST VIR_OPCODE_ITEM(ADD, "add.i32") VIR_OPCODE_ITEM(SUB, "sub.i32") VIR_OPCODE_ITEM(MUL, "mul.i32") VIR_OPCODE_ITEM(SDIV, "sdiv.i32") VIR_OPCODE_ITEM(UDIV, "udiv.i32") VIR_OPCODE_ITEM(SREM, "srem.i32") VIR_OPCODE_ITEM(UREM, "urem.i32") VIR_OPCODE_ITEM(SHL, "shl.i32") VIR_OPCODE_ITEM(ASHR, "ashr.i32") VIR_OPCODE_ITEM(LSHR, "lshr.i32") VIR_OPCODE_ITEM(BITAND, "bitand.i32") VIR_OPCODE_ITEM(BITOR, "bitor.i32") VIR_OPCODE_ITEM(BITXOR, "bitxor.i32") VIR_OPCODE_ITEM(EQ, "eq.i32") VIR_OPCODE_ITEM(SLT, "slt.i32") VIR_OPCODE_ITEM(ULT, "ult.i32") VIR_OPCODE_ITEM(PTRADD, "ptradd")
#define VIR_OPCODE_LIST VIR_OPCODE_ITEM(CONST, "const") VIR_PURE_BINARY_OPCODE_LIST VIR_PURE_UNARY_OPCODE_LIST VIR_OPCODE_ITEM(STACK_ADDR, "stackaddr") VIR_OPCODE_ITEM(GLOBAL_ADDR, "globaladdr") VIR_OPCODE_ITEM(LOAD, "load") VIR_OPCODE_ITEM(CALL, "call") VIR_OPCODE_ITEM(PARAM, "param") VIR_OPCODE_ITEM(RODATA_ADDR, "rodataaddr") VIR_OPCODE_ITEM(FUNC_ADDR, "funcaddr")
/* clang-format on */

typedef enum {
#define VIR_TYPE_ITEM(name, spelling, width) VIR_TYPE_##name,
    VIR_TYPE_LIST
#undef VIR_TYPE_ITEM
} vir_type_t;

typedef enum {
    VIR_OPT_O0,
    VIR_OPT_O1,
    VIR_OPT_O2,
} vir_opt_level_t;

typedef enum {
#define VIR_OPCODE_ITEM(name, spelling) VIR_OP_##name,
    VIR_OPCODE_LIST
#undef VIR_OPCODE_ITEM
} vir_opcode_t;

typedef struct vir_value vir_value_t;
typedef struct vir_block vir_block_t;
typedef struct vir_edge vir_edge_t;
typedef struct vir_use vir_use_t;
typedef struct vir_effect vir_effect_t;
typedef struct vir_arena_block vir_arena_block_t;
typedef struct vir_ssa vir_ssa_t;

typedef enum {
    VIR_ADDRESS_NONE,
    VIR_ADDRESS_STACK,
    VIR_ADDRESS_GLOBAL,
} vir_address_kind_t;

struct vir_use {
    vir_value_t *user;
    vir_edge_t *edge;
    vir_effect_t *effect;
    vir_block_t *return_block;
    vir_block_t *branch_block;
    vir_use_t *next;
    int operand;
};

struct vir_value {
    int id;
    vir_opcode_t opcode;
    vir_type_t type;
    int nr_ops;
    vir_value_t *op0;
    vir_value_t *op1;
    vir_use_t *uses;
    vir_block_t *block;
    int position;
    int order; /* Shared construction order with ordered effects. */
    unsigned long long constant;
    vir_value_t *next;
    vir_value_t *param_next;
    int is_block_param;
    vir_effect_t *def_effect;

    /* Root-object metadata exists only on VIR_OP_{STACK,GLOBAL}_ADDR. The
     * slot/name is stable source-independent provenance, not a backend offset.
     */
    vir_address_kind_t address_kind;
    unsigned int address_slot;
    unsigned int address_size;
    unsigned int address_alignment;
    const char *address_name;
};

typedef enum {
    VIR_TERM_NONE,
    VIR_TERM_JUMP,
    VIR_TERM_BRANCH,
    VIR_TERM_RETURN,
} vir_terminator_t;

typedef enum {
    VIR_EFFECT_LOAD,
    VIR_EFFECT_STORE,
    VIR_EFFECT_CALL,
    VIR_EFFECT_VOLATILE,
    VIR_EFFECT_VOLATILE_LOAD,
    VIR_EFFECT_VOLATILE_STORE,
} vir_effect_kind_t;

static inline bool vir_effect_is_load(vir_effect_kind_t kind)
{
    return kind == VIR_EFFECT_LOAD || kind == VIR_EFFECT_VOLATILE_LOAD;
}

static inline bool vir_effect_is_store(vir_effect_kind_t kind)
{
    return kind == VIR_EFFECT_STORE || kind == VIR_EFFECT_VOLATILE_STORE;
}

/* An attached call signature describes every passed argument. Variadic calls
 * retain the fixed parameter boundary for target ABI alignment. Signedness and
 * bool identity are retained because VIR integer opcodes intentionally encode
 * width, not C source-type rules.
 */
typedef struct {
    vir_type_t type;
    bool is_unsigned;
    bool is_bool;
} vir_call_abi_type_t;

typedef struct {
    vir_call_abi_type_t result;
    const vir_call_abi_type_t *params;
    int param_count;
    bool is_variadic;
    int fixed_param_count;
    const unsigned int *param_slots;
    unsigned int
        va_start_slot; /* UINT_MAX when no variadic cursor root exists. */
} vir_call_signature_t;

struct vir_block {
    int id;
    vir_value_t *head;
    vir_value_t *tail;
    vir_value_t *params;
    vir_value_t *last_param;
    int param_count;
    vir_edge_t *incoming;
    vir_edge_t *outgoing;
    vir_terminator_t terminator;
    vir_value_t *return_value;
    vir_value_t *branch_condition;
    vir_edge_t *true_edge;
    vir_edge_t *false_edge;
    vir_effect_t *effects;
    vir_effect_t *last_effect;
    int next_order;
    vir_block_t *next;
};

struct vir_effect {
    vir_effect_kind_t kind;
    vir_block_t *block;
    int position;
    int order;
    vir_value_t *address;
    vir_value_t *stored_value;
    vir_value_t *result;
    vir_use_t *address_use;
    vir_use_t *stored_value_use;
    const char *callee;
    vir_value_t *callee_value;
    vir_use_t *callee_value_use;
    vir_value_t **args;
    vir_use_t *arg_uses;
    int arg_count;
    vir_type_t result_type;
    const vir_call_signature_t *signature;
    vir_effect_t *next;
};

struct vir_edge {
    vir_block_t *from;
    vir_block_t *to;
    vir_value_t **args;
    vir_use_t *arg_uses;
    int arg_count;
    vir_edge_t *next_outgoing;
    vir_edge_t *next_incoming;
};

typedef struct {
    vir_block_t *to;
    vir_value_t **args;
    int arg_count;
} vir_edge_args_t;

struct vir_arena_block {
    vir_arena_block_t *next;
    int capacity;
    int used;
    char data[1];
};

typedef struct {
    vir_arena_block_t *head;
    int block_size;
    int capacity;
    int peak_capacity;
    int fail_after; /* Test-only: allocations remaining before forced failure.
                     */
} vir_arena_t;

typedef struct {
    vir_arena_t arena;
    vir_block_t *blocks;
    vir_block_t *last_block;
    int next_value_id;
    int next_block_id;
    /* Selected target layout, never inferred from the compiler host. */
    int pointer_bits;
} vir_function_t;

typedef struct {
    int blocks;
    int values;
    int params;
    int uses;
    int edges;
    int effects;
    int volatile_effects;
    int arena_capacity;
    int arena_peak_capacity;
} vir_stats_t;

typedef struct {
    int root_count;
    int max_edge_args;
    int max_call_args;
    bool has_calls;
    bool has_effects;
    bool has_globals;
} vir_function_shape_t;

typedef struct {
    /* Successfully allocated per-block tables; an empty block has one slot. */
    int tables;
    int table_slots; /* Slots in successfully allocated tables only. */
    int values;  /* Binary values scanned in successfully allocated tables. */
    int lookups; /* One key lookup for every counted value. */
    int hits;    /* Existing-key matches, including failed RAUW attempts. */
    int probes;  /* Occupied slots inspected while resolving lookups. */
    int replacements; /* Successful RAUW operations. */
} vir_cse_stats_t;

void vir_function_init(vir_function_t *func, int arena_block_size);

/* Bit width of an integer type; 0 for void and pointers. */
int vir_integer_type_width(vir_type_t type);
vir_type_t vir_integer_type_from_size(int bytes);

/* Returns false when an operation cannot be folded, including invalid division
 * and shift operands.
 */
bool vir_fold_i32_binary(vir_opcode_t opcode, int left, int right, int *result);
bool vir_type_is_scalar(vir_type_t type);
bool vir_call_shape_valid(const vir_effect_t *effect, int max_args);
bool vir_function_shape(const vir_function_t *func,
                        vir_function_shape_t *shape);
static inline bool vir_type_is_i32_or_i64(vir_type_t type)
{
    return type == VIR_TYPE_I32 || type == VIR_TYPE_I64;
}
static inline bool vir_type_is_i32_or_i64_or_pointer(vir_type_t type)
{
    return vir_type_is_i32_or_i64(type) || type == VIR_TYPE_PTR;
}
static inline bool vir_type_is_pointer_or_i64(vir_type_t type)
{
    return type == VIR_TYPE_PTR || type == VIR_TYPE_I64;
}
static inline bool vir_opcode_is_commutative(vir_opcode_t opcode)
{
    return opcode == VIR_OP_ADD || opcode == VIR_OP_MUL ||
           opcode == VIR_OP_BITAND || opcode == VIR_OP_BITOR ||
           opcode == VIR_OP_BITXOR || opcode == VIR_OP_EQ;
}
static inline bool vir_opcode_is_compare(vir_opcode_t opcode)
{
    return opcode == VIR_OP_EQ || opcode == VIR_OP_SLT || opcode == VIR_OP_ULT;
}
static inline bool vir_opcode_is_shift(vir_opcode_t opcode)
{
    return opcode == VIR_OP_SHL || opcode == VIR_OP_ASHR ||
           opcode == VIR_OP_LSHR;
}
static inline bool vir_opcode_is_division(vir_opcode_t opcode)
{
    return opcode == VIR_OP_SDIV || opcode == VIR_OP_UDIV ||
           opcode == VIR_OP_SREM || opcode == VIR_OP_UREM;
}
/* Conservatively include division and remainder, which can fault at runtime. */
static inline bool vir_opcode_may_trap(vir_opcode_t opcode)
{
    return vir_opcode_is_division(opcode);
}

/* Must be called before creating blocks; only the currently supported target
 * pointer widths are admitted. Pointer constants and ptr/int casts require this
 * explicit layout.
 */
int vir_function_set_pointer_bits(vir_function_t *func, int pointer_bits);
void vir_function_release(vir_function_t *func);
void vir_collect_stats(const vir_function_t *func, vir_stats_t *stats);
vir_block_t *vir_block_create(vir_function_t *func);
vir_value_t *vir_block_add_param(vir_function_t *func,
                                 vir_block_t *block,
                                 vir_type_t type);
vir_edge_t *vir_edge_create(vir_function_t *func,
                            vir_block_t *from,
                            vir_block_t *to,
                            vir_value_t **args,
                            int arg_count);
/* Deleting a conditional arm atomically promotes the other arm to a jump. */
int vir_edge_delete(vir_function_t *func, vir_edge_t *edge);
int vir_edge_redirect(vir_function_t *func, vir_edge_t *edge, vir_block_t *to);
vir_block_t *vir_edge_split(vir_function_t *func, vir_edge_t *edge);

/* Remove unreachable blocks after validating that every dead value has only
 * dead uses. The transformation leaves the function unchanged on failure.
 */
int vir_remove_unreachable(vir_function_t *func);
int vir_function_block_count(const vir_function_t *func);

/* At O1/O2, redirect through empty parameter-free jump blocks. The regular
 * helper also drops newly unreachable blocks; the explicit helper can defer
 * that deletion for a caller that must run another pass on the rewritten
 * topology. Effects and block arguments are intentionally out of scope for this
 * first CFG simplification slice. A deferred caller must prune before lowering
 * (and before any final reachability-sensitive verification).
 */
int vir_simplify_cfg(vir_function_t *func, vir_opt_level_t opt_level);
int vir_simplify_cfg_with_pruning(vir_function_t *func,
                                  vir_opt_level_t opt_level,
                                  bool prune_unreachable);
int vir_block_set_branch(vir_function_t *func,
                         vir_block_t *block,
                         vir_value_t *condition,
                         const vir_edge_args_t *true_edge,
                         const vir_edge_args_t *false_edge);
int vir_block_set_return(vir_function_t *func,
                         vir_block_t *block,
                         vir_value_t *value);

/* Only payload-less CALL/VOLATILE placeholders use this constructor. Stores
 * must carry their typed address/value uses through vir_store().
 */
vir_effect_t *vir_effect_create(vir_function_t *func,
                                vir_block_t *block,
                                vir_effect_kind_t kind);
vir_value_t *vir_load(vir_function_t *func,
                      vir_block_t *block,
                      vir_value_t *address,
                      vir_type_t type);
vir_effect_t *vir_store(vir_function_t *func,
                        vir_block_t *block,
                        vir_value_t *address,
                        vir_value_t *value);
vir_value_t *vir_volatile_load(vir_function_t *func,
                               vir_block_t *block,
                               vir_value_t *address,
                               vir_type_t type);
vir_effect_t *vir_volatile_store(vir_function_t *func,
                                 vir_block_t *block,
                                 vir_value_t *address,
                                 vir_value_t *value);

/* Frontend construction may reserve a source-ordered load/store before its
 * merged operands are available. Resolve it only after CFG construction.
 */
vir_value_t *vir_deferred_load(vir_function_t *func,
                               vir_block_t *block,
                               vir_type_t type,
                               bool is_volatile);
vir_effect_t *vir_deferred_store(vir_function_t *func,
                                 vir_block_t *block,
                                 bool is_volatile);
bool vir_deferred_load_resolve(vir_function_t *func,
                               vir_effect_t *effect,
                               vir_value_t *address);
bool vir_deferred_store_resolve(vir_function_t *func,
                                vir_effect_t *effect,
                                vir_value_t *address,
                                vir_value_t *value);

/* Calls are ordered effects. A result_type of VIR_TYPE_VOID leaves result NULL.
 * Indirect targets are explicit pointer-valued operands; ABI and alias
 * semantics remain below VIR.
 */
vir_effect_t *vir_call(vir_function_t *func,
                       vir_block_t *block,
                       const char *callee,
                       vir_value_t **args,
                       int arg_count,
                       vir_type_t result_type);
vir_effect_t *vir_call_indirect(vir_function_t *func,
                                vir_block_t *block,
                                vir_value_t *callee,
                                vir_value_t **args,
                                int arg_count,
                                vir_type_t result_type);
bool vir_call_set_signature(vir_function_t *func,
                            vir_effect_t *effect,
                            const vir_call_signature_t *signature);
vir_value_t *vir_const_i32(vir_function_t *func,
                           vir_block_t *block,
                           int constant);
vir_value_t *vir_const_i1(vir_function_t *func,
                          vir_block_t *block,
                          int constant);
vir_value_t *vir_const_int(vir_function_t *func,
                           vir_block_t *block,
                           vir_type_t type,
                           unsigned long long bits);
vir_value_t *vir_const_ptr(vir_function_t *func,
                           vir_block_t *block,
                           unsigned long long bits);

/* Materialize an addressable object root. Repeated identities must retain the
 * same extent/alignment; byte offset and ABI placement remain below VIR.
 */
vir_value_t *vir_stack_addr(vir_function_t *func,
                            vir_block_t *block,
                            unsigned int slot,
                            unsigned int size,
                            unsigned int alignment);
vir_value_t *vir_global_addr(vir_function_t *func,
                             vir_block_t *block,
                             const char *name,
                             unsigned int size,
                             unsigned int alignment);

/* The address of a string literal, @offset bytes into read-only data. It is an
 * opaque pointer: no root, so nothing is assumed about what it points into.
 */
bool vir_effect_remove(vir_function_t *func, vir_effect_t *effect);
vir_value_t *vir_function_addr(vir_function_t *func,
                               vir_block_t *block,
                               const char *name);

vir_value_t *vir_rodata_addr(vir_function_t *func,
                             vir_block_t *block,
                             unsigned int offset);
vir_value_t *vir_binary(vir_function_t *func,
                        vir_block_t *block,
                        vir_opcode_t opcode,
                        vir_type_t type,
                        vir_value_t *op0,
                        vir_value_t *op1);
#define VIR_DECLARE(name, ...) \
    vir_value_t *name(vir_function_t *func, vir_block_t *block, __VA_ARGS__)
VIR_DECLARE(vir_add, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_sub, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_mul, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_div,
            vir_value_t *op0,
            vir_value_t *op1,
            bool is_unsigned,
            bool remainder);
VIR_DECLARE(vir_shl, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_ashr, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_lshr, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_bitand, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_bitor, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_bitxor, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_eq, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_slt, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_ult, vir_value_t *op0, vir_value_t *op1);
VIR_DECLARE(vir_neg, vir_value_t *operand);
VIR_DECLARE(vir_bitnot, vir_value_t *operand);
VIR_DECLARE(vir_zext_i1, vir_value_t *operand);
VIR_DECLARE(vir_trunc_i1, vir_value_t *operand);
VIR_DECLARE(vir_zext, vir_value_t *operand, vir_type_t type);
VIR_DECLARE(vir_sext, vir_value_t *operand, vir_type_t type);
VIR_DECLARE(vir_trunc, vir_value_t *operand, vir_type_t type);
VIR_DECLARE(vir_ptrtoint, vir_value_t *operand, vir_type_t type);
VIR_DECLARE(vir_inttoptr, vir_value_t *operand);
#undef VIR_DECLARE
/* Add a byte offset represented at the selected target pointer width. */
vir_value_t *vir_ptradd(vir_function_t *func,
                        vir_block_t *block,
                        vir_value_t *base,
                        vir_value_t *byte_offset);
int vir_replace_all_uses(vir_function_t *func,
                         vir_value_t *from,
                         vir_value_t *to);
int vir_local_cse(vir_function_t *func, vir_opt_level_t opt_level);
int vir_simplify_params(vir_function_t *func, vir_opt_level_t opt_level);
int vir_strength_reduce(vir_function_t *func, vir_opt_level_t opt_level);

/* Optional pass-local accounting; no state is retained in VIR objects. Stats
 * are zeroed before validation and before O0's no-work return. A failed table
 * allocation leaves that block and its counters untouched; the pass continues
 * with later blocks and returns its successful replacement count.
 */
int vir_local_cse_with_stats(vir_function_t *func,
                             vir_opt_level_t opt_level,
                             vir_cse_stats_t *stats);

/* O2-only function-wide value numbering. The temporary table is pass state;
 * dominance-checked RAUW rejects leaders unavailable at a use.
 */
int vir_gvn_with_stats(vir_function_t *func,
                       vir_opt_level_t opt_level,
                       vir_cse_stats_t *stats);

/* O2-only LICM derives natural loops from temporary dominator state. It moves
 * only pure values whose operands are already outside a loop into its existing
 * single-edge preheader; effects, block parameters, and CFG are untouched.
 */
#define VIR_LICM_STAT_LOOPS 0
#define VIR_LICM_STAT_VALUES_HOISTED 1
#define VIR_LICM_STAT_TEMPORARY_BYTES 2
#define VIR_LICM_STAT_VALUE_WORKLIST_ALLOCS 3
#define VIR_LICM_STAT_READINESS_SAMPLE_VALUES 4
#define VIR_LICM_STAT_READINESS_EAGER_ADMISSIONS 5
#define VIR_LICM_STAT_COUNT 6
#define VIR_LICM_READINESS_SAMPLE_BLOCK_LIMIT 8
#define VIR_LICM_READINESS_SAMPLE_VALUE_LIMIT 64
#define VIR_LICM_READINESS_SAMPLE_MAX_VALUES \
    (VIR_LICM_READINESS_SAMPLE_BLOCK_LIMIT * \
     VIR_LICM_READINESS_SAMPLE_VALUE_LIMIT)
int vir_licm_with_stats(vir_function_t *func,
                        vir_opt_level_t opt_level,
                        int *stats);
int vir_licm(vir_function_t *func, vir_opt_level_t opt_level);

/* Remove unused pure values at O1/O2. Effects, their results, and block
 * parameters remain intact: removing ordered effects requires a separate
 * effect-semantics pass. The arena retains reclaimed object storage until the
 * function completes, so this pass reduces downstream work, not peak arena
 * capacity.
 */
int vir_dce(vir_function_t *func, vir_opt_level_t opt_level);
int vir_dce_with_stats(vir_function_t *func,
                       vir_opt_level_t opt_level,
                       int *passes,
                       int *values_scanned,
                       int *values_removed,
                       int *temporary_bytes,
                       int *temporary_peak_bytes);

/* Sparse conditional constant propagation keeps its lattice, executable-CFG
 * markers, and block-index work queue in function-scoped temporary arrays. It
 * only reasons about immutable pure values; memory and calls are conservatively
 * overdefined. CFG changes delete branches proved constant and, in the regular
 * helpers, remove unreachable blocks. The explicit pruning variant can defer
 * that removal; its caller must invoke vir_remove_unreachable() before lowering
 * (and before any final reachability-sensitive verification).
 */
int vir_sccp(vir_function_t *func, vir_opt_level_t opt_level);

/* Keep these as macros rather than a new enum type: the self-hosted frontend
 * has a fixed type table, while callers only need integer array indices.
 */
#define VIR_SCCP_STAT_PASSES 0
#define VIR_SCCP_STAT_VALUES_SCANNED 1
#define VIR_SCCP_STAT_VALUES_REPLACED 2
#define VIR_SCCP_STAT_BRANCHES_SIMPLIFIED 3
#define VIR_SCCP_STAT_BLOCKS_REMOVED 4
#define VIR_SCCP_STAT_TEMPORARY_BYTES 5
#define VIR_SCCP_STAT_TEMPORARY_PEAK_BYTES 6
#define VIR_SCCP_STAT_COUNT 7
int vir_sccp_with_stats(vir_function_t *func,
                        vir_opt_level_t opt_level,
                        int *stats);
int vir_sccp_with_stats_and_pruning(vir_function_t *func,
                                    vir_opt_level_t opt_level,
                                    int *stats,
                                    bool prune_unreachable);
int vir_verify(const vir_function_t *func, char **error);
vir_block_t *vir_use_block(const vir_use_t *use);
int vir_use_order(const vir_use_t *use);
void vir_print(const vir_function_t *func, FILE *out);
vir_ssa_t *vir_ssa_create(vir_function_t *func);
void vir_ssa_release(vir_ssa_t *ssa);
vir_value_t *vir_ssa_predeclare(vir_ssa_t *ssa,
                                vir_block_t *block,
                                unsigned int variable,
                                vir_type_t type);

/* Discard scratch bindings before wiring CFG edges, once provisional memory
 * reads have replaced their SSA users. Live parameters are never removed.
 */
bool vir_ssa_discard_variable(vir_ssa_t *ssa, unsigned int variable);

int vir_ssa_write(vir_ssa_t *ssa,
                  vir_block_t *block,
                  unsigned int variable,
                  vir_value_t *value);
vir_value_t *vir_ssa_read(const vir_ssa_t *ssa,
                          const vir_block_t *block,
                          unsigned int variable);
bool vir_ssa_is_param(const vir_ssa_t *ssa,
                      const vir_block_t *block,
                      unsigned int variable);
vir_value_t *vir_ssa_read_sealed(vir_ssa_t *ssa,
                                 vir_block_t *block,
                                 unsigned int variable);
int vir_ssa_seal(vir_ssa_t *ssa, vir_block_t *block);
vir_edge_t *vir_ssa_jump(vir_ssa_t *ssa, vir_block_t *from, vir_block_t *to);
int vir_ssa_branch(vir_ssa_t *ssa,
                   vir_block_t *from,
                   vir_value_t *condition,
                   vir_block_t *true_to,
                   vir_block_t *false_to);
