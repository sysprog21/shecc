#include "vir-frontend.h"
#include <stdlib.h>
#include <string.h>
#include "vir.h"

int vir_frontend_opt_level = VIR_OPT_O1;
extern int unevaluated_expression_depth;
int alignment_var(var_t *var);
static int type_bytes(vir_type_t type)
{
    return type == VIR_TYPE_PTR ? PTR_SIZE : vir_integer_type_width(type) / 8;
}
bool vir_frontend_fold_i32(opcode_t opcode, int left, int right, int *result)
{
    vir_opcode_t fold_opcode;
    bool invert = false, swap = false;

    switch (opcode) {
    case OP_add:
        fold_opcode = VIR_OP_ADD;
        break;
    case OP_sub:
        fold_opcode = VIR_OP_SUB;
        break;
    case OP_mul:
        fold_opcode = VIR_OP_MUL;
        break;
    case OP_div:
        fold_opcode = VIR_OP_SDIV;
        break;
    case OP_mod:
        fold_opcode = VIR_OP_SREM;
        break;
    case OP_lshift:
        fold_opcode = VIR_OP_SHL;
        break;
    case OP_rshift:
        fold_opcode = VIR_OP_ASHR;
        break;
    case OP_bit_and:
        fold_opcode = VIR_OP_BITAND;
        break;
    case OP_bit_or:
        fold_opcode = VIR_OP_BITOR;
        break;
    case OP_bit_xor:
        fold_opcode = VIR_OP_BITXOR;
        break;
    case OP_eq:
        fold_opcode = VIR_OP_EQ;
        break;
    case OP_neq:
        fold_opcode = VIR_OP_EQ;
        invert = true;
        break;
    case OP_lt:
        fold_opcode = VIR_OP_SLT;
        break;
    case OP_leq:
        fold_opcode = VIR_OP_SLT;
        swap = invert = true;
        break;
    case OP_gt:
        fold_opcode = VIR_OP_SLT;
        swap = true;
        break;
    case OP_geq:
        fold_opcode = VIR_OP_SLT;
        invert = true;
        break;
    case OP_log_and:
    case OP_log_or:
        *result = opcode == OP_log_and ? left && right : left || right;
        return true;
    default:
        return false;
    }
    if (!vir_fold_i32_binary(fold_opcode, swap ? right : left,
                             swap ? left : right, result))
        return false;
    if (invert)
        *result = !*result;
    return true;
}

static vir_value_t *native_integer_binary(vir_function_t *func,
                                          vir_block_t *block,
                                          opcode_t opcode,
                                          vir_value_t *left,
                                          vir_value_t *right,
                                          bool is_unsigned)
{
    if (!func || !block || !left || !right)
        return NULL;
    switch (opcode) {
    case OP_add:
        return vir_add(func, block, left, right);
    case OP_sub:
        return vir_sub(func, block, left, right);
    case OP_mul:
        return vir_mul(func, block, left, right);
    case OP_lshift:
        return vir_shl(func, block, left, right);
    case OP_bit_and:
        return vir_bitand(func, block, left, right);
    case OP_bit_or:
        return vir_bitor(func, block, left, right);
    case OP_bit_xor:
        return vir_bitxor(func, block, left, right);
    case OP_div:
    case OP_mod:
        return vir_div(func, block, left, right, is_unsigned, opcode == OP_mod);
    case OP_rshift:
        return is_unsigned ? vir_lshr(func, block, left, right)
                           : vir_ashr(func, block, left, right);
    default:
        return NULL;
    }
}

/* Every parsed body owns one value graph. Parser blocks are cursors, not a
 * second instruction representation. Edges are wired after parsing so forward
 * labels and loop backedges can supply every predeclared block argument.
 */
typedef struct native_root {
    var_t *object;
    const char *symbol;
    vir_value_t *address;
    unsigned int slot;
    bool temporary_initialized;
    struct native_root *next;
} native_root_t;
typedef struct native_store {
    var_t *object;
    vir_effect_t *effect;
    struct native_store *next;
} native_store_t;
typedef struct native_load {
    var_t *object;
    vir_effect_t *effect;
    vir_value_t *ssa_value;
    struct native_load *next;
} native_load_t;
typedef struct native_block {
    basic_block_t *cursor;
    vir_value_t *condition;
    struct native_block *next;
} native_block_t;
typedef struct native_source {
    var_t *object;
    unsigned int id;
    vir_type_t type;
    bool is_parameter;
    bool is_object;
    struct native_source *next;
} native_source_t;
typedef struct native_param {
    basic_block_t *cursor;
    var_t *object;
    vir_value_t *value;
    struct native_param *next;
} native_param_t;
typedef struct native_function {
    func_t *source;
    vir_function_t graph;
    vir_ssa_t *ssa;
    vir_call_signature_t signature;
    native_block_t *blocks, *last_block;
    native_block_t **block_table;
    int block_count, block_capacity;
    native_source_t *sources;
    native_param_t *params;
    native_root_t *roots;
    native_store_t *stores;
    native_load_t *loads;
    unsigned int source_count, root_count;
    native_source_t **source_table;
    int source_capacity;
    vir_value_t **args;
    var_t **arg_sources;
    int arg_count, arg_capacity;
    basic_block_t *call_block;
    const char *callee;
    var_t *callee_source;
    vir_value_t *target;
    vir_value_t *va_start;
    bool finished;
    struct native_function *next;
} native_function_t;
static native_function_t *native_functions;

static void native_require(bool ok, const char *operation)
{
    if (!ok) {
        fprintf(stderr, "VIR construction failed: %s\n", operation);
        fatal("Invalid native VIR construction");
    }
}
static vir_type_t native_type(const var_t *var)
{
    if (!var || !var->type)
        return VIR_TYPE_VOID;
    if (var->ptr_level || var->type->ptr_level || var->is_func ||
        var->array_size || var->has_unsized_array || is_record_type(var->type))
        return VIR_TYPE_PTR;
    if (var->type->is_bool)
        return VIR_TYPE_I1;
    return vir_integer_type_from_size(var->type->size);
}
static bool native_unsigned(const var_t *var)
{
    return var && var->type && var->type->is_unsigned;
}
static bool native_object(const var_t *var)
{
    return var && var->var_name && *var->var_name && var->var_name[0] != '.' &&
           !var->is_function_designator;
}
static native_block_t *native_block(native_function_t *ctx,
                                    basic_block_t *cursor);
static native_function_t *native_context(func_t *func)
{
    native_function_t *ctx;
    native_require(func != NULL, "function identity");
    if (func->vir_context)
        return func->vir_context;
    ctx = arena_calloc(GENERAL_ARENA, 1, sizeof(*ctx));
    ctx->source = func;
    ctx->signature.va_start_slot = (unsigned int) -1;
    vir_function_init(&ctx->graph, 1024);
    native_require(vir_function_set_pointer_bits(&ctx->graph, PTR_SIZE * 8),
                   "pointer width");
    ctx->ssa = vir_ssa_create(&ctx->graph);
    native_require(ctx->ssa != NULL, "SSA state");
    func->vir_context = ctx;
    ctx->next = native_functions;
    native_functions = ctx;
    if (func->bbs)
        native_block(ctx, func->bbs);
    return ctx;
}
static native_block_t *native_block(native_function_t *ctx,
                                    basic_block_t *cursor)
{
    native_block_t *node;
    native_require(cursor && cursor->belong_to == ctx->source,
                   "block ownership");
    int index = cursor->vir_map_id - 1;
    if (index >= 0 && index < ctx->block_count &&
        ctx->block_table[index]->cursor == cursor)
        return ctx->block_table[index];
    node = arena_calloc(GENERAL_ARENA, 1, sizeof(*node));
    if (ctx->block_count >= ctx->block_capacity)
        ctx->block_table = arena_grow(GENERAL_ARENA, (char *) ctx->block_table,
                                      &ctx->block_capacity,
                                      sizeof(*ctx->block_table), 8, 0, NULL);
    ctx->block_table[ctx->block_count++] = node;
    cursor->vir_map_id = ctx->block_count;
    node->cursor = cursor;
    cursor->vir = vir_block_create(&ctx->graph);
    native_require(cursor->vir != NULL, "block allocation");
    if (ctx->last_block)
        ctx->last_block->next = node;
    else
        ctx->blocks = node;
    ctx->last_block = node;
    return node;
}
static unsigned int native_source(native_function_t *ctx, var_t *var)
{
    native_source_t *source;
    int index = var->vir_source_index - 1;
    if (index >= 0 && (unsigned int) index < ctx->source_count &&
        ctx->source_table[index]->object == var)
        return index;
    if ((int) ctx->source_count >= ctx->source_capacity)
        ctx->source_table = arena_grow(
            GENERAL_ARENA, (char *) ctx->source_table, &ctx->source_capacity,
            sizeof(*ctx->source_table), 16, 0, NULL);
    source = arena_alloc(GENERAL_ARENA, sizeof(*source));
    source->object = var;
    source->id = ctx->source_count++;
    source->type = native_type(var);
    source->is_parameter = false;
    source->is_object = false;
    ctx->source_table[source->id] = source;
    var->vir_source_index = source->id + 1;
    source->next = ctx->sources;
    ctx->sources = source;
    return source->id;
}
static native_source_t *native_source_info(native_function_t *ctx, var_t *var)
{
    unsigned int id = native_source(ctx, var);
    return ctx->source_table[id];
}
/* Adjusted array formals are pointer objects; array views remain SSA values. */
static bool native_array_parameter(native_function_t *ctx, var_t *var)
{
    return (var->array_size || var->has_unsized_array) &&
           native_source_info(ctx, var)->is_parameter;
}
static bool native_storage_volatile(native_function_t *ctx, var_t *var)
{
    native_source_t *source = native_source_info(ctx, var);
    return (native_object(var) || source->is_object || source->is_parameter) &&
           var_volatile_storage(var);
}
static bool native_memory(native_function_t *ctx, var_t *var)
{
    return var && (var->is_global || var->address_taken ||
                   native_storage_volatile(ctx, var) ||
                   ((var->array_size || var->has_unsized_array) &&
                    !native_array_parameter(ctx, var)) ||
                   (!var->ptr_level && is_record_type(var->type) &&
                    !native_array_parameter(ctx, var)));
}
static bool native_temporary(native_function_t *ctx, var_t *var)
{
    return var->var_name[0] == '.' && !var->is_global &&
           !native_source_info(ctx, var)->is_object &&
           (!var->address_taken || var->array_size || var->has_unsized_array);
}
static unsigned int native_storage_size(native_function_t *ctx, var_t *var)
{
    native_source_t *source = native_source_info(ctx, var);
    if (source->is_parameter && (var->array_size || var->has_unsized_array))
        return PTR_SIZE;
    if (var->var_name[0] == '.' && !var->array_size &&
        !var->has_unsized_array && !is_record_type(var->type))
        return source->type == VIR_TYPE_I1 ? 1 : type_bytes(source->type);
    return size_var(var);
}
static vir_value_t *native_not(vir_function_t *graph,
                               vir_block_t *block,
                               vir_value_t *value)
{
    vir_value_t *zero = vir_const_i1(graph, block, 0);
    return vir_eq(graph, block, value, zero);
}
static vir_value_t *native_convert(native_function_t *ctx,
                                   vir_block_t *block,
                                   vir_value_t *value,
                                   vir_type_t type,
                                   bool is_unsigned)
{
    int from, to;
    if (!value || type == VIR_TYPE_VOID)
        return NULL;
    if (value->type == type)
        return value;
    if (type == VIR_TYPE_I1) {
        vir_value_t *zero =
            value->type == VIR_TYPE_PTR
                ? vir_const_ptr(&ctx->graph, block, 0)
                : vir_const_int(&ctx->graph, block, value->type, 0);
        vir_value_t *equal = vir_eq(&ctx->graph, block, value, zero);
        return native_not(&ctx->graph, block, equal);
    }
    if (value->type == VIR_TYPE_PTR) {
        value = vir_ptrtoint(&ctx->graph, block, value,
                             PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32);
        return native_convert(ctx, block, value, type, is_unsigned);
    }
    if (type == VIR_TYPE_PTR) {
        value = native_convert(ctx, block, value,
                               PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32,
                               is_unsigned);
        return vir_inttoptr(&ctx->graph, block, value);
    }
    from = vir_integer_type_width(value->type);
    to = vir_integer_type_width(type);
    if (from > to)
        return vir_trunc(&ctx->graph, block, value, type);
    return is_unsigned || value->type == VIR_TYPE_I1
               ? vir_zext(&ctx->graph, block, value, type)
               : vir_sext(&ctx->graph, block, value, type);
}
static native_root_t *native_root(native_function_t *ctx, var_t *var)
{
    native_root_t *root;
    unsigned int size, alignment;
    for (root = ctx->roots; root; root = root->next)
        if (root->object == var)
            return root;
    root = arena_calloc(GENERAL_ARENA, 1, sizeof(*root));
    root->object = var;
    root->slot = ctx->root_count++;
    root->next = ctx->roots;
    ctx->roots = root;
    root->symbol = var->var_name;
    if (var->is_global && var->scope && ((block_t *) var->scope)->func) {
        if (ctx->source == GLOBAL_FUNC) {
            char name[MAX_VAR_LEN];
            snprintf(name, sizeof(name), ".vir.global.%u", root->slot);
            root->symbol = intern_string(name);
        } else {
            root->symbol =
                native_root(native_context(GLOBAL_FUNC), var)->symbol;
        }
    }
    size = native_storage_size(ctx, var);
    if (!size && var->has_unsized_array)
        size = var->ptr_level ? PTR_SIZE : var->type->size;
    native_require(size > 0, "object extent");
    alignment = alignment_var(var);
    vir_block_t *entry = native_block(ctx, ctx->source->bbs)->cursor->vir;
    root->address =
        var->is_global
            ? vir_global_addr(&ctx->graph, entry, root->symbol, size, alignment)
            : vir_stack_addr(&ctx->graph, entry, root->slot, size, alignment);
    native_require(root->address != NULL, "storage root");
    return root;
}
static vir_value_t *native_address(native_function_t *ctx,
                                   vir_block_t *block,
                                   var_t *var)
{
    if (var->is_aggregate_param)
        return vir_ssa_read(ctx->ssa, ctx->source->bbs->vir,
                            native_source(ctx, var));
    if (var->var_name[0] == '.' && !var->is_global &&
        !native_source_info(ctx, var)->is_object &&
        (var->array_size || var->has_unsized_array ||
         (!var->ptr_level && var->type->array_size))) {
        vir_value_t *value =
            vir_ssa_read(ctx->ssa, block, native_source(ctx, var));
        if (value && value->type == VIR_TYPE_PTR)
            return value;
    }
    native_root_t *root = native_root(ctx, var);
    if (!root->temporary_initialized && var->var_name[0] == '.' &&
        !var->is_global && !native_source_info(ctx, var)->is_object) {
        root->temporary_initialized = true;
        vir_value_t *value =
            vir_ssa_read(ctx->ssa, block, native_source(ctx, var));
        if (value)
            native_require(vir_store(&ctx->graph, block, root->address, value),
                           "addressed temporary initialization");
    }
    return root->address;
}
static vir_value_t *native_predeclare(native_function_t *ctx,
                                      basic_block_t *cursor,
                                      var_t *var)
{
    if (cursor == ctx->source->bbs) {
        vir_type_t type = native_source_info(ctx, var)->type;
        vir_value_t *undefined =
            type == VIR_TYPE_PTR
                ? vir_const_ptr(&ctx->graph, cursor->vir, 0)
                : vir_const_int(&ctx->graph, cursor->vir, type, 0);
        native_require(
            undefined && vir_ssa_write(ctx->ssa, cursor->vir,
                                       native_source(ctx, var), undefined),
            "undefined local value");
        return undefined;
    }
    vir_value_t *value =
        vir_ssa_predeclare(ctx->ssa, cursor->vir, native_source(ctx, var),
                           native_source_info(ctx, var)->type);
    native_param_t *param = arena_alloc(GENERAL_ARENA, sizeof(*param));
    native_require(value != NULL, "source parameter");
    param->cursor = cursor;
    param->object = var;
    param->value = value;
    param->next = ctx->params;
    ctx->params = param;
    return value;
}
static vir_value_t *native_read(native_function_t *ctx,
                                basic_block_t *cursor,
                                var_t *var)
{
    vir_block_t *block = native_block(ctx, cursor)->cursor->vir;
    vir_value_t *value;
    unsigned int id;
    if (!var)
        return NULL;
    if (var->is_compound_literal &&
        (var->array_size || var->has_unsized_array ||
         (!var->ptr_level && is_record_type(var->type))))
        return native_address(ctx, block, var);
    if (native_array_parameter(ctx, var) && !native_memory(ctx, var)) {
        id = native_source(ctx, var);
        value = vir_ssa_read(ctx->ssa, block, id);
        return value ? value : native_predeclare(ctx, cursor, var);
    }
    if (native_temporary(ctx, var)) {
        id = native_source(ctx, var);
        value = vir_ssa_read(ctx->ssa, block, id);
        return value ? value : native_predeclare(ctx, cursor, var);
    }
    if (((var->array_size || var->has_unsized_array) &&
         !native_array_parameter(ctx, var)) ||
        (!var->ptr_level && is_record_type(var->type) &&
         !native_array_parameter(ctx, var)))
        return native_address(ctx, block, var);
    if (var->is_function_designator)
        return vir_function_addr(&ctx->graph, block, var->var_name);
    if (native_memory(ctx, var) || native_object(var) ||
        native_source_info(ctx, var)->is_object) {
        vir_value_t *address = native_address(ctx, block, var);
        vir_type_t value_type = var->var_name[0] == '.'
                                    ? native_source_info(ctx, var)->type
                                    : native_type(var);
        vir_type_t type = value_type == VIR_TYPE_I1 ? VIR_TYPE_I8 : value_type;
        value = native_storage_volatile(ctx, var)
                    ? vir_volatile_load(&ctx->graph, block, address, type)
                    : vir_load(&ctx->graph, block, address, type);
        if (!native_memory(ctx, var)) {
            native_load_t *load = arena_alloc(GENERAL_ARENA, sizeof(*load));
            id = native_source(ctx, var);
            load->ssa_value = vir_ssa_read(ctx->ssa, block, id);
            if (!load->ssa_value)
                load->ssa_value = native_predeclare(ctx, cursor, var);
            load->ssa_value = native_convert(ctx, block, load->ssa_value,
                                             value->type, native_unsigned(var));
            load->object = var;
            load->effect = block->last_effect;
            load->next = ctx->loads;
            ctx->loads = load;
        }
        return native_convert(ctx, block, value, value_type, true);
    }
    id = native_source(ctx, var);
    value = vir_ssa_read(ctx->ssa, block, id);
    if (!value)
        value = native_predeclare(ctx, cursor, var);
    native_require(value != NULL, "source value");
    return native_type(var) == VIR_TYPE_PTR && value->type != VIR_TYPE_PTR
               ? native_convert(ctx, block, value, VIR_TYPE_PTR,
                                native_unsigned(var))
               : value;
}
static void native_write(native_function_t *ctx,
                         basic_block_t *cursor,
                         var_t *var,
                         vir_value_t *value)
{
    vir_block_t *block = native_block(ctx, cursor)->cursor->vir;
    native_require(var && value, "destination value");
    if (var->var_name[0] == '.' && !var->is_global) {
        native_source_t *source = native_source_info(ctx, var);
        if (value->type == VIR_TYPE_PTR || source->type == VIR_TYPE_PTR)
            source->type = VIR_TYPE_PTR;
        else if (value->type == VIR_TYPE_I64 || source->type == VIR_TYPE_I64)
            source->type = VIR_TYPE_I64;
        value = native_convert(ctx, block, value, source->type,
                               native_unsigned(var));
    } else
        value = native_convert(ctx, block, value, native_type(var),
                               native_unsigned(var));
    native_require(value != NULL, "destination conversion");
    if (native_temporary(ctx, var)) {
        native_require(
            vir_ssa_write(ctx->ssa, block, native_source(ctx, var), value),
            "temporary binding");
        return;
    }
    if (native_source_info(ctx, var)->is_parameter || var->is_aggregate_param ||
        (!var->is_global && !var->array_size && !var->has_unsized_array &&
         (var->ptr_level || !is_record_type(var->type))))
        native_require(
            vir_ssa_write(ctx->ssa, block, native_source(ctx, var), value),
            "source binding");
    if (!var->is_aggregate_param &&
        (native_memory(ctx, var) || native_object(var) ||
         native_source_info(ctx, var)->is_parameter ||
         native_source_info(ctx, var)->is_object)) {
        vir_value_t *address = native_address(ctx, block, var);
        vir_type_t stored_type = var->var_name[0] == '.'
                                     ? native_source_info(ctx, var)->type
                                     : native_type(var);
        vir_value_t *stored = native_convert(
            ctx, block, value,
            stored_type == VIR_TYPE_I1 ? VIR_TYPE_I8 : stored_type,
            native_unsigned(var));
        vir_effect_t *effect =
            native_storage_volatile(ctx, var)
                ? vir_volatile_store(&ctx->graph, block, address, stored)
                : vir_store(&ctx->graph, block, address, stored);
        native_require(effect != NULL, "object assignment");
        if (!native_memory(ctx, var)) {
            native_store_t *store = arena_alloc(GENERAL_ARENA, sizeof(*store));
            store->object = var;
            store->effect = effect;
            store->next = ctx->stores;
            ctx->stores = store;
        }
    }
}
static vir_call_abi_type_t native_abi(const var_t *var)
{
    vir_call_abi_type_t type = {native_type(var), native_unsigned(var),
                                var && var->type && var->type->is_bool};
    if (type.is_bool) {
        type.type = VIR_TYPE_I8;
        type.is_unsigned = false;
    }
    if (type.type == VIR_TYPE_PTR)
        type.is_unsigned = false;
    return type;
}
static void native_flush_call(native_function_t *ctx, var_t *result)
{
    func_t *signature;
    vir_call_signature_t abi = {0};
    abi.va_start_slot = (unsigned int) -1;
    vir_call_abi_type_t *params;
    vir_effect_t *effect;
    vir_type_t type;
    if (!ctx->callee && !ctx->target)
        return;
    signature = ctx->callee ? find_func((char *) ctx->callee)
                            : get_func_signature(ctx->callee_source);
    type = result ? native_type(result) : VIR_TYPE_VOID;
    if (signature && !signature->returns_aggregate)
        type = native_type(&signature->return_def);
    params = arena_alloc(GENERAL_ARENA, sizeof(*params) * (ctx->arg_count + 1));
    for (int i = 0; i < ctx->arg_count; i++) {
        params[i] = native_abi(ctx->arg_sources[i]);
        params[i].is_bool = ctx->args[i]->type == VIR_TYPE_I1;
        params[i].type = params[i].is_bool ? VIR_TYPE_I8 : ctx->args[i]->type;
        if (params[i].is_bool || params[i].type == VIR_TYPE_PTR)
            params[i].is_unsigned = false;
    }
    abi.params = params;
    abi.param_count = ctx->arg_count;
    abi.fixed_param_count =
        signature ? signature->num_params + signature->returns_aggregate
                  : ctx->arg_count;
    abi.is_variadic = signature && signature->va_args;
    abi.result = signature && !signature->returns_aggregate
                     ? native_abi(&signature->return_def)
                     : (vir_call_abi_type_t) {type, false, false};
    if (signature && signature->returns_aggregate)
        abi.result.type = type = VIR_TYPE_VOID;
    if (ctx->target)
        ctx->target = native_convert(ctx, ctx->call_block->vir, ctx->target,
                                     VIR_TYPE_PTR, true);
    effect =
        ctx->target
            ? vir_call_indirect(&ctx->graph, ctx->call_block->vir, ctx->target,
                                ctx->args, ctx->arg_count, type)
            : vir_call(&ctx->graph, ctx->call_block->vir, ctx->callee,
                       ctx->args, ctx->arg_count, type);
    bool attached = effect && vir_call_set_signature(&ctx->graph, effect, &abi);
    native_require(attached, "call effect");
    if (result && effect->result)
        native_write(ctx, ctx->call_block, result, effect->result);
    ctx->callee = NULL;
    ctx->target = NULL;
    ctx->arg_count = 0;
}
static vir_value_t *native_binary(native_function_t *ctx,
                                  vir_block_t *block,
                                  opcode_t opcode,
                                  var_t *result,
                                  var_t *left_var,
                                  var_t *right_var,
                                  vir_value_t *left,
                                  vir_value_t *right)
{
    vir_function_t *graph = &ctx->graph;
    vir_type_t type = native_type(result);
    bool shift = opcode == OP_lshift || opcode == OP_rshift;
    if (result->var_name[0] == '.' && type == VIR_TYPE_I32 &&
        (left->type == VIR_TYPE_I64 || (!shift && right->type == VIR_TYPE_I64)))
        type = VIR_TYPE_I64;
    if (opcode == OP_add && right->type == VIR_TYPE_PTR &&
        left->type != VIR_TYPE_PTR) {
        vir_value_t *swap = left;
        left = right;
        right = swap;
        var_t *source_swap = left_var;
        left_var = right_var;
        right_var = source_swap;
    }
    bool is_unsigned =
        shift ? native_unsigned(left_var)
              : integer_common_type(left_var, right_var)->is_unsigned;
    if (type == VIR_TYPE_PTR && left->type != VIR_TYPE_PTR &&
        (opcode == OP_add || opcode == OP_sub))
        left = native_convert(ctx, block, left, VIR_TYPE_PTR, true);
    if ((opcode == OP_add || opcode == OP_sub) && left->type == VIR_TYPE_PTR) {
        if (right->type == VIR_TYPE_PTR) {
            vir_type_t word = PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32;
            left = vir_ptrtoint(graph, block, left, word);
            right = vir_ptrtoint(graph, block, right, word);
            return vir_sub(graph, block, left, right);
        }
        right = native_convert(ctx, block, right,
                               PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32,
                               native_unsigned(right_var));
        if (opcode == OP_sub)
            right = vir_neg(graph, block, right);
        return vir_ptradd(graph, block, left, right);
    }
    if (op_is_comparison(opcode)) {
        type = left->type == VIR_TYPE_PTR || right->type == VIR_TYPE_PTR
                   ? VIR_TYPE_PTR
                   : left->type;
        if (type != VIR_TYPE_PTR &&
            vir_integer_type_width(right->type) > vir_integer_type_width(type))
            type = right->type;
        left =
            native_convert(ctx, block, left, type, native_unsigned(left_var));
        right =
            native_convert(ctx, block, right, type, native_unsigned(right_var));
        if (opcode == OP_eq)
            return vir_eq(graph, block, left, right);
        if (opcode == OP_neq) {
            vir_value_t *equal = vir_eq(graph, block, left, right);
            return native_not(graph, block, equal);
        }
        if (type == VIR_TYPE_PTR) {
            vir_type_t word = PTR_SIZE == 8 ? VIR_TYPE_I64 : VIR_TYPE_I32;
            left = vir_ptrtoint(graph, block, left, word);
            right = vir_ptrtoint(graph, block, right, word);
            is_unsigned = true;
        }
        if (opcode == OP_gt || opcode == OP_leq) {
            vir_value_t *swap = left;
            left = right;
            right = swap;
        }
        vir_value_t *value = is_unsigned ? vir_ult(graph, block, left, right)
                                         : vir_slt(graph, block, left, right);
        if (opcode == OP_leq || opcode == OP_geq)
            value = native_not(graph, block, value);
        return value;
    }
    left = native_convert(ctx, block, left, type, native_unsigned(left_var));
    right = native_convert(ctx, block, right, shift ? VIR_TYPE_I32 : type,
                           native_unsigned(right_var));
    return native_integer_binary(graph, block, opcode, left, right,
                                 is_unsigned);
}
void vir_frontend_note_insn(basic_block_t *cursor,
                            opcode_t opcode,
                            var_t *rd,
                            var_t *rs1,
                            var_t *rs2,
                            int size,
                            const char *symbol)
{
    native_function_t *ctx;
    native_block_t *node;
    vir_block_t *block;
    vir_value_t *left, *right, *value = NULL;
    if (!cursor || !cursor->belong_to || unevaluated_expression_depth)
        return;
    ctx = native_context(cursor->belong_to);
    node = native_block(ctx, cursor);
    block = cursor->vir;
    if (opcode != OP_func_ret && opcode != OP_allocat)
        native_flush_call(ctx, NULL);
    if (rd && native_type(rd) == VIR_TYPE_VOID && opcode != OP_func_ret)
        return;
    switch (opcode) {
    case OP_va_start:
        if (!ctx->va_start) {
            ctx->signature.va_start_slot = ctx->root_count++;
            ctx->va_start = vir_stack_addr(&ctx->graph, ctx->source->bbs->vir,
                                           ctx->signature.va_start_slot,
                                           PTR_SIZE, PTR_SIZE);
        }
        value = ctx->va_start;
        break;
    case OP_allocat:
        native_source_info(ctx, rd)->is_object = true;
        if (native_memory(ctx, rd))
            native_root(ctx, rd);
        return;
    case OP_load_constant:
        value =
            native_type(rd) == VIR_TYPE_PTR
                ? vir_const_ptr(
                      &ctx->graph, block,
                      (unsigned int) rd->init_val |
                          ((unsigned long long) (unsigned int) rd->init_val_hi
                           << 32))
                : vir_const_int(
                      &ctx->graph, block, native_type(rd),
                      (unsigned int) rd->init_val |
                          ((unsigned long long) (unsigned int) rd->init_val_hi
                           << 32));
        break;
    case OP_load_rodata_address:
        value = vir_rodata_addr(&ctx->graph, block, rd->init_val);
        break;
    case OP_address_of:
    case OP_global_address_of:
        value = native_address(ctx, block, rs1);
        break;
    case OP_assign:
        value = native_read(ctx, cursor, rs1);
        break;
    case OP_read: {
        vir_type_t load_type = native_type(rd) == VIR_TYPE_PTR
                                   ? VIR_TYPE_PTR
                                   : vir_integer_type_from_size(size);
        left = native_convert(ctx, block, native_read(ctx, cursor, rs1),
                              VIR_TYPE_PTR, true);
        value = (var_volatile_pointee(rs1) || rs1->is_volatile_access) &&
                        !rd->is_assignment_reload
                    ? vir_volatile_load(&ctx->graph, block, left, load_type)
                    : vir_load(&ctx->graph, block, left, load_type);
        break;
    }
    case OP_write:
        left = native_convert(ctx, block, native_read(ctx, cursor, rs1),
                              VIR_TYPE_PTR, true);
        right = native_read(ctx, cursor, rs2);
        right = native_convert(ctx, block, right,
                               right->type == VIR_TYPE_PTR && size == PTR_SIZE
                                   ? VIR_TYPE_PTR
                                   : vir_integer_type_from_size(size),
                               native_unsigned(rs2));
        native_require(
            ((var_volatile_pointee(rs1) || rs1->is_volatile_access)
                 ? vir_volatile_store(&ctx->graph, block, left, right)
                 : vir_store(&ctx->graph, block, left, right)) != NULL,
            "memory store");
        return;
    case OP_push:
        if (ctx->arg_count >= ctx->arg_capacity) {
            ctx->args =
                arena_grow(GENERAL_ARENA, (char *) ctx->args,
                           &ctx->arg_capacity, sizeof(*ctx->args), 8, 0, NULL);
            ctx->arg_sources =
                arena_realloc(GENERAL_ARENA, (char *) ctx->arg_sources,
                              ctx->arg_count * sizeof(*ctx->arg_sources),
                              ctx->arg_capacity * sizeof(*ctx->arg_sources));
        }
        ctx->args[ctx->arg_count] = native_read(ctx, cursor, rs1);
        ctx->arg_sources[ctx->arg_count++] = rs1;
        return;
    case OP_call:
        ctx->callee = symbol;
        ctx->call_block = cursor;
        return;
    case OP_indirect:
        ctx->target = native_read(ctx, cursor, rs1);
        ctx->callee_source = rs1;
        ctx->call_block = cursor;
        return;
    case OP_func_ret:
        native_flush_call(ctx, rd);
        return;
    case OP_branch:
        node->condition = native_convert(
            ctx, block, native_read(ctx, cursor, rs1), VIR_TYPE_I1, true);
        native_require(node->condition != NULL, "branch condition");
        return;
    case OP_return:
        value = rs1 ? native_read(ctx, cursor, rs1) : NULL;
        if (value)
            value = native_convert(ctx, block, value,
                                   native_type(&ctx->source->return_def),
                                   native_unsigned(rs1));
        native_require(vir_block_set_return(&ctx->graph, block, value),
                       "return terminator");
        return;
    case OP_cast:
    case OP_trunc:
    case OP_sign_ext:
        if (native_type(rd) == VIR_TYPE_VOID)
            return;
        value = native_convert(ctx, block, native_read(ctx, cursor, rs1),
                               native_type(rd), native_unsigned(rs1));
        break;
    case OP_negate:
    case OP_bit_not:
    case OP_log_not:
        left = native_read(ctx, cursor, rs1);
        if (opcode == OP_log_not) {
            left = native_convert(ctx, block, left, VIR_TYPE_I1, true);
            value = native_not(&ctx->graph, block, left);
        } else {
            left = native_convert(ctx, block, left, native_type(rd),
                                  native_unsigned(rs1));
            value = opcode == OP_negate ? vir_neg(&ctx->graph, block, left)
                                        : vir_bitnot(&ctx->graph, block, left);
        }
        break;
    default:
        left = native_read(ctx, cursor, rs1);
        right = native_read(ctx, cursor, rs2);
        native_require(left && right, "binary operands");
        value = native_binary(ctx, block, opcode, rd, rs1, rs2, left, right);
        break;
    }
    native_require(value != NULL, "semantic operation");
    if (opcode == OP_read && rd->var_name[0] == '.' &&
        type_bytes(value->type) != type_bytes(native_type(rd))) {
        native_source_info(ctx, rd)->type = value->type;
        if (native_source_info(ctx, rd)->is_object || native_memory(ctx, rd)) {
            native_write(ctx, cursor, rd, value);
            return;
        }
        native_require(
            vir_ssa_write(ctx->ssa, block, native_source(ctx, rd), value),
            "loaded value binding");
        return;
    }
    native_write(ctx, cursor, rd, value);
}

/* Complete predecessor bindings before constructing edges. Predeclaring the
 * value before recursing breaks cycles at loop headers and arbitrary gotos.
 */
static vir_value_t *native_incoming(native_function_t *ctx,
                                    basic_block_t *cursor,
                                    var_t *var)
{
    vir_block_t *block = native_block(ctx, cursor)->cursor->vir;
    unsigned int id = native_source(ctx, var);
    vir_value_t *value = vir_ssa_read(ctx->ssa, block, id);
    if (value) {
        if (var->var_name[0] == '.' &&
            value->type != native_source_info(ctx, var)->type) {
            value = native_convert(ctx, block, value,
                                   native_source_info(ctx, var)->type,
                                   native_unsigned(var));
            native_require(value && vir_ssa_write(ctx->ssa, block, id, value),
                           "incoming temporary conversion");
        }
        return value;
    }
    value = native_predeclare(ctx, cursor, var);
    native_require(value != NULL, "incoming block argument");
    return value;
}
static void native_complete_bindings(native_function_t *ctx)
{
    for (native_param_t *param = ctx->params; param; param = param->next)
        if (param->object->var_name[0] == '.')
            param->value->type = native_source_info(ctx, param->object)->type;
    native_param_t *completed = NULL;
    while (completed != ctx->params) {
        native_param_t *batch = ctx->params;
        for (native_param_t *param = batch; param != completed;
             param = param->next) {
            basic_block_t *cursor = param->cursor;
            for (int p = 0; p < cursor->prev_idx; p++)
                if (cursor->prev[p].bb) {
                    native_incoming(ctx, cursor->prev[p].bb, param->object);
                }
        }
        completed = batch;
    }
}
static void native_finish(native_function_t *ctx)
{
    char *error = NULL;
    if (ctx->finished)
        return;
    native_flush_call(ctx, NULL);
    for (native_root_t *root = ctx->roots; root; root = root->next) {
        for (vir_block_t *block = ctx->graph.blocks; block; block = block->next)
            for (vir_value_t *value = block->head; value; value = value->next)
                if ((value->opcode == VIR_OP_STACK_ADDR &&
                     !root->object->is_global &&
                     value->address_slot == root->slot) ||
                    (value->opcode == VIR_OP_GLOBAL_ADDR &&
                     root->object->is_global &&
                     !strcmp(value->address_name, root->symbol)))
                    value->address_size =
                        native_storage_size(ctx, root->object);
    }
    for (native_source_t *source = ctx->sources; source;
         source = source->next) {
        if (!native_memory(ctx, source->object) ||
            !vir_ssa_discard_variable(ctx->ssa, source->id))
            continue;
        native_param_t **link = &ctx->params;
        while (*link) {
            if ((*link)->object == source->object)
                *link = (*link)->next;
            else
                link = &(*link)->next;
        }
    }
    native_complete_bindings(ctx);
    for (native_block_t *node = ctx->blocks; node; node = node->next) {
        basic_block_t *cursor = node->cursor;
        vir_block_t *block = cursor->vir;
        if (block->terminator != VIR_TERM_NONE)
            continue;
        if (cursor->then_ || cursor->else_) {
            native_require(cursor->then_ && cursor->else_ && node->condition,
                           "complete branch");
            bool connected =
                vir_ssa_branch(ctx->ssa, block, node->condition,
                               native_block(ctx, cursor->then_)->cursor->vir,
                               native_block(ctx, cursor->else_)->cursor->vir);
            native_require(connected, "branch arguments");
        } else if (cursor->next && cursor->next != ctx->source->exit) {
            native_require(
                vir_ssa_jump(ctx->ssa, block,
                             native_block(ctx, cursor->next)->cursor->vir) !=
                    NULL,
                "jump arguments");
        } else {
            vir_value_t *result = NULL;
            vir_type_t type = native_type(&ctx->source->return_def);
            /* Existing non-strict C permits falling off a nonvoid body. */
            if (ctx->source != GLOBAL_FUNC && type != VIR_TYPE_VOID &&
                !ctx->source->returns_aggregate)
                result = type == VIR_TYPE_PTR
                             ? vir_const_ptr(&ctx->graph, block, 0)
                             : vir_const_int(&ctx->graph, block, type, 0);
            native_require(vir_block_set_return(&ctx->graph, block, result),
                           "fallthrough return");
        }
    }
    for (native_load_t *load = ctx->loads; load; load = load->next) {
        if (native_memory(ctx, load->object))
            continue;
        vir_value_t *replacement = native_convert(
            ctx, load->effect->block, load->ssa_value,
            load->effect->result->type, native_unsigned(load->object));
        native_require(
            replacement && vir_replace_all_uses(
                               &ctx->graph, load->effect->result, replacement),
            "local load promotion");
        native_require(vir_effect_remove(&ctx->graph, load->effect),
                       "unused local load");
    }
    native_require(vir_ssa_seal_blocks(ctx->ssa, ctx->graph.blocks->next),
                   "sealed block arguments");
    for (native_store_t *store = ctx->stores; store; store = store->next)
        if (!native_memory(ctx, store->object))
            native_require(vir_effect_remove(&ctx->graph, store->effect),
                           "unused local storage");
    native_require(vir_remove_unreachable(&ctx->graph), "CFG reachability");
    if (!vir_verify(&ctx->graph, &error)) {
        fprintf(stderr, "VIR function %s: %s\n",
                ctx->source->return_def.var_name, error ? error : "invalid");
        free(error);
        fatal("Native VIR verification failed");
    }
    ctx->finished = true;
}
void vir_frontend_begin(func_t *func)
{
    native_function_t *ctx = native_context(func);
    ctx->signature.va_start_slot = (unsigned int) -1;
    vir_block_t *entry = native_block(ctx, func->bbs)->cursor->vir;
    if (func->returns_aggregate) {
        vir_value_t *value =
            vir_block_add_param(&ctx->graph, entry, VIR_TYPE_PTR);
        native_write(ctx, func->bbs, &func->sret_def, value);
    }
    for (int i = 0; i < func->num_params; i++) {
        var_t *param = &func->param_defs[i];
        native_source_info(ctx, param)->is_parameter = true;
        vir_value_t *value =
            vir_block_add_param(&ctx->graph, entry, native_type(param));
        native_write(ctx, func->bbs, param, value);
    }
    int count = func->num_params + func->returns_aggregate;
    vir_call_abi_type_t *types =
        arena_alloc(GENERAL_ARENA, sizeof(*types) * (count + 1));
    unsigned int *slots =
        arena_alloc(GENERAL_ARENA, sizeof(*slots) * (count + 1));
    int n = 0;
    if (func->returns_aggregate) {
        types[n] = (vir_call_abi_type_t) {VIR_TYPE_PTR, false, false};
        slots[n++] = native_root(ctx, &func->sret_def)->slot;
    }
    for (int i = 0; i < func->num_params; i++, n++) {
        types[n] = native_abi(&func->param_defs[i]);
        slots[n] = func->param_defs[i].is_aggregate_param
                       ? (unsigned int) -1
                       : native_root(ctx, &func->param_defs[i])->slot;
    }
    ctx->signature.result =
        func->returns_aggregate
            ? (vir_call_abi_type_t) {VIR_TYPE_VOID, false, false}
            : native_abi(&func->return_def);
    ctx->signature.params = types;
    ctx->signature.param_slots = slots;
    ctx->signature.param_count = ctx->signature.fixed_param_count = count;
    ctx->signature.is_variadic = func->va_args;
}
void vir_frontend_end(func_t *func)
{
    native_finish(native_context(func));
}
void vir_frontend_note_edge(basic_block_t *pred, basic_block_t *succ)
{
    native_function_t *ctx;
    if (!pred || !succ || !pred->belong_to || unevaluated_expression_depth)
        return;
    ctx = native_context(pred->belong_to);
    native_block(ctx, pred);
    if (succ != ctx->source->exit)
        native_block(ctx, succ);
}
void vir_frontend_finish_globals(void)
{
    if (GLOBAL_FUNC && GLOBAL_FUNC->vir_context)
        native_finish(GLOBAL_FUNC->vir_context);
}
const vir_function_t *vir_frontend_function(const func_t *func)
{
    native_function_t *ctx = func ? func->vir_context : NULL;
    return ctx ? &ctx->graph : NULL;
}
void vir_frontend_release(void)
{
    for (native_function_t *ctx = native_functions; ctx; ctx = ctx->next) {
        vir_ssa_release(ctx->ssa);
        vir_function_release(&ctx->graph);
        ctx->source->vir_context = NULL;
    }
    native_functions = NULL;
}

const vir_call_signature_t *vir_frontend_signature(const func_t *func)
{
    native_function_t *ctx = func ? func->vir_context : NULL;
    return ctx ? &ctx->signature : NULL;
}

void vir_frontend_layout_globals(void)
{
    native_function_t *ctx = GLOBAL_FUNC ? GLOBAL_FUNC->vir_context : NULL;
    if (!ctx)
        return;
    for (native_root_t *root = ctx->roots; root; root = root->next) {
        var_t *object = root->object;
        int alignment;
        if (!object->is_global || object->space_is_allocated)
            continue;
        alignment = alignment_var(object);
        GLOBAL_FUNC->stack_size =
            (GLOBAL_FUNC->stack_size + alignment - 1) & -alignment;
        object->offset = GLOBAL_FUNC->stack_size;
        GLOBAL_FUNC->stack_size += size_var(object);
        object->space_is_allocated = true;
        object->has_backing_storage = true;
    }
}
int vir_frontend_global_offset(const char *name, void *context)
{
    (void) context;
    native_function_t *ctx = GLOBAL_FUNC ? GLOBAL_FUNC->vir_context : NULL;
    if (ctx)
        for (native_root_t *root = ctx->roots; root; root = root->next)
            if (root->object->is_global && !strcmp(root->symbol, name))
                return root->object->offset;
    fprintf(stderr, "Global object has no definition: %s\n", name);
    fatal("Undefined global object");
    return -1;
}
