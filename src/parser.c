/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "../config"
#include "defs.h"
#include "globals.c"

/* C language syntactic analyzer */
int global_var_idx = 0;

/* Side effect instructions cache */
insn_t side_effect[MAX_SIDE_EFFECT];
int se_idx = 0;

/* Control flow utilities */
basic_block_t *break_bb[MAX_NESTING];
int break_exit_idx = 0;
basic_block_t *continue_bb[MAX_NESTING];
int continue_pos_idx = 0;

/* Label utilities */
label_t labels[MAX_LABELS];
int label_idx = 0;
basic_block_t *backpatch_bb[MAX_LABELS];
int backpatch_bb_idx = 0;

/* stack of the operands of 3AC */
var_t *operand_stack[MAX_OPERAND_STACK_SIZE];
int operand_stack_idx = 0;

/* sizeof parses expression operands for constraints and type information, but
 * those operands are unevaluated and must not invalidate value metadata.
 */
int unevaluated_expression_depth = 0;

/* A function prototype nested in a sizeof type-name describes only a pointer
 * pointee. It never introduces a floating value into IR or an ABI boundary.
 */
bool parsing_sizeof_function_signature = false;
static bool parsing_block_typedef_declarator = false;
static bool parsing_for_initializer_declaration = false;
static block_t *global_constant_initializer_scope = NULL;

/* Keep the current storage contract explicit while shape handling is migrated
 * to helpers. Later rank expansion changes this one limit and its consumers,
 * rather than duplicating bound ordering and products at every call site.
 */
#define MAX_FIXED_ARRAY_RANK 4

typedef struct fixed_array_shape {
    int rank;
    int bounds[MAX_FIXED_ARRAY_RANK];
} fixed_array_shape_t;

/* Forward declarations */
source_location_t *cur_token_loc(void);
source_location_t *next_token_loc(void);

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb);
static fixed_array_shape_t fixed_array_shape_from_type(const type_t *type)
{
    fixed_array_shape_t shape = {0};
    int trailing = 1;

    if (!type->array_size)
        return shape;
    if (type->array_dim2)
        trailing *= type->array_dim2;
    if (type->array_dim3)
        trailing *= type->array_dim3;
    if (type->array_dim4)
        trailing *= type->array_dim4;
    shape.bounds[shape.rank++] = type->array_size / trailing;
    if (type->array_dim2)
        shape.bounds[shape.rank++] = type->array_dim2;
    if (type->array_dim3)
        shape.bounds[shape.rank++] = type->array_dim3;
    if (type->array_dim4)
        shape.bounds[shape.rank++] = type->array_dim4;
    return shape;
}

static fixed_array_shape_t fixed_array_shape_from_var(const var_t *var)
{
    type_t shape = {0};

    shape.array_size = var->array_size;
    shape.array_dim2 = var->array_dim2;
    shape.array_dim3 = var->array_dim3;
    shape.array_dim4 = var->array_dim4;
    return fixed_array_shape_from_type(&shape);
}

static fixed_array_shape_t fixed_array_shape_from_pointee_type(
    const type_t *type)
{
    type_t shape = {0};

    shape.array_size = type->pointee_array_size;
    shape.array_dim2 = type->pointee_array_dim2;
    shape.array_dim3 = type->pointee_array_dim3;
    shape.array_dim4 = type->pointee_array_dim4;
    return fixed_array_shape_from_type(&shape);
}

static fixed_array_shape_t fixed_array_shape_from_pointee_var(const var_t *var)
{
    type_t shape = {0};

    shape.array_size = var->pointee_array_size;
    shape.array_dim2 = var->pointee_array_dim2;
    shape.array_dim3 = var->pointee_array_dim3;
    shape.array_dim4 = var->pointee_array_dim4;
    return fixed_array_shape_from_type(&shape);
}

static void fixed_array_shape_to_type(type_t *type,
                                      const fixed_array_shape_t *shape)
{
    type->array_size = shape->rank ? shape->bounds[0] : 0;
    type->array_dim2 = type->array_dim3 = type->array_dim4 = 0;
    for (int i = 1; i < shape->rank; i++) {
        type->array_size *= shape->bounds[i];
        if (i == 1)
            type->array_dim2 = shape->bounds[i];
        else if (i == 2)
            type->array_dim3 = shape->bounds[i];
        else
            type->array_dim4 = shape->bounds[i];
    }
}

static void fixed_array_shape_to_var(var_t *var,
                                     const fixed_array_shape_t *shape)
{
    var->array_size = shape->rank ? shape->bounds[0] : 0;
    var->array_dim2 = var->array_dim3 = var->array_dim4 = 0;
    for (int i = 1; i < shape->rank; i++) {
        var->array_size *= shape->bounds[i];
        if (i == 1)
            var->array_dim2 = shape->bounds[i];
        else if (i == 2)
            var->array_dim3 = shape->bounds[i];
        else
            var->array_dim4 = shape->bounds[i];
    }
}

static void fixed_array_shape_to_pointee_var(var_t *var,
                                             const fixed_array_shape_t *shape)
{
    var->pointee_array_size = shape->rank ? shape->bounds[0] : 0;
    var->pointee_array_dim2 = var->pointee_array_dim3 =
        var->pointee_array_dim4 = 0;
    for (int i = 1; i < shape->rank; i++) {
        var->pointee_array_size *= shape->bounds[i];
        if (i == 1)
            var->pointee_array_dim2 = shape->bounds[i];
        else if (i == 2)
            var->pointee_array_dim3 = shape->bounds[i];
        else
            var->pointee_array_dim4 = shape->bounds[i];
    }
}

static fixed_array_shape_t fixed_array_shape_prepend(
    const fixed_array_shape_t *outer,
    const fixed_array_shape_t *inner)
{
    fixed_array_shape_t shape = {0};

    if (outer->rank + inner->rank > MAX_FIXED_ARRAY_RANK)
        error_at("Array declarators support at most four dimensions",
                 cur_token_loc());
    for (int i = 0; i < outer->rank; i++)
        shape.bounds[shape.rank++] = outer->bounds[i];
    for (int i = 0; i < inner->rank; i++)
        shape.bounds[shape.rank++] = inner->bounds[i];
    return shape;
}

static int fixed_array_shape_stride(const fixed_array_shape_t *shape,
                                    int subscript_depth,
                                    int element_size);

static void compose_block_typedef_array(type_t *alias,
                                        type_t *base,
                                        var_t *decl)
{
    fixed_array_shape_t base_shape = fixed_array_shape_from_type(base);
    fixed_array_shape_t decl_shape = fixed_array_shape_from_var(decl);
    fixed_array_shape_t shape;

    shape = fixed_array_shape_prepend(&decl_shape, &base_shape);
    fixed_array_shape_to_type(alias, &shape);
    alias->array_element_ptr_level = decl->ptr_level;
    alias->array_element_type = base;
}

basic_block_t *handle_block_typedef_statement(block_t *parent,
                                              basic_block_t *bb);
void perform_side_effect(block_t *parent, basic_block_t *bb);
bool read_assignment_expression(block_t *parent, basic_block_t **bb);
bool is_null_pointer_constant(var_t *value);
void read_control_expression(block_t *parent, basic_block_t **bb);
basic_block_t *read_full_expression_statement(block_t *parent,
                                              basic_block_t *bb);
void read_expr_operand(block_t *parent, basic_block_t **bb);
bool accept_compound_assign_op(opcode_t *op);
bool is_pointer_operation(opcode_t op, var_t *rs1, var_t *rs2);
void handle_pointer_arithmetic(block_t *parent,
                               basic_block_t **bb,
                               opcode_t op,
                               var_t *rs1,
                               var_t *rs2);
type_t *integer_binary_result_type(opcode_t op,
                                   const var_t *left,
                                   const var_t *right);
void normalize_integer_binary_operands(block_t *parent,
                                       basic_block_t **bb,
                                       opcode_t op,
                                       var_t **left,
                                       var_t **right);
void read_inner_var_decl(var_t *vd,
                         bool anon,
                         bool is_param,
                         bool is_record_member);
void read_partial_var_decl(var_t *vd, var_t *template);
void parse_array_init(var_t *var,
                      block_t *parent,
                      basic_block_t **bb,
                      bool emit_code);
void parse_global_record_init(var_t *var, block_t *block);
void parse_global_compound_record_init(var_t *var, block_t *block);
void parse_global_compound_scalar_init(var_t *var, block_t *block);
void parse_global_compound_array_init(var_t *var, block_t *block);
bool read_global_assignment_var(var_t *var);
void initialize_struct_field(var_t *nv, var_t *v, int offset);
bool is_array_literal_placeholder(const var_t *var);
bool is_incomplete_record_object(const var_t *var);
void copy_call_result_array_shape(var_t *result, const var_t *return_def);
void lower_call_result_array_postfix(var_t **value,
                                     block_t *parent,
                                     basic_block_t **bb);
void lower_call_result_prefix_update(var_t **value,
                                     opcode_t op,
                                     block_t *parent,
                                     basic_block_t **bb);

/* Pointer declarators can be carried by a typedef's type object rather than the
 * variable's direct ptr_level. Scalarization decisions need that full effective
 * depth, not only the spelling at the use site.
 */
bool has_effective_pointer(const var_t *var)
{
    return var && (var->ptr_level || (var->type && var->type->ptr_level));
}

/* Float, double, and long double are reserved C99 type spellings. Their
 * semantic representation is staged, but every grammar entry point must
 * recognize the complete spelling before integer-specifier parsing consumes its
 * leading `long`.
 */
bool floating_type_starts_here(void)
{
    return lex_peek(T_float, NULL) || lex_peek(T_double, NULL) ||
           (lex_peek(T_long, NULL) && cur_token->next->next &&
            cur_token->next->next->kind == T_double);
}

bool function_signature_has_floating(const func_t *signature)
{
    if (!signature)
        return false;
    if (signature->return_def.type && signature->return_def.type->is_floating)
        return true;
    for (int i = 0; i < signature->num_params; i++) {
        const var_t *param = &signature->param_defs[i];

        if ((param->type && param->type->is_floating) ||
            function_signature_has_floating(param->func_signature))
            return true;
    }
    return false;
}

bool global_compound_literal_starts_here(void)
{
    token_t *next;

    if (!lex_peek(T_open_bracket, NULL))
        return false;
    next = cur_token->next->next;
    return next &&
           ((next->kind == T_identifier && find_type(next->literal, true)) ||
            next->kind == T_struct || next->kind == T_union);
}

/* A grouped function designator remains a C99 address constant in a global
 * initializer. Callers may have consumed an optional leading `&` already, or
 * ask this predicate to recognize it as part of the spelling.
 */
bool grouped_global_function_designator_starts_here(bool allow_address)
{
    token_t *token = cur_token->next;

    if (allow_address && token && token->kind == T_ampersand)
        token = token->next;
    return token && token->kind == T_open_bracket && token->next &&
           token->next->kind == T_identifier && token->next->next &&
           token->next->next->kind == T_close_bracket &&
           find_func(token->next->literal);
}

/* `&*function` and `*&function` are both the original function designator.
 * Preserve balanced grouping around either that pair or its function operand,
 * while recognizing only declared functions so object expressions with the same
 * unary spelling follow their ordinary initializer rules.
 */
bool global_function_designator_tokens(token_t *token,
                                       token_t **after,
                                       token_t **identifier)
{
    token_t *inner_after;

    if (!token)
        return false;
    if (token->kind == T_identifier && find_func(token->literal)) {
        *after = token->next;
        *identifier = token;
        return true;
    }
    if (token->kind != T_open_bracket ||
        !global_function_designator_tokens(token->next, &inner_after,
                                           identifier) ||
        !inner_after || inner_after->kind != T_close_bracket)
        return false;
    *after = inner_after->next;
    return true;
}

bool global_function_address_dereference_tokens(token_t *token,
                                                token_t **after,
                                                token_t **identifier)
{
    token_t *operand_after;
    token_t *inner_after;

    if (!token)
        return false;
    if ((token->kind == T_ampersand && token->next &&
         token->next->kind == T_asterisk) ||
        (token->kind == T_asterisk && token->next &&
         token->next->kind == T_ampersand))
        return global_function_designator_tokens(token->next->next, after,
                                                 identifier);
    if ((token->kind == T_ampersand || token->kind == T_asterisk) &&
        token->next && token->next->kind == T_open_bracket &&
        token->next->next &&
        token->next->next->kind ==
            (token->kind == T_ampersand ? T_asterisk : T_ampersand) &&
        global_function_designator_tokens(token->next->next->next, &inner_after,
                                          identifier) &&
        inner_after && inner_after->kind == T_close_bracket) {
        *after = inner_after->next;
        return true;
    }
    if (token->kind != T_open_bracket ||
        !global_function_address_dereference_tokens(token->next, &inner_after,
                                                    identifier) ||
        !inner_after || inner_after->kind != T_close_bracket)
        return false;
    operand_after = inner_after->next;
    *after = operand_after;
    return true;
}

bool global_function_address_dereference_starts_here(void)
{
    token_t *after;
    token_t *identifier;

    return global_function_address_dereference_tokens(cur_token->next, &after,
                                                      &identifier);
}

token_t *consume_global_function_address_dereference(void)
{
    token_t *after;
    token_t *identifier;

    if (!global_function_address_dereference_tokens(cur_token->next, &after,
                                                    &identifier))
        fatal("expected grouped function address dereference");
    while (cur_token->next != after)
        lex_next();
    return identifier;
}

label_t *find_label(const char *name)
{
    for (int i = 0; i < label_idx; i++) {
        if (!strcmp(name, labels[i].label_name))
            return &labels[i];
    }
    return NULL;
}

void add_label(const char *name, basic_block_t *bb)
{
    if (label_idx > MAX_LABELS - 1)
        error_at("Too many labels in function", cur_token_loc());

    label_t *l = &labels[label_idx++];
    strncpy(l->label_name, name, MAX_ID_LEN);
    l->bb = bb;
}

/* Name for a compiler-generated temporary, interned so that the var_t only has
 * to hold a pointer to it.
 *
 * This is the parser's most frequent call by a wide margin -- one per temporary
 * value -- and sprintf() spends most of a call parsing a format string that
 * never changes. Writing the fixed prefix and the decimal digits directly
 * produces the same name for a fraction of the work.
 */
char *gen_name(void)
{
    char buf[MAX_ID_LEN], digits[16];
    int val = global_var_idx++;
    int len = 0, i = 2;

    buf[0] = '.';
    buf[1] = 't';

    if (!val)
        digits[len++] = '0';
    while (val) {
        digits[len++] = '0' + val % 10;
        val /= 10;
    }
    while (len)
        buf[i++] = digits[--len];
    buf[i] = 0;
    return arena_strdup(GENERAL_ARENA, buf);
}

var_t *require_var(block_t *blk)
{
    var_list_t *var_list = &blk->locals;

    if (var_list->size >= var_list->capacity) {
        int old_cap = var_list->capacity;
        var_list->capacity <<= 1;
        var_list->elements = arena_realloc(
            BLOCK_ARENA, (char *) var_list->elements, old_cap * sizeof(var_t *),
            var_list->capacity * sizeof(var_t *));
    }

    var_t *var = arena_calloc(BLOCK_ARENA, 1, sizeof(var_t));
    var_list->elements[var_list->size++] = var;

    /* var_name is a pointer now; every reader dereferences it unconditionally,
     * so an unnamed variable points at the empty string rather than NULL.
     */
    var->var_name = "";
    var->consumed = -1;
    var->phys_reg = -1;
    var->phys_reg_hi = -1;
    var->first_use = -1;
    var->last_use = -1;
    var->use_count = 0;
    var->base = var;
    var->type = TY_int;
    var->scope = blk;
    var->space_is_allocated = false;
    var->has_backing_storage = false;
    var->ofs_based_on_stack_top = false;
    return var;
}

var_t *require_typed_var(block_t *blk, type_t *type)
{
    if (!type)
        error_at("Type must not be NULL", cur_token_loc());

    var_t *var = require_var(blk);
    var->type = type;
    return var;
}

/* Function-address operands carry a function name, but are not declarations in
 * the current scope. Keeping them out of the local lookup list lets find_var()
 * distinguish a resolved function-pointer variable from a generated function
 * symbol.
 */
var_t *require_func_symbol_var(block_t *blk)
{
    var_t *var = require_var(blk);
    blk->locals.size--;
    return var;
}

var_t *require_typed_ptr_var(block_t *blk, type_t *type, int ptr)
{
    var_t *var = require_typed_var(blk, type);
    var->ptr_level = ptr;
    return var;
}

type_t *pointee_type_from_pointer_typedef(type_t *type);

var_t *require_ref_var(block_t *blk, type_t *type, int ptr)
{
    if (!type)
        error_at("Cannot reference variable from NULL type", cur_token_loc());

    var_t *var = require_typed_var(blk, type);
    var->ptr_level = ptr + 1;
    return var;
}

var_t *require_deref_var(block_t *blk, type_t *type, int ptr)
{
    if (!type)
        error_at("Cannot dereference variable from NULL type", cur_token_loc());

    int effective_ptr = ptr + type->ptr_level;

    /* Allowing integer dereferencing */
    if (!effective_ptr && type->base_type != TYPE_struct &&
        type->base_type != TYPE_typedef)
        return require_var(blk);

    if (!effective_ptr)
        error_at("Cannot dereference from non-pointer typed variable",
                 cur_token_loc());

    var_t *var =
        require_typed_var(blk, pointee_type_from_pointer_typedef(type));
    var->ptr_level = effective_ptr - 1;
    return var;
}

/* A typedef keeps its stars (and their qualifiers) on type_t while ordinary
 * declarators keep them on var_t. Expression results are ordinary vars, so
 * dereferencing a typedef pointer must move the surviving qualifier bits into
 * that representation instead of silently losing them.
 */
int effective_pointer_depth(const var_t *var)
{
    return var && var->type ? var->ptr_level + var->type->ptr_level : 0;
}

/* A typedef can conceal an incomplete record tag. Object declarations must
 * still reject that type by value, while pointers to it remain valid.
 */
bool is_incomplete_record_object(const var_t *var)
{
    const type_t *type;

    if (!var || effective_pointer_depth(var))
        return false;
    type = var->type;
    if (type && type->base_type == TYPE_typedef && type->base_struct)
        type = type->base_struct;
    return type &&
           (type->base_type == TYPE_struct || type->base_type == TYPE_union) &&
           !type->size;
}

unsigned int effective_pointer_const_mask(const var_t *var)
{
    if (!var || !var->type)
        return 0;
    if (var->type->ptr_level >= 32)
        return var->type->pointer_const_mask;
    return var->type->pointer_const_mask |
           (var->pointer_const_mask << var->type->ptr_level);
}

unsigned int dereferenced_pointer_const_mask(const var_t *var)
{
    int depth = effective_pointer_depth(var);
    unsigned int mask = effective_pointer_const_mask(var);

    /* The outermost pointer object was consumed by the dereference. */
    if (depth > 0 && depth <= 32)
        mask &= ~(1U << (depth - 1));
    return mask;
}

/* A pointer typedef stores its pointer depth in type_t rather than var_t. After
 * indexing through it, use the underlying scalar type for the loaded value and
 * carry any remaining pointer depth in var_t. Otherwise a `typedef unsigned
 * char *P; P p; p[0]` read looks like a pointer-sized alias instead of an
 * unsigned byte to the backend.
 */
type_t *pointee_type_from_pointer_typedef(type_t *type)
{
    if (!type || !type->ptr_level)
        return type;

    if (type->base_type == TYPE_typedef && type->base_struct)
        return type->base_struct;

    switch (type->base_type) {
    case TYPE_void:
        return TY_void;
    case TYPE_char:
        return type->is_unsigned ? TY_uchar
                                 : (type->is_signed_char ? TY_schar : TY_char);
    case TYPE_short:
        return type->is_unsigned ? TY_ushort : TY_short;
    case TYPE_int:
        return type->is_unsigned ? TY_uint : TY_int;
    case TYPE_long:
        return type->is_unsigned ? TY_ulong : TY_long;
    case TYPE_long_long:
        return type->is_unsigned ? TY_ulong_long : TY_long_long;
    case TYPE_float:
        return TY_float;
    case TYPE_double:
        return TY_double;
    case TYPE_long_double:
        return TY_long_double;
    default:
        return type;
    }
}

/* Scalar typedefs have their own descriptor, but C declaration compatibility is
 * based on the represented type. Records retain nominal identity.
 */
bool compatible_function_signature(const func_t *left, const func_t *right);
func_t *find_visible_func(char *name, block_t *scope);
var_t *materialize_function_designator(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *value);
var_t *emit_direct_call_result(func_t *func,
                               bool want_value,
                               block_t *parent,
                               basic_block_t **bb);
var_t *emit_indirect_call_result(var_t *callee,
                                 func_t *signature,
                                 bool want_value,
                                 block_t *parent,
                                 basic_block_t **bb);

static int callback_pointer_indirection(const var_t *var)
{
    if (!var || !var->is_func)
        return 0;
    if (var->parenthesized_function_pointer_level)
        return var->parenthesized_function_pointer_level;

    /* A non-direct function typedef denotes the established one-pointer
     * callback object representation.
     */
    if (var->type && var->type->func_signature &&
        !var->type->is_direct_function_type)
        return 1;
    return 0;
}

bool compatible_decl_type(const type_t *left, const type_t *right)
{
    if (left == right)
        return true;

    /* A callback-pointer typedef may use a distinct descriptor at block and
     * file scope while still denoting the same pointer-to-function type.
     */
    if (left && right && left->func_signature && right->func_signature &&
        !left->is_direct_function_type && !right->is_direct_function_type)
        return left->size == right->size &&
               left->ptr_level == right->ptr_level &&
               left->pointer_const_mask == right->pointer_const_mask &&
               left->is_const_qualified == right->is_const_qualified &&
               left->is_volatile_qualified == right->is_volatile_qualified &&
               compatible_function_signature(left->func_signature,
                                             right->func_signature);
    if (!left || !right || left->base_type != right->base_type ||
        left->size != right->size || left->ptr_level != right->ptr_level ||
        left->is_unsigned != right->is_unsigned ||
        left->is_signed_char != right->is_signed_char ||
        left->is_bool != right->is_bool)
        return false;
    if (!!left->func_signature != !!right->func_signature)
        return false;
    if (left->func_signature &&
        !compatible_function_signature(left->func_signature,
                                       right->func_signature))
        return false;
    if (left->base_type == TYPE_struct || left->base_type == TYPE_union)
        return false;
    if (left->base_type == TYPE_typedef &&
        (left->num_fields || right->num_fields))
        return left->base_struct == right->base_struct;
    return true;
}

/* Function-pointer declarators carry their pointee function type separately
 * from the pointer-sized var_t representation. Redeclarations must compare that
 * syntax-only signature as well: comparing just the outer storage shape accepts
 * incompatible callbacks such as `int (*)(int)` and `int (*)(long)`. When one
 * side has no prototype, retain the existing old-style policy.
 */
bool compatible_function_param_decl(const var_t *left, const var_t *right)
{
    bool left_points_to_value;
    bool right_points_to_value;

    if (!left || !right || left->is_func != right->is_func)
        return false;

    if (left->is_func) {
        if (!left->func_signature || !right->func_signature)
            return false;
        return callback_pointer_indirection(left) ==
                   callback_pointer_indirection(right) &&
               compatible_function_signature(left->func_signature,
                                             right->func_signature);
    }

    if (left->pointee_func_signature || right->pointee_func_signature) {
        if (!left->pointee_func_signature || !right->pointee_func_signature ||
            !compatible_function_signature(left->pointee_func_signature,
                                           right->pointee_func_signature))
            return false;
    }

    if (!compatible_decl_type(left->type, right->type) ||
        left->ptr_level != right->ptr_level)
        return false;

    left_points_to_value = left->ptr_level || left->type->ptr_level;
    right_points_to_value = right->ptr_level || right->type->ptr_level;

    /* C99 ignores top-level parameter qualifiers, but qualifiers on the reached
     * object remain part of a pointer parameter's type.
     */
    return !((left_points_to_value || right_points_to_value) &&
             (left->is_const_qualified != right->is_const_qualified ||
              left->is_volatile != right->is_volatile));
}

/* C99 6.7.5.3 requires a prototype following an earlier `f()` declaration to
 * use only parameter types unchanged by the default argument promotions. Array
 * and function parameters have already adjusted to pointers here, so every
 * pointer-shaped declaration is stable.
 */
bool parameter_changes_under_default_promotion(const var_t *param)
{
    type_t *type;

    if (!param || !(type = param->type))
        return true;
    if (param->is_func || param->ptr_level || type->ptr_level ||
        param->array_size || param->has_unsized_array)
        return false;
    if (type->is_bool)
        return true;

    switch (type->base_type) {
    case TYPE_char:
    case TYPE_short:
    case TYPE_float:
        return true;
    default:
        return false;
    }
}

bool compatible_function_signature(const func_t *left, const func_t *right)
{
    if (!left || !right ||
        !compatible_decl_type(left->return_def.type, right->return_def.type) ||
        left->return_def.ptr_level != right->return_def.ptr_level ||
        left->return_def.is_const_qualified !=
            right->return_def.is_const_qualified)
        return false;

    if (!left->has_prototype || !right->has_prototype)
        return true;
    if (left->num_params != right->num_params ||
        left->va_args != right->va_args)
        return false;
    for (int i = 0; i < left->num_params; i++)
        if (!compatible_function_param_decl(&left->param_defs[i],
                                            &right->param_defs[i]))
            return false;
    return true;
}

/* Function linkage and ordinary-identifier scope are separate in C. A prototype
 * first introduced in a block stays in the translation-unit table for
 * compatibility with a later definition, but expressions may name it only while
 * its lexical function alias is visible.
 */
func_t *find_visible_func(char *name, block_t *scope)
{
    func_t *func = find_func(name);
    var_t *binding;

    if (!func || !func->is_block_scope_only_declaration)
        return func;
    binding = find_var(name, scope);
    return binding && binding->is_extern_function_alias ? func : NULL;
}

/* An inline record pointer typedef stores PTR_SIZE in type->size, while its
 * fields still describe the pointee layout. Recover that layout for indexing;
 * this differs on 32-bit targets whenever the record is wider than a pointer.
 */
int pointer_typedef_pointee_size(type_t *type, type_t *pointee)
{
    if (!type || !type->ptr_level || type->base_type != TYPE_typedef ||
        !type->num_fields)
        return pointee == TY_void ? 1 : pointee->size;

    int size = 0;
    for (int i = 0; i < type->num_fields; i++) {
        int field_size = size_var(&type->fields[i]);
        int end =
            type->is_union ? field_size : type->fields[i].offset + field_size;
        if (end > size)
            size = end;
    }
    return size;
}

/* The next free field slot of @type.
 *
 * Struct and union bodies fill these in from two places -- one field per
 * declaration, and one more per comma in a multiple declarator -- so the bound
 * belongs here rather than being repeated, and missed, at each of them.
 */
var_t *type_add_field(type_t *type, int *idx)
{
    int i = *idx;

    if (i >= MAX_FIELDS)
        error_at("Too many fields in struct or union", cur_token_loc());
    type_ensure_fields(type);
    *idx = i + 1;
    return &type->fields[i];
}

/* C99 record members start at their natural ABI alignment and a record's extent
 * is rounded up to its strongest member. The supported targets use the scalar
 * sizes below as natural alignments (up to eight bytes); pointer aliases retain
 * pointer alignment even though their base typedef may carry record field
 * metadata.
 */
int alignment_type(type_t *type)
{
    if (!type)
        return 1;
    if (type->ptr_level)
        return PTR_SIZE;
    if ((type->base_type == TYPE_struct || type->base_type == TYPE_union ||
         (type->base_type == TYPE_typedef && type->num_fields)) &&
        type->alignment)
        return type->alignment;
    if (type->size >= 8)
        return 8;
    if (type->size >= 4)
        return 4;
    if (type->size >= 2)
        return 2;
    return 1;
}

int alignment_var(var_t *var)
{
    if (var->ptr_level || var->is_func || var->type->ptr_level)
        return PTR_SIZE;
    return alignment_type(var->type);
}

int layout_struct_field(int size, var_t *field, int *record_alignment)
{
    int alignment = alignment_var(field);

    if (alignment > *record_alignment)
        *record_alignment = alignment;
    field->offset = ALIGN_UP(size, alignment);
    return field->offset + size_var(field);
}

/* Stateful packing cursor for the C99 bit-field layout path. Ordinary fields
 * flush the cursor before their existing aligned placement; bit-fields will
 * share one unit until its remaining bits are exhausted.
 */
typedef struct {
    int unit_offset;
    int unit_size;
    int used_bits;
} bitfield_layout_t;

/* Switch labels are collected only while enforcing strict C99 constraints. A
 * linked list avoids imposing an arbitrary case-label count on a valid
 * translation unit merely to check duplicate converted values.
 */
typedef struct switch_case_value {
    int value;
    struct switch_case_value *next;
} switch_case_value_t;

int read_const_expr(block_t *scope);
int read_global_address_offset(block_t *scope,
                               block_t *parent,
                               basic_block_t *bb);

/* Enumerator values must be representable as int. */
bool checking_enum_constant = false;
bool typed_global_literal_appears_before_initializer_end(token_t *token);
void read_literal_param(block_t *parent, basic_block_t *bb);
var_t *bitfield_constant(block_t *parent, basic_block_t **bb, unsigned value);
unsigned bitfield_mask(const var_t *field);
void write_bitfield_value(block_t *parent,
                          basic_block_t **bb,
                          var_t *address,
                          var_t *value,
                          const var_t *field);

bool is_bitfield(const var_t *field)
{
    return field && field->is_bitfield;
}

bool is_bool_type(const type_t *type)
{
    return type && type->is_bool;
}

/* Empty initializer lists are a useful permissive-mode extension, but C99's
 * initializer-list grammar requires at least one initializer. Call this
 * immediately after consuming an opening initializer brace.
 */
void reject_empty_initializer_in_strict_c99(void)
{
    if (strict_c99 && lex_peek(T_close_curly, NULL))
        error_at("empty initializer list is not permitted in C99",
                 cur_token_loc());
}

/* shecc documents conventional allocation units for C99's required bit-field
 * types: four-byte int/unsigned int and one-byte _Bool, packed
 * least-significant bit first.
 */
void read_bitfield_width(var_t *field, block_t *scope)
{
    int width;

    if (!lex_accept(T_colon))
        return;
    if (field->ptr_level || field->is_func || field->array_size ||
        field->has_unsized_array ||
        !(is_bool_type(field->type) ||
          (field->type->base_type == TYPE_int && field->type->size == 4)))
        error_at("Bit-field must have type _Bool, int, or unsigned int",
                 cur_token_loc());

    width = read_const_expr(scope);
    int storage_size = is_bool_type(field->type) ? 1 : 4;
    int max_width = is_bool_type(field->type) ? 1 : storage_size * 8;
    if (width < 0 || width > max_width)
        error_at("Bit-field width exceeds its storage unit", cur_token_loc());
    if (!width && field->var_name[0])
        error_at("Zero-width bit-field must be unnamed", cur_token_loc());

    field->is_bitfield = true;
    field->bit_width = width;
    field->bit_offset = 0;
    field->bit_storage_size = storage_size;
}

int flush_bitfield_layout(int size, bitfield_layout_t *bits)
{
    if (bits->used_bits) {
        int end = bits->unit_offset + bits->unit_size;
        if (end > size)
            size = end;
    }
    bits->unit_offset = 0;
    bits->unit_size = 0;
    bits->used_bits = 0;
    return size;
}

int layout_bitfield_field(int size,
                          var_t *field,
                          int *record_alignment,
                          bitfield_layout_t *bits)
{
    int unit_bits = field->bit_storage_size * 8;
    int alignment = field->bit_storage_size;

    if (alignment > *record_alignment)
        *record_alignment = alignment;
    if (!field->bit_width) {
        size = flush_bitfield_layout(size, bits);
        return ALIGN_UP(size, alignment);
    }
    if (!bits->used_bits || bits->unit_size != field->bit_storage_size ||
        bits->used_bits + field->bit_width > unit_bits) {
        size = flush_bitfield_layout(size, bits);
        bits->unit_offset = ALIGN_UP(size, alignment);
        bits->unit_size = field->bit_storage_size;
    }
    field->offset = bits->unit_offset;
    field->bit_offset = bits->used_bits;
    bits->used_bits += field->bit_width;
    return size;
}

/* An incomplete array has a distinct meaning in a record: C99 permits it only
 * as the final member of a struct, where it contributes no bytes to the
 * record's fixed layout.
 */
void mark_flexible_array_member(var_t *field, bool is_union)
{
    if (!field->has_unsized_array)
        return;
    if (is_union)
        error_at("Flexible array member is not permitted in a union",
                 cur_token_loc());
    field->is_flexible_array_member = true;
}

bool is_array_declarator(const var_t *var)
{
    return var->array_size > 0 || var->is_flexible_array_member;
}

bool type_has_flexible_array_member(const type_t *type)
{
    return type && (type->has_flexible_array_member ||
                    (type->base_struct &&
                     type->base_struct->has_flexible_array_member));
}

bool is_flexible_array_member_container(const var_t *field)
{
    return !field->ptr_level && type_has_flexible_array_member(field->type);
}

void reject_flexible_array_member_container(const var_t *field)
{
    if (is_flexible_array_member_container(field))
        error_at(
            "A struct with a flexible array member cannot be embedded by value",
            cur_token_loc());
}

void opstack_push(var_t *var)
{
    if (operand_stack_idx >= MAX_OPERAND_STACK_SIZE)
        fatal("Expression too complex: operand stack exhausted");
    operand_stack[operand_stack_idx++] = var;
}

/* The break/continue targets form a stack indexed by loop and switch nesting.
 * Pushing through these keeps the depth check in one place instead of at each
 * of the sites that open a new nesting level.
 */
void break_bb_push(basic_block_t *bb)
{
    if (break_exit_idx >= MAX_NESTING)
        fatal("Too many nested loops or switch statements");
    break_bb[break_exit_idx++] = bb;
}

void continue_bb_push(basic_block_t *bb)
{
    if (continue_pos_idx >= MAX_NESTING)
        fatal("Too many nested loops");
    continue_bb[continue_pos_idx++] = bb;
}

var_t *opstack_pop(void)
{
    return operand_stack[--operand_stack_idx];
}

/* Declarators with global storage are made available to the constant
 * initializer parser through operand_stack. Scalar initialization consumes that
 * entry itself, while zero and aggregate initialization do not.
 */
void discard_global_declarator_operand(var_t *var)
{
    if (operand_stack_idx && operand_stack[operand_stack_idx - 1] == var)
        opstack_pop();
}

void read_expr(block_t *parent, basic_block_t **bb);

int write_symbol(const char *data)
{
    /* Write string literals to .rodata section */
    const int start_len = elf_rodata->size;
    elf_write_str(elf_rodata, data);
    elf_write_byte(elf_rodata, 0);
    return start_len;
}

int write_wide_symbol(const int *data, int length)
{
    int start_len = elf_rodata->size;

    for (int i = 0; i < length; i++)
        elf_write_int(elf_rodata, data[i]);
    elf_write_int(elf_rodata, 0);
    return start_len;
}

/* The narrow decoder translates UCNs to UTF-8, which is correct for char
 * strings but not for a wide literal: one UCN is one wchar_t element. This
 * execution-wide-character policy stores each source byte/escape value as an
 * int unit and preserves UCN scalar values directly.
 */
int decode_wstring_units(const char *text, int *units, int capacity)
{
    int in = 0;
    int out = 0;

    while (text[in]) {
        unsigned int value;

        if (out >= capacity)
            return -1;
        if (text[in] != '\\') {
            units[out++] = (unsigned char) text[in++];
            continue;
        }
        in++;
        switch (text[in]) {
        case 'a':
            value = '\a';
            in++;
            break;
        case 'b':
            value = '\b';
            in++;
            break;
        case 'f':
            value = '\f';
            in++;
            break;
        case 'n':
            value = '\n';
            in++;
            break;
        case 'r':
            value = '\r';
            in++;
            break;
        case 't':
            value = '\t';
            in++;
            break;
        case 'v':
            value = '\v';
            in++;
            break;
        case '\\':
            value = '\\';
            in++;
            break;
        case '\'':
            value = '\'';
            in++;
            break;
        case '"':
            value = '"';
            in++;
            break;
        case '?':
            value = '?';
            in++;
            break;
        case 'e':
            if (strict_c99)
                return -1;
            value = 27;
            in++;
            break;
        case 'x':
            in++;
            if (!isxdigit(text[in]))
                return -1;
            value = 0;
            while (isxdigit(text[in])) {
                if (value > 0x07ffffffU)
                    return -1;
                value = (value << 4) + hex_digit_value(text[in++]);
            }
            if (value > 0x7fffffffU)
                return -1;
            break;
        case 'u':
        case 'U': {
            int digits = text[in] == 'u' ? 4 : 8;

            value = 0;
            in++;
            for (int i = 0; i < digits; i++) {
                if (!isxdigit(text[in]))
                    return -1;
                if (value > 0x07ffffffU)
                    return -1;
                value = (value << 4) + hex_digit_value(text[in++]);
            }
            if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
                (value < 0xa0 && value != '$' && value != '@' && value != '`'))
                return -1;
            break;
        }
        default:
            if (text[in] < '0' || text[in] > '7')
                value = (unsigned char) text[in++];
            else {
                value = 0;
                for (int i = 0; i < 3 && text[in] >= '0' && text[in] <= '7';
                     i++)
                    value = value * 8 + (text[in++] - '0');
            }
            break;
        }
        units[out++] = (int) value;
    }
    return out;
}

/* A single wide character constant denotes one execution-wide unit. Keep the
 * existing implementation-defined packing for multi-character constants, but do
 * not route a UCN such as L'\u00e9' through the narrow UTF-8 decoder.
 */
int parse_wide_character_constant(const char *literal)
{
    int units[MAX_TOKEN_LEN];
    int length;

    bool has_ucn = false;
    for (int i = 0; literal[i]; i++)
        if (literal[i] == '\\' &&
            (literal[i + 1] == 'u' || literal[i + 1] == 'U')) {
            has_ucn = true;
            break;
        }
    if (!has_ucn)
        return parse_character_constant(literal);
    length = decode_wstring_units(literal, units, MAX_TOKEN_LEN);
    if (length < 0)
        error_at("Invalid wide character escape sequence", cur_token_loc());
    if (length == 1)
        return units[0];
    return parse_character_constant(literal);
}

int read_wstring_units(int *units, int capacity)
{
    char literal[MAX_STRING_LEN];
    int length = 0;

    do {
        int part_length;

        lex_ident(T_wstring, literal);
        part_length =
            decode_wstring_units(literal, units + length, capacity - length);
        if (part_length < 0)
            error_at("Invalid wide string escape sequence", cur_token_loc());
        length += part_length;
    } while (lex_peek(T_wstring, NULL));
    return length;
}

int get_size(var_t *var)
{
    if (var->ptr_level || var->is_func)
        return PTR_SIZE;
    return var->type->size;
}

int get_operator_prio(opcode_t op)
{
    /* https://www.cs.uic.edu/~i109/Notes/COperatorPrecedenceTable.pdf */
    switch (op) {
    case OP_ternary:
        return 3;
    case OP_log_or:
        return 4;
    case OP_log_and:
        return 5;
    case OP_bit_or:
        return 6;
    case OP_bit_xor:
        return 7;
    case OP_bit_and:
        return 8;
    case OP_eq:
    case OP_neq:
        return 9;
    case OP_lt:
    case OP_leq:
    case OP_gt:
    case OP_geq:
        return 10;
    case OP_lshift:
    case OP_rshift:
        return 11;
    case OP_add:
    case OP_sub:
        return 12;
    case OP_mul:
    case OP_div:
    case OP_mod:
        return 13;
    default:
        return 0;
    }
}

opcode_t get_operator(void)
{
    opcode_t op = OP_generic;
    if (lex_accept(T_plus))
        op = OP_add;
    else if (lex_accept(T_minus))
        op = OP_sub;
    else if (lex_accept(T_asterisk))
        op = OP_mul;
    else if (lex_accept(T_divide))
        op = OP_div;
    else if (lex_accept(T_mod))
        op = OP_mod;
    else if (lex_accept(T_lshift))
        op = OP_lshift;
    else if (lex_accept(T_rshift))
        op = OP_rshift;
    else if (lex_accept(T_log_and))
        op = OP_log_and;
    else if (lex_accept(T_log_or))
        op = OP_log_or;
    else if (lex_accept(T_eq))
        op = OP_eq;
    else if (lex_accept(T_noteq))
        op = OP_neq;
    else if (lex_accept(T_lt))
        op = OP_lt;
    else if (lex_accept(T_le))
        op = OP_leq;
    else if (lex_accept(T_gt))
        op = OP_gt;
    else if (lex_accept(T_ge))
        op = OP_geq;
    else if (lex_accept(T_ampersand))
        op = OP_bit_and;
    else if (lex_accept(T_bit_or))
        op = OP_bit_or;
    else if (lex_accept(T_bit_xor))
        op = OP_bit_xor;
    else if (lex_peek(T_question, NULL))
        op = OP_ternary;
    return op;
}

var_t *promote_unchecked(block_t *block,
                         basic_block_t **bb,
                         var_t *var,
                         type_t *target_type,
                         int target_ptr)
{
    var_t *rd = require_typed_ptr_var(block, target_type, target_ptr);
    rd->var_name = gen_name();

    /* Encode both source and target sizes in src1: Lower 16 bits: target size
     * Upper 16 bits: source size This allows codegen to distinguish between
     * different promotion types without changing IR semantics.
     */
    int encoded_size = ((var->type->size) << 16);
    if (target_ptr)
        encoded_size |= PTR_SIZE;
    else
        encoded_size |= target_type->size;
    add_insn(block, *bb, OP_sign_ext, rd, var, NULL, encoded_size, NULL);
    return rd;
}

var_t *promote(block_t *block,
               basic_block_t **bb,
               var_t *var,
               type_t *target_type,
               int target_ptr)
{
    /* Effectively checking whether var has size of int */
    if (var->type->size == target_type->size || var->ptr_level ||
        var->array_size)
        return var;

    if (var->type->size > TY_int->size && !var->ptr_level) {
        printf("Warning: Suspicious type promotion %s\n", var->type->type_name);
        return var;
    }

    return promote_unchecked(block, bb, var, target_type, target_ptr);
}

var_t *truncate_unchecked(block_t *block,
                          basic_block_t **bb,
                          var_t *var,
                          type_t *target_type,
                          int target_ptr)
{
    var_t *rd = require_typed_ptr_var(block, target_type, target_ptr);
    rd->var_name = gen_name();
    add_insn(block, *bb, OP_trunc, rd, var, NULL,
             target_ptr ? PTR_SIZE : target_type->size, NULL);
    return rd;
}

var_t *normalize_bool(block_t *block, basic_block_t **bb, var_t *var)
{
    var_t *zero;
    var_t *rd;

    if (var->type == TY_bool && !var->ptr_level)
        return var;
    if (var->is_const && !var->ptr_level) {
        rd = require_typed_var(block, TY_bool);
        rd->var_name = gen_name();
        rd->init_val = var->init_val != 0;
        rd->is_const = true;
        add_insn(block, *bb, OP_load_constant, rd, NULL, NULL, 0, NULL);
        return rd;
    }

    zero = require_typed_var(block, TY_int);
    zero->var_name = gen_name();
    zero->init_val = 0;
    zero->is_const = true;
    add_insn(block, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);

    rd = require_typed_var(block, TY_bool);
    rd->var_name = gen_name();
    add_insn(block, *bb, OP_neq, rd, var, zero, 0, NULL);
    return rd;
}

var_t *resize_var(block_t *block, basic_block_t **bb, var_t *from, var_t *to)
{
    bool is_from_ptr = from->ptr_level || from->array_size,
         is_to_ptr = to->ptr_level || to->array_size ||
                     (to->type && to->type->ptr_level > 0);

    if (is_from_ptr && is_to_ptr)
        return from;

    if (!is_to_ptr && to->type == TY_bool)
        return normalize_bool(block, bb, from);

    int from_size = get_size(from), to_size = get_size(to);

    if (from_size > to_size) {
        /* Truncation */
        return truncate_unchecked(block, bb, from, to->type, to->ptr_level);
    }

    if (from_size < to_size) {
        /* Widening into a pointer needs no conversion instruction. Values
         * already occupy a full register and integer loads sign-extend, so the
         * pointer's bits are the value's bits. Emitting the conversion here
         * also placed it ahead of the instructions computing its own operand,
         * which produced a garbage pointer.
         *
         * On the 32-bit targets PTR_SIZE equals an int, so this case cannot
         * arise there and behaviour is unchanged.
         */
        if (is_to_ptr)
            return from;

        /* Sign extend */
        return promote_unchecked(block, bb, from, to->type, to->ptr_level);
    }

    /* A same-rank unsigned assignment still has a required representation
     * conversion on a wider register machine. Keeping an arithmetic result in a
     * 64-bit register can leave carry bits above an unsigned int's object
     * width; a later equality comparison would then see 2^32 + 1 instead of the
     * stored value 1.
     */
    if (!is_from_ptr && !is_to_ptr && to->type && to->type->is_unsigned &&
        to_size < PTR_SIZE)
        return truncate_unchecked(block, bb, from, to->type, to->ptr_level);

    return from;
}

/* Convert @val to @type at @ptr_level levels of indirection.
 *
 * resize_var() takes its target as a var_t, and every caller builds that the
 * same way: a zeroed local carrying only those two fields. Building it here
 * keeps the zeroing in one place -- shecc miscompiles "var_t t = {0};", so it
 * has to be a memset, and nine copies of that were nine chances to leave one
 * out.
 */
var_t *resize_to(block_t *block,
                 basic_block_t **bb,
                 var_t *val,
                 type_t *type,
                 int ptr_level)
{
    var_t target;

    memset(&target, 0, sizeof(var_t));
    target.type = type;
    target.ptr_level = ptr_level;
    return resize_var(block, bb, val, &target);
}

/* C99 6.5.16.1 permits adding qualifiers at the referenced object, but the
 * familiar int ** -> const int ** conversion is unsafe: a caller could store a
 * pointer-to-const through the converted value and later write through the
 * original int *. The compiler retains base qualification and pointer depth,
 * which is enough to reject that class without modeling every level yet.
 */
bool incompatible_const_pointer_conversion(const var_t *from, const var_t *to)
{
    unsigned int from_mask, to_mask;
    int from_depth, to_depth;

    if (!from || !to)
        return false;

    from_depth = from->ptr_level + from->type->ptr_level;
    to_depth = to->ptr_level + to->type->ptr_level;
    if (!from_depth || !to_depth)
        return false;

    if (from->is_const_qualified && !to->is_const_qualified)
        return true;

    /* A top-level pointer qualifier belongs to the source or destination
     * object, not to the pointed-to type used by a value conversion. Keep every
     * inner level: `int * const *` must accept `&p` when p is an `int * const`,
     * and converting it back to `int **` must be rejected.
     */
    from_mask = from->type->pointer_const_mask |
                (from->pointer_const_mask << from->type->ptr_level);
    to_mask = to->type->pointer_const_mask |
              (to->pointer_const_mask << to->type->ptr_level);
    if (from_depth <= 32)
        from_mask &= ~(1U << (from_depth - 1));
    if (to_depth <= 32)
        to_mask &= ~(1U << (to_depth - 1));
    if (from_mask & ~to_mask)
        return true;

    return from_depth > 1 && to_depth > 1 &&
           from->is_const_qualified != to->is_const_qualified;
}

/* C still accepts assigning a string literal to char *, but treating the
 * literal as read-only catches the common accidental-write case. Keep this
 * diagnostic behind an explicit option: shecc's bundled, self-hosted sources
 * contain older char * interfaces for string data.
 */
void diagnose_const_pointer_conversion(const var_t *from, const var_t *to)
{
    if (!incompatible_const_pointer_conversion(from, to))
        return;

    if (from->is_string_literal) {
        if (warn_string_literals)
            printf("Warning: string literal is read-only\n");
        return;
    }

    error_at("discarding const qualifier", cur_token_loc());
}

/* Plain, signed, and unsigned char are distinct C types despite sharing a byte
 * representation. Preserve that identity for implicit pointer conversions
 * without changing the legacy non-character pointer extensions.
 */
bool incompatible_character_pointer_conversion(const var_t *from,
                                               const var_t *to)
{
    type_t *from_pointee, *to_pointee;

    if (!from || !to || !(from->ptr_level || from->type->ptr_level) ||
        !(to->ptr_level || to->type->ptr_level))
        return false;

    from_pointee = pointee_type_from_pointer_typedef(from->type);
    to_pointee = pointee_type_from_pointer_typedef(to->type);
    return from_pointee->base_type == TYPE_char &&
           to_pointee->base_type == TYPE_char &&
           !compatible_decl_type(from_pointee, to_pointee);
}

bool incompatible_pointee_callback_conversion(const var_t *from,
                                              const var_t *to)
{
    func_t *from_signature;
    func_t *to_signature;

    if (!from || !to)
        return false;
    if (is_null_pointer_constant((var_t *) from) ||
        (!from->pointee_func_signature && !from->func_signature &&
         !(from->ptr_level || from->type->ptr_level) && !from->is_func &&
         !from->init_val && !from->init_val_hi))
        return false;
    from_signature = from->pointee_func_signature;
    to_signature = to->pointee_func_signature;

    /* An array parameter of callbacks is adjusted to a callback slot. The
     * parser keeps its element callback signature in `func_signature` while
     * retaining the written array bound, rather than in the ordinary slot field
     * used by a spelled `(**slot)` declarator.
     */
    if (!to_signature && to->func_signature &&
        (to->array_size || to->has_unsized_array))
        to_signature = to->func_signature;
    if (!from_signature && effective_pointer_depth(from) > 0 && from->type &&
        from->type->func_signature && !from->type->is_direct_function_type)
        from_signature = from->type->func_signature;
    if (!to_signature && effective_pointer_depth(to) > 0 && to->type &&
        to->type->func_signature && !to->type->is_direct_function_type)
        to_signature = to->type->func_signature;
    if (!(from_signature || to_signature))
        return false;
    return !from_signature || !to_signature ||
           !compatible_function_signature(from_signature, to_signature);
}

void read_parameter_list_decl(func_t *func, bool anon);
void read_indirect_call(var_t *callee, block_t *parent, basic_block_t **bb);
var_t *integer_promote_operand(block_t *parent, basic_block_t **bb, var_t *var);
var_t *resolve_global_declarator(block_t *block,
                                 var_t *var,
                                 bool is_static,
                                 bool *is_redeclaration);
void read_global_function_declarator(block_t *block,
                                     var_t *var,
                                     bool is_static);
var_t *bind_block_extern_object(block_t *parent, var_t *var);
int read_const_expr(block_t *scope);

/* Forward declaration for ternary handling used by initializers */
void read_ternary_operation(block_t *parent, basic_block_t **bb);

/* Parse array initializer to determine size for implicit arrays and optionally
 * emit initialization code.
 */
var_t *compute_element_address(block_t *parent,
                               basic_block_t **bb,
                               var_t *base_addr,
                               int index,
                               int elem_size)
{
    if (index == 0)
        return base_addr;

    var_t *offset = require_var(parent);
    offset->var_name = gen_name();
    offset->init_val = index * elem_size;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    addr->var_name = gen_name();
    add_insn(parent, *bb, OP_add, addr, base_addr, offset, 0, NULL);
    return addr;
}

var_t *compute_field_address(block_t *parent,
                             basic_block_t **bb,
                             var_t *struct_addr,
                             const var_t *field)
{
    if (field->offset == 0)
        return struct_addr;

    var_t *offset = require_var(parent);
    offset->var_name = gen_name();
    offset->init_val = field->offset;
    add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0, NULL);

    var_t *addr = require_var(parent);
    addr->var_name = gen_name();
    add_insn(parent, *bb, OP_add, addr, struct_addr, offset, 0, NULL);
    return addr;
}

/* A record assignment is a value copy, not the scalar OP_assign used for
 * ordinary variables. Keep the lowering in phase 1 so every backend can use its
 * existing 1-, 2-, and 4-byte indirect accesses.
 */
bool is_record_object(const var_t *var)
{
    return var && !var->ptr_level && !var->array_size && var->type &&
           (var->type->base_type == TYPE_struct ||
            var->type->base_type == TYPE_union ||
            (var->type->base_type == TYPE_typedef &&
             var->type->num_fields > 0));
}

void emit_record_copy(block_t *parent,
                      basic_block_t **bb,
                      var_t *dest,
                      var_t *src)
{
    int size = size_var(dest);
    var_t *dest_addr = require_ref_var(parent, dest->type, 0);
    var_t *src_addr = require_ref_var(parent, src->type, 0);

    dest_addr->var_name = gen_name();
    src_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
    add_insn(parent, *bb, OP_address_of, src_addr, src, NULL, 0, NULL);

    for (int offset = 0; offset < size;) {
        int width = 1;
        if (size - offset >= 4)
            width = 4;
        else if (size - offset >= 2)
            width = 2;
        var_t *src_part =
            compute_element_address(parent, bb, src_addr, offset, 1);
        var_t *dest_part =
            compute_element_address(parent, bb, dest_addr, offset, 1);
        var_t *value = require_var(parent);

        value->var_name = gen_name();
        add_insn(parent, *bb, OP_read, value, src_part, NULL, width, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_part, value, width, NULL);
        offset += width;
    }
}

/* Copy a record into an address which is already known, such as a nested record
 * member. A compound literal is an object, not an integer value, so lowering it
 * through one OP_write with the member's aggregate size leaves narrow backends
 * with an impossible 8-byte store.
 */
void emit_record_copy_to_address(block_t *parent,
                                 basic_block_t **bb,
                                 var_t *dest_addr,
                                 var_t *src)
{
    int size = size_var(src);
    var_t *src_addr = require_ref_var(parent, src->type, 0);

    src_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, src_addr, src, NULL, 0, NULL);

    for (int offset = 0; offset < size;) {
        int width = 1;
        if (size - offset >= 4)
            width = 4;
        else if (size - offset >= 2)
            width = 2;
        var_t *src_part =
            compute_element_address(parent, bb, src_addr, offset, 1);
        var_t *dest_part =
            compute_element_address(parent, bb, dest_addr, offset, 1);
        var_t *value = require_var(parent);

        value->var_name = gen_name();
        add_insn(parent, *bb, OP_read, value, src_part, NULL, width, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_part, value, width, NULL);
        offset += width;
    }
}

void emit_object_assignment(block_t *parent,
                            basic_block_t **bb,
                            var_t *dest,
                            var_t *src)
{
    func_t *target = src->func_target;

    if (is_record_object(dest) && is_record_object(src)) {
        emit_record_copy(parent, bb, dest, src);
    } else if (dest->is_func && src->is_func &&
               find_var(src->var_name, parent) != src) {
        /* Function symbols are not scalar values: materialize their final
         * address through OP_write, which the backend patches after laying out
         * all functions. This is also needed for a declaration initializer such
         * as `int (*fn)(int) = target;`.
         */
        var_t *dest_addr = require_ref_var(parent, dest->type, dest->ptr_level);
        dest_addr->var_name = gen_name();
        add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_addr, src, PTR_SIZE, NULL);
        dest->func_target = src->func_target;
        dest->func_target_invalid =
            src->func_target_invalid || !target || !target->bbs;
    } else {
        src = resize_var(parent, bb, src, dest);
        add_insn(parent, *bb, OP_assign, dest, src, NULL, 0, NULL);
    }
}

/* Lower the designator that follows "&object" in a static initializer: member
 * selections and constant subscripts in any order, then an optional constant
 * offset. @object_addr is the address of @object; the address of the designated
 * subobject is returned and that subobject is left in @object. The scalar and
 * the aggregate initializer readers both come through here, so the forms they
 * accept, the scope names resolve in, and the diagnostics cannot drift apart.
 */
var_t *read_global_address_designator(block_t *scope,
                                      block_t *parent,
                                      basic_block_t **bb,
                                      var_t **object,
                                      var_t *object_addr)
{
    var_t *target = *object;
    fixed_array_shape_t shape = fixed_array_shape_from_var(target);
    int subscripts = 0;

    for (;;) {
        if (lex_accept(T_dot)) {
            char field_name[MAX_ID_LEN];
            var_t *field;

            lex_ident(T_identifier, field_name);
            field = find_member(field_name, target->type);
            if (!field)
                error_at("Unknown struct or union member", cur_token_loc());
            object_addr = compute_field_address(parent, bb, object_addr, field);
            target = field;
            shape = fixed_array_shape_from_var(target);
            subscripts = 0;
        } else if (lex_accept(T_open_square)) {
            int element_size =
                target->ptr_level ? PTR_SIZE : target->type->size;
            int index;

            if (!target->array_size || subscripts >= shape.rank)
                error_at("Subscripted global address needs an array object",
                         cur_token_loc());
            index = read_const_expr(scope);
            lex_expect(T_close_square);
            object_addr = compute_element_address(
                parent, bb, object_addr, index,
                fixed_array_shape_stride(&shape, subscripts, element_size));
            subscripts++;
        } else
            break;
        object_addr->ptr_level = target->ptr_level + 1;
        object_addr->is_global_address = true;
    }

    /* A trailing offset after a partial multidimensional subscript advances by
     * the remaining row or plane, the complete pointed-to object, rather than
     * by its scalar leaf. read_global_address_offset() takes the sign as part
     * of the constant expression.
     */
    if (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL)) {
        int element_size = target->ptr_level ? PTR_SIZE : target->type->size;
        int index = read_global_address_offset(scope, parent, *bb);

        object_addr = compute_element_address(
            parent, bb, object_addr, index,
            fixed_array_shape_stride(&shape, subscripts - 1, element_size));
        object_addr->ptr_level = target->ptr_level + 1;
        object_addr->is_global_address = true;
    }
    *object = target;
    return object_addr;
}

var_t *parse_global_constant_value(block_t *parent, basic_block_t **bb)
{
    var_t *val = NULL;
    block_t *scope = global_constant_initializer_scope
                         ? global_constant_initializer_scope
                         : parent;
    bool address_dereference =
        global_function_address_dereference_starts_here();
    token_t *address_dereference_identifier = NULL;
    bool explicit_address;
    bool grouped_function_designator = false;

    if (address_dereference) {
        address_dereference_identifier =
            consume_global_function_address_dereference();
        if (!find_visible_func(address_dereference_identifier->literal, scope))
            error_at("Function address requires a visible declaration",
                     cur_token_loc());
        val = require_func_symbol_var(parent);
        val->var_name = intern_string(address_dereference_identifier->literal);
        val->is_func = true;
        return val;
    }
    explicit_address = lex_accept(T_ampersand);

    if (grouped_global_function_designator_starts_here(false)) {
        lex_expect(T_open_bracket);
        grouped_function_designator = true;
    }

    if (explicit_address || grouped_function_designator ||
        lex_peek(T_identifier, NULL)) {
        char name[MAX_ID_LEN];

        if (lex_peek(T_identifier, name)) {
            func_t *func = find_visible_func(name, scope);
            if (func) {
                lex_expect(T_identifier);
                if (grouped_function_designator)
                    lex_expect(T_close_bracket);
                val = require_func_symbol_var(parent);
                val->var_name = intern_string(name);
                val->is_func = true;
                return val;
            }
            if (explicit_address) {
                /* A block-scope static initializer lowers through this global
                 * constant path, yet its names, subscripts, and offsets resolve
                 * in the declaration's lexical scope. The is_global test still
                 * rejects the address of an automatic object.
                 */
                var_t *object = find_var(name, scope);

                if (object && object->is_global) {
                    lex_expect(T_identifier);
                    val = require_ref_var(parent, object->type,
                                          object->ptr_level);
                    val->var_name = gen_name();
                    val->is_global_address = true;
                    add_insn(parent, *bb, OP_address_of, val, object, NULL, 0,
                             NULL);
                    return read_global_address_designator(scope, parent, bb,
                                                          &object, val);
                }
            }
        }
        if (explicit_address)
            error_at("Expected a global object or function after '&'",
                     cur_token_loc());
        error_at("Global aggregate initializer requires a constant value",
                 cur_token_loc());
    } else if (lex_peek(T_numeric, NULL) || lex_peek(T_minus, NULL)) {
        bool is_neg = false;
        if (lex_accept(T_minus))
            is_neg = true;
        char numtok[MAX_TOKEN_LEN];
        lex_ident_n(T_numeric, numtok, MAX_TOKEN_LEN);
        int num_val = parse_numeric_constant(numtok);
        if (is_neg)
            num_val = -num_val;

        val = require_var(parent);
        val->var_name = gen_name();
        val->init_val = num_val;
        add_insn(parent, *bb, OP_load_constant, val, NULL, NULL, 0, NULL);
    } else if (lex_peek(T_char, NULL) || lex_peek(T_wchar, NULL)) {
        char chtok[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];
        token_kind_t kind = lex_peek(T_wchar, NULL) ? T_wchar : T_char;
        lex_ident(kind, chtok);
        unescape_string(chtok, unescaped, MAX_TOKEN_LEN);

        val = require_typed_var(parent, TY_int);
        val->var_name = gen_name();
        val->init_val = kind == T_wchar ? parse_wide_character_constant(chtok)
                                        : parse_character_constant(chtok);
        add_insn(parent, *bb, OP_load_constant, val, NULL, NULL, 0, NULL);
    } else if (lex_peek(T_string, NULL)) {
        /* A character-pointer member has the same constant-expression form as a
         * standalone global pointer: retain the rodata address, including
         * adjacent-literal concatenation, for the aggregate store.
         */
        read_literal_param(parent, *bb);
        val = opstack_pop();
    } else {
        error_at("Global array initialization requires constant values",
                 next_token_loc());
    }

    return val;
}

void consume_global_constant_syntax(void)
{
    if (lex_peek(T_numeric, NULL)) {
        lex_accept(T_numeric);
    } else if (lex_peek(T_minus, NULL)) {
        lex_accept(T_minus);
        lex_accept(T_numeric);
    } else if (lex_peek(T_string, NULL)) {
        lex_accept(T_string);
    } else if (lex_peek(T_char, NULL) || lex_peek(T_wchar, NULL)) {
        lex_next();
    } else {
        error_at("Global array initialization requires constant values",
                 next_token_loc());
    }
}

bool is_record_type(const type_t *type)
{
    return type &&
           (type->base_type == TYPE_struct || type->base_type == TYPE_union ||
            (type->base_type == TYPE_typedef && type->num_fields > 0));
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr,
                             bool emit_code);

void parse_array_field_row_values(block_t *parent,
                                  basic_block_t **bb,
                                  const var_t *field,
                                  var_t *target_addr,
                                  int start,
                                  bool emit_code,
                                  bool braced)
{
    int count = 0;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *value = NULL;
        var_t *elem_addr;

        if (count >= field->array_dim2)
            error_at("Too many elements in array initializer",
                     next_token_loc());

        elem_addr = compute_element_address(parent, bb, target_addr,
                                            start + count, elem_size);
        if (lex_peek(T_open_curly, NULL) && is_record_type(field->type)) {
            type_t *record_type = field->type;
            if (record_type->base_type == TYPE_typedef &&
                record_type->base_struct)
                record_type = record_type->base_struct;
            lex_expect(T_open_curly);
            parse_struct_field_init(parent, bb, record_type, elem_addr,
                                    emit_code);
            lex_expect(T_close_curly);
        } else if (parent == GLOBAL_BLOCK) {
            if (emit_code)
                value = parse_global_constant_value(parent, bb);
            else
                consume_global_constant_syntax();
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            value = opstack_pop();
        }

        if (value && emit_code) {
            var_t *stored = value->is_func
                                ? value
                                : resize_to(parent, bb, value, field->type,
                                            field->ptr_level);
            add_insn(parent, *bb, OP_write, NULL, elem_addr, stored, elem_size,
                     NULL);
        }

        count++;
        if (count == field->array_dim2) {
            if (braced && lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                error_at("Too many elements in array initializer",
                         next_token_loc());
            break;
        }
        if (!lex_accept(T_comma))
            break;
    }
    if (braced)
        lex_expect(T_close_curly);

    if (emit_code) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (; count < field->array_dim2; count++) {
            var_t *elem_addr = compute_element_address(
                parent, bb, target_addr, start + count, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }
}

void parse_array_field_row_init(block_t *parent,
                                basic_block_t **bb,
                                const var_t *field,
                                var_t *target_addr,
                                int start,
                                bool emit_code)
{
    parse_array_field_row_values(parent, bb, field, target_addr, start,
                                 emit_code, true);
}

/* Parse one braced plane of a three-dimensional array. Rows reuse the
 * two-dimensional helper, which also supplies C99's trailing zero fill.
 */
void parse_array_field_plane_values(block_t *parent,
                                    basic_block_t **bb,
                                    const var_t *field,
                                    var_t *target_addr,
                                    int start,
                                    bool emit_code,
                                    bool braced)
{
    var_t row;
    int rows = 0;
    int row_width = field->array_dim3;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    memcpy(&row, field, sizeof(row));
    row.array_dim2 = row_width;
    row.array_dim3 = 0;
    if (braced) {
        lex_expect(T_open_curly);
        reject_empty_initializer_in_strict_c99();
    }
    while (!lex_peek(T_close_curly, NULL)) {
        if (lex_accept(T_open_square)) {
            int index = read_const_expr(field->scope ? field->scope : parent);

            lex_expect(T_close_square);
            if (index < 0 || index >= field->array_dim2)
                error_at("Array designator index is out of bounds",
                         cur_token_loc());
            rows = index;
            lex_expect(T_assign);
        }
        if (rows >= field->array_dim2)
            error_at("Too many rows in array initializer", next_token_loc());
        parse_array_field_row_values(parent, bb, &row, target_addr,
                                     start + rows * row_width, emit_code,
                                     lex_peek(T_open_curly, NULL));
        rows++;
        if (rows == field->array_dim2) {
            if (braced && lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                error_at("Too many rows in array initializer",
                         next_token_loc());
            break;
        }
        if (!lex_accept(T_comma))
            break;
    }
    if (braced)
        lex_expect(T_close_curly);

    if (emit_code) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (; rows < field->array_dim2; rows++) {
            for (int col = 0; col < row_width; col++) {
                var_t *addr = compute_element_address(
                    parent, bb, target_addr, start + rows * row_width + col,
                    elem_size);
                for (int byte = 0; byte < elem_size; byte++) {
                    var_t *byte_addr =
                        compute_element_address(parent, bb, addr, byte, 1);
                    add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1,
                             NULL);
                }
            }
        }
    }
}

void parse_array_field_plane_init(block_t *parent,
                                  basic_block_t **bb,
                                  const var_t *field,
                                  var_t *target_addr,
                                  int start,
                                  bool emit_code)
{
    parse_array_field_plane_values(parent, bb, field, target_addr, start,
                                   emit_code, true);
}

/* Parse one braced outer slice of a four-dimensional array. Each contained
 * three-dimensional plane reuses the existing plane parser, so row bounds and
 * trailing zero fill remain identical at every nesting level.
 */
void parse_array_field_hyperplane_init(block_t *parent,
                                       basic_block_t **bb,
                                       const var_t *field,
                                       var_t *target_addr,
                                       int start,
                                       bool emit_code)
{
    var_t plane;
    int planes = 0;
    int plane_size = field->array_dim3 * field->array_dim4;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    memcpy(&plane, field, sizeof(plane));
    plane.array_size = plane_size;
    plane.array_dim2 = field->array_dim3;
    plane.array_dim3 = field->array_dim4;
    plane.array_dim4 = 0;
    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    while (!lex_peek(T_close_curly, NULL)) {
        if (lex_accept(T_open_square)) {
            int index = read_const_expr(field->scope ? field->scope : parent);

            lex_expect(T_close_square);
            if (index < 0 || index >= field->array_dim2)
                error_at("Array designator index is out of bounds",
                         cur_token_loc());
            planes = index;
            lex_expect(T_assign);
        }
        if (planes >= field->array_dim2)
            error_at("Too many planes in array initializer", next_token_loc());
        parse_array_field_plane_values(parent, bb, &plane, target_addr,
                                       start + planes * plane_size, emit_code,
                                       lex_peek(T_open_curly, NULL));
        planes++;
        if (!lex_accept(T_comma))
            break;
    }
    lex_expect(T_close_curly);

    if (emit_code) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (; planes < field->array_dim2; planes++) {
            for (int element = 0; element < plane_size; element++) {
                var_t *addr = compute_element_address(
                    parent, bb, target_addr,
                    start + planes * plane_size + element, elem_size);
                for (int byte = 0; byte < elem_size; byte++) {
                    var_t *byte_addr =
                        compute_element_address(parent, bb, addr, byte, 1);
                    add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1,
                             NULL);
                }
            }
        }
    }
}

/* Read a string literal and every adjacent one after it, decoded and joined
 * into @combined, which holds MAX_STRING_LEN bytes (C99 translation phase 6).
 * Each piece is decoded in place at the end of what came before, so the
 * capacity handed to the decoder and the buffer it writes are the same object.
 * Returns the joined length.
 */
int read_concatenated_string(char *combined)
{
    char literal[MAX_STRING_LEN];
    int used;

    lex_ident(T_string, literal);
    unescape_string(literal, combined, MAX_STRING_LEN);
    used = strlen(combined);
    while (lex_peek(T_string, NULL)) {
        int added;

        lex_ident(T_string, literal);
        unescape_string(literal, combined + used, MAX_STRING_LEN - used);
        added = strlen(combined + used);
        if (used + added >= MAX_STRING_LEN - 1)
            error_at("Concatenated string literal too long", cur_token_loc());
        used += added;
    }
    return used;
}

/* An array member can be initialized directly by a string literal just like a
 * standalone character array. The member has no independent var_t storage, so
 * write through its already-computed field address rather than routing this
 * through parse_string_array_init().
 */
void parse_string_field_init(block_t *parent,
                             basic_block_t **bb,
                             const var_t *field,
                             var_t *target_addr,
                             bool emit_code)
{
    char combined[MAX_STRING_LEN];
    int len;

    read_concatenated_string(combined);

    len = strlen(combined) + 1;
    if (len > field->array_size)
        error_at("String initializer is too long for character array",
                 cur_token_loc());
    if (!emit_code)
        return;

    for (int i = 0; i < field->array_size; i++) {
        var_t *value = require_var(parent);
        var_t *addr = compute_element_address(parent, bb, target_addr, i, 1);

        value->var_name = gen_name();
        value->init_val = i < len ? (unsigned char) combined[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, addr, value, 1, NULL);
    }
}

bool is_char_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_type("char", true));
}

bool is_wchar_array(const var_t *var)
{
    return var && !var->ptr_level && !var->array_dim2 && !var->array_dim3 &&
           !var->array_dim4 &&
           compatible_decl_type(var->type, find_type("wchar_t", true));
}

/* C99 permits a string literal initializer only for an array of matching
 * character units. Diagnose this before the ordinary assignment path, which
 * treats a literal as a pointer and otherwise produces a misleading error.
 */
void validate_string_array_initializer(const var_t *var)
{
    if (!var || var->ptr_level ||
        !(var->array_size > 0 || var->has_unsized_array) ||
        !(lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)))
        return;
    if ((lex_peek(T_string, NULL) && !is_char_array(var)) ||
        (lex_peek(T_wstring, NULL) && !is_wchar_array(var)))
        error_at(
            "String literal initializer has incompatible array element type",
            cur_token_loc());
}

void parse_wstring_field_init(block_t *parent,
                              basic_block_t **bb,
                              const var_t *field,
                              var_t *target_addr,
                              bool emit_code)
{
    int values[MAX_STRING_LEN];
    int length;
    int units;

    length = read_wstring_units(values, MAX_STRING_LEN);

    units = length + 1;
    if (units > field->array_size)
        error_at("Wide string initializer is too long for array",
                 cur_token_loc());
    if (!emit_code)
        return;

    for (int i = 0; i < field->array_size; i++) {
        var_t *value = require_typed_var(parent, field->type);
        var_t *addr = compute_element_address(parent, bb, target_addr, i,
                                              field->type->size);

        value->var_name = gen_name();
        value->init_val = i < length ? values[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        add_insn(parent, *bb, OP_write, NULL, addr, value, field->type->size,
                 NULL);
    }
}

void parse_array_field_init(block_t *parent,
                            basic_block_t **bb,
                            const var_t *field,
                            var_t *target_addr,
                            bool emit_code)
{
    int count = 0;
    int elem_size =
        (field->ptr_level || field->is_func) ? PTR_SIZE : field->type->size;

    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *value = NULL;

        if (count >= field->array_size)
            error_at("Too many elements in array initializer",
                     next_token_loc());

        var_t *elem_addr =
            compute_element_address(parent, bb, target_addr, count, elem_size);
        if (field->array_dim4 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_hyperplane_init(parent, bb, field, target_addr,
                                              count, emit_code);
            count += field->array_dim2 * field->array_dim3 * field->array_dim4;
            if (!lex_accept(T_comma))
                break;
            continue;
        } else if (field->array_dim3 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_plane_init(parent, bb, field, target_addr, count,
                                         emit_code);
            count += field->array_dim2 * field->array_dim3;
            if (!lex_accept(T_comma))
                break;
            continue;
        } else if (field->array_dim2 && lex_peek(T_open_curly, NULL)) {
            parse_array_field_row_init(parent, bb, field, target_addr, count,
                                       emit_code);
            count += field->array_dim2;
            if (!lex_accept(T_comma))
                break;
            continue;
        } else if (lex_peek(T_open_curly, NULL) &&
                   is_record_type(field->type)) {
            type_t *record_type = field->type;
            if (record_type->base_type == TYPE_typedef &&
                record_type->base_struct)
                record_type = record_type->base_struct;
            lex_expect(T_open_curly);
            parse_struct_field_init(parent, bb, record_type, elem_addr,
                                    emit_code);
            lex_expect(T_close_curly);
        } else if (parent == GLOBAL_BLOCK) {
            if (emit_code)
                value = parse_global_constant_value(parent, bb);
            else
                consume_global_constant_syntax();
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            value = opstack_pop();
        }

        if (value && emit_code) {
            var_t *stored = value->is_func
                                ? value
                                : resize_to(parent, bb, value, field->type,
                                            field->ptr_level);
            add_insn(parent, *bb, OP_write, NULL, elem_addr, stored, elem_size,
                     NULL);
        }

        count++;
        if (!lex_accept(T_comma))
            break;
    }
    lex_expect(T_close_curly);

    if (emit_code) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);

        for (; count < field->array_size; count++) {
            var_t *elem_addr = compute_element_address(parent, bb, target_addr,
                                                       count, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }
}

void parse_struct_field_init(block_t *parent,
                             basic_block_t **bb,
                             type_t *struct_type,
                             var_t *target_addr,
                             bool emit_code)
{
    int field_idx = 0;
    int initializer_count = 0;

    reject_empty_initializer_in_strict_c99();

    /* Both of these back a "field" pointer that outlives the designator block
     * they are filled in from, so they have to live as long as the loop that
     * dereferences it rather than as long as that block.
     */
    var_t array_element;
    var_t pending_array_element;
    var_t *pending_array_base = NULL;
    int pending_array_index = 0;
    int pending_array_count = 0;
    int pending_array_elem_size = 0;
    bool has_pending_array_element = false;

    /* A static aggregate is zero-initialized before its initializer runs. A
     * bit-field store must nevertheless preserve earlier fields in its shared
     * allocation unit. Collect constant slices here and emit one direct store
     * per unit after the whole initializer has been parsed, avoiding a global
     * setup read before the global-pointer frame is established.
     */
    var_t *global_bitfield_unit[MAX_FIELDS];
    unsigned global_bitfield_value[MAX_FIELDS];
    int global_bitfield_count = 0;

    /* Zero the complete struct before processing fields. Positional
     * initializers could defer this until the first omitted member, but a
     * designator may skip forward or return to an earlier member.
     */
    if (emit_code && parent != GLOBAL_BLOCK &&
        struct_type->base_type != TYPE_union && !struct_type->is_union) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);

        for (int i = 0; i < struct_type->num_fields; i++) {
            var_t *field = &struct_type->fields[i];
            var_t *field_addr =
                compute_field_address(parent, bb, target_addr, field);
            int field_size = size_var(field);

            for (int offset = 0; offset < field_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, field_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *field_val_raw = NULL;
            var_t *field = NULL;
            var_t *designated_field_addr = NULL;
            bool consumed_pending_array_element = false;

            if (lex_accept(T_dot)) {
                char field_name[MAX_ID_LEN];
                type_t *designator_type = struct_type;
                var_t *designator_base = target_addr;
                bool first = true;

                for (;;) {
                    bool found = false;

                    lex_ident(T_identifier, field_name);
                    for (int i = 0; i < designator_type->num_fields; i++) {
                        if (!strcmp(designator_type->fields[i].var_name,
                                    field_name)) {
                            if (first)
                                field_idx = i;
                            field = &designator_type->fields[i];
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        error_at("Unknown field in record initializer",
                                 cur_token_loc());

                    designated_field_addr = compute_field_address(
                        parent, bb, designator_base, field);
                    if (lex_accept(T_open_square)) {
                        int index, element_count, linear_index, stride;
                        int total_element_count;
                        int elem_size;
                        var_t *array_base_addr = designated_field_addr;

                        if (!field->array_size)
                            error_at(
                                "Array designator requires an array member",
                                cur_token_loc());
                        index = read_const_expr(parent);
                        lex_expect(T_close_square);
                        total_element_count = field->array_size;
                        element_count = total_element_count;
                        stride = 1;
                        if (field->array_dim2) {
                            stride = field->array_dim2;
                            if (field->array_dim3) {
                                stride *= field->array_dim3;
                                if (field->array_dim4)
                                    stride *= field->array_dim4;
                            }
                            element_count /= stride;
                        }
                        if (index < 0 || index >= element_count)
                            error_at("Array designator index is out of bounds",
                                     cur_token_loc());
                        elem_size = (field->ptr_level || field->is_func)
                                        ? PTR_SIZE
                                        : field->type->size;
                        linear_index = index * stride;
                        designated_field_addr = compute_element_address(
                            parent, bb, designated_field_addr, linear_index,
                            elem_size);
                        memcpy(&array_element, field, sizeof(var_t));
                        if (field->array_dim4) {
                            array_element.array_size = field->array_dim2 *
                                                       field->array_dim3 *
                                                       field->array_dim4;
                            array_element.array_dim2 = field->array_dim3;
                            array_element.array_dim3 = field->array_dim4;
                            array_element.array_dim4 = 0;
                        } else if (field->array_dim3) {
                            array_element.array_size =
                                field->array_dim2 * field->array_dim3;
                            array_element.array_dim2 = field->array_dim3;
                            array_element.array_dim3 = 0;
                        } else if (field->array_dim2) {
                            array_element.array_size = field->array_dim2;
                            array_element.array_dim2 = 0;
                        } else {
                            array_element.array_size = 0;
                        }
                        field = &array_element;

                        /* Each subscript leaves the remaining inner array in
                         * `field`, so rows, planes, and a final scalar can all
                         * share the same bounds and continuation path.
                         */
                        while (field->array_size && lex_accept(T_open_square)) {
                            index = read_const_expr(parent);
                            lex_expect(T_close_square);
                            stride = field->array_dim2 ? field->array_dim2 : 1;
                            if (field->array_dim3)
                                stride *= field->array_dim3;
                            element_count = field->array_size / stride;
                            if (index < 0 || index >= element_count)
                                error_at(
                                    "Array designator index is out of bounds",
                                    cur_token_loc());
                            designated_field_addr = compute_element_address(
                                parent, bb, designated_field_addr,
                                index * stride, elem_size);
                            linear_index += index * stride;
                            if (field->array_dim3) {
                                array_element.array_size =
                                    field->array_dim2 * field->array_dim3;
                                array_element.array_dim2 = field->array_dim3;
                                array_element.array_dim3 = 0;
                            } else if (field->array_dim2) {
                                array_element.array_size = field->array_dim2;
                                array_element.array_dim2 = 0;
                            } else {
                                array_element.array_size = 0;
                            }
                        }
                        if (!field->array_size) {
                            memcpy(&pending_array_element, field,
                                   sizeof(var_t));
                            pending_array_base = array_base_addr;
                            pending_array_index = linear_index + 1;
                            pending_array_count = total_element_count;
                            pending_array_elem_size = elem_size;
                            has_pending_array_element = true;
                        }
                    }
                    if (!lex_accept(T_dot))
                        break;
                    if (!is_record_type(field->type))
                        error_at("Nested designator requires a record member",
                                 cur_token_loc());
                    designator_type = field->type;
                    if (designator_type->base_type == TYPE_typedef &&
                        designator_type->base_struct)
                        designator_type = designator_type->base_struct;
                    designator_base = designated_field_addr;
                    first = false;
                }
                lex_expect(T_assign);
            } else if (has_pending_array_element) {
                field = &pending_array_element;
                designated_field_addr = compute_element_address(
                    parent, bb, pending_array_base, pending_array_index,
                    pending_array_elem_size);
                consumed_pending_array_element = true;
            }

            /* An unnamed bit-field is padding, not an aggregate member. It
             * participates in layout but consumes no positional initializer;
             * otherwise `{ low, high }` would incorrectly assign `high` to the
             * padding field and leave the named field zero-initialized.
             */
            if (!field && !has_pending_array_element)
                while (field_idx < struct_type->num_fields &&
                       is_bitfield(&struct_type->fields[field_idx]) &&
                       !struct_type->fields[field_idx].var_name[0])
                    field_idx++;

            if (field_idx >= struct_type->num_fields ||
                ((struct_type->base_type == TYPE_union ||
                  struct_type->is_union) &&
                 initializer_count > 0 && !consumed_pending_array_element))
                error_at("Too many elements in record initializer",
                         next_token_loc());

            if (!field && field_idx < struct_type->num_fields)
                field = &struct_type->fields[field_idx];

            /* A scalar that meets an array member without braces initializes
             * the member's first element, and the following scalars fill the
             * rest before the next member (C99 6.7.8p20). Writing it at the
             * member's aggregate size instead corrupts the elements and gives
             * narrow backends a store width they cannot encode.
             */
            if (field && !designated_field_addr && !has_pending_array_element &&
                field->array_size &&
                (field->ptr_level || !is_record_type(field->type)) &&
                !lex_peek(T_open_curly, NULL) && !lex_peek(T_string, NULL) &&
                !lex_peek(T_wstring, NULL)) {
                pending_array_base =
                    compute_field_address(parent, bb, target_addr, field);
                pending_array_elem_size = (field->ptr_level || field->is_func)
                                              ? PTR_SIZE
                                              : field->type->size;
                pending_array_count = field->array_size;
                pending_array_index = 0;
                memcpy(&pending_array_element, field, sizeof(var_t));
                pending_array_element.array_size = 0;
                pending_array_element.array_dim2 = 0;
                pending_array_element.array_dim3 = 0;
                pending_array_element.array_dim4 = 0;
                field = &pending_array_element;
                designated_field_addr = compute_element_address(
                    parent, bb, pending_array_base, 0, pending_array_elem_size);
                has_pending_array_element = true;
                consumed_pending_array_element = true;
            }

            if (field && field->array_size && !field->ptr_level &&
                (lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)) &&
                ((lex_peek(T_string, NULL) && !is_char_array(field)) ||
                 (lex_peek(T_wstring, NULL) && !is_wchar_array(field))))
                error_at(
                    "String literal initializer has incompatible array element "
                    "type",
                    cur_token_loc());
            if (field && field->array_size && is_char_array(field) &&
                lex_peek(T_string, NULL)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_string_field_init(parent, bb, field, field_addr,
                                        emit_code);
            } else if (field && field->array_size && !field->ptr_level &&
                       compatible_decl_type(field->type,
                                            find_type("wchar_t", true)) &&
                       lex_peek(T_wstring, NULL)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_wstring_field_init(parent, bb, field, field_addr,
                                         emit_code);
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       field->array_size) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_array_field_init(parent, bb, field, field_addr,
                                       emit_code);
            } else if (field && lex_peek(T_open_curly, NULL) &&
                       is_record_type(field->type)) {
                type_t *nested_type = field->type;
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                if (nested_type->base_type == TYPE_typedef &&
                    nested_type->base_struct)
                    nested_type = nested_type->base_struct;
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, bb, nested_type, field_addr,
                                        emit_code);
                lex_expect(T_close_curly);
            } else if (parent == GLOBAL_BLOCK) {
                if (emit_code) {
                    field_val_raw = parse_global_constant_value(parent, bb);
                } else {
                    consume_global_constant_syntax();
                }
            } else {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                field_val_raw = opstack_pop();
            }

            if (field_val_raw && field_idx < struct_type->num_fields) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);

                if (is_record_type(field->type) &&
                    is_record_object(field_val_raw)) {
                    emit_record_copy_to_address(parent, bb, field_addr,
                                                field_val_raw);
                } else if (parent == GLOBAL_BLOCK && field_val_raw->is_func) {
                    /* Keep a function designator intact until global lowering
                     * can patch its final code address. Converting it through a
                     * scalar temporary loses that relocation provenance.
                     */
                    add_insn(parent, *bb, OP_write, NULL, field_addr,
                             field_val_raw, PTR_SIZE, NULL);
                } else {
                    var_t *field_val = resize_to(parent, bb, field_val_raw,
                                                 field->type, field->ptr_level);
                    int field_size = size_var(field);
                    if (is_bitfield(field) && parent == GLOBAL_BLOCK) {
                        int unit = -1;
                        for (int i = 0; i < global_bitfield_count; i++)
                            if (global_bitfield_unit[i]->offset ==
                                    field->offset &&
                                global_bitfield_unit[i]->bit_storage_size ==
                                    field->bit_storage_size) {
                                unit = i;
                                break;
                            }
                        if (unit < 0) {
                            unit = global_bitfield_count++;
                            global_bitfield_unit[unit] = field;
                            global_bitfield_value[unit] = 0;
                        }
                        unsigned mask = bitfield_mask(field)
                                        << field->bit_offset;
                        unsigned init_val =
                            is_bool_type(field->type)
                                ? field_val_raw->init_val != 0
                                : (unsigned) field_val_raw->init_val;
                        global_bitfield_value[unit] =
                            (global_bitfield_value[unit] & ~mask) |
                            ((init_val & bitfield_mask(field))
                             << field->bit_offset);
                    } else if (is_bitfield(field))
                        write_bitfield_value(parent, bb, field_addr, field_val,
                                             field);
                    else
                        add_insn(parent, *bb, OP_write, NULL, field_addr,
                                 field_val, field_size, NULL);
                }
            }

            if (has_pending_array_element) {
                if (consumed_pending_array_element)
                    pending_array_index++;
                if (pending_array_index >= pending_array_count) {
                    has_pending_array_element = false;
                    field_idx++;
                }
            } else {
                field_idx++;
            }
            initializer_count++;
            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }

    if (emit_code && parent == GLOBAL_BLOCK) {
        for (int i = 0; i < global_bitfield_count; i++) {
            var_t *unit = global_bitfield_unit[i];
            var_t *unit_addr =
                compute_field_address(parent, bb, target_addr, unit);
            var_t *value =
                bitfield_constant(parent, bb, global_bitfield_value[i]);
            add_insn(parent, *bb, OP_write, NULL, unit_addr, value,
                     unit->bit_storage_size, NULL);
        }
    }
}

void parse_array_literal_expr(block_t *parent, basic_block_t **bb)
{
    var_t *array_var = require_var(parent);
    array_var->var_name = gen_name();
    array_var->is_compound_literal = true;

    int element_count = 0;
    var_t *first_element = NULL;

    if (!lex_peek(T_close_curly, NULL)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        first_element = opstack_pop();
        element_count = 1;

        while (lex_accept(T_comma)) {
            if (lex_peek(T_close_curly, NULL))
                break;

            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            opstack_pop();
            element_count++;
        }
    }

    lex_expect(T_close_curly);

    array_var->array_size = element_count;
    if (first_element) {
        array_var->type = first_element->type;
        array_var->init_val = first_element->init_val;
    } else {
        array_var->type = TY_int;
        array_var->init_val = 0;
    }

    opstack_push(array_var);
    add_insn(parent, *bb, OP_load_constant, array_var, NULL, NULL, 0, NULL);
}

basic_block_t *handle_return_statement(block_t *parent, basic_block_t *bb)
{
    if (lex_accept(T_semicolon)) {
        if (strict_c99 && parent->func->return_def.type &&
            (parent->func->return_def.type->base_type != TYPE_void ||
             has_effective_pointer(&parent->func->return_def)))
            error_at("non-void function requires a return expression in C99",
                     cur_token_loc());
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    if (strict_c99 && parent->func->return_def.type &&
        parent->func->return_def.type->base_type == TYPE_void &&
        !has_effective_pointer(&parent->func->return_def))
        error_at("void function cannot return an expression in C99",
                 cur_token_loc());

    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, bb);
        if (!read_assignment_expression(parent, &bb)) {
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
        }
    }
    lex_expect(T_semicolon);

    var_t *rs1 = opstack_pop();

    /* Handle array compound literals in return context. Convert array compound
     * literals to their first element value.
     */
    if (is_array_literal_placeholder(rs1) && strict_c99 &&
        !has_effective_pointer(&parent->func->return_def))
        error_at("array compound literal cannot be used as a scalar in C99",
                 cur_token_loc());

    if (rs1 && rs1->array_size > 0 && rs1->var_name[0] == '.' && !strict_c99) {
        var_t *val = require_var(parent);
        val->type = rs1->type;
        val->init_val = rs1->init_val;
        val->var_name = gen_name();
        add_insn(parent, bb, OP_load_constant, val, NULL, NULL, 0, NULL);
        rs1 = val;
    }

    /* "return i++" yields the value i held before the increment, yet the
     * increment still has to happen before the function leaves. Applying the
     * pending side effects before the value was read returned the modified
     * variable instead, so take a copy first and return that.
     */
    if (se_idx > 0 && rs1) {
        var_t *snapshot = require_var(parent);
        snapshot->type = rs1->type;
        snapshot->ptr_level = rs1->ptr_level;
        snapshot->var_name = gen_name();
        add_insn(parent, bb, OP_assign, snapshot, rs1, NULL, 0, NULL);
        rs1 = snapshot;
    }
    perform_side_effect(parent, bb);

    /* Every ordinary use of a function designator converts it to a pointer.
     * This includes scalar conversions such as `_Bool f(void) { return cb; }`,
     * not only callback-pointer returns.
     */
    rs1 = materialize_function_designator(parent, &bb, rs1);
    if (parent->func->return_def.type->func_signature) {
        rs1->func_signature = parent->func->return_def.type->func_signature;
    }

    if (parent->func->returns_aggregate) {
        if (!is_record_object(rs1) ||
            !compatible_decl_type(rs1->type, parent->func->return_def.type))
            error_at("incompatible record return expression", cur_token_loc());
        emit_record_copy_to_address(parent, &bb, &parent->func->sret_def, rs1);
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    /* A return expression is converted to the function's declared type just
     * like an assignment. This is particularly important for _Bool: a pointer
     * return value must become 0 or 1 before it crosses the ABI boundary,
     * rather than leaving an address in the low return byte.
     */
    if (!parent->func->return_def.type->func_signature)
        rs1 = resize_to(parent, &bb, rs1, parent->func->return_def.type,
                        parent->func->return_def.ptr_level);

    add_insn(parent, bb, OP_return, NULL, rs1, NULL, 0, NULL);
    bb_connect(bb, parent->func->exit, NEXT);
    return NULL;
}

basic_block_t *handle_if_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    lex_expect(T_open_bracket);
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(bb, then_, THEN);
    bb_connect(bb, else_, ELSE);

    basic_block_t *then_body = read_body_statement(parent, then_);
    basic_block_t *then_next_ = NULL;
    if (then_body) {
        then_next_ = bb_create(parent);
        bb_connect(then_body, then_next_, NEXT);
    }

    if (lex_accept(T_else)) {
        basic_block_t *else_body = read_body_statement(parent, else_);
        basic_block_t *else_next_ = NULL;
        if (else_body) {
            else_next_ = bb_create(parent);
            bb_connect(else_body, else_next_, NEXT);
        }

        if (then_next_ && else_next_) {
            basic_block_t *next_ = bb_create(parent);
            bb_connect(then_next_, next_, NEXT);
            bb_connect(else_next_, next_, NEXT);
            return next_;
        }

        return then_next_ ? then_next_ : else_next_;
    } else {
        if (then_next_) {
            bb_connect(else_, then_next_, NEXT);
            return then_next_;
        }
        return else_;
    }
}

basic_block_t *handle_while_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    continue_bb_push(bb);

    basic_block_t *cond = bb;
    lex_expect(T_open_bracket);
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);

    var_t *vd = opstack_pop();
    add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(bb, then_, THEN);
    bb_connect(bb, else_, ELSE);
    break_bb_push(else_);

    basic_block_t *body_ = read_body_statement(parent, then_);

    continue_pos_idx--;
    break_exit_idx--;

    if (body_)
        bb_connect(body_, cond, NEXT);

    return else_;
}

basic_block_t *handle_goto_statement(block_t *parent, basic_block_t *bb)
{
    /* Since a goto splits the current program into two basic blocks and makes
     * the subsequent basic block unreachable, this causes problems for later
     * CFG operations. Therefore, we create a fake if that always executes to
     * wrap the goto, and connect the unreachable basic block to the else
     * branch. Finally, return this else block.
     *
     * after: a = b + c; goto label; c *= d;
     *
     * before: a = b + c; if (1)
     *     goto label;
     * c *= d;
     */

    char token[MAX_ID_LEN];
    if (!lex_peek(T_identifier, token))
        error_at("Expected identifier after 'goto'", next_token_loc());

    lex_expect(T_identifier);
    lex_expect(T_semicolon);

    basic_block_t *fake_if = bb_create(parent);
    bb_connect(bb, fake_if, NEXT);
    var_t *val = require_var(parent);
    val->var_name = gen_name();
    val->init_val = 1;
    add_insn(parent, fake_if, OP_load_constant, val, NULL, NULL, 0, NULL);
    add_insn(parent, fake_if, OP_branch, NULL, val, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    bb_connect(fake_if, then_, THEN);
    bb_connect(fake_if, else_, ELSE);

    add_insn(parent, then_, OP_jump, NULL, NULL, NULL, 0, token);
    label_t *label = find_label(token);
    if (label) {
        label->used = true;
        bb_connect(then_, label->bb, NEXT);
        return else_;
    }

    if (backpatch_bb_idx > MAX_LABELS - 1)
        error_at("Too many forward-referenced labels", cur_token_loc());

    backpatch_bb[backpatch_bb_idx++] = then_;
    return else_;
}

int read_const_expr(block_t *scope);

void parse_array_init(var_t *var,
                      block_t *parent,
                      basic_block_t **bb,
                      bool emit_code)
{
    int count = 0;
    int inferred_size = 0;
    var_t *base_addr = NULL;

    /* An omitted outer bound of a multidimensional declaration already has an
     * inner-dimension product in `array_size`. It is nevertheless inferred from
     * the initializer, just like a one-dimensional `int a[]`.
     */
    bool is_implicit = (var->array_size == 0 || var->has_unsized_array);
    block_t *initializer_scope = parent;

    if (parent == GLOBAL_BLOCK && var->scope && var->scope != GLOBAL_BLOCK)
        initializer_scope = var->scope;
    block_t *saved_initializer_scope = global_constant_initializer_scope;
    if (parent == GLOBAL_BLOCK)
        global_constant_initializer_scope = initializer_scope;

    /* Elements of a pointer array are pointer-sized. Using the base type's
     * width strided "char *a[2] = {...}" by one byte, so every element but the
     * first got a bogus address. `ptr_level` describes the array element type
     * even when its outer bound is inferred.
     */
    int elem_size = var->type->size;
    if (var->ptr_level > 0 || var->is_func)
        elem_size = PTR_SIZE;

    if (emit_code)
        base_addr = var;

    /* Reordered array designators can leave holes both before and after a
     * written element. Initialize the whole automatic array first, then let
     * explicit elements overwrite their slots. Byte stores also cover record
     * elements without relying on a backend-wide aggregate store.
     */
    if (parent != GLOBAL_BLOCK && emit_code && !is_implicit) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (int i = 0; i < var->array_size; i++) {
            var_t *elem_addr =
                compute_element_address(parent, bb, base_addr, i, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    lex_expect(T_open_curly);
    reject_empty_initializer_in_strict_c99();
    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *val = NULL;
            var_t designated_array;
            var_t *initializer_var = var;
            int prior_count = count;

            if (lex_accept(T_open_square)) {
                int index;
                int stride = 1;
                int elements;
                int remaining_dims = 0;

                index = read_const_expr(initializer_scope);
                lex_expect(T_close_square);
                if (var->array_dim2) {
                    stride = var->array_dim2;
                    remaining_dims = 1;
                    if (var->array_dim3) {
                        stride *= var->array_dim3;
                        remaining_dims = 2;
                        if (var->array_dim4) {
                            stride *= var->array_dim4;
                            remaining_dims = 3;
                        }
                    }
                }
                elements = var->array_size / stride;
                if (index < 0 || (!is_implicit && index >= elements))
                    error_at("Array designator index is out of bounds",
                             cur_token_loc());
                count = index * stride;

                /* A complete multidimensional designator names a scalar
                 * element. Keep the outer-only form on the existing braced
                 * slice path, but turn every complete path into the row-major
                 * flat offset used by the ordinary store path.
                 */
                while (remaining_dims && lex_accept(T_open_square)) {
                    index = read_const_expr(initializer_scope);
                    lex_expect(T_close_square);
                    if (remaining_dims == 3) {
                        elements = var->array_dim2;
                        stride = var->array_dim3 * var->array_dim4;
                    } else if (remaining_dims == 2) {
                        elements = var->array_dim2;
                        stride = var->array_dim3;
                        if (var->array_dim4) {
                            elements = var->array_dim3;
                            stride = var->array_dim4;
                        }
                    } else {
                        elements = var->array_dim2;
                        if (var->array_dim3)
                            elements = var->array_dim3;
                        if (var->array_dim4)
                            elements = var->array_dim4;
                        stride = 1;
                    }
                    if (index < 0 || index >= elements)
                        error_at("Array designator index is out of bounds",
                                 cur_token_loc());
                    count += index * stride;
                    remaining_dims--;
                }
                if (remaining_dims == 1 &&
                    (var->array_dim3 || var->array_dim4)) {
                    /* A path ending one dimension short of a scalar names a
                     * row. Present it to the braced-row helper rather than
                     * asking a scalar initializer to consume `{ ... }`.
                     */
                    int row_width =
                        var->array_dim4 ? var->array_dim4 : var->array_dim3;
                    memcpy(&designated_array, var, sizeof(designated_array));
                    designated_array.array_size = row_width;
                    designated_array.array_dim2 = row_width;
                    designated_array.array_dim3 = 0;
                    designated_array.array_dim4 = 0;
                    initializer_var = &designated_array;
                }
                lex_expect(T_assign);
            }

            if (!is_implicit && count >= var->array_size)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (initializer_var->array_dim4 && lex_peek(T_open_curly, NULL)) {
                parse_array_field_hyperplane_init(parent, bb, initializer_var,
                                                  base_addr, count, emit_code);
                count += initializer_var->array_dim2 *
                         initializer_var->array_dim3 *
                         initializer_var->array_dim4;
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (initializer_var->array_dim3 &&
                       lex_peek(T_open_curly, NULL)) {
                parse_array_field_plane_init(parent, bb, initializer_var,
                                             base_addr, count, emit_code);
                count +=
                    initializer_var->array_dim2 * initializer_var->array_dim3;
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (initializer_var->array_dim2 &&
                       lex_peek(T_open_curly, NULL)) {
                parse_array_field_row_init(parent, bb, initializer_var,
                                           base_addr, count, emit_code);
                count += initializer_var->array_dim2;
                if (is_implicit && count > inferred_size)
                    inferred_size = count;
                if (!lex_accept(T_comma))
                    break;
                continue;
            } else if (lex_peek(T_open_curly, NULL) &&
                       is_record_type(var->type)) {
                type_t *struct_type = var->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                if (emit_code) {
                    var_t *elem_addr = compute_element_address(
                        parent, bb, base_addr, count, elem_size);
                    lex_expect(T_open_curly);
                    parse_struct_field_init(parent, bb, struct_type, elem_addr,
                                            emit_code);
                    lex_expect(T_close_curly);
                    val = NULL;
                } else {
                    lex_expect(T_open_curly);
                    while (!lex_peek(T_close_curly, NULL)) {
                        if (parent == GLOBAL_BLOCK) {
                            consume_global_constant_syntax();
                        } else {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            opstack_pop();
                        }
                        if (!lex_accept(T_comma))
                            break;
                        if (lex_peek(T_close_curly, NULL))
                            break;
                    }
                    lex_expect(T_close_curly);
                    val = NULL;
                }
            } else {
                /* A global initializer is restricted to simple constants, but
                 * it still has to be stored. Consuming the tokens and dropping
                 * the value left every global array zero-filled, while the same
                 * initializer on a local worked.
                 */
                if (parent == GLOBAL_BLOCK &&
                    (lex_peek(T_ampersand, NULL) ||
                     grouped_global_function_designator_starts_here(true) ||
                     global_function_address_dereference_starts_here())) {
                    val = parse_global_constant_value(parent, bb);
                } else {
                    char initializer_name[MAX_ID_LEN];
                    char global_token[MAX_ID_LEN];
                    bool function_initializer =
                        lex_peek(T_identifier, initializer_name) &&
                        find_visible_func(initializer_name, initializer_scope);
                    var_t *object_constant = NULL;

                    if (parent == GLOBAL_BLOCK &&
                        lex_peek(T_identifier, global_token))
                        object_constant =
                            find_var(global_token, initializer_scope);

                    if (parent == GLOBAL_BLOCK &&
                        initializer_scope != GLOBAL_BLOCK &&
                        !lex_peek(T_string, NULL) && !function_initializer &&
                        !lex_peek(T_ampersand, NULL)) {
                        /* Storage for a block-scope static lives globally,
                         * while its initializer is an integer constant
                         * expression in the surrounding block. Resolve local
                         * enumerators before emitting the global setup-store
                         * value.
                         */
                        val = require_var(GLOBAL_BLOCK);
                        val->var_name = gen_name();
                        val->init_val = read_const_expr(var->scope);
                        val->is_const = true;
                        add_insn(GLOBAL_BLOCK, *bb, OP_load_constant, val, NULL,
                                 NULL, 0, NULL);
                    } else {
                        if (parent == GLOBAL_BLOCK) {
                            char token[MAX_ID_LEN];
                            bool enum_constant =
                                lex_peek(T_identifier, token) &&
                                find_scoped_constant(token, parent);
                            bool function_constant =
                                lex_peek(T_identifier, token) &&
                                find_visible_func(token, initializer_scope);

                            if (!lex_peek(T_numeric, NULL) &&
                                !lex_peek(T_minus, NULL) &&
                                !lex_peek(T_string, NULL) &&
                                !lex_peek(T_char, NULL) &&
                                !lex_peek(T_wchar, NULL) && !enum_constant &&
                                !function_constant &&
                                !lex_peek(T_ampersand, NULL) &&
                                !(object_constant &&
                                  object_constant->is_global &&
                                  object_constant->array_size))
                                error_at(
                                    "Global array initialization requires "
                                    "constant "
                                    "values",
                                    next_token_loc());
                        }

                        if (parent == GLOBAL_BLOCK && object_constant &&
                            object_constant->is_global &&
                            object_constant->array_size) {
                            int stride = (object_constant->ptr_level ||
                                          object_constant->is_func)
                                             ? PTR_SIZE
                                             : object_constant->type->size;

                            /* Array-to-pointer conversion is a permitted
                             * address constant in static aggregate
                             * initializers. Preserve its row stride here rather
                             * than reading an object value, which global setup
                             * cannot do before GP is established.
                             */
                            val = require_ref_var(parent, object_constant->type,
                                                  object_constant->ptr_level);
                            val->var_name = gen_name();
                            lex_ident(T_identifier, global_token);
                            add_insn(parent, *bb, OP_address_of, val,
                                     object_constant, NULL, 0, NULL);
                            if (object_constant->array_dim2)
                                stride *= object_constant->array_dim2;
                            if (object_constant->array_dim3)
                                stride *= object_constant->array_dim3;
                            if (object_constant->array_dim4)
                                stride *= object_constant->array_dim4;
                            if (lex_accept(T_plus)) {
                                int index = read_const_expr(initializer_scope);

                                val = compute_element_address(parent, bb, val,
                                                              index, stride);
                            } else if (lex_accept(T_minus)) {
                                int index = read_const_expr(initializer_scope);

                                val = compute_element_address(parent, bb, val,
                                                              -index, stride);
                            }
                        } else {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            val = opstack_pop();
                        }
                    }
                }
            }

            if (is_implicit && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (val && var->type->array_element_pointee_func_signature) {
                /* The array descriptor is not an element descriptor. Build the
                 * slot type that this scalar initializer is about to store so
                 * callback prototype validation remains elementwise.
                 */
                var_t element_target = {0};

                element_target.type = var->type;
                element_target.ptr_level = var->type->array_element_ptr_level;
                element_target.pointee_func_signature =
                    var->type->array_element_pointee_func_signature;
                if (incompatible_pointee_callback_conversion(val,
                                                             &element_target))
                    error_at(
                        "incompatible callback slot types in array "
                        "initializer",
                        cur_token_loc());
            }

            if (val && emit_code && (is_implicit || count < var->array_size)) {
                /* Keep a function symbol intact until OP_address_of_func can
                 * emit its deferred relocation. Treating an array-of-callback
                 * element as an `int` conversion loses that provenance.
                 */
                var_t *v;

                if (val->is_func || var->ptr_level > 0)
                    v = val;
                else
                    v = resize_to(parent, bb, val, var->type, 0);

                /* A forward designator leaves a gap. Explicit arrays were
                 * zeroed before parsing; inferred local arrays do not have a
                 * known bound yet, so zero just the newly skipped elements.
                 * Global storage begins zeroed.
                 */
                if (is_implicit && parent != GLOBAL_BLOCK &&
                    count > prior_count) {
                    var_t *zero = require_var(parent);
                    zero->var_name = gen_name();
                    zero->init_val = 0;
                    add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0,
                             NULL);
                    for (int i = prior_count; i < count; i++) {
                        var_t *gap_addr = compute_element_address(
                            parent, bb, base_addr, i, elem_size);
                        for (int offset = 0; offset < elem_size; offset++) {
                            var_t *byte_addr = compute_element_address(
                                parent, bb, gap_addr, offset, 1);
                            add_insn(parent, *bb, OP_write, NULL, byte_addr,
                                     zero, 1, NULL);
                        }
                    }
                }

                var_t *elem_addr = compute_element_address(
                    parent, bb, base_addr, count, elem_size);

                if (elem_size <= PTR_SIZE) {
                    add_insn(parent, *bb, OP_write, NULL, elem_addr, v,
                             elem_size, NULL);
                } else {
                    fatal("Unsupported: array element wider than a pointer");
                }
            }

            count++;
            if (is_implicit && count > inferred_size)
                inferred_size = count;
            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }

    lex_expect(T_close_curly);

    if (is_implicit) {
        var->array_size = inferred_size;
        var->has_unsized_array = false;
    }
    global_constant_initializer_scope = saved_initializer_scope;
}

void parse_array_compound_literal(var_t *var,
                                  block_t *parent,
                                  basic_block_t **bb)
{
    int elem_size = var->type->size;
    int count = 0;

    reject_empty_initializer_in_strict_c99();

    /* A compound literal may spell either an inferred bound, ``int[]``, or an
     * actual array type, ``int[4]``. The latter is not merely syntax: omitted
     * members are zero-initialized and an excess initializer is a constraint
     * violation. Keep the parsed bound until the initializer has been consumed;
     * previously this routine reset it and silently turned every declared-bound
     * literal into an inferred-size one.
     */
    int declared_size = var->array_size;
    int inferred_size = 0;
    var->init_val = 0;

    /* A designated element can leave holes before or after it, so initialize
     * the declared object before parsing any explicit elements.
     */
    if (declared_size) {
        var_t *zero = require_var(parent);
        zero->var_name = gen_name();
        zero->init_val = 0;
        add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        for (int i = 0; i < declared_size; i++) {
            var_t *elem_addr =
                compute_element_address(parent, bb, var, i, elem_size);
            for (int offset = 0; offset < elem_size; offset++) {
                var_t *byte_addr =
                    compute_element_address(parent, bb, elem_addr, offset, 1);
                add_insn(parent, *bb, OP_write, NULL, byte_addr, zero, 1, NULL);
            }
        }
    }

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            if (lex_accept(T_open_square)) {
                count = read_const_expr(parent);
                lex_expect(T_close_square);
                lex_expect(T_assign);
            }
            if (declared_size && count >= declared_size)
                error_at("Too many elements in array compound literal",
                         next_token_loc());
            if (!declared_size && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array compound literal",
                         next_token_loc());

            /* An inferred-bound array gets its size only after the closing
             * brace. Still zero every gap before storing a designator so the
             * automatic object obeys C99's aggregate initialization rule.
             * inferred_size is one past the highest initialized slot, so a
             * later backward designator cannot make a forward gap overwrite an
             * earlier explicit value.
             */
            if (!declared_size && count > inferred_size) {
                var_t *zero = require_var(parent);
                zero->var_name = gen_name();
                zero->init_val = 0;
                add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0,
                         NULL);
                for (int i = inferred_size; i < count; i++) {
                    var_t *gap_addr =
                        compute_element_address(parent, bb, var, i, elem_size);
                    for (int offset = 0; offset < elem_size; offset++) {
                        var_t *byte_addr = compute_element_address(
                            parent, bb, gap_addr, offset, 1);
                        add_insn(parent, *bb, OP_write, NULL, byte_addr, zero,
                                 1, NULL);
                    }
                }
            }

            var_t *elem_addr =
                compute_element_address(parent, bb, var, count, elem_size);
            if (lex_peek(T_open_curly, NULL) && is_record_type(var->type)) {
                /* The array compound literal owns a real aggregate object, just
                 * like an ordinary array initializer. A braced element must
                 * therefore be lowered through the shared record path; treating
                 * it as an expression rejected the opening brace and made
                 * (struct S[]){ { ... }, { ... } } unusable.
                 */
                type_t *record_type = var->type;
                if (record_type->base_type == TYPE_typedef &&
                    record_type->base_struct)
                    record_type = record_type->base_struct;

                lex_expect(T_open_curly);
                parse_struct_field_init(parent, bb, record_type, elem_addr,
                                        true);
                lex_expect(T_close_curly);
            } else {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                var_t *value = opstack_pop();
                if (count == 0)
                    var->init_val = value->init_val;

                var_t *store_val = resize_to(parent, bb, value, var->type, 0);
                add_insn(parent, *bb, OP_write, NULL, elem_addr, store_val,
                         elem_size, NULL);
            }

            if (!declared_size) {
                if (count + 1 > inferred_size)
                    inferred_size = count + 1;
            }
            count++;
            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }

    lex_expect(T_close_curly);

    var->array_size = declared_size ? declared_size : inferred_size;
}

/* Identify compiler-emitted temporaries that hold array compound literals. They
 * keep array metadata without pointer indirection and are marked via
 * is_compound_literal when synthesized.
 */
bool is_array_literal_placeholder(const var_t *var)
{
    return var && var->array_size > 0 && !var->ptr_level &&
           var->is_compound_literal;
}

bool is_pointer_like_value(var_t *var)
{
    return var && (var->ptr_level || var->array_size ||
                   (var->type && var->type->ptr_level > 0));
}

/* Lower a compiler-emitted array literal placeholder (marked via
 * is_compound_literal) into a scalar temporary when later IR expects a plain
 * value instead of addressable storage. This keeps SSA joins uniform when only
 * one branch originates from an array literal.
 */
var_t *scalarize_array_literal(block_t *parent,
                               basic_block_t **bb,
                               var_t *array_var,
                               type_t *hint_type)
{
    if (!is_array_literal_placeholder(array_var))
        return array_var;

    if (strict_c99)
        error_at("array compound literal cannot be used as a scalar in C99",
                 cur_token_loc());

    /* Array literal placeholders carry the literal's natural type; default to
     * int when the parser left the type unset.
     */
    type_t *literal_type = array_var->type ? array_var->type : TY_int;
    int literal_size = literal_type->size;
    if (literal_size <= 0)
        literal_size = TY_int->size;

    /* A caller-provided hint (e.g., assignment target) dictates the result type
     * when available so we reuse wider/narrower scalar destinations.
     */
    type_t *result_type = hint_type ? hint_type : literal_type;
    if (!result_type)
        result_type = TY_int;

    /* Create a new scalar temporary, giving it a unique name and copying over
     * the literal data so downstream code can treat it like a normal value.
     */
    var_t *scalar = require_typed_var(parent, result_type);
    scalar->ptr_level = 0;
    scalar->var_name = gen_name();
    scalar->init_val = array_var->init_val;

    /* Materialize the literal data into the scalar temporary via an OP_read. */
    add_insn(parent, *bb, OP_read, scalar, array_var, NULL, literal_size, NULL);

    return scalar;
}

/* Centralized guard for lowering array literal placeholders when a scalar value
 * is expected, keeping the scattered special cases consistent.
 */
var_t *scalarize_array_literal_if_needed(block_t *parent,
                                         basic_block_t **bb,
                                         var_t *value,
                                         type_t *hint_type,
                                         bool needs_scalar)
{
    if (!needs_scalar)
        return value;

    return scalarize_array_literal(parent, bb, value, hint_type);
}

/* Integer constant-expression parser.
 *
 * Array dimensions, and other places C requires an integer constant expression,
 * accept far more than a bare literal. These evaluate such an expression at
 * parse time without emitting any IR, folding through the same precedence table
 * (get_operator_prio()) and the same operator semantics (eval_expression_imm())
 * the rest of the parser already uses, so there is only one statement of what
 * C's operators mean.
 */
#define MAX_CONST_EXPR_OPS 16

int eval_expression_imm(opcode_t op, int op1, int op2);
int read_const_expr(block_t *scope);
int read_const_wstring_size(void);

void read_sizeof_function_prototype(void)
{
    func_t *func = arena_alloc_func();
    bool saved_sizeof_signature = parsing_sizeof_function_signature;

    parsing_sizeof_function_signature = true;
    read_parameter_list_decl(func, true);
    parsing_sizeof_function_signature = saved_sizeof_signature;
}

/* Consume a nested direct-abstract-declarator in forms such as `int
 * (*(*)(int))[2]` and `int (*(*(*)(int))(int))(int)`. The surrounding
 * declarator already consumed its leading `(` and pointer chain. Each recursive
 * invocation consumes its own parenthesized pointer declarator and function
 * suffix, then the enclosing `)` and that declarator's suffix. The complete
 * type remains pointer-sized, but its bounds and prototypes must still obey C99
 * syntax and the project's fixed-array-only policy.
 */
int read_sizeof_nested_function_pointer_suffix(block_t *scope,
                                               int *outer_array_size)
{
    int inner_ptr_count = 0;

    lex_expect(T_open_bracket);
    while (lex_accept(T_asterisk)) {
        inner_ptr_count++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    if (!inner_ptr_count)
        error_at("sizeof nested abstract declarator needs a pointer",
                 cur_token_loc());

    if (lex_peek(T_open_bracket, NULL))
        inner_ptr_count +=
            read_sizeof_nested_function_pointer_suffix(scope, outer_array_size);
    else {
        int direct_array_size = 0;

        while (lex_accept(T_open_square)) {
            int bound = read_const_expr(scope);

            lex_expect(T_close_square);
            if (bound <= 0)
                error_at("sizeof array type needs a positive constant bound",
                         cur_token_loc());
            if (bound > 0 && direct_array_size &&
                direct_array_size > INT_MAX / bound)
                error_at("sizeof array type is too large", cur_token_loc());
            if (bound > 0)
                direct_array_size =
                    direct_array_size ? direct_array_size * bound : bound;
        }
        if (direct_array_size && outer_array_size)
            *outer_array_size = direct_array_size;
        lex_expect(T_close_bracket);
        if (lex_peek(T_open_bracket, NULL)) {
            read_sizeof_function_prototype();
        } else if (lex_peek(T_open_square, NULL)) {
            do {
                int bound;

                lex_expect(T_open_square);
                bound = read_const_expr(scope);
                lex_expect(T_close_square);
                if (bound <= 0)
                    error_at(
                        "sizeof array type needs a positive constant bound",
                        cur_token_loc());
            } while (lex_peek(T_open_square, NULL));
        } else {
            error_at(
                "sizeof nested abstract declarator needs a function or "
                "array suffix",
                cur_token_loc());
        }
    }

    lex_expect(T_close_bracket);
    while (lex_peek(T_open_bracket, NULL))
        read_sizeof_function_prototype();
    while (lex_accept(T_open_square)) {
        int bound = read_const_expr(scope);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
    }
    return inner_ptr_count;
}

/* Integer constant expressions may contain sizeof(type-name). This parser only
 * needs type metadata, so keep it separate from expression lowering and avoid
 * emitting the otherwise unevaluated sizeof IR into an array bound or
 * enumerator declaration.
 */
int read_const_sizeof_type(block_t *scope)
{
    char token[MAX_ID_LEN];
    type_t *type = NULL;
    int ptr_level = 0;
    int array_size = 0;
    int array_element_size = 0;
    bool is_unsigned = false;
    bool is_signed = false;
    int long_count = 0;

    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        lex_ident(T_identifier, token);
        type = find_type(token, 2);
    } else if (lex_accept(T_enum)) {
        lex_ident(T_identifier, token);
        type = find_type_tag(token, scope);
    } else {
        /* Declaration specifiers may appear in any order: all of "unsigned long
         * int", "long unsigned int", and "int unsigned" name the same type.
         */
        while (true) {
            if (lex_accept(T_unsigned)) {
                if (is_unsigned)
                    error_at("duplicate unsigned type specifier",
                             cur_token_loc());
                is_unsigned = true;
            } else if (lex_accept(T_signed)) {
                if (is_signed)
                    error_at("duplicate signed type specifier",
                             cur_token_loc());
                is_signed = true;
            } else if (lex_accept(T_long)) {
                long_count++;
            } else if (lex_accept(T_float)) {
                type = TY_float;
            } else if (lex_accept(T_double)) {
                type = TY_double;
            } else if (lex_accept(T_const) || lex_accept(T_volatile) ||
                       lex_accept(T_restrict)) {
                ;
            } else if (lex_peek(T_identifier, token)) {
                type_t *candidate = find_visible_type(token, scope);

                if (!candidate)
                    break;
                lex_expect(T_identifier);
                type = candidate;
            } else {
                break;
            }
        }
        if (is_unsigned && is_signed)
            error_at("both signed and unsigned specified", cur_token_loc());
        if (long_count > 2)
            error_at("too many long type specifiers", cur_token_loc());
        if (type == TY_float && (is_unsigned || is_signed || long_count))
            error_at("invalid float type specifiers", cur_token_loc());
        if (type == TY_double) {
            if (is_unsigned || is_signed || long_count > 1)
                error_at("invalid double type specifiers", cur_token_loc());
            if (long_count)
                type = TY_long_double;
        } else if (long_count) {
            type = is_unsigned ? TY_ulong : TY_long;
            if (long_count == 2)
                type = is_unsigned ? TY_ulong_long : TY_long_long;
        } else if (is_unsigned) {
            if (type == TY_char)
                type = TY_uchar;
            else if (type == TY_short)
                type = TY_ushort;
            else
                type = TY_uint;
        } else if (is_signed) {
            type = type == TY_char ? TY_schar : (type ? type : TY_int);
        }
    }
    if (!type)
        error_at(
            "sizeof in an integer constant expression requires a type name",
            cur_token_loc());
    while (lex_accept(T_asterisk)) {
        ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }
    while (ptr_level == 0 && lex_accept(T_open_square)) {
        int bound = read_const_expr(scope);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
        if (bound > 0 && array_size && array_size > INT_MAX / bound)
            error_at("sizeof array type is too large", cur_token_loc());
        if (bound > 0)
            array_size = array_size ? array_size * bound : bound;
    }
    if (lex_accept(T_open_bracket)) {
        int nested_array_size = 0;
        int nested_ptr_count = 0;
        int nested_function_ptr_count = 0;
        int nested_outer_array_size = 0;

        while (lex_accept(T_asterisk)) {
            nested_ptr_count++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
        if (!nested_ptr_count)
            error_at("sizeof abstract declarator needs a pointer",
                     cur_token_loc());
        if (lex_peek(T_open_bracket, NULL)) {
            nested_function_ptr_count =
                read_sizeof_nested_function_pointer_suffix(
                    scope, &nested_outer_array_size);
            if (nested_outer_array_size) {
                array_size = nested_outer_array_size;
                array_element_size = PTR_SIZE;
            } else
                ptr_level += nested_ptr_count + nested_function_ptr_count;
        } else
            while (lex_accept(T_open_square)) {
                int bound = read_const_expr(scope);

                lex_expect(T_close_square);
                if (bound <= 0)
                    error_at(
                        "sizeof array type needs a positive constant bound",
                        cur_token_loc());
                if (bound > 0 && nested_array_size &&
                    nested_array_size > INT_MAX / bound)
                    error_at("sizeof array type is too large", cur_token_loc());
                if (bound > 0)
                    nested_array_size =
                        nested_array_size ? nested_array_size * bound : bound;
            }
        if (!nested_function_ptr_count) {
            lex_expect(T_close_bracket);
            if (nested_array_size) {
                array_size = nested_array_size;
                array_element_size = PTR_SIZE;

                /* `int (*[2][3])(int)` is an array of function pointers: the
                 * parenthesized declarator owns the array bounds, while the
                 * following parameter list belongs to each pointed-to function.
                 * Consume that suffix before the enclosing sizeof parenthesis.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            } else {
                ptr_level += nested_ptr_count;
                while (lex_accept(T_open_square)) {
                    int bound = read_const_expr(scope);

                    lex_expect(T_close_square);
                    if (bound <= 0)
                        error_at(
                            "sizeof array type needs a positive constant bound",
                            cur_token_loc());
                }

                /* A pointer declarator followed by a parameter list names a
                 * function pointer. Its function type has no object
                 * representation, but the pointer itself is an object and
                 * sizeof is pointer-sized. Reuse the declaration parser for
                 * complete prototype syntax, then discard the otherwise-unused
                 * signature.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            }
        }
    }
    lex_expect(T_close_bracket);
    if (ptr_level)
        return PTR_SIZE;
    if (type == TY_void)
        error_at("sizeof cannot be applied to void", cur_token_loc());
    if (type->is_direct_function_type)
        error_at("sizeof(function) is invalid", cur_token_loc());
    if (!type->size && (!type->base_struct || !type->base_struct->size))
        error_at("sizeof cannot be applied to an incomplete type",
                 cur_token_loc());
    if (!array_size && !ptr_level && type->array_size)
        array_size = type->array_size;
    if (array_size)
        return array_size *
               (array_element_size ? array_element_size : type->size);
    if (type->size)
        return type->size;
    return type->base_struct->size;
}

/* Return the row-major offset contributed by a fixed terminal array member in
 * offsetof. A one-past designator is valid only for the final dimension.
 */
static int read_offsetof_array_subscripts(var_t *field, block_t *scope)
{
    int offset = 0;
    fixed_array_shape_t shape;
    int dimensions;

    if (!lex_accept(T_open_square))
        return 0;
    if (field->array_size <= 0)
        error_at("offsetof subscript requires a fixed array member",
                 cur_token_loc());
    shape = fixed_array_shape_from_var(field);
    dimensions = shape.rank;
    for (int dim = 0;; dim++) {
        int index = read_const_expr(scope);

        lex_expect(T_close_square);
        if (index < 0 || index > shape.bounds[dim])
            error_at("offsetof array subscript is out of bounds",
                     cur_token_loc());
        offset += index * fixed_array_shape_stride(&shape, dim, 1) *
                  field->type->size;
        if (!lex_accept(T_open_square))
            return offset;
        if (dim + 1 == dimensions)
            error_at("offsetof subscript exceeds array dimensions",
                     cur_token_loc());
        if (index == shape.bounds[dim])
            error_at("offsetof one-past array row cannot be subscripted",
                     cur_token_loc());
    }
}

int read_const_expr_operand(block_t *scope)
{
    char buffer[MAX_TOKEN_LEN];

    if (lex_accept(T_minus)) {
        int value = read_const_expr_operand(scope);

        if (checking_enum_constant && value == INT_MIN)
            error_at("Enumerator value exceeds int range", cur_token_loc());
        return -value;
    }
    if (lex_accept(T_plus))
        return read_const_expr_operand(scope);
    if (lex_accept(T_bit_not))
        return ~read_const_expr_operand(scope);
    if (lex_accept(T_log_not))
        return !read_const_expr_operand(scope);
    if (lex_accept(T_sizeof)) {
        if (lex_peek(T_wstring, NULL))
            return read_const_wstring_size();
        return read_const_sizeof_type(scope);
    }

    if (lex_accept(T_open_bracket)) {
        int res = read_const_expr(scope);
        lex_expect(T_close_bracket);
        return res;
    }
    if (lex_peek(T_numeric, buffer)) {
        lex_expect(T_numeric);
        return parse_numeric_constant(buffer);
    }
    if (lex_peek(T_char, buffer) || lex_peek(T_wchar, buffer)) {
        char unescaped[MAX_TOKEN_LEN];
        token_kind_t kind = lex_peek(T_wchar, NULL) ? T_wchar : T_char;
        lex_expect(kind);
        if (unescape_string(buffer, unescaped, MAX_TOKEN_LEN) < 0)
            error_at("Invalid escape sequence", cur_token_loc());
        return kind == T_wchar ? parse_wide_character_constant(buffer)
                               : parse_character_constant(buffer);
    }
    if (lex_peek(T_identifier, buffer)) {
        if (!strcmp(buffer, "__builtin_offsetof")) {
            char type_name[MAX_ID_LEN];
            char member_name[MAX_ID_LEN];
            type_t *record;
            var_t *field;
            int offset = 0;
            bool has_nested_member;

            lex_expect(T_identifier);
            lex_expect(T_open_bracket);
            if (lex_accept(T_struct) || lex_accept(T_union)) {
                lex_ident(T_identifier, type_name);
                record = find_type(type_name, 2);
            } else {
                lex_ident(T_identifier, type_name);
                record = find_type(type_name, true);
            }
            if (!is_record_type(record))
                error_at("offsetof requires a struct or union type",
                         cur_token_loc());
            lex_expect(T_comma);
            do {
                lex_ident(T_identifier, member_name);
                field = find_member(member_name, record);
                if (!field)
                    error_at("Unknown record member", cur_token_loc());
                offset += field->offset;
                offset += read_offsetof_array_subscripts(field, scope);
                record = field->type;
                has_nested_member = lex_accept(T_dot);
                if (has_nested_member &&
                    (field->ptr_level || !is_record_type(record)))
                    error_at("Nested offsetof member requires a record",
                             cur_token_loc());
            } while (has_nested_member);
            lex_expect(T_close_bracket);
            return offset;
        }
        lex_expect(T_identifier);
        constant_t *con = find_scoped_constant(buffer, scope);
        if (con)
            return con->value;
        error_at("Identifier is not an integer constant", next_token_loc());
    }
    error_at("Expected an integer constant expression", next_token_loc());
    return 0;
}

int read_const_expr(block_t *scope)
{
    opcode_t op_stack[MAX_CONST_EXPR_OPS];
    int val_stack[MAX_CONST_EXPR_OPS];
    int op_n = 0, val_n = 0;

    val_stack[val_n++] = read_const_expr_operand(scope);

    while (true) {
        opcode_t op = get_operator();

        if (op == OP_generic)
            break;

        /* The conditional binds loosest, so everything folded so far is its
         * condition, and its arms are whole constant expressions of their own.
         */
        if (op == OP_ternary) {
            while (op_n > 0) {
                val_n--;
                op_n--;
                val_stack[val_n - 1] = eval_expression_imm(
                    op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
            }
            lex_expect(T_question);
            bool saved_checking = checking_enum_constant;
            checking_enum_constant = saved_checking && val_stack[0];
            int then_val = read_const_expr(scope);
            lex_expect(T_colon);
            checking_enum_constant = saved_checking && !val_stack[0];
            int else_val = read_const_expr(scope);
            checking_enum_constant = saved_checking;
            return val_stack[0] ? then_val : else_val;
        }

        /* Everything at least as tight as the operator just read is complete,
         * so fold it before pushing.
         */
        int prio = get_operator_prio(op);
        while (op_n > 0 && get_operator_prio(op_stack[op_n - 1]) >= prio) {
            val_n--;
            op_n--;
            val_stack[val_n - 1] = eval_expression_imm(
                op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
        }
        if (op_n >= MAX_CONST_EXPR_OPS - 1)
            error_at("Constant expression nests too deeply", next_token_loc());
        op_stack[op_n++] = op;
        val_stack[val_n++] = read_const_expr_operand(scope);
    }

    while (op_n > 0) {
        val_n--;
        op_n--;
        val_stack[val_n - 1] = eval_expression_imm(
            op_stack[op_n], val_stack[val_n - 1], val_stack[val_n]);
    }
    return val_stack[0];
}

void read_inner_var_decl(var_t *vd,
                         bool anon,
                         bool is_param,
                         bool is_record_member)
{
    /* Preserve typedef pointer level - don't reset if already inherited */
    vd->init_val = 0;
    vd->has_direct_array_declarator = false;
    vd->has_direct_pointee_array_declarator = false;

    /* A plain typedef alias inherits every recorded bound, including the fourth
     * one. Fresh declarators start with no bound metadata; a derived suffix is
     * later composed against its base by the declaration path.
     */
    if (!vd->type || !vd->type->array_size)
        vd->array_dim4 = 0;
    if (is_param) {
        /* However, if the parsed variable is a function parameter, reset its
         * pointer level to zero.
         */
        vd->ptr_level = 0;
    }

    while (lex_accept(T_asterisk)) {
        vd->ptr_level++;

        /* Check for const after asterisk (e.g., int * const ptr). For now, we
         * just consume const qualifiers after pointer. Full support would
         * require tracking const-ness of the pointer itself vs the pointed-to
         * data separately.
         */
        while (true) {
            if (lex_accept(T_const)) {
                vd->is_const_pointer = true;
                if (vd->ptr_level <= 32)
                    vd->pointer_const_mask |= 1U << (vd->ptr_level - 1);
            } else if (lex_accept(T_volatile))
                vd->is_volatile = true;
            else if (lex_accept(T_restrict))
                ; /* restrict is an aliasing contract, not storage state. */
            else
                break;
        }
    }

    /* `typedef int row[2]; row *p` declares a pointer object, not an array
     * object. read_partial_var_decl() copied row's bounds into vd before the
     * declarator was known; move them to the pointer's pointee descriptor so
     * allocation loads p's value and postfix indexing still advances a row.
     */
    if (vd->ptr_level && vd->type && vd->type->array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_type(vd->type);
        fixed_array_shape_t empty_shape = {0};

        fixed_array_shape_to_pointee_var(vd, &shape);
        fixed_array_shape_to_var(vd, &empty_shape);
    }

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t *func = arena_alloc_func();
        char temp_name[MAX_VAR_LEN];
        int nested_ptr_level = 0;

        do {
            lex_expect(T_asterisk);
            nested_ptr_level++;
            while (true) {
                if (lex_accept(T_const)) {
                    vd->parenthesized_function_pointer_const = true;
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_const = true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                    vd->is_const_pointer = true;
                    if (vd->ptr_level + nested_ptr_level <= 32)
                        vd->pointer_const_mask |=
                            1U << (vd->ptr_level + nested_ptr_level - 1);
                } else if (lex_accept(T_volatile)) {
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_volatile =
                            true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                    vd->is_volatile = true;
                } else if (lex_accept(T_restrict)) {
                    vd->parenthesized_function_pointer_restrict = true;
                    if (nested_ptr_level == 2)
                        vd->parenthesized_function_pointer_outer_restrict =
                            true;
                    else
                        vd->parenthesized_function_pointer_inner_qualified =
                            true;
                } else
                    break;
            }
        } while (lex_peek(T_asterisk, NULL));
        if (lex_peek(T_identifier, NULL)) {
            lex_ident(T_identifier, temp_name);
            vd->var_name = intern_string(temp_name);
        } else if (!anon || !is_param)
            lex_ident(T_identifier, temp_name);

        /* The array suffix belongs inside the parenthesized pointer declarator
         * in `int (*callbacks[2])(int)`. It is an array whose elements are
         * function pointers, not a function returning an array. Keep the
         * representation used by ordinary arrays so indexing and storage
         * allocation can share their existing paths.
         */
        for (int dim = 0; lex_accept(T_open_square); dim++) {
            bool parameter_static_bound = false;
            bool parameter_const_bound = false;

            if (dim >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            while (lex_peek(T_static, NULL) || lex_peek(T_const, NULL) ||
                   lex_peek(T_volatile, NULL) || lex_peek(T_restrict, NULL)) {
                if (!is_param || dim)
                    error_at("array bracket qualifiers require a parameter",
                             cur_token_loc());
                if (lex_accept(T_static)) {
                    if (parameter_static_bound)
                        error_at("duplicate static array parameter qualifier",
                                 cur_token_loc());
                    parameter_static_bound = true;
                } else if (lex_accept(T_const))
                    parameter_const_bound = true;
                else if (lex_accept(T_volatile))
                    vd->is_volatile = true;
                else
                    lex_expect(T_restrict);
            }
            if (lex_peek(T_close_square, NULL)) {
                if (parameter_static_bound)
                    error_at("static array parameter needs a bound",
                             cur_token_loc());
                if (dim)
                    error_at(
                        "Only the outer function-pointer array bound may "
                        "be omitted",
                        cur_token_loc());
                vd->has_unsized_array = true;
            } else {
                int bound = read_const_expr(vd->scope);

                if (parameter_static_bound && bound <= 0)
                    error_at("static array parameter needs a positive bound",
                             cur_token_loc());
                if (bound <= 0)
                    error_at("Array size must be positive", cur_token_loc());
                if (dim == 0)
                    vd->array_size = bound;
                else {
                    if (dim == 1)
                        vd->array_dim2 = bound;
                    else if (dim == 2)
                        vd->array_dim3 = bound;
                    else if (dim == 3)
                        vd->array_dim4 = bound;
                    vd->array_size *= bound;
                }
            }
            if (parameter_const_bound && dim == 0) {
                vd->is_const_pointer = true;
                vd->pointer_const_mask |= 1U;
            }
            lex_expect(T_close_square);
        }
        lex_expect(T_close_bracket);

        /* A parenthesized pointer followed by array suffixes is a pointer to an
         * array, not a function pointer. Keep the pointee's row bounds apart
         * from the pointer object's own storage extent.
         */
        if (lex_peek(T_open_square, NULL)) {
            int dims = 0;

            /* The nested stars make this a pointer-to-array. Preserve whether
             * its array element was plain void before adding them; a typedef or
             * explicit pointer to void is a valid element.
             */
            bool void_array_element = vd->type &&
                                      vd->type->base_type == TYPE_void &&
                                      !effective_pointer_depth(vd);

            vd->ptr_level += nested_ptr_level;
            vd->pointee_array_element_ptr_level =
                vd->ptr_level - nested_ptr_level;
            vd->has_direct_pointee_array_declarator = true;
            while (lex_accept(T_open_square)) {
                int bound;

                if (dims >= 4)
                    error_at(
                        "Array declarators support at most four dimensions",
                        cur_token_loc());
                if (lex_peek(T_close_square, NULL))
                    error_at("Pointer-to-array needs a bound", cur_token_loc());
                bound = read_const_expr(vd->scope);
                if (bound <= 0)
                    error_at("Array size must be positive", cur_token_loc());
                if (dims == 0)
                    vd->pointee_array_size = bound;
                else {
                    if (dims == 1)
                        vd->pointee_array_dim2 = bound;
                    else if (dims == 2)
                        vd->pointee_array_dim3 = bound;
                    else
                        vd->pointee_array_dim4 = bound;
                    vd->pointee_array_size *= bound;
                }
                lex_expect(T_close_square);
                dims++;
            }
            if (dims && void_array_element)
                error_at("void type cannot be an array element",
                         cur_token_loc());
            return;
        }

        /* The return declaration was parsed before the parenthesized
         * declarator. Copy it into the syntax-only signature before the
         * function-pointer marker is set on vd.
         */
        memcpy(&func->return_def, vd, sizeof(var_t));
        func->returns_aggregate = is_record_type(func->return_def.type) &&
                                  !has_effective_pointer(&func->return_def);
        read_parameter_list_decl(func, true);
        vd->func_signature = func;
        vd->is_func = true;
        vd->parenthesized_function_pointer_level = nested_ptr_level;
        if (nested_ptr_level == 2) {
            /* `int (**slot)(int)` is an ordinary pointer object whose pointee
             * is the compact one-pointer callback representation. Keep that
             * outer object pointer in var_t; the final unary dereference
             * restores the callback signature before a call.
             */
            if (vd->parenthesized_function_pointer_inner_qualified)
                error_at(
                    "inner callback-slot pointer qualifiers are not yet "
                    "supported",
                    cur_token_loc());
            if (vd->parenthesized_function_pointer_restrict &&
                !vd->parenthesized_function_pointer_outer_restrict)
                error_at("callback-slot restrict typedef is not yet supported",
                         cur_token_loc());

            /* The second star is the outer slot pointer. After collapsing the
             * compact callback representation to one retained pointer level,
             * move that star's const bit from level two to level one.
             */
            if (vd->parenthesized_function_pointer_outer_const) {
                vd->pointer_const_mask &= ~2U;
                vd->pointer_const_mask |= 1U;
                vd->is_const_pointer = true;
            }
            vd->ptr_level += nested_ptr_level - 1;
            vd->pointee_func_signature = vd->func_signature;
            vd->func_signature = NULL;
            vd->is_func = false;
        }
    } else {
        /* Parameter declarations may use an abstract declarator in a prototype,
         * but a spelled identifier still has to be consumed even when callers
         * permit it to be omitted. Other anonymous type-name paths retain their
         * original no-identifier grammar.
         */
        if ((!anon || (is_param && lex_peek(T_identifier, NULL))) &&
            !lex_peek(T_colon, NULL)) {
            char temp_name[MAX_VAR_LEN];
            lex_ident(T_identifier, temp_name);
            vd->var_name = intern_string(temp_name);
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    opstack_push(vd);
                }
            }
        }
        if (!lex_peek(T_open_square, NULL) &&
            !(vd->type && vd->type->array_size)) {
            vd->array_size = 0;
            vd->array_dim2 = 0;
            vd->array_dim3 = 0;
            vd->array_dim4 = 0;
        }

        /* Every dimension multiplies into array_size, so "int matrix[3][4]"
         * becomes an array of 12 elements. The second dimension is kept
         * separately as well, because indexing needs the row length; further
         * dimensions only contribute to the total. A dimension left empty
         * contributes no size.
         */
        bool first_dim_empty = false;
        int dims = 0;

        /* Preserve the element shape before an unsized parameter array is
         * adjusted to a pointer. Arrays of void pointers remain valid.
         */
        bool void_array_element = vd->type &&
                                  vd->type->base_type == TYPE_void &&
                                  !effective_pointer_depth(vd);

        /* A block typedef's base array shape is retained in vd->type for
         * compose_block_typedef_array(). A new direct suffix contributes only
         * its own bounds; otherwise inherited inner bounds are appended twice
         * when aliases are composed.
         */
        if (parsing_block_typedef_declarator && vd->type &&
            vd->type->array_size && lex_peek(T_open_square, NULL)) {
            vd->array_size = 0;
            vd->array_dim2 = vd->array_dim3 = vd->array_dim4 = 0;
        }

        for (int dim = 0; lex_accept(T_open_square); dim++) {
            vd->has_direct_array_declarator = true;
            bool parameter_static_bound = false;
            bool parameter_const_bound = false;

            if (dim >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            while (lex_peek(T_static, NULL) || lex_peek(T_const, NULL) ||
                   lex_peek(T_volatile, NULL) || lex_peek(T_restrict, NULL)) {
                if (!is_param || dim)
                    error_at("array bracket qualifiers require a parameter",
                             cur_token_loc());
                if (lex_accept(T_static)) {
                    if (parameter_static_bound || dim)
                        error_at(
                            "static is only valid in the outermost array "
                            "parameter bound",
                            cur_token_loc());
                    parameter_static_bound = true;
                } else if (lex_accept(T_const))
                    parameter_const_bound = true;
                else if (lex_accept(T_volatile))
                    vd->is_volatile = true;
                else
                    lex_expect(T_restrict);
            }
            if (lex_peek(T_close_square, NULL)) {
                if (parameter_static_bound)
                    error_at("static array parameter needs a bound",
                             cur_token_loc());

                /* An omitted leading size is only a pointer when nothing
                 * follows it: "int a[]" is "int *", but "int a[][4]" points at
                 * rows of four and is indexed exactly like "int a[3][4]".
                 * Raising the pointer level for the latter would scale the row
                 * index by a pointer instead of by the row, and add a
                 * dereference that is not there.
                 */
                if (dim == 0)
                    first_dim_empty = true;
                else
                    vd->ptr_level++;
            } else {
                int next_dim = read_const_expr(vd->scope);

                if (parameter_static_bound && next_dim <= 0)
                    error_at("static array parameter needs a positive bound",
                             cur_token_loc());

                if (dim == 0) {
                    vd->array_size = next_dim;
                } else {
                    if (dim == 1)
                        vd->array_dim2 = next_dim;
                    else if (dim == 2)
                        vd->array_dim3 = next_dim;
                    else if (dim == 3)
                        vd->array_dim4 = next_dim;
                    if (vd->array_size > 0)
                        vd->array_size *= next_dim;
                    else
                        vd->array_size = next_dim;
                }
            }
            if (parameter_const_bound && dim == 0) {
                vd->is_const_pointer = true;
                vd->pointer_const_mask |= 1U;
            }
            lex_expect(T_close_square);
            dims++;
        }
        if (first_dim_empty && is_record_member) {
            /* A record's omitted outer bound is its flexible array member.
             * Inner dimensions still describe the complete element type, so
             * retain their product and row stride for member indexing.
             */
            vd->has_unsized_array = true;
        } else if (first_dim_empty && !is_param) {
            /* An object declarator such as `char text[] = "hi"` has an inferred
             * outer bound. This also applies when it carries known inner
             * dimensions, e.g. `int table[][4]`.
             */
            vd->has_unsized_array = true;
        } else if (first_dim_empty && dims == 1) {
            /* Only a one-dimensional parameter needs the pointer adjustment; a
             * multidimensional parameter retains its inner row stride.
             */
            vd->ptr_level++;
        }

        /* Parameter adjustment turns `void values[]` into a pointer-shaped
         * declarator, but C99 still forbids void as an array element type. The
         * pre-suffix shape distinguishes it from an array of void pointers,
         * including a pointer typedef.
         */
        if (dims && void_array_element)
            error_at("void type cannot be an array element", cur_token_loc());

        /* C99 permits an object of a flexible-record type, but not an array
         * whose elements have no fixed extent. An explicitly pointer-typed
         * element remains valid, including through a pointer typedef.
         */
        if (vd->array_size > 0 && !vd->ptr_level && !vd->type->ptr_level &&
            type_has_flexible_array_member(vd->type))
            error_at(
                "A struct with a flexible array member cannot be an array "
                "element",
                cur_token_loc());

        /* The ordinary declarator spelling `result name(parameters)` is a
         * function type just as the parenthesized pointer form above is. Keep
         * this syntax-only signature on the declaration; block typedefs can
         * then bind a direct function alias without creating an object or IR.
         */
        if ((parsing_block_typedef_declarator ||
             (strict_c99 && parsing_for_initializer_declaration)) &&
            lex_peek(T_open_bracket, NULL) && !vd->array_size &&
            !vd->has_unsized_array) {
            func_t *func = arena_alloc_func();
            bool saved_block_typedef_declarator =
                parsing_block_typedef_declarator;

            memcpy(&func->return_def, vd, sizeof(var_t));
            func->returns_aggregate = is_record_type(func->return_def.type) &&
                                      !has_effective_pointer(&func->return_def);

            /* The enclosing typedef is the only declaration entitled to use
             * this direct-function syntax. Parameter declarators continue
             * through their established parsing path.
             */
            parsing_block_typedef_declarator = false;
            read_parameter_list_decl(func, true);
            parsing_block_typedef_declarator = saved_block_typedef_declarator;
            vd->func_signature = func;
            vd->is_func = true;
            vd->is_direct_function_declarator = true;
            return;
        }

        /* An ordinary declarator can name a function-pointer typedef. Its
         * prototype belongs to the typedef's type descriptor, while the object
         * retains the existing function-pointer representation.
         */
        if (vd->ptr_level == 1 && !vd->array_size &&
            vd->type->is_direct_function_type && vd->type->func_signature) {
            /* A pointer applied directly to a function typedef is a callable
             * function-pointer object. It is not a function designator, but
             * indirect-call lowering needs its prototype.
             */
            vd->func_signature = vd->type->func_signature;
            vd->is_func = false;
        } else if (vd->ptr_level || vd->type->ptr_level || vd->array_size) {
            /* A derived declarator is not itself a callable callback object.
             * Preserve no direct-call marker until dereference/subscript
             * lowering can carry the element prototype separately.
             */
            vd->func_signature = NULL;
            vd->is_func = false;
        } else {
            vd->is_func = vd->func_signature != NULL;
        }
    }

    /* The legacy flag remains the outermost pointer qualifier for lvalue
     * writes. Intermediate qualifiers are retained in pointer_const_mask.
     */
    if (vd->ptr_level > 0 && vd->ptr_level <= 32)
        vd->is_const_pointer =
            vd->pointer_const_mask & (1U << (vd->ptr_level - 1));

    /* C99 permits void only as a function return type, a pointer target, or the
     * separately validated lone parameter-list sentinel. A by-value void
     * declarator has no object size and must not reach allocation or layout.
     */
    if (vd->type && vd->type->base_type == TYPE_void &&
        !effective_pointer_depth(vd) && !vd->is_func &&
        !lex_peek(T_open_bracket, NULL))
        error_at("void type cannot define an object", cur_token_loc());
}

/* starting next_token, need to check the type */
void read_full_var_decl(var_t *vd,
                        bool anon,
                        bool is_param,
                        bool is_record_member)
{
    char type_name[MAX_ID_LEN];

    /* Callers which have already consumed a leading qualifier leave it on the
     * declaration. Keep it separate from qualification inherited from a
     * typedef: `const int_pointer` qualifies the pointer object, while an
     * unqualified `const_int_pointer` only qualifies the pointed-to object.
     */
    bool declaration_const = vd->is_const_qualified;
    bool declaration_inline = vd->is_inline;
    bool declaration_volatile = vd->is_volatile;
    bool is_signed = false;
    bool is_unsigned = false;
    bool is_const = false;
    bool is_inline = false;
    bool is_volatile = false;
    bool is_long = false;
    bool is_long_long = false;
    type_t *leading_scalar_type = NULL;

    /* The declaration dispatcher normally leaves the base type for this routine
     * to consume. C99 also permits the modifier after that base, e.g. `short
     * unsigned value`; retain the base while consuming its following scalar
     * modifiers instead of mistaking `unsigned` for the declarator name.
     */
    if (lex_peek(T_identifier, type_name) &&
        (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
         !strcmp(type_name, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(type_name, true);
        }
    }

    /* C permits these declaration specifiers in either order. Consume the
     * scalar set as a group so `const unsigned int` and `unsigned const int`
     * follow the same path.
     */
    while (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_const, NULL) || lex_peek(T_inline, NULL) ||
           lex_peek(T_volatile, NULL) || lex_peek(T_long, NULL)) {
        if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
        } else if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
        } else if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_inline)) {
            if (is_inline || declaration_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else {
            lex_expect(T_long);
            if (is_long_long)
                error_at("too many long type specifiers", cur_token_loc());
            if (is_long)
                is_long_long = true;
            else
                is_long = true;
        }
    }
    if (is_signed && is_unsigned)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (leading_scalar_type == TY_int && lex_peek(T_identifier, type_name) &&
        !strcmp(type_name, "char"))
        error_at("int cannot be combined with char", cur_token_loc());
    bool is_enum_type = lex_accept(T_enum);
    if (is_enum_type && (is_signed || is_unsigned || is_long))
        error_at("enum type cannot be combined with integer specifiers",
                 cur_token_loc());
    bool is_record_type = lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
    bool is_union_type = lex_accept(T_union);
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (is_union_type)
        find_type_flag = 2;
    type_t *type;

    /* `signed` has the existing signed scalar semantics. C permits its `int`
     * spelling to be omitted, while `signed char` and `signed short` retain
     * their explicit base type.
     */
    if (parsing_sizeof_function_signature && lex_accept(T_float)) {
        if (is_signed || is_unsigned || is_long)
            error_at("invalid float type specifiers", cur_token_loc());
        type = TY_float;
    } else if (parsing_sizeof_function_signature && lex_accept(T_double)) {
        if (is_signed || is_unsigned || is_long_long)
            error_at("invalid double type specifiers", cur_token_loc());
        type = is_long ? TY_long_double : TY_double;
    } else if (is_enum_type) {
        lex_ident(T_identifier, type_name);
        type = find_type_tag(type_name, vd->scope);
    } else if (is_unsigned) {
        if (is_long) {
            if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
                lex_expect(T_identifier);
            type = is_long_long ? TY_ulong_long : TY_ulong;
        } else if (leading_scalar_type == TY_char) {
            type = TY_uchar;
        } else if (leading_scalar_type == TY_short) {
            if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
                lex_expect(T_identifier);
            type = TY_ushort;
        } else if (lex_peek(T_identifier, type_name) &&
                   (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
                    !strcmp(type_name, "int"))) {
            lex_expect(T_identifier);
            if (!strcmp(type_name, "char"))
                type = TY_uchar;
            else if (!strcmp(type_name, "short"))
                type = TY_ushort;
            else
                type = TY_uint;
        } else
            type = TY_uint; /* `unsigned` is `unsigned int` */
    } else if (is_long) {
        /* The current ABI gives long the same 32-bit representation as int,
         * while retaining its distinct C type and rank.
         */
        if (lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
            lex_expect(T_identifier);
        type = is_long_long ? TY_long_long : TY_long;
    } else if (is_signed && leading_scalar_type &&
               (leading_scalar_type == TY_char ||
                leading_scalar_type == TY_short)) {
        if (leading_scalar_type == TY_short &&
            lex_peek(T_identifier, type_name) && !strcmp(type_name, "int"))
            lex_expect(T_identifier);
        type = leading_scalar_type == TY_char ? TY_schar : leading_scalar_type;
    } else if (is_signed && lex_peek(T_identifier, type_name) &&
               (!strcmp(type_name, "char") || !strcmp(type_name, "short") ||
                !strcmp(type_name, "int"))) {
        lex_expect(T_identifier);
        type = !strcmp(type_name, "char")
                   ? TY_schar
                   : (!strcmp(type_name, "short") ? TY_short : TY_int);
    } else if (is_signed && find_type_flag == 1 &&
               (!lex_peek(T_identifier, type_name) ||
                (strcmp(type_name, "int") && strcmp(type_name, "char") &&
                 strcmp(type_name, "short")))) {
        type = TY_int;
    } else if (leading_scalar_type) {
        type = leading_scalar_type;
    } else {
        lex_ident(T_identifier, type_name);
        type = find_type_flag == 2 ? find_record_tag(type_name, vd->scope)
                                   : find_visible_type(type_name, vd->scope);
        if (!type && find_type_flag == 2)
            type = find_type(type_name, 2);
        if (find_type_flag == 2 &&
            (!type || !is_record_type ||
             (is_union_type ? type->base_type != TYPE_union
                            : type->base_type != TYPE_struct)))
            error_at("Unknown struct/union type", cur_token_loc());
    }

    if (!type) {
        char message[MAX_LINE_LEN];

        snprintf(message, MAX_LINE_LEN, "Could not find type %s%s",
                 find_type_flag == 2 ? "struct/union " : "", type_name);
        error_at(message, cur_token_loc());
    }

    vd->type = type;
    vd->func_signature = type->ptr_level ? NULL : type->func_signature;
    vd->pointee_func_signature = type->pointee_func_signature;
    vd->is_func = vd->func_signature != NULL;
    vd->is_const_qualified = type->is_const_qualified;
    if (type->func_signature && !type->is_direct_function_type &&
        (type->pointer_const_mask & 1U)) {
        vd->is_const_pointer = true;
        vd->pointer_const_mask |= 1U;
    }
    if (type->array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_type(type);

        fixed_array_shape_to_var(vd, &shape);
    }
    if (type->pointee_array_size) {
        fixed_array_shape_t shape = fixed_array_shape_from_pointee_type(type);

        fixed_array_shape_to_pointee_var(vd, &shape);
        vd->pointee_array_element_ptr_level =
            type->pointee_array_element_ptr_level;
    }
    if (type->ptr_level && type->ptr_level <= 32)
        vd->is_const_pointer =
            type->pointer_const_mask & (1U << (type->ptr_level - 1));

    /* A qualifier may follow the base type as well as precede it: both "const
     * int" and "int const" qualify the object. Consume it before parsing
     * pointer declarators, where a following const instead qualifies the
     * pointer itself ("int * const").
     */
    while (true) {
        if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_restrict)) {
            /* A typedef-hidden array may be qualified when its elements are
             * pointers. As for `const` and `volatile`, the qualifier then
             * applies to the element type rather than to an array object.
             */
            if (!type->ptr_level &&
                !(type->array_size && type->array_element_ptr_level &&
                  (!type->func_signature ||
                   type->array_element_pointee_func_signature)))
                error_at("restrict requires a pointer type", cur_token_loc());
        } else if (lex_accept(T_inline)) {
            if (is_inline || declaration_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else
            break;
    }
    vd->is_inline = declaration_inline || is_inline;
    vd->is_volatile =
        declaration_volatile || is_volatile || type->is_volatile_qualified;
    if (is_const || declaration_const) {
        if (type->ptr_level) {
            vd->is_const_pointer = true;
            if (type->pointee_func_signature && type->ptr_level == 1)
                vd->pointer_const_mask |= 1U;
        } else
            vd->is_const_qualified = true;
    }

    read_inner_var_decl(vd, anon, is_param, is_record_member);

    if (!parsing_sizeof_function_signature && vd->func_signature &&
        function_signature_has_floating(vd->func_signature))
        error_at("Floating point function types are not yet supported",
                 cur_token_loc());

    /* Typedef aliases preserve floating type identity for composition and
     * sizeof, but no floating value may enter the integer-only IR/ABI path.
     */
    if (vd->type && vd->type->is_floating && !parsing_sizeof_function_signature)
        error_at("Floating point types are not yet supported", cur_token_loc());

    /* A 32-bit target can carry a pointer to an eight-byte object without a
     * paired-value register representation. Keep direct wide objects, arrays,
     * parameters, returns, and function-pointer returns rejected until that
     * lowering exists, but admit declarations such as `long long *p` and
     * typedef aliases used exclusively behind a pointer.
     */
    if (PTR_SIZE < 8 && vd->type && vd->type->base_type == TYPE_long_long &&
        (!(vd->ptr_level || vd->type->ptr_level) || vd->is_func))
        error_at("long long value needs 64-bit target lowering",
                 cur_token_loc());
}

/* starting next_token, need to check the type */
void read_partial_var_decl(var_t *vd, var_t *template)
{
    UNUSED(template);
    read_inner_var_decl(vd, false, false, false);
}

void read_parameter_list_decl(func_t *func, bool anon)
{
    int vn = 0;
    lex_expect(T_open_bracket);

    char token[MAX_ID_LEN];
    /* C99's empty parameter list is deliberately not a prototype. */
    if (lex_accept(T_close_bracket))
        return;
    if (lex_peek(T_identifier, token) && !strncmp(token, "void", 4)) {
        lex_next();
        if (lex_accept(T_close_bracket)) {
            func->has_prototype = true;
            return;
        }
        func->param_defs[vn].type = TY_void;
        func->param_defs[vn].scope = func->return_def.scope;
        read_inner_var_decl(&func->param_defs[vn], anon, true, false);
        if (!func->param_defs[vn].ptr_level && !func->param_defs[vn].is_func &&
            !func->param_defs[vn].array_size)
            error_at("'void' must be the only parameter and unnamed",
                     cur_token_loc());
        vn++;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in parameter list is not permitted in C99",
                     cur_token_loc());
    }

    if (floating_type_starts_here() && !parsing_sizeof_function_signature)
        error_at("Floating point types are not yet supported", cur_token_loc());

    while ((parsing_sizeof_function_signature &&
            (lex_peek(T_float, NULL) || lex_peek(T_double, NULL))) ||
           lex_peek(T_identifier, NULL) || lex_peek(T_const, NULL) ||
           lex_peek(T_volatile, NULL) || lex_peek(T_register, NULL) ||
           lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL) || lex_peek(T_struct, NULL) ||
           lex_peek(T_union, NULL) || lex_peek(T_enum, NULL)) {
        /* Check for const qualifier */
        bool is_const = false;
        bool is_register = false;
        if (lex_accept(T_const))
            is_const = true;
        if (lex_accept(T_register))
            is_register = true;

        if (vn >= MAX_PARAMS)
            error_at("Too many parameters", cur_token_loc());
        func->param_defs[vn].scope = func->return_def.scope;
        read_full_var_decl(&func->param_defs[vn], anon, true, false);
        if (func->param_defs[vn].is_inline)
            error_at("inline specifier requires a function declarator",
                     cur_token_loc());
        func->param_defs[vn].is_const_qualified |= is_const;
        func->param_defs[vn].is_register = is_register;
        func->param_defs[vn].is_aggregate_param =
            is_record_type(func->param_defs[vn].type) &&
            !func->param_defs[vn].ptr_level;
        vn++;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in parameter list is not permitted in C99",
                     cur_token_loc());
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function. */
    if (lex_accept(T_elipsis)) {
        if (strict_c99 && vn == 0)
            error_at("ellipsis requires at least one named parameter in C99",
                     cur_token_loc());
        func->va_args = 1;
    }

    func->has_prototype = true;
    lex_expect(T_close_bracket);
}

void read_literal_param(block_t *parent, basic_block_t *bb)
{
    char combined[MAX_STRING_LEN];

    read_concatenated_string(combined);

    const int index = write_symbol(combined);

    var_t *vd = require_typed_ptr_var(parent, TY_char, true);
    vd->var_name = gen_name();
    vd->init_val = index;
    vd->is_const_qualified = true;
    vd->is_string_literal = true;
    opstack_push(vd);
    /* String literals are now in .rodata section */
    add_insn(parent, bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
}

/* A character array initialized from a string owns writable object storage;
 * unlike a char * initializer, it must not retain the string literal's
 * read-only address. `parent` selects normal local stores or the synthetic
 * global block used by static-storage arrays.
 */
void parse_string_array_init(var_t *var, block_t *parent, basic_block_t **bb)
{
    char combined[MAX_STRING_LEN];
    int len;

    read_concatenated_string(combined);

    len = strlen(combined) + 1;
    if (var->has_unsized_array) {
        var->array_size = len;
        var->has_unsized_array = false;
    } else if (len > var->array_size)
        error_at("String initializer is too long for character array",
                 cur_token_loc());

    for (int i = 0; i < len; i++) {
        var_t *value = require_var(parent);
        var_t *addr;

        value->var_name = gen_name();
        value->init_val = (unsigned char) combined[i];
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        addr = compute_element_address(parent, bb, var, i, 1);
        add_insn(parent, *bb, OP_write, NULL, addr, value, 1, NULL);
    }
}

void parse_wstring_array_init(var_t *var, block_t *parent, basic_block_t **bb)
{
    int values[MAX_STRING_LEN];
    int length;
    int units;

    length = read_wstring_units(values, MAX_STRING_LEN);

    units = length + 1;
    if (var->has_unsized_array) {
        var->array_size = units;
        var->has_unsized_array = false;
    } else if (units > var->array_size)
        error_at("Wide string initializer is too long for array",
                 cur_token_loc());

    for (int i = 0; i < var->array_size; i++) {
        var_t *value = require_typed_var(parent, var->type);
        var_t *addr;

        value->var_name = gen_name();
        value->init_val = i < length ? values[i] : 0;
        value->is_const = true;
        add_insn(parent, *bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        addr = compute_element_address(parent, bb, var, i, var->type->size);
        add_insn(parent, *bb, OP_write, NULL, addr, value, var->type->size,
                 NULL);
    }
}

bool numeric_has_unsigned_suffix(const char *token)
{
    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'u')
            return true;
    return false;
}

/* C99 permits U, L, LL, UL, ULL, LU, and LLU (case-insensitively). Keep this
 * separate from type selection: malformed suffixes must not become a valid wide
 * literal merely because their letters happen to be counted.
 */
bool numeric_suffix_is_valid(const char *suffix)
{
    int pos = 0;

    if ((suffix[pos] | 32) == 'u')
        pos++;
    if ((suffix[pos] | 32) == 'l') {
        pos++;
        if ((suffix[pos] | 32) == 'l')
            pos++;
    }
    if ((suffix[pos] | 32) == 'u')
        pos++;
    return suffix[pos] == '\0';
}

bool numeric_has_long_long_suffix(const char *token)
{
    int long_suffix_count = 0;

    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'l')
            long_suffix_count++;
    return long_suffix_count == 2;
}

int numeric_long_suffix_count(const char *token)
{
    int count = 0;

    for (int i = 0; token[i]; i++)
        if ((token[i] | 32) == 'l')
            count++;
    return count;
}

/* The file-scope constant evaluator is still word-sized. Decide from the token
 * spelling whether it must use the two-word literal path before that evaluator
 * consumes and narrows it.
 */
bool numeric_literal_needs_wide_path(const char *token)
{
    const char *digits = token;
    int count = 0;
    bool is_unsigned = numeric_has_unsigned_suffix(token);

    if (numeric_has_long_long_suffix(token))
        return true;
    if (digits[0] == '0' && (digits[1] | 32) == 'x') {
        digits += 2;
        while (isxdigit(digits[count]))
            count++;
        return count > 8;
    }
    if (digits[0] == '0' && (digits[1] | 32) == 'b') {
        digits += 2;
        while (digits[count] == '0' || digits[count] == '1')
            count++;
        return count > 32;
    }
    if (digits[0] == '0') {
        while (digits[count] >= '0' && digits[count] <= '7')
            count++;
        return count > 12 ||
               (count == 12 && strncmp(digits, "037777777777", count) > 0);
    }
    while (isdigit(digits[count]))
        count++;
    if (count > 10)
        return true;
    if (count < 10)
        return false;
    return strncmp(digits, is_unsigned ? "4294967295" : "2147483647", count) >
           0;
}

/* Some bootstrap stages still materialize the exact decimal 2^31 token as an
 * int bit pattern before the global initializer path sees it. This path was
 * selected from the original token spelling, so restore the required wide
 * candidate type before phase-2 chooses its constant-load width.
 */
void force_wide_global_literal_type(var_t *value, const char *token)
{
    /* The typed global path also handles a one-word unsigned literal such as
     * 0xffffffff: it must retain TY_uint for shifts, comparisons, and division,
     * but it is not a long-long candidate. Promoting it here made every 32-bit
     * target reject a valid C99 unsigned-int initializer merely because that
     * path was selected.
     */
    if (!numeric_literal_needs_wide_path(token))
        return;
    if (value->type->size >= 8)
        return;
    if (PTR_SIZE < 8)
        error_at("long long literal needs 64-bit target lowering",
                 cur_token_loc());
    value->type = numeric_has_unsigned_suffix(token) ||
                          (unsigned int) value->init_val_hi > 0x7fffffffU
                      ? TY_ulong_long
                      : TY_long_long;
}

/* Accumulate a 64-bit token in four 16-bit limbs. Each intermediate stays small
 * enough for the self-hosted compiler's unsigned arithmetic.
 */
bool numeric_mul_add_wide(unsigned int *hi,
                          unsigned int *lo,
                          unsigned int base,
                          unsigned int digit)
{
    unsigned int a = *lo & 0xffffU;
    unsigned int b = *lo >> 16;
    unsigned int c = *hi & 0xffffU;
    unsigned int d = *hi >> 16;
    unsigned int t;

    t = a * base + digit;
    a = t & 0xffffU;
    t = b * base + (t >> 16);
    b = t & 0xffffU;
    t = c * base + (t >> 16);
    c = t & 0xffffU;
    t = d * base + (t >> 16);
    if (t > 0xffffU)
        return false;
    *lo = (b << 16) | a;
    *hi = (t << 16) | c;
    return true;
}

/* The global-initializer fast path historically carries only an int value.
 * Route literals whose C99 candidate is unsigned through the typed path even
 * when they fit in one word: otherwise 0xffffffff is folded as -1 before a
 * right shift, comparison, or division sees its unsigned rank.
 */
bool numeric_literal_needs_typed_global_path(const char *token)
{
    unsigned int hi = 0;
    unsigned int lo = 0;
    int i = 0;
    int base = 10;
    bool is_decimal = true;

    if (numeric_has_unsigned_suffix(token))
        return true;
    if (token[0] == '0') {
        if ((token[1] | 32) == 'x') {
            i = 2;
            base = 16;
            is_decimal = false;
            while (isxdigit(token[i])) {
                char digit = token[i++];

                if (isdigit(digit))
                    digit -= '0';
                else
                    digit = (digit | 32) - 'a' + 10;
                numeric_mul_add_wide(&hi, &lo, base, digit);
            }
        } else if ((token[1] | 32) == 'b') {
            i = 2;
            base = 2;
            is_decimal = false;
            while (token[i] == '0' || token[i] == '1')
                numeric_mul_add_wide(&hi, &lo, base, token[i++] - '0');
        } else {
            base = 8;
            is_decimal = false;
            while (token[i] >= '0' && token[i] <= '7')
                numeric_mul_add_wide(&hi, &lo, base, token[i++] - '0');
        }
    }
    if (is_decimal)
        return false;
    return hi != 0 || lo > 0x7fffffffU;
}

void read_numeric_param(block_t *parent, basic_block_t *bb, bool is_neg)
{
    char token[MAX_TOKEN_LEN];
    unsigned int value = 0;
    unsigned int value_hi = 0;
    int i = 0;
    char c;
    int base = 10;
    bool is_decimal = true;
    bool has_unsigned_suffix;
    int long_suffix_count;
    bool is_long_long;

    lex_ident_n(T_numeric, token, MAX_TOKEN_LEN);
    has_unsigned_suffix = numeric_has_unsigned_suffix(token);
    long_suffix_count = numeric_long_suffix_count(token);
    is_long_long = long_suffix_count >= 2;

    if (token[0] == '-') {
        is_neg = !is_neg;
        i++;
    }
    if (token[0] == '0') {
        if ((token[1] | 32) == 'x') { /* hexdecimal */
            i = 2;
            base = 16;
            is_decimal = false;
            do {
                c = token[i++];
                if (isdigit(c))
                    c -= '0';
                else {
                    c |= 32; /* convert to lower case */
                    if (c >= 'a' && c <= 'f')
                        c = (c - 'a') + 10;
                    else
                        error_at("Invalid numeric constant", cur_token_loc());
                }

                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (isxdigit(token[i]));
        } else if ((token[1] | 32) == 'b') { /* binary */
            i = 2;
            base = 2;
            is_decimal = false;
            do {
                c = token[i++];
                if (c != '0' && c != '1')
                    error_at("Invalid binary constant", cur_token_loc());
                c -= '0';
                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (token[i] == '0' || token[i] == '1');
        } else { /* octal */
            base = 8;
            is_decimal = false;
            do {
                c = token[i++];
                if (c > '7')
                    error_at("Invalid numeric constant", cur_token_loc());
                c -= '0';
                if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                    error_at("Integer literal exceeds supported range",
                             cur_token_loc());
            } while (isdigit(token[i]));
        }
    } else {
        do {
            c = token[i++] - '0';
            if (!numeric_mul_add_wide(&value_hi, &value, base, c))
                error_at("Integer literal exceeds supported range",
                         cur_token_loc());
        } while (isdigit(token[i]));
    }

    if (!numeric_suffix_is_valid(token + i))
        error_at("Invalid integer literal suffix", cur_token_loc());

    /* Decimal constants have only signed candidates unless they carry U: C99
     * may not silently select unsigned long long for 2^63 or above. The one
     * exception is the magnitude in the standard spelling of LLONG_MIN, where
     * the separately parsed unary minus consumes exactly 2^63.
     */
    if (is_decimal && !has_unsigned_suffix &&
        (value_hi > 0x80000000U ||
         (value_hi == 0x80000000U && (value != 0 || !is_neg))))
        error_at("Decimal integer literal exceeds signed long long range",
                 cur_token_loc());

    /* C99 gives the magnitude in -2147483648 type long long. A target without
     * paired 64-bit lowering cannot hold a long long value, yet the negated
     * result is INT_MIN, which int represents exactly; typing that one spelling
     * as int keeps INT_MIN usable there instead of rejecting it.
     */
    bool narrow_int_min = PTR_SIZE < 8 && is_neg && is_decimal &&
                          !has_unsigned_suffix && !long_suffix_count &&
                          !value_hi && value == 0x80000000U;

    var_t *vd = require_var(parent);
    vd->var_name = gen_name();
    if (narrow_int_min) {
        vd->type = TY_int;
    } else if (is_long_long || value_hi ||
               (is_decimal && !has_unsigned_suffix &&
                numeric_literal_needs_wide_path(token)) ||
               (is_decimal && !has_unsigned_suffix && value > 0x80000000U)) {
        if (PTR_SIZE < 8)
            error_at("long long literal needs 64-bit target lowering",
                     cur_token_loc());
        if (has_unsigned_suffix || (!is_decimal && value_hi > 0x7fffffffU))
            vd->type = TY_ulong_long;
        else
            vd->type = TY_long_long;
    } else if (has_unsigned_suffix || (!is_decimal && value > 0x7fffffffU))
        vd->type = long_suffix_count ? TY_ulong : TY_uint;
    else if (long_suffix_count)
        vd->type = TY_long;

    /* Unary minus is applied after candidate selection. In particular,
     * `-2147483648` is a negated long-long literal on this 32-bit-long ABI,
     * rather than a nonstandard signed-int bit pattern.
     */
    if (is_neg) {
        value = 0 - value;
        value_hi = ~value_hi + (value == 0);
    }
    vd->init_val = value;
    vd->init_val_hi = value_hi;
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_char_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];

    lex_ident(T_char, literal);
    unescape_string(literal, unescaped, MAX_TOKEN_LEN);

    var_t *vd = require_typed_var(parent, TY_int);
    vd->var_name = gen_name();
    vd->init_val = parse_character_constant(literal);
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

/* The current execution wide-character representation is int. Decode the same
 * escape spelling as an ordinary character constant; a wide string needs
 * separate element-array lowering and is intentionally not accepted here.
 */
void read_wchar_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];

    lex_ident(T_wchar, literal);
    unescape_string(literal, unescaped, MAX_TOKEN_LEN);

    var_t *vd = require_typed_var(parent, TY_int);
    vd->var_name = gen_name();
    vd->init_val = parse_wide_character_constant(literal);
    vd->is_const = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_wstring_param(block_t *parent, basic_block_t *bb)
{
    int values[MAX_STRING_LEN];
    int length = read_wstring_units(values, MAX_STRING_LEN);

    var_t *vd = require_typed_ptr_var(parent, find_type("wchar_t", true), 1);
    vd->var_name = gen_name();
    vd->init_val = write_wide_symbol(values, length);
    vd->is_const_qualified = true;
    vd->is_string_literal = true;
    opstack_push(vd);
    add_insn(parent, bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
}

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb);
var_t *materialize_function_designator(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *value);
void read_func_parameters_with_sret(func_t *func,
                                    var_t *sret,
                                    block_t *parent,
                                    basic_block_t **bb)
{
    int param_num = 0;
    var_t *params[MAX_PARAMS], *param;

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }

        param = opstack_pop();
        param = materialize_function_designator(parent, bb, param);

        /* Writing past 'params' corrupts this frame, and the damage only
         * surfaces later as a wrong argument value. The check has to come
         * before the conversions below: those index func->param_defs[], a
         * MAX_PARAMS-element array embedded in func_t, so an over-long argument
         * list reads past it and dereferences a garbage type pointer -- the
         * compiler crashed instead of reporting the limit.
         */
        if (param_num >= MAX_PARAMS)
            error_at("Too many arguments in function call", cur_token_loc());

        if (func && param_num < func->num_params) {
            var_t *target = &func->param_defs[param_num];
            if (incompatible_character_pointer_conversion(param, target))
                error_at(
                    "incompatible character pointer types for function "
                    "argument",
                    cur_token_loc());
            if (incompatible_pointee_callback_conversion(param, target))
                error_at(
                    "incompatible callback slot types for function "
                    "argument",
                    cur_token_loc());
            diagnose_const_pointer_conversion(param, target);
            if (is_record_type(target->type) &&
                !has_effective_pointer(target)) {
                /* A record parameter is a by-value object. Keep that promise
                 * without teaching every backend its native aggregate ABI: give
                 * the callee an addressable caller-side copy in one
                 * pointer-sized ABI slot.
                 */
                if (!is_record_object(param))
                    error_at("Record argument required", cur_token_loc());

                var_t *copy = require_typed_var(parent, target->type);
                copy->var_name = gen_name();
                add_insn(parent, *bb, OP_allocat, copy, NULL, NULL, 0, NULL);
                emit_record_copy(parent, bb, copy, param);

                param = require_ref_var(parent, target->type, 0);
                param->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, param, copy, NULL, 0,
                         NULL);
            } else if (!has_effective_pointer(target) && !target->array_size) {
                param =
                    scalarize_array_literal(parent, bb, param, target->type);
            }
        }

        /* Handle parameter type conversion whenever the callee has a known
         * prototype. Function-pointer declarations retain one too.
         */
        if (func && param_num >= func->num_params && func->va_args) {
            /* Default promotions apply to scalar varargs, but pointer-like
             * values (including array literals) must flow through unchanged so
             * "%p" and friends see an address rather than a scalarized value.
             */
            if (is_record_type(param->type) && !has_effective_pointer(param)) {
                if (!is_record_object(param))
                    error_at("Record argument required", cur_token_loc());
                var_t *copy = require_typed_var(parent, param->type);

                copy->var_name = gen_name();
                add_insn(parent, *bb, OP_allocat, copy, NULL, NULL, 0, NULL);
                emit_record_copy(parent, bb, copy, param);
                param = require_ref_var(parent, param->type, 0);
                param->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, param, copy, NULL, 0,
                         NULL);
            } else if (!is_pointer_like_value(param))
                param = promote(parent, bb, param, TY_int, 0);
        } else if (func && param_num < func->num_params) {
            /* Only a declared parameter has a type to convert towards. Beyond
             * num_params the param_defs[] entry was never filled in, so its
             * type is NULL and resize_var() dereferenced it: passing more
             * arguments than a non-variadic function declares crashed the
             * compiler instead of compiling or diagnosing the call.
             */
            if (!is_record_type(func->param_defs[param_num].type) ||
                func->param_defs[param_num].ptr_level)
                param =
                    resize_var(parent, bb, param, &func->param_defs[param_num]);
        }

        params[param_num++] = param;
        if (lex_accept(T_comma) && strict_c99 &&
            lex_peek(T_close_bracket, NULL))
            error_at("trailing comma in function call is not permitted in C99",
                     cur_token_loc());
    }

    if (func && func->has_prototype &&
        (param_num < func->num_params ||
         (!func->va_args && param_num > func->num_params)))
        error_at(param_num < func->num_params
                     ? "Too few arguments in function call"
                     : "Too many arguments in function call",
                 cur_token_loc());

    if (sret)
        add_insn(parent, *bb, OP_push, NULL, sret, NULL, param_num + 1, NULL);
    for (int i = 0; i < param_num; i++) {
        /* The operand should keep alive before calling function. Pass the
         * number of remained parameters to allocator to extend their liveness.
         */
        add_insn(parent, *bb, OP_push, NULL, params[i], NULL, param_num - i,
                 NULL);
    }
}

void read_func_parameters(func_t *func, block_t *parent, basic_block_t **bb)
{
    read_func_parameters_with_sret(func, NULL, parent, bb);
}

void read_func_call_with_sret(func_t *func,
                              var_t *sret,
                              block_t *parent,
                              basic_block_t **bb)
{
    /* direct function call */
    read_func_parameters_with_sret(func, sret, parent, bb);

    add_insn(parent, *bb, OP_call, NULL, NULL, NULL, 0,
             func->return_def.var_name);
}

void read_func_call(func_t *func, block_t *parent, basic_block_t **bb)
{
    read_func_call_with_sret(func, NULL, parent, bb);
}

/* A call returning `row *`, where `typedef int row[2]`, carries the row shape
 * on the return declarator rather than the scalar element type. Preserve that
 * shape on OP_func_ret and consume immediate postfix subscripts in one shared
 * path for direct, indirect, and grouped calls.
 */
void copy_call_result_array_shape(var_t *result, const var_t *return_def)
{
    fixed_array_shape_t shape;

    if (!result || !return_def)
        return;
    shape = return_def->pointee_array_size
                ? fixed_array_shape_from_pointee_var(return_def)
                : fixed_array_shape_from_type(return_def->type);
    fixed_array_shape_to_pointee_var(result, &shape);
    result->pointee_array_element_ptr_level =
        return_def->pointee_array_element_ptr_level
            ? return_def->pointee_array_element_ptr_level
            : return_def->type->array_element_ptr_level;
    result->is_const_qualified =
        return_def->is_const_qualified || return_def->type->is_const_qualified;
}

void lower_call_result_array_postfix(var_t **value,
                                     block_t *parent,
                                     basic_block_t **bb)
{
    var_t *base;
    var_t *address;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;

    if (!value || !(base = *value) || !base->pointee_array_size ||
        !lex_peek(T_open_square, NULL))
        return;

    shape = fixed_array_shape_from_pointee_var(base);

    /* One subscript selects the pointed-to array; its outer bound and every
     * inner bound then consume their own postfix subscript.
     */
    dimensions = shape.rank + 1;
    opstack_pop();
    address = base;
    do {
        var_t *index;
        int stride =
            base->pointee_array_element_ptr_level ? PTR_SIZE : base->type->size;

        if (depth >= dimensions)
            error_at("Too many subscripts for function result",
                     cur_token_loc());
        if (depth == 0) {
            stride *= base->pointee_array_size;
        } else
            stride = fixed_array_shape_stride(&shape, depth - 1, stride);

        lex_expect(T_open_square);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        if (stride != 1) {
            var_t *scale = require_var(parent);
            var_t *scaled = require_var(parent);

            scale->var_name = gen_name();
            scale->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
            scaled->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, scaled, index, scale, 0, NULL);
            index = scaled;
        }
        var_t *indexed = require_typed_ptr_var(parent, base->type, 1);
        indexed->var_name = gen_name();
        add_insn(parent, *bb, OP_add, indexed, address, index, 0, NULL);
        address = indexed;
        depth++;
    } while (lex_peek(T_open_square, NULL));

    if (depth == dimensions) {
        var_t *element = require_typed_var(parent, base->type);
        opcode_t compound_op = OP_generic;
        bool assignment;

        element->ptr_level = base->pointee_array_element_ptr_level;
        element->func_signature = base->type->func_signature;
        element->is_const_qualified =
            base->is_const_qualified || base->type->is_const_qualified;
        element->var_name = gen_name();
        add_insn(parent, *bb, OP_read, element, address, NULL,
                 element->ptr_level ? PTR_SIZE : base->type->size, NULL);
        element->is_compound_literal_reference = true;
        element->compound_literal_address = address;

        assignment =
            lex_accept(T_assign) || accept_compound_assign_op(&compound_op);
        if (assignment) {
            var_t *assigned;

            if (element->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }
            assigned = opstack_pop();
            if (compound_op != OP_generic) {
                if (is_pointer_operation(compound_op, element, assigned)) {
                    handle_pointer_arithmetic(parent, bb, compound_op, element,
                                              assigned);
                    assigned = opstack_pop();
                } else {
                    var_t *current =
                        integer_promote_operand(parent, bb, element);

                    assigned = integer_promote_operand(parent, bb, assigned);
                    normalize_integer_binary_operands(parent, bb, compound_op,
                                                      &current, &assigned);
                    var_t *combined = require_var(parent);

                    combined->var_name = gen_name();
                    combined->type = integer_binary_result_type(
                        compound_op, current, assigned);
                    add_insn(parent, *bb, compound_op, combined, current,
                             assigned, 0, NULL);
                    assigned = combined;
                }
            }
            assigned = resize_var(parent, bb, assigned, element);
            add_insn(parent, *bb, OP_write, NULL, address, assigned,
                     element->ptr_level ? PTR_SIZE : base->type->size, NULL);
            *value = assigned;
            opstack_push(*value);
            return;
        }

        if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
            var_t *one;
            var_t *updated;

            if (element->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            one = require_typed_var(parent, TY_int);
            one->var_name = gen_name();
            one->init_val = 1;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            if (is_pointer_operation(op, element, one)) {
                handle_pointer_arithmetic(parent, bb, op, element, one);
                updated = opstack_pop();
            } else {
                updated = require_var(parent);
                updated->var_name = gen_name();
                updated->type = integer_binary_result_type(op, element, one);
                add_insn(parent, *bb, op, updated, element, one, 0, NULL);
            }
            updated = resize_var(parent, bb, updated, element);
            add_insn(parent, *bb, OP_write, NULL, address, updated,
                     element->ptr_level ? PTR_SIZE : base->type->size, NULL);
        }
        *value = element;
    } else {
        *value = address;
    }
    opstack_push(*value);
    var_t *callable = *value;
    if (callable->func_signature && lex_peek(T_open_bracket, NULL)) {
        func_t *signature = callable->func_signature;

        var_t *result =
            emit_indirect_call_result(callable, signature, true, parent, bb);
        *value = result;
        lower_call_result_array_postfix(value, parent, bb);
    }
}

void lower_call_result_prefix_update(var_t **value,
                                     opcode_t op,
                                     block_t *parent,
                                     basic_block_t **bb)
{
    var_t *object;
    var_t *one;
    var_t *updated;

    if (op == OP_generic)
        return;
    object = opstack_pop();
    if (!object->is_compound_literal_reference)
        error_at("Prefix update requires a scalar modifiable lvalue",
                 cur_token_loc());
    if (object->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    one = require_typed_var(parent, TY_int);
    one->var_name = gen_name();
    one->init_val = 1;
    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
    if (is_pointer_operation(op, object, one)) {
        handle_pointer_arithmetic(parent, bb, op, object, one);
        updated = opstack_pop();
    } else {
        updated = require_var(parent);
        updated->var_name = gen_name();
        updated->type = integer_binary_result_type(op, object, one);
        add_insn(parent, *bb, op, updated, object, one, 0, NULL);
    }
    updated = resize_var(parent, bb, updated, object);
    add_insn(parent, *bb, OP_write, NULL, object->compound_literal_address,
             updated, object->ptr_level ? PTR_SIZE : object->type->size, NULL);
    *value = updated;
    opstack_push(updated);
}

/* The freestanding <stddef.h> maps offsetof(type, member-designator) here. Keep
 * it unevaluated: no null pointer or temporary object is materialized.
 */
void read_builtin_offsetof(block_t *parent, basic_block_t **bb)
{
    char name[MAX_ID_LEN];
    type_t *record;
    var_t *field;
    var_t *result;
    int offset = 0;
    bool has_nested_member;

    lex_expect(T_identifier);
    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        lex_ident(T_identifier, name);
        record = find_type(name, 2);
    } else {
        lex_ident(T_identifier, name);
        record = find_type(name, true);
    }
    if (!is_record_type(record))
        error_at("offsetof requires a struct or union type", cur_token_loc());
    lex_expect(T_comma);
    do {
        lex_ident(T_identifier, name);
        field = find_member(name, record);
        if (!field)
            error_at("Unknown record member", cur_token_loc());
        offset += field->offset;
        offset += read_offsetof_array_subscripts(field, parent);
        record = field->type;
        has_nested_member = lex_accept(T_dot);
        if (has_nested_member && (field->ptr_level || !is_record_type(record)))
            error_at("Nested offsetof member requires a record",
                     cur_token_loc());
    } while (has_nested_member);
    lex_expect(T_close_bracket);
    result = require_typed_var(parent, find_type("size_t", true));
    result->var_name = gen_name();
    result->init_val = offset;
    result->is_const = true;
    opstack_push(result);
    add_insn(parent, *bb, OP_load_constant, result, NULL, NULL, 0, NULL);
}

/* Parse a C99 type name for the va_arg intrinsic. A type name has no
 * identifier, but it may have an abstract declarator: `int (*)[4]` and `int
 * (*)(int)` are pointer types, whereas `int *[4]` and `int (int)` are
 * array/function types and are not objects that va_arg may fetch. The current
 * IR only needs the base type and pointer depth for a pointer value; consume
 * the remaining abstract-declarator syntax here so type names are not
 * accidentally restricted to the spelling of a declaration.
 */
typedef struct va_arg_type {
    type_t *type;
    int ptr_level;
    bool is_array;
    bool is_function;
    func_t *func_signature;
    int pointee_array_size;
    int pointee_array_dim2;
    int pointee_array_dim3;
    int pointee_array_dim4;
    int pointee_element_size;
    int pointee_element_ptr_level;
} va_arg_type_t;

void read_builtin_va_arg_type(block_t *parent, va_arg_type_t *result)
{
    char name[MAX_ID_LEN];
    type_t *type;
    bool is_signed = false;
    bool is_unsigned = false;
    int long_count = 0;
    bool is_short = false;
    bool is_char = false;
    bool saw_base = false;

    (void) parent;
    result->ptr_level = 0;
    result->is_array = false;
    result->is_function = false;
    result->func_signature = NULL;
    result->pointee_array_size = 0;
    result->pointee_array_dim2 = 0;
    result->pointee_array_dim3 = 0;
    result->pointee_array_dim4 = 0;
    result->pointee_element_size = 0;
    result->pointee_element_ptr_level = 0;
    while (true) {
        if (lex_accept(T_const) || lex_accept(T_volatile))
            continue;
        if (lex_accept(T_signed)) {
            if (is_signed)
                error_at("duplicate signed type specifier", cur_token_loc());
            is_signed = true;
            continue;
        }
        if (lex_accept(T_unsigned)) {
            if (is_unsigned)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            is_unsigned = true;
            continue;
        }
        if (lex_accept(T_long)) {
            long_count++;
            continue;
        }
        if (lex_peek(T_identifier, name) && !strcmp(name, "short")) {
            if (is_short)
                error_at("duplicate short type specifier", cur_token_loc());
            lex_expect(T_identifier);
            is_short = true;
            continue;
        }
        if (lex_peek(T_identifier, name) &&
            (!strcmp(name, "int") || !strcmp(name, "char"))) {
            if (saw_base)
                error_at("duplicate type specifier", cur_token_loc());
            is_char = !strcmp(name, "char");
            saw_base = true;
            lex_expect(T_identifier);
            continue;
        }
        break;
    }
    if (is_signed && is_unsigned)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (long_count > 2 || (is_short && long_count))
        error_at("invalid va_arg integer type", cur_token_loc());

    if (lex_accept(T_struct) || lex_accept(T_union)) {
        if (is_signed || is_unsigned || long_count || is_short)
            error_at("record type cannot have integer specifiers",
                     cur_token_loc());
        lex_ident(T_identifier, name);
        type = find_type(name, 2);
    } else if (lex_accept(T_enum)) {
        if (is_signed || is_unsigned || long_count || is_short || saw_base)
            error_at("enum type cannot have integer specifiers",
                     cur_token_loc());
        lex_ident(T_identifier, name);
        type = find_type_tag(name, parent);
    } else {
        if (!saw_base && !is_signed && !is_unsigned && !long_count &&
            !is_short) {
            lex_ident(T_identifier, name);
            type = find_type(name, true);
            goto parsed_base_type;
        }

        if (long_count)
            type = is_unsigned ? (long_count == 2 ? TY_ulong_long : TY_ulong)
                               : (long_count == 2 ? TY_long_long : TY_long);
        else if (is_short)
            type = is_unsigned ? TY_ushort : TY_short;
        else if (is_char)
            type = is_unsigned ? TY_uchar : (is_signed ? TY_schar : TY_char);
        else
            type = is_unsigned ? TY_uint : TY_int;
    }
parsed_base_type:
    if (!type)
        error_at("Unknown va_arg type", cur_token_loc());
    while (lex_accept(T_const) || lex_accept(T_volatile))
        ;
    while (lex_accept(T_asterisk)) {
        result->ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* An array typedef is not itself an object type suitable for va_arg, but
     * `typedef int row[2]; row *` is a pointer to a row. Preserve the row
     * extent separately: type_t supplies the scalar element type while the
     * derived declarator supplies the stride for postfix indexing.
     */
    if (type->array_size) {
        type_t *element_type;

        if (type->base_type == TYPE_void)
            error_at("va_arg pointer-to-array cannot have void elements",
                     cur_token_loc());

        /* A pointer introduced by a typedef lives on type_t rather than the
         * abstract declarator's direct-star count. Both spellings make an array
         * typedef into a pointer-to-array object for va_arg.
         */
        if (!(result->ptr_level + type->ptr_level))
            error_at("va_arg array type is not an object type",
                     cur_token_loc());
        result->pointee_array_size = type->array_size;
        result->pointee_array_dim2 = type->array_dim2;
        result->pointee_array_dim3 = type->array_dim3;
        result->pointee_array_dim4 = type->array_dim4;

        /* A pointer-to-array typedef has already folded its bound into the
         * alias size. Its va_arg result still indexes scalar elements.
         */
        element_type = pointee_type_from_pointer_typedef(type);
        result->pointee_element_size = element_type->size;
        result->pointee_element_ptr_level = type->array_element_ptr_level;
    }

    /* Parenthesized abstract pointer declarators keep the pointer next to the
     * base type even when it is followed by an array or prototype suffix.
     */
    if (lex_accept(T_open_bracket)) {
        int nested_ptr = 0;
        int return_ptr_level = result->ptr_level;
        bool has_function_suffix = false;

        while (lex_accept(T_asterisk)) {
            nested_ptr++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
        if (!nested_ptr)
            error_at("va_arg type name needs an abstract pointer declarator",
                     cur_token_loc());
        result->ptr_level += nested_ptr;
        lex_expect(T_close_bracket);

        while (lex_peek(T_open_square, NULL) ||
               lex_peek(T_open_bracket, NULL)) {
            if (lex_accept(T_open_square)) {
                int dims = 0;
                int total = 0;

                if (type->base_type == TYPE_void)
                    error_at(
                        "va_arg pointer-to-array cannot have void elements",
                        cur_token_loc());

                if (nested_ptr != 1)
                    error_at(
                        "va_arg pointer-element array type is not yet "
                        "supported",
                        cur_token_loc());

                /* `int (*)[2][3]`: the parenthesized star denotes the value
                 * fetched from va_list; each suffix describes that pointer's
                 * pointee. Keep the familiar flattened-array representation
                 * used by normal declarations so the first subscript advances a
                 * whole row (or plane), not one scalar element.
                 */
                do {
                    int bound;

                    if (dims >= 4)
                        error_at(
                            "Array type names support at most four dimensions",
                            cur_token_loc());
                    if (lex_peek(T_close_square, NULL))
                        error_at("pointer-to-array type needs a bound",
                                 cur_token_loc());
                    bound = read_const_expr(parent);
                    if (bound <= 0)
                        error_at("pointer-to-array bound must be positive",
                                 cur_token_loc());
                    lex_expect(T_close_square);
                    if (!dims)
                        total = bound;
                    else {
                        total *= bound;
                        if (dims == 1)
                            result->pointee_array_dim2 = bound;
                        else if (dims == 2)
                            result->pointee_array_dim3 = bound;
                        else
                            result->pointee_array_dim4 = bound;
                    }
                    dims++;
                } while (lex_accept(T_open_square));
                result->pointee_array_size = total;
                result->pointee_element_size = type->size;
                result->pointee_element_ptr_level =
                    return_ptr_level + type->ptr_level;
            } else {
                func_t *func = arena_alloc_func();

                /* This is a function suffix on the pointed-to declarator.
                 * Preserve its prototype just as a named function-pointer
                 * declaration does, so the value returned by va_arg remains
                 * directly callable.
                 */
                func->return_def.type = type;
                func->return_def.ptr_level = return_ptr_level;
                func->returns_aggregate =
                    is_record_type(type) && !return_ptr_level;
                read_parameter_list_decl(func, true);
                result->func_signature = func;
                has_function_suffix = true;
            }
        }

        /* Stars before the parenthesized declarator belong to the function's
         * return type (`int *(*)(int)`), not to the pointer-sized function
         * pointer value.
         */
        if (has_function_suffix)
            result->ptr_level = nested_ptr;
    }

    /* A suffix after the ordinary pointer chain makes the result an array or a
     * function, not a pointer to one. Parse it so the diagnostic below is about
     * va_arg's object requirement instead of a stray closing token.
     */
    while (lex_peek(T_open_square, NULL) || lex_peek(T_open_bracket, NULL)) {
        if (lex_accept(T_open_square)) {
            result->is_array = true;
            if (!lex_peek(T_close_square, NULL))
                read_const_expr(parent);
            lex_expect(T_close_square);
        } else {
            int depth = 0;

            result->is_function = true;
            do {
                if (lex_accept(T_open_bracket))
                    depth++;
                else if (lex_accept(T_close_bracket))
                    depth--;
                else
                    lex_next();
            } while (depth > 0);
        }
    }
    result->type = type;
}

void emit_record_copy_from_address(block_t *parent,
                                   basic_block_t **bb,
                                   var_t *dest,
                                   var_t *src_addr)
{
    int size = size_var(dest);
    var_t *dest_addr = require_ref_var(parent, dest->type, 0);

    dest_addr->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, dest_addr, dest, NULL, 0, NULL);
    for (int offset = 0; offset < size;) {
        int width = size - offset >= 4 ? 4 : size - offset >= 2 ? 2 : 1;
        var_t *src_part =
            compute_element_address(parent, bb, src_addr, offset, 1);
        var_t *dest_part =
            compute_element_address(parent, bb, dest_addr, offset, 1);
        var_t *value = require_var(parent);

        value->var_name = gen_name();
        add_insn(parent, *bb, OP_read, value, src_part, NULL, width, NULL);
        add_insn(parent, *bb, OP_write, NULL, dest_part, value, width, NULL);
        offset += width;
    }
}

void read_builtin_va_arg(block_t *parent, basic_block_t **bb)
{
    va_arg_type_t requested;
    var_t *ap_addr;
    var_t *old;
    var_t *step;
    var_t *next;

    lex_expect(T_identifier);
    lex_expect(T_open_bracket);
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }
    ap_addr = opstack_pop();
    if (!has_effective_pointer(ap_addr))
        error_at("va_arg first argument must be va_list lvalue",
                 cur_token_loc());
    lex_expect(T_comma);
    read_builtin_va_arg_type(parent, &requested);
    lex_expect(T_close_bracket);
    if (requested.type->base_type == TYPE_void && !requested.ptr_level)
        error_at("va_arg cannot request void", cur_token_loc());
    if (is_record_type(requested.type) && !requested.ptr_level &&
        !requested.type->num_fields)
        error_at("va_arg cannot request incomplete record type",
                 cur_token_loc());
    if (requested.is_array || requested.is_function)
        error_at("va_arg type must be an object type", cur_token_loc());

    old = require_typed_ptr_var(parent, TY_int, 1);
    old->var_name = gen_name();
    add_insn(parent, *bb, OP_read, old, ap_addr, NULL, PTR_SIZE, NULL);
    step = require_typed_var(parent, TY_int);
    step->var_name = gen_name();

    /* This direct IR add is byte-addressed; unlike parsed pointer arithmetic it
     * does not apply the pointee-size scale itself.
     */
    step->init_val = PTR_SIZE;
    step->is_const = true;
    add_insn(parent, *bb, OP_load_constant, step, NULL, NULL, 0, NULL);
    next = require_typed_ptr_var(parent, TY_int, 1);
    next->var_name = gen_name();
    add_insn(parent, *bb, OP_add, next, old, step, 0, NULL);
    add_insn(parent, *bb, OP_write, NULL, ap_addr, next, PTR_SIZE, NULL);

    if (is_record_type(requested.type) && !requested.ptr_level) {
        var_t *source = require_ref_var(parent, requested.type, 0);
        var_t *result = require_typed_var(parent, requested.type);

        source->var_name = gen_name();
        add_insn(parent, *bb, OP_read, source, old, NULL, PTR_SIZE, NULL);
        result->var_name = gen_name();
        add_insn(parent, *bb, OP_allocat, result, NULL, NULL, 0, NULL);
        emit_record_copy_from_address(parent, bb, result, source);
        opstack_push(result);
    } else {
        var_t *result =
            require_typed_ptr_var(parent, requested.type, requested.ptr_level);

        result->var_name = gen_name();
        add_insn(parent, *bb, OP_read, result, old, NULL,
                 requested.ptr_level ? PTR_SIZE : requested.type->size, NULL);
        result->func_signature = requested.func_signature;
        opstack_push(result);

        /* A builtin result is a temporary, not a named lvalue, so it never
         * reaches read_lvalue()'s multidimensional subscript lowering. Apply
         * the derived pointee bounds here. For `int (*)[2][3]`, index zero
         * advances 6 ints, index one advances 3 ints, and the final index reads
         * the scalar selected by the address.
         */
        if (requested.pointee_array_size && lex_peek(T_open_square, NULL)) {
            int dims[4] = {
                requested.pointee_array_size, requested.pointee_array_dim2,
                requested.pointee_array_dim3, requested.pointee_array_dim4};
            int dimension_count = 1 + !!requested.pointee_array_dim2 +
                                  !!requested.pointee_array_dim3 +
                                  !!requested.pointee_array_dim4;
            int depth = 0;
            var_t *base = opstack_pop();

            while (lex_accept(T_open_square)) {
                var_t *index;
                var_t *address;
                int element_size = requested.pointee_element_ptr_level
                                       ? PTR_SIZE
                                   : requested.pointee_element_size
                                       ? requested.pointee_element_size
                                       : requested.type->size;
                int multiplier = element_size;

                /* The first subscript selects the array object itself; its
                 * elements are reached by one further subscript than there are
                 * declared array dimensions.
                 */
                if (depth > dimension_count)
                    error_at("Too many subscripts for pointer-to-array",
                             cur_token_loc());
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                index = opstack_pop();
                lex_expect(T_close_square);
                if (!depth)
                    multiplier *= dims[0];
                else
                    for (int i = depth; i < dimension_count; i++)
                        multiplier *= dims[i];
                if (multiplier != 1) {
                    var_t *scale = require_typed_var(parent, TY_int);
                    scale->var_name = gen_name();
                    scale->init_val = multiplier;
                    scale->is_const = true;
                    add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL,
                             0, NULL);
                    var_t *scaled = require_var(parent);
                    scaled->var_name = gen_name();
                    add_insn(parent, *bb, OP_mul, scaled, index, scale, 0,
                             NULL);
                    index = scaled;
                }
                address = require_typed_ptr_var(
                    parent, requested.type,
                    requested.pointee_element_ptr_level + 1);
                address->var_name = gen_name();
                add_insn(parent, *bb, OP_add, address, base, index, 0, NULL);
                depth++;
                if (depth == dimension_count + 1 &&
                    !requested.pointee_element_ptr_level &&
                    is_record_type(requested.type)) {
                    /* A record element is an object: copy it whole rather than
                     * read one scalar of the record's size, which drops bytes
                     * and is a width no narrow backend can load.
                     */
                    var_t *value = require_typed_var(parent, requested.type);

                    value->var_name = gen_name();
                    add_insn(parent, *bb, OP_allocat, value, NULL, NULL, 0,
                             NULL);
                    emit_record_copy_from_address(parent, bb, value, address);
                    base = value;
                } else if (depth == dimension_count + 1) {
                    var_t *value = require_typed_ptr_var(
                        parent, requested.type,
                        requested.pointee_element_ptr_level);
                    value->var_name = gen_name();
                    add_insn(parent, *bb, OP_read, value, address, NULL,
                             element_size, NULL);
                    base = value;
                } else
                    base = address;
            }
            opstack_push(base);
        }

        if (requested.func_signature && lex_peek(T_open_bracket, NULL)) {
            result = emit_indirect_call_result(result, requested.func_signature,
                                               true, parent, bb);
        }
    }
}

/* Function-pointer prototypes are stored as opaque data in var_t so defs.h
 * remains parseable before func_t is complete.
 */
func_t *get_func_signature(var_t *var)
{
    if (!var)
        return NULL;
    return var->func_signature;
}

/* A function-pointer object is represented as an is_func declaration, while a
 * value read from that object is an ordinary pointer-sized SSA value carrying
 * the declaration's prototype. Keeping the two distinct matters for calls: the
 * former must be addressed and loaded; the latter can be passed to the
 * indirect-call instruction directly.
 */
var_t *load_function_pointer_object(block_t *parent,
                                    basic_block_t **bb,
                                    var_t *object)
{
    /* SSA versions of parameters already contain the incoming pointer value.
     * Preserve it through an ordinary pointer temporary: is_func denotes a
     * storage object, whereas the indirect-call backend expects a value.
     */
    if (parent && parent->func && !object->address_taken) {
        for (int i = 0; i < parent->func->num_params; i++) {
            var_t *param = &parent->func->param_defs[i];

            if (object == param || object->base == param) {
                func_t *param_signature = get_func_signature(object);
                var_t *value = require_typed_ptr_var(
                    parent, param_signature->return_def.type, 1);

                value->var_name = gen_name();
                value->func_signature = param_signature;
                add_insn(parent, *bb, OP_assign, value, object, NULL, 0, NULL);
                return value;
            }
        }
    }

    var_t *address = require_ref_var(parent, object->type, object->ptr_level);
    func_t *signature = get_func_signature(object);
    var_t *target = require_typed_ptr_var(
        parent, signature ? signature->return_def.type : object->type, 1);

    address->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, address, object, NULL, 0, NULL);
    target->var_name = gen_name();
    target->func_signature = object->func_signature;
    target->func_target = object->func_target;
    target->func_target_invalid = object->func_target_invalid;
    add_insn(parent, *bb, OP_read, target, address, NULL, PTR_SIZE, NULL);
    return target;
}

/* C99 function designators decay to a pointer value in an argument expression.
 * A raw symbol deliberately has no storage or defining IR so global
 * initializers can emit a relocation; feeding it straight to OP_push makes
 * register allocation reload an unallocated local. Reuse the established
 * temporary-object path, which lowers the symbol through OP_address_of_func and
 * then reads the resulting pointer-sized value.
 */
var_t *materialize_function_designator(block_t *parent,
                                       basic_block_t **bb,
                                       var_t *value)
{
    func_t *func;
    var_t *object;

    if (!value || !value->is_func)
        return value;
    if (find_var(value->var_name, parent) == value)
        return load_function_pointer_object(parent, bb, value);
    func = find_func(value->var_name);
    if (!func)
        return value;

    object = require_typed_ptr_var(parent, func->return_def.type,
                                   func->return_def.ptr_level);
    object->var_name = gen_name();
    object->is_func = true;
    object->func_signature = func;
    object->func_target = func;
    add_insn(parent, *bb, OP_allocat, object, NULL, NULL, 0, NULL);
    emit_object_assignment(parent, bb, object, value);
    return load_function_pointer_object(parent, bb, object);
}

void read_indirect_call_with_sret(var_t *callee,
                                  func_t *signature,
                                  var_t *sret,
                                  block_t *parent,
                                  basic_block_t **bb)
{
    /* A function pointer carries its parsed prototype on the declaration. This
     * makes indirect calls obey the same record-by-value lowering and scalar
     * conversions as a direct call. Legacy/unprototyped pointers keep the old
     * generic behaviour. Keep the evaluated target as the OP_indirect source:
     * the call's liveness then protects it while ABI argument staging reuses
     * the argument registers.
     */
    var_t *target = opstack_pop();

    UNUSED(callee);

    read_func_parameters_with_sret(signature, sret, parent, bb);

    add_insn(parent, *bb, OP_indirect, NULL, target, NULL, 0, NULL);
}

void read_indirect_call(var_t *callee, block_t *parent, basic_block_t **bb)
{
    read_indirect_call_with_sret(callee, get_func_signature(callee), NULL,
                                 parent, bb);
}

/* Aggregate calls write their value to caller-owned storage. The object is a
 * temporary expression value, not a modifiable lvalue; callers only receive it
 * on the operand stack after the call has completed.
 */
var_t *prepare_aggregate_call_result(block_t *parent,
                                     basic_block_t **bb,
                                     func_t *signature,
                                     var_t **sret)
{
    var_t *result = require_typed_var(parent, signature->return_def.type);
    var_t *destination;

    result->var_name = gen_name();
    add_insn(parent, *bb, OP_allocat, result, NULL, NULL, 0, NULL);

    destination = require_ref_var(parent, signature->return_def.type, 0);
    destination->var_name = gen_name();
    add_insn(parent, *bb, OP_address_of, destination, result, NULL, 0, NULL);
    *sret = destination;
    return result;
}

/* Keep the ABI-only aggregate destination coupled to call emission. Call sites
 * that merely discard an aggregate result still must provide it.
 */
var_t *emit_direct_call_result(func_t *func,
                               bool want_value,
                               block_t *parent,
                               basic_block_t **bb)
{
    func_t *returned_signature = func->return_def.type->func_signature;
    var_t *result = NULL;

    if (func->returns_aggregate) {
        var_t *sret;
        func->aggregate_call_used = true;
        result = prepare_aggregate_call_result(parent, bb, func, &sret);
        read_func_call_with_sret(func, sret, parent, bb);
    } else {
        read_func_call(func, parent, bb);
        if (want_value) {
            result = require_typed_ptr_var(
                parent,
                returned_signature ? returned_signature->return_def.type
                                   : func->return_def.type,
                returned_signature ? 1 : func->return_def.ptr_level);
            result->var_name = gen_name();
            result->func_signature = returned_signature;
            copy_call_result_array_shape(result, &func->return_def);
            add_insn(parent, *bb, OP_func_ret, result, NULL, NULL, 0, NULL);
        }
    }
    if (want_value)
        opstack_push(result);
    return result;
}

var_t *emit_indirect_call_result(var_t *callee,
                                 func_t *signature,
                                 bool want_value,
                                 block_t *parent,
                                 basic_block_t **bb)
{
    var_t *result = NULL;

    if (signature && signature->returns_aggregate) {
        var_t *sret;
        func_t *target = callee ? callee->func_target : NULL;

        if (callee->func_target_invalid || !target || !target->bbs ||
            !target->returns_aggregate)
            error_at(
                "aggregate-return indirect call requires a shecc-defined "
                "target",
                cur_token_loc());
        result = prepare_aggregate_call_result(parent, bb, signature, &sret);
        read_indirect_call_with_sret(callee, signature, sret, parent, bb);
    } else {
        read_indirect_call(callee, parent, bb);
        if (want_value && signature) {
            result = require_typed_ptr_var(parent, signature->return_def.type,
                                           signature->return_def.ptr_level);
            result->var_name = gen_name();
            result->func_signature = signature->return_def.type->func_signature;
            copy_call_result_array_shape(result, &signature->return_def);
            add_insn(parent, *bb, OP_func_ret, result, NULL, NULL, 0, NULL);
        }
    }
    if (want_value)
        opstack_push(result);
    return result;
}

var_t *bitfield_constant(block_t *parent, basic_block_t **bb, unsigned value)
{
    var_t *result = require_var(parent);
    result->var_name = gen_name();
    result->init_val = (int) value;
    result->is_const = true;
    result->type = TY_uint;
    add_insn(parent, *bb, OP_load_constant, result, NULL, NULL, 0, NULL);
    return result;
}

var_t *bitfield_binary(block_t *parent,
                       basic_block_t **bb,
                       opcode_t op,
                       var_t *left,
                       var_t *right,
                       type_t *type)
{
    var_t *result = require_var(parent);
    result->var_name = gen_name();
    result->type = type;
    add_insn(parent, *bb, op, result, left, right, 0, NULL);
    return result;
}

unsigned bitfield_mask(const var_t *field)
{
    return field->bit_width == 32 ? ~0U : (1U << field->bit_width) - 1;
}

var_t *read_bitfield_value(block_t *parent,
                           basic_block_t **bb,
                           var_t *address,
                           const var_t *field)
{
    var_t *value = require_var(parent);
    value->var_name = gen_name();
    value->type = TY_uint;
    add_insn(parent, *bb, OP_read, value, address, NULL,
             field->bit_storage_size, NULL);
    if (field->bit_offset)
        value = bitfield_binary(
            parent, bb, OP_rshift, value,
            bitfield_constant(parent, bb, field->bit_offset), TY_uint);
    if (field->bit_width != 32)
        value = bitfield_binary(
            parent, bb, OP_bit_and, value,
            bitfield_constant(parent, bb, bitfield_mask(field)), TY_uint);
    if (!is_bool_type(field->type) && !field->type->is_unsigned &&
        field->bit_width != 32) {
        unsigned sign_bit = 1U << (field->bit_width - 1);

        /* Avoid depending on a target's right-shift instruction being
         * arithmetic: (x ^ sign) - sign is sign extension in pure integer
         * arithmetic for every width below the allocation unit.
         */
        value =
            bitfield_binary(parent, bb, OP_bit_xor, value,
                            bitfield_constant(parent, bb, sign_bit), TY_int);
        value =
            bitfield_binary(parent, bb, OP_sub, value,
                            bitfield_constant(parent, bb, sign_bit), TY_int);
    }
    if (is_bool_type(field->type))
        value = normalize_bool(parent, bb, value);

    /* C99's integer promotions apply to bit-fields too. A narrow unsigned int
     * bit-field fits in int on the supported targets, so retain its extracted
     * value but mark it as int before ordinary expression lowering.
     */
    value->type = field->type->is_unsigned &&
                          field->bit_width < field->bit_storage_size * 8
                      ? TY_int
                      : field->type;

    /* Retain this constraint-only provenance until an operator creates a new
     * expression value. `sizeof` must reject a direct bit-field operand.
     */
    value->is_bitfield = true;
    return value;
}

void write_bitfield_value(block_t *parent,
                          basic_block_t **bb,
                          var_t *address,
                          var_t *value,
                          const var_t *field)
{
    if (is_bool_type(field->type))
        value = normalize_bool(parent, bb, value);
    var_t *old = require_var(parent);
    old->var_name = gen_name();
    old->type = TY_uint;
    add_insn(parent, *bb, OP_read, old, address, NULL, field->bit_storage_size,
             NULL);
    value = bitfield_binary(parent, bb, OP_bit_and, value,
                            bitfield_constant(parent, bb, bitfield_mask(field)),
                            TY_uint);
    if (field->bit_offset)
        value = bitfield_binary(
            parent, bb, OP_lshift, value,
            bitfield_constant(parent, bb, field->bit_offset), TY_uint);
    unsigned mask = bitfield_mask(field) << field->bit_offset;
    var_t *clear = require_var(parent);
    clear->var_name = gen_name();
    clear->type = TY_uint;
    add_insn(parent, *bb, OP_bit_not, clear,
             bitfield_constant(parent, bb, mask), NULL, 0, NULL);
    old = bitfield_binary(parent, bb, OP_bit_and, old, clear, TY_uint);
    value = bitfield_binary(parent, bb, OP_bit_or, old, value, TY_uint);
    add_insn(parent, *bb, OP_write, NULL, address, value,
             field->bit_storage_size, NULL);
}

void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t op,
                 bool allow_ptr_arith);

/* Maintain a stack of expression values and operators, depending on next
 * operators' priority. Either apply it or operator on stack first.
 */
void handle_address_of_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_VAR_LEN];
    lvalue_t lvalue;
    var_t *vd, *rs1;

    if (!lex_peek(T_identifier, token))
        error_at("Expected an identifier", next_token_loc());
    if (!strcmp(token, "__func__")) {
        if (!parent->func || !parent->func->return_def.var_name[0])
            error_at("__func__ is only defined inside a function",
                     next_token_loc());
        lex_expect(T_identifier);
        vd = require_typed_ptr_var(parent, TY_char, 1);
        vd->var_name = gen_name();
        vd->init_val = write_symbol(parent->func->return_def.var_name);
        vd->is_string_literal = true;
        vd->is_func_name_array_address = true;
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_rodata_address, vd, NULL, NULL, 0, NULL);
        return;
    }

    /* A function designator already converts to its address. Preserve the
     * symbol instead of sending it through lvalue lookup, which only knows
     * objects and previously made `&function` fail outside file-scope scalar
     * initializers.
     */
    func_t *addressed_func = find_visible_func(token, parent);
    if (addressed_func) {
        if (parent->func && parent->func->is_inline &&
            !parent->func->is_static && addressed_func->is_static)
            error_at(
                "external inline definition references internal-linkage "
                "function",
                next_token_loc());
        lex_expect(T_identifier);
        vd = require_func_symbol_var(parent);
        vd->is_func = true;
        vd->var_name = intern_string(token);
        opstack_push(vd);
        return;
    }
    var_t *var = find_var(token, parent);
    if (var && var->is_register)
        error_at("cannot take address of register object", next_token_loc());
    read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);

    if (is_bitfield(lvalue.decl))
        error_at("cannot take address of bit-field", next_token_loc());

    if (!lvalue.is_reference) {
        rs1 = opstack_pop();
        vd = require_ref_var(parent, lvalue.type, lvalue.ptr_level);
        vd->var_name = gen_name();

        /* Address-of moves a pointer object's qualification one level inward:
         * `&p`, for `int * const p`, is `int * const *`, not `const int **`.
         */
        vd->is_const_qualified = lvalue.decl ? lvalue.decl->is_const_qualified
                                             : lvalue.is_const_qualified;
        vd->pointer_const_mask = lvalue.pointer_const_mask;
        vd->pointee_func_signature =
            lvalue.decl ? (lvalue.decl->pointee_func_signature
                               ? lvalue.decl->pointee_func_signature
                               : lvalue.decl->func_signature)
                        : NULL;
        opstack_push(vd);
        if (lvalue.decl && is_array_declarator(lvalue.decl)) {
            /* An array expression already denotes its backing address in the
             * IR. Taking OP_address_of of its variable would instead point at
             * the compiler's local array-base slot, so `&row` passed a pointer
             * to that slot rather than a pointer to row. Keep the same runtime
             * address while adding the source-level pointer indirection
             * required by the address-of operator.
             */
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
        } else
            add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0, NULL);
    }
}

/* A scalar pointer-to-array dereference designates the addressed row; it does
 * not load a scalar object. Retain that row as an array descriptor so a
 * following grouped postfix subscript can use the ordinary array path.
 */
static bool lower_scalar_pointee_array_dereference(var_t *source,
                                                   block_t *parent,
                                                   basic_block_t **bb)
{
    type_t *element_type;
    var_t *row;

    if (!source || !source->type || !source->pointee_array_size ||
        source->pointee_array_element_ptr_level ||
        effective_pointer_depth(source) != 1)
        return false;

    element_type = source->type->pointee_array_element_type
                       ? source->type->pointee_array_element_type
                       : source->type;
    row = require_var(parent);
    row->type = element_type;
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_var(row, &shape);
    row->is_const_qualified =
        source->is_const_qualified || element_type->is_const_qualified;
    row->var_name = gen_name();
    add_insn(parent, *bb, OP_assign, row, source, NULL, 0, NULL);
    opstack_push(row);
    return true;
}

static void copy_pointee_array_shape(var_t *destination, const var_t *source)
{
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(source);

    fixed_array_shape_to_pointee_var(destination, &shape);
    destination->pointee_array_element_ptr_level =
        source->pointee_array_element_ptr_level;
}

static bool is_scalar_pointee_array_pointer(const var_t *var)
{
    return var && var->type && var->pointee_array_size > 0 &&
           !var->pointee_array_element_ptr_level &&
           effective_pointer_depth(var) == 1;
}

static int scalar_pointee_array_row_stride(const var_t *var)
{
    type_t *element_type = var->type->pointee_array_element_type
                               ? var->type->pointee_array_element_type
                               : var->type;

    return var->pointee_array_size * element_type->size;
}

/* Byte extent remaining after direct-array subscript depth. This is shared by
 * ordinary postfix indexing and the bounded grouped pointer-to-array path.
 */
static int fixed_array_shape_stride(const fixed_array_shape_t *shape,
                                    int subscript_depth,
                                    int element_size)
{
    int stride = element_size;

    for (int i = subscript_depth + 1; i < shape->rank; i++)
        stride *= shape->bounds[i];
    return stride;
}

static bool fixed_array_shape_drop_outer(fixed_array_shape_t *shape)
{
    if (!shape->rank)
        return false;
    for (int i = 1; i < shape->rank; i++)
        shape->bounds[i - 1] = shape->bounds[i];
    shape->rank--;
    return true;
}

static int fixed_array_subscript_stride(const var_t *var,
                                        int subscript_depth,
                                        int element_size)
{
    fixed_array_shape_t shape = fixed_array_shape_from_var(var);

    return fixed_array_shape_stride(&shape, subscript_depth, element_size);
}

static int fixed_pointee_array_subscript_stride(const var_t *var,
                                                int subscript_depth,
                                                int element_size)
{
    fixed_array_shape_t shape = fixed_array_shape_from_pointee_var(var);

    if (subscript_depth == 0)
        return var->pointee_array_size * element_size;
    return fixed_array_shape_stride(&shape, subscript_depth - 1, element_size);
}

static int fixed_array_decay_stride(const var_t *var)
{
    return fixed_array_subscript_stride(var, 0, var->type->size);
}

static bool scalar_pointee_array_shapes_compatible(const var_t *left,
                                                   const var_t *right)
{
    type_t *left_element;
    type_t *right_element;

    if (!is_scalar_pointee_array_pointer(left) ||
        !is_scalar_pointee_array_pointer(right))
        return false;
    left_element = left->type->pointee_array_element_type
                       ? left->type->pointee_array_element_type
                       : left->type;
    right_element = right->type->pointee_array_element_type
                        ? right->type->pointee_array_element_type
                        : right->type;
    return left->pointee_array_size == right->pointee_array_size &&
           left->pointee_array_dim2 == right->pointee_array_dim2 &&
           left->pointee_array_dim3 == right->pointee_array_dim3 &&
           left->pointee_array_dim4 == right->pointee_array_dim4 &&
           left->pointee_array_element_ptr_level ==
               right->pointee_array_element_ptr_level &&
           compatible_decl_type(left_element, right_element);
}

void handle_single_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    if (lex_peek(T_open_bracket, NULL)) {
        /* Handle general expression dereference: *(expr) */
        lex_expect(T_open_bracket);
        read_expr(parent, bb);
        lex_expect(T_close_bracket);

        rs1 = opstack_pop();

        if (rs1->is_func_name_array_address) {
            /* The address of an array has the same runtime value as its first
             * element. Dereferencing it restores the array, which immediately
             * decays back to char * in this expression context.
             */
            vd = require_typed_ptr_var(parent, TY_char, 1);
            vd->var_name = gen_name();
            vd->is_string_literal = true;
            opstack_push(vd);
            add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            return;
        }

        if (lower_scalar_pointee_array_dereference(rs1, parent, bb))
            return;

        /* For pointer dereference, we need to determine the target type and
         * size. Since we do not have full type tracking in expressions, use
         * defaults
         */
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        /* require_deref_var() takes the *source* pointer level and returns a
         * variable one level shallower. Passing the already-decremented value
         * dropped two levels per dereference, so "**(q + 0)" on an int** ended
         * up reading an int-sized word where a pointer was stored. The sizes
         * coincide on the 32-bit targets, which is why it only surfaces here.
         */
        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        if (deref_ptr > 0)
            sz = PTR_SIZE;
        else
            sz = deref_type->size;
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

        /* Pointer arithmetic can produce a pointer to a callback typedef. The
         * actual dereference consumes that object-pointer level and yields the
         * pointer-valued callback result for a following postfix call.
         */
        if (deref_type && deref_type->func_signature &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = deref_type->func_signature;
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
        /* A prefix update is itself a unary expression, so `*++pointer` and
         * `*--pointer` dereference its updated pointer result just like
         * `*(pointer + 1)` dereferences a parenthesized operand.
         */
        read_expr_operand(parent, bb);
        rs1 = opstack_pop();
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        sz = deref_ptr > 0 ? PTR_SIZE
                           : pointer_typedef_pointee_size(
                                 deref_type,
                                 pointee_type_from_pointer_typedef(deref_type));
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
        if (deref_type && deref_type->func_signature &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = deref_type->func_signature;
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else {
        /* Handle simple identifier dereference: *var */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());

        /* Builtins are expression operands, not objects in the local symbol
         * table. Keep the identifier lvalue path below for `*pointer`
         * assignments, but let a pointer-valued builtin such as `*va_arg(...)`
         * use the same rvalue dereference lowering as `*(expression)`.
         */
        if (!strcmp(token, "__builtin_va_arg")) {
            type_t *deref_type;
            int deref_ptr;

            read_expr_operand(parent, bb);
            rs1 = opstack_pop();
            deref_type = rs1->type ? rs1->type : TY_int;
            deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;
            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            sz = deref_ptr > 0 ? PTR_SIZE : deref_type->size;
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
            return;
        }
        var_t *var = find_var(token, parent);

        /* A raw function name is a function designator, not an object lvalue.
         * Dereferencing it is a no-op in C99 (`*f` is another designator), so
         * route it through the same symbol representation used by ordinary
         * direct calls instead of asking read_lvalue() to find object storage.
         */
        if (!var && find_visible_func(token, parent)) {
            lex_expect(T_identifier);
            rs1 = require_func_symbol_var(parent);
            rs1->var_name = intern_string(token);
            rs1->is_func = true;
            opstack_push(rs1);
            return;
        }
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic, false);

        rs1 = opstack_pop();
        if (var->is_func) {
            /* C99 6.3.2.1 makes a function-pointer dereference a function
             * designator. The pointer object itself therefore needs one load,
             * not a dereference of its return type.
             */
            opstack_push(load_function_pointer_object(parent, bb, rs1));
            return;
        }
        if (lower_scalar_pointee_array_dereference(rs1, parent, bb))
            return;

        /* A member function-pointer typedef was already loaded by
         * read_lvalue(). Unary `*` turns that pointer into a function
         * designator; it does not read from the code address.
         */
        if (rs1->func_signature && !effective_pointer_depth(rs1)) {
            rs1->ptr_level = 1;
            opstack_push(rs1);
            return;
        }

        /* `read_lvalue()` may have resolved a member expression. Derive the
         * final indirection from its evaluated value rather than from the
         * initial record object, so `*record.pointer` dereferences the member
         * pointer instead of attempting to dereference the record itself.
         */
        type_t *deref_type = rs1->type ? rs1->type : TY_int;
        int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

        vd = require_deref_var(parent, deref_type, rs1->ptr_level);
        sz = deref_ptr > 0 ? PTR_SIZE : deref_type->size;
        vd->var_name = gen_name();
        vd->is_const_qualified = rs1->is_const_qualified;
        vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

        /* `*slot` is the function-pointer value when slot is a pointer to a
         * callback typedef. The outer declarator's signature was deliberately
         * cleared to prevent the invalid `slot(...)` form, so restore the
         * element signature only after the real dereference has occurred.
         */
        if ((rs1->func_signature || rs1->pointee_func_signature ||
             (deref_type && deref_type->func_signature)) &&
            effective_pointer_depth(rs1) == 1) {
            vd->func_signature = rs1->func_signature
                                     ? rs1->func_signature
                                     : (rs1->pointee_func_signature
                                            ? rs1->pointee_func_signature
                                            : deref_type->func_signature);
            vd->ptr_level = 1;
            sz = PTR_SIZE;
        }
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    }
}

/* Scan ahead for an assignment operator at the top level of the statement that
 * starts at the current token, stopping at its terminating semicolon.
 *
 * A statement beginning with '*' is either a store through a pointer or a plain
 * expression, and the two need opposite treatment of the leading asterisk.
 * Deciding by looking at tokens keeps the choice free of side effects: by the
 * time an expression has been parsed, its instructions have already been
 * emitted and there is no way back.
 */
bool stmt_starts_assignment(void)
{
    int depth = 0;

    for (token_t *t = cur_token->next; t; t = t->next) {
        switch (t->kind) {
        case T_open_bracket:
        case T_open_square:
            depth++;
            break;
        case T_close_bracket:
        case T_close_square:
            depth--;
            break;
        case T_semicolon:
        case T_open_curly:
        case T_close_curly:
        case T_eof:
            return false;
        case T_assign:
        case T_pluseq:
        case T_minuseq:
        case T_asteriskeq:
        case T_divideeq:
        case T_modeq:
        case T_lshifteq:
        case T_rshifteq:
        case T_andeq:
        case T_oreq:
        case T_xoreq:
            if (depth == 0)
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

void handle_multiple_dereference(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    int sz;

    /* Handle consecutive asterisks for multiple dereference: **pp, ***ppp, and
     * the parenthesized ***(expr) form.
     */
    int deref_count = 1; /* We already consumed one asterisk */
    while (lex_accept(T_asterisk))
        deref_count++;

    /* Check if we have a parenthesized expression or simple identifier */
    if (lex_peek(T_open_bracket, NULL)) {
        /* Handle ***(expr) case */
        lex_expect(T_open_bracket);
        read_expr(parent, bb);
        lex_expect(T_close_bracket);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();
            /* For expression dereference, use default type info */
            type_t *deref_type = rs1->type ? rs1->type : TY_int;
            int deref_ptr = rs1->ptr_level > 0 ? rs1->ptr_level - 1 : 0;

            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            if (deref_ptr > 0)
                sz = PTR_SIZE;
            else
                sz = deref_type->size;
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

            /* A parenthesized pointer-arithmetic result can designate a
             * callback object just like `*slot`. Restore its prototype only
             * after consuming exactly that final object-pointer indirection.
             */
            if (deref_type && deref_type->func_signature &&
                effective_pointer_depth(rs1) == 1) {
                vd->func_signature = deref_type->func_signature;
                vd->ptr_level = 1;
                sz = PTR_SIZE;
            }
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    } else {
        /* Handle **pp, ***ppp case with simple identifier */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic, false);

        /* Apply dereferences one by one */
        for (int i = 0; i < deref_count; i++) {
            rs1 = opstack_pop();

            /* A member lvalue has already been read into rs1. Each unary
             * asterisk must consume that evaluated pointer, not re-derive its
             * type from the initial record identifier.
             */
            type_t *deref_type = rs1->type ? rs1->type : TY_int;
            int deref_ptr = rs1->ptr_level + deref_type->ptr_level - 1;

            vd = require_deref_var(parent, deref_type, rs1->ptr_level);
            sz = deref_ptr > 0
                     ? PTR_SIZE
                     : pointer_typedef_pointee_size(
                           deref_type,
                           pointee_type_from_pointer_typedef(deref_type));
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = dereferenced_pointer_const_mask(rs1);
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));

            /* Only the final dereference of `**slots` (or deeper spelling)
             * reaches the callback object. Earlier reads still produce a
             * pointer-to-callback and must not be callable.
             */
            if (i + 1 == deref_count &&
                (rs1->func_signature ||
                 (deref_type && deref_type->func_signature)) &&
                effective_pointer_depth(rs1) == 1) {
                vd->func_signature = rs1->func_signature
                                         ? rs1->func_signature
                                         : deref_type->func_signature;
                vd->ptr_level = 1;
                sz = PTR_SIZE;
            }
            opstack_push(vd);
            add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
        }
    }
}

void read_expr_operand(block_t *parent, basic_block_t **bb);

/* Consume one adjacent-string-literal sequence and return the size of its C99
 * array object, including the terminating null byte. This is deliberately
 * independent of expression lowering: a string literal decays in an ordinary
 * expression, but not when it is the operand of sizeof.
 */
int read_sizeof_string_literal(void)
{
    char literal[MAX_STRING_LEN];
    char unescaped[MAX_STRING_LEN];
    int size = 1;

    do {
        lex_ident(T_string, literal);
        unescape_string(literal, unescaped, MAX_STRING_LEN);
        size += strlen(unescaped);
    } while (lex_peek(T_string, NULL));

    return size;
}

int read_sizeof_wstring_literal(void)
{
    int values[MAX_STRING_LEN];
    type_t *wide_type = find_type("wchar_t", true);
    return (read_wstring_units(values, MAX_STRING_LEN) + 1) * wide_type->size;
}

int sizeof_array_object(const var_t *array)
{
    int element_size = array->type->size;

    if (array->ptr_level || array->type->ptr_level || array->is_func)
        element_size = PTR_SIZE;
    return array->array_size * element_size;
}

/* What a type-only walk over a sizeof operand has seen. */
typedef struct {
    int members;       /* record member selections */
    int subscripts;    /* array subscripts */
    bool element_leaf; /* the last subscript selected a non-array element */
    bool addressed;    /* the operand so far is "&array" */
} sizeof_walk_t;

/* A sizeof operand needs the declared type of an lvalue, before ordinary
 * expression lowering decays an array or evaluates a postfix operand. Keep this
 * descriptor as a copy of the declaration metadata: no IR value is ever
 * constructed while walking it.
 */
static bool scan_sizeof_postfix_operand(block_t *scope,
                                        token_t **cursor,
                                        var_t *object,
                                        sizeof_walk_t *walk)
{
    token_t *token = *cursor;

    if (!token)
        return false;
    if (token->kind == T_asterisk) {
        bool addressed;
        int depth;

        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk))
            return false;
        addressed = walk->addressed;
        depth = effective_pointer_depth(object);
        if (depth <= 0)
            return false;
        if (object->pointee_array_size > 0 &&
            depth == object->pointee_array_element_ptr_level + 1) {
            /* A pointer to an array designates the complete array, so the
             * dereference restores its bounds rather than decaying them.
             */
            fixed_array_shape_t shape =
                fixed_array_shape_from_pointee_var(object);

            if (!addressed && object->type->pointee_array_element_type)
                object->type = object->type->pointee_array_element_type;
            fixed_array_shape_to_var(object, &shape);
            object->ptr_level = object->pointee_array_element_ptr_level;
            object->pointee_array_size = 0;
            object->pointee_array_dim2 = 0;
            object->pointee_array_dim3 = 0;
            object->pointee_array_dim4 = 0;
            object->pointee_array_element_ptr_level = 0;
        } else {
            object->type = pointee_type_from_pointer_typedef(object->type);
            object->ptr_level = depth - 1;
        }
        walk->addressed = false;
        walk->element_leaf = false;
    } else if (token->kind == T_ampersand) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk))
            return false;
        walk->addressed = object->array_size > 0;
        if (walk->addressed) {
            fixed_array_shape_t shape = fixed_array_shape_from_var(object);
            fixed_array_shape_t scalar = {0};

            fixed_array_shape_to_pointee_var(object, &shape);
            object->pointee_array_element_ptr_level = object->ptr_level;
            fixed_array_shape_to_var(object, &scalar);
        }
        object->ptr_level++;
        walk->element_leaf = false;
    } else if (token->kind == T_open_bracket) {
        token = token->next;
        if (!scan_sizeof_postfix_operand(scope, &token, object, walk) ||
            !token || token->kind != T_close_bracket)
            return false;
        token = token->next;
    } else if (token->kind == T_identifier) {
        var_t *root = find_var(token->literal, scope);

        if (!root)
            return false;
        memcpy(object, root, sizeof(*object));
        token = token->next;
    } else
        return false;

    while (token && (token->kind == T_dot || token->kind == T_arrow ||
                     token->kind == T_open_square)) {
        walk->addressed = false;
        if (token->kind == T_dot || token->kind == T_arrow) {
            bool through_pointer = token->kind == T_arrow;
            type_t *record;
            var_t *field;

            token = token->next;
            if (!token || token->kind != T_identifier ||
                (through_pointer && effective_pointer_depth(object) != 1) ||
                (!through_pointer && effective_pointer_depth(object) != 0))
                return false;
            record = through_pointer
                         ? pointee_type_from_pointer_typedef(object->type)
                         : object->type;
            if (!is_record_type(record))
                return false;
            field = find_member(token->literal, record);
            if (!field)
                return false;
            memcpy(object, field, sizeof(*object));
            walk->members++;
            walk->element_leaf = false;
            token = token->next;
        } else {
            int depth = 0;
            fixed_array_shape_t shape;

            /* An array may legitimately have pointer elements. Reject a scalar
             * pointer here, but retain the element pointer depth until after
             * this array subscript so a following `->` can consume it.
             */
            if (object->array_size <= 0)
                return false;
            for (; token; token = token->next) {
                if (token->kind == T_open_square)
                    depth++;
                else if (token->kind == T_close_square && --depth == 0) {
                    token = token->next;
                    break;
                }
            }
            if (depth)
                return false;
            shape = fixed_array_shape_from_var(object);
            if (!fixed_array_shape_drop_outer(&shape))
                return false;
            fixed_array_shape_to_var(object, &shape);
            walk->subscripts++;
            walk->element_leaf = !object->array_size;
        }
    }
    *cursor = token;
    return true;
}

/* The preceding scan proves the exact token shape and type constraints before
 * this pass advances the lexer. Each subscript is read in a detached block so
 * its side effects remain unevaluated. @open_consumed says the operand's
 * opening parenthesis was already taken as the one following sizeof.
 */
static void consume_sizeof_postfix_operand(block_t *parent,
                                           basic_block_t **bb,
                                           bool open_consumed)
{
    if (open_consumed) {
        consume_sizeof_postfix_operand(parent, bb, false);
        lex_expect(T_close_bracket);
    } else if (lex_accept(T_asterisk) || lex_accept(T_ampersand))
        consume_sizeof_postfix_operand(parent, bb, false);
    else if (lex_accept(T_open_bracket)) {
        consume_sizeof_postfix_operand(parent, bb, false);
        lex_expect(T_close_bracket);
    } else
        lex_expect(T_identifier);

    while (lex_peek(T_dot, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_open_square, NULL)) {
        if (lex_accept(T_dot) || lex_accept(T_arrow))
            lex_expect(T_identifier);
        else {
            basic_block_t *unevaluated_bb;
            int saved_side_effects;

            lex_expect(T_open_square);
            unevaluated_bb = bb_create(parent);
            saved_side_effects = se_idx;
            read_expr(parent, &unevaluated_bb);
            read_ternary_operation(parent, &unevaluated_bb);
            opstack_pop();
            se_idx = saved_side_effects;
            lex_expect(T_close_square);
        }
    }
}

/* The operand must end where sizeof's operand ends: at the closing parenthesis
 * of a parenthesized one, and otherwise before anything that would keep it
 * going as a call or an update.
 */
static bool sizeof_operand_tail_ends(const token_t *tail, bool parenthesized)
{
    return tail && (parenthesized ? tail->kind == T_close_bracket
                                  : tail->kind != T_open_bracket &&
                                        tail->kind != T_increment &&
                                        tail->kind != T_decrement);
}

/* A non-mutating admission check for callers that need to hand a global sizeof
 * operand to the detached local parser. It admits only a member array, so type
 * names and scalar forms remain owned by their existing global constant paths.
 */
static bool can_scan_sizeof_postfix_extent(block_t *scope,
                                           token_t *start,
                                           bool parenthesized,
                                           bool allow_flexible)
{
    token_t *tail = start;
    var_t object;
    sizeof_walk_t walk = {0};

    return scan_sizeof_postfix_operand(scope, &tail, &object, &walk) &&
           walk.members && sizeof_operand_tail_ends(tail, parenthesized) &&
           !effective_pointer_depth(&object) &&
           (object.array_size > 0 ||
            (allow_flexible && object.is_flexible_array_member));
}

/* Walk a local sizeof operand from @start and say whether this walker owns it:
 * an operand that still designates an array, whatever the path to it, or a
 * single element selected from one. Anything else keeps the ordinary
 * unevaluated-expression path.
 */
static bool walk_local_sizeof_operand(block_t *parent,
                                      token_t *start,
                                      bool parenthesized,
                                      var_t *object)
{
    token_t *tail = start;
    sizeof_walk_t walk = {0};

    if (!scan_sizeof_postfix_operand(parent, &tail, object, &walk) ||
        !sizeof_operand_tail_ends(tail, parenthesized))
        return false;
    if (object->is_flexible_array_member)
        return walk.members > 0;
    return object->array_size > 0 || walk.element_leaf;
}

/* Every sizeof operand made of an identifier, grouping, dereference, address,
 * member selections and constant or runtime subscripts goes through here: the
 * result is the declared extent of what the operand designates, which ordinary
 * expression lowering would decay or evaluate.
 */
static bool read_sizeof_postfix_operand(block_t *parent,
                                        basic_block_t **bb,
                                        bool parenthesized)
{
    bool open_consumed = false;
    var_t object;
    var_t *result;
    int size;

    if (!walk_local_sizeof_operand(parent, cur_token->next, parenthesized,
                                   &object)) {
        /* In "sizeof (*p).member" the parenthesis taken as sizeof's own opens a
         * grouping inside a longer unary expression. Walk it again from that
         * parenthesis as an operand without one.
         */
        if (!parenthesized || cur_token->kind != T_open_bracket ||
            !walk_local_sizeof_operand(parent, cur_token, false, &object))
            return false;
        open_consumed = true;
    }
    if (object.is_flexible_array_member)
        error_at("sizeof cannot be applied to a flexible array member",
                 cur_token_loc());
    if (is_bitfield(&object))
        error_at("sizeof cannot be applied to a bit-field", cur_token_loc());

    consume_sizeof_postfix_operand(parent, bb, open_consumed);
    if (parenthesized && !open_consumed)
        lex_expect(T_close_bracket);
    if (object.array_size > 0)
        size = sizeof_array_object(&object);
    else if (object.ptr_level || object.type->ptr_level || object.is_func)
        size = PTR_SIZE;
    else
        size = object.type->size;
    result = require_var(parent);
    result->init_val = size;
    result->var_name = gen_name();
    opstack_push(result);
    add_insn(parent, *bb, OP_load_constant, result, NULL, NULL, 0, NULL);
    return true;
}

static bool is_direct_fixed_array_pointer_slot(const var_t *pointer)
{
    return pointer && is_array_declarator(pointer) && !pointer->array_dim2 &&
           pointer->pointee_array_size > 0 &&
           !pointer->pointee_array_element_ptr_level &&
           ((pointer->ptr_level == 1 && !pointer->type->ptr_level) ||
            (!pointer->ptr_level && pointer->type->ptr_level == 1 &&
             pointer->type->array_element_ptr_level == 1));
}

void handle_sizeof_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_ID_LEN];
    int ptr_cnt = 0;
    int array_size = 0;
    int array_element_size = 0;
    token_t *sizeof_tk = cur_token;
    type_t *type = NULL;
    bool is_function = false;
    var_t *vd;

    bool parenthesized = lex_accept(T_open_bracket);

    /* A string literal is an array, not a pointer, before the array-to-pointer
     * conversion that ordinary expression lowering applies. Parenthesized
     * sizeof may take the fast path only when the literal sequence is the
     * complete operand.
     */
    token_t *after_string = cur_token->next;
    while (after_string && after_string->kind == T_string)
        after_string = after_string->next;
    if (lex_peek(T_string, NULL) &&
        (!parenthesized ||
         (after_string && after_string->kind == T_close_bracket))) {
        vd = require_var(parent);
        vd->init_val = read_sizeof_string_literal();
        vd->var_name = gen_name();
        if (parenthesized)
            lex_expect(T_close_bracket);
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        return;
    }

    token_t *after_wstring = cur_token->next;
    while (after_wstring && after_wstring->kind == T_wstring)
        after_wstring = after_wstring->next;
    if (lex_peek(T_wstring, NULL) &&
        (!parenthesized ||
         (after_wstring && after_wstring->kind == T_close_bracket))) {
        vd = require_var(parent);
        vd->init_val = read_sizeof_wstring_literal();
        vd->var_name = gen_name();
        if (parenthesized)
            lex_expect(T_close_bracket);
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        return;
    }

    if (read_sizeof_postfix_operand(parent, bb, parenthesized))
        return;

    /* The type-name alternative requires parentheses, but C99 also permits a
     * unary expression directly after sizeof. Keep direct array identifiers
     * from decaying before their extent is observed.
     */
    if (!parenthesized) {
        if (parent->func && lex_peek(T_identifier, token) &&
            !strcmp(token, "__func__")) {
            lex_expect(T_identifier);
            vd = require_var(parent);
            vd->init_val = strlen(parent->func->return_def.var_name) + 1;
            vd->var_name = gen_name();
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
            return;
        }

        if (lex_peek(T_identifier, token)) {
            var_t *array = find_var(token, parent);

            if (array && array->array_size > 0) {
                lex_expect(T_identifier);
                vd = require_var(parent);
                vd->init_val = sizeof_array_object(array);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
                return;
            }
        }

        basic_block_t *unevaluated_bb = bb_create(parent);
        int saved_side_effects = se_idx;
        read_expr_operand(parent, &unevaluated_bb);
        se_idx = saved_side_effects;
        var_t *expr_var = opstack_pop();
        if (is_bitfield(expr_var))
            error_at("sizeof cannot be applied to a bit-field",
                     &sizeof_tk->location);
        type = expr_var->type;
        ptr_cnt = expr_var->ptr_level;
        array_size = expr_var->array_size;
        is_function = expr_var->is_func;
        if (type == TY_void && ptr_cnt == 0)
            error_at("sizeof(void) is invalid", &sizeof_tk->location);
        if (is_function && ptr_cnt == 0)
            error_at("sizeof(function) is invalid", &sizeof_tk->location);

        vd = require_var(parent);
        vd->init_val = type->size;
        if (array_size > 0)
            vd->init_val = array_size * type->size;
        if (ptr_cnt)
            vd->init_val = PTR_SIZE;
        vd->var_name = gen_name();
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        return;
    }

    /* C99 specifies __func__ as if each function contained a distinct `static
     * const char []` initialized with its unadorned name. The normal expression
     * lowering materializes its address, but sizeof must retain the array
     * extent rather than observing that decayed pointer.
     */
    if (parent->func && lex_peek(T_identifier, token) &&
        !strcmp(token, "__func__") && cur_token->next &&
        cur_token->next->next &&
        cur_token->next->next->kind == T_close_bracket) {
        lex_expect(T_identifier);
        lex_expect(T_close_bracket);
        vd = require_var(parent);
        vd->init_val = strlen(parent->func->return_def.var_name) + 1;
        vd->var_name = gen_name();
        opstack_push(vd);
        add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        return;
    }

    /* A bare array identifier is the one expression form that must retain its
     * declared extent for sizeof; ordinary expression parsing intentionally
     * decays it to a pointer. cur_token is the opening parenthesis here.
     */
    if (lex_peek(T_identifier, token) && cur_token->next &&
        cur_token->next->next &&
        cur_token->next->next->kind == T_close_bracket) {
        var_t *array = find_var(token, parent);

        if (array && array->array_size > 0) {
            lex_expect(T_identifier);
            vd = require_var(parent);
            vd->init_val = sizeof_array_object(array);
            vd->var_name = gen_name();
            opstack_push(vd);
            lex_expect(T_close_bracket);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
            return;
        }
    }

    /* Check if this is sizeof(type) or sizeof(expression) */
    bool has_signed_type = false;
    bool has_unsigned_type = false;
    int long_type_count = 0;
    type_t *leading_scalar_type = NULL;

    if (lex_peek(T_identifier, token) &&
        (!strcmp(token, "char") || !strcmp(token, "short") ||
         !strcmp(token, "int"))) {
        token_t *after_base = cur_token->next->next;

        while (after_base && after_base->kind == T_const)
            after_base = after_base->next;
        if (after_base &&
            (after_base->kind == T_signed || after_base->kind == T_unsigned)) {
            lex_expect(T_identifier);
            leading_scalar_type = find_type(token, true);
        }
    }
    while (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
           lex_peek(T_long, NULL) || lex_peek(T_const, NULL) ||
           lex_peek(T_volatile, NULL)) {
        if (lex_accept(T_signed)) {
            if (has_signed_type)
                error_at("duplicate signed type specifier", cur_token_loc());
            has_signed_type = true;
        } else if (lex_accept(T_unsigned)) {
            if (has_unsigned_type)
                error_at("duplicate unsigned type specifier", cur_token_loc());
            has_unsigned_type = true;
        } else if (lex_accept(T_long))
            long_type_count++;
        else if (lex_accept(T_const))
            ;
        else
            lex_expect(T_volatile);
    }
    if (long_type_count > 2)
        error_at("too many long type specifiers", cur_token_loc());
    if (has_signed_type && has_unsigned_type)
        error_at("both signed and unsigned specified", cur_token_loc());
    if (leading_scalar_type == TY_int && lex_peek(T_identifier, token) &&
        !strcmp(token, "char"))
        error_at("int cannot be combined with char", cur_token_loc());
    bool has_long_type = long_type_count > 0;
    bool has_enum_type = lex_accept(T_enum);
    if (has_enum_type &&
        (has_signed_type || has_unsigned_type || has_long_type))
        error_at("enum type cannot be combined with integer specifiers",
                 cur_token_loc());
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union))
        find_type_flag = 2;

    /* `sizeof` only consumes object representation metadata, so it can admit
     * the C99 floating type names before value arithmetic and ABI lowering
     * exist. Keep all expression/declaration admission gates intact.
     */
    if (lex_accept(T_float)) {
        if (has_signed_type || has_unsigned_type || has_long_type ||
            has_enum_type || find_type_flag != 1)
            error_at("invalid float type specifiers", cur_token_loc());
        type = TY_float;
    } else if (lex_accept(T_double)) {
        if (has_signed_type || has_unsigned_type || has_enum_type ||
            find_type_flag != 1 || long_type_count > 1)
            error_at("invalid double type specifiers", cur_token_loc());
        type = has_long_type ? TY_long_double : TY_double;
    } else if (has_enum_type) {
        lex_ident(T_identifier, token);
        type = find_type_tag(token, parent);
        if (!type)
            error_at("Unknown enum type", cur_token_loc());
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (has_long_type) {
        type = has_unsigned_type ? TY_ulong : TY_long;
        if (long_type_count > 1) {
            /* sizeof only consumes type metadata. It does not materialize a
             * long-long value, so 32-bit targets can correctly report the
             * required eight-byte object size before their paired-register
             * value ABI is implemented.
             */
            if (has_unsigned_type)
                type = TY_ulong_long;
            else
                type = TY_long_long;
        }
        lex_accept(T_signed);
        lex_accept(T_const);
        if (lex_peek(T_identifier, token) && !strcmp(token, "int"))
            lex_expect(T_identifier);
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (has_unsigned_type) {
        if (leading_scalar_type == TY_char) {
            type = TY_uchar;
        } else if (leading_scalar_type == TY_short) {
            if (lex_peek(T_identifier, token) && !strcmp(token, "int"))
                lex_expect(T_identifier);
            type = TY_ushort;
        } else if (lex_peek(T_identifier, token) &&
                   (!strcmp(token, "int") || !strcmp(token, "char") ||
                    !strcmp(token, "short"))) {
            lex_expect(T_identifier);
            if (!strcmp(token, "char"))
                type = TY_uchar;
            else if (!strcmp(token, "short"))
                type = TY_ushort;
            else
                type = TY_uint;
        } else
            type = TY_uint;
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (has_signed_type) {
        if (leading_scalar_type == TY_char || leading_scalar_type == TY_short) {
            if (leading_scalar_type == TY_short &&
                lex_peek(T_identifier, token) && !strcmp(token, "int"))
                lex_expect(T_identifier);
            type =
                leading_scalar_type == TY_char ? TY_schar : leading_scalar_type;
        } else if (lex_peek(T_identifier, token) &&
                   (!strcmp(token, "int") || !strcmp(token, "char") ||
                    !strcmp(token, "short"))) {
            lex_expect(T_identifier);
            type = !strcmp(token, "char") ? TY_schar : find_type(token, true);
        } else
            type = TY_int;
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (leading_scalar_type) {
        type = leading_scalar_type;
        while (lex_accept(T_asterisk)) {
            ptr_cnt++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
    } else if (lex_peek(T_identifier, token)) {
        /* Try to parse as a type first */
        type = find_type_flag == 2 ? find_type(token, find_type_flag)
                                   : find_visible_type(token, parent);
        if (type) {
            /* sizeof(type) */
            lex_expect(T_identifier);
            while (lex_accept(T_asterisk)) {
                ptr_cnt++;
                while (lex_accept(T_const) || lex_accept(T_volatile) ||
                       lex_accept(T_restrict))
                    ;
            }
        }
    }

    /* The integer branches above consume their own abstract pointer declarators
     * for historical reasons. Float/double type names select a type before that
     * legacy code, so finish the common suffix here.
     */
    while (type && lex_accept(T_asterisk)) {
        ptr_cnt++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* VLA support is intentionally outside this compiler's C99 scope, but a
     * type name in sizeof may still carry fixed abstract array bounds. Keep the
     * simple direct form here rather than treating '[' as an expression token
     * after the scalar type name.
     */
    while (type && ptr_cnt == 0 && lex_accept(T_open_square)) {
        int bound = read_const_expr(parent);

        lex_expect(T_close_square);
        if (bound <= 0)
            error_at("sizeof array type needs a positive constant bound",
                     cur_token_loc());
        if (bound > 0 && array_size && array_size > INT_MAX / bound)
            error_at("sizeof array type is too large", cur_token_loc());
        if (bound > 0)
            array_size = array_size ? array_size * bound : bound;
    }

    if (type && lex_accept(T_open_bracket)) {
        int nested_array_size = 0;
        int nested_ptr_count = 0;
        int nested_function_ptr_count = 0;
        int nested_outer_array_size = 0;

        while (lex_accept(T_asterisk)) {
            nested_ptr_count++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        }
        if (!nested_ptr_count)
            error_at("sizeof abstract declarator needs a pointer",
                     cur_token_loc());
        if (lex_peek(T_open_bracket, NULL)) {
            nested_function_ptr_count =
                read_sizeof_nested_function_pointer_suffix(
                    parent, &nested_outer_array_size);
            if (nested_outer_array_size) {
                array_size = nested_outer_array_size;
                array_element_size = PTR_SIZE;
            } else
                ptr_cnt += nested_ptr_count + nested_function_ptr_count;
        } else
            while (lex_accept(T_open_square)) {
                int bound = read_const_expr(parent);

                lex_expect(T_close_square);
                if (bound <= 0)
                    error_at(
                        "sizeof array type needs a positive constant bound",
                        cur_token_loc());
                if (bound > 0 && nested_array_size &&
                    nested_array_size > INT_MAX / bound)
                    error_at("sizeof array type is too large", cur_token_loc());
                if (bound > 0)
                    nested_array_size =
                        nested_array_size ? nested_array_size * bound : bound;
            }
        if (!nested_function_ptr_count) {
            lex_expect(T_close_bracket);
            if (nested_array_size) {
                array_size = nested_array_size;
                array_element_size = PTR_SIZE;

                /* The grouped array declarator in `int (*[2][3])(int)` leaves
                 * its pointed-to function suffix outside the group. sizeof only
                 * needs pointer-sized elements, but still must parse that valid
                 * C99 prototype completely.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            } else {
                ptr_cnt += nested_ptr_count;
                while (lex_accept(T_open_square)) {
                    int bound = read_const_expr(parent);

                    lex_expect(T_close_square);
                    if (bound <= 0)
                        error_at(
                            "sizeof array type needs a positive constant bound",
                            cur_token_loc());
                }

                /* A pointer declarator followed by a parameter list names a
                 * function pointer. sizeof observes the pointer object, not the
                 * function type, so no expression lowering or function ABI is
                 * needed. Reuse the declaration parser for prototype syntax.
                 */
                if (lex_peek(T_open_bracket, NULL))
                    read_sizeof_function_prototype();
            }
        }
    }

    if (!type) {
        /* sizeof(expression) - parse the expression and get its type */
        basic_block_t *unevaluated_bb = bb_create(parent);
        int saved_side_effects = se_idx;
        unevaluated_expression_depth++;
        if (!read_assignment_expression(parent, &unevaluated_bb)) {
            read_expr(parent, &unevaluated_bb);
            read_ternary_operation(parent, &unevaluated_bb);
        }
        unevaluated_expression_depth--;
        se_idx = saved_side_effects;
        var_t *expr_var = opstack_pop();
        if (is_bitfield(expr_var))
            error_at("sizeof cannot be applied to a bit-field",
                     &sizeof_tk->location);
        type = expr_var->type;
        ptr_cnt = expr_var->ptr_level;
        array_size = expr_var->array_size;
        is_function = expr_var->is_func;
        if (is_incomplete_record_object(expr_var))
            error_at("sizeof cannot be applied to an incomplete record type",
                     &sizeof_tk->location);
    }

    if (!type)
        error_at("Unable to determine type in sizeof", &sizeof_tk->location);
    if (type == TY_void && ptr_cnt == 0)
        error_at("sizeof(void) is invalid", &sizeof_tk->location);
    if ((is_function || type->is_direct_function_type) && ptr_cnt == 0)
        error_at("sizeof(function) is invalid", &sizeof_tk->location);

    if (!array_size && !ptr_cnt && type->array_size)
        array_size = type->array_size;
    vd = require_var(parent);
    vd->init_val = type->size;
    if (array_size > 0)
        vd->init_val =
            array_size * (array_element_size ? array_element_size : type->size);
    if (ptr_cnt)
        vd->init_val = PTR_SIZE;

    /* VLA is intentionally outside shecc's C99 scope, so every admitted sizeof
     * result is an integer constant expression.
     */
    vd->is_const = true;
    vd->var_name = gen_name();
    opstack_push(vd);
    lex_expect(T_close_bracket);
    add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

/* Keep grouped row-element postfix support deliberately exact until general
 * grouped lvalues have an address representation. This scanner never consumes
 * tokens, so every non-match remains owned by ordinary expression parsing.
 */
static bool grouped_scalar_pointee_row_postfix_starts(void)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int suffixes = 0;

    if (!token || token->kind != T_open_bracket || !(token = token->next) ||
        token->kind != T_asterisk || !(token = token->next) ||
        token->kind != T_identifier || !(token = token->next) ||
        token->kind != T_close_bracket || !(token = token->next))
        return false;

    while (suffixes < 4 && token && token->kind == T_open_square) {
        int depth = 0;

        for (; token; token = token->next) {
            if (token->kind == T_open_square || token->kind == T_open_bracket ||
                token->kind == T_open_curly)
                depth++;
            else if (token->kind == T_close_square ||
                     token->kind == T_close_bracket ||
                     token->kind == T_close_curly) {
                if (!--depth) {
                    token = token->next;
                    break;
                }
            }
        }
        if (!token)
            return false;
        suffixes++;
    }

    return suffixes &&
           (token->kind == T_increment || token->kind == T_decrement);
}

static void handle_grouped_scalar_pointee_row_postfix(block_t *parent,
                                                      basic_block_t **bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *old, *one, *updated;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;
    opcode_t op;

    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_scalar_pointee_array_dereference(source, parent, bb))
        error_at("Grouped pointer-to-array update requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    shape = fixed_array_shape_from_var(row);
    dimensions = shape.rank;
    if (dimensions >= 3 &&
        (is_record_type(row->type) || row->type->is_floating))
        error_at(
            "Grouped rank-three/four postfix update requires an integer scalar",
            cur_token_loc());
    if (dimensions > 4)
        error_at(
            "Grouped pointer-to-array postfix update supports at most four "
            "dimensions",
            cur_token_loc());
    lex_expect(T_close_bracket);
    address = row;
    while (lex_accept(T_open_square)) {
        int stride;

        if (depth >= dimensions)
            error_at("Too many grouped array subscripts", cur_token_loc());
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        stride = fixed_array_shape_stride(&shape, depth, row->type->size);
        if (stride != 1) {
            one = require_var(parent);
            one->var_name = gen_name();
            one->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            updated = require_var(parent);
            updated->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, updated, index, one, 0, NULL);
            index = updated;
        }
        updated = require_typed_ptr_var(parent, row->type, 1);
        updated->var_name = gen_name();
        add_insn(parent, *bb, OP_add, updated, address, index, 0, NULL);
        address = updated;
        depth++;
    }
    if (depth != dimensions)
        error_at("Grouped pointer-to-array postfix update needs all subscripts",
                 cur_token_loc());
    old = require_typed_var(parent, row->type);
    old->var_name = gen_name();
    add_insn(parent, *bb, OP_read, old, address, NULL, row->type->size, NULL);
    if (lex_accept(T_increment))
        op = OP_add;
    else {
        lex_expect(T_decrement);
        op = OP_sub;
    }
    one = require_typed_var(parent, TY_int);
    one->var_name = gen_name();
    one->init_val = 1;
    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
    updated = require_var(parent);
    updated->var_name = gen_name();
    updated->type = integer_binary_result_type(op, old, one);
    add_insn(parent, *bb, op, updated, old, one, 0, NULL);
    updated = resize_to(parent, bb, updated, row->type, 0);
    add_insn(parent, *bb, OP_write, NULL, address, updated, row->type->size,
             NULL);
    opstack_push(old);
}

/* Keep prefix support equally narrow: the generic prefix path has no general
 * grouped-lvalue address representation. This scanner is non-mutating so a
 * non-match remains entirely owned by that generic path.
 */
static bool grouped_scalar_pointee_row_prefix_starts(void)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int suffixes = 0;

    if (!token || (token->kind != T_increment && token->kind != T_decrement) ||
        !(token = token->next) || token->kind != T_open_bracket ||
        !(token = token->next) || token->kind != T_asterisk ||
        !(token = token->next) || token->kind != T_identifier ||
        !(token = token->next) || token->kind != T_close_bracket ||
        !(token = token->next))
        return false;

    while (suffixes < 4 && token && token->kind == T_open_square) {
        int depth = 0;

        for (; token; token = token->next) {
            if (token->kind == T_open_square || token->kind == T_open_bracket ||
                token->kind == T_open_curly)
                depth++;
            else if (token->kind == T_close_square ||
                     token->kind == T_close_bracket ||
                     token->kind == T_close_curly) {
                if (!--depth) {
                    token = token->next;
                    break;
                }
            }
        }
        if (!token)
            return false;
        suffixes++;
    }
    return suffixes;
}

static void handle_grouped_scalar_pointee_row_prefix(block_t *parent,
                                                     basic_block_t **bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *current, *one, *updated;
    fixed_array_shape_t shape;
    int dimensions;
    int depth = 0;
    opcode_t op;

    if (lex_accept(T_increment))
        op = OP_add;
    else {
        lex_expect(T_decrement);
        op = OP_sub;
    }
    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_scalar_pointee_array_dereference(source, parent, bb))
        error_at("Grouped pointer-to-array update requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    shape = fixed_array_shape_from_var(row);
    dimensions = shape.rank;
    if (dimensions >= 3 &&
        (is_record_type(row->type) || row->type->is_floating))
        error_at(
            "Grouped rank-three/four prefix update requires an integer scalar",
            cur_token_loc());
    if (dimensions > 4)
        error_at(
            "Grouped pointer-to-array prefix update supports at most four "
            "dimensions",
            cur_token_loc());
    lex_expect(T_close_bracket);
    address = row;
    while (lex_accept(T_open_square)) {
        int stride;

        if (depth >= dimensions)
            error_at("Too many grouped array subscripts", cur_token_loc());
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        index = opstack_pop();
        lex_expect(T_close_square);
        stride = fixed_array_shape_stride(&shape, depth, row->type->size);
        if (stride != 1) {
            one = require_var(parent);
            one->var_name = gen_name();
            one->init_val = stride;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            updated = require_var(parent);
            updated->var_name = gen_name();
            add_insn(parent, *bb, OP_mul, updated, index, one, 0, NULL);
            index = updated;
        }
        updated = require_typed_ptr_var(parent, row->type, 1);
        updated->var_name = gen_name();
        add_insn(parent, *bb, OP_add, updated, address, index, 0, NULL);
        address = updated;
        depth++;
    }
    if (depth != dimensions)
        error_at("Grouped pointer-to-array prefix update needs all subscripts",
                 cur_token_loc());
    current = require_typed_var(parent, row->type);
    current->var_name = gen_name();
    add_insn(parent, *bb, OP_read, current, address, NULL, row->type->size,
             NULL);
    one = require_typed_var(parent, TY_int);
    one->var_name = gen_name();
    one->init_val = 1;
    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
    updated = require_var(parent);
    updated->var_name = gen_name();
    updated->type = integer_binary_result_type(op, current, one);
    add_insn(parent, *bb, op, updated, current, one, 0, NULL);
    updated = resize_to(parent, bb, updated, row->type, 0);
    add_insn(parent, *bb, OP_write, NULL, address, updated, row->type->size,
             NULL);
    opstack_push(updated);
}

void read_expr_operand(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    var_t *compound_object = NULL;
    bool is_neg = false;

    if (grouped_scalar_pointee_row_postfix_starts()) {
        handle_grouped_scalar_pointee_row_postfix(parent, bb);
        return;
    }

    if ((lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) &&
        grouped_scalar_pointee_row_prefix_starts()) {
        handle_grouped_scalar_pointee_row_prefix(parent, bb);
        return;
    }

    bool prefix_increment = lex_peek(T_increment, NULL);
    bool prefix_decrement = lex_peek(T_decrement, NULL);
    if ((prefix_increment || prefix_decrement) && cur_token->next->next &&
        cur_token->next->next->kind == T_open_bracket) {
        opcode_t op = prefix_increment ? OP_add : OP_sub;
        var_t *object;
        var_t *one;

        if (prefix_increment)
            lex_expect(T_increment);
        else
            lex_expect(T_decrement);
        read_expr_operand(parent, bb);
        object = opstack_pop();
        if (object->is_compound_literal_reference) {
            if (object->is_const_qualified)
                error_at("assignment of read-only location", cur_token_loc());
            one = require_typed_var(parent, TY_int);
            one->var_name = gen_name();
            one->init_val = 1;
            add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
            if (is_pointer_operation(op, object, one)) {
                handle_pointer_arithmetic(parent, bb, op, object, one);
                vd = opstack_pop();
            } else {
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = integer_binary_result_type(op, object, one);
                add_insn(parent, *bb, op, vd, object, one, 0, NULL);
            }
            vd = resize_var(parent, bb, vd, object);
            if (object->compound_literal_bitfield)
                write_bitfield_value(parent, bb,
                                     object->compound_literal_address, vd,
                                     object->compound_literal_bitfield);
            else
                add_insn(parent, *bb, OP_write, NULL,
                         object->compound_literal_address, vd,
                         object->ptr_level ? PTR_SIZE : object->type->size,
                         NULL);
            opstack_push(vd);
            return;
        }
        if (!object->is_compound_literal || object->array_size ||
            is_record_type(object->type))
            error_at("Prefix update requires a scalar modifiable lvalue",
                     cur_token_loc());
        if (object->is_const_qualified)
            error_at("assignment of read-only location", cur_token_loc());
        one = require_typed_var(parent, TY_int);
        one->var_name = gen_name();
        one->init_val = 1;
        add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0, NULL);
        if (is_pointer_operation(op, object, one)) {
            handle_pointer_arithmetic(parent, bb, op, object, one);
            vd = opstack_pop();
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = integer_binary_result_type(op, object, one);
            add_insn(parent, *bb, op, vd, object, one, 0, NULL);
        }
        vd = resize_var(parent, bb, vd, object);
        add_insn(parent, *bb, OP_assign, object, vd, NULL, 0, NULL);
        opstack_push(vd);
        return;
    }

    if (lex_accept(T_plus)) {
        read_expr_operand(parent, bb);
        rs1 = opstack_pop();
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("unary plus requires an arithmetic operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);

        /* Unary plus performs the integer promotions, producing an ordinary
         * arithmetic expression rather than a bit-field designator.
         */
        rs1->is_bitfield = false;
        opstack_push(rs1);
        return;
    }

    if (lex_accept(T_minus)) {
        is_neg = true;
        if (!lex_peek(T_numeric, NULL) && !lex_peek(T_identifier, NULL) &&
            !lex_peek(T_open_bracket, NULL)) {
            error_at("Unexpected token after unary minus", next_token_loc());
        }
    }

    if (lex_peek(T_string, NULL))
        read_literal_param(parent, *bb);
    else if (lex_peek(T_wstring, NULL))
        read_wstring_param(parent, *bb);
    else if (lex_peek(T_char, NULL))
        read_char_param(parent, *bb);
    else if (lex_peek(T_wchar, NULL))
        read_wchar_param(parent, *bb);

    else if (lex_peek(T_floating, NULL))
        error_at("Floating point literals are not yet supported",
                 cur_token_loc());

    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(parent, *bb, is_neg);
    else if (lex_accept(T_log_not)) {
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();
        rs1 = materialize_function_designator(parent, bb, rs1);

        /* Constant folding for logical NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = TY_int;
            vd->is_const = true;
            vd->init_val = !(rs1->init_val || rs1->init_val_hi);
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* C99 6.5.3.3: logical negation always yields int. Preserving the
             * operand's type made !pointer inherit the pointed-to record size,
             * so a later comparison attempted an invalid truncation on 32-bit
             * targets.
             */
            vd->type = TY_int;
            vd->ptr_level = 0;
            opstack_push(vd);
            add_insn(parent, *bb, OP_log_not, vd, rs1, NULL, 0, NULL);
        }
    } else if (lex_accept(T_bit_not)) {
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("bitwise complement requires an integer operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);

        /* Constant folding for bitwise NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
            vd->is_const = true;
            vd->init_val = ~rs1->init_val;
            vd->init_val_hi = ~rs1->init_val_hi;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = rs1->type;
            opstack_push(vd);
            add_insn(parent, *bb, OP_bit_not, vd, rs1, NULL, 0, NULL);
        }
    } else if (lex_accept(T_ampersand)) {
        handle_address_of_operator(parent, bb);
    } else if (lex_accept(T_asterisk)) {
        /* dereference */
        if (lex_peek(T_asterisk, NULL)) {
            handle_multiple_dereference(parent, bb);
        } else {
            handle_single_dereference(parent, bb);
        }
    } else if (lex_accept(T_open_bracket)) {
        /* Check if this is a cast, compound literal, or parenthesized
         * expression
         */
        char lookahead_token[MAX_ID_LEN];
        bool is_compound_literal = false;
        bool is_cast = false;
        type_t *cast_or_literal_type = NULL;
        int cast_ptr_level = 0;
        int cast_array_size = 0;
        int cast_array_dim2 = 0;
        int cast_array_dim3 = 0;
        int cast_array_dim4 = 0;
        int cast_array_dims = 0;
        int cast_array_element_ptr_level = 0;
        int cast_pointee_array_size = 0;
        int cast_pointee_array_dim2 = 0;
        int cast_pointee_array_dim3 = 0;
        int cast_pointee_array_dim4 = 0;
        bool cast_array_outer_unsized = false;
        bool cast_parenthesized_array = false;
        bool cast_const_qualified = false;
        bool cast_const_pointer = false;
        unsigned int cast_pointer_const_mask = 0;
        bool cast_volatile_qualified = false;

        if (floating_type_starts_here())
            error_at("Floating point types are not yet supported",
                     cur_token_loc());

        /* Look ahead to see if we have a typename followed by ) */
        token_t *type_start = cur_token;
        bool has_const_type = false;
        bool has_signed_type = false;
        bool has_unsigned_type = false;
        int long_type_count = 0;
        type_t *leading_scalar_type = NULL;

        if (lex_peek(T_identifier, lookahead_token) &&
            (!strcmp(lookahead_token, "char") ||
             !strcmp(lookahead_token, "short") ||
             !strcmp(lookahead_token, "int"))) {
            token_t *after_base = cur_token->next->next;

            while (after_base && after_base->kind == T_const)
                after_base = after_base->next;
            if (after_base && (after_base->kind == T_signed ||
                               after_base->kind == T_unsigned)) {
                lex_expect(T_identifier);
                leading_scalar_type = find_type(lookahead_token, true);
            }
        }
        while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
               lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
               lex_peek(T_long, NULL)) {
            if (lex_accept(T_const))
                has_const_type = true;
            else if (lex_accept(T_volatile))
                cast_volatile_qualified = true;
            else if (lex_accept(T_signed)) {
                if (has_signed_type)
                    error_at("duplicate signed type specifier",
                             cur_token_loc());
                has_signed_type = true;
            } else if (lex_accept(T_unsigned)) {
                if (has_unsigned_type)
                    error_at("duplicate unsigned type specifier",
                             cur_token_loc());
                has_unsigned_type = true;
            } else {
                lex_expect(T_long);
                long_type_count++;
            }
        }
        if (long_type_count > 2)
            error_at("too many long type specifiers", cur_token_loc());
        if (has_signed_type && has_unsigned_type)
            error_at("both signed and unsigned specified", cur_token_loc());
        if (leading_scalar_type == TY_int &&
            lex_peek(T_identifier, lookahead_token) &&
            !strcmp(lookahead_token, "char"))
            error_at("int cannot be combined with char", cur_token_loc());
        bool has_long_type = long_type_count > 0;
        bool has_enum_type = lex_accept(T_enum);
        if (has_enum_type &&
            (has_signed_type || has_unsigned_type || has_long_type))
            error_at("enum type cannot be combined with integer specifiers",
                     cur_token_loc());
        bool has_type_identifier = lex_peek(T_identifier, lookahead_token);
        if (has_const_type || has_signed_type || has_unsigned_type ||
            has_long_type || has_enum_type || has_type_identifier ||
            lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
            /* Check if it's a basic type or typedef */
            token_t *saved_token = type_start;
            bool is_record = lex_accept(T_struct);
            if (!is_record)
                is_record = lex_accept(T_union);
            if (is_record)
                lex_ident(T_identifier, lookahead_token);

            type_t *type;
            if (has_enum_type) {
                lex_ident(T_identifier, lookahead_token);
                type = find_type_tag(lookahead_token, parent);
            } else if (has_unsigned_type && !is_record) {
                if (has_long_type) {
                    type = TY_ulong;
                    if (long_type_count > 1) {
                        type = TY_ulong_long;
                    }
                    if (lex_peek(T_identifier, lookahead_token) &&
                        !strcmp(lookahead_token, "int"))
                        lex_expect(T_identifier);
                } else if (leading_scalar_type == TY_char) {
                    type = TY_uchar;
                } else if (leading_scalar_type == TY_short) {
                    if (lex_peek(T_identifier, lookahead_token) &&
                        !strcmp(lookahead_token, "int"))
                        lex_expect(T_identifier);
                    type = TY_ushort;
                } else if (lex_peek(T_identifier, lookahead_token) &&
                           (!strcmp(lookahead_token, "int") ||
                            !strcmp(lookahead_token, "char") ||
                            !strcmp(lookahead_token, "short"))) {
                    lex_expect(T_identifier);
                    if (!strcmp(lookahead_token, "char"))
                        type = TY_uchar;
                    else if (!strcmp(lookahead_token, "short"))
                        type = TY_ushort;
                    else
                        type = TY_uint;
                } else
                    type = TY_uint;
            } else if (has_long_type && !is_record) {
                type = TY_long;
                if (long_type_count > 1) {
                    type = TY_long_long;
                }
                lex_accept(T_signed);
                lex_accept(T_const);
                if (lex_peek(T_identifier, lookahead_token) &&
                    !strcmp(lookahead_token, "int"))
                    lex_expect(T_identifier);
            } else if (has_signed_type && !is_record) {
                if (leading_scalar_type == TY_char ||
                    leading_scalar_type == TY_short) {
                    if (leading_scalar_type == TY_short &&
                        lex_peek(T_identifier, lookahead_token) &&
                        !strcmp(lookahead_token, "int"))
                        lex_expect(T_identifier);
                    type = leading_scalar_type == TY_char ? TY_schar
                                                          : leading_scalar_type;
                } else if (lex_peek(T_identifier, lookahead_token) &&
                           (!strcmp(lookahead_token, "int") ||
                            !strcmp(lookahead_token, "char") ||
                            !strcmp(lookahead_token, "short"))) {
                    lex_expect(T_identifier);
                    type = !strcmp(lookahead_token, "char")
                               ? TY_schar
                               : find_type(lookahead_token, true);
                } else {
                    type = TY_int;
                }
            } else if (leading_scalar_type) {
                type = leading_scalar_type;
            } else {
                type = find_type(lookahead_token, is_record ? 2 : true);
            }

            if (type) {
                if (type->is_floating)
                    error_at("Floating point types are not yet supported",
                             cur_token_loc());

                /* Save current position to backtrack if needed Try to parse as
                 * typename
                 */
                if (!is_record && !has_enum_type && !has_signed_type &&
                    !has_unsigned_type && !has_long_type)
                    lex_expect(T_identifier);

                /* A qualifier may appear before or after the base type. */
                while (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL)) {
                    if (lex_accept(T_const))
                        has_const_type = true;
                    else
                        cast_volatile_qualified = true;
                }

                /* Check for pointer types: int*, char*, etc. */
                int ptr_level = 0;
                while (lex_accept(T_asterisk)) {
                    ptr_level++;
                    while (lex_peek(T_const, NULL) ||
                           lex_peek(T_volatile, NULL) ||
                           lex_peek(T_restrict, NULL)) {
                        if (lex_accept(T_const)) {
                            cast_const_pointer = true;
                            if (ptr_level <= 32)
                                cast_pointer_const_mask |= 1U
                                                           << (ptr_level - 1);
                        } else if (lex_accept(T_volatile))
                            cast_volatile_qualified = true;
                        else
                            lex_expect(T_restrict);
                    }
                }

                if (PTR_SIZE < 8 && type->base_type == TYPE_long_long &&
                    !(ptr_level || type->ptr_level))
                    error_at("long long value needs 64-bit target lowering",
                             cur_token_loc());

                bool is_array = false;

                /* A parenthesized abstract declarator, such as `(int
                 * (*[])[2]){...}`, is an array whose elements are pointers to
                 * rows. Keep the outer array and pointee bounds separate,
                 * matching named pointer-to-array declarations.
                 */
                if (lex_accept(T_open_bracket)) {
                    int pointee_dims = 0;

                    if (ptr_level || !lex_peek(T_asterisk, NULL))
                        error_at(
                            "Array compound literal needs a pointer declarator",
                            cur_token_loc());
                    do {
                        lex_expect(T_asterisk);
                        cast_array_element_ptr_level++;
                        while (lex_accept(T_const) || lex_accept(T_volatile) ||
                               lex_accept(T_restrict))
                            ;
                    } while (lex_peek(T_asterisk, NULL));
                    lex_expect(T_open_square);
                    if (!lex_peek(T_close_square, NULL)) {
                        cast_array_size = read_const_expr(parent);
                        if (cast_array_size <= 0)
                            error_at(
                                "Array compound literal needs a positive bound",
                                cur_token_loc());
                    } else {
                        cast_array_outer_unsized = true;
                    }
                    lex_expect(T_close_square);
                    lex_expect(T_close_bracket);
                    while (lex_accept(T_open_square)) {
                        int bound = read_const_expr(parent);

                        if (pointee_dims >= 4)
                            error_at(
                                "Array declarators support at most four "
                                "dimensions",
                                cur_token_loc());
                        if (bound <= 0)
                            error_at("Array size must be positive",
                                     cur_token_loc());
                        if (pointee_dims == 0)
                            cast_pointee_array_size = bound;
                        else {
                            if (pointee_dims == 1)
                                cast_pointee_array_dim2 = bound;
                            else if (pointee_dims == 2)
                                cast_pointee_array_dim3 = bound;
                            else
                                cast_pointee_array_dim4 = bound;
                            cast_pointee_array_size *= bound;
                        }
                        lex_expect(T_close_square);
                        pointee_dims++;
                    }
                    if (!pointee_dims)
                        error_at("Array compound literal needs a row bound",
                                 cur_token_loc());
                    is_array = true;
                    cast_array_dims = 1;
                    cast_parenthesized_array = true;
                }

                /* Parse every array bound in a compound-literal type name.
                 * `var_t` records the complete flattened element count plus up
                 * to three trailing dimensions, the same representation
                 * ordinary declarators use for `int a[2][3]`.
                 */
                while (!cast_parenthesized_array && lex_accept(T_open_square)) {
                    int bound = 0;

                    is_array = true;
                    if (cast_array_dims >= 4)
                        error_at(
                            "Array declarators support at most four dimensions",
                            cur_token_loc());
                    if (lex_peek(T_numeric, NULL)) {
                        char bound_token[MAX_TOKEN_LEN];
                        lex_ident_n(T_numeric, bound_token, MAX_TOKEN_LEN);
                        bound = parse_numeric_constant(bound_token);
                        if (bound <= 0)
                            error_at(
                                "Array compound literal needs a positive bound",
                                next_token_loc());
                    }
                    if (!bound && cast_array_dims)
                        error_at("Only the outer array bound may be omitted",
                                 cur_token_loc());
                    if (!cast_array_dims)
                        cast_array_size = bound;
                    else {
                        if (cast_array_size)
                            cast_array_size *= bound;
                        else
                            cast_array_size = bound;
                        if (cast_array_dims == 1)
                            cast_array_dim2 = bound;
                        else if (cast_array_dims == 2)
                            cast_array_dim3 = bound;
                        else
                            cast_array_dim4 = bound;
                    }
                    if (!cast_array_dims && !bound)
                        cast_array_outer_unsized = true;
                    cast_array_dims++;
                    lex_expect(T_close_square);
                }
                if (!is_array && type->array_size) {
                    is_array = true;
                    cast_array_size = type->array_size;
                    cast_array_dim2 = type->array_dim2;
                    cast_array_dim3 = type->array_dim3;
                    cast_array_dim4 = type->array_dim4;
                    cast_array_dims = 1 + !!cast_array_dim2 +
                                      !!cast_array_dim3 + !!cast_array_dim4;
                }

                /* Check what follows the closing ) */
                if (lex_accept(T_close_bracket)) {
                    if (lex_peek(T_open_curly, NULL)) {
                        /* (type){...} - compound literal */
                        is_compound_literal = true;
                        cast_or_literal_type = type;
                        cast_ptr_level = ptr_level;
                        cast_const_qualified =
                            has_const_type || type->is_const_qualified;

                        /* Store is_array flag in cast_ptr_level if it's an
                         * array
                         */
                        if (is_array) {
                            /* Special marker for array compound literal */
                            cast_ptr_level = -1;
                        }
                    } else {
                        if (is_array)
                            error_at("Cast cannot specify an array type",
                                     cur_token_loc());
                        /* (type)expr - cast expression */
                        is_cast = true;
                        cast_or_literal_type = type;
                        cast_ptr_level = ptr_level;
                        cast_const_qualified =
                            has_const_type || type->is_const_qualified;
                    }
                } else {
                    /* Not a cast or compound literal - backtrack */
                    cur_token = saved_token;
                }
            }
        }

        if (is_cast) {
            /* Process cast: (type)expr Parse the expression to be cast */
            read_expr_operand(parent, bb);

            /* Get the expression result */
            var_t *expr_var = opstack_pop();

            /* A cast of a function designator is still a pointer value. Raw
             * symbols have no local defining IR, so materialize the code
             * address before OP_cast treats it as an ordinary operand.
             */
            if (cast_or_literal_type->func_signature)
                expr_var =
                    materialize_function_designator(parent, bb, expr_var);

            /* Create variable for cast result */
            var_t *cast_var = require_typed_ptr_var(
                parent, cast_or_literal_type, cast_ptr_level);
            cast_var->var_name = gen_name();
            cast_var->is_const_qualified = cast_const_qualified;
            cast_var->is_const_pointer = cast_const_pointer;
            cast_var->pointer_const_mask = cast_pointer_const_mask;
            cast_var->is_volatile = cast_volatile_qualified;
            if (cast_or_literal_type->func_signature) {
                cast_var->ptr_level = 1;
                cast_var->func_signature = cast_or_literal_type->func_signature;
            }

            /* A cast of an integer constant expression remains an integer
             * constant expression. Preserve that payload for consumers such as
             * the null-pointer-constant constraint of `?:`; pointer casts
             * deliberately do not receive this integer classification.
             */
            if (!cast_ptr_level && expr_var->is_const &&
                !is_pointer_like_value(expr_var) && !expr_var->is_func &&
                cast_var->type->base_type != TYPE_void) {
                unsigned int lo = (unsigned int) expr_var->init_val;
                unsigned int hi = (unsigned int) expr_var->init_val_hi;

                cast_var->is_const = true;

                /* Mirror the scalar cast's stored representation instead of
                 * merely copying its source payload: `(unsigned char)256`,
                 * `(short)65536`, and a 64-to-32 cast all become the integer
                 * constant zero and may therefore serve as null pointers.
                 */
                if (cast_var->type->is_bool) {
                    /* `_Bool` conversion is boolean, not truncation: any
                     * nonzero source becomes one, including a high 64-bit word
                     * that a 32-bit truncation would otherwise lose.
                     */
                    cast_var->init_val = lo || hi;
                    cast_var->init_val_hi = 0;
                } else if (cast_var->type->size < TY_int->size) {
                    unsigned int bits = cast_var->type->size * 8;
                    unsigned int mask = (1U << bits) - 1;

                    lo &= mask;
                    if (!cast_var->type->is_unsigned &&
                        (lo & (1U << (bits - 1))))
                        lo |= ~mask;
                    cast_var->init_val = (int) lo;
                    cast_var->init_val_hi = cast_var->init_val < 0 ? -1 : 0;
                } else if (cast_var->type->size == TY_int->size) {
                    cast_var->init_val = (int) lo;
                    cast_var->init_val_hi =
                        cast_var->type->is_unsigned
                            ? 0
                            : (cast_var->init_val < 0 ? -1 : 0);
                } else {
                    cast_var->init_val = (int) lo;
                    cast_var->init_val_hi = (int) hi;
                }
            }

            /* An explicit C cast is permitted to remove qualifiers, but it is
             * almost always a bug. Keep compiling it while making that loss
             * visible, unlike implicit pointer assignments which are rejected.
             */
            if (incompatible_const_pointer_conversion(expr_var, cast_var))
                printf("Warning: discarding const qualifier in cast\n");

            /* Generate cast IR. A cast down to a narrower type has to discard
             * the high bits: OP_cast is only a move, so "(char) 300" kept the
             * whole 300 and compared unequal to 44, even though assigning the
             * same value to a char produced 44. get_size() decides that the
             * same way the rest of the parser does, including for pointers,
             * arrays and typedefs.
             */
            opcode_t cast_op = OP_cast;
            if (get_size(cast_var) < get_size(expr_var))
                cast_op = OP_trunc;
            add_insn(parent, *bb, cast_op, cast_var, expr_var, NULL,
                     get_size(cast_var), NULL);

            /* Push the cast result */
            opstack_push(cast_var);

        } else if (is_compound_literal) {
            /* Create variable for compound literal result */
            var_t *compound_var =
                require_typed_var(parent, cast_or_literal_type);
            compound_var->var_name = gen_name();
            compound_var->is_compound_literal = true;
            compound_var->is_const_qualified = cast_const_qualified;
            compound_var->is_const_pointer = cast_const_pointer;
            compound_var->pointer_const_mask = cast_pointer_const_mask;

            /* Check if this is an array compound literal (int[]){...} */
            bool is_array_literal = (cast_ptr_level == -1);
            if (is_array_literal)
                cast_ptr_level = 0; /* Reset for normal processing */
            bool consumed_close_brace = false;

            /* parse_array_init() consumes its own opening brace. The older
             * one-dimensional literal helper expects it already consumed.
             */
            if (!is_array_literal ||
                (cast_array_dims <= 1 && !cast_array_element_ptr_level))
                lex_expect(T_open_curly);
            if (!is_array_literal ||
                (cast_array_dims <= 1 && !cast_array_element_ptr_level))
                reject_empty_initializer_in_strict_c99();
            /* Check if this is a pointer compound literal */
            if (is_array_literal) {
                compound_var->array_size = cast_array_size;
                compound_var->array_dim2 = cast_array_dim2;
                compound_var->array_dim3 = cast_array_dim3;
                compound_var->array_dim4 = cast_array_dim4;
                compound_var->has_unsized_array = cast_array_outer_unsized;
                compound_var->ptr_level = cast_array_element_ptr_level;
                compound_var->pointee_array_size = cast_pointee_array_size;
                compound_var->pointee_array_dim2 = cast_pointee_array_dim2;
                compound_var->pointee_array_dim3 = cast_pointee_array_dim3;
                compound_var->pointee_array_dim4 = cast_pointee_array_dim4;
                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);
                if (cast_array_dims > 1 || cast_array_element_ptr_level)
                    parse_array_init(compound_var, parent, bb, true);
                else
                    parse_array_compound_literal(compound_var, parent, bb);

                if (compound_var->array_size == 0) {
                    compound_var->init_val = 0;
                    add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                             NULL, 0, NULL);
                }
                opstack_push(compound_var);
                consumed_close_brace = true;
            } else if (cast_ptr_level > 0) {
                /* Pointer compound literal: (int*){&x} */
                var_t *initializer;

                compound_var->ptr_level = cast_ptr_level;

                /* Materialize the pointer object from the expression value,
                 * rather than treating a local address as an init_val constant.
                 * Compound literals have automatic storage and must retain a
                 * usable pointer value for later updates.
                 */
                if (!lex_peek(T_close_curly, NULL)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    initializer = opstack_pop();

                    if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                        error_at("Too many elements in scalar compound literal",
                                 cur_token_loc());
                } else {
                    /* Empty pointer compound literal: (int*){} */
                    initializer =
                        require_typed_var(parent, cast_or_literal_type);
                    initializer->ptr_level = cast_ptr_level;
                    initializer->var_name = gen_name();
                    initializer->init_val = 0;
                    add_insn(parent, *bb, OP_load_constant, initializer, NULL,
                             NULL, 0, NULL);
                }

                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);
                emit_object_assignment(parent, bb, compound_var, initializer);
                opstack_push(compound_var);

                /* Like scalar integer compound literals, a pointer compound
                 * literal is a block-lived modifiable object. Keep it as the
                 * assignment target for an immediately following `=`.
                 */
                compound_object = compound_var;
            } else if (is_record_type(cast_or_literal_type)) {
                /* Record compound literals use the same aggregate initializer
                 * path for structs, unions, and their typedef aliases.
                 */
                type_t *struct_type = cast_or_literal_type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);
                var_t *compound_addr =
                    require_ref_var(parent, compound_var->type, 0);
                compound_addr->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, compound_addr,
                         compound_var, NULL, 0, NULL);
                parse_struct_field_init(parent, bb, struct_type, compound_addr,
                                        true);
                compound_object = compound_var;
                opstack_push(compound_var);
            } else if (cast_or_literal_type->base_type == TYPE_int ||
                       cast_or_literal_type->base_type == TYPE_short ||
                       cast_or_literal_type->base_type == TYPE_char) {
                /* Handle empty compound literals */
                if (lex_peek(T_close_curly, NULL)) {
                    /* Empty compound literal: (int){} */
                    compound_var->init_val = 0;
                    compound_var->array_size = 0;
                    add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL,
                             0, NULL);
                    var_t *zero =
                        require_typed_var(parent, cast_or_literal_type);
                    zero->var_name = gen_name();
                    zero->init_val = 0;
                    add_insn(parent, *bb, OP_load_constant, zero, NULL, NULL, 0,
                             NULL);
                    emit_object_assignment(parent, bb, compound_var, zero);
                    opstack_push(compound_var);
                    compound_object = compound_var;
                } else if (lex_peek(T_numeric, NULL) ||
                           lex_peek(T_identifier, NULL) ||
                           lex_peek(T_char, NULL)) {
                    /* Parse first element */
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);

                    /* A scalar compound literal has one initializer; a trailing
                     * comma is permitted, but a second value is a C99
                     * constraint violation.
                     */
                    if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                        error_at("Too many elements in scalar compound literal",
                                 cur_token_loc());

                    /* Retained for the parser's array-literal extension; scalar
                     * spellings above have already rejected a second
                     * initializer.
                     */
                    if (lex_peek(T_comma, NULL)) {
                        /* Array compound literal: (int[]){1, 2, 3} */
                        var_t *first_element = opstack_pop();

                        /* Store elements temporarily */
                        var_t *elements[256];
                        elements[0] = first_element;
                        int element_count = 1;

                        /* Parse remaining elements */
                        while (lex_accept(T_comma)) {
                            if (lex_peek(T_close_curly, NULL))
                                break; /* Trailing comma */

                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                            if (element_count < 256) {
                                elements[element_count] = opstack_pop();
                            } else {
                                opstack_pop(); /* Discard if too many */
                            }
                            element_count++;
                        }

                        /* Set array metadata */
                        compound_var->array_size = element_count;
                        compound_var->init_val = first_element->init_val;

                        /* Allocate space for the array on stack */
                        add_insn(parent, *bb, OP_allocat, compound_var, NULL,
                                 NULL, 0, NULL);

                        /* Initialize each element */
                        for (int i = 0; i < element_count && i < 256; i++) {
                            if (!elements[i])
                                continue;

                            /* Store element at offset i * sizeof(element) */
                            var_t *elem_offset = require_var(parent);
                            elem_offset->init_val =
                                i * cast_or_literal_type->size;
                            elem_offset->var_name = gen_name();
                            add_insn(parent, *bb, OP_load_constant, elem_offset,
                                     NULL, NULL, 0, NULL);

                            /* Calculate address of element */
                            var_t *elem_addr = require_var(parent);
                            elem_addr->ptr_level = 1;
                            elem_addr->var_name = gen_name();
                            add_insn(parent, *bb, OP_add, elem_addr,
                                     compound_var, elem_offset, 0, NULL);

                            /* Store the element value */
                            add_insn(parent, *bb, OP_write, NULL, elem_addr,
                                     elements[i], cast_or_literal_type->size,
                                     NULL);
                        }

                        /* Store first element value for array-to-scalar */
                        compound_var->init_val = first_element->init_val;

                        /* Create result that provides first element access.
                         * This enables array compound literals in scalar
                         * contexts: int x = (int[]){1,2,3}; // x gets 1 int y =
                         * 5 + (int[]){10}; // adds 5 + 10
                         */
                        var_t *result_var = require_var(parent);
                        result_var->var_name = gen_name();
                        result_var->type = compound_var->type;
                        result_var->ptr_level = 0;
                        result_var->array_size = 0;

                        /* Read first element from the array */
                        add_insn(parent, *bb, OP_read, result_var, compound_var,
                                 NULL, compound_var->type->size, NULL);
                        opstack_push(result_var);
                    } else {
                        /* Single value: (int){42} - scalar compound literal */
                        var_t *initializer = opstack_pop();
                        add_insn(parent, *bb, OP_allocat, compound_var, NULL,
                                 NULL, 0, NULL);
                        emit_object_assignment(parent, bb, compound_var,
                                               initializer);
                        compound_object = compound_var;
                        opstack_push(compound_var);
                    }
                }
            }

            if (!consumed_close_brace)
                lex_expect(T_close_curly);

            /* Array compound literals are addressable automatic arrays. Carry
             * every direct postfix subscript through the flattened backing
             * object, scaling each one by the extent of the dimensions still to
             * its right. This keeps `(int[2][3]){...}[1][2]` an lvalue just
             * like an ordinary multidimensional array access.
             */
            if (is_array_literal && lex_peek(T_open_square, NULL)) {
                var_t *base = opstack_pop();
                var_t *address = base;
                var_t *element;
                int element_size = compound_var->type->size;
                int subscript_depth = 0;

                while (lex_accept(T_open_square)) {
                    var_t *index;
                    int stride = element_size;

                    if (!read_assignment_expression(parent, bb)) {
                        read_expr(parent, bb);
                        read_ternary_operation(parent, bb);
                    }
                    index = opstack_pop();
                    lex_expect(T_close_square);
                    if (subscript_depth == 0 && compound_var->array_dim2) {
                        stride *= compound_var->array_dim2;
                        if (compound_var->array_dim3)
                            stride *= compound_var->array_dim3;
                        if (compound_var->array_dim4)
                            stride *= compound_var->array_dim4;
                    } else if (subscript_depth == 1 &&
                               compound_var->array_dim3) {
                        stride *= compound_var->array_dim3;
                        if (compound_var->array_dim4)
                            stride *= compound_var->array_dim4;
                    } else if (subscript_depth == 2 &&
                               compound_var->array_dim4) {
                        stride *= compound_var->array_dim4;
                    }
                    if (stride != 1) {
                        var_t *scale = require_var(parent);
                        scale->var_name = gen_name();
                        scale->init_val = stride;
                        add_insn(parent, *bb, OP_load_constant, scale, NULL,
                                 NULL, 0, NULL);
                        var_t *scaled = require_var(parent);
                        scaled->var_name = gen_name();
                        add_insn(parent, *bb, OP_mul, scaled, index, scale, 0,
                                 NULL);
                        index = scaled;
                    }
                    var_t *indexed =
                        require_typed_ptr_var(parent, compound_var->type, 1);
                    indexed->var_name = gen_name();
                    add_insn(parent, *bb, OP_add, indexed, address, index, 0,
                             NULL);
                    address = indexed;
                    subscript_depth++;
                }

                if (subscript_depth < cast_array_dims) {
                    /* A partially selected row decays to its first element. It
                     * remains a pointer value, not a scalar read.
                     */
                    opstack_push(address);
                } else {
                    opcode_t compound_op = OP_generic;
                    if (lex_accept(T_assign) ||
                        accept_compound_assign_op(&compound_op)) {
                        if (compound_var->is_const_qualified)
                            error_at("assignment of read-only location",
                                     cur_token_loc());
                        var_t *value;
                        if (!read_assignment_expression(parent, bb)) {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                        }
                        value = opstack_pop();
                        if (compound_op != OP_generic) {
                            var_t *current =
                                require_typed_var(parent, compound_var->type);
                            current->var_name = gen_name();
                            add_insn(parent, *bb, OP_read, current, address,
                                     NULL, element_size, NULL);
                            if (!is_pointer_operation(compound_op, current,
                                                      value)) {
                                current = integer_promote_operand(parent, bb,
                                                                  current);
                                value =
                                    integer_promote_operand(parent, bb, value);
                                normalize_integer_binary_operands(
                                    parent, bb, compound_op, &current, &value);
                            }
                            var_t *combined = require_var(parent);
                            combined->var_name = gen_name();
                            combined->type = integer_binary_result_type(
                                compound_op, current, value);
                            add_insn(parent, *bb, compound_op, combined,
                                     current, value, 0, NULL);
                            value = combined;
                        }
                        add_insn(parent, *bb, OP_write, NULL, address, value,
                                 element_size, NULL);
                    }
                    if (lex_peek(T_increment, NULL) ||
                        lex_peek(T_decrement, NULL)) {
                        opcode_t op = OP_sub;
                        var_t *one;
                        var_t *updated;

                        if (lex_accept(T_increment))
                            op = OP_add;
                        else
                            lex_expect(T_decrement);

                        if (compound_var->is_const_qualified)
                            error_at("assignment of read-only location",
                                     cur_token_loc());
                        element = require_typed_var(parent, compound_var->type);
                        element->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, element, address, NULL,
                                 element_size, NULL);
                        one = require_typed_var(parent, TY_int);
                        one->var_name = gen_name();
                        one->init_val = 1;
                        add_insn(parent, *bb, OP_load_constant, one, NULL, NULL,
                                 0, NULL);
                        updated = require_var(parent);
                        updated->var_name = gen_name();
                        updated->type =
                            integer_binary_result_type(op, element, one);
                        add_insn(parent, *bb, op, updated, element, one, 0,
                                 NULL);
                        updated = resize_var(parent, bb, updated, element);
                        add_insn(parent, *bb, OP_write, NULL, address, updated,
                                 element_size, NULL);
                        opstack_push(element);
                    } else {
                        element = require_typed_var(parent, compound_var->type);
                        element->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, element, address, NULL,
                                 element_size, NULL);
                        element->is_compound_literal_reference = true;
                        element->is_const_qualified =
                            compound_var->is_const_qualified;
                        element->compound_literal_address = address;
                        opstack_push(element);
                    }
                }
            }

            /* A record compound literal is likewise an automatic object, so
             * preserve a direct member postfix as an addressable lvalue.
             */
            if (compound_object && is_record_type(cast_or_literal_type) &&
                lex_accept(T_dot)) {
                char field_name[MAX_ID_LEN];
                var_t *base = opstack_pop();
                var_t *base_addr = require_ref_var(parent, base->type, 0);
                var_t *field;
                var_t *address;
                var_t *element;
                opcode_t compound_op = OP_generic;
                type_t *value_type;
                int value_ptr_level;
                int value_size;
                type_t *record_type = cast_or_literal_type;

                base_addr->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, base_addr, base, NULL, 0,
                         NULL);
                address = base_addr;
                while (1) {
                    lex_ident(T_identifier, field_name);
                    field = find_member(field_name, record_type);
                    if (!field)
                        error_at("Unknown record member", cur_token_loc());
                    address = compute_field_address(parent, bb, address, field);

                    /* The member-chain delimiter is already a token stream
                     * property. Advance it directly rather than depending on a
                     * boolean helper result after the current field's IR
                     * lowering; this keeps the syntactic chain intact through
                     * self-hosted target builds as well.
                     */
                    if (!cur_token->next || cur_token->next->kind != T_dot)
                        break;
                    lex_next();
                    if (!is_record_type(field->type) || field->ptr_level ||
                        field->array_size)
                        error_at("Member access requires a record",
                                 cur_token_loc());
                    record_type = field->type;
                }
                value_type = field->type;
                value_ptr_level = field->ptr_level;
                value_size = field->type->size;

                /* Keep array-member subscripts as lvalues. The field address
                 * starts at element zero; each index is scaled by the complete
                 * extent of the remaining dimensions, just as ordinary array
                 * postfix parsing does.
                 */
                int subscript_depth = 0;
                while (field->array_size && lex_accept(T_open_square)) {
                    var_t *index;
                    int stride = value_size;

                    if (!read_assignment_expression(parent, bb)) {
                        read_expr(parent, bb);
                        read_ternary_operation(parent, bb);
                    }
                    index = opstack_pop();
                    lex_expect(T_close_square);
                    if (subscript_depth == 0 && field->array_dim2 > 0) {
                        stride *= field->array_dim2;
                        if (field->array_dim3 > 0)
                            stride *= field->array_dim3;
                        if (field->array_dim4 > 0)
                            stride *= field->array_dim4;
                    } else if (subscript_depth == 1 && field->array_dim3 > 0) {
                        stride *= field->array_dim3;
                        if (field->array_dim4 > 0)
                            stride *= field->array_dim4;
                    } else if (subscript_depth == 2 && field->array_dim4 > 0) {
                        stride *= field->array_dim4;
                    }
                    if (stride != 1) {
                        var_t *scale = require_var(parent);
                        scale->var_name = gen_name();
                        scale->init_val = stride;
                        add_insn(parent, *bb, OP_load_constant, scale, NULL,
                                 NULL, 0, NULL);
                        var_t *scaled = require_var(parent);
                        scaled->var_name = gen_name();
                        add_insn(parent, *bb, OP_mul, scaled, index, scale, 0,
                                 NULL);
                        index = scaled;
                    }
                    var_t *indexed =
                        require_typed_ptr_var(parent, value_type, 1);
                    indexed->var_name = gen_name();
                    add_insn(parent, *bb, OP_add, indexed, address, index, 0,
                             NULL);
                    address = indexed;
                    value_ptr_level = 0;
                    subscript_depth++;
                }

                if (lex_accept(T_assign) ||
                    accept_compound_assign_op(&compound_op)) {
                    if (compound_object->is_const_qualified)
                        error_at("assignment of read-only location",
                                 cur_token_loc());
                    var_t *value;
                    if (!read_assignment_expression(parent, bb)) {
                        read_expr(parent, bb);
                        read_ternary_operation(parent, bb);
                    }
                    value = opstack_pop();
                    if (compound_op != OP_generic) {
                        var_t *current;
                        if (is_bitfield(field))
                            current =
                                read_bitfield_value(parent, bb, address, field);
                        else {
                            current = require_typed_var(parent, value_type);
                            current->ptr_level = value_ptr_level;
                            current->var_name = gen_name();
                            add_insn(parent, *bb, OP_read, current, address,
                                     NULL, value_size, NULL);
                        }
                        if (!is_pointer_operation(compound_op, current,
                                                  value)) {
                            current =
                                integer_promote_operand(parent, bb, current);
                            value = integer_promote_operand(parent, bb, value);
                            normalize_integer_binary_operands(
                                parent, bb, compound_op, &current, &value);
                        }
                        var_t *combined = require_var(parent);
                        combined->var_name = gen_name();
                        combined->type = integer_binary_result_type(
                            compound_op, current, value);
                        add_insn(parent, *bb, compound_op, combined, current,
                                 value, 0, NULL);
                        value = combined;
                    }
                    if (is_bitfield(field))
                        write_bitfield_value(parent, bb, address, value, field);
                    else
                        add_insn(parent, *bb, OP_write, NULL, address, value,
                                 value_size, NULL);
                }

                if (lex_peek(T_increment, NULL) ||
                    lex_peek(T_decrement, NULL)) {
                    opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
                    var_t *old;
                    var_t *one;
                    var_t *updated;

                    if (compound_object->is_const_qualified)
                        error_at("assignment of read-only location",
                                 cur_token_loc());
                    if (is_bitfield(field))
                        old = read_bitfield_value(parent, bb, address, field);
                    else {
                        old = require_typed_var(parent, value_type);
                        old->ptr_level = value_ptr_level;
                        old->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, old, address, NULL,
                                 value_size, NULL);
                    }
                    one = require_typed_var(parent, TY_int);
                    one->var_name = gen_name();
                    one->init_val = 1;
                    add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0,
                             NULL);
                    updated = require_var(parent);
                    updated->var_name = gen_name();
                    updated->type = integer_binary_result_type(op, old, one);
                    add_insn(parent, *bb, op, updated, old, one, 0, NULL);
                    updated = resize_var(parent, bb, updated, old);
                    if (is_bitfield(field))
                        write_bitfield_value(parent, bb, address, updated,
                                             field);
                    else
                        add_insn(parent, *bb, OP_write, NULL, address, updated,
                                 value_size, NULL);
                    element = old;
                } else if (is_bitfield(field)) {
                    element = read_bitfield_value(parent, bb, address, field);
                    element->is_bitfield = false;
                } else {
                    element = require_typed_var(parent, value_type);
                    element->ptr_level = value_ptr_level;
                    element->var_name = gen_name();
                    add_insn(parent, *bb, OP_read, element, address, NULL,
                             value_size, NULL);
                }

                /* A scalar member of a compound literal remains a modifiable
                 * lvalue. Retain its storage location for a surrounding prefix
                 * ++/--, including bit-fields which need the masked write path.
                 * Partially selected array rows decay instead.
                 */
                int field_dims = field->array_size ? 1 : 0;
                if (field->array_dim2)
                    field_dims++;
                if (field->array_dim3)
                    field_dims++;
                if (field->array_dim4)
                    field_dims++;
                if (!field->array_size || subscript_depth >= field_dims) {
                    element->is_compound_literal_reference = true;
                    element->is_const_qualified =
                        compound_object->is_const_qualified;
                    element->compound_literal_address = address;
                    if (is_bitfield(field))
                        element->compound_literal_bitfield = field;
                }
                opstack_push(element);
            }

            /* A non-const scalar or record compound literal is a block-lived
             * object in C99, hence a modifiable lvalue. The ordinary parser
             * previously collapsed scalar literals to their initializer value,
             * losing that property before an immediately following assignment
             * could be recognized.
             */
            if (compound_object && !is_record_type(cast_or_literal_type) &&
                (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL))) {
                opcode_t op = lex_accept(T_increment) ? OP_add : OP_sub;
                var_t *old;
                var_t *one;
                var_t *updated;

                if (compound_object->is_const_qualified ||
                    compound_object->is_const_pointer)
                    error_at("assignment of read-only location",
                             cur_token_loc());
                opstack_pop();
                old = require_typed_var(parent, compound_object->type);
                old->ptr_level = compound_object->ptr_level;
                old->var_name = gen_name();
                add_insn(parent, *bb, OP_assign, old, compound_object, NULL, 0,
                         NULL);
                one = require_typed_var(parent, TY_int);
                one->var_name = gen_name();
                one->init_val = 1;
                add_insn(parent, *bb, OP_load_constant, one, NULL, NULL, 0,
                         NULL);
                if (is_pointer_operation(op, compound_object, one)) {
                    handle_pointer_arithmetic(parent, bb, op, compound_object,
                                              one);
                    updated = opstack_pop();
                } else {
                    updated = require_var(parent);
                    updated->var_name = gen_name();
                    updated->type =
                        integer_binary_result_type(op, compound_object, one);
                    add_insn(parent, *bb, op, updated, compound_object, one, 0,
                             NULL);
                }
                updated = resize_var(parent, bb, updated, compound_object);
                add_insn(parent, *bb, OP_assign, compound_object, updated, NULL,
                         0, NULL);
                opstack_push(old);
            }
            opcode_t scalar_compound_op = OP_generic;
            if (compound_object &&
                (lex_accept(T_assign) ||
                 accept_compound_assign_op(&scalar_compound_op))) {
                if (compound_object->is_const_qualified ||
                    compound_object->is_const_pointer)
                    error_at("assignment of read-only location",
                             cur_token_loc());
                opstack_pop();
                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                }
                var_t *value = opstack_pop();
                if (scalar_compound_op != OP_generic) {
                    var_t *current = compound_object;

                    if (is_pointer_operation(scalar_compound_op, current,
                                             value)) {
                        handle_pointer_arithmetic(
                            parent, bb, scalar_compound_op, current, value);
                        value = opstack_pop();
                    } else {
                        current = integer_promote_operand(parent, bb, current);
                        value = integer_promote_operand(parent, bb, value);
                        normalize_integer_binary_operands(
                            parent, bb, scalar_compound_op, &current, &value);
                        var_t *combined = require_var(parent);
                        combined->var_name = gen_name();
                        combined->type = integer_binary_result_type(
                            scalar_compound_op, current, value);
                        add_insn(parent, *bb, scalar_compound_op, combined,
                                 current, value, 0, NULL);
                        value = combined;
                    }
                }
                emit_object_assignment(parent, bb, compound_object, value);
                opstack_push(compound_object);
            }
        } else {
            /* Regular parenthesized expression */
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }
            while (lex_accept(T_comma)) {
                /* A comma inside an explicit parenthesized expression is the
                 * C99 sequencing operator, not an argument or initializer
                 * delimiter. Discard the completed left value after its IR is
                 * emitted; the final expression supplies the result.
                 */
                opstack_pop();
                perform_side_effect(parent, *bb);
                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                }
            }
            lex_expect(T_close_bracket);

            bool lowered_call_result_postfix = false;
            if (lex_peek(T_open_square, NULL)) {
                var_t *grouped = operand_stack[operand_stack_idx - 1];

                if (grouped->pointee_array_size) {
                    lower_call_result_array_postfix(&grouped, parent, bb);
                    lowered_call_result_postfix = true;
                }
            }

            /* A parenthesized pointer/string expression is still a postfix
             * operand. Unlike identifier lvalues it has no declaration to feed
             * read_lvalue(), so lower its first subscript directly while
             * retaining the same explicit element-size scaling.
             */
            if (!lowered_call_result_postfix && lex_accept(T_open_square)) {
                var_t *base = opstack_pop();
                var_t *index;
                var_t *address;
                int element_size;

                if (base->array_size || base->has_unsized_array) {
                    int subscript_depth = 0;
                    int array_dims = 1 + !!base->array_dim2 +
                                     !!base->array_dim3 + !!base->array_dim4;

                    /* A grouping around an array compound literal preserves its
                     * array type. Keep lowering each following index against
                     * the original shape, just as the direct compound-literal
                     * postfix path does.
                     */
                    address = base;
                    element_size = base->type->size;
                    do {
                        int stride = element_size;

                        if (!read_assignment_expression(parent, bb)) {
                            read_expr(parent, bb);
                            read_ternary_operation(parent, bb);
                        }
                        index = opstack_pop();
                        lex_expect(T_close_square);
                        if (subscript_depth == 0 && base->array_dim2) {
                            stride *= base->array_dim2;
                            if (base->array_dim3)
                                stride *= base->array_dim3;
                            if (base->array_dim4)
                                stride *= base->array_dim4;
                        } else if (subscript_depth == 1 && base->array_dim3) {
                            stride *= base->array_dim3;
                            if (base->array_dim4)
                                stride *= base->array_dim4;
                        } else if (subscript_depth == 2 && base->array_dim4) {
                            stride *= base->array_dim4;
                        }
                        if (stride != 1) {
                            var_t *scale = require_var(parent);
                            var_t *scaled = require_var(parent);

                            scale->var_name = gen_name();
                            scale->init_val = stride;
                            add_insn(parent, *bb, OP_load_constant, scale, NULL,
                                     NULL, 0, NULL);
                            scaled->var_name = gen_name();
                            add_insn(parent, *bb, OP_mul, scaled, index, scale,
                                     0, NULL);
                            index = scaled;
                        }
                        var_t *indexed =
                            require_typed_ptr_var(parent, base->type, 1);
                        indexed->var_name = gen_name();
                        add_insn(parent, *bb, OP_add, indexed, address, index,
                                 0, NULL);
                        address = indexed;
                        subscript_depth++;
                    } while (lex_accept(T_open_square));

                    if (subscript_depth < array_dims) {
                        opstack_push(address);
                    } else {
                        vd = require_typed_var(parent, base->type);
                        vd->var_name = gen_name();
                        opstack_push(vd);
                        add_insn(parent, *bb, OP_read, vd, address, NULL,
                                 element_size, NULL);
                    }
                } else {
                    if (!base->ptr_level && !base->type->ptr_level)
                        error_at("Cannot apply square operator to non-pointer",
                                 cur_token_loc());
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    index = opstack_pop();
                    lex_expect(T_close_square);
                    element_size = base->type->size;
                    if (element_size != 1) {
                        var_t *scale = require_var(parent);
                        scale->var_name = gen_name();
                        scale->init_val = element_size;
                        add_insn(parent, *bb, OP_load_constant, scale, NULL,
                                 NULL, 0, NULL);
                        var_t *scaled = require_var(parent);
                        scaled->var_name = gen_name();
                        add_insn(parent, *bb, OP_mul, scaled, index, scale, 0,
                                 NULL);
                        index = scaled;
                    }
                    address = require_typed_ptr_var(parent, base->type, 1);
                    address->var_name = gen_name();
                    add_insn(parent, *bb, OP_add, address, base, index, 0,
                             NULL);
                    vd = require_typed_var(parent, base->type);
                    vd->ptr_level =
                        base->ptr_level > 1 ? base->ptr_level - 1 : 0;
                    vd->var_name = gen_name();
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, address, NULL,
                             vd->ptr_level ? PTR_SIZE : element_size, NULL);
                }
            }

            /* Grouping does not stop a postfix member chain. The expression
             * result is an aggregate value rather than an identifier lvalue, so
             * materialize its address and lower the selected member here. This
             * covers the ordinary C99 spellings `(*p).field` and
             * `(*record.member).field`.
             */
            while (lex_accept(T_dot)) {
                char token[MAX_ID_LEN];
                var_t *base = opstack_pop();
                var_t *field;
                var_t *base_address;
                var_t *address;
                var_t *offset;

                if (!base->type || !is_record_type(base->type) ||
                    effective_pointer_depth(base))
                    error_at("Cannot apply dot operator to non-record",
                             cur_token_loc());
                lex_ident(T_identifier, token);
                field = find_member(token, base->type);
                if (!field)
                    error_at("Unknown struct or union member",
                             next_token_loc());

                base_address = require_typed_ptr_var(parent, base->type, 1);
                base_address->var_name = gen_name();
                add_insn(parent, *bb, OP_address_of, base_address, base, NULL,
                         0, NULL);
                offset = require_var(parent);
                offset->var_name = gen_name();
                offset->init_val = field->offset;
                add_insn(parent, *bb, OP_load_constant, offset, NULL, NULL, 0,
                         NULL);
                address = require_typed_ptr_var(parent, field->type, 1);
                address->var_name = gen_name();
                add_insn(parent, *bb, OP_add, address, base_address, offset, 0,
                         NULL);

                vd = require_typed_var(parent, field->type);
                vd->ptr_level = field->ptr_level;
                vd->func_signature = field->func_signature;
                vd->is_const_qualified = field->is_const_qualified;
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, address, NULL,
                         field->ptr_level || field->type->ptr_level
                             ? PTR_SIZE
                             : get_size(field),
                         NULL);
            }

            /* Function calls are postfix expressions, so a parenthesized
             * function designator remains callable: `(fn)(...)` and
             * `(*fp)(...)` have the same meaning as `fn(...)` and `fp(...)`.
             */
            if (lex_peek(T_open_bracket, NULL)) {
                var_t *callee = operand_stack[operand_stack_idx - 1];
                func_t *signature = get_func_signature(callee);

                if (signature) {
                    if (!callee->ptr_level) {
                        opstack_pop();
                        opstack_push(
                            load_function_pointer_object(parent, bb, callee));
                    }
                    vd = emit_indirect_call_result(callee, signature, true,
                                                   parent, bb);
                } else if (callee->is_func) {
                    signature = find_func(callee->var_name);
                    if (!signature)
                        error_at("Called object is not a function",
                                 cur_token_loc());
                    opstack_pop();
                    vd = emit_direct_call_result(signature, true, parent, bb);
                } else {
                    error_at("Called object is not a function pointer",
                             cur_token_loc());
                }
                lower_call_result_array_postfix(&vd, parent, bb);
            }
        }
    } else if (lex_accept(T_sizeof)) {
        handle_sizeof_operator(parent, bb);
    } else {
        /* function call, constant or variable - read token and determine */
        opcode_t prefix_op = OP_generic;
        char token[MAX_ID_LEN];

        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;

        lex_peek(T_identifier, token);

        /* is a constant or variable? */
        const constant_t *con = find_scoped_constant(token, parent);
        var_t *var = find_var(token, parent);
        func_t *func = find_visible_func(token, parent);

        /* A block-scope function prototype shadows an automatic object, but its
         * designator is the file-scope function rather than an indirect call
         * through that object.
         */
        if (var && var->is_extern_function_alias)
            var = NULL;

        if (!strcmp(token, "__builtin_offsetof")) {
            read_builtin_offsetof(parent, bb);
        } else if (!strcmp(token, "__builtin_va_arg")) {
            read_builtin_va_arg(parent, bb);
        } else if (!strcmp(token, "__func__")) {
            if (!parent->func || !parent->func->return_def.var_name[0])
                error_at("__func__ is only defined inside a function",
                         next_token_loc());
            lex_expect(T_identifier);
            vd = require_typed_ptr_var(parent, TY_char, true);
            vd->var_name = gen_name();
            vd->init_val = write_symbol(parent->func->return_def.var_name);
            vd->is_string_literal = true;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_rodata_address, vd, NULL, NULL, 0,
                     NULL);
            if (lex_accept(T_open_square)) {
                var_t *base = opstack_pop();
                var_t *index;
                var_t *address;

                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
                index = opstack_pop();
                lex_expect(T_close_square);
                address = require_typed_ptr_var(parent, TY_char, true);
                address->var_name = gen_name();
                add_insn(parent, *bb, OP_add, address, base, index, 0, NULL);
                vd = require_typed_var(parent, TY_char);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, address, NULL, 1, NULL);
            }
        } else if (con) {
            vd = require_var(parent);
            vd->init_val = con->value;
            vd->var_name = gen_name();
            opstack_push(vd);
            lex_expect(T_identifier);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
        } else if (var) {
            /* evalue lvalue expression */
            lvalue_t lvalue;
            bool deferred_call_prefix =
                prefix_op != OP_generic && cur_token->next->next &&
                cur_token->next->next->kind == T_open_bracket;

            read_lvalue(&lvalue, var, parent, bb, true,
                        deferred_call_prefix ? OP_generic : prefix_op, true);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                var_t *callee = operand_stack[operand_stack_idx - 1];
                func_t *signature = get_func_signature(lvalue.decl);

                /* An indexed callback typedef has a reference to the selected
                 * element on the operand stack. Its declaration is the array
                 * (whose direct-call signature is intentionally absent), but
                 * the selected element's type retains the callback prototype.
                 */
                if (!signature && lvalue.is_reference && lvalue.type &&
                    !lvalue.value_ptr_level)
                    signature = lvalue.type->func_signature;

                if (!signature)
                    error_at("Called object is not a function pointer",
                             cur_token_loc());

                /* A standalone function-pointer object is an lvalue, so the
                 * operand stack still holds its storage location here. Calls
                 * need the stored code address instead. Member pointers have
                 * already been read by read_lvalue() because they are
                 * references; only materialize the load for an object itself.
                 */
                if (!lvalue.is_reference && lvalue.decl->func_signature) {
                    var_t *object = opstack_pop();
                    opstack_push(
                        load_function_pointer_object(parent, bb, object));
                    callee = operand_stack[operand_stack_idx - 1];
                } else if (lvalue.is_reference) {
                    /* The element/member load already produced the pointer
                     * value. Carry its prototype and pointer-valued IR shape
                     * into indirect-call lowering.
                     */
                    callee->func_signature = signature;
                    callee->ptr_level = 1;
                }
                vd = emit_indirect_call_result(callee, signature, true, parent,
                                               bb);
                lower_call_result_array_postfix(&vd, parent, bb);
                lower_call_result_prefix_update(
                    &vd, deferred_call_prefix ? prefix_op : OP_generic, parent,
                    bb);
            }
        } else if (func) {
            if (parent->func && parent->func->is_inline &&
                !parent->func->is_static && func->is_static)
                error_at(
                    "external inline definition references internal-linkage "
                    "function",
                    next_token_loc());
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                func_t *returned_signature =
                    func->return_def.type->func_signature;
                vd = emit_direct_call_result(func, true, parent, bb);
                lower_call_result_array_postfix(&vd, parent, bb);
                lower_call_result_prefix_update(&vd, prefix_op, parent, bb);
                if (returned_signature && lex_peek(T_open_bracket, NULL)) {
                    vd = emit_indirect_call_result(vd, returned_signature, true,
                                                   parent, bb);
                }
            } else {
                /* indirective function pointer assignment */
                vd = require_func_symbol_var(parent);
                vd->is_func = true;
                vd->var_name = intern_string(token);
                vd->func_target = func;
                opstack_push(vd);
            }
        } else if (lex_accept(T_open_curly)) {
            parse_array_literal_expr(parent, bb);
        } else {
            /* unknown expression */
            error_at("Unrecognized expression token", next_token_loc());
        }

        if (is_neg) {
            rs1 = opstack_pop();
            if (is_pointer_like_value(rs1) || rs1->is_func)
                error_at("unary minus requires an arithmetic operand",
                         cur_token_loc());
            rs1 = integer_promote_operand(parent, bb, rs1);

            /* Constant folding for negation */
            if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = rs1->type;
                vd->is_const = true;
                vd->init_val = -rs1->init_val;
                vd->init_val_hi = ~rs1->init_val_hi + (vd->init_val == 0);
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
            } else {
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = rs1->type;
                opstack_push(vd);
                add_insn(parent, *bb, OP_negate, vd, rs1, NULL, 0, NULL);
            }
        }
    }
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      basic_block_t *shared_bb);

bool is_logical(opcode_t op)
{
    return op == OP_log_and || op == OP_log_or;
}

/* Consume a compound-assignment operator ("+=", "-=", ...) and report the
 * arithmetic it applies.
 *
 * Returns false and consumes nothing when the next token is not one, so it can
 * sit in an else-if chain beside the other statement forms.
 */
bool accept_compound_assign_op(opcode_t *op)
{
    if (lex_accept(T_pluseq))
        op[0] = OP_add;
    else if (lex_accept(T_minuseq))
        op[0] = OP_sub;
    else if (lex_accept(T_asteriskeq))
        op[0] = OP_mul;
    else if (lex_accept(T_divideeq))
        op[0] = OP_div;
    else if (lex_accept(T_modeq))
        op[0] = OP_mod;
    else if (lex_accept(T_lshifteq))
        op[0] = OP_lshift;
    else if (lex_accept(T_rshifteq))
        op[0] = OP_rshift;
    else if (lex_accept(T_xoreq))
        op[0] = OP_bit_xor;
    else if (lex_accept(T_oreq))
        op[0] = OP_bit_or;
    else if (lex_accept(T_andeq))
        op[0] = OP_bit_and;
    else
        return false;
    return true;
}

bool lvalue_write_follows(opcode_t prefix_op)
{
    return prefix_op != OP_generic || lex_peek(T_assign, NULL) ||
           lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL) ||
           lex_peek(T_pluseq, NULL) || lex_peek(T_minuseq, NULL) ||
           lex_peek(T_asteriskeq, NULL) || lex_peek(T_divideeq, NULL) ||
           lex_peek(T_modeq, NULL) || lex_peek(T_lshifteq, NULL) ||
           lex_peek(T_rshifteq, NULL) || lex_peek(T_xoreq, NULL) ||
           lex_peek(T_oreq, NULL) || lex_peek(T_andeq, NULL);
}

int get_pointer_element_size(var_t *ptr_var)
{
    int pointer_depth;

    if (!ptr_var || !ptr_var->type)
        return PTR_SIZE; /* Default to pointer size */

    pointer_depth = effective_pointer_depth(ptr_var);

    /* An array of pointers decays to a pointer-to-pointer. The declaration
     * records its element's indirection level, which may be held in a typedef.
     * Account for the decay before deriving the pointed-to object size.
     */
    if ((ptr_var->array_size || ptr_var->has_unsized_array) && pointer_depth)
        return PTR_SIZE;

    /* Direct pointer with type info.
     *
     * Only a single level of indirection points at the base type. For deeper
     * pointers (int **, char ***, ...) the element is itself a pointer, so the
     * step is PTR_SIZE. Returning the base type size there makes "q + 1"
     * advance by 4 instead of 8 on LP64 and drops a level of type information
     * from the result.
     */
    if (pointer_depth) {
        if (pointer_depth > 1)
            return PTR_SIZE;

        /* A single typedef-hidden pointer still advances by its underlying
         * pointee, not by the alias object's pointer-sized representation.
         */
        return pointer_typedef_pointee_size(
            ptr_var->type, pointee_type_from_pointer_typedef(ptr_var->type));
    }

    /* Typedef pointer or array-derived pointer */
    switch (ptr_var->type->base_type) {
    case TYPE_char:
        return TY_char->size;
    case TYPE_short:
        return TY_short->size;
    case TYPE_int:
        return TY_int->size;
    case TYPE_void:
        return 1;
    default:
        break;
    }

    return ptr_var->type->size ? ptr_var->type->size : PTR_SIZE;
}

/* A direct void pointer has no complete pointed-to object type. A pointer to
 * void pointer (void **) is different: its elements are pointer objects and
 * therefore have a known size.
 */
bool is_direct_void_pointer(const var_t *var)
{
    if (!var || !var->type || var->type->base_type != TYPE_void)
        return false;

    /* An array of void pointers decays to void ** before arithmetic. Its
     * elements are complete pointer objects, even though the declared base type
     * and direct declarator depth otherwise resemble void *.
     */
    if (var->array_size || var->has_unsized_array)
        return false;
    return var->ptr_level == 1 ||
           (var->ptr_level == 0 && var->type->ptr_level == 1);
}

bool is_direct_void_pointer_type(const type_t *type, int ptr_level)
{
    return type && type->base_type == TYPE_void &&
           (ptr_level == 1 || (ptr_level == 0 && type->ptr_level == 1));
}

/* Helper function to handle pointer arithmetic (add/sub with scaling) */
void handle_pointer_arithmetic(block_t *parent,
                               basic_block_t **bb,
                               opcode_t op,
                               var_t *rs1,
                               var_t *rs2)
{
    var_t *ptr_var = NULL;
    var_t *int_var = NULL;
    int element_size = 0;

    /* Functions are not objects, so no form of C99 pointer arithmetic may use a
     * function pointer. Keep this before the add/sub split below: only the
     * subtraction path performs the more specific compatible-pointee check.
     */
    if ((rs1 && rs1->is_func) || (rs2 && rs2->is_func))
        error_at("Pointer arithmetic requires object pointers",
                 cur_token_loc());

    if (is_direct_void_pointer(rs1) || is_direct_void_pointer(rs2))
        error_at("Pointer arithmetic on void* is invalid", cur_token_loc());

    /* Pointer arithmetic: differences (char*, int*, struct*, etc.),
     * addition/increment with scaling, and array indexing.
     */

    /* Check if both operands are pointers (pointer difference) */
    if (op == OP_sub) {
        /* If both are variables (not temporaries), look them up */
        var_t *orig_rs1 = rs1, *orig_rs2 = rs2;

        /* If they have names, they might be variable references - look them up
         */
        if (rs1->var_name[0]) {
            var_t *found = find_var(rs1->var_name, parent);
            if (found)
                orig_rs1 = found;
        }
        if (rs2->var_name[0]) {
            var_t *found = find_var(rs2->var_name, parent);
            if (found)
                orig_rs2 = found;
        }

        /* Check if both have ptr_level or typedef pointer type */
        bool rs1_is_ptr = is_pointer_like_value(orig_rs1) || orig_rs1->is_func;
        bool rs2_is_ptr = is_pointer_like_value(orig_rs2) || orig_rs2->is_func;

        /* If variable lookup failed, check the passed variables directly */
        if (!rs1_is_ptr)
            rs1_is_ptr = is_pointer_like_value(rs1) || rs1->is_func;
        if (!rs2_is_ptr)
            rs2_is_ptr = is_pointer_like_value(rs2) || rs2->is_func;

        if (rs1_is_ptr && rs2_is_ptr) {
            /* Both are pointers - this is pointer difference Determine element
             * size C99 6.5.6 confines pointer subtraction to pointers to
             * complete object types. A function pointer is pointer-like for
             * calls and comparisons, but it has no object elements to count.
             */
            if (orig_rs1->is_func || orig_rs2->is_func)
                error_at("Pointer subtraction requires object pointers",
                         cur_token_loc());
            type_t *left_pointee =
                pointee_type_from_pointer_typedef(orig_rs1->type);
            type_t *right_pointee =
                pointee_type_from_pointer_typedef(orig_rs2->type);
            int left_depth = orig_rs1->ptr_level + orig_rs1->type->ptr_level;
            int right_depth = orig_rs2->ptr_level + orig_rs2->type->ptr_level;
            bool left_scalar_row = is_scalar_pointee_array_pointer(orig_rs1);
            bool right_scalar_row = is_scalar_pointee_array_pointer(orig_rs2);

            if ((left_scalar_row || right_scalar_row) &&
                !scalar_pointee_array_shapes_compatible(orig_rs1, orig_rs2))
                error_at(
                    "Pointer subtraction requires compatible pointed-to types",
                    cur_token_loc());
            if (!compatible_decl_type(left_pointee, right_pointee) ||
                left_depth != right_depth)
                error_at(
                    "Pointer subtraction requires compatible pointed-to types",
                    cur_token_loc());

            element_size = PTR_SIZE; /* Default */

            element_size = left_scalar_row
                               ? scalar_pointee_array_row_stride(orig_rs1)
                               : get_pointer_element_size(orig_rs1);

            /* Perform subtraction first */
            var_t *diff = require_var(parent);
            diff->var_name = gen_name();
            add_insn(parent, *bb, OP_sub, diff, rs1, rs2, 0, NULL);

            /* Then divide by element size if needed */
            if (element_size > 1) {
                var_t *size_const = require_var(parent);
                size_const->var_name = gen_name();
                size_const->init_val = element_size;
                add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL,
                         0, NULL);

                var_t *result = require_var(parent);
                result->var_name = gen_name();
                add_insn(parent, *bb, OP_div, result, diff, size_const, 0,
                         NULL);
                opstack_push(result);
            } else {
                opstack_push(diff);
            }
            return;
        }
    }
    /* Determine which operand is the pointer for regular pointer arithmetic */
    if (is_pointer_like_value(rs1)) {
        ptr_var = rs1;
        int_var = rs2;
        element_size =
            is_scalar_pointee_array_pointer(rs1)
                ? scalar_pointee_array_row_stride(rs1)
                : (is_array_declarator(rs1) && rs1->array_dim2 &&
                           !has_effective_pointer(rs1) && !rs1->is_func
                       ? fixed_array_decay_stride(rs1)
                       : get_pointer_element_size(rs1));
    } else if (is_pointer_like_value(rs2)) {
        /* Only for addition (p + n == n + p) */
        if (op == OP_add) {
            ptr_var = rs2;
            int_var = rs1;
            element_size =
                is_scalar_pointee_array_pointer(rs2)
                    ? scalar_pointee_array_row_stride(rs2)
                    : (is_array_declarator(rs2) && rs2->array_dim2 &&
                               !has_effective_pointer(rs2) && !rs2->is_func
                           ? fixed_array_decay_stride(rs2)
                           : get_pointer_element_size(rs2));
            /* Swap operands so pointer is rs1 */
            rs1 = ptr_var;
            rs2 = int_var;
        }
    }

    /* If we need to scale the integer operand */
    if (ptr_var && element_size > 1) {
        /* Create multiplication by element size */
        var_t *size_const = require_var(parent);
        size_const->var_name = gen_name();
        size_const->init_val = element_size;
        add_insn(parent, *bb, OP_load_constant, size_const, NULL, NULL, 0,
                 NULL);

        var_t *scaled = require_var(parent);
        scaled->var_name = gen_name();
        add_insn(parent, *bb, OP_mul, scaled, int_var, size_const, 0, NULL);

        /* Use scaled value as rs2 */
        rs2 = scaled;
    }

    /* Perform the operation */
    var_t *vd = require_var(parent);
    /* Preserve pointer type metadata on results of pointer arithmetic */
    if (ptr_var) {
        vd->type = ptr_var->type;

        /* An array operand decays to a pointer before arithmetic. Retain that
         * extra level on the result so `slots + 0` for an array of pointer
         * typedefs cannot be mistaken for a direct callback value.
         */
        vd->ptr_level = ptr_var->ptr_level + !!ptr_var->array_size;
        if (is_scalar_pointee_array_pointer(ptr_var))
            copy_pointee_array_shape(vd, ptr_var);
        else if (is_array_declarator(ptr_var) && ptr_var->array_dim2 &&
                 !has_effective_pointer(ptr_var) && !ptr_var->is_func) {
            fixed_array_shape_t shape = fixed_array_shape_from_var(ptr_var);

            fixed_array_shape_drop_outer(&shape);
            fixed_array_shape_to_pointee_var(vd, &shape);
        }
    }
    vd->var_name = gen_name();
    opstack_push(vd);
    add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);
}

/* Helper function to check if pointer arithmetic is needed */
bool is_pointer_operation(opcode_t op, var_t *rs1, var_t *rs2)
{
    if (op != OP_add && op != OP_sub)
        return false;

    return is_pointer_like_value(rs1) || is_pointer_like_value(rs2) ||
           (rs1 && rs1->is_func) || (rs2 && rs2->is_func);
}

/* The first unsigned slice has the existing int/short/char widths. Narrow
 * unsigned operands promote to int because int represents their full range; an
 * unsigned int operand gives the arithmetic result unsigned int.
 */
bool unsigned_int_operand(const var_t *var)
{
    return var && !var->ptr_level && var->type && var->type->is_unsigned &&
           var->type->size >= TY_int->size;
}

/* A source-level write invalidates the parser's constant-propagation cache.
 * This is distinct from const qualification, which controls write legality.
 */
void mark_var_mutated(var_t *var)
{
    if (var && !unevaluated_expression_depth)
        var->is_const = false;
}

/* The integer ranks currently represented by shecc are int, long (both 32-bit),
 * and long long (64-bit). Equal representation widths do not merge int and
 * long: C99 still gives long the higher rank.
 */
type_t *integer_common_type(const var_t *left, const var_t *right)
{
    /* The current type lattice has a 32-bit int/long tier and a distinct 64-bit
     * long-long tier. A 64-bit signed operand can represent every 32-bit
     * unsigned value; an unsigned 64-bit operand wins at its rank.
     */
    if ((left && left->type && left->type->size > TY_int->size) ||
        (right && right->type && right->type->size > TY_int->size)) {
        if ((left && left->type && left->type->size > TY_int->size &&
             left->type->is_unsigned) ||
            (right && right->type && right->type->size > TY_int->size &&
             right->type->is_unsigned))
            return TY_ulong_long;
        return TY_long_long;
    }

    if ((left && (left->type == TY_long || left->type == TY_ulong)) ||
        (right && (right->type == TY_long || right->type == TY_ulong))) {
        if ((left && left->type && left->type->is_unsigned) ||
            (right && right->type && right->type->is_unsigned))
            return TY_ulong;
        return TY_long;
    }

    if (unsigned_int_operand(left) || unsigned_int_operand(right))
        return TY_uint;
    return TY_int;
}

type_t *integer_binary_result_type(opcode_t op,
                                   const var_t *left,
                                   const var_t *right)
{
    if (op == OP_eq || op == OP_neq || op == OP_lt || op == OP_leq ||
        op == OP_gt || op == OP_geq)
        return TY_int;

    /* Shift counts are promoted, but do not participate in the usual arithmetic
     * conversions; the result has the promoted left type.
     */
    if (op == OP_lshift || op == OP_rshift)
        return left->type;
    return integer_common_type(left, right);
}

var_t *integer_promote_operand(block_t *parent, basic_block_t **bb, var_t *var)
{
    if (!var || var->ptr_level || !var->type || var->type->size >= TY_int->size)
        return var;
    return promote_unchecked(parent, bb, var, TY_int, 0);
}

/* Apply C99's usual arithmetic conversions after the individual integer
 * promotions. In particular this must materialize a zero-extension for an
 * unsigned int that meets a signed long long, and a sign-extension for a
 * negative int that meets unsigned long long. Merely giving the result the
 * common type leaves the machine operation to consume stale upper bits.
 */
void normalize_integer_binary_operands(block_t *parent,
                                       basic_block_t **bb,
                                       opcode_t op,
                                       var_t **left,
                                       var_t **right)
{
    type_t *common;

    if (op == OP_lshift || op == OP_rshift)
        return;

    /* Equality and relational operators also reach this helper. Their pointer
     * cases keep address semantics and are not usual arithmetic conversions.
     */
    if (is_pointer_like_value(left[0]) || is_pointer_like_value(right[0]))
        return;

    /* The ABI-visible 64-bit rank is distinct today. int and long are both
     * 32-bit in the current type model, so normalizing their signed/unsigned
     * combinations here would add conversions throughout the self-hosted
     * compiler without yet representing a distinct long rank.
     */
    if (get_size(left[0]) <= TY_int->size && get_size(right[0]) <= TY_int->size)
        return;

    common = integer_common_type(*left, *right);

    /* Do not manufacture no-op conversions. Besides bloating every unsigned
     * expression, doing so makes stage1's self-hosting input prohibitively
     * large. A conversion is observable here only across a width boundary, or
     * when a signed value is reinterpreted at an unsigned common rank.
     */
    if (get_size(left[0]) != common->size ||
        (!left[0]->type->is_unsigned && common->is_unsigned))
        left[0] = resize_to(parent, bb, left[0], common, 0);
    if (get_size(right[0]) != common->size ||
        (!right[0]->type->is_unsigned && common->is_unsigned))
        right[0] = resize_to(parent, bb, right[0], common, 0);
}

void read_expr_body(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1, *rs2;
    opcode_t oper_stack[MAX_OPERATOR_STACK_SIZE];
    int oper_stack_idx = 0;

    /* These variables used for parsing logical-and/or operation.
     *
     * For the logical-and operation, the false condition code path for testing
     * each operand uses the same code snippet (basic block).
     *
     * Likewise, when testing each operand for the logical-or operation, all of
     * them share a unified code path for the true condition.
     */
    bool has_prev_log_op = false;
    opcode_t prev_log_op = 0, pprev_log_op = 0;
    basic_block_t *log_and_shared_bb = bb_create(parent),
                  *log_or_shared_bb = bb_create(parent);

    read_expr_operand(parent, bb);

    opcode_t op = get_operator();
    if (op == OP_generic || op == OP_ternary)
        return;
    if (is_logical(op)) {
        bb_connect(*bb, op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                   op == OP_log_and ? ELSE : THEN);
        read_logical(op, parent, bb);
        has_prev_log_op = true;
        prev_log_op = op;
    } else {
        if (oper_stack_idx >= MAX_OPERATOR_STACK_SIZE)
            fatal("Expression too complex: operator stack exhausted");
        oper_stack[oper_stack_idx++] = op;
    }
    read_expr_operand(parent, bb);
    op = get_operator();

    while (op != OP_generic && op != OP_ternary) {
        if (oper_stack_idx > 0) {
            int same = 0;
            do {
                opcode_t top_op = oper_stack[oper_stack_idx - 1];
                if (get_operator_prio(top_op) >= get_operator_prio(op)) {
                    rs2 = opstack_pop();
                    rs1 = opstack_pop();

                    /* Handle pointer arithmetic for addition and subtraction */
                    if (is_pointer_operation(top_op, rs1, rs2)) {
                        /* handle_pointer_arithmetic handles both pointer
                         * differences and regular pointer arithmetic internally
                         */
                        handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
                        oper_stack_idx--;
                        continue;
                    }

                    rs1 = integer_promote_operand(parent, bb, rs1);
                    rs2 = integer_promote_operand(parent, bb, rs2);
                    normalize_integer_binary_operands(parent, bb, top_op, &rs1,
                                                      &rs2);
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = integer_binary_result_type(top_op, rs1, rs2);
                    opstack_push(vd);
                    add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);

                    oper_stack_idx--;
                } else
                    same = 1;
            } while (oper_stack_idx > 0 && same == 0);
        }
        if (is_logical(op)) {
            if (prev_log_op == 0 || prev_log_op == op) {
                bb_connect(
                    *bb,
                    op == OP_log_and ? log_and_shared_bb : log_or_shared_bb,
                    op == OP_log_and ? ELSE : THEN);
                read_logical(op, parent, bb);
                prev_log_op = op;
                has_prev_log_op = true;
            } else if (prev_log_op == OP_log_and) {
                /* For example: a && b || c
                 * previous opcode: prev_log_op == OP_log_and current opcode: op
                 * == OP_log_or current operand: b
                 *
                 * Finalize the logical-and operation and test the operand for
                 * the following logical-or operation.
                 */
                finalize_logical(prev_log_op, parent, bb, log_and_shared_bb);
                log_and_shared_bb = bb_create(parent);
                bb_connect(*bb, log_or_shared_bb, THEN);
                read_logical(op, parent, bb);

                /* Here are two cases to illustrate the following assignments
                 * after finalizing the logical-and operation and testing the
                 * operand for the following logical-or operation.
                 *
                 * 1. a && b || c
                 *    pprev opcode:    pprev_log_op == 0 (no opcode)
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The current opcode should become the previous opcode,
                 * and the pprev opcode remains 0.
                 *
                 * 2. a || b && c || d
                 *    pprev opcode:    pprev_log_op == OP_log_or
                 *    previous opcode: prev_log_op == OP_log_and
                 *    current opcode:  op == OP_log_or
                 *    current operand: b
                 *
                 *    The previous opcode should inherit the pprev opcode, which
                 * is equivalent to inheriting the current opcode because both
                 * of pprev opcode and current opcode are logical-or operator.
                 *
                 *    Thus, pprev opcode is considered used and is cleared to 0.
                 *
                 * Eventually, the current opcode becomes the previous opcode
                 * and pprev opcode is set to 0.
                 */
                prev_log_op = op;
                pprev_log_op = 0;
            } else {
                /* For example: a || b && c
                 * previous opcode: prev_log_op == OP_log_or current opcode: op
                 * == OP_log_and current operand: b
                 *
                 * Using the logical-and operation to test the current operand
                 * instead of using the logical-or operation.
                 *
                 * Then, the previous opcode becomes pprev opcode and the
                 * current opcode becomes the previous opcode.
                 */
                bb_connect(*bb, log_and_shared_bb, ELSE);
                read_logical(op, parent, bb);
                pprev_log_op = prev_log_op;
                prev_log_op = op;
            }
        } else {
            while (has_prev_log_op &&
                   (get_operator_prio(op) < get_operator_prio(prev_log_op))) {
                /* When encountering an operator with lower priority, conclude
                 * the current logical-and/or and create a new basic block for
                 * next logical-and/or operator.
                 */
                finalize_logical(prev_log_op, parent, bb,
                                 prev_log_op == OP_log_and ? log_and_shared_bb
                                                           : log_or_shared_bb);
                if (prev_log_op == OP_log_and)
                    log_and_shared_bb = bb_create(parent);
                else
                    log_or_shared_bb = bb_create(parent);

                /* After finalizing the previous logical-and/or operation, the
                 * prev_log_op should inherit pprev_log_op and continue to check
                 * whether to finalize a logical-and/or operation.
                 */
                prev_log_op = pprev_log_op;
                has_prev_log_op = prev_log_op != 0;
                pprev_log_op = 0;
            }
        }
        read_expr_operand(parent, bb);
        if (!is_logical(op)) {
            if (oper_stack_idx >= MAX_OPERATOR_STACK_SIZE)
                fatal("Expression too complex: operator stack exhausted");
            oper_stack[oper_stack_idx++] = op;
        }
        op = get_operator();
    }

    while (oper_stack_idx > 0) {
        opcode_t top_op = oper_stack[--oper_stack_idx];
        rs2 = opstack_pop();
        rs1 = opstack_pop();

        /* Equality compares function pointers after the C99 function-to-
         * pointer conversion. A raw symbol has no SSA value and otherwise looks
         * like zero to the backend, making `function == 0` spuriously true.
         * Other arithmetic operations retain their function-pointer constraint
         * diagnostics below.
         */
        if (top_op == OP_eq || top_op == OP_neq) {
            rs1 = materialize_function_designator(parent, bb, rs1);
            rs2 = materialize_function_designator(parent, bb, rs2);
        }

        if ((top_op == OP_lt || top_op == OP_leq || top_op == OP_gt ||
             top_op == OP_geq) &&
            ((rs1 && (rs1->is_func || get_func_signature(rs1))) ||
             (rs2 && (rs2->is_func || get_func_signature(rs2)))))
            error_at("Relational comparison requires object pointers",
                     cur_token_loc());

        if (top_op == OP_eq || top_op == OP_neq) {
            func_t *left_signature = get_func_signature(rs1);
            func_t *right_signature = get_func_signature(rs2);

            if (left_signature || right_signature) {
                var_t *other = left_signature ? rs2 : rs1;

                if ((left_signature && right_signature &&
                     !compatible_function_signature(left_signature,
                                                    right_signature)) ||
                    (!get_func_signature(other) &&
                     !is_null_pointer_constant(other)))
                    error_at(
                        "Function pointer comparison requires compatible "
                        "pointers or null",
                        cur_token_loc());
            }
        }

        bool rs1_is_placeholder = is_array_literal_placeholder(rs1);
        bool rs2_is_placeholder = is_array_literal_placeholder(rs2);
        bool rs1_is_ptr_like =
            is_pointer_like_value(rs1) || (rs1 && rs1->is_func);
        bool rs2_is_ptr_like =
            is_pointer_like_value(rs2) || (rs2 && rs2->is_func);
        bool pointer_context = (rs1_is_ptr_like && !rs1_is_placeholder) ||
                               (rs2_is_ptr_like && !rs2_is_placeholder);

        /* Pointer arithmetic handling */
        if (pointer_context && is_pointer_operation(top_op, rs1, rs2)) {
            handle_pointer_arithmetic(parent, bb, top_op, rs1, rs2);
            continue; /* skip normal processing */
        }

        if ((top_op == OP_eq || top_op == OP_neq || top_op == OP_lt ||
             top_op == OP_leq || top_op == OP_gt || top_op == OP_geq) &&
            incompatible_character_pointer_conversion(rs1, rs2))
            error_at("incompatible character pointer types in comparison",
                     cur_token_loc());

        if (rs1_is_placeholder && rs2_is_placeholder) {
            rs1 = scalarize_array_literal(parent, bb, rs1, NULL);
            rs2 = scalarize_array_literal(parent, bb, rs2, NULL);
        } else {
            if (rs1_is_placeholder && !rs2_is_ptr_like)
                rs1 = scalarize_array_literal(
                    parent, bb, rs1, rs2 && rs2->type ? rs2->type : NULL);

            if (rs2_is_placeholder && !rs1_is_ptr_like)
                rs2 = scalarize_array_literal(
                    parent, bb, rs2, rs1 && rs1->type ? rs1->type : NULL);
        }
        rs1 = integer_promote_operand(parent, bb, rs1);
        rs2 = integer_promote_operand(parent, bb, rs2);
        normalize_integer_binary_operands(parent, bb, top_op, &rs1, &rs2);
        type_t *result_type = integer_binary_result_type(top_op, rs1, rs2);
        /* Constant folding for binary operations */
        if (rs1 && rs2 && rs1->is_const && !rs1->ptr_level && !rs1->is_global &&
            rs2->is_const && !rs2->ptr_level && !rs2->is_global &&
            !unsigned_int_operand(rs1) && !unsigned_int_operand(rs2) &&
            rs1->type->size <= TY_int->size &&
            rs2->type->size <= TY_int->size) {
            /* Both operands are compile-time constants */
            int result = 0;
            bool folded = true;

            switch (top_op) {
            case OP_add:
                result = rs1->init_val + rs2->init_val;
                break;
            case OP_sub:
                result = rs1->init_val - rs2->init_val;
                break;
            case OP_mul:
                result = rs1->init_val * rs2->init_val;
                break;
            case OP_div:
                if (rs2->init_val != 0)
                    result = rs1->init_val / rs2->init_val;
                else
                    folded = false; /* Division by zero */
                break;
            case OP_mod:
                if (rs2->init_val != 0)
                    result = rs1->init_val % rs2->init_val;
                else
                    folded = false; /* Modulo by zero */
                break;
            case OP_bit_and:
                result = rs1->init_val & rs2->init_val;
                break;
            case OP_bit_or:
                result = rs1->init_val | rs2->init_val;
                break;
            case OP_bit_xor:
                result = rs1->init_val ^ rs2->init_val;
                break;
            case OP_lshift:
                result = rs1->init_val << rs2->init_val;
                break;
            case OP_rshift:
                result = rs1->init_val >> rs2->init_val;
                break;
            case OP_eq:
                result = rs1->init_val == rs2->init_val;
                break;
            case OP_neq:
                result = rs1->init_val != rs2->init_val;
                break;
            case OP_lt:
                result = rs1->init_val < rs2->init_val;
                break;
            case OP_leq:
                result = rs1->init_val <= rs2->init_val;
                break;
            case OP_gt:
                result = rs1->init_val > rs2->init_val;
                break;
            case OP_geq:
                result = rs1->init_val >= rs2->init_val;
                break;
            default:
                folded = false;
                break;
            }

            if (folded) {
                /* Create constant result */
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = result_type;
                vd->is_const = true;
                vd->init_val = result;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);
            } else {
                /* Normal operation - folding failed or not supported */
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = result_type;
                opstack_push(vd);
                add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);
            }
        } else {
            /* Normal operation */
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->type = result_type;
            opstack_push(vd);
            add_insn(parent, *bb, top_op, vd, rs1, rs2, 0, NULL);
        }
    }
    while (has_prev_log_op) {
        finalize_logical(
            prev_log_op, parent, bb,
            prev_log_op == OP_log_and ? log_and_shared_bb : log_or_shared_bb);

        prev_log_op = pprev_log_op;
        has_prev_log_op = prev_log_op != 0;
        pprev_log_op = 0;
    }
}

/* Nesting counter for read_expr(). The expression grammar descends recursively
 * through read_expr_operand(), so input nested deeply enough would run out of
 * machine stack before any diagnostic could be printed.
 */
int expr_depth = 0;

void read_expr(block_t *parent, basic_block_t **bb)
{
    expr_depth++;
    if (expr_depth > MAX_EXPR_DEPTH)
        error_at("Expression nesting too deep", cur_token_loc());
    read_expr_body(parent, bb);
    expr_depth--;
}


/* Return the address that an expression points to, or evaluate its value.
 *   x =;
 *   x[<expr>] =;
 *   x[expr].field =;
 *   x[expr]->field =;
 *
 * @allow_ptr_arith says whether a following "+ expr" belongs to this lvalue.
 * Normally it does, and the addend is scaled by the element size. The
 * dereference handlers pass false, because unary '*' binds tighter than '+': in
 * "*p + 1" the sum belongs to the enclosing expression, and reading it as
 * pointer arithmetic gives p[1] instead of one more than p[0]. It applies to
 * this lvalue alone -- an lvalue parsed further in, as a subscript or a call
 * argument, gets the normal behaviour from its own call.
 */
void read_lvalue(lvalue_t *lvalue,
                 var_t *var,
                 block_t *parent,
                 basic_block_t **bb,
                 bool eval,
                 opcode_t prefix_op,
                 bool allow_ptr_arith)
{
    var_t *vd, *rs1, *rs2;
    var_t *loaded_scalar_row = NULL;
    type_t *pointer_row_element_type = NULL;
    bool pending_scalar_row_pointer_slot = false;
    bool pending_fixed_array_pointer_slot = false;
    bool is_address_got = false;
    bool is_member = false;
    int subscript_depth = 0;

    /* Callers pass a find_var() result, which is NULL for a name that was never
     * declared.
     */
    if (!var)
        error_at("Undeclared identifier", next_token_loc());

    /* C99 6.7.4 forbids an external-linkage inline definition from referencing
     * an identifier with internal linkage. File-scope `static` objects have
     * GLOBAL_BLOCK as their lexical owner; a static local has a function block
     * instead and is diagnosed at its definition below.
     */
    if (parent && parent->func && parent->func->is_inline &&
        !parent->func->is_static && var->is_static &&
        var->scope == GLOBAL_BLOCK)
        error_at(
            "external inline definition references internal-linkage object",
            next_token_loc());

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = var->type;
    lvalue->decl = var;
    lvalue->size = get_size(var);
    lvalue->ptr_level = var->ptr_level;
    lvalue->value_ptr_level = var->ptr_level + var->type->ptr_level;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = false;
    lvalue->pointee_func_signature = NULL;

    /* A pointer hidden in a typedef keeps its depth on the type rather than the
     * declarator. Its outer const still makes the pointer object read-only,
     * just as for an explicitly spelled `T * const`.
     */
    lvalue->is_const_qualified =
        (var->ptr_level || (var->type && var->type->ptr_level) ||
         var->is_func || var->array_size || var->has_unsized_array)
            ? var->is_const_pointer
            : var->is_const_qualified;
    lvalue->pointer_const_mask = var->pointer_const_mask;

    opstack_push(var);

    if (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
        lex_peek(T_dot, NULL))
        lvalue->is_reference = true;

    while (lex_peek(T_open_square, NULL) || lex_peek(T_arrow, NULL) ||
           lex_peek(T_dot, NULL)) {
        if (lex_accept(T_open_square)) {
            int indexed_ptr_level;
            bool indexes_fixed_array_pointer_slot;
            bool indexes_direct_pointee_array;
            bool indexes_scalar_pointee_row;
            bool indexes_loaded_scalar_pointee_row;
            bool indexes_pointee_row;

            /* if subscripted member's is not yet resolved, dereference to
             * resolve base address. e.g., dereference of "->" in "data->raw[0]"
             * would be performed here.
             */
            if (lvalue->is_reference &&
                (lvalue->ptr_level || pending_fixed_array_pointer_slot) &&
                is_member) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                if (pending_scalar_row_pointer_slot) {
                    vd->type = var->type;
                    vd->ptr_level = 1;
                    copy_pointee_array_shape(vd, var);
                    loaded_scalar_row = vd;
                    pending_scalar_row_pointer_slot = false;
                } else if (pending_fixed_array_pointer_slot) {
                    /* A direct `int (*planes[])[rows][columns]` array selects a
                     * pointer slot before it selects a row. Load that slot once
                     * and retain its fixed pointee shape.
                     */
                    vd->type = var->type->pointee_array_element_type
                                   ? var->type->pointee_array_element_type
                                   : var->type;
                    vd->ptr_level = 1;
                    copy_pointee_array_shape(vd, var);
                    loaded_scalar_row = vd;
                    pending_fixed_array_pointer_slot = false;
                }
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);

                /* The loaded value is the base for this index. A chain such as
                 * `int **p` still advances by pointer slots until its last
                 * indirection; a pointer-to-row then advances by base elements
                 * (or its preserved row extent below).
                 */
                lvalue->size =
                    lvalue->value_ptr_level > 1 ? PTR_SIZE : lvalue->type->size;
            }

            /* var must be either a pointer or an array of some type For typedef
             * pointers, check the type's ptr_level
             */
            bool is_typedef_pointer = (var->type && var->type->ptr_level > 0);
            if (var->ptr_level == 0 && !is_array_declarator(var) &&
                !is_typedef_pointer)
                error_at("Cannot apply square operator to non-pointer",
                         cur_token_loc());

            /* The selected value has one less indirection than the expression
             * being indexed. An array first decays to a pointer to its element;
             * after an earlier subscript, use that selected value as the next
             * indexing source.
             */
            if (subscript_depth)
                indexed_ptr_level = lvalue->value_ptr_level;
            else
                indexed_ptr_level = var->ptr_level + var->type->ptr_level +
                                    !!is_array_declarator(var);
            indexes_fixed_array_pointer_slot =
                !subscript_depth && is_direct_fixed_array_pointer_slot(var);
            indexes_direct_pointee_array =
                !subscript_depth && var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level > 0 &&
                indexed_ptr_level == var->pointee_array_element_ptr_level + 1;

            /* `int (*p)[N]` selects an array row on its first subscript even
             * though that row's scalar elements have no pointer depth. Keep
             * this distinct from the pointer-element row case above: the row
             * decays to `int *` for exactly the following scalar subscript.
             */
            indexes_scalar_pointee_row =
                !subscript_depth && !is_array_declarator(var) &&
                var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level == 0 &&
                effective_pointer_depth(var) == 1;
            indexes_loaded_scalar_pointee_row =
                loaded_scalar_row && subscript_depth == 1;
            indexes_pointee_row = indexes_direct_pointee_array ||
                                  indexes_scalar_pointee_row ||
                                  indexes_loaded_scalar_pointee_row;

            /* Selecting a row designates an array, which immediately decays
             * back to a pointer to its first element for a following postfix
             * subscript. Keep that element pointer depth instead of consuming
             * an indirection as though the row itself were a pointer object.
             */
            lvalue->value_ptr_level =
                indexes_pointee_row
                    ? indexed_ptr_level
                    : (indexed_ptr_level ? indexed_ptr_level - 1 : 0);

            /* if nested pointer, still pointer Also handle typedef pointers
             * which have ptr_level == 0
             */
            if (indexes_direct_pointee_array) {
                /* Preserve the row's base element separately. The row itself
                 * must retain its existing address lowering, but a following
                 * subscript reads a pointer element whose value cannot carry a
                 * lexical alias's outer row-pointer descriptor.
                 */
                pointer_row_element_type =
                    var->type->pointee_array_element_type
                        ? var->type->pointee_array_element_type
                        : var->type;
            } else if (!subscript_depth && is_array_declarator(var) &&
                       var->type->array_element_ptr_level > 0) {
                pointer_row_element_type = var->type->array_element_type
                                               ? var->type->array_element_type
                                               : var->type;
            } else if (indexes_scalar_pointee_row ||
                       indexes_loaded_scalar_pointee_row) {
                var_t *row_source =
                    indexes_loaded_scalar_pointee_row ? loaded_scalar_row : var;
                lvalue->type =
                    row_source->type->pointee_array_element_type
                        ? row_source->type->pointee_array_element_type
                        : row_source->type;
                lvalue->size = lvalue->type->size;
            } else if ((var->ptr_level <= 1 || is_typedef_pointer) &&
                       !is_array_declarator(var)) {
                /* For typedef pointers, get the size of the base type that the
                 * pointer points to
                 */
                if (lvalue->type->ptr_level > 0) {
                    type_t *pointee =
                        pointee_type_from_pointer_typedef(lvalue->type);

                    /* void pointers retain the existing byte-stride extension;
                     * every other typedef pointer advances by its actual
                     * pointee, including a tagged record reached through a
                     * typedef alias.
                     */
                    lvalue->size =
                        pointer_typedef_pointee_size(lvalue->type, pointee);
                    lvalue->type = pointee;
                } else {
                    lvalue->size = lvalue->type->size;
                }
            }

            read_expr(parent, bb);

            /* Multiply by the remaining element extent. */
            int multiplier = lvalue->size;

            if (subscript_depth == 0 && var->array_dim2 > 0) {
                multiplier = fixed_array_subscript_stride(var, subscript_depth,
                                                          lvalue->size);
            } else if (subscript_depth == 0 && var->pointee_array_size > 0 &&
                       indexes_pointee_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    var, subscript_depth, lvalue->size);
            } else if (indexes_loaded_scalar_pointee_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    loaded_scalar_row, 0, lvalue->size);
            } else if (loaded_scalar_row) {
                multiplier = fixed_pointee_array_subscript_stride(
                    loaded_scalar_row, subscript_depth - 1, lvalue->size);
            } else if (subscript_depth >= 1 && var->pointee_array_size > 0) {
                multiplier = fixed_pointee_array_subscript_stride(
                    var, subscript_depth, lvalue->size);
            } else if ((subscript_depth == 1 && var->array_dim3 > 0) ||
                       (subscript_depth == 2 && var->array_dim4 > 0)) {
                multiplier = fixed_array_subscript_stride(var, subscript_depth,
                                                          lvalue->size);
            }

            if (multiplier != 1) {
                vd = require_var(parent);
                vd->init_val = multiplier;
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);

            /* A subscript expression computes the address of its selected
             * element. Preserve that pointer provenance: unary '&' leaves an
             * already-addressable subscript alone, and pointer subtraction must
             * still know whether this is an int or struct element.
             */
            vd->type = lvalue->type;

            /* The computed address points at the selected element. If that
             * element is itself a pointer (for example, `int *items[]`), its
             * value indirection remains inside the address type: `&items[i]` is
             * `int **`, not `int *`.
             */
            vd->ptr_level = lvalue->ptr_level + 1;
            vd->var_name = gen_name();
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            lex_expect(T_close_square);
            is_address_got = true;

            /* A following subscript loads only when this selected element is
             * itself a pointer (for example, `int **p; p[0][1]`). A
             * pointer-to-array selects a row, not a pointer object, so treating
             * every subscript as a member would dereference row data on its
             * next index.
             */
            is_member = !indexes_pointee_row && lvalue->value_ptr_level > 0;
            subscript_depth++;
            lvalue->is_reference = !indexes_pointee_row;
            if (subscript_depth == 1 && var->pointee_array_size > 0 &&
                var->pointee_array_element_ptr_level == 0 &&
                effective_pointer_depth(var) == 2)
                pending_scalar_row_pointer_slot = true;
            if (indexes_fixed_array_pointer_slot)
                pending_fixed_array_pointer_slot = true;
            if (loaded_scalar_row) {
                fixed_array_shape_t loaded_shape =
                    fixed_array_shape_from_pointee_var(loaded_scalar_row);

                if (subscript_depth >= 1 + loaded_shape.rank)
                    loaded_scalar_row = NULL;
            }

            /* A multidimensional slot array carries the slot descriptor on the
             * whole array. Only its final subscript selects an element; earlier
             * subscripts select rows that decay for the next index.
             */
            if (var->type->array_element_pointee_func_signature) {
                int array_dims = 1 + !!var->array_dim2 + !!var->array_dim3 +
                                 !!var->array_dim4;

                if (subscript_depth == array_dims)
                    lvalue->pointee_func_signature =
                        var->type->array_element_pointee_func_signature;
            }

            /* A subscript designates the pointee, whose qualification is the
             * declaration's base qualification rather than `* const`.
             */
            lvalue->is_const_qualified =
                var->is_const_qualified ||
                (lvalue->pointee_func_signature &&
                 var->type->array_element_is_const_pointer);
        } else {
            char token[MAX_ID_LEN];

            if (lex_accept(T_arrow)) {
                /* resolve where the pointer points at from the calculated
                 * address in a structure.
                 */
                if (is_member) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE,
                             NULL);
                }
            } else {
                lex_expect(T_dot);

                if (!is_address_got) {
                    rs1 = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0,
                             NULL);

                    is_address_got = true;
                }
            }

            lex_ident(T_identifier, token);

            /* change type currently pointed to */
            var = find_member(token, lvalue->type);
            if (!var)
                error_at("Unknown struct or union member", next_token_loc());
            lvalue->type = var->type;
            lvalue->decl = var;
            lvalue->ptr_level = var->ptr_level;
            lvalue->value_ptr_level = var->ptr_level + var->type->ptr_level;
            lvalue->is_func = var->is_func;
            lvalue->size = get_size(var);
            lvalue->is_const_qualified |= var->is_const_qualified;
            subscript_depth = 0;

            /* if it is an array, get the address of first element instead of
             * its value.
             */
            if (is_array_declarator(var))
                lvalue->is_reference = false;

            /* move pointer to offset of structure */
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = var->offset;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);
            vd->var_name = gen_name();
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            is_address_got = true;
            is_member = true;
        }
    }

    if (!eval)
        return;

    if (lvalue->is_const_qualified &&
        (prefix_op != OP_generic || lex_peek(T_increment, NULL) ||
         lex_peek(T_decrement, NULL)))
        error_at("assignment of read-only location", next_token_loc());

    /* Only handle pointer arithmetic if we have a pointer/array that hasn't
     * been dereferenced. After array indexing like arr[0], we have a value, not
     * a pointer.
     */
    bool scalar_pointee_row = is_scalar_pointee_array_pointer(var);
    if (allow_ptr_arith && lex_peek(T_plus, NULL) &&
        (var->ptr_level || is_array_declarator(var) || scalar_pointee_row) &&
        !lvalue->is_reference) {
        if (!is_array_declarator(var) &&
            is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
            error_at("Pointer arithmetic on void* is invalid", cur_token_loc());
        while (lex_peek(T_plus, NULL) &&
               (var->ptr_level || is_array_declarator(var) ||
                scalar_pointee_row)) {
            lex_expect(T_plus);
            if (lvalue->is_reference) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, lvalue->size,
                         NULL);
            }

            read_expr_operand(parent, bb);

            /* The element stepped over is the pointee. For a multi-level
             * pointer that pointee is itself a pointer, so the stride is
             * PTR_SIZE rather than the base type's width.
             */
            if (scalar_pointee_row) {
                lvalue->size = scalar_pointee_array_row_stride(var);
            } else if (is_array_declarator(var) && var->array_dim2 &&
                       !has_effective_pointer(var) && !var->is_func) {
                lvalue->size = fixed_array_decay_stride(var);
            } else if (var->ptr_level > 1 ||
                       (is_array_declarator(var) && var->ptr_level))
                lvalue->size = PTR_SIZE;
            else
                lvalue->size = lvalue->type->size;

            if (lvalue->size > 1) {
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->init_val = lvalue->size;
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);
            }

            rs2 = opstack_pop();
            rs1 = opstack_pop();
            vd = require_var(parent);

            /* A pointer plus an integer is still a pointer. Array expressions
             * first decay to a pointer, adding one level to the declaration's
             * element indirection.
             */
            if (var->ptr_level || is_array_declarator(var) ||
                scalar_pointee_row) {
                vd->type = lvalue->type;
                vd->ptr_level = var->ptr_level + !!is_array_declarator(var);
                if (scalar_pointee_row)
                    copy_pointee_array_shape(vd, var);
                else if (is_array_declarator(var) && var->array_dim2 &&
                         !has_effective_pointer(var) && !var->is_func) {
                    fixed_array_shape_t shape = fixed_array_shape_from_var(var);

                    fixed_array_shape_drop_outer(&shape);
                    fixed_array_shape_to_pointee_var(vd, &shape);
                }
            }
            vd->var_name = gen_name();
            vd->is_const_qualified = var->is_const_qualified;
            vd->pointer_const_mask = var->pointer_const_mask;
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);
        }
    } else {
        /* Set and read only under 'is_reference'; the initializer says so to a
         * compiler that cannot correlate the two tests.
         */
        var_t *t = NULL;

        /* If operand is a reference, read the value and push to stack for the
         * incoming addition/subtraction. Otherwise, use the top element of
         * stack as the one of operands and the destination.
         */
        if (lvalue->is_reference) {
            rs1 = operand_stack[operand_stack_idx - 1];
            t = require_var(parent);
            t->var_name = gen_name();
            t->type = pointer_row_element_type
                          ? pointer_row_element_type
                          : pointee_type_from_pointer_typedef(lvalue->type);
            t->ptr_level = lvalue->value_ptr_level;
            if (pointer_row_element_type)
                t->is_const_qualified = lvalue->type->is_const_qualified;

            /* Retain a callback prototype even through a selected slot. A
             * direct loaded callback is callable, while unary `*` restores this
             * marker after consuming an extra object-pointer level.
             */
            t->func_signature = lvalue->type->func_signature;
            if (lvalue->pointee_func_signature) {
                /* `slots[i]` loads a callback slot, not a callable callback.
                 * Preserve the element's prototype until one unary `*` consumes
                 * this outer object-pointer level.
                 */
                t->ptr_level = 1;
                t->func_signature = NULL;
                t->pointee_func_signature = lvalue->pointee_func_signature;
                t->is_const_pointer = var->type->array_element_is_const_pointer;
                t->pointer_const_mask = t->is_const_pointer ? 1U : 0;
                t->is_volatile = var->type->array_element_is_volatile;
            }
            opstack_push(t);
            if (is_bitfield(lvalue->decl)) {
                /* Bit-fields are values, never independently addressable
                 * storage: extract their slice from the containing unit.
                 */
                opstack_pop();
                t = read_bitfield_value(parent, bb, rs1, lvalue->decl);
                opstack_push(t);
            } else {
                add_insn(parent, *bb, OP_read, t, rs1, NULL, lvalue->size,
                         NULL);
            }
        }
        if (prefix_op != OP_generic) {
            if ((prefix_op == OP_add || prefix_op == OP_sub) &&
                is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* For pointer arithmetic, increment by the size of pointed-to type
             */
            if (!lvalue->is_reference && is_scalar_pointee_array_pointer(var))
                vd->init_val = scalar_pointee_array_row_stride(var);
            else if (lvalue->ptr_level > 1)
                vd->init_val = PTR_SIZE;
            else if (lvalue->ptr_level)
                vd->init_val = lvalue->type->size;
            else if (lvalue->type && lvalue->type->ptr_level)
                vd->init_val = get_pointer_element_size(var);
            else
                vd->init_val = 1;
            opstack_push(vd);
            add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs2 = opstack_pop();
            if (lvalue->is_reference)
                rs1 = opstack_pop();
            else
                rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            vd->var_name = gen_name();
            add_insn(parent, *bb, prefix_op, vd, rs1, rs2, 0, NULL);

            if (lvalue->is_reference) {
                rs1 = vd;
                vd = opstack_pop();

                /* The column of arguments of the new insn of 'OP_write' is
                 * different from 'ph1_ir'
                 */
                add_insn(parent, *bb, OP_write, NULL, vd, rs1, lvalue->size,
                         NULL);
                /* Push the new value onto the operand stack */
                opstack_push(rs1);
            } else {
                rs1 = vd;
                vd = operand_stack[operand_stack_idx - 1];
                mark_var_mutated(vd);
                add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
            }
        } else if (lex_peek(T_increment, NULL) || lex_peek(T_decrement, NULL)) {
            if (is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());

            /* Deferred postfix stores are plain OP_write instructions. A
             * bit-field instead needs a masked read-modify-write of its
             * containing allocation unit, so lower it here while retaining the
             * extracted pre-update value as the expression result.
             */
            if (lvalue->is_reference && is_bitfield(lvalue->decl)) {
                opcode_t postfix_op = lex_accept(T_increment) ? OP_add : OP_sub;
                var_t *old = opstack_pop();
                var_t *address = opstack_pop();
                var_t *one = bitfield_constant(parent, bb, 1);
                var_t *updated = require_var(parent);

                updated->var_name = gen_name();
                updated->type =
                    integer_binary_result_type(postfix_op, old, one);
                add_insn(parent, *bb, postfix_op, updated, old, one, 0, NULL);
                write_bitfield_value(parent, bb, address, updated,
                                     lvalue->decl);
                old->is_bitfield = false;
                opstack_push(old);
                return;
            }

            /* This arm appends three entries, so check for room once before
             * writing any of them.
             */
            if (se_idx + 3 > MAX_SIDE_EFFECT)
                error_at("Too many postfix operators in one statement",
                         next_token_loc());

            side_effect[se_idx].opcode = OP_load_constant;
            vd = require_var(parent);
            vd->var_name = gen_name();

            /* Calculate increment size based on pointer type */
            int increment_size = 1;
            if (!lvalue->is_reference && is_scalar_pointee_array_pointer(var)) {
                increment_size = scalar_pointee_array_row_stride(var);
            } else if (lvalue->ptr_level > 1 && !lvalue->is_reference) {
                increment_size = PTR_SIZE;
            } else if (lvalue->ptr_level && !lvalue->is_reference) {
                increment_size = lvalue->type->size;
            } else if (!lvalue->is_reference && lvalue->type &&
                       lvalue->type->ptr_level > 0) {
                /* This is a typedef pointer */
                switch (lvalue->type->base_type) {
                case TYPE_char:
                    increment_size = TY_char->size;
                    break;
                case TYPE_short:
                    increment_size = TY_short->size;
                    break;
                case TYPE_int:
                    increment_size = TY_int->size;
                    break;
                case TYPE_void:
                    increment_size = 1;
                    break;
                default:
                    increment_size = lvalue->type->size;
                    break;
                }
            }
            vd->init_val = increment_size;

            side_effect[se_idx].rd = vd;
            side_effect[se_idx].rs1 = NULL;
            side_effect[se_idx].rs2 = NULL;
            se_idx++;

            /* Consume whichever operator is actually there. Testing only for
             * '++' picks the right opcode but leaves a '--' in the stream, so
             * postfix decrement parsed only where the leftover token happened
             * to be harmless -- "i--;" as a statement worked, "a = i--" did
             * not.
             */
            opcode_t postfix_op = OP_sub;
            if (lex_accept(T_increment))
                postfix_op = OP_add;
            else
                lex_expect(T_decrement);
            side_effect[se_idx].opcode = postfix_op;
            side_effect[se_idx].rs2 = vd;
            if (lvalue->is_reference)
                side_effect[se_idx].rs1 = opstack_pop();
            else
                side_effect[se_idx].rs1 = operand_stack[operand_stack_idx - 1];
            vd = require_var(parent);
            vd->var_name = gen_name();
            if (!lvalue->is_reference && is_scalar_pointee_array_pointer(var)) {
                vd->type = lvalue->type;
                vd->ptr_level = var->ptr_level;
                copy_pointee_array_shape(vd, var);
            }
            side_effect[se_idx].rd = vd;
            se_idx++;

            if (lvalue->is_reference) {
                side_effect[se_idx].opcode = OP_write;
                side_effect[se_idx].rs2 = vd;
                side_effect[se_idx].rs1 = opstack_pop();
                side_effect[se_idx].sz = lvalue->size;
                side_effect[se_idx].rd = NULL;
                opstack_push(t);
                se_idx++;
            } else {
                side_effect[se_idx].opcode = OP_assign;
                side_effect[se_idx].rs1 = vd;
                side_effect[se_idx].rd = operand_stack[operand_stack_idx - 1];
                side_effect[se_idx].rs2 = NULL;
                se_idx++;
            }
        } else {
            if (lvalue->is_reference) {
                /* pop the address and keep the read value */
                t = opstack_pop();
                opstack_pop();
                opstack_push(t);
            }
        }
    }
}

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb)
{
    var_t *vd;

    if (op != OP_log_and && op != OP_log_or)
        error_at("encounter an invalid logical opcode in read_logical()",
                 cur_token_loc());

    /* Test the operand before the logical-and/or operator */
    vd = opstack_pop();
    vd = materialize_function_designator(parent, bb, vd);
    add_insn(parent, *bb, OP_branch, NULL, vd, NULL, 0, NULL);

    /* Create a proper branch label for the operand of the logical-and/or
     * operation.
     */
    basic_block_t *new_bb = bb_create(parent);
    bb_connect(*bb, new_bb, op == OP_log_and ? THEN : ELSE);

    bb[0] = new_bb;
}

void finalize_logical(opcode_t op,
                      block_t *parent,
                      basic_block_t **bb,
                      basic_block_t *shared_bb)
{
    basic_block_t *then, *then_next, *else_if, *else_bb;
    basic_block_t *end = bb_create(parent);
    var_t *vd, *log_op_res;

    if (op == OP_log_and) {
        /* For example: a && b
         *
         * If handling the expression, the basic blocks will connect to each
         * other as the following illustration:
         *
         * bb1 bb2 bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | True  | teq b, #0 | True  | ldr 1   |
         * | bne bb2   | ----> | bne bb3   | ----> | b   bb5 |
         * | b   bb4   |       | b   bb4   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | False             | False            |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 0   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * In this case, finalize_logical() should add some instructions to bb2
         * ~ bb5 and properly connect them to each other.
         *
         * Notice that
         * - bb1 has been handled by read_logical().
         * - bb2 is equivalent to '*bb'.
         * - bb3 needs to be created.
         * - bb4 is 'shared_bb'.
         * - bb5 needs to be created.
         *
         * Thus, here uses 'then', 'then_next', 'else_bb' and 'end' to
         * respectively point to bb2 ~ bb5. Subsequently, perform the mentioned
         * operations for finalizing.
         */
        then = *bb;
        then_next = bb_create(parent);
        else_bb = shared_bb;
        bb_connect(then, then_next, THEN);
        bb_connect(then, else_bb, ELSE);
        bb_connect(then_next, end, NEXT);
    } else if (op == OP_log_or) {
        /* For example: a || b
         *
         * Similar to handling logical-and operations, it should add some
         * instructions to the basic blocks and connect them to each other for
         * logical-or operations as in the figure:
         *
         * bb1 bb2 bb3
         * +-----------+       +-----------+       +---------+
         * | teq a, #0 | False | teq b, #0 | False | ldr 0   |
         * | bne bb4   | ----> | bne bb4   | ----> | b   bb5 |
         * | b   bb2   |       | b   bb3   |       +---------+
         * +-----------+       +-----------+           |
         *      |                   |                  |
         *      | True              | True             |
         *      |                   |                  |
         *      |              +---------+         +--------+
         *      -------------> | ldr 1   | ------> |        |
         *                     | b   bb5 |         |        |
         *                     +---------+         +--------+
         *                      bb4                 bb5
         *
         * Similarly, here uses 'else_if', 'else_bb', 'then' and 'end' to
         * respectively point to bb2 ~ bb5, and then finishes the finalization.
         */
        then = shared_bb;
        else_if = *bb;
        else_bb = bb_create(parent);
        bb_connect(else_if, then, THEN);
        bb_connect(else_if, else_bb, ELSE);
        bb_connect(then, end, NEXT);
    } else
        error_at("encounter an invalid logical opcode in finalize_logical()",
                 cur_token_loc());
    bb_connect(else_bb, end, NEXT);

    /* Create the branch instruction for final logical-and/or operand */
    vd = opstack_pop();
    add_insn(parent, op == OP_log_and ? then : else_if, OP_branch, NULL, vd,
             NULL, 0, NULL);

    /* If handling logical-and operation, here creates a true branch for the
     * logical-and operation and assigns a true value.
     *
     * Otherwise, create a false branch and assign a false value for logical-or
     * operation.
     */
    vd = require_var(parent);
    vd->var_name = gen_name();
    vd->init_val = op == OP_log_and;
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_load_constant,
             vd, NULL, NULL, 0, NULL);

    log_op_res = require_var(parent);
    log_op_res->var_name = gen_name();
    add_insn(parent, op == OP_log_and ? then_next : else_bb, OP_assign,
             log_op_res, vd, NULL, 0, NULL);

    /* After assigning a value, go to the final basic block, this is done by BB
     * fallthrough.
     */

    /* Create the shared branch and assign the other value for the other
     * condition of a logical-and/or operation.
     *
     * If handing a logical-and operation, assign a false value. else, assign a
     * true value for a logical-or operation.
     */
    vd = require_var(parent);
    vd->var_name = gen_name();
    vd->init_val = op != OP_log_and;
    add_insn(parent, op == OP_log_and ? else_bb : then, OP_load_constant, vd,
             NULL, NULL, 0, NULL);

    add_insn(parent, op == OP_log_and ? else_bb : then, OP_assign, log_op_res,
             vd, NULL, 0, NULL);

    log_op_res->is_logical_ret = true;
    opstack_push(log_op_res);

    bb[0] = end;
}

/* Parse the full expression grammar used by control statements. This keeps
 * comma sequencing and conditional expressions consistent across if, loops, and
 * switch while argument and initializer lists retain their own delimiter rules.
 */
void read_control_expression(block_t *parent, basic_block_t **bb)
{
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }

    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, *bb);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
    }

    /* A function designator in a controlling expression decays to its code
     * pointer before OP_branch tests it. Raw symbols intentionally have no
     * allocated value, so branching on one directly can observe zero or an
     * unrelated register instead of C99's non-null function address.
     */
    var_t *result = opstack_pop();
    result = materialize_function_designator(parent, bb, result);
    opstack_push(result);
}

/* Expression statements use the same full-expression grammar as controls: parse
 * an assignment or conditional expression, sequence top-level commas, then
 * discard only the final value after its side effects have been emitted.
 */
basic_block_t *read_full_expression_statement(block_t *parent,
                                              basic_block_t *bb)
{
    read_control_expression(parent, &bb);
    opstack_pop();
    perform_side_effect(parent, bb);
    lex_expect(T_semicolon);
    return bb;
}

/* This is deliberately narrower than expression assignment recognition: a
 * grouped pointer-to-row store has no general lvalue representation yet.
 */
static bool grouped_scalar_pointee_row_store_starts(void)
{
    token_t *token = cur_token ? cur_token->next : NULL;
    int depth = 0;

    if (!token || token->kind != T_open_bracket || !(token = token->next) ||
        token->kind != T_asterisk || !(token = token->next) ||
        token->kind != T_identifier || !(token = token->next) ||
        token->kind != T_close_bracket || !(token = token->next) ||
        token->kind != T_open_square)
        return false;
    for (; token; token = token->next) {
        if (token->kind == T_open_square || token->kind == T_open_bracket ||
            token->kind == T_open_curly)
            depth++;
        else if (token->kind == T_close_square ||
                 token->kind == T_close_bracket ||
                 token->kind == T_close_curly) {
            if (!--depth)
                break;
        }
    }
    if (!token || !(token = token->next) ||
        (token->kind != T_assign && token->kind != T_pluseq &&
         token->kind != T_minuseq))
        return false;
    for (depth = 0, token = token->next; token; token = token->next) {
        if (token->kind == T_open_square || token->kind == T_open_bracket ||
            token->kind == T_open_curly)
            depth++;
        else if (token->kind == T_close_square ||
                 token->kind == T_close_bracket || token->kind == T_close_curly)
            depth--;
        else if (!depth && token->kind == T_comma)
            return false;
        else if (!depth && token->kind == T_semicolon)
            return true;
    }
    return false;
}

static basic_block_t *handle_grouped_scalar_pointee_row_store(block_t *parent,
                                                              basic_block_t *bb)
{
    char name[MAX_VAR_LEN];
    var_t *source, *row, *index, *address, *value;

    lex_expect(T_open_bracket);
    lex_expect(T_asterisk);
    lex_ident(T_identifier, name);
    source = find_var(name, parent);
    if (!source)
        error_at("Undeclared identifier", cur_token_loc());
    if (!lower_scalar_pointee_array_dereference(source, parent, &bb))
        error_at("Grouped pointer-to-array store requires a scalar fixed row",
                 cur_token_loc());
    row = opstack_pop();
    if (row->is_const_qualified)
        error_at("assignment of read-only location", cur_token_loc());
    lex_expect(T_close_bracket);
    lex_expect(T_open_square);
    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    index = opstack_pop();
    lex_expect(T_close_square);
    if (row->type->size != 1) {
        var_t *scale = require_var(parent);
        var_t *scaled = require_var(parent);
        scale->var_name = gen_name();
        scale->init_val = row->type->size;
        add_insn(parent, bb, OP_load_constant, scale, NULL, NULL, 0, NULL);
        scaled->var_name = gen_name();
        add_insn(parent, bb, OP_mul, scaled, index, scale, 0, NULL);
        index = scaled;
    }
    address = require_typed_ptr_var(parent, row->type, 1);
    address->var_name = gen_name();
    add_insn(parent, bb, OP_add, address, row, index, 0, NULL);
    opcode_t compound_op = OP_generic;
    if (lex_accept(T_pluseq))
        compound_op = OP_add;
    else if (lex_accept(T_minuseq))
        compound_op = OP_sub;
    else
        lex_expect(T_assign);
    if (!read_assignment_expression(parent, &bb)) {
        read_expr(parent, &bb);
        read_ternary_operation(parent, &bb);
    }
    value = opstack_pop();
    if (compound_op != OP_generic) {
        if (value->is_func || is_pointer_like_value(value) ||
            is_record_type(value->type))
            error_at("Invalid compound assignment operand", cur_token_loc());
        var_t *current = require_typed_var(parent, row->type);
        current->var_name = gen_name();
        add_insn(parent, bb, OP_read, current, address, NULL, row->type->size,
                 NULL);
        current = integer_promote_operand(parent, &bb, current);
        value = integer_promote_operand(parent, &bb, value);
        normalize_integer_binary_operands(parent, &bb, compound_op, &current,
                                          &value);
        var_t *updated = require_var(parent);
        updated->var_name = gen_name();
        updated->type = integer_binary_result_type(compound_op, current, value);
        add_insn(parent, bb, compound_op, updated, current, value, 0, NULL);
        value = resize_to(parent, &bb, updated, row->type, 0);
    }
    add_insn(parent, bb, OP_write, NULL, address, value, row->type->size, NULL);
    lex_expect(T_semicolon);
    return bb;
}

/* The middle operand of ?: is an expression, rather than merely an
 * assignment-expression, so top-level commas belong to the selected true
 * branch. Keep this small sequencing layer here until all full-expression entry
 * points share the core comma grammar.
 */
void read_conditional_true_expression(block_t *parent, basic_block_t **bb)
{
    if (!read_assignment_expression(parent, bb)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
    }

    while (lex_accept(T_comma)) {
        opstack_pop();
        perform_side_effect(parent, *bb);
        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
    }
}

/* An integer constant expression with value zero is the sole scalar that can
 * form a conditional pointer expression. Keep this small predicate local to
 * `?:`: ordinary pointer conversions have their own qualifier diagnostics.
 */
bool is_null_pointer_constant(var_t *value)
{
    return value && value->is_const && !is_pointer_like_value(value) &&
           !value->is_func && !value->init_val && !value->init_val_hi;
}

void read_ternary_operation(block_t *parent, basic_block_t **bb)
{
    var_t *vd;

    if (!lex_accept(T_question))
        return;

    /* ternary-operator */
    vd = opstack_pop();
    add_insn(parent, *bb, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *then_ = bb_create(parent);
    basic_block_t *else_ = bb_create(parent);
    basic_block_t *end_ternary = bb_create(parent);
    basic_block_t *then_entry = then_;
    basic_block_t *else_entry = else_;

    /* true branch */
    read_conditional_true_expression(parent, &then_);
    bb_connect(*bb, then_entry, THEN);

    if (!lex_accept(T_colon)) {
        /* ternary operator in standard C needs three operands */
        error_at("Expected ':' in conditional expression", next_token_loc());
    }

    var_t *true_val = opstack_pop();

    /* false branch */
    if (!read_assignment_expression(parent, &else_))
        read_expr(parent, &else_);

    /* The third operand has conditional-expression grammar, making nested
     * conditionals right-associative: a ? b : c ? d : e.
     */
    read_ternary_operation(parent, &else_);
    bb_connect(*bb, else_entry, ELSE);
    var_t *false_val = opstack_pop();

    /* A function designator decays to a pointer in each conditional operand.
     * Raw function symbols have no storage/defining IR, so materialize both
     * branch values before the join assigns the selected callback. This is the
     * same representation used by function-pointer assignment, casts, and
     * returns; delaying it until after the join can leave OP_assign carrying a
     * symbol with no allocated value and make a later indirect call jump to
     * garbage.
     */
    true_val = materialize_function_designator(parent, &then_, true_val);
    false_val = materialize_function_designator(parent, &else_, false_val);
    func_t *true_signature = get_func_signature(true_val);
    func_t *false_signature = get_func_signature(false_val);
    func_t *true_pointee_signature = true_val->pointee_func_signature;
    func_t *false_pointee_signature = false_val->pointee_func_signature;
    bool true_array = is_array_literal_placeholder(true_val);
    bool false_array = is_array_literal_placeholder(false_val);
    bool true_ptr_like = is_pointer_like_value(true_val);
    bool false_ptr_like = is_pointer_like_value(false_val);

    /* The ternary result must look like whichever side is pointer-like. If the
     * "true" expression is still a raw array literal but the "false" side is a
     * plain scalar, materialize the literal now so both branches produce
     * comparable scalar SSA values.
     */
    true_val = scalarize_array_literal_if_needed(
        parent, &then_, true_val, false_val ? false_val->type : NULL,
        true_array && !false_ptr_like);

    /* Apply the same conversion symmetrically when only the false branch is a
     * literal array. This prevents OP_assign from trying to move array storage
     * into a scalar destination later in code generation.
     */
    false_val = scalarize_array_literal_if_needed(
        parent, &else_, false_val, true_val ? true_val->type : NULL,
        false_array && !true_ptr_like);

    if (is_record_object(true_val) || is_record_object(false_val)) {
        var_t *true_addr;
        var_t *false_addr;
        var_t *selected;

        if (!is_record_object(true_val) || !is_record_object(false_val) ||
            size_var(true_val) != size_var(false_val))
            error_at("Conditional record operands must have the same type",
                     cur_token_loc());

        /* A record operand is an object, not a register value, so joining the
         * two operands as scalars would keep only a truncated prefix. Select
         * the operand's address instead and copy the chosen object into a
         * temporary after the join, which dominates every later use.
         */
        true_addr = require_ref_var(parent, true_val->type, 0);
        false_addr = require_ref_var(parent, false_val->type, 0);
        selected = require_ref_var(parent, true_val->type, 0);
        true_addr->var_name = gen_name();
        false_addr->var_name = gen_name();
        selected->var_name = gen_name();
        add_insn(parent, then_, OP_address_of, true_addr, true_val, NULL, 0,
                 NULL);
        add_insn(parent, else_, OP_address_of, false_addr, false_val, NULL, 0,
                 NULL);
        add_insn(parent, then_, OP_assign, selected, true_addr, NULL, 0, NULL);
        add_insn(parent, else_, OP_assign, selected, false_addr, NULL, 0, NULL);
        bb_connect(then_, end_ternary, NEXT);
        bb_connect(else_, end_ternary, NEXT);

        vd = require_typed_var(parent, true_val->type);
        vd->var_name = gen_name();
        add_insn(parent, end_ternary, OP_allocat, vd, NULL, NULL, 0, NULL);
        emit_record_copy_from_address(parent, &end_ternary, vd, selected);
        opstack_push(vd);
        bb[0] = end_ternary;
        return;
    }

    vd = require_var(parent);
    vd->var_name = gen_name();
    if (true_pointee_signature || false_pointee_signature) {
        func_t *signature = true_pointee_signature ? true_pointee_signature
                                                   : false_pointee_signature;
        var_t *pointer_value = true_pointee_signature ? true_val : false_val;
        var_t *other_value = true_pointee_signature ? false_val : true_val;

        if ((true_pointee_signature && false_pointee_signature &&
             !compatible_function_signature(true_pointee_signature,
                                            false_pointee_signature)) ||
            (!other_value->pointee_func_signature &&
             !is_null_pointer_constant(other_value)))
            error_at("Conditional callback slots must be compatible or null",
                     cur_token_loc());

        /* A matching conditional slot remains non-callable until dereferenced,
         * so preserve its pointee signature rather than presenting the outer
         * pointer as a function pointer.
         */
        vd->type = pointer_value->type;
        vd->ptr_level = pointer_value->ptr_level;
        vd->pointee_func_signature = signature;
    } else if (true_signature || false_signature) {
        func_t *signature = true_signature ? true_signature : false_signature;
        var_t *pointer_value = true_signature ? true_val : false_val;
        var_t *other_value = true_signature ? false_val : true_val;

        if ((true_signature && false_signature &&
             !compatible_function_signature(true_signature, false_signature)) ||
            (!get_func_signature(other_value) &&
             !is_null_pointer_constant(other_value)))
            error_at("Conditional function pointers must be compatible or null",
                     cur_token_loc());

        /* The branch join is itself a function-pointer value: retain the
         * selected callback's pointer representation and prototype so a postfix
         * call on `(condition ? first : second)` lowers indirectly.
         */
        vd->type = pointer_value->type;
        vd->ptr_level = 1;
        vd->func_signature = signature;
    } else if (!true_ptr_like && !false_ptr_like && !true_val->ptr_level &&
               !false_val->ptr_level) {
        true_val = integer_promote_operand(parent, &then_, true_val);
        false_val = integer_promote_operand(parent, &else_, false_val);
        vd->type = integer_binary_result_type(OP_add, true_val, false_val);
        true_val = resize_to(parent, &then_, true_val, vd->type, 0);
        false_val = resize_to(parent, &else_, false_val, vd->type, 0);
    }
    add_insn(parent, then_, OP_assign, vd, true_val, NULL, 0, NULL);
    add_insn(parent, else_, OP_assign, vd, false_val, NULL, 0, NULL);

    /* Recursive conditionals advance then_/else_ to their completed branch
     * tails. Join those tails, not the original entries, to the outer end.
     */
    bb_connect(then_, end_ternary, NEXT);
    bb_connect(else_, end_ternary, NEXT);

    var_t *array_ref = NULL;
    if (is_array_literal_placeholder(true_val))
        array_ref = true_val;
    else if (is_array_literal_placeholder(false_val))
        array_ref = false_val;

    if (array_ref) {
        vd->array_size = array_ref->array_size;
        vd->init_val = array_ref->init_val;
        vd->type = array_ref->type;
    }

    vd->is_ternary_ret = true;
    opstack_push(vd);
    bb[0] = end_ternary;
}

bool read_body_assignment(char *token,
                          block_t *parent,
                          opcode_t prefix_op,
                          basic_block_t **bb,
                          var_t **assignment_result,
                          int lvalue_paren_depth)
{
    var_t *var = find_local_var(token, parent), *vd, *rs1, *rs2, *t;
    if (!var)
        var = find_global_var(token);
    if (assignment_result)
        assignment_result[0] = NULL;

    if (var) {
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);
        while (lvalue_paren_depth-- > 0)
            lex_expect(T_close_bracket);
        size = lvalue.size;

        if (lvalue.is_const_qualified && lvalue_write_follows(prefix_op))
            error_at(lvalue.is_reference ? "assignment of read-only location"
                                         : "assignment of read-only variable",
                     next_token_loc());

        if (lex_accept(T_increment)) {
            op = OP_add;
            one = 1;
        } else if (lex_accept(T_decrement)) {
            op = OP_sub;
            one = 1;
        } else if (accept_compound_assign_op(&op)) {
            /* op now holds the arithmetic the operator applies */
        } else if (lex_peek(T_open_bracket, NULL)) {
            /* Dereference lvalue first if lvalue is a member access; otherwise,
             * pass the function pointer value on the stack to
             * read_indirect_call.
             */
            if (lvalue.is_reference) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);
            }

            emit_indirect_call_result(lvalue.decl,
                                      get_func_signature(lvalue.decl), false,
                                      parent, bb);
            return true;
        } else if (prefix_op == OP_generic) {
            lex_expect(T_assign);
        } else {
            op = prefix_op;
            one = 1;
        }

        if (op != OP_generic) {
            int increment_size = 1;

            if ((op == OP_add || op == OP_sub) &&
                is_direct_void_pointer_type(lvalue.type, lvalue.ptr_level))
                error_at("Pointer arithmetic on void* is invalid",
                         cur_token_loc());

            /* if we have a pointer, shift it by element size But not if we are
             * operating on a dereferenced value (array indexing)
             */
            if (!one && (op == OP_add || op == OP_sub) &&
                !lvalue.is_reference && is_scalar_pointee_array_pointer(var))
                increment_size = scalar_pointee_array_row_stride(var);
            else if (lvalue.ptr_level > 1 && !lvalue.is_reference)
                increment_size = PTR_SIZE;
            else if (lvalue.ptr_level && !lvalue.is_reference)
                increment_size = lvalue.type->size;
            /* Also check for typedef pointers which have is_ptr == 0 */
            else if (!lvalue.is_reference && lvalue.type &&
                     lvalue.type->ptr_level > 0) {
                /* Keep typedef-hidden depth: a void ** alias advances over
                 * pointer objects, whereas a direct void * was rejected above
                 * and never reaches this scaling path.
                 */
                increment_size = get_pointer_element_size(var);
            }

            /* If operand is a reference, read the value and push to stack for
             * the incoming addition/subtraction. Otherwise, use the top element
             * of stack as the one of operands and the destination.
             */
            if (one == 1) {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = pointee_type_from_pointer_typedef(lvalue.type);
                    vd->ptr_level = lvalue.value_ptr_level;
                    opstack_push(vd);
                    if (is_bitfield(lvalue.decl)) {
                        opstack_pop();
                        vd = read_bitfield_value(parent, bb, t, lvalue.decl);
                        opstack_push(vd);
                    } else {
                        add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                                 NULL);
                    }
                } else
                    t = operand_stack[operand_stack_idx - 1];

                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->init_val = increment_size;
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = vd;
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    if (is_bitfield(lvalue.decl))
                        write_bitfield_value(parent, bb, t, vd, lvalue.decl);
                    else
                        add_insn(parent, *bb, OP_write, NULL, t, vd, size,
                                 NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    mark_var_mutated(t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
                if (assignment_result) {
                    if (!lvalue.is_reference) {
                        assignment_result[0] = vd;
                    } else if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, t, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        var_t *stored = require_typed_var(parent, lvalue.type);
                        stored->ptr_level = lvalue.value_ptr_level;
                        stored->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, stored, t, NULL,
                                 lvalue.size, NULL);
                        assignment_result[0] = stored;
                    }
                }
            } else {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = pointee_type_from_pointer_typedef(lvalue.type);
                    vd->ptr_level = lvalue.value_ptr_level;
                    opstack_push(vd);
                    if (is_bitfield(lvalue.decl)) {
                        opstack_pop();
                        vd = read_bitfield_value(parent, bb, t, lvalue.decl);
                        opstack_push(vd);
                    } else {
                        add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                                 NULL);
                    }
                } else
                    t = operand_stack[operand_stack_idx - 1];

                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);

                    /* read_expr stops before `?`; compound assignment has the
                     * same conditional-expression RHS grammar as ordinary
                     * assignment.
                     */
                    read_ternary_operation(parent, bb);
                }

                var_t *rhs_val = opstack_pop();
                rhs_val = scalarize_array_literal_if_needed(
                    parent, bb, rhs_val, lvalue.type,
                    !lvalue.ptr_level &&
                        !(lvalue.type && lvalue.type->ptr_level) &&
                        !lvalue.is_reference);
                opstack_push(rhs_val);
                vd = require_var(parent);
                vd->init_val = increment_size;
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0,
                         NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = integer_binary_result_type(OP_mul, rs1, rs2);
                opstack_push(vd);
                add_insn(parent, *bb, OP_mul, vd, rs1, rs2, 0, NULL);

                rs2 = opstack_pop();
                rs1 = opstack_pop();

                /* Compound assignment performs the same integer promotions and
                 * usual arithmetic conversions as the corresponding binary
                 * operator, then converts the result back to the lvalue type.
                 * In particular, unsigned char and unsigned short must be
                 * promoted before unsigned division, rather than being consumed
                 * as sign-extended byte/halfword values by the backend.
                 */
                if (!is_pointer_operation(op, rs1, rs2)) {
                    rs1 = integer_promote_operand(parent, bb, rs1);
                    rs2 = integer_promote_operand(parent, bb, rs2);
                    normalize_integer_binary_operands(parent, bb, op, &rs1,
                                                      &rs2);
                }
                vd = require_var(parent);
                vd->var_name = gen_name();
                vd->type = integer_binary_result_type(op, rs1, rs2);
                add_insn(parent, *bb, op, vd, rs1, rs2, 0, NULL);

                if (lvalue.is_reference) {
                    if (is_bitfield(lvalue.decl))
                        write_bitfield_value(parent, bb, t, vd, lvalue.decl);
                    else
                        add_insn(parent, *bb, OP_write, NULL, t, vd,
                                 lvalue.size, NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
                if (assignment_result) {
                    /* Compound assignment has the value retained by its left
                     * operand. A bit-field store can truncate or sign-extend
                     * the arithmetic result, so its expression value must be
                     * reloaded just like an ordinary bit-field assignment.
                     */
                    if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, t, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        assignment_result[0] = vd;
                    }
                }
            }
        } else {
            if (!read_assignment_expression(parent, bb)) {
                read_expr(parent, bb);
                read_ternary_operation(parent, bb);
            }

            if (lvalue.is_func) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();

                /* is_func labels both function symbols and function-pointer
                 * variables. A variable on the RHS must contribute its stored
                 * pointer value, rather than its identifier being lowered as a
                 * function address.
                 */
                if (rs2->is_func && find_var(rs2->var_name, parent) == rs2) {
                    t = require_ref_var(parent, rs2->type, rs2->ptr_level);
                    t->var_name = gen_name();
                    add_insn(parent, *bb, OP_address_of, t, rs2, NULL, 0, NULL);

                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->func_target = rs2->func_target;
                    add_insn(parent, *bb, OP_read, vd, t, NULL, PTR_SIZE, NULL);
                    rs2 = vd;
                }

                /* Acquire destination address of lvalue if lvalue is a local
                 * variable.
                 */
                if (!lvalue.is_reference) {
                    var_t *addr =
                        require_ref_var(parent, lvalue.type, lvalue.ptr_level);
                    addr->var_name = gen_name();
                    add_insn(parent, *bb, OP_address_of, addr, rs1, NULL, 0,
                             NULL);
                    rs1 = addr;
                }

                add_insn(parent, *bb, OP_write, NULL, rs1, rs2, PTR_SIZE, NULL);

                /* Only a named function-pointer object owns this metadata. A
                 * record member or array element shares lvalue.decl with every
                 * object of that type, so storing provenance there would let
                 * one object's assignment authorize another's call. Those
                 * reference forms intentionally remain unknown until SSA
                 * provenance is available per storage location.
                 */
                if (!lvalue.is_reference && !lvalue.decl->func_target_invalid) {
                    func_t *assigned_target = rs2->func_target;

                    if (!assigned_target || !assigned_target->bbs) {
                        lvalue.decl->func_target = NULL;
                        lvalue.decl->func_target_invalid = true;
                    } else
                        lvalue.decl->func_target = assigned_target;
                }
                if (assignment_result)
                    assignment_result[0] = rs2;
            } else if (lvalue.is_reference) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();
                if (lvalue.pointee_func_signature) {
                    /* `slots[index]` denotes a callback slot whose descriptor
                     * is carried by the array, not by its scalar base type.
                     * Reconstruct that element target before writing so an
                     * ordinary indexed assignment observes the same prototype
                     * rule as a brace initializer.
                     */
                    var_t element_target = {0};

                    element_target.type = lvalue.decl->type;
                    element_target.ptr_level =
                        lvalue.decl->type->array_element_ptr_level;
                    element_target.pointee_func_signature =
                        lvalue.pointee_func_signature;
                    if (incompatible_pointee_callback_conversion(
                            rs2, &element_target))
                        error_at(
                            "incompatible callback slot types in array "
                            "assignment",
                            cur_token_loc());
                }
                if (is_bitfield(lvalue.decl))
                    write_bitfield_value(parent, bb, rs1, rs2, lvalue.decl);
                else
                    add_insn(parent, *bb, OP_write, NULL, rs1, rs2, size, NULL);
                if (assignment_result) {
                    /* The value of an assignment is the value retained by its
                     * left operand. A bit-field store may mask or sign-extend
                     * the source, so reload its extracted value instead of
                     * leaking the unconverted right operand.
                     */
                    if (is_bitfield(lvalue.decl)) {
                        var_t *stored =
                            read_bitfield_value(parent, bb, rs1, lvalue.decl);
                        stored->is_bitfield = false;
                        assignment_result[0] = stored;
                    } else {
                        var_t *stored = require_typed_var(parent, lvalue.type);
                        stored->ptr_level = lvalue.value_ptr_level;
                        stored->var_name = gen_name();
                        add_insn(parent, *bb, OP_read, stored, rs1, NULL, size,
                                 NULL);
                        assignment_result[0] = stored;
                    }
                }
            } else {
                rs1 = opstack_pop();
                vd = opstack_pop();
                rs1 = materialize_function_designator(parent, bb, rs1);
                if (is_record_object(vd) && is_record_object(rs1)) {
                    emit_record_copy(parent, bb, vd, rs1);

                    /* C99 assignment expressions have the assigned value,
                     * including struct and union copies. The source record is
                     * that value after the copy and can feed a grouped or
                     * larger expression without re-lowering the assignment.
                     */
                    if (assignment_result)
                        assignment_result[0] = rs1;
                } else if (incompatible_character_pointer_conversion(rs1, vd)) {
                    error_at(
                        "incompatible character pointer types in assignment",
                        cur_token_loc());
                } else if (incompatible_pointee_callback_conversion(rs1, vd)) {
                    error_at("incompatible callback slot types in assignment",
                             cur_token_loc());
                } else if (incompatible_const_pointer_conversion(rs1, vd)) {
                    diagnose_const_pointer_conversion(rs1, vd);
                } else {
                    rs1 = resize_var(parent, bb, rs1, vd);
                    mark_var_mutated(vd);
                    add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
                    if (assignment_result)
                        assignment_result[0] = rs1;
                }
            }
        }
        return true;
    }

    return false;
}

/* Assignment is right-associative and binds more weakly than the expression
 * parser's binary operators. Existing statement parsing has an lvalue-aware
 * lowering path; use a side-effect-free token scan to select that path before
 * parsing an expression, rather than attempting to rewind emitted IR.
 */
bool assignment_expression_follows(void)
{
    int depth = 0;

    for (token_t *t = cur_token->next; t; t = t->next) {
        switch (t->kind) {
        case T_open_bracket:
        case T_open_square:
        case T_open_curly:
            depth++;
            break;
        case T_close_bracket:
        case T_close_square:
        case T_close_curly:
            if (!depth)
                return false;
            depth--;
            break;
        case T_assign:
        case T_pluseq:
        case T_minuseq:
        case T_asteriskeq:
        case T_divideeq:
        case T_modeq:
        case T_lshifteq:
        case T_rshifteq:
        case T_andeq:
        case T_oreq:
        case T_xoreq:
            if (!depth)
                return true;
            break;
        case T_question:
            /* `?:` has lower precedence than assignment. An assignment in an
             * arm belongs to that conditional expression, not to the leading
             * lvalue that this scanner is classifying.
             */
            if (!depth)
                return false;
            break;
        case T_comma:
        case T_semicolon:
        case T_eof:
            if (!depth)
                return false;
            break;
        default:
            break;
        }
    }
    return false;
}

/* A compound literal begins with a parenthesized type name followed by `{`. Its
 * initializer may itself contain `=`, and the compound-literal lowering owns
 * the assignment which follows the closing brace.
 */
bool compound_literal_starts_expression(void)
{
    token_t *t = cur_token->next;
    int depth = 0;

    if (!t || t->kind != T_open_bracket)
        return false;
    for (; t; t = t->next) {
        if (t->kind == T_open_bracket)
            depth++;
        else if (t->kind == T_close_bracket && --depth == 0)
            return t->next && t->next->kind == T_open_curly;
    }
    return false;
}

bool read_assignment_expression(block_t *parent, basic_block_t **bb)
{
    char token[MAX_ID_LEN];
    var_t *result;
    int lvalue_paren_depth = 0;

    if (!assignment_expression_follows())
        return false;
    if (compound_literal_starts_expression())
        return false;

    /* An assignment operator after a balanced parenthesized prefix means the
     * parentheses wrap the left operand: `(item) = value`, unlike `(item =
     * value)`, whose assignment is nested and therefore not seen by the
     * top-level scan above.
     */
    while (lex_accept(T_open_bracket))
        lvalue_paren_depth++;

    if (lex_peek(T_asterisk, NULL)) {
        var_t *address;
        var_t *object_address;
        var_t *value;
        var_t *field = NULL;
        opcode_t compound_op = OP_generic;
        int store_size;
        int grouped_fixed_array_dimensions = 0;
        type_t *value_type;
        int value_ptr_level;

        /* Consume one dereference. read_expr() then evaluates its operand as an
         * address: this also retains the established **pp and *(p + n)
         * statement semantics.
         */
        lex_expect(T_asterisk);
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);
        address = opstack_pop();
        object_address = address;
        while (lvalue_paren_depth-- > 0)
            lex_expect(T_close_bracket);

        /* The ordinary `(*pointer)` assignment path historically stopped at the
         * grouping close. Admit only exact scalar fixed-array suffixes here,
         * retaining its common store/result lowering below.
         */
        if (lex_peek(T_open_square, NULL)) {
            var_t shape = {0};
            fixed_array_shape_t array_shape;
            int depth = 0;
            int dimensions;

            if (!address->pointee_array_size ||
                address->pointee_array_element_ptr_level ||
                effective_pointer_depth(address) != 1)
                error_at(
                    "Grouped subscript requires a scalar fixed array pointer",
                    cur_token_loc());
            shape.type = address->type->pointee_array_element_type
                             ? address->type->pointee_array_element_type
                             : address->type;
            array_shape = fixed_array_shape_from_pointee_var(address);
            fixed_array_shape_to_var(&shape, &array_shape);
            dimensions = array_shape.rank;
            grouped_fixed_array_dimensions = dimensions;
            if (dimensions > 4)
                error_at(
                    "Grouped fixed-array assignment supports at most four "
                    "dimensions",
                    cur_token_loc());
            while (lex_accept(T_open_square)) {
                var_t *index;
                var_t *scale;
                var_t *offset;
                var_t *next;
                int stride;

                if (depth >= dimensions)
                    error_at("Too many grouped array subscripts",
                             cur_token_loc());
                if (!read_assignment_expression(parent, bb)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                }
                index = opstack_pop();
                lex_expect(T_close_square);
                stride = fixed_array_subscript_stride(&shape, depth,
                                                      shape.type->size);
                offset = index;
                if (stride != 1) {
                    scale = require_var(parent);
                    scale->var_name = gen_name();
                    scale->init_val = stride;
                    add_insn(parent, *bb, OP_load_constant, scale, NULL, NULL,
                             0, NULL);
                    offset = require_var(parent);
                    offset->var_name = gen_name();
                    add_insn(parent, *bb, OP_mul, offset, index, scale, 0,
                             NULL);
                }
                next = require_typed_ptr_var(parent, shape.type, 1);
                next->var_name = gen_name();
                next->is_const_qualified = object_address->is_const_qualified ||
                                           shape.type->is_const_qualified;
                add_insn(parent, *bb, OP_add, next, address, offset, 0, NULL);
                address = next;
                depth++;
            }
            if (depth != dimensions)
                error_at("Grouped fixed-array assignment needs all subscripts",
                         cur_token_loc());
        }

        value_type = pointee_type_from_pointer_typedef(address->type);
        value_ptr_level = address->ptr_level > 1 ? address->ptr_level - 1 : 0;
        store_size = get_pointer_element_size(address);

        /* Parenthesized dereference may be followed by ordinary record-member
         * postfixes: `(*pointer).member = value`. Resolve their address before
         * selecting the shared scalar/bit-field store path.
         */
        if (lex_accept(T_dot)) {
            type_t *record_type = value_type;
            char field_name[MAX_ID_LEN];
            do {
                lex_ident(T_identifier, field_name);
                field = find_member(field_name, record_type);
                if (!field)
                    error_at("Unknown record member", cur_token_loc());
                address = compute_field_address(parent, bb, address, field);
                if (!lex_accept(T_dot))
                    break;
                if (!is_record_type(field->type) || field->ptr_level ||
                    field->array_size)
                    error_at("Member access requires a record",
                             cur_token_loc());
                record_type = field->type;
            } while (true);
            value_type = field->type;
            value_ptr_level = field->ptr_level;
            store_size = field->is_bitfield ? field->bit_storage_size
                                            : field->type->size;
        }

        int address_depth = effective_pointer_depth(object_address);
        unsigned int address_mask =
            effective_pointer_const_mask(object_address);
        if ((field && field->is_const_qualified) ||
            address->is_const_qualified ||
            (address_depth > 1 && address_depth <= 32 &&
             (address_mask & (1U << (address_depth - 2)))))
            error_at("assignment of read-only location", cur_token_loc());
        if (!lex_accept(T_assign) && !accept_compound_assign_op(&compound_op))
            error_at("Expected assignment after pointer dereference",
                     cur_token_loc());
        if (grouped_fixed_array_dimensions >= 3 && compound_op != OP_generic &&
            compound_op != OP_add && compound_op != OP_sub &&
            compound_op != OP_mul && compound_op != OP_div &&
            compound_op != OP_mod && compound_op != OP_lshift &&
            compound_op != OP_rshift && compound_op != OP_bit_and &&
            compound_op != OP_bit_xor && compound_op != OP_bit_or)
            error_at("Unsupported rank-three/four grouped compound assignment",
                     cur_token_loc());

        if (!read_assignment_expression(parent, bb)) {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
        }
        value = opstack_pop();

        if (grouped_fixed_array_dimensions >= 3 && compound_op != OP_generic &&
            (value_ptr_level != 0 || is_record_type(value_type) ||
             value_type->is_floating || value->is_func ||
             is_pointer_like_value(value) || is_record_type(value->type) ||
             value->type->is_floating))
            error_at(
                "Invalid rank-three/four grouped compound assignment operand",
                cur_token_loc());

        if (compound_op != OP_generic) {
            var_t *current;
            if (field && is_bitfield(field)) {
                current = read_bitfield_value(parent, bb, address, field);
            } else {
                current = require_typed_var(parent, value_type);
                current->ptr_level = value_ptr_level;
                current->var_name = gen_name();
                add_insn(parent, *bb, OP_read, current, address, NULL,
                         store_size, NULL);
            }
            if (!is_pointer_operation(compound_op, current, value)) {
                current = integer_promote_operand(parent, bb, current);
                value = integer_promote_operand(parent, bb, value);
                normalize_integer_binary_operands(parent, bb, compound_op,
                                                  &current, &value);
            }
            var_t *combined = require_var(parent);
            combined->var_name = gen_name();
            combined->type =
                integer_binary_result_type(compound_op, current, value);
            add_insn(parent, *bb, compound_op, combined, current, value, 0,
                     NULL);
            value = combined;
        }

        if (field && is_bitfield(field))
            write_bitfield_value(parent, bb, address, value, field);
        else
            add_insn(parent, *bb, OP_write, NULL, address, value, store_size,
                     NULL);

        /* Reload after the store: assignment expressions observe the stored
         * object representation, including narrow integer conversion.
         */
        if (field && is_bitfield(field)) {
            result = read_bitfield_value(parent, bb, address, field);
            result->is_bitfield = false;
        } else {
            result = require_typed_var(parent, value_type);
            result->ptr_level = value_ptr_level;
            result->var_name = gen_name();
            add_insn(parent, *bb, OP_read, result, address, NULL, store_size,
                     NULL);
        }
        if (result->type == TY_bool && !result->ptr_level)
            result = normalize_bool(parent, bb, result);
        opstack_push(result);
        return true;
    }

    /* Identifier-rooted lvalues include direct objects, member access, and
     * subscripts through the shared lvalue parser.
     */
    if (!lex_peek(T_identifier, token))
        return false;

    if (!read_body_assignment(token, parent, OP_generic, bb, &result,
                              lvalue_paren_depth))
        return false;
    if (!result)
        error_at("Assignment expression does not yield a value",
                 cur_token_loc());
    opstack_push(result);
    return true;
}

/* Return the extent left after selecting leading array dimensions. The global
 * constant evaluator retains dimensions in var_t instead of constructing an
 * expression IR, so every sizeof object path uses this one calculation.
 */
static int sizeof_subscripted_array(var_t *object, int subscript_depth)
{
    int res;

    if (!subscript_depth)
        return size_var(object);

    res = object->type->size;
    if (subscript_depth == 1 && object->array_dim2 > 0) {
        res *= object->array_dim2;
        if (object->array_dim3 > 0)
            res *= object->array_dim3;
        if (object->array_dim4 > 0)
            res *= object->array_dim4;
    } else if (subscript_depth == 2 && object->array_dim3 > 0) {
        res *= object->array_dim3;
        if (object->array_dim4 > 0)
            res *= object->array_dim4;
    } else if (subscript_depth == 3 && object->array_dim4 > 0) {
        res *= object->array_dim4;
    }
    return res;
}

static void validate_global_sizeof_object(var_t *object)
{
    if (object->is_func)
        error_at("sizeof(function) is invalid", cur_token_loc());
    if (is_bitfield(object))
        error_at("sizeof cannot be applied to a bit-field", cur_token_loc());
    if (object->is_flexible_array_member)
        error_at("sizeof cannot be applied to a flexible array member",
                 cur_token_loc());
}

/* Resolve unevaluated record postfixes while preserving an array member's
 * declared dimensions for the global sizeof constant evaluator.
 */
static var_t *read_const_object_members(var_t *object)
{
    while (lex_peek(T_dot, NULL) || lex_peek(T_arrow, NULL)) {
        bool through_pointer = lex_accept(T_arrow);
        char field_name[MAX_ID_LEN];
        var_t *field;
        int pointer_depth = effective_pointer_depth(object);

        if ((through_pointer && pointer_depth != 1) ||
            (!through_pointer && pointer_depth != 0))
            error_at("Invalid record member access", cur_token_loc());
        if (!through_pointer)
            lex_expect(T_dot);
        lex_ident(T_identifier, field_name);
        field = find_member(field_name, object->type);
        if (!field)
            error_at("Unknown record member", cur_token_loc());
        object = field;
    }
    return object;
}

/* Parse the object portion of a constant address expression. sizeof only needs
 * its pointer type, but still applies the ordinary addressability constraints.
 */
static var_t *read_const_addressed_object(block_t *scope, int *subscript_depth)
{
    char buffer[MAX_TOKEN_LEN];
    var_t *object;

    lex_expect(T_ampersand);
    if (subscript_depth)
        *subscript_depth = 0;
    lex_ident(T_identifier, buffer);
    object = find_var(buffer, scope);
    if (!object)
        error_at("sizeof address requires a declared object", cur_token_loc());
    object = read_const_object_members(object);
    while (lex_accept(T_open_square)) {
        if (!object->array_size && !object->has_unsized_array)
            error_at("Cannot apply square operator to non-array",
                     cur_token_loc());
        read_const_expr(scope);
        lex_expect(T_close_square);
        if (subscript_depth)
            *subscript_depth = *subscript_depth + 1;
    }
    if (is_bitfield(object))
        error_at("Cannot take address of bit-field", cur_token_loc());
    return object;
}

/* Use the ordinary sizeof expression parser in a detached block. Global
 * initializers need the operand's type, never its generated instructions or
 * side effects. cur_token remains the already-consumed sizeof token.
 */
static int read_global_sizeof_expression(block_t *scope)
{
    basic_block_t *unevaluated_bb = bb_create(scope);
    int saved_side_effects = se_idx;
    var_t *result;

    handle_sizeof_operator(scope, &unevaluated_bb);
    result = opstack_pop();
    se_idx = saved_side_effects;
    return result->init_val;
}

static int read_const_string_size(void)
{
    char buffer[MAX_STRING_LEN];
    char unescaped[MAX_STRING_LEN];
    int res = 0;

    do {
        int length;

        lex_ident(T_string, buffer);
        length = unescape_string(buffer, unescaped, sizeof(unescaped));
        if (length < 0)
            error_at("Invalid escape sequence", cur_token_loc());
        res += length;
    } while (lex_peek(T_string, buffer));
    return res + 1;
}

int read_const_wstring_size(void)
{
    int values[MAX_STRING_LEN];
    type_t *wide_type = find_type("wchar_t", true);
    return (read_wstring_units(values, MAX_STRING_LEN) + 1) * wide_type->size;
}

int read_primary_constant(block_t *scope)
{
    /* return signed constant */
    int isneg = 0, res;

    /* This recurses once per nesting level of a constant expression, so the
     * buffer holds only what is copied into it: a number, a character constant
     * or an identifier, each at most MAX_TOKEN_LEN. A string is only tested for
     * and never copied.
     */
    char buffer[MAX_TOKEN_LEN];
    if (lex_accept(T_minus))
        isneg = 1;
    if (lex_accept(T_sizeof)) {
        /* The common scalar expression form needs no IR in a global constant:
         * integer and character literals have type int. Type names continue
         * through the declaration-only evaluator below.
         */
        token_t *inside =
            lex_peek(T_open_bracket, NULL) ? cur_token->next->next : NULL;
        var_t *nested_array = NULL;
        token_t *nested_root = NULL;
        token_t *nested_after = NULL;
        int nested_groups = 0;

        if (inside && inside->kind == T_open_bracket) {
            token_t *token = inside;

            while (token && token->kind == T_open_bracket) {
                nested_groups++;
                token = token->next;
            }
            nested_root = token;
            if (token && token->kind == T_identifier)
                nested_array = find_var(token->literal, scope);
            nested_after = token ? token->next : NULL;
            for (int i = 0; i < nested_groups && nested_after &&
                            nested_after->kind == T_close_bracket;
                 i++)
                nested_after = nested_after->next;
        }
        if (can_scan_sizeof_postfix_extent(
                scope,
                lex_peek(T_open_bracket, NULL) ? cur_token->next->next
                                               : cur_token->next,
                lex_peek(T_open_bracket, NULL), false)) {
            /* The shared postfix walker has already proved this is an
             * identifier-rooted fixed array extent, so the detached parser can
             * preserve it without a global-prefix spelling table.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   inside->next->kind == T_string) {
            lex_expect(T_open_bracket);
            lex_expect(T_open_bracket);
            res = read_const_string_size();
            lex_expect(T_close_bracket);
            lex_expect(T_close_bracket);
        } else if (inside && inside->kind == T_wstring) {
            res = read_const_wstring_size();
        } else if (nested_array && nested_array->array_size > 0 &&
                   nested_groups > 0 && nested_after &&
                   nested_after->kind == T_close_bracket) {
            lex_expect(T_open_bracket);
            for (int i = 0; i < nested_groups; i++)
                lex_expect(T_open_bracket);
            lex_expect(T_identifier);
            for (int i = 0; i < nested_groups; i++)
                lex_expect(T_close_bracket);
            lex_expect(T_close_bracket);
            res = size_var(nested_array);
        } else if (nested_array && nested_groups > 0 && nested_root &&
                   nested_root->next &&
                   (nested_root->next->kind == T_dot ||
                    nested_root->next->kind == T_arrow)) {
            /* The local sizeof helper preserves a member-array lvalue through
             * arbitrary grouping before a postfix subscript. Reuse it in a
             * detached block for a global constant instead of duplicating that
             * type-only traversal here.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   inside->next->kind == T_open_bracket && inside->next->next &&
                   inside->next->next->kind == T_identifier) {
            var_t *grouped_object =
                find_var(inside->next->next->literal, scope);

            if (grouped_object && !grouped_object->array_size &&
                !grouped_object->has_unsized_array)
                res = read_global_sizeof_expression(scope);
            else
                res = read_const_sizeof_type(scope);
        } else if (inside &&
                   (inside->kind == T_increment ||
                    inside->kind == T_decrement || inside->kind == T_plus ||
                    inside->kind == T_minus || inside->kind == T_bit_not ||
                    inside->kind == T_log_not)) {
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   ((inside->next->kind == T_identifier &&
                     find_type(inside->next->literal, true)) ||
                    inside->next->kind == T_signed ||
                    inside->next->kind == T_unsigned ||
                    inside->next->kind == T_long ||
                    inside->next->kind == T_const ||
                    inside->next->kind == T_volatile ||
                    inside->next->kind == T_struct ||
                    inside->next->kind == T_union ||
                    inside->next->kind == T_enum)) {
            /* A cast begins with an extra parenthesis and cannot be evaluated
             * as an integer constant when its operand is a declared object.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_identifier &&
                   (find_var(inside->literal, scope) ||
                    find_func(inside->literal)) &&
                   inside->next && inside->next->kind != T_close_bracket &&
                   inside->next->kind != T_dot &&
                   inside->next->kind != T_arrow &&
                   inside->next->kind != T_open_square) {
            /* Parenthesized non-postfix operands need type derivation rather
             * than constant evaluation: sizeof(global + 1), sizeof(global = 1),
             * and calls are all unevaluated but need their expression type.
             * Reuse the detached expression path used for local sizeof.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   inside->next->kind == T_asterisk && inside->next->next &&
                   inside->next->next->kind == T_identifier &&
                   find_var(inside->next->next->literal, scope)) {
            var_t *pointer = find_var(inside->next->next->literal, scope);
            var_t dereferenced;
            var_t *object;
            int subscript_depth = 0;

            if (effective_pointer_depth(pointer) != 1)
                error_at("Cannot dereference non-pointer in sizeof",
                         cur_token_loc());

            /* This is compile-time descriptor manipulation, not a source
             * aggregate expression. A direct `dereferenced = *pointer` becomes
             * one wide OP_read while self-hosting; ARM's scalar load backend
             * correctly admits only 1/2/4-byte reads. Copy through libc
             * instead, which keeps the generated compiler IR word-sized and
             * preserves every descriptor field.
             */
            memcpy(&dereferenced, pointer, sizeof(dereferenced));
            dereferenced.type =
                pointee_type_from_pointer_typedef(pointer->type);
            dereferenced.ptr_level = 0;
            object = &dereferenced;

            lex_expect(T_open_bracket);
            lex_expect(T_open_bracket);
            lex_expect(T_asterisk);
            lex_expect(T_identifier);
            lex_expect(T_close_bracket);
            object = read_const_object_members(object);
            while (lex_accept(T_open_square)) {
                if (!object->array_size && !object->has_unsized_array)
                    error_at("Cannot apply square operator to non-array",
                             cur_token_loc());
                read_const_expr(scope);
                lex_expect(T_close_square);
                subscript_depth++;
            }
            lex_expect(T_close_bracket);
            validate_global_sizeof_object(object);
            res = sizeof_subscripted_array(object, subscript_depth);
        } else if (inside && inside->kind == T_asterisk && inside->next &&
                   inside->next->kind == T_identifier && inside->next->next &&
                   inside->next->next->kind == T_open_square &&
                   is_direct_fixed_array_pointer_slot(
                       find_var(inside->next->literal, scope))) {
            /* The shared direct-pointer helper also owns the supported global
             * pointer-array slot form.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_asterisk && inside->next &&
                   inside->next->kind == T_ampersand) {
            var_t *object;
            int subscript_depth;

            lex_expect(T_open_bracket);
            lex_expect(T_asterisk);
            object = read_const_addressed_object(scope, &subscript_depth);
            lex_expect(T_close_bracket);
            res = sizeof_subscripted_array(object, subscript_depth);
        } else if (inside && inside->kind == T_ampersand) {
            lex_expect(T_open_bracket);
            read_const_addressed_object(scope, NULL);
            lex_expect(T_close_bracket);
            res = PTR_SIZE;
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   inside->next->kind == T_identifier &&
                   find_var(inside->next->literal, scope) &&
                   inside->next->next &&
                   inside->next->next->kind != T_close_bracket &&
                   inside->next->next->kind != T_dot &&
                   inside->next->next->kind != T_arrow &&
                   inside->next->next->kind != T_open_square) {
            /* A grouped object followed by an operator is an ordinary
             * parenthesized expression, not the array/member postfix form
             * handled below.
             */
            res = read_global_sizeof_expression(scope);
        } else if (inside && inside->kind == T_open_bracket && inside->next &&
                   inside->next->kind == T_identifier &&
                   find_var(inside->next->literal, scope)) {
            /* Preserve the array object type through an explicitly grouped
             * object expression, e.g. sizeof((record.items)[0]).
             */
            var_t *object = find_var(inside->next->literal, scope);
            int subscript_depth = 0;

            lex_expect(T_open_bracket);
            lex_expect(T_open_bracket);
            lex_expect(T_identifier);
            object = read_const_object_members(object);
            lex_expect(T_close_bracket);
            while (lex_accept(T_open_square)) {
                if (!object->array_size && !object->has_unsized_array)
                    error_at("Cannot apply square operator to non-array",
                             cur_token_loc());
                read_const_expr(scope);
                lex_expect(T_close_square);
                subscript_depth++;
            }
            lex_expect(T_close_bracket);
            validate_global_sizeof_object(object);
            res = sizeof_subscripted_array(object, subscript_depth);
        } else if (inside && inside->kind == T_open_bracket) {
            lex_expect(T_open_bracket);
            read_const_expr(scope);
            lex_expect(T_close_bracket);
            res = TY_int->size;
        } else if (inside &&
                   (inside->kind == T_numeric || inside->kind == T_char ||
                    inside->kind == T_wchar)) {
            lex_expect(T_open_bracket);
            read_const_expr(scope);
            lex_expect(T_close_bracket);
            res = TY_int->size;
        } else if (inside && inside->kind == T_identifier) {
            var_t *object = find_var(inside->literal, scope);
            constant_t *constant = find_scoped_constant(inside->literal, scope);

            if (object || constant) {
                lex_expect(T_open_bracket);
                lex_expect(T_identifier);
                if (object) {
                    int subscript_depth = 0;

                    object = read_const_object_members(object);
                    while (lex_accept(T_open_square)) {
                        if (!object->array_size && !object->has_unsized_array)
                            error_at(
                                "Cannot apply square operator to non-array",
                                cur_token_loc());
                        read_const_expr(scope);
                        lex_expect(T_close_square);
                        subscript_depth++;
                    }
                    lex_expect(T_close_bracket);
                    validate_global_sizeof_object(object);
                    res = sizeof_subscripted_array(object, subscript_depth);
                } else {
                    lex_expect(T_close_bracket);
                    res = TY_int->size;
                }
            } else {
                res = read_const_sizeof_type(scope);
            }
        } else if (lex_peek(T_numeric, buffer) || lex_peek(T_char, buffer) ||
                   lex_peek(T_wchar, buffer)) {
            read_primary_constant(scope);
            res = TY_int->size;
        } else if (lex_peek(T_string, NULL)) {
            res = read_const_string_size();
        } else if (lex_peek(T_wstring, NULL)) {
            res = read_const_wstring_size();
        } else if (lex_peek(T_ampersand, NULL)) {
            read_const_addressed_object(scope, NULL);
            res = PTR_SIZE;
        } else if (lex_accept(T_asterisk)) {
            var_t *object;
            int subscript_depth;

            object = read_const_addressed_object(scope, &subscript_depth);
            res = sizeof_subscripted_array(object, subscript_depth);
        } else if (lex_peek(T_minus, NULL) || lex_peek(T_plus, NULL) ||
                   lex_peek(T_bit_not, NULL) || lex_peek(T_log_not, NULL)) {
            res = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_identifier, buffer)) {
            var_t *object = find_var(buffer, scope);
            constant_t *constant = find_scoped_constant(buffer, scope);

            lex_expect(T_identifier);
            if (object) {
                int subscript_depth = 0;

                object = read_const_object_members(object);
                while (lex_accept(T_open_square)) {
                    if (!object->array_size && !object->has_unsized_array)
                        error_at("Cannot apply square operator to non-array",
                                 cur_token_loc());
                    read_const_expr(scope);
                    lex_expect(T_close_square);
                    subscript_depth++;
                }
                validate_global_sizeof_object(object);
                res = sizeof_subscripted_array(object, subscript_depth);
            } else if (constant) {
                res = TY_int->size;
            } else {
                error_at("sizeof requires a type or declared object",
                         cur_token_loc());
                res = 0;
            }
        } else {
            res = read_const_sizeof_type(scope);
        }
    } else if (lex_accept(T_open_bracket)) {
        res = read_primary_constant(scope);
        lex_expect(T_close_bracket);
    } else if (lex_peek(T_numeric, buffer)) {
        res = parse_numeric_constant(buffer);
        lex_expect(T_numeric);
    } else if (lex_peek(T_char, buffer) || lex_peek(T_wchar, buffer)) {
        char unescaped[MAX_TOKEN_LEN];
        unescape_string(buffer, unescaped, MAX_TOKEN_LEN);
        res = lex_peek(T_wchar, NULL) ? parse_wide_character_constant(buffer)
                                      : parse_character_constant(buffer);
        lex_expect(lex_peek(T_wchar, NULL) ? T_wchar : T_char);
    } else if (lex_peek(T_identifier, buffer)) {
        constant_t *con;

        if (!strcmp(buffer, "__builtin_offsetof")) {
            res = read_const_expr_operand(scope);
            if (isneg)
                return (-1) * res;
            return res;
        }
        lex_expect(T_identifier);
        con = find_scoped_constant(buffer, scope);
        if (!con)
            error_at("Identifier is not an integer constant", next_token_loc());
        res = con->value;
    } else
        error_at("Invalid value after assignment", next_token_loc());
    if (isneg)
        return (-1) * res;
    return res;
}

int eval_expression_imm(opcode_t op, int op1, int op2)
{
    /* return immediate result */
    int tmp = op2;
    int res = 0;
    switch (op) {
    case OP_add:
        if (checking_enum_constant && ((op2 > 0 && op1 > INT_MAX - op2) ||
                                       (op2 < 0 && op1 < INT_MIN - op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        res = op1 + op2;
        break;
    case OP_sub:
        if (checking_enum_constant && ((op2 < 0 && op1 > INT_MAX + op2) ||
                                       (op2 > 0 && op1 < INT_MIN + op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        res = op1 - op2;
        break;
    case OP_mul:
        if (checking_enum_constant && op1 && op2 &&
            ((op1 > 0 && op2 > 0 && op1 > INT_MAX / op2) ||
             (op1 > 0 && op2 < 0 && op2 < INT_MIN / op1) ||
             (op1 < 0 && op2 > 0 && op1 < INT_MIN / op2) ||
             (op1 < 0 && op2 < 0 && op1 < INT_MAX / op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        res = op1 * op2;
        break;
    case OP_div:
        if (!op2)
            error_at("Division by zero in constant expression",
                     cur_token_loc());

        /* INT_MIN / -1 has no representable result; on x86 it raises SIGFPE
         * rather than producing one.
         */
        if (op1 == INT_MIN && op2 == -1)
            error_at("Overflow in constant expression", cur_token_loc());
        res = op1 / op2;
        break;
    case OP_mod:
        if (!op2)
            error_at("Modulo by zero in constant expression", cur_token_loc());
        if (op1 == INT_MIN && op2 == -1)
            error_at("Overflow in constant expression", cur_token_loc());
        /* Use bitwise AND for modulo optimization when divisor is power of 2 */
        if (tmp == INT_MIN) {
            res = op1 % op2;
            break;
        }
        tmp = tmp < 0 ? -tmp : tmp;
        tmp &= (tmp - 1);
        if (tmp != 0) {
            res = op1 % op2;
            break;
        }
        op2 = op2 < 0 ? -op2 : op2;
        res = op1 & (op2 - 1);
        if (op1 < 0 && res != 0)
            res -= op2;
        break;
    case OP_lshift:
        if (checking_enum_constant &&
            (op2 < 0 || op2 >= 32 || op1 < 0 || op1 > (INT_MAX >> op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        res = op1 << op2;
        break;
    case OP_rshift:
        res = op1 >> op2;
        break;
    case OP_log_and:
        res = op1 && op2;
        break;
    case OP_log_or:
        res = op1 || op2;
        break;
    case OP_eq:
        res = op1 == op2;
        break;
    case OP_neq:
        res = op1 != op2;
        break;
    case OP_lt:
        res = op1 < op2;
        break;
    case OP_gt:
        res = op1 > op2;
        break;
    case OP_leq:
        res = op1 <= op2;
        break;
    case OP_geq:
        res = op1 >= op2;
        break;
    case OP_bit_and:
        res = op1 & op2;
        break;
    case OP_bit_or:
        res = op1 | op2;
        break;
    case OP_bit_xor:
        res = op1 ^ op2;
        break;
    default:
        error_at("The requested operation is not supported.", cur_token_loc());
    }
    return res;
}

bool read_global_assignment_var(var_t *var);

void emit_global_scalar_assignment(block_t *parent,
                                   basic_block_t *bb,
                                   var_t *dest,
                                   var_t *src)
{
    if (!dest->ptr_level && dest->type == TY_bool)
        src->init_val = src->init_val != 0;
    add_insn(parent, bb, OP_assign, dest, src, NULL, 0, NULL);
}

/* Keep the legacy word-sized evaluator for ordinary constants and casts, but
 * select the two-word path whenever a literal-only initializer contains a wide
 * token. Looking past leading narrow operands matters for expressions such as
 * `(3 + 0x100000000LL)`: the old evaluator would consume the later token before
 * it had a chance to preserve its high word.
 */
bool typed_global_literal_appears_before_initializer_end(token_t *token)
{
    int bracket_depth = 0;

    for (; token; token = token->next) {
        if (token->kind == T_open_bracket)
            bracket_depth++;
        else if (token->kind == T_close_bracket) {
            if (bracket_depth == 0)
                return false;
            bracket_depth--;
        } else if (bracket_depth == 0 &&
                   (token->kind == T_semicolon || token->kind == T_comma))
            return false;
        else if (token->kind == T_numeric &&
                 (numeric_literal_needs_wide_path(token->literal) ||
                  numeric_literal_needs_typed_global_path(token->literal)))
            return true;
    }
    return false;
}

var_t *read_wide_global_literal_expression(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope);

/* Global address construction currently carries its scaled index as an int.
 * Accept a wide constant expression when its final value is representable by
 * that index, but never silently discard its high word.
 */
int narrow_wide_global_address_offset(var_t *value)
{
    if ((value->type->is_unsigned &&
         (value->init_val_hi || (unsigned int) value->init_val > INT_MAX)) ||
        (!value->type->is_unsigned &&
         value->init_val_hi != (value->init_val < 0 ? -1 : 0)))
        error_at("Global address offset exceeds supported integer range",
                 cur_token_loc());
    return value->init_val;
}

/* A global pointer offset may use the same literal-only wide expression as a
 * scalar global initializer. Its final index still flows through the current
 * word-sized address relocation representation.
 */
int read_global_address_offset(block_t *scope,
                               block_t *parent,
                               basic_block_t *bb)
{
    token_t *operator_token = cur_token->next;

    if (operator_token && operator_token->next &&
        typed_global_literal_appears_before_initializer_end(
            operator_token->next)) {
        bool negate = lex_accept(T_minus);
        var_t *wide_offset;
        int index;

        if (!negate)
            lex_expect(T_plus);
        wide_offset = read_wide_global_literal_expression(parent, bb, scope);
        index = narrow_wide_global_address_offset(wide_offset);
        return negate ? -index : index;
    }
    return read_const_expr(scope);
}

static int wide_global_unevaluated_depth;

/* Fold the operations that only need word arithmetic before emitting global
 * setup code. That setup block is deliberately conservative about constants,
 * and previously allowed a paired add/subtract to lose its high half.
 */
bool emit_wide_global_word_arithmetic(block_t *parent,
                                      basic_block_t *bb,
                                      var_t *result,
                                      opcode_t op,
                                      var_t *left,
                                      var_t *right)
{
    unsigned int lo = (unsigned int) left->init_val;
    unsigned int hi = (unsigned int) left->init_val_hi;
    unsigned int rhs_lo = right ? (unsigned int) right->init_val : 0;
    unsigned int rhs_hi = right ? (unsigned int) right->init_val_hi : 0;
    unsigned int out_lo, out_hi;

    if (wide_global_unevaluated_depth) {
        result->init_val = 0;
        result->init_val_hi = 0;
        result->is_const = true;
        return true;
    }

    /* The restricted global evaluator selects a conditional arm while parsing,
     * so comparison results must carry their constant payload rather than exist
     * only as setup-block IR. Keep this in the compiler's existing two-word
     * representation: it must also self-host on targets without a complete
     * host-level 64-bit value ABI.
     */
    if (op == OP_eq || op == OP_neq || op == OP_lt || op == OP_leq ||
        op == OP_gt || op == OP_geq) {
        type_t *common = integer_common_type(left, right);
        bool equal;
        bool less;
        bool comparison;

        /* Materialize the usual arithmetic conversion in word form. A narrow
         * signed source sign-extends to either signed long long or unsigned
         * long long; a narrow unsigned source zero-extends in both cases.
         */
        if (left->type->size <= TY_int->size)
            hi = left->type->is_unsigned || !(lo & 0x80000000U) ? 0
                                                                : 0xffffffffU;
        if (right->type->size <= TY_int->size)
            rhs_hi = right->type->is_unsigned || !(rhs_lo & 0x80000000U)
                         ? 0
                         : 0xffffffffU;

        if (common->size <= TY_int->size) {
            equal = lo == rhs_lo;
            if (common->is_unsigned)
                less = lo < rhs_lo;
            else if ((lo ^ rhs_lo) & 0x80000000U)
                less = lo & 0x80000000U;
            else
                less = lo < rhs_lo;
        } else {
            equal = hi == rhs_hi && lo == rhs_lo;
            if (common->is_unsigned)
                less = hi < rhs_hi || (hi == rhs_hi && lo < rhs_lo);
            else if ((hi ^ rhs_hi) & 0x80000000U)
                less = hi & 0x80000000U;
            else
                less = hi < rhs_hi || (hi == rhs_hi && lo < rhs_lo);
        }

        switch (op) {
        case OP_eq:
            comparison = equal;
            break;
        case OP_neq:
            comparison = !equal;
            break;
        case OP_lt:
            comparison = less;
            break;
        case OP_leq:
            comparison = less || equal;
            break;
        case OP_gt:
            comparison = !less && !equal;
            break;
        default:
            comparison = !less;
            break;
        }
        result->init_val = comparison;
        result->init_val_hi = 0;
        result->is_const = true;
        add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
        return true;
    }

    if (op == OP_log_and || op == OP_log_or) {
        bool left_true = lo || hi;
        bool right_true = rhs_lo || rhs_hi;

        result->init_val = op == OP_log_and ? left_true && right_true
                                            : left_true || right_true;
        result->init_val_hi = 0;
        result->is_const = true;
        add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
        return true;
    }

    if (op == OP_div || op == OP_mod) {
        unsigned int rem_lo = 0, rem_hi = 0, quo_lo = 0, quo_hi = 0;
        bool is_unsigned = left->type->is_unsigned || right->type->is_unsigned;
        bool neg_left = !is_unsigned && (hi >> 31);
        bool neg_right = !is_unsigned && (rhs_hi >> 31);

        if (rhs_lo == 0 && rhs_hi == 0)
            error_at("division by zero in global constant expression",
                     cur_token_loc());
        if (neg_left) {
            lo = ~lo + 1;
            hi = ~hi + (lo == 0);
        }
        if (neg_right) {
            rhs_lo = ~rhs_lo + 1;
            rhs_hi = ~rhs_hi + (rhs_lo == 0);
        }
        for (int i = 0; i < 64; i++) {
            unsigned int incoming = hi >> 31;

            hi = (hi << 1) | (lo >> 31);
            lo <<= 1;
            rem_hi = (rem_hi << 1) | (rem_lo >> 31);
            rem_lo = (rem_lo << 1) | incoming;
            quo_hi = (quo_hi << 1) | (quo_lo >> 31);
            quo_lo <<= 1;
            if (rem_hi > rhs_hi || (rem_hi == rhs_hi && rem_lo >= rhs_lo)) {
                unsigned int borrow = rem_lo < rhs_lo;

                rem_lo -= rhs_lo;
                rem_hi = rem_hi - rhs_hi - borrow;
                quo_lo |= 1;
            }
        }
        if (op == OP_div) {
            out_lo = quo_lo;
            out_hi = quo_hi;
            if (neg_left != neg_right) {
                out_lo = ~out_lo + 1;
                out_hi = ~out_hi + (out_lo == 0);
            }
        } else {
            out_lo = rem_lo;
            out_hi = rem_hi;
            if (neg_left) {
                out_lo = ~out_lo + 1;
                out_hi = ~out_hi + (out_lo == 0);
            }
        }
        result->init_val = out_lo;
        result->init_val_hi = out_hi;
        result->is_const = true;
        add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
        return true;
    }

    switch (op) {
    case OP_add:
        out_lo = lo + rhs_lo;
        out_hi = hi + rhs_hi + (out_lo < lo);
        break;
    case OP_sub:
        out_lo = lo - rhs_lo;
        out_hi = hi - rhs_hi - (lo < rhs_lo);
        break;
    case OP_mul: {
        unsigned int p0 = (lo & 0xffffU) * (rhs_lo & 0xffffU);
        unsigned int p1 = (lo & 0xffffU) * (rhs_lo >> 16);
        unsigned int p2 = (lo >> 16) * (rhs_lo & 0xffffU);
        unsigned int p3 = (lo >> 16) * (rhs_lo >> 16);
        unsigned int carry = (p0 >> 16) + (p1 & 0xffffU) + (p2 & 0xffffU);

        out_lo = (p0 & 0xffffU) | (carry << 16);
        out_hi = p3 + (p1 >> 16) + (p2 >> 16) + (carry >> 16) + hi * rhs_lo +
                 lo * rhs_hi;
        break;
    }
    case OP_lshift:
        if (rhs_lo >= 64) {
            out_lo = 0;
            out_hi = 0;
        } else if (rhs_lo >= 32) {
            out_lo = 0;
            out_hi = lo << (rhs_lo - 32);
        } else if (rhs_lo == 0) {
            out_lo = lo;
            out_hi = hi;
        } else {
            out_lo = lo << rhs_lo;
            out_hi = (hi << rhs_lo) | (lo >> (32 - rhs_lo));
        }
        break;
    case OP_rshift:
        if (rhs_lo >= 64) {
            out_hi = left->type->is_unsigned || !(hi >> 31) ? 0 : ~0U;
            out_lo = out_hi;
        } else if (rhs_lo >= 32) {
            out_lo = left->type->is_unsigned
                         ? hi >> (rhs_lo - 32)
                         : (unsigned int) ((int) hi >> (rhs_lo - 32));
            out_hi = left->type->is_unsigned || !(hi >> 31) ? 0 : ~0U;
        } else if (rhs_lo == 0) {
            out_lo = lo;
            out_hi = hi;
        } else {
            out_lo = (lo >> rhs_lo) | (hi << (32 - rhs_lo));
            out_hi = left->type->is_unsigned
                         ? hi >> rhs_lo
                         : (unsigned int) ((int) hi >> rhs_lo);
        }
        break;
    case OP_bit_and:
        out_lo = lo & rhs_lo;
        out_hi = hi & rhs_hi;
        break;
    case OP_bit_or:
        out_lo = lo | rhs_lo;
        out_hi = hi | rhs_hi;
        break;
    case OP_bit_xor:
        out_lo = lo ^ rhs_lo;
        out_hi = hi ^ rhs_hi;
        break;
    case OP_bit_not:
        out_lo = ~lo;
        out_hi = ~hi;
        break;
    default:
        return false;
    }
    result->init_val = out_lo;
    result->init_val_hi = out_hi;
    result->is_const = true;
    add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
    return true;
}

/* A grouped primary is lowered into the same global setup block as its parent.
 * The caller owns the closing parenthesis, so get_operator() naturally stops an
 * inner precedence stack without consuming its delimiter.
 */
var_t *read_wide_global_literal_primary(block_t *parent,
                                        basic_block_t *bb,
                                        block_t *scope)
{
    char literal[MAX_TOKEN_LEN];
    var_t *value;

    /* Keep wide unary operators out of the legacy word-sized constant
     * evaluator. Besides making `~0ULL` usable in a static initializer, the
     * recursive form gives grouped operands and repeated unary operators the
     * same semantics as ordinary expression parsing.
     */
    if (lex_accept(T_plus))
        return read_wide_global_literal_primary(parent, bb, scope);
    if (lex_accept(T_minus)) {
        var_t *zero = require_var(parent);
        var_t *result;

        /* Preserve the special lexical treatment of the magnitude of LLONG_MIN.
         * read_numeric_param() needs to know that the immediately preceding
         * unary minus will consume 2^63.
         */
        if (lex_peek(T_numeric, literal)) {
            read_numeric_param(parent, bb, true);
            value = opstack_pop();
            force_wide_global_literal_type(value, literal);
            return value;
        }
        value = read_wide_global_literal_primary(parent, bb, scope);
        zero->var_name = gen_name();
        zero->type = value->type;
        zero->init_val = 0;
        add_insn(parent, bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        result = require_var(parent);
        result->var_name = gen_name();
        result->type = value->type;
        emit_wide_global_word_arithmetic(parent, bb, result, OP_sub, zero,
                                         value);
        return result;
    }
    if (lex_accept(T_bit_not)) {
        var_t *result;

        value = read_wide_global_literal_primary(parent, bb, scope);
        result = require_var(parent);
        result->var_name = gen_name();
        result->type = value->type;
        emit_wide_global_word_arithmetic(parent, bb, result, OP_bit_not, value,
                                         NULL);
        return result;
    }
    if (lex_accept(T_log_not)) {
        var_t *result;

        value = read_wide_global_literal_primary(parent, bb, scope);
        result = require_typed_var(parent, TY_int);
        result->var_name = gen_name();
        result->init_val = !(value->init_val || value->init_val_hi);
        result->init_val_hi = 0;
        result->is_const = true;
        if (!wide_global_unevaluated_depth)
            add_insn(parent, bb, OP_log_not, result, value, NULL, 0, NULL);
        return result;
    }
    if (lex_accept(T_sizeof)) {
        char sizeof_name[MAX_ID_LEN];

        value = require_typed_var(parent, find_type("size_t", true));
        value->var_name = gen_name();
        if (lex_peek(T_string, NULL))
            value->init_val = read_const_string_size();
        else if (lex_peek(T_wstring, NULL))
            value->init_val = read_const_wstring_size();
        else if (lex_peek(T_numeric, NULL))
            value->init_val = read_global_sizeof_expression(scope);
        else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                 cur_token->next->next &&
                 cur_token->next->next->kind == T_string) {
            lex_expect(T_open_bracket);
            value->init_val = read_const_string_size();
            lex_expect(T_close_bracket);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_wstring) {
            lex_expect(T_open_bracket);
            value->init_val = read_const_wstring_size();
            lex_expect(T_close_bracket);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_numeric) {
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_open_bracket) {
            /* A nested group is an expression operand, e.g. sizeof((void)1) or
             * sizeof((*incomplete_pointer)).
             */
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_asterisk &&
                   cur_token->next->next->next &&
                   cur_token->next->next->next->kind == T_identifier &&
                   cur_token->next->next->next->next &&
                   cur_token->next->next->next->next->kind == T_open_square &&
                   is_direct_fixed_array_pointer_slot(
                       find_var(cur_token->next->next->next->literal, scope))) {
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_identifier &&
                   cur_token->next->next->next &&
                   cur_token->next->next->next->kind == T_close_bracket &&
                   find_var(cur_token->next->next->literal, scope)) {
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_identifier, sizeof_name) &&
                   find_var(sizeof_name, scope)) {
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_open_bracket, NULL) && cur_token->next &&
                   cur_token->next->next &&
                   cur_token->next->next->kind == T_identifier &&
                   cur_token->next->next->next &&
                   cur_token->next->next->next->kind == T_close_bracket &&
                   find_func(cur_token->next->next->literal)) {
            value->init_val = read_global_sizeof_expression(scope);
        } else if (lex_peek(T_identifier, sizeof_name) &&
                   find_func(sizeof_name)) {
            value->init_val = read_global_sizeof_expression(scope);
        } else
            value->init_val = read_const_sizeof_type(scope);
        value->init_val_hi = 0;
        value->is_const = true;
        add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        return value;
    }
    if (lex_accept(T_open_bracket)) {
        value = read_wide_global_literal_expression(parent, bb, scope);
        lex_expect(T_close_bracket);
        return value;
    }
    if (lex_peek(T_identifier, literal)) {
        constant_t *constant = find_scoped_constant(literal, scope);

        if (!constant)
            error_at("Typed global initializer needs a constant operand",
                     next_token_loc());
        lex_expect(T_identifier);
        value = require_typed_var(parent, TY_int);
        value->var_name = gen_name();
        value->init_val = constant->value;
        value->is_const = true;
        add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        return value;
    }
    if (!lex_peek(T_numeric, literal))
        error_at("Wide global initializer needs a literal operand",
                 next_token_loc());
    read_numeric_param(parent, bb, false);
    value = opstack_pop();
    force_wide_global_literal_type(value, literal);
    return value;
}

/* Parse arithmetic literal-only global expressions, including grouped
 * subexpressions, without sending a high word through the legacy int-only
 * constant evaluator.
 */
var_t *read_wide_global_literal_expression(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope)
{
    opcode_t op_stack[MAX_OPERATOR_STACK_SIZE];
    bool protected_rhs[MAX_OPERATOR_STACK_SIZE] = {0};
    var_t *val_stack[MAX_OPERATOR_STACK_SIZE];
    int op_stack_index = 0, val_stack_index = 0;
    opcode_t op;

    val_stack[val_stack_index++] =
        read_wide_global_literal_primary(parent, bb, scope);
    op = get_operator();
    while (op != OP_generic) {
        if (op == OP_ternary) {
            var_t *condition;
            var_t *when_true;
            var_t *when_false;
            type_t *common;
            bool condition_true;

            /* `?:` has the lowest precedence. Fold every pending binary
             * operator first so the condition is the complete expression, not
             * merely the primary immediately before `?`.
             */
            while (op_stack_index > 0) {
                var_t *right = val_stack[--val_stack_index];
                var_t *left = val_stack[--val_stack_index];
                var_t *result = require_var(parent);

                op_stack_index--;
                if (protected_rhs[op_stack_index])
                    wide_global_unevaluated_depth--;
                result->var_name = gen_name();
                result->type = integer_binary_result_type(
                    op_stack[op_stack_index], left, right);
                if (!emit_wide_global_word_arithmetic(parent, bb, result,
                                                      op_stack[op_stack_index],
                                                      left, right))
                    add_insn(parent, bb, op_stack[op_stack_index], result, left,
                             right, 0, NULL);
                val_stack[val_stack_index++] = result;
            }
            condition = val_stack[--val_stack_index];
            condition_true = condition->init_val || condition->init_val_hi;

            /* A global conditional expression is still an integer constant
             * expression, but both arms undergo the usual arithmetic
             * conversions before the constant condition selects one. Parse both
             * through the wide reader so a discarded high word can never leak
             * through the legacy int-only evaluator.
             */
            lex_expect(T_question);
            if (!condition_true)
                wide_global_unevaluated_depth++;
            when_true = read_wide_global_literal_expression(parent, bb, scope);
            if (!condition_true)
                wide_global_unevaluated_depth--;
            lex_expect(T_colon);
            if (condition_true)
                wide_global_unevaluated_depth++;
            when_false = read_wide_global_literal_expression(parent, bb, scope);
            if (condition_true)
                wide_global_unevaluated_depth--;
            common = integer_common_type(when_true, when_false);
            normalize_integer_binary_operands(parent, &bb, OP_add, &when_true,
                                              &when_false);
            if (when_true->type != common)
                when_true = resize_to(parent, &bb, when_true, common, 0);
            if (when_false->type != common)
                when_false = resize_to(parent, &bb, when_false, common, 0);
            return condition_true ? when_true : when_false;
        }
        while (op_stack_index > 0 &&
               get_operator_prio(op_stack[op_stack_index - 1]) >=
                   get_operator_prio(op)) {
            var_t *right = val_stack[--val_stack_index];
            var_t *left = val_stack[--val_stack_index];
            var_t *result = require_var(parent);

            op_stack_index--;
            if (protected_rhs[op_stack_index])
                wide_global_unevaluated_depth--;
            result->var_name = gen_name();
            result->type = integer_binary_result_type(op_stack[op_stack_index],
                                                      left, right);
            if (!emit_wide_global_word_arithmetic(
                    parent, bb, result, op_stack[op_stack_index], left, right))
                add_insn(parent, bb, op_stack[op_stack_index], result, left,
                         right, 0, NULL);
            val_stack[val_stack_index++] = result;
        }
        if (op_stack_index >= MAX_OPERATOR_STACK_SIZE ||
            val_stack_index >= MAX_OPERATOR_STACK_SIZE)
            fatal("Wide global initializer is too complex");
        protected_rhs[op_stack_index] =
            is_logical(op) &&
            ((op == OP_log_and &&
              !(val_stack[val_stack_index - 1]->init_val ||
                val_stack[val_stack_index - 1]->init_val_hi)) ||
             (op == OP_log_or &&
              (val_stack[val_stack_index - 1]->init_val ||
               val_stack[val_stack_index - 1]->init_val_hi)));
        op_stack[op_stack_index++] = op;
        if (protected_rhs[op_stack_index - 1]) {
            wide_global_unevaluated_depth++;
        }
        val_stack[val_stack_index++] =
            read_wide_global_literal_primary(parent, bb, scope);
        op = get_operator();
    }
    while (op_stack_index > 0) {
        var_t *right = val_stack[--val_stack_index];
        var_t *left = val_stack[--val_stack_index];
        var_t *result = require_var(parent);

        op_stack_index--;
        if (protected_rhs[op_stack_index])
            wide_global_unevaluated_depth--;
        result->var_name = gen_name();
        result->type =
            integer_binary_result_type(op_stack[op_stack_index], left, right);
        if (!emit_wide_global_word_arithmetic(
                parent, bb, result, op_stack[op_stack_index], left, right))
            add_insn(parent, bb, op_stack[op_stack_index], result, left, right,
                     0, NULL);
        val_stack[val_stack_index++] = result;
    }
    return val_stack[0];
}

void eval_ternary_imm(int cond, var_t *var)
{
    if (cond == 0) {
        while (!lex_peek(T_colon, NULL)) {
            lex_next();
        }
        lex_accept(T_colon);
        read_global_assignment_var(var);
    } else {
        read_global_assignment_var(var);
        lex_expect(T_colon);
        while (!lex_peek(T_semicolon, NULL)) {
            lex_next();
        }
    }
}

bool read_global_assignment_var(var_t *var)
{
    var_t *vd, *rs1;

    /* A block-scope static is lowered in the global setup block, but its
     * initializer is parsed in the declaration's lexical scope. In particular
     * an enumerator declared by an enclosing block remains an integer constant
     * expression here.
     */
    block_t *scope = var->scope ? var->scope : GLOBAL_BLOCK;
    block_t *parent = GLOBAL_BLOCK;
    basic_block_t *bb = GLOBAL_FUNC->bbs;

    validate_string_array_initializer(var);
    if ((var->array_size > 0 || var->has_unsized_array) && is_char_array(var) &&
        lex_peek(T_string, NULL)) {
        parse_string_array_init(var, parent, &bb);
        return true;
    }
    if ((var->array_size > 0 || var->has_unsized_array) &&
        is_wchar_array(var) && lex_peek(T_wstring, NULL)) {
        parse_wstring_array_init(var, parent, &bb);
        return true;
    }

    /* global initialization must be constant */
    {
        /* A function designator is a valid address constant. Keep it as the
         * function symbol until lowering: OP_address_of_func has the deferred
         * relocation needed because the target function's code offset is not
         * known while global initializers are parsed.
         */
        bool address_dereference =
            global_function_address_dereference_starts_here();
        token_t *address_dereference_identifier = NULL;
        bool explicit_address;
        bool grouped_function_designator = false;

        if (address_dereference) {
            var_t *addr;
            var_t *symbol;

            address_dereference_identifier =
                consume_global_function_address_dereference();
            if (!find_visible_func(address_dereference_identifier->literal,
                                   scope))
                error_at("Function address requires a visible declaration",
                         cur_token_loc());
            if (!var->is_func && !var->ptr_level &&
                !(var->type && var->type->ptr_level))
                error_at("Function address requires a pointer initializer",
                         cur_token_loc());
            addr = require_ref_var(parent, var->type, var->ptr_level);
            symbol = require_func_symbol_var(parent);
            addr->var_name = gen_name();
            symbol->is_func = true;
            symbol->var_name =
                intern_string(address_dereference_identifier->literal);
            add_insn(parent, bb, OP_address_of, addr, var, NULL, 0, NULL);
            add_insn(parent, bb, OP_write, NULL, addr, symbol, PTR_SIZE, NULL);
            return true;
        }
        explicit_address = lex_accept(T_ampersand);
        if (grouped_global_function_designator_starts_here(false)) {
            lex_expect(T_open_bracket);
            grouped_function_designator = true;
        }
        char token[MAX_ID_LEN];
        if (lex_peek(T_identifier, token)) {
            func_t *func = find_visible_func(token, scope);
            if (func) {
                if (!var->is_func && !var->ptr_level &&
                    !(var->type && var->type->ptr_level))
                    error_at("Function address requires a pointer initializer",
                             cur_token_loc());
                var_t *addr =
                    require_ref_var(parent, var->type, var->ptr_level);
                var_t *symbol = require_func_symbol_var(parent);

                addr->var_name = gen_name();
                symbol->is_func = true;
                symbol->var_name = intern_string(token);
                lex_expect(T_identifier);
                if (grouped_function_designator)
                    lex_expect(T_close_bracket);
                add_insn(parent, bb, OP_address_of, addr, var, NULL, 0, NULL);
                add_insn(parent, bb, OP_write, NULL, addr, symbol, PTR_SIZE,
                         NULL);
                return true;
            }

            /* Static locals have global storage but lexical visibility. Use the
             * declaration scope for name resolution while continuing to emit
             * their initializer into the synthetic global block.
             */
            var_t *object = find_var(token, scope);
            if (object && object->is_global &&
                (explicit_address || object->array_size)) {
                fixed_array_shape_t shape = fixed_array_shape_from_var(object);
                var_t *object_addr =
                    require_ref_var(parent, object->type, object->ptr_level);

                object_addr->var_name = gen_name();
                object_addr->is_global_address = true;

                /* Taking the address of a callback object creates a slot.
                 * Retain the callback prototype on that non-callable outer
                 * pointer so the global initializer conversion below has the
                 * same information as block-scope `&callback`.
                 */
                object_addr->pointee_func_signature =
                    object->pointee_func_signature
                        ? object->pointee_func_signature
                        : object->func_signature;
                lex_expect(T_identifier);
                add_insn(parent, bb, OP_address_of, object_addr, object, NULL,
                         0, NULL);
                if (!explicit_address && object->array_size &&
                    (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL))) {
                    int index = read_global_address_offset(scope, parent, bb);
                    int stride =
                        object->ptr_level ? PTR_SIZE : object->type->size;
                    var_t *byte_offset = require_var(parent);
                    var_t *offset_addr = require_ref_var(parent, object->type,
                                                         object->ptr_level);

                    stride = fixed_array_shape_stride(&shape, 0, stride);
                    byte_offset->var_name = gen_name();
                    byte_offset->init_val = index * stride;
                    add_insn(parent, bb, OP_load_constant, byte_offset, NULL,
                             NULL, 0, NULL);
                    offset_addr->var_name = gen_name();
                    offset_addr->is_global_address = true;
                    add_insn(parent, bb, OP_add, offset_addr, object_addr,
                             byte_offset, 0, NULL);
                    object_addr = offset_addr;
                }
                if (explicit_address)
                    object_addr = read_global_address_designator(
                        scope, parent, &bb, &object, object_addr);
                if (incompatible_pointee_callback_conversion(object_addr, var))
                    error_at("incompatible callback slot types in initializer",
                             cur_token_loc());
                add_insn(parent, bb, OP_assign, var, object_addr, NULL, 0,
                         NULL);
                return true;
            }
        }
        if (explicit_address)
            error_at("Expected a global object or function after '&'",
                     cur_token_loc());

        /* The legacy global evaluator stores operands in int. Parse a wide
         * literal-only expression separately so its upper payload survives;
         * lower each reduction into the global setup block instead of trying to
         * narrow the expression through that evaluator.
         */
        if (typed_global_literal_appears_before_initializer_end(
                cur_token->next)) {
            rs1 = read_wide_global_literal_expression(parent, bb, scope);
            add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
            return true;
        }
        if (lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)) {
            /* String literal global initialization: String literals are now
             * stored in .rodata section. TODO: Implement compile-time address
             * resolution for global pointer initialization with rodata
             * addresses (e.g., char *p = "str";)
             */
            if (lex_peek(T_wstring, NULL))
                read_wstring_param(parent, bb);
            else
                read_literal_param(parent, bb);
            rs1 = opstack_pop();
            vd = var;
            diagnose_const_pointer_conversion(rs1, vd);
            emit_global_scalar_assignment(parent, bb, vd, rs1);
            return true;
        }

        opcode_t op_stack[MAX_OPERATOR_STACK_SIZE];
        opcode_t op, next_op;
        int val_stack[MAX_OPERATOR_STACK_SIZE];
        int op_stack_index = 0, val_stack_index = 0;
        int operand1, operand2;
        operand1 = read_primary_constant(scope);
        op = get_operator();
        /* only one value after assignment */
        if (op == OP_generic) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = operand1;
            add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs1 = vd;
            emit_global_scalar_assignment(parent, bb, var, rs1);
            return true;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(operand1, var);
            return true;
        }
        operand2 = read_primary_constant(scope);
        next_op = get_operator();
        if (next_op == OP_generic) {
            /* only two operands, apply and return */
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = eval_expression_imm(op, operand1, operand2);
            add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            rs1 = vd;
            add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
            return true;
        }

        /* using stack if operands more than two */
        op_stack[op_stack_index++] = op;
        op = next_op;
        val_stack[val_stack_index++] = operand1;
        val_stack[val_stack_index++] = operand2;

        while (op != OP_generic && op != OP_ternary) {
            if (op_stack_index > 0) {
                /* we have a continuation, use stack */
                int same_op = 0;
                do {
                    opcode_t stack_op = op_stack[op_stack_index - 1];
                    if (get_operator_prio(stack_op) >= get_operator_prio(op)) {
                        operand1 = val_stack[val_stack_index - 2];
                        operand2 = val_stack[val_stack_index - 1];
                        val_stack_index -= 2;

                        /* apply stack operator and push result back */
                        val_stack[val_stack_index++] =
                            eval_expression_imm(stack_op, operand1, operand2);

                        /* pop op stack */
                        op_stack_index--;
                    } else {
                        same_op = 1;
                    }
                    /* continue util next operation is higher prio */
                } while (op_stack_index > 0 && same_op == 0);
            }
            /* push next operand on stack */
            if (val_stack_index >= MAX_OPERATOR_STACK_SIZE ||
                op_stack_index >= MAX_OPERATOR_STACK_SIZE)
                fatal("Constant expression too complex");
            val_stack[val_stack_index++] = read_primary_constant(scope);
            /* push operator on stack */
            op_stack[op_stack_index++] = op;
            op = get_operator();
        }
        /* unwind stack and apply operations */
        while (op_stack_index > 0) {
            opcode_t stack_op = op_stack[op_stack_index - 1];

            /* pop stack and apply operators */
            operand1 = val_stack[val_stack_index - 2];
            operand2 = val_stack[val_stack_index - 1];
            val_stack_index -= 2;

            /* apply stack operator and push value back on stack */
            val_stack[val_stack_index++] =
                eval_expression_imm(stack_op, operand1, operand2);

            if (op_stack_index == 1) {
                if (op == OP_ternary) {
                    lex_expect(T_question);
                    eval_ternary_imm(val_stack[0], var);
                } else {
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->init_val = val_stack[0];
                    add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0,
                             NULL);

                    rs1 = vd;
                    emit_global_scalar_assignment(parent, bb, var, rs1);
                }
                return true;
            }

            /* pop op stack */
            op_stack_index--;
        }
        if (op == OP_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(val_stack[0], var);
        } else {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = val_stack[0];
            add_insn(parent, GLOBAL_FUNC->bbs, OP_load_constant, vd, NULL, NULL,
                     0, NULL);

            rs1 = vd;
            emit_global_scalar_assignment(parent, GLOBAL_FUNC->bbs, var, rs1);
        }
        return true;
    }
    return false;
}

bool read_global_assignment(char *token)
{
    var_t *var = find_global_var(token);
    return var && read_global_assignment_var(var);
}

void perform_side_effect(block_t *parent, basic_block_t *bb)
{
    for (int i = 0; i < se_idx; i++) {
        insn_t *insn = &side_effect[i];
        add_insn(parent, bb, insn->opcode, insn->rd, insn->rs1, insn->rs2,
                 insn->sz, insn->str);
    }
    se_idx = 0;
}

basic_block_t *read_code_block(func_t *func,
                               block_t *parent,
                               basic_block_t *bb);

/* A switch dispatch is built while its labeled statements are read. Keep the
 * deferred no-match edge separate from the source-order body chain: labels may
 * occur after ordinary statements or inside nested compound statements.
 */
typedef struct switch_label_context {
    block_t *dispatch_parent;
    var_t *control;
    basic_block_t *dispatch_tail;
    basic_block_t *default_body;
    switch_case_value_t *seen_cases;
    bool has_default;
} switch_label_context_t;

switch_label_context_t switch_label_contexts[MAX_NESTING];
int switch_label_context_idx = 0;

void reject_label_followed_by_declaration_in_strict_c99(void)
{
    char name[MAX_ID_LEN];

    if (!strict_c99)
        return;

    if (lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
        lex_peek(T_restrict, NULL) || lex_peek(T_static, NULL) ||
        lex_peek(T_extern, NULL) || lex_peek(T_register, NULL) ||
        lex_peek(T_auto, NULL) || lex_peek(T_typedef, NULL) ||
        lex_peek(T_inline, NULL) || lex_peek(T_signed, NULL) ||
        lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL) ||
        lex_peek(T_struct, NULL) || lex_peek(T_union, NULL) ||
        lex_peek(T_enum, NULL) ||
        (lex_peek(T_identifier, name) && find_type(name, true)))
        error_at("a C99 label must precede a statement, not a declaration",
                 cur_token_loc());
}

basic_block_t *read_switch_label_statement(block_t *parent, basic_block_t *body)
{
    switch_label_context_t *context;
    basic_block_t *label_body;

    if (!switch_label_context_idx)
        error_at("case or default label outside switch", next_token_loc());
    context = &switch_label_contexts[switch_label_context_idx - 1];
    label_body = bb_create(parent);
    if (body)
        bb_connect(body, label_body, NEXT);

    if (lex_accept(T_default)) {
        if (context->has_default)
            error_at("duplicate default label in switch", cur_token_loc());
        context->has_default = true;
        context->default_body = label_body;
    } else {
        int case_val;
        var_t *constant;
        var_t *comparison;
        basic_block_t *next_dispatch;

        lex_expect(T_case);
        case_val = read_const_expr(parent);
        if (strict_c99) {
            switch_case_value_t *seen;

            for (seen = context->seen_cases; seen; seen = seen->next)
                if (seen->value == case_val)
                    error_at("duplicate case value in C99 switch",
                             cur_token_loc());
            seen = arena_alloc(GENERAL_ARENA, sizeof(*seen));
            seen->value = case_val;
            seen->next = context->seen_cases;
            context->seen_cases = seen;
        }

        constant = require_var(context->dispatch_parent);
        constant->var_name = gen_name();
        constant->init_val = case_val;
        add_insn(context->dispatch_parent, context->dispatch_tail,
                 OP_load_constant, constant, NULL, NULL, 0, NULL);

        comparison = require_var(context->dispatch_parent);
        comparison->var_name = gen_name();
        add_insn(context->dispatch_parent, context->dispatch_tail, OP_eq,
                 comparison, constant, context->control, 0, NULL);
        add_insn(context->dispatch_parent, context->dispatch_tail, OP_branch,
                 NULL, comparison, NULL, 0, NULL);
        bb_connect(context->dispatch_tail, label_body, THEN);
        next_dispatch = bb_create(context->dispatch_parent);
        bb_connect(context->dispatch_tail, next_dispatch, ELSE);
        context->dispatch_tail = next_dispatch;
    }
    lex_expect(T_colon);
    reject_label_followed_by_declaration_in_strict_c99();
    return label_body;
}

/* A switch, its cases, and the block they break out of. */
basic_block_t *handle_switch_statement(block_t *parent, basic_block_t *bb)
{
    basic_block_t *n = bb_create(parent);
    basic_block_t *body;
    basic_block_t *switch_end;
    switch_label_context_t *context;

    bb_connect(bb, n, NEXT);
    bb = n;

    lex_expect(T_open_bracket);
    read_control_expression(parent, &bb);
    lex_expect(T_close_bracket);
    var_t *control = operand_stack[operand_stack_idx - 1];

    /* C99 6.8.4.2 requires an integer controlling expression. After the
     * ordinary expression reader produces the operand, enforce that constraint
     * and apply integer promotions before deferred dispatch captures it.
     */
    if (!control->type || control->type == TY_void ||
        is_pointer_like_value(control) || control->is_func ||
        is_record_type(control->type))
        error_at("switch controlling expression must have integer type",
                 cur_token_loc());
    control = integer_promote_operand(parent, &bb, control);

    /* create exit jump for breaks */
    switch_end = bb_create(parent);
    break_bb_push(switch_end);
    if (switch_label_context_idx >= MAX_NESTING)
        fatal("Too many nested switch statements");
    context = &switch_label_contexts[switch_label_context_idx++];
    context->dispatch_parent = parent;
    context->control = control;
    context->dispatch_tail = bb;
    context->default_body = NULL;
    context->seen_cases = NULL;
    context->has_default = false;

    /* Statements before the first label are legal but are not an entry point of
     * the switch. A following label supplies the source-order fallthrough edge
     * without making those statements reachable from dispatch.
     */
    body = bb_create(parent);

    lex_expect(T_open_curly);
    while (!lex_accept(T_close_curly)) {
        body = read_body_statement(parent, body);
        perform_side_effect(parent, body);
    }

    /* Complete the deferred no-match dispatch after every case comparison is
     * known. This also supplies the natural exit edge for an empty switch.
     */
    bb_connect(context->dispatch_tail,
               context->has_default ? context->default_body : switch_end, NEXT);

    if (body)
        /* if the last label has no explicit break, connect it to the end */
        bb_connect(body, switch_end, NEXT);

    break_exit_idx--;
    switch_label_context_idx--;
    /* remove the expression in switch() */
    opstack_pop();

    int dangling = 1;
    for (int i = 0; i < switch_end->prev_idx; i++)
        if (switch_end->prev[i].bb)
            dangling = 0;

    if (dangling)
        return NULL;

    return switch_end;
}

/* A for loop: setup, condition, body and increment. */
basic_block_t *handle_for_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    type_t *type;
    var_t *vd;
    var_t *rs1;
    var_t *var;
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_register = false;
    bool is_auto = false;
    bool is_volatile = false;
    bool saw_decl_specifier = false;
    bool for_decl_semicolon_consumed = false;

    lex_expect(T_open_bracket);

    /* synthesize for loop block */
    block_t *blk = add_block(parent, parent->func);

    /* setup - execute once */
    basic_block_t *setup = bb_create(blk);
    bb_connect(bb, setup, NEXT);

    if (lex_peek(T_typedef, NULL)) {
        if (strict_c99)
            error_at("C99 for initializer cannot declare a typedef",
                     next_token_loc());

        /* The default-mode typedef extension owns the loop's synthetic block
         * scope. The typedef parser consumes its terminating semicolon and
         * emits no setup IR.
         */
        handle_block_typedef_statement(blk, setup);
        for_decl_semicolon_consumed = true;
    } else if (!lex_accept(T_semicolon)) {
        while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
               lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
               lex_peek(T_register, NULL) || lex_peek(T_auto, NULL)) {
            saw_decl_specifier = true;
            if (lex_accept(T_static)) {
                if (is_static)
                    error_at("duplicate static storage class specifier",
                             cur_token_loc());
                is_static = true;
            } else if (lex_accept(T_extern)) {
                if (is_extern)
                    error_at("duplicate extern storage class specifier",
                             cur_token_loc());
                is_extern = true;
            } else if (lex_accept(T_register)) {
                if (is_register)
                    error_at("duplicate register storage class specifier",
                             cur_token_loc());
                is_register = true;
            } else if (lex_accept(T_auto)) {
                if (is_auto)
                    error_at("duplicate auto storage class specifier",
                             cur_token_loc());
                is_auto = true;
            } else if (lex_accept(T_volatile)) {
                is_volatile = true;
            } else {
                lex_expect(T_const);
                is_const = true;
            }
        }
        if ((is_static && (is_register || is_extern || is_auto)) ||
            (is_register && (is_extern || is_auto)) || (is_extern && is_auto))
            error_at("incompatible storage class specifiers", cur_token_loc());

        bool has_builtin_type = lex_peek(T_signed, NULL) ||
                                lex_peek(T_unsigned, NULL) ||
                                lex_peek(T_long, NULL);
        bool has_identifier = lex_peek(T_identifier, token);
        bool has_record_type = lex_peek(T_struct, NULL) ||
                               lex_peek(T_union, NULL) ||
                               lex_peek(T_enum, NULL);
        bool has_enum_type = lex_peek(T_enum, NULL);
        bool has_tagged_record =
            lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
        if (has_enum_type) {
            /* read_full_var_decl() owns consuming and resolving `enum TAG`. Use
             * a scalar placeholder only to select its declaration path.
             */
            type = TY_int;
        } else if (has_tagged_record) {
            /* read_full_var_decl() owns consuming and resolving the tag. */
            type = TY_int;
        } else {
            type = has_builtin_type                    ? TY_int
                   : has_identifier || has_record_type ? find_type(token, 1)
                                                       : NULL;
        }
        if (!type && saw_decl_specifier)
            error_at("declaration specifier requires a type", cur_token_loc());
        if (type) {
            /* C99 6.8.5.3 admits only automatic or register object declarations
             * here. The default mode retains its historical block-scope
             * static/extern extension.
             */
            if (strict_c99 && (is_static || is_extern))
                error_at(
                    "C99 for initializer permits only auto or register objects",
                    cur_token_loc());
            var = require_typed_var(blk, type);
            var->is_static = is_static;
            var->is_register = is_register;
            var->is_global = is_static;
            var->is_const_qualified = is_const;
            var->is_volatile = is_volatile;
            parsing_for_initializer_declaration = true;
            read_full_var_decl(var, false, false, false);
            parsing_for_initializer_declaration = false;
            if (strict_c99 && var->is_func)
                error_at("C99 for initializer cannot declare a function",
                         cur_token_loc());
            if (is_extern) {
                if (var->is_func || lex_peek(T_open_bracket, NULL)) {
                    /* This helper consumes the declaration's semicolon. */
                    blk->locals.size--;
                    var->is_block_scope_function_declaration = true;
                    GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] =
                        var;
                    read_global_function_declarator(GLOBAL_BLOCK, var, false);
                    var_t *alias = require_var(blk);
                    alias->var_name = var->var_name;
                    alias->is_extern_function_alias = true;
                    for_decl_semicolon_consumed = true;
                } else {
                    for (;;) {
                        var = bind_block_extern_object(blk, var);
                        if (lex_peek(T_assign, NULL))
                            error_at(
                                "extern declaration cannot have an initializer",
                                next_token_loc());
                        if (!lex_accept(T_comma))
                            break;
                        var = require_typed_var(blk, type);
                        var->is_const_qualified = is_const;
                        var->is_volatile = is_volatile;
                        read_partial_var_decl(var, NULL);
                    }
                }
            } else {
                if (is_incomplete_record_object(var))
                    error_at(
                        "Incomplete struct/union type cannot define an object",
                        cur_token_loc());
                add_insn(is_static ? GLOBAL_BLOCK : blk,
                         is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat, var,
                         NULL, NULL, 0, NULL);
                add_symbol(setup, var);
                if (lex_accept(T_assign)) {
                    validate_string_array_initializer(var);
                    if (is_static) {
                        if (lex_peek(T_open_curly, NULL) &&
                            (var->array_size > 0 || var->has_unsized_array ||
                             var->ptr_level > 0))
                            parse_array_init(var, GLOBAL_BLOCK,
                                             &GLOBAL_FUNC->bbs, true);
                        else if (lex_peek(T_open_curly, NULL))
                            parse_global_record_init(var, GLOBAL_BLOCK);
                        else
                            read_global_assignment_var(var);
                    } else if ((var->has_unsized_array ||
                                var->array_size > 0) &&
                               is_char_array(var) && lex_peek(T_string, NULL)) {
                        parse_string_array_init(var, blk, &setup);
                    } else if ((var->has_unsized_array ||
                                var->array_size > 0) &&
                               is_wchar_array(var) &&
                               lex_peek(T_wstring, NULL)) {
                        parse_wstring_array_init(var, blk, &setup);
                    } else if (lex_peek(T_open_curly, NULL) &&
                               (var->array_size > 0 || var->has_unsized_array ||
                                var->ptr_level > 0)) {
                        parse_array_init(var, blk, &setup, true);
                    } else {
                        read_expr(blk, &setup);
                        read_ternary_operation(blk, &setup);

                        rs1 = resize_var(parent, &bb, opstack_pop(), var);
                        add_insn(blk, setup, OP_assign, var, rs1, NULL, 0,
                                 NULL);
                    }
                }
                while (lex_accept(T_comma)) {
                    var_t *nv;

                    /* add sequence point at T_comma */
                    perform_side_effect(blk, setup);

                    /* multiple (partial) declarations */
                    nv = require_typed_var(blk, type);
                    nv->is_static = is_static;
                    nv->is_register = is_register;
                    nv->is_global = is_static;
                    nv->is_const_qualified = is_const;
                    nv->is_volatile = is_volatile;
                    read_partial_var_decl(nv, var); /* partial */
                    if (is_incomplete_record_object(nv))
                        error_at(
                            "Incomplete struct/union type cannot define an "
                            "object",
                            cur_token_loc());
                    add_insn(is_static ? GLOBAL_BLOCK : blk,
                             is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat,
                             nv, NULL, NULL, 0, NULL);
                    add_symbol(setup, nv);
                    if (lex_accept(T_assign)) {
                        validate_string_array_initializer(nv);
                        if (is_static) {
                            if (lex_peek(T_open_curly, NULL) &&
                                (nv->array_size > 0 || nv->has_unsized_array ||
                                 nv->ptr_level > 0))
                                parse_array_init(nv, GLOBAL_BLOCK,
                                                 &GLOBAL_FUNC->bbs, true);
                            else if (lex_peek(T_open_curly, NULL))
                                parse_global_record_init(nv, GLOBAL_BLOCK);
                            else
                                read_global_assignment_var(nv);
                        } else if ((nv->has_unsized_array ||
                                    nv->array_size > 0) &&
                                   is_char_array(nv) &&
                                   lex_peek(T_string, NULL)) {
                            parse_string_array_init(nv, blk, &setup);
                        } else if ((nv->has_unsized_array ||
                                    nv->array_size > 0) &&
                                   is_wchar_array(nv) &&
                                   lex_peek(T_wstring, NULL)) {
                            parse_wstring_array_init(nv, blk, &setup);
                        } else if (lex_peek(T_open_curly, NULL) &&
                                   (nv->array_size > 0 ||
                                    nv->has_unsized_array ||
                                    nv->ptr_level > 0)) {
                            parse_array_init(nv, blk, &setup, true);
                        } else {
                            read_expr(blk, &setup);

                            rs1 = resize_var(parent, &bb, opstack_pop(), nv);
                            add_insn(blk, setup, OP_assign, nv, rs1, NULL, 0,
                                     NULL);
                        }
                    }
                }
            }
        } else {
            read_control_expression(blk, &setup);
            opstack_pop();
            perform_side_effect(blk, setup);
        }

        if (!for_decl_semicolon_consumed)
            lex_expect(T_semicolon);
    }

    basic_block_t *cond_ = bb_create(blk);
    basic_block_t *for_end = bb_create(parent);
    basic_block_t *cond_start = cond_;
    break_bb_push(for_end);
    bb_connect(setup, cond_, NEXT);

    /* condition - check before the loop */
    if (!lex_accept(T_semicolon)) {
        read_control_expression(blk, &cond_);
        lex_expect(T_semicolon);
    } else {
        /* always true */
        vd = require_var(blk);
        vd->init_val = 1;
        vd->var_name = gen_name();
        opstack_push(vd);
        add_insn(blk, cond_, OP_load_constant, vd, NULL, NULL, 0, NULL);
    }
    bb_connect(cond_, for_end, ELSE);

    vd = opstack_pop();
    add_insn(blk, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

    basic_block_t *inc_ = bb_create(blk);
    continue_bb_push(inc_);

    /* increment after each loop */
    if (!lex_accept(T_close_bracket)) {
        read_control_expression(blk, &inc_);
        opstack_pop();
        perform_side_effect(blk, inc_);
        lex_expect(T_close_bracket);
    }

    /* loop body */
    basic_block_t *body_ = bb_create(blk);
    bb_connect(cond_, body_, THEN);
    body_ = read_body_statement(blk, body_);

    /* Normal fallthrough from the loop body goes through the increment block. A
     * continue statement may already have connected another predecessor to
     * inc_.
     */
    if (body_)
        bb_connect(body_, inc_, NEXT);

    /* An empty increment block still needs its back-edge when it is reachable
     * through normal fallthrough or continue.
     *
     * Do not connect a completely unreachable increment block, such as:
     *
     *     for (;;) {
     *         break;
     *     }
     */
    bool has_pred = false;
    for (int i = 0; i < inc_->prev_idx; i++) {
        if (inc_->prev[i].bb) {
            has_pred = true;
            break;
        }
    }
    if (has_pred)
        bb_connect(inc_, cond_start, NEXT);

    /* jump to increment */
    continue_pos_idx--;
    break_exit_idx--;
    return for_end;
}

/* A do-while loop, whose condition is tested after the body. */
basic_block_t *handle_do_statement(block_t *parent, basic_block_t *bb)
{
    var_t *vd;

    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    basic_block_t *cond_ = bb_create(parent);
    basic_block_t *do_while_end = bb_create(parent);

    continue_bb_push(cond_);
    break_bb_push(do_while_end);

    basic_block_t *do_body = read_body_statement(parent, bb);
    if (do_body)
        bb_connect(do_body, cond_, NEXT);

    lex_expect(T_while);
    lex_expect(T_open_bracket);
    read_control_expression(parent, &cond_);
    lex_expect(T_close_bracket);

    vd = opstack_pop();
    add_insn(parent, cond_, OP_branch, NULL, vd, NULL, 0, NULL);

    lex_expect(T_semicolon);

    for (int i = 0; i < cond_->prev_idx; i++) {
        if (cond_->prev[i].bb) {
            bb_connect(cond_, bb, THEN);
            bb_connect(cond_, do_while_end, ELSE);
            break;
        }
        /* if breaking out of loop, skip condition block */
    }

    continue_pos_idx--;
    break_exit_idx--;
    return do_while_end;
}

/* A local struct or union declaration. */
basic_block_t *handle_record_statement(block_t *parent,
                                       basic_block_t *bb,
                                       bool is_const,
                                       bool is_static)
{
    char token[MAX_ID_LEN];
    type_t *type;
    var_t *var;

    bool is_union = false;
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
        is_union = true;
    }
    lex_ident(T_identifier, token);
    type = find_type(token, find_type_flag);
    if (lex_peek(T_open_curly, NULL)) {
        int i = 0;
        int size = 0;
        int alignment = 1;
        int max_size = 0;
        bitfield_layout_t bits = {0};
        bool has_flexible_array_member = false;

        if (!type)
            type = add_type();
        set_type_name(type, token);
        type->base_type = is_union ? TYPE_union : TYPE_struct;

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            var_t *last = v;

            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, parent);
            reject_flexible_array_member_container(v);
            mark_flexible_array_member(v, is_union);
            if (is_union) {
                v->offset = 0;
                int field_size = is_bitfield(v)
                                     ? (v->bit_width ? v->bit_storage_size : 0)
                                     : size_var(v);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(v) > alignment)
                    alignment = alignment_var(v);
            } else {
                size = is_bitfield(v)
                           ? layout_bitfield_field(size, v, &alignment, &bits)
                           : layout_struct_field(
                                 flush_bitfield_layout(size, &bits), v,
                                 &alignment);
            }

            while (lex_accept(T_comma)) {
                if (!is_union && last->is_flexible_array_member)
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                var_t *nv = type_add_field(type, &i);
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, parent);
                reject_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, is_union);
                last = nv;
                if (is_union) {
                    nv->offset = 0;
                    int field_size =
                        is_bitfield(nv)
                            ? (nv->bit_width ? nv->bit_storage_size : 0)
                            : size_var(nv);
                    if (field_size > max_size)
                        max_size = field_size;
                    if (alignment_var(nv) > alignment)
                        alignment = alignment_var(nv);
                } else {
                    size =
                        is_bitfield(nv)
                            ? layout_bitfield_field(size, nv, &alignment, &bits)
                            : layout_struct_field(
                                  flush_bitfield_layout(size, &bits), nv,
                                  &alignment);
                }
            }

            lex_expect(T_semicolon);
            if (!is_union && last->is_flexible_array_member) {
                if (!lex_peek(T_close_curly, NULL))
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                if (i == 1)
                    error_at(
                        "Struct needs a named member before its flexible array "
                        "member",
                        cur_token_loc());
                has_flexible_array_member = true;
            }
        } while (!lex_accept(T_close_curly));

        type->alignment = alignment;
        type->size =
            is_union ? ALIGN_UP(max_size, alignment)
                     : ALIGN_UP(flush_bitfield_layout(size, &bits), alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        if (lex_peek(T_semicolon, NULL)) {
            lex_expect(T_semicolon);
            return bb;
        }
    }
    if (!type && lex_peek(T_semicolon, NULL)) {
        /* A block-scope `struct tag;` or `union tag;` introduces an incomplete
         * tag. Its pointer declarators become valid immediately; the later
         * complete definition reuses this type record.
         */
        type = add_type();
        type->base_type = is_union ? TYPE_union : TYPE_struct;
        set_type_name(type, token);
        lex_expect(T_semicolon);
        return bb;
    }
    if (type) {
        var = require_typed_var(parent, type);
        var->is_const_qualified = is_const;
        var->is_static = is_static;
        var->is_global = is_static;
        read_partial_var_decl(var, NULL);
        if (is_incomplete_record_object(var))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            validate_string_array_initializer(var);
            if (is_static && lex_peek(T_open_curly, NULL) &&
                (var->array_size > 0 || var->has_unsized_array ||
                 var->ptr_level > 0)) {
                parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
            } else if (is_static && lex_peek(T_open_curly, NULL)) {
                parse_global_record_init(var, GLOBAL_BLOCK);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->array_size > 0 || var->has_unsized_array ||
                        var->ptr_level > 0)) {
                parse_array_init(var, parent, &bb, 1); /* Always emit code */
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->type->base_type == TYPE_struct ||
                        var->type->base_type == TYPE_union ||
                        var->type->base_type == TYPE_typedef)) {
                type_t *struct_type = var->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                var_t *struct_addr = require_var(parent);
                struct_addr->var_name = gen_name();
                add_insn(parent, bb, OP_address_of, struct_addr, var, NULL, 0,
                         NULL);
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                        true);
                lex_expect(T_close_curly);
            } else {
                if (!read_assignment_expression(parent, &bb)) {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                }

                var_t *rhs = opstack_pop();
                rhs = scalarize_array_literal_if_needed(
                    parent, &bb, rhs, var->type,
                    !has_effective_pointer(var) && var->array_size == 0);

                emit_object_assignment(parent, &bb, var, rhs);
            }
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_typed_var(parent, type);
            nv->is_static = is_static;
            nv->is_global = is_static;
            nv->is_const_qualified = is_const;
            read_inner_var_decl(nv, false, false, false);
            if (is_incomplete_record_object(nv))
                error_at("Incomplete struct/union type cannot define an object",
                         cur_token_loc());
            add_insn(is_static ? GLOBAL_BLOCK : parent,
                     is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, nv, NULL,
                     NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                validate_string_array_initializer(nv);
                if (is_static && lex_peek(T_open_curly, NULL) &&
                    (nv->array_size > 0 || nv->has_unsized_array ||
                     nv->ptr_level > 0)) {
                    parse_array_init(nv, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
                } else if (is_static && lex_peek(T_open_curly, NULL)) {
                    parse_global_record_init(nv, GLOBAL_BLOCK);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->array_size > 0 || nv->has_unsized_array ||
                            nv->ptr_level > 0)) {
                    parse_array_init(nv, parent, &bb, true);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->type->base_type == TYPE_struct ||
                            nv->type->base_type == TYPE_union ||
                            nv->type->base_type == TYPE_typedef)) {
                    type_t *struct_type = nv->type;
                    if (struct_type->base_type == TYPE_typedef &&
                        struct_type->base_struct)
                        struct_type = struct_type->base_struct;

                    var_t *struct_addr = require_var(parent);
                    struct_addr->var_name = gen_name();
                    add_insn(parent, bb, OP_address_of, struct_addr, nv, NULL,
                             0, NULL);
                    lex_expect(T_open_curly);
                    parse_struct_field_init(parent, &bb, struct_type,
                                            struct_addr, true);
                    lex_expect(T_close_curly);
                } else {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                    var_t *rhs = opstack_pop();
                    rhs = scalarize_array_literal_if_needed(
                        parent, &bb, rhs, nv->type,
                        !has_effective_pointer(nv) && nv->array_size == 0);

                    emit_object_assignment(parent, &bb, nv, rhs);
                }
            }
        }
        lex_expect(T_semicolon);
        return bb;
    }
    error_at("Unknown struct/union type", next_token_loc());
}

basic_block_t *handle_enum_declarators(block_t *parent,
                                       basic_block_t *bb,
                                       type_t *type,
                                       bool is_const,
                                       bool is_static)
{
    for (;;) {
        var_t *var = require_typed_var(parent, type);

        var->is_const_qualified = is_const;
        var->is_static = is_static;
        var->is_global = is_static;
        read_partial_var_decl(var, NULL);
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);

        if (lex_accept(T_assign)) {
            validate_string_array_initializer(var);
            if (is_static) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->has_unsized_array ||
                     var->ptr_level > 0)) {
                    parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs,
                                     true);
                } else {
                    read_global_assignment_var(var);
                }
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_char_array(var) && lex_peek(T_string, NULL)) {
                parse_string_array_init(var, parent, &bb);
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_wchar_array(var) && lex_peek(T_wstring, NULL)) {
                parse_wstring_array_init(var, parent, &bb);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->array_size > 0 || var->has_unsized_array ||
                        var->ptr_level > 0)) {
                parse_array_init(var, parent, &bb, true);
            } else {
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);

                var_t *rhs = opstack_pop();
                rhs = scalarize_array_literal_if_needed(
                    parent, &bb, rhs, var->type,
                    !has_effective_pointer(var) && var->array_size == 0);
                emit_object_assignment(parent, &bb, var, rhs);
            }
        }
        if (is_static)
            discard_global_declarator_operand(var);

        if (!lex_accept(T_comma))
            break;
        perform_side_effect(parent, bb);
    }
    lex_expect(T_semicolon);
    return bb;
}

/* C99 6.7.2.2 constrains every enumerator value to int range, while leaving the
 * compatible integer type of an enum implementation-defined. shecc deliberately
 * selects int for every target, so enum objects, parameters, returns, arrays,
 * and record fields use the ordinary int ABI consistently.
 */
void initialize_enum_type(type_t *type)
{
    type->base_type = TYPE_int;
    type->size = TY_int->size;
}

/* Advance an implicitly numbered enumerator without wrapping past C99's
 * required int domain. Callers invoke this only for an actual implicit value.
 */
int next_enum_value(int value)
{
    if (value == INT_MAX)
        error_at("Enumerator value exceeds int range", cur_token_loc());
    return value + 1;
}

int read_enum_constant(block_t *scope)
{
    bool saved_checking = checking_enum_constant;
    int value;

    if (typed_global_literal_appears_before_initializer_end(cur_token->next)) {
        pp_integer_t typed_value;
        block_t *saved_scope = pp_integer_constant_scope;
        token_t *last;

        pp_integer_constant_scope = scope;
        last = pp_read_constant_infix_expr(0, cur_token, &typed_value, true);
        pp_integer_constant_scope = saved_scope;
        cur_token = last;
        if ((typed_value.is_unsigned &&
             (typed_value.hi || typed_value.lo > 0x7fffffffU)) ||
            (!typed_value.is_unsigned &&
             typed_value.hi != (typed_value.lo & 0x80000000U ? ~0U : 0)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        return typed_value.lo;
    }

    checking_enum_constant = true;
    value = read_const_expr(scope);
    checking_enum_constant = saved_checking;
    return value;
}

/* A block-scope enum definition contributes integer constants to the current
 * expression parser just as a file-scope definition does. Like a record
 * definition, it may introduce declarators after the closing brace.
 */
basic_block_t *handle_enum_statement(block_t *parent,
                                     basic_block_t *bb,
                                     bool is_const,
                                     bool is_static)
{
    char token[MAX_ID_LEN];
    int val = 0;
    type_t *type = NULL;
    bool has_tag = false;

    lex_expect(T_enum);
    if (lex_peek(T_identifier, token)) {
        lex_expect(T_identifier);
        type = find_local_type_tag(token, parent);
        has_tag = true;
    }
    if (!lex_peek(T_open_curly, NULL)) {
        if (!has_tag)
            error_at("Unknown enum type", next_token_loc());
        if (!type)
            type = find_type_tag(token, parent);
        if (!type)
            error_at("Unknown enum type", next_token_loc());
        return handle_enum_declarators(parent, bb, type, is_const, is_static);
    }
    if (!type)
        type = add_type();
    initialize_enum_type(type);
    if (has_tag) {
        set_type_name(type, token);
        if (!find_local_type_tag(token, parent))
            add_type_tag(parent, token, type);
    }
    lex_expect(T_open_curly);
    bool first = true;
    do {
        lex_ident(T_identifier, token);
        if (!first && !lex_peek(T_assign, NULL))
            val = next_enum_value(val);
        if (lex_accept(T_assign)) {
            val = read_enum_constant(parent);
        }
        add_scoped_constant(parent, token, val);
        first = false;
    } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
    lex_expect(T_close_curly);

    if (lex_accept(T_semicolon))
        return bb;
    return handle_enum_declarators(parent, bb, type, is_const, is_static);
}

/* Bind a block-scope extern declaration to the file-scope declaration table.
 *
 * The provisional declarator was added to @parent while its syntax was read. It
 * must not become an automatic object: an extern declaration has no local
 * storage. Move it to GLOBAL_BLOCK so the ordinary file-scope redeclaration
 * checks and eventual definition share one var_t, then leave a scope alias in
 * the current block. The alias matters when the declaration hides an outer
 * automatic object of the same name.
 */
var_t *bind_block_extern_object(block_t *parent, var_t *var)
{
    bool is_redeclaration;

    parent->locals.size--;
    var->is_global = true;
    var->is_static = false;
    GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] = var;
    var =
        resolve_global_declarator(GLOBAL_BLOCK, var, false, &is_redeclaration);
    if (!is_redeclaration)
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                 NULL);

    parent->locals.elements[parent->locals.size++] = var;
    return var;
}

static void reject_ordinary_typedef_collision(block_t *block, var_t *var)
{
    if (find_block_typedef(block, var->var_name))
        error_at("ordinary identifier conflicts with typedef name",
                 cur_token_loc());
}

/* Everything a statement can still be: a declaration, an assignment, a call, or
 * an expression evaluated for its effect.
 */
basic_block_t *handle_declaration(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    func_t *func;
    type_t *type;
    var_t *var;
    opcode_t prefix_op = OP_generic;
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_register = false;
    bool is_auto = false;
    bool is_volatile = false;

    while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
           lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
           lex_peek(T_register, NULL) || lex_peek(T_auto, NULL)) {
        if (lex_accept(T_static)) {
            if (is_static)
                error_at("duplicate static storage class specifier",
                         cur_token_loc());
            is_static = true;
        } else if (lex_accept(T_extern)) {
            if (is_extern)
                error_at("duplicate extern storage class specifier",
                         cur_token_loc());
            is_extern = true;
        } else if (lex_accept(T_register)) {
            if (is_register)
                error_at("duplicate register storage class specifier",
                         cur_token_loc());
            is_register = true;
        } else if (lex_accept(T_auto)) {
            if (is_auto)
                error_at("duplicate auto storage class specifier",
                         cur_token_loc());
            is_auto = true;
        } else if (lex_accept(T_volatile)) {
            is_volatile = true;
        } else {
            lex_expect(T_const);
            is_const = true;
        }
    }
    if ((is_static && (is_register || is_extern || is_auto)) ||
        (is_register && (is_extern || is_auto)) || (is_extern && is_auto))
        error_at("incompatible storage class specifiers", cur_token_loc());

    if (floating_type_starts_here())
        error_at("Floating point types are not yet supported", cur_token_loc());

    if (lex_peek(T_enum, NULL))
        return handle_enum_statement(parent, bb, is_const, is_static);

    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL)) {
        if (is_extern || is_register)
            error_at("Unsupported storage class on record definition",
                     next_token_loc());
        return handle_record_statement(parent, bb, is_const, is_static);
    }

    /* must be an identifier or asterisk (for pointer dereference) */
    bool has_asterisk = lex_peek(T_asterisk, NULL);
    bool has_identifier = lex_peek(T_identifier, token);
    bool has_record_keyword =
        lex_peek(T_struct, NULL) || lex_peek(T_union, NULL);
    bool has_signed_keyword = lex_peek(T_signed, NULL);
    bool has_unsigned_keyword = lex_peek(T_unsigned, NULL);
    bool has_long_keyword = lex_peek(T_long, NULL);
    if (!is_const && !has_identifier && !has_asterisk && !has_record_keyword &&
        !has_signed_keyword && !has_unsigned_keyword && !has_long_keyword)
        error_at("Unexpected token", next_token_loc());

    /* is it a variable declaration? Special handling when statement starts with
     * asterisk
     */
    if (has_asterisk) {
        /* For "*identifier", check if identifier is a type. If not, it's a
         * dereference, not a declaration.
         */
        token_t *saved_token = cur_token;

        /* Skip the asterisk to peek at the identifier */
        lex_accept(T_asterisk);
        char next_ident[MAX_TOKEN_LEN];
        bool could_be_type = false;

        if (lex_peek(T_identifier, next_ident)) {
            /* Check if it's a type name */
            type = find_visible_type(next_ident, parent);
            if (type)
                could_be_type = true;
        }

        /* Restore position */
        cur_token = saved_token;

        /* If it's not a type, skip the declaration block */
        if (!could_be_type)
            type = NULL;
    } else {
        /* Normal type checking without asterisk */
        token_t *type_token = cur_token;
        if (lex_peek(T_signed, NULL) || lex_peek(T_unsigned, NULL) ||
            lex_peek(T_long, NULL)) {
            type = TY_int;
        } else {
            int find_type_flag = lex_accept(T_struct) ? 2 : 1;
            if (find_type_flag == 1 && lex_accept(T_union))
                find_type_flag = 2;
            if (find_type_flag == 2)
                lex_peek(T_identifier, token);
            type = find_type_flag == 2 ? find_type(token, find_type_flag)
                                       : find_visible_type(token, parent);
            if (find_type_flag == 2)
                cur_token = type_token;
        }
    }

    if ((is_static || is_extern) && !type)
        error_at("Expected declaration after storage class specifier",
                 next_token_loc());

    if (type) {
        var = require_typed_var(parent, type);
        var->is_static = is_static;
        var->is_register = is_register;
        var->is_global = is_static;
        var->is_const_qualified = is_const;
        var->is_volatile = is_volatile;
        read_full_var_decl(var, false, false, false);
        reject_ordinary_typedef_collision(parent, var);

        /* A direct function typedef used without a star declares a function,
         * not an automatic object. Bind it through the file-scope function
         * table and leave a lexical function alias so it also hides an outer
         * local object of the same ordinary identifier.
         */
        if (var->is_func && var->type->is_direct_function_type) {
            for (;;) {
                if (is_static || is_register || is_auto)
                    error_at(
                        "invalid storage class for block function declaration",
                        cur_token_loc());
                if (is_const || is_volatile || var->is_const_qualified ||
                    var->is_volatile)
                    error_at("function type cannot be qualified",
                             cur_token_loc());
                parent->locals.size--;
                var->is_block_scope_function_declaration = true;
                GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] =
                    var;
                read_global_function_declarator(GLOBAL_BLOCK, var, false);
                var_t *alias = require_var(parent);

                alias->var_name = var->var_name;
                alias->is_extern_function_alias = true;
                if (!lex_accept(T_comma))
                    return bb;

                var = require_typed_var(parent, type);
                var->is_const_qualified = is_const;
                var->is_volatile = is_volatile;
                var->func_signature = type->func_signature;
                var->is_func = var->func_signature != NULL;
                read_partial_var_decl(var, NULL);
                reject_ordinary_typedef_collision(parent, var);
                if (!(var->is_func && var->type->is_direct_function_type))

                    /* The comma list may continue with an object derived from
                     * the direct-function typedef, such as `unary_t declared,
                     * *callback`. The function declaration above has already
                     * been registered; hand this fully parsed object to the
                     * ordinary declaration lowering below.
                     */
                    break;
            }
        }
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());

        /* Decide after the full declarator has been read: both `const int` and
         * `int const`, and an outer `* const`, make the defined object
         * non-modifiable.
         */
        if (is_static && parent->func && parent->func->is_inline &&
            !parent->func->is_static && !var->is_const_qualified &&
            !var->is_const_pointer)
            error_at("external inline definition cannot define static object",
                     cur_token_loc());
        if (is_extern) {
            if (var->is_func || lex_peek(T_open_bracket, NULL)) {
                /* The global helper owns function redeclaration compatibility
                 * and parameter parsing. Unlike an object declaration it
                 * consumes the trailing semicolon itself.
                 */
                parent->locals.size--;
                var->is_block_scope_function_declaration = true;
                GLOBAL_BLOCK->locals.elements[GLOBAL_BLOCK->locals.size++] =
                    var;
                read_global_function_declarator(GLOBAL_BLOCK, var, false);

                /* Keep a lexical marker so this declaration hides an outer
                 * automatic object of the same name.
                 */
                var_t *alias = require_var(parent);
                alias->var_name = var->var_name;
                alias->is_extern_function_alias = true;
                return bb;
            }
            for (;;) {
                var = bind_block_extern_object(parent, var);
                if (lex_peek(T_assign, NULL))
                    error_at("extern declaration cannot have an initializer",
                             next_token_loc());
                if (!lex_accept(T_comma))
                    break;
                var = require_typed_var(parent, type);
                var->is_const_qualified = is_const;
                var->is_volatile = is_volatile;
                read_partial_var_decl(var, NULL);
            }
            lex_expect(T_semicolon);
            return bb;
        }
        if (is_incomplete_record_object(var))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            validate_string_array_initializer(var);
            if (is_static) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->has_unsized_array ||
                     var->ptr_level > 0)) {
                    /* A block-scope static has global storage duration, so its
                     * brace initializer belongs to the same constant-data
                     * lowering as a file-scope array.
                     */
                    parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs,
                                     true);
                } else if (global_compound_literal_starts_here() &&
                           !(var->ptr_level || var->type->ptr_level) &&
                           is_record_type(var->type)) {
                    parse_global_compound_record_init(var, GLOBAL_BLOCK);
                } else if (global_compound_literal_starts_here() &&
                           (var->ptr_level || var->type->ptr_level)) {
                    parse_global_compound_array_init(var, GLOBAL_BLOCK);
                } else if (global_compound_literal_starts_here()) {
                    parse_global_compound_scalar_init(var, GLOBAL_BLOCK);
                } else if (lex_peek(T_open_curly, NULL)) {
                    parse_global_record_init(var, GLOBAL_BLOCK);
                } else {
                    read_global_assignment_var(var);
                }
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_char_array(var) && lex_peek(T_string, NULL)) {
                parse_string_array_init(var, parent, &bb);
            } else if ((var->has_unsized_array || var->array_size > 0) &&
                       is_wchar_array(var) && lex_peek(T_wstring, NULL)) {
                parse_wstring_array_init(var, parent, &bb);
            } else if (lex_peek(T_open_curly, NULL) &&
                       (var->array_size > 0 || var->has_unsized_array ||
                        var->ptr_level > 0)) {
                /* Emit code for locals in functions */
                parse_array_init(var, parent, &bb, 1);
            } else if (lex_peek(T_open_curly, NULL) &&
                       is_record_type(var->type)) {
                type_t *struct_type = var->type;
                if (struct_type->base_type == TYPE_typedef &&
                    struct_type->base_struct)
                    struct_type = struct_type->base_struct;

                var_t *struct_addr = require_var(parent);
                struct_addr->var_name = gen_name();
                add_insn(parent, bb, OP_address_of, struct_addr, var, NULL, 0,
                         NULL);
                lex_expect(T_open_curly);
                parse_struct_field_init(parent, &bb, struct_type, struct_addr,
                                        true);
                lex_expect(T_close_curly);
            } else {
                if (!read_assignment_expression(parent, &bb)) {
                    read_expr(parent, &bb);
                    read_ternary_operation(parent, &bb);
                }

                var_t *expr_result = opstack_pop();

                /* Keep direct function-pointer initializers on their relocation
                 * path, but every other initializer consumes a function
                 * designator as its converted pointer value.
                 */
                if (!var->is_func)
                    expr_result = materialize_function_designator(parent, &bb,
                                                                  expr_result);

                if (strict_c99 && is_array_literal_placeholder(expr_result) &&
                    !has_effective_pointer(var) && var->array_size == 0)
                    error_at(
                        "array compound literal cannot be used as a scalar in "
                        "C99",
                        cur_token_loc());

                /* Handle array compound literal to scalar assignment */
                if (expr_result && expr_result->array_size > 0 &&
                    !var->ptr_level && var->array_size == 0 && var->type &&
                    (var->type->base_type == TYPE_int ||
                     var->type->base_type == TYPE_short) &&
                    expr_result->var_name[0] == '.') {
                    /* Extract first element from compound literal array */
                    var_t *first_elem = require_var(parent);
                    first_elem->type = var->type;
                    first_elem->var_name = gen_name();

                    /* Read first element from array at offset 0 expr_result is
                     * the array itself, so we can read directly from it
                     */
                    add_insn(parent, bb, OP_read, first_elem, expr_result, NULL,
                             var->type->size, NULL);
                    expr_result = first_elem;
                }

                if (incompatible_pointee_callback_conversion(expr_result, var))
                    error_at("incompatible callback slot types in initializer",
                             cur_token_loc());
                diagnose_const_pointer_conversion(expr_result, var);
                emit_object_assignment(parent, &bb, var, expr_result);
            }
        }
        if (is_static)
            discard_global_declarator_operand(var);
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_typed_var(parent, type);
            nv->is_static = is_static;
            nv->is_register = is_register;
            nv->is_global = is_static;
            nv->is_const_qualified = var->is_const_qualified;
            nv->is_volatile = var->is_volatile;
            read_partial_var_decl(nv, var); /* partial */
            reject_ordinary_typedef_collision(parent, nv);
            if (is_incomplete_record_object(nv))
                error_at("Incomplete struct/union type cannot define an object",
                         cur_token_loc());
            add_insn(is_static ? GLOBAL_BLOCK : parent,
                     is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, nv, NULL,
                     NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                validate_string_array_initializer(nv);
                if (is_static) {
                    if (lex_peek(T_open_curly, NULL) &&
                        (nv->array_size > 0 || nv->has_unsized_array ||
                         nv->ptr_level > 0)) {
                        parse_array_init(nv, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs,
                                         true);
                    } else if (global_compound_literal_starts_here() &&
                               !(nv->ptr_level || nv->type->ptr_level) &&
                               is_record_type(nv->type)) {
                        parse_global_compound_record_init(nv, GLOBAL_BLOCK);
                    } else if (global_compound_literal_starts_here() &&
                               (nv->ptr_level || nv->type->ptr_level)) {
                        parse_global_compound_array_init(nv, GLOBAL_BLOCK);
                    } else if (global_compound_literal_starts_here()) {
                        parse_global_compound_scalar_init(nv, GLOBAL_BLOCK);
                    } else if (lex_peek(T_open_curly, NULL)) {
                        parse_global_record_init(nv, GLOBAL_BLOCK);
                    } else {
                        read_global_assignment_var(nv);
                    }
                } else if ((nv->has_unsized_array || nv->array_size > 0) &&
                           is_char_array(nv) && lex_peek(T_string, NULL)) {
                    parse_string_array_init(nv, parent, &bb);
                } else if ((nv->has_unsized_array || nv->array_size > 0) &&
                           is_wchar_array(nv) && lex_peek(T_wstring, NULL)) {
                    parse_wstring_array_init(nv, parent, &bb);
                } else if (lex_peek(T_open_curly, NULL) &&
                           (nv->array_size > 0 || nv->has_unsized_array ||
                            nv->ptr_level > 0)) {
                    /* Emit code for locals */
                    parse_array_init(nv, parent, &bb, 1);
                } else if (lex_peek(T_open_curly, NULL) &&
                           is_record_type(nv->type)) {
                    type_t *struct_type = nv->type;
                    if (struct_type->base_type == TYPE_typedef &&
                        struct_type->base_struct)
                        struct_type = struct_type->base_struct;

                    var_t *struct_addr = require_var(parent);
                    struct_addr->var_name = gen_name();
                    add_insn(parent, bb, OP_address_of, struct_addr, nv, NULL,
                             0, NULL);
                    lex_expect(T_open_curly);
                    parse_struct_field_init(parent, &bb, struct_type,
                                            struct_addr, true);
                    lex_expect(T_close_curly);
                } else {
                    if (!read_assignment_expression(parent, &bb)) {
                        read_expr(parent, &bb);
                        read_ternary_operation(parent, &bb);
                    }

                    emit_object_assignment(parent, &bb, nv, opstack_pop());
                }
            }
            if (is_static)
                discard_global_declarator_operand(nv);
        }
        lex_expect(T_semicolon);
        return bb;
    }

    /* Keep the long-standing direct-call lowering only for a truly standalone
     * `function(...);` statement. An identifier-led call that is followed by an
     * operator or comma belongs to the full expression grammar below. The
     * self-hosted compiler still exercises this short path heavily, while the
     * bounded lookahead prevents it from stealing valid C99 expressions such as
     * `first(), second();` or `function() + 1;`.
     */
    var_t *local = find_local_var(token, parent);
    if (!has_asterisk && (!local || local->is_extern_function_alias)) {
        token_t *call = cur_token->next;
        token_t *end = call && call->next ? call->next : NULL;
        int depth = 0;
        bool matched_call = false;

        if (end && end->kind == T_open_bracket) {
            for (; end; end = end->next) {
                if (end->kind == T_open_bracket)
                    depth++;
                else if (end->kind == T_close_bracket && !--depth) {
                    end = end->next;
                    matched_call = true;
                    break;
                }
            }
        }
        if (matched_call && end && end->kind == T_semicolon && call) {
            func = find_visible_func(token, parent);
            if (func) {
                lex_expect(T_identifier);
                emit_direct_call_result(func, false, parent, &bb);
                perform_side_effect(parent, bb);
                lex_expect(T_semicolon);
                return bb;
            }
        }
    }

    /* A declaration has already returned above. Resolve a label before routing
     * every remaining non-declaration statement through the full C99 expression
     * grammar.
     */
    if (lex_peek(T_identifier, token) && cur_token->next->next &&
        cur_token->next->next->kind == T_colon) {
        lex_accept(T_identifier);
        token_t *id_tk = cur_token;

        lex_expect(T_colon);
        const label_t *l = find_label(token);
        if (l)
            error_at("label redefinition", &id_tk->location);
        reject_label_followed_by_declaration_in_strict_c99();
        basic_block_t *n = bb_create(parent);
        bb_connect(bb, n, NEXT);
        add_label(token, n);
        add_insn(parent, n, OP_label, NULL, NULL, NULL, 0, token);
        return n;
    }
    return read_full_expression_statement(parent, bb);

    /* handle pointer dereference expressions like *ptr = value */
    if (lex_peek(T_asterisk, NULL)) {
        if (stmt_starts_assignment()) {
            /* Consume exactly one asterisk and evaluate what follows as an
             * ordinary expression. That expression is the address to store to:
             * for "*p" it is p, for "**pp" it is the value of *pp, and for "*(p
             * + 1)" it is p + 1. Letting read_expr() consume the leading
             * asterisk too would dereference once more than the assignment asks
             * for, and the store then went to whatever the pointee happened to
             * hold.
             */
            lex_expect(T_asterisk);
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
            var_t *addr = opstack_pop();

            int addr_depth = effective_pointer_depth(addr);
            unsigned int addr_mask = effective_pointer_const_mask(addr);
            if (addr->is_const_qualified ||
                (addr_depth > 1 && addr_depth <= 32 &&
                 (addr_mask & (1U << (addr_depth - 2)))))
                error_at("assignment of read-only location", next_token_loc());

            /* The width of the store is the pointee's, not the address's. */
            int store_sz = get_pointer_element_size(addr);

            opcode_t compound_op = OP_generic;
            if (!lex_accept(T_assign) &&
                !accept_compound_assign_op(&compound_op))
                error_at("Expected assignment after pointer dereference",
                         next_token_loc());

            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
            var_t *rvalue = opstack_pop();

            if (compound_op != OP_generic) {
                /* "*p op= v" reads the pointee, combines, and writes back. */
                var_t *cur = require_var(parent);
                cur->var_name = gen_name();
                add_insn(parent, bb, OP_read, cur, addr, NULL, store_sz, NULL);

                var_t *combined = require_var(parent);
                combined->var_name = gen_name();
                add_insn(parent, bb, compound_op, combined, cur, rvalue, 0,
                         NULL);
                rvalue = combined;
            }

            add_insn(parent, bb, OP_write, NULL, addr, rvalue, store_sz, NULL);
        } else {
            /* Not a store: an ordinary expression statement. */
            read_expr(parent, &bb);
            read_ternary_operation(parent, &bb);
            perform_side_effect(parent, bb);
        }
        lex_expect(T_semicolon);
        return bb;
    }

    /* is an assignment? */
    if (read_body_assignment(token, parent, prefix_op, &bb, NULL, 0)) {
        perform_side_effect(parent, bb);
        while (lex_accept(T_comma)) {
            prefix_op = OP_generic;
            lex_peek(T_identifier, token);
            if (!read_body_assignment(token, parent, prefix_op, &bb, NULL, 0))
                error_at("Expected assignment after comma", next_token_loc());
            perform_side_effect(parent, bb);
        }
        lex_expect(T_semicolon);
        return bb;
    }

    if (lex_peek(T_identifier, token)) {
        lex_accept(T_identifier);
        token_t *id_tk = cur_token;
        if (lex_accept(T_colon)) {
            const label_t *l = find_label(token);
            if (l)
                error_at("label redefinition", &id_tk->location);

            basic_block_t *n = bb_create(parent);
            bb_connect(bb, n, NEXT);
            add_label(token, n);
            add_insn(parent, n, OP_label, NULL, NULL, NULL, 0, token);
            return n;
        }
    }

    error_at("Unrecognized statement token", next_token_loc());
    return NULL;
}

/* Lexical typedef aliases are declaration-only bindings on the current block,
 * never ordinary objects or global type names. This bounded parser admits
 * selected direct-function, callback-pointer, and fixed callback-array forms;
 * remaining derived declarations await the shared declarator path.
 */
basic_block_t *handle_block_typedef_statement(block_t *parent,
                                              basic_block_t *bb)
{
    var_t decl = {0};

    lex_expect(T_typedef);
    decl.scope = parent;
    parsing_block_typedef_declarator = true;
    read_full_var_decl(&decl, false, false, false);
    parsing_block_typedef_declarator = false;
    do {
        type_t *base = decl.type;
        type_t *alias = add_type();
        func_t *direct_function_signature = decl.func_signature;
        func_t *callback_signature = decl.func_signature;
        func_t *callback_slot_signature = decl.pointee_func_signature;
        bool direct_array = decl.has_direct_array_declarator;
        bool direct_pointee_array = decl.has_direct_pointee_array_declarator;
        bool inherited_pointee_array = base->pointee_array_size != 0;
        bool direct_function_alias =
            decl.is_direct_function_declarator && decl.is_func &&
            decl.func_signature && !decl.ptr_level && !decl.array_size &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !direct_function_signature->va_args &&
            !direct_function_signature->returns_aggregate;
        bool callback_pointer_alias =
            decl.is_func && decl.func_signature &&
            decl.parenthesized_function_pointer_level == 1 && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !callback_signature->va_args &&
            !callback_signature->returns_aggregate;
        bool callback_pointer_realias =
            decl.is_func && decl.func_signature &&
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size && !base->ptr_level &&
            base->func_signature && !base->is_direct_function_type;

        /* `int (**slot_t)(int)` is a pointer-to-callback object, not a callable
         * callback pointer. The shared declarator parser has already normalized
         * the extra star into decl.ptr_level and retained the prototype as
         * pointee metadata. Keep this first exact typedef form equally narrow:
         * scalar/void, no arrays or qualifiers, and no deeper pointer
         * composition.
         */
        bool callback_slot_alias =
            callback_slot_signature &&
            decl.parenthesized_function_pointer_level == 2 &&
            decl.ptr_level == 1 && !decl.array_size &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !decl.is_const_qualified &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            !callback_slot_signature->va_args &&
            !callback_slot_signature->returns_aggregate;
        bool callback_slot_realias =
            decl.pointee_func_signature &&
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.array_size && !decl.pointee_array_size &&
            base->ptr_level == 1 && base->pointee_func_signature &&
            !decl.is_const_qualified &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            !decl.parenthesized_function_pointer_restrict;
        bool callback_slot_array_alias =
            callback_slot_signature &&
            decl.parenthesized_function_pointer_level == 2 &&
            decl.ptr_level == 1 && decl.array_size > 0 &&
            !decl.has_unsized_array && !decl.pointee_array_size &&
            !base->ptr_level && !base->array_size && !is_record_type(base) &&
            !base->is_floating && !decl.is_const_qualified &&
            (!decl.is_const_pointer ||
             decl.parenthesized_function_pointer_outer_const) &&
            (!decl.pointer_const_mask ||
             (decl.parenthesized_function_pointer_outer_const &&
              decl.pointer_const_mask == 1U)) &&
            (!decl.is_volatile ||
             decl.parenthesized_function_pointer_outer_volatile) &&
            !decl.parenthesized_function_pointer_inner_qualified &&
            (!decl.parenthesized_function_pointer_restrict ||
             decl.parenthesized_function_pointer_outer_restrict) &&
            !callback_slot_signature->va_args &&
            !callback_slot_signature->returns_aggregate;
        bool callback_slot_array_realias =
            !decl.parenthesized_function_pointer_level && !decl.ptr_level &&
            !decl.pointee_array_size && base->array_size > 0 &&
            base->array_element_ptr_level == 1 &&
            base->array_element_pointee_func_signature &&
            !decl.is_const_pointer && !decl.pointer_const_mask &&
            !decl.parenthesized_function_pointer_restrict;
        bool callback_array_alias =
            decl.is_func && decl.func_signature &&
            decl.parenthesized_function_pointer_level == 1 &&
            decl.array_size > 0 && !decl.ptr_level &&
            !decl.pointee_array_size && !base->ptr_level &&
            !is_record_type(base) && !base->is_floating &&
            !callback_signature->va_args &&
            !callback_signature->returns_aggregate;
        bool callback_array_realias =
            decl.is_func && decl.func_signature &&
            !decl.parenthesized_function_pointer_level && !direct_array &&
            !decl.ptr_level && !decl.pointee_array_size &&
            base->array_size > 0 && base->array_element_ptr_level == 1 &&
            base->func_signature && !base->is_direct_function_type;

        if (decl.parenthesized_function_pointer_level > 1 &&
            !callback_slot_alias && !callback_slot_array_alias)
            error_at(
                "deeper block callback-pointer typedef is not yet supported",
                cur_token_loc());
        if (callback_pointer_alias &&
            decl.parenthesized_function_pointer_restrict)
            error_at("restrict requires a pointer to an object type",
                     cur_token_loc());
        if (callback_array_alias &&
            (decl.is_const_pointer || (decl.pointer_const_mask & 1U) ||
             decl.parenthesized_function_pointer_const || decl.is_volatile ||
             decl.parenthesized_function_pointer_restrict))
            error_at(
                "qualified block callback-array typedef is not yet supported",
                cur_token_loc());
        if (direct_array && base->func_signature &&
            !base->is_direct_function_type &&
            (decl.is_const_qualified || decl.is_volatile))
            error_at(
                "qualified block callback-array typedef is not yet supported",
                cur_token_loc());
        if (direct_array && base->array_size &&
            base->array_element_ptr_level == 1 && base->func_signature &&
            !base->is_direct_function_type)
            error_at(
                "derived block callback-array typedef is not yet supported",
                cur_token_loc());

        /* Keep pointer-to-array aliases intentionally narrow until their
         * composition rules share the full declarator path. The supported forms
         * are exactly fixed scalar rows and fixed one-pointer-element rows,
         * plus a plain re-alias of a completed type.
         */
        if (decl.has_unsized_array ||
            ((decl.is_func || decl.func_signature) && !direct_function_alias &&
             !callback_pointer_alias && !callback_pointer_realias &&
             !callback_array_alias && !callback_array_realias &&
             !callback_slot_alias && !callback_slot_realias &&
             !callback_slot_array_alias && !callback_slot_array_realias) ||
            (base->pointee_func_signature && !callback_slot_realias) ||
            (base->array_element_pointee_func_signature &&
             !callback_slot_array_realias) ||
            (direct_array &&
             ((base->ptr_level && !inherited_pointee_array) ||
              (inherited_pointee_array && decl.array_dim2) ||
              (decl.ptr_level && (decl.array_dim2 || decl.ptr_level > 1)))) ||
            (direct_pointee_array &&
             (base->ptr_level || base->array_size ||
              decl.ptr_level != decl.pointee_array_element_ptr_level + 1 ||
              decl.pointee_array_element_ptr_level > 1)) ||
            (inherited_pointee_array &&
             (decl.ptr_level || direct_pointee_array)))
            error_at("block typedef derived declarator is not yet supported",
                     cur_token_loc());
        if (lex_peek(T_assign, NULL))
            error_at("typedef declaration cannot have an initializer",
                     next_token_loc());
        memcpy(alias, base, sizeof(*alias));
        alias->base_type = TYPE_typedef;
        alias->base_struct = is_record_type(base) ? base : base->base_struct;
        alias->ptr_level = base->ptr_level + decl.ptr_level;
        alias->pointer_const_mask =
            base->pointer_const_mask |
            (decl.pointer_const_mask << base->ptr_level);
        alias->is_const_qualified = decl.is_const_qualified;
        if (callback_pointer_realias && decl.is_const_qualified) {
            /* A callback alias itself is a pointer type, even though its
             * compact descriptor stores no ordinary pointer depth.
             */
            alias->pointer_const_mask |= 1U;
            alias->is_const_qualified = false;
        }
        alias->is_volatile_qualified =
            decl.is_volatile || base->is_volatile_qualified;
        if (direct_function_alias) {
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = true;
        }
        if (callback_pointer_alias) {
            alias->size = PTR_SIZE;
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = false;
        }
        if (callback_slot_alias) {
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = NULL;
            alias->pointee_func_signature = callback_slot_signature;
            alias->is_direct_function_type = false;
        }
        if (callback_slot_array_alias) {
            alias->ptr_level = 0;
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = NULL;
            alias->pointee_func_signature = NULL;
            alias->is_direct_function_type = false;
            alias->pointer_const_mask = 0;
            alias->is_volatile_qualified = false;
            compose_block_typedef_array(alias, base, &decl);
            alias->array_element_ptr_level = 1;
            alias->array_element_pointee_func_signature =
                callback_slot_signature;
            alias->array_element_is_const_pointer =
                decl.parenthesized_function_pointer_outer_const;
            alias->array_element_is_volatile =
                decl.parenthesized_function_pointer_outer_volatile;
        }
        if (callback_slot_array_realias && direct_array) {
            /* Wrapping a completed slot-array alias prepends bounds but must
             * keep its element as a callback slot. compose_* records the
             * declaration's zero direct pointer depth, so restore the completed
             * element descriptor after composition.
             */
            compose_block_typedef_array(alias, base, &decl);
            alias->array_element_ptr_level = base->array_element_ptr_level;
            alias->array_element_pointee_func_signature =
                base->array_element_pointee_func_signature;
        }
        if (callback_slot_array_realias) {
            /* Qualifying an array typedef qualifies its element type. Keep that
             * fact on the final callback-slot lvalue rather than making the
             * array object itself const or volatile. `restrict` has no runtime
             * representation, but was validated while parsing.
             */
            alias->is_const_qualified = false;
            alias->is_volatile_qualified = false;
            alias->array_element_is_const_pointer |= decl.is_const_qualified;
            alias->array_element_is_volatile |= decl.is_volatile;
        }
        if (callback_slot_realias && decl.is_const_pointer) {
            /* `slot_t const` qualifies the typedef-hidden outer slot pointer,
             * so do not shift the declaration's compact bit past it.
             */
            alias->pointer_const_mask |= 1U;
            alias->is_const_qualified = false;
        }
        if (callback_array_alias) {
            alias->size = PTR_SIZE;
            alias->alignment = PTR_SIZE;
            alias->func_signature = decl.func_signature;
            alias->is_direct_function_type = false;
        }
        if (direct_array || callback_array_alias) {
            compose_block_typedef_array(alias, base, &decl);
            if (callback_array_alias)
                alias->array_element_ptr_level = 1;
            if (direct_array && inherited_pointee_array) {
                /* A direct array suffix around `int (*row_t)[N]` stores
                 * pointer-to-row elements. Keep the row descriptor on the
                 * completed array and mark the selected element as that pointer
                 * slot so its later load retains the row stride.
                 */
                alias->array_element_ptr_level = base->ptr_level;
                alias->array_element_type =
                    base->pointee_array_element_type
                        ? base->pointee_array_element_type
                        : base;
            }
        }
        if (direct_pointee_array) {
            alias->pointee_array_size = decl.pointee_array_size;
            alias->pointee_array_dim2 = decl.pointee_array_dim2;
            alias->pointee_array_dim3 = decl.pointee_array_dim3;
            alias->pointee_array_dim4 = decl.pointee_array_dim4;
            alias->pointee_array_element_ptr_level =
                decl.pointee_array_element_ptr_level;
            alias->pointee_array_element_type = base;
        }
        if (alias->ptr_level)
            alias->size = PTR_SIZE;
        alias->type_name[0] = '\0';
        add_block_typedef(parent, decl.var_name, alias);
        if (!lex_accept(T_comma))
            break;
        decl = (var_t) {0};
        decl.scope = parent;
        decl.type = base;

        /* A comma continuation may itself be the bounded direct function
         * typedef form, e.g. `typedef int *p_t, unary_t(int);`.
         */
        parsing_block_typedef_declarator = true;
        read_partial_var_decl(&decl, NULL);
        parsing_block_typedef_declarator = false;
    } while (true);
    lex_expect(T_semicolon);
    return bb;
}

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb)
{
    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

    if (lex_peek(T_case, NULL) || lex_peek(T_default, NULL))
        return read_switch_label_statement(parent, bb);

    if (!bb)
        printf("Warning: unreachable code detected\n");

    if (lex_peek(T_open_curly, NULL))
        return read_code_block(parent->func, parent, bb);

    if (lex_accept(T_return)) {
        return handle_return_statement(parent, bb);
    }

    if (lex_accept(T_if)) {
        return handle_if_statement(parent, bb);
    }

    if (lex_accept(T_while)) {
        return handle_while_statement(parent, bb);
    }

    if (lex_accept(T_switch))
        return handle_switch_statement(parent, bb);

    if (lex_accept(T_break)) {
        if (!break_exit_idx)
            error_at("'break' outside of a loop or switch", cur_token_loc());
        bb_connect(bb, break_bb[break_exit_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_continue)) {
        if (!continue_pos_idx)
            error_at("'continue' outside of a loop", cur_token_loc());
        bb_connect(bb, continue_bb[continue_pos_idx - 1], NEXT);
        lex_expect(T_semicolon);
        return NULL;
    }

    if (lex_accept(T_for))
        return handle_for_statement(parent, bb);

    if (lex_accept(T_do))
        return handle_do_statement(parent, bb);

    if (lex_accept(T_goto))
        return handle_goto_statement(parent, bb);

    if (lex_peek(T_typedef, NULL))
        return handle_block_typedef_statement(parent, bb);

    /* empty statement */
    if (lex_accept(T_semicolon))
        return bb;

    if (grouped_scalar_pointee_row_store_starts())
        return handle_grouped_scalar_pointee_row_store(parent, bb);

    /* These cannot begin a declaration, so they are unambiguously expression
     * statements. Identifiers remain delegated to handle_declaration(), which
     * first checks whether they name a type.
     */
    if (lex_peek(T_open_bracket, NULL) || lex_peek(T_numeric, NULL) ||
        lex_peek(T_char, NULL) || lex_peek(T_string, NULL) ||
        lex_peek(T_wstring, NULL) || lex_peek(T_ampersand, NULL) ||
        lex_peek(T_asterisk, NULL) || lex_peek(T_plus, NULL) ||
        lex_peek(T_minus, NULL) || lex_peek(T_increment, NULL) ||
        lex_peek(T_decrement, NULL) || lex_peek(T_log_not, NULL) ||
        lex_peek(T_bit_not, NULL))
        return read_full_expression_statement(parent, bb);

    /* struct/union variable declaration */
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL))
        return handle_record_statement(parent, bb, false, false);

    if (lex_peek(T_enum, NULL))
        return handle_enum_statement(parent, bb, false, false);

    /* Handle const qualifier for local variable declarations */
    return handle_declaration(parent, bb);
}

/* Nesting counter for read_code_block(), which recurses through
 * read_body_statement() for every nested block.
 */
int block_depth = 0;

basic_block_t *read_code_block(func_t *func, block_t *parent, basic_block_t *bb)
{
    block_t *blk = add_block(parent, func);

    if (bb)
        bb->scope = blk;

    block_depth++;
    if (block_depth > MAX_BLOCK_DEPTH)
        error_at("Block nesting too deep", cur_token_loc());

    lex_expect(T_open_curly);

    while (!lex_accept(T_close_curly)) {
        bb = read_body_statement(blk, bb);
        perform_side_effect(blk, bb);
    }

    block_depth--;
    return bb;
}

void var_add_killed_bb(var_t *var, basic_block_t *bb);

void read_func_body(func_t *func)
{
    block_t *blk = add_block(NULL, func);
    func->bbs = bb_create(blk);
    func->exit = bb_create(blk);

    if (func->returns_aggregate) {
        func->sret_def.type = func->return_def.type;
        func->sret_def.ptr_level = 1;
        func->sret_def.var_name = "__shecc_sret";
        func->sret_def.base = &func->sret_def;
        add_symbol(func->bbs, &func->sret_def);
        var_add_killed_bb(&func->sret_def, func->bbs);
    }

    for (int i = 0; i < func->num_params; i++) {
        /* arguments */
        func->param_defs[i].is_aggregate_param =
            is_record_type(func->param_defs[i].type) &&
            !func->param_defs[i].ptr_level;
        add_symbol(func->bbs, &func->param_defs[i]);
        func->param_defs[i].base = &func->param_defs[i];
        var_add_killed_bb(&func->param_defs[i], func->bbs);
    }
    basic_block_t *body = read_code_block(func, NULL, func->bbs);
    if (body)
        bb_connect(body, func->exit, NEXT);

    for (int i = 0; i < backpatch_bb_idx; i++) {
        basic_block_t *bb = backpatch_bb[i];
        insn_t *g = bb->insn_list.tail;
        label_t *label = find_label(g->str);
        if (!label)
            error_at("goto label undefined", cur_token_loc());

        label->used = true;
        bb_connect(bb, label->bb, NEXT);
    }

    for (int i = 0; i < label_idx; i++) {
        const label_t *label = &labels[i];
        if (label->used)
            continue;

        printf("Warning: unused label %s\n", label->label_name);
    }

    backpatch_bb_idx = 0;
    label_idx = 0;
}

void print_ptr_level(int level)
{
    while (level > 0) {
        printf("*");
        level--;
    }
}

void print_func_decl(func_t *func, const char *prefix, bool newline)
{
    if (prefix)
        printf("%s", prefix);

    if (func->return_def.is_const_qualified)
        printf("const ");
    printf("%s ", func->return_def.type->type_name);
    print_ptr_level(func->return_def.ptr_level -
                    func->return_def.type->ptr_level);
    printf("%s(", func->return_def.var_name);

    for (int i = 0; i < func->num_params; i++) {
        const var_t *var = &func->param_defs[i];

        if (var->is_const_qualified)
            printf("const ");
        printf("%s ", var->type->type_name);

        print_ptr_level(var->ptr_level - var->type->ptr_level);

        printf("%s", var->var_name);

        if (i != func->num_params - 1)
            printf(", ");
    }

    if (func->va_args)
        printf(", ...");
    printf(")");

    if (newline)
        printf("\n");
}

/* Emit the optional initializer of a global declarator. Arrays and pointers
 * written with a brace list go through the array initializer; everything else
 * is a scalar constant.
 */
void read_global_init_var(var_t *var, block_t *block)
{
    if (!lex_accept(T_assign))
        return;

    var->has_initializer = true;

    if (lex_peek(T_open_curly, NULL) &&
        (var->array_size > 0 || var->has_unsized_array || var->ptr_level > 0))
        parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
    else if (global_compound_literal_starts_here() &&
             !(var->ptr_level || var->type->ptr_level) &&
             is_record_type(var->type))
        parse_global_compound_record_init(var, block);
    else if (global_compound_literal_starts_here() &&
             (var->ptr_level || var->type->ptr_level))
        parse_global_compound_array_init(var, block);
    else if (global_compound_literal_starts_here())
        parse_global_compound_scalar_init(var, block);
    else if (lex_peek(T_open_curly, NULL) && is_record_type(var->type))
        parse_global_record_init(var, block);
    else
        read_global_assignment_var(var);
}

/* A declarator's base type is already known when this runs. Keeping function
 * completion independent of how that type was spelled lets enum, record, and
 * ordinary scalar declarations share linkage and redeclaration checks.
 */
void read_global_function_declarator(block_t *block, var_t *var, bool is_static)
{
    /* Functions and objects share C's ordinary identifier namespace at file
     * scope. `var` is the provisional declarator for this function, so a
     * different matching global object is a conflict rather than a function
     * redeclaration. Without this check the back end emitted colliding labels
     * and the resulting program could jump through object storage.
     */
    var_t *object = find_global_var(var->var_name);

    if (object && object != var)
        error_at("function declaration conflicts with global object",
                 next_token_loc());

    func_t *func = find_func(var->var_name);
    func_t func_tmp;
    bool check_decl = false;
    bool inherited_direct_function_type =
        var->is_func && var->type->is_direct_function_type;
    func_t *inherited_signature = var->func_signature;

    if (func) {
        memcpy(&func_tmp, func, sizeof(func_t));
        check_decl = true;
    } else {
        func = add_func(var->var_name, false);
    }
    if (!var->is_block_scope_function_declaration)
        func->is_block_scope_only_declaration = false;
    else if (!check_decl)
        func->is_block_scope_only_declaration = true;

    if (inherited_direct_function_type) {
        memcpy(&func->return_def, &inherited_signature->return_def,
               sizeof(var_t));
        func->return_def.var_name = var->var_name;
    } else {
        memcpy(&func->return_def, var, sizeof(var_t));
    }
    func->returns_aggregate = is_record_type(func->return_def.type) &&
                              !has_effective_pointer(&func->return_def);

    /* A typedef can hide an array return type: `typedef int row[2]; row
     * f(void);` has no array suffix after the function declarator, but C99
     * still forbids a function from returning an array. Count pointer depth
     * carried by both the declarator and its typedef, so a typedef-hidden
     * pointer to that array remains a legal return type.
     */
    if (!effective_pointer_depth(&func->return_def) && func->return_def.type &&
        func->return_def.type->array_size)
        error_at("function cannot return an array type", next_token_loc());
    if (check_decl && !func_tmp.is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    func->is_static = check_decl && func_tmp.is_static ? true : is_static;
    func->is_inline = var->is_inline;
    var_reset_subscripts(&func->return_def);
    block->locals.size--;

    /* Parse this declarator independently of any earlier declaration. A later
     * `f()` must not inherit stale parameter slots while a later typed list may
     * legitimately refine an earlier unprototyped declaration. A block direct
     * function typedef has already parsed its prototype, so copy that exact
     * syntax-only signature instead of trying to consume another suffix.
     */
    func->num_params = 0;
    func->va_args = 0;
    func->has_prototype = false;
    memset(func->param_defs, 0, sizeof(func->param_defs));
    if (inherited_direct_function_type) {
        func->num_params = inherited_signature->num_params;
        func->va_args = inherited_signature->va_args;
        func->has_prototype = inherited_signature->has_prototype;
        memcpy(func->param_defs, inherited_signature->param_defs,
               sizeof(func->param_defs));
    } else {
        /* Parameter identifiers are optional in declarations. Definitions check
         * for them below before body lowering makes parameter symbols visible.
         */
        read_parameter_list_decl(func, true);
    }

    if (check_decl) {
        if (!compatible_decl_type(func->return_def.type,
                                  func_tmp.return_def.type) ||
            func->return_def.ptr_level != func_tmp.return_def.ptr_level ||
            func->return_def.is_const_qualified !=
                func_tmp.return_def.is_const_qualified)
            error_at("conflicting types for function declaration",
                     next_token_loc());
        if (func->has_prototype && func_tmp.has_prototype) {
            if (func->num_params != func_tmp.num_params ||
                func->va_args != func_tmp.va_args)
                error_at("conflicting types for function declaration",
                         next_token_loc());
            for (int i = 0; i < func->num_params; i++) {
                const var_t *now = &func->param_defs[i];
                const var_t *before = &func_tmp.param_defs[i];
                if (!compatible_function_param_decl(now, before))
                    error_at("conflicting types for function declaration",
                             next_token_loc());
            }
        } else if (strict_c99 && func->has_prototype &&
                   !func_tmp.has_prototype) {
            /* A variadic prototype and parameters promoted from char, short, or
             * _Bool cannot be compatible with an earlier empty parameter list
             * declaration. Keep the historical extension outside strict C99
             * mode, where existing old-style sources rely on it.
             */
            if (func->va_args)
                error_at("conflicting types for function declaration",
                         next_token_loc());
            for (int i = 0; i < func->num_params; i++)
                if (parameter_changes_under_default_promotion(
                        &func->param_defs[i]))
                    error_at("conflicting types for function declaration",
                             next_token_loc());
        } else if (!func->has_prototype && func_tmp.has_prototype) {
            /* An empty-list definition has no named parameters. It cannot
             * define a function previously declared with fixed parameters or an
             * ellipsis, even though a non-defining `f()` declaration may
             * coexist with that prototype.
             */
            if (lex_peek(T_open_curly, NULL) &&
                (func_tmp.num_params || func_tmp.va_args))
                error_at("conflicting types for function declaration",
                         next_token_loc());

            /* A prior prototype remains visible after a compatible `f()`
             * declaration and still constrains subsequent calls.
             */
            memcpy(func->param_defs, func_tmp.param_defs,
                   sizeof(func->param_defs));
            func->num_params = func_tmp.num_params;
            func->va_args = func_tmp.va_args;
            func->has_prototype = true;
        }
    }

    if (lex_peek(T_open_curly, NULL)) {
        if (check_decl && func_tmp.bbs)
            error_at("redefinition of function", next_token_loc());
        if (is_incomplete_record_object(&func->return_def))
            error_at("function definition cannot return incomplete record type",
                     next_token_loc());
        for (int i = 0; i < func->num_params; i++)
            if (!func->param_defs[i].var_name ||
                !func->param_defs[i].var_name[0])
                error_at("function definition parameter requires an identifier",
                         next_token_loc());
            else if (!func->param_defs[i].array_size &&
                     !func->param_defs[i].has_unsized_array &&
                     is_incomplete_record_object(&func->param_defs[i]))
                error_at("Incomplete struct/union type cannot define an object",
                         next_token_loc());
        read_func_body(func);
        return;
    }
    if (inherited_direct_function_type && lex_peek(T_comma, NULL))
        return;
    if (!lex_accept(T_semicolon))
        error_at("Syntax error in global declaration", next_token_loc());
}

/* A compatible repeated file-scope declaration names the same object. The
 * parser creates a provisional var_t while reading its declarator, so discard
 * that entry before emitting allocation or initializer IR and keep the first
 * declaration's storage.
 */
var_t *resolve_global_declarator(block_t *block,
                                 var_t *var,
                                 bool is_static,
                                 bool *is_redeclaration)
{
    var_t *previous = NULL;

    *is_redeclaration = false;

    /* The ordinary identifier namespace is shared with functions. This is
     * intentionally before object redeclaration handling: a function is not a
     * compatible tentative definition of an object, even when both happen to
     * have the same declared scalar type.
     */
    if (find_func(var->var_name))
        error_at("global object declaration conflicts with function",
                 next_token_loc());

    for (int i = 0; i + 1 < block->locals.size; i++) {
        var_t *candidate = block->locals.elements[i];
        if (!strcmp(candidate->var_name, var->var_name)) {
            previous = candidate;
            break;
        }
    }
    if (!previous)
        return var;

    *is_redeclaration = true;

    if (!compatible_decl_type(previous->type, var->type) ||
        (!!previous->pointee_func_signature != !!var->pointee_func_signature) ||
        (previous->pointee_func_signature &&
         !compatible_function_signature(previous->pointee_func_signature,
                                        var->pointee_func_signature)) ||
        previous->ptr_level != var->ptr_level ||
        previous->array_size != var->array_size ||
        previous->array_dim2 != var->array_dim2 ||
        previous->array_dim3 != var->array_dim3 ||
        previous->array_dim4 != var->array_dim4 ||
        previous->pointee_array_size != var->pointee_array_size ||
        previous->pointee_array_dim2 != var->pointee_array_dim2 ||
        previous->pointee_array_dim3 != var->pointee_array_dim3 ||
        previous->pointee_array_dim4 != var->pointee_array_dim4 ||
        previous->is_const_qualified != var->is_const_qualified)
        error_at("conflicting types for global declaration", next_token_loc());
    if (!previous->is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    if (lex_peek(T_assign, NULL) && previous->has_initializer)
        error_at("redefinition of global variable", next_token_loc());

    /* Scalar declarators were placed on the operand stack by
     * read_inner_var_decl(). Its later initializer lowering pops that entry, so
     * point it at the shared object rather than the discarded declaration.
     */
    if (operand_stack_idx && operand_stack[operand_stack_idx - 1] == var)
        operand_stack[operand_stack_idx - 1] = previous;
    block->locals.size--;
    return previous;
}

/* Read one declarator after the first in a global declaration. Each shares the
 * declaration's base type: "int a = 1, b, c = 3;".
 */
bool read_global_declarator(block_t *block,
                            type_t *decl_type,
                            bool is_const,
                            bool is_static,
                            bool is_volatile,
                            bool is_extern)
{
    bool is_redeclaration;
    var_t *nv = require_typed_var(block, decl_type);
    nv->is_global = true;
    nv->is_static = is_static;
    nv->is_const_qualified = is_const;
    nv->is_volatile = is_volatile;
    read_inner_var_decl(nv, false, false, false);
    nv->is_extern = is_extern && !lex_peek(T_assign, NULL);
    if (lex_peek(T_open_bracket, NULL)) {
        read_global_function_declarator(block, nv, is_static);
        return true;
    }
    bool is_definition = !nv->is_extern;
    if (is_definition && is_incomplete_record_object(nv))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    nv = resolve_global_declarator(block, nv, is_static, &is_redeclaration);
    if (is_definition && (!is_redeclaration || nv->is_extern)) {
        nv->is_extern = false;
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, nv, NULL, NULL, 0, NULL);
    }
    read_global_init_var(nv, block);
    discard_global_declarator_operand(nv);
    return false;
}

void consume_global_compound_literal(void);

/* Lower a scalar record initializer into the global initializer block. Global
 * array elements already use parse_struct_field_init(); scalar records need the
 * same field-address writes rather than merely consuming their braces.
 */
void parse_global_record_init(var_t *var, block_t *block)
{
    type_t *record_type = var->type;
    block_t *saved_initializer_scope = global_constant_initializer_scope;
    if (var->scope)
        global_constant_initializer_scope = var->scope;
    if (record_type->base_type == TYPE_typedef && record_type->base_struct)
        record_type = record_type->base_struct;

    lex_expect(T_open_curly);
    parse_struct_field_init(block, &GLOBAL_FUNC->bbs, record_type, var, true);
    lex_expect(T_close_curly);
    global_constant_initializer_scope = saved_initializer_scope;
}

/* At file scope a compound literal has static storage duration. The target
 * object is already global, so a record compound literal can use its normal
 * constant aggregate lowering after consuming the spelled type name.
 */
void parse_global_compound_record_init(var_t *var, block_t *block)
{
    char type_name[MAX_ID_LEN];
    int find_type_flag = 1;
    type_t *compound_type, *target_type;

    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        find_type_flag = 2;
        lex_ident(T_identifier, type_name);
    } else {
        lex_ident(T_identifier, type_name);
    }
    lex_expect(T_close_bracket);

    compound_type = find_type(type_name, find_type_flag);
    target_type = var->type;
    if (target_type->base_type == TYPE_typedef && target_type->base_struct)
        target_type = target_type->base_struct;
    if (compound_type && compound_type->base_type == TYPE_typedef &&
        compound_type->base_struct)
        compound_type = compound_type->base_struct;
    if (!compound_type || !is_record_type(compound_type) ||
        compound_type != target_type)
        error_at("Incompatible record compound literal", cur_token_loc());

    if (!lex_peek(T_open_curly, NULL))
        error_at("Record compound literal needs an initializer",
                 next_token_loc());
    parse_global_record_init(var, block);
}

/* A scalar compound literal at file scope also has static storage duration. Its
 * sole initializer is the target object's constant initializer, so no temporary
 * storage is necessary after validating the spelled scalar type.
 */
void parse_global_compound_scalar_init(var_t *var, block_t *block)
{
    char type_name[MAX_ID_LEN];
    type_t *compound_type;

    UNUSED(block);

    lex_expect(T_open_bracket);
    lex_ident(T_identifier, type_name);
    compound_type = find_type(type_name, true);
    lex_expect(T_close_bracket);

    if (!compound_type || is_record_type(compound_type) || var->ptr_level ||
        compound_type != var->type)
        error_at("Incompatible scalar compound literal", cur_token_loc());

    lex_expect(T_open_curly);
    if (lex_peek(T_close_curly, NULL))
        error_at("Scalar compound literal needs an initializer",
                 next_token_loc());
    read_global_assignment_var(var);
    if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
        error_at("Too many elements in scalar compound literal",
                 next_token_loc());
    lex_expect(T_close_curly);
}

/* An array compound literal at file scope is an unnamed static array. Keep that
 * array in the global initializer block so its backing storage survives for the
 * full program, then initialize the declared pointer with its base.
 */
void parse_global_compound_array_init(var_t *var, block_t *block)
{
    char type_name[MAX_ID_LEN];
    type_t *element_type;
    var_t *array;
    int find_type_flag = 1;
    int element_ptr_level = 0;

    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        find_type_flag = 2;
        lex_ident(T_identifier, type_name);
    } else {
        lex_ident(T_identifier, type_name);
    }
    element_type = find_type(type_name, find_type_flag);
    while (lex_accept(T_asterisk)) {
        element_ptr_level++;
        while (lex_accept(T_const) || lex_accept(T_volatile) ||
               lex_accept(T_restrict))
            ;
    }

    /* `(int (*[])[2]){...}` is an array whose elements are pointers to rows.
     * The inner suffix supplies the backing array bound; the suffixes after `)`
     * describe each pointer element's pointee.
     */
    if (lex_accept(T_open_bracket)) {
        int pointee_dims = 0;

        if (element_ptr_level || !lex_peek(T_asterisk, NULL))
            error_at("Array compound literal needs a pointer declarator",
                     cur_token_loc());
        do {
            lex_expect(T_asterisk);
            element_ptr_level++;
            while (lex_accept(T_const) || lex_accept(T_volatile) ||
                   lex_accept(T_restrict))
                ;
        } while (lex_peek(T_asterisk, NULL));
        lex_expect(T_open_square);

        array = require_typed_var(GLOBAL_BLOCK, element_type);
        array->var_name = gen_name();
        array->is_global = true;
        array->ptr_level = element_ptr_level;
        if (!lex_peek(T_close_square, NULL)) {
            array->array_size = read_const_expr(GLOBAL_BLOCK);
            if (array->array_size <= 0)
                error_at("Array compound literal needs a positive bound",
                         cur_token_loc());
        } else {
            array->has_unsized_array = true;
        }
        lex_expect(T_close_square);
        lex_expect(T_close_bracket);
        while (lex_accept(T_open_square)) {
            int bound = read_const_expr(GLOBAL_BLOCK);

            if (pointee_dims >= 4)
                error_at("Array declarators support at most four dimensions",
                         cur_token_loc());
            if (bound <= 0)
                error_at("Array size must be positive", cur_token_loc());
            if (pointee_dims == 0)
                array->pointee_array_size = bound;
            else {
                if (pointee_dims == 1)
                    array->pointee_array_dim2 = bound;
                else if (pointee_dims == 2)
                    array->pointee_array_dim3 = bound;
                else
                    array->pointee_array_dim4 = bound;
                array->pointee_array_size *= bound;
            }
            lex_expect(T_close_square);
            pointee_dims++;
        }
        lex_expect(T_close_bracket);

        if (!element_type || element_type != var->type ||
            var->ptr_level != element_ptr_level + 1 ||
            var->pointee_array_size != array->pointee_array_size ||
            var->pointee_array_dim2 != array->pointee_array_dim2 ||
            var->pointee_array_dim3 != array->pointee_array_dim3 ||
            var->pointee_array_dim4 != array->pointee_array_dim4)
            error_at("Incompatible array compound literal", cur_token_loc());
        if (!lex_peek(T_open_curly, NULL))
            error_at("Array compound literal needs an initializer",
                     next_token_loc());
        add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, array, NULL, NULL,
                 0, NULL);
        parse_array_init(array, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
        add_insn(block, GLOBAL_FUNC->bbs, OP_assign, var, array, NULL, 0, NULL);
        return;
    }
    array = require_typed_var(GLOBAL_BLOCK, element_type);
    array->var_name = gen_name();
    array->is_global = true;
    array->ptr_level = element_ptr_level;
    for (int dim = 0; lex_accept(T_open_square); dim++) {
        int bound;

        if (dim >= 4)
            error_at("Array compound literal supports at most four dimensions",
                     cur_token_loc());
        if (lex_peek(T_close_square, NULL)) {
            if (dim)
                error_at("Only the outer array bound may be inferred",
                         cur_token_loc());
            array->has_unsized_array = true;
            lex_expect(T_close_square);
            continue;
        }
        bound = read_const_expr(GLOBAL_BLOCK);
        if (bound <= 0)
            error_at("Array compound literal needs a positive bound",
                     cur_token_loc());
        if (!dim)
            array->array_size = bound;
        else {
            if (dim == 1)
                array->array_dim2 = bound;
            else if (dim == 2)
                array->array_dim3 = bound;
            else
                array->array_dim4 = bound;
            array->array_size *= bound;
        }
        lex_expect(T_close_square);
    }
    lex_expect(T_close_bracket);

    /* The backing array has an outer compound-literal bound and the typedef
     * element's inner row shape. parse_array_init() keeps that shape on var_t
     * rather than type_t.
     */
    if (element_type && element_type->array_size) {
        int trailing = 1;

        if (element_type->array_dim2)
            trailing *= element_type->array_dim2;
        if (element_type->array_dim3)
            trailing *= element_type->array_dim3;
        if (element_type->array_dim4)
            trailing *= element_type->array_dim4;
        if (element_type->array_dim4)
            error_at("Array compound literal supports at most four dimensions",
                     cur_token_loc());
        array->array_dim2 = element_type->array_size / trailing;
        array->array_dim3 = element_type->array_dim2;
        array->array_dim4 = element_type->array_dim3;
        if (array->array_size)
            array->array_size *= element_type->array_size;
    }

    if (!element_type || element_type != var->type ||
        element_ptr_level + 1 != var->ptr_level ||
        var->pointee_array_size != array->array_dim2)
        error_at("Incompatible array compound literal", cur_token_loc());
    if (!lex_peek(T_open_curly, NULL))
        error_at("Array compound literal needs an initializer",
                 next_token_loc());

    add_insn(GLOBAL_BLOCK, GLOBAL_FUNC->bbs, OP_allocat, array, NULL, NULL, 0,
             NULL);
    parse_array_init(array, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs, true);
    add_insn(block, GLOBAL_FUNC->bbs, OP_assign, var, array, NULL, 0, NULL);
}

/* Struct and union objects accept brace initializers, unlike scalar globals.
 * Keep their continuation declarators on the same path as the first one so that
 * linkage, qualifiers, and declarator-specific modifiers cannot diverge.
 */
bool read_global_record_declarator(block_t *block,
                                   type_t *decl_type,
                                   bool is_const,
                                   bool is_static,
                                   bool is_extern)
{
    bool is_redeclaration;
    var_t *var = require_typed_var(block, decl_type);
    var->is_global = true;
    var->is_static = is_static;
    var->is_const_qualified = is_const;
    read_inner_var_decl(var, false, false, false);
    var->is_extern = is_extern && !lex_peek(T_assign, NULL);
    if (lex_peek(T_open_bracket, NULL)) {
        read_global_function_declarator(block, var, is_static);
        return true;
    }
    bool is_definition = !var->is_extern;
    if (is_definition && is_incomplete_record_object(var))
        error_at("Incomplete struct/union type cannot define an object",
                 cur_token_loc());
    var = resolve_global_declarator(block, var, is_static, &is_redeclaration);
    if (is_definition && (!is_redeclaration || var->is_extern)) {
        var->is_extern = false;
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);
    }

    if (!lex_accept(T_assign)) {
        discard_global_declarator_operand(var);
        return false;
    }

    var->has_initializer = true;

    if (lex_peek(T_open_curly, NULL) &&
        (var->array_size > 0 || var->has_unsized_array || var->ptr_level > 0)) {
        parse_array_init(var, block, &GLOBAL_FUNC->bbs, true);
    } else if (global_compound_literal_starts_here() &&
               (var->ptr_level || var->type->ptr_level)) {
        parse_global_compound_array_init(var, block);
    } else if (global_compound_literal_starts_here()) {
        parse_global_compound_record_init(var, block);
    } else if (lex_peek(T_open_curly, NULL)) {
        parse_global_record_init(var, block);
    } else {
        read_global_assignment_var(var);
    }
    discard_global_declarator_operand(var);
    return false;
}

void read_global_decl(block_t *block,
                      bool is_const,
                      bool is_static,
                      bool is_extern,
                      bool is_inline,
                      bool is_volatile)
{
    bool is_redeclaration;
    var_t *var = require_var(block);
    var->is_global = true;
    var->is_static = is_static;
    var->is_inline = is_inline;
    var->is_const_qualified = is_const;
    var->is_volatile = is_volatile;

    /* new function, or variables under parent */
    read_full_var_decl(var, false, false, false);
    var->is_extern = is_extern && !lex_peek(T_assign, NULL);

    if (lex_peek(T_open_bracket, NULL)) {
        read_global_function_declarator(block, var, is_static);
        return;
    } else {
        bool is_definition = !var->is_extern;
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());
        if (is_definition && is_incomplete_record_object(var))
            error_at("Incomplete struct/union type cannot define an object",
                     cur_token_loc());
        var =
            resolve_global_declarator(block, var, is_static, &is_redeclaration);
        if (is_definition && (!is_redeclaration || var->is_extern)) {
            var->is_extern = false;
            add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                     NULL);
        }
    }

    /* is a variable */
    if (lex_peek(T_assign, NULL)) {
        read_global_init_var(var, block);
    } else if (lex_peek(T_semicolon, NULL)) {
    } else if (!lex_peek(T_comma, NULL)) {
        error_at("Syntax error in global declaration", next_token_loc());
    }
    discard_global_declarator_operand(var);

    /* Continuation: "int a = 1, b, c = 3;". Every declarator after the first
     * shares this declaration's base type and is handled exactly like the
     * first, mirroring what the struct-tagged global path already does.
     */
    while (lex_accept(T_comma))
        read_global_declarator(block, var->type, var->is_const_qualified,
                               is_static, var->is_volatile, is_extern);

    lex_expect(T_semicolon);
    return;
}

void consume_global_compound_literal(void)
{
    lex_expect(T_open_curly);

    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            /* Just consume constant values for now */
            if (lex_peek(T_numeric, NULL)) {
                lex_accept(T_numeric);
            } else if (lex_peek(T_minus, NULL)) {
                lex_accept(T_minus);
                lex_accept(T_numeric);
            } else if (lex_peek(T_string, NULL)) {
                lex_accept(T_string);
            } else if (lex_peek(T_char, NULL) || lex_peek(T_wchar, NULL)) {
                lex_next();
            } else {
                error_at(
                    "Global struct initialization requires constant values",
                    next_token_loc());
            }

            if (!lex_accept(T_comma))
                break;
            if (lex_peek(T_close_curly, NULL))
                break;
        }
    }
    lex_expect(T_close_curly);
}

void initialize_struct_field(var_t *nv, var_t *v, int offset)
{
    nv->type = v->type;
    nv->var_name = "";
    nv->ptr_level = 0;
    nv->is_func = false;
    nv->is_global = false;
    nv->is_const_qualified = false;
    nv->array_size = 0;
    nv->offset = offset;
    nv->is_bitfield = false;
    nv->bit_width = 0;
    nv->bit_offset = 0;
    nv->bit_storage_size = 0;
    nv->init_val = 0;
    nv->base = NULL;
    nv->subscript = 0;
    var_reset_subscripts(nv);
    nv->is_compound_literal = false;
}

void read_global_statement(void)
{
    char token[MAX_ID_LEN];
    block_t *block = GLOBAL_BLOCK; /* global block */
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_inline = false;
    bool is_volatile = false;

    /* These specifiers may appear in either order. */
    while (lex_peek(T_const, NULL) || lex_peek(T_static, NULL) ||
           lex_peek(T_extern, NULL) || lex_peek(T_inline, NULL) ||
           lex_peek(T_volatile, NULL)) {
        if (lex_accept(T_const))
            is_const = true;
        else if (lex_accept(T_volatile))
            is_volatile = true;
        else if (lex_accept(T_static)) {
            if (is_static)
                error_at("duplicate static storage class specifier",
                         cur_token_loc());
            is_static = true;
        } else if (lex_accept(T_inline)) {
            if (is_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else {
            lex_expect(T_extern);
            if (is_extern)
                error_at("duplicate extern storage class specifier",
                         cur_token_loc());
            is_extern = true;
        }
    }
    if (is_static && is_extern)
        error_at("static and extern storage classes cannot be combined",
                 cur_token_loc());

    if (floating_type_starts_here())
        error_at("Floating point types are not yet supported", cur_token_loc());

    /* `inline` is a function specifier, not a general declaration qualifier.
     * Scalar declarators are checked by read_global_decl(), but record, enum,
     * and typedef declarations bypass that path entirely. Rejecting those forms
     * here keeps every file-scope declaration category under the same C99
     * constraint.
     */
    if (is_inline && (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL) ||
                      lex_peek(T_enum, NULL) || lex_peek(T_typedef, NULL)))
        error_at("inline specifier requires a function declarator",
                 cur_token_loc());

    if (lex_accept(T_struct)) {
        int i = 0, size = 0, alignment = 1;
        bitfield_layout_t bits = {0};
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);
        token_t *id_tk = cur_token;

        /* variable declaration using existing struct tag? */
        if (!lex_peek(T_open_curly, NULL)) {
            type_t *decl_type = find_type(token, 2);
            if (!decl_type && lex_peek(T_semicolon, NULL)) {
                decl_type = add_type();
                decl_type->base_type = TYPE_struct;
                set_type_name(decl_type, token);
                lex_expect(T_semicolon);
                return;
            }
            if (!decl_type)
                error_at("Unknown struct type", &id_tk->location);

            if (read_global_record_declarator(block, decl_type, is_const,
                                              is_static, is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_record_declarator(block, decl_type, is_const,
                                              is_static, is_extern);
            lex_expect(T_semicolon);
            return;
        }

        /* struct definition has forward declaration? */
        type_t *type = find_type(token, 2);
        if (!type)
            type = add_type();

        set_type_name(type, token);
        type->base_type = TYPE_struct;

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            var_t *last = v;
            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, block);
            reject_flexible_array_member_container(v);
            mark_flexible_array_member(v, false);
            size = is_bitfield(v)
                       ? layout_bitfield_field(size, v, &alignment, &bits)
                       : layout_struct_field(flush_bitfield_layout(size, &bits),
                                             v, &alignment);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                if (last->is_flexible_array_member)
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                var_t *nv = type_add_field(type, &i);
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, block);
                reject_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, false);
                last = nv;
                size = is_bitfield(nv)
                           ? layout_bitfield_field(size, nv, &alignment, &bits)
                           : layout_struct_field(
                                 flush_bitfield_layout(size, &bits), nv,
                                 &alignment);
            }

            lex_expect(T_semicolon);
            if (last->is_flexible_array_member) {
                if (!lex_peek(T_close_curly, NULL))
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                if (i == 1)
                    error_at(
                        "Struct needs a named member before its flexible array "
                        "member",
                        cur_token_loc());
                has_flexible_array_member = true;
            }
        } while (!lex_accept(T_close_curly));

        size = flush_bitfield_layout(size, &bits);
        type->alignment = alignment;
        type->size = ALIGN_UP(size, alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        /* A record definition may be followed by its declarators, as in "struct
         * pair { int x, y; } first, *second;".
         */
        if (!lex_peek(T_semicolon, NULL)) {
            if (read_global_record_declarator(block, type, is_const, is_static,
                                              is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_record_declarator(block, type, is_const, is_static,
                                              is_extern);
        }
        lex_expect(T_semicolon);
    } else if (lex_accept(T_union)) {
        int i = 0, max_size = 0, alignment = 1;
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);
        token_t *id_tk = cur_token;

        /* A tagged union declaration may name an already-complete tag, just
         * like `struct tag object;`. Do not require a second definition body
         * before routing its declarators through the shared record path.
         */
        if (!lex_peek(T_open_curly, NULL)) {
            type_t *decl_type = find_type(token, 2);
            if (!decl_type && lex_peek(T_semicolon, NULL)) {
                decl_type = add_type();
                decl_type->base_type = TYPE_union;
                set_type_name(decl_type, token);
                lex_expect(T_semicolon);
                return;
            }
            if (!decl_type)
                error_at("Unknown union type", &id_tk->location);

            if (read_global_record_declarator(block, decl_type, is_const,
                                              is_static, is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_record_declarator(block, decl_type, is_const,
                                              is_static, is_extern);
            lex_expect(T_semicolon);
            return;
        }

        /* has forward declaration? */
        type_t *type = find_type(token, 2);
        if (!type)
            type = add_type();

        set_type_name(type, token);
        type->base_type = TYPE_union;

        lex_expect(T_open_curly);
        do {
            var_t *v = type_add_field(type, &i);
            read_full_var_decl(v, false, false, true);
            read_bitfield_width(v, block);
            has_flexible_array_member |= is_flexible_array_member_container(v);
            mark_flexible_array_member(v, true);
            v->offset = 0; /* All union fields start at offset 0 */
            int field_size = is_bitfield(v)
                                 ? (v->bit_width ? v->bit_storage_size : 0)
                                 : size_var(v);
            if (field_size > max_size)
                max_size = field_size;
            if (alignment_var(v) > alignment)
                alignment = alignment_var(v);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                var_t *nv = type_add_field(type, &i);
                /* All union fields start at offset 0 */
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                read_bitfield_width(nv, block);
                has_flexible_array_member |=
                    is_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, true);
                field_size = is_bitfield(nv)
                                 ? (nv->bit_width ? nv->bit_storage_size : 0)
                                 : size_var(nv);
                if (field_size > max_size)
                    max_size = field_size;
                if (alignment_var(nv) > alignment)
                    alignment = alignment_var(nv);
            }

            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->alignment = alignment;
        type->size = ALIGN_UP(max_size, alignment);
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        if (!lex_peek(T_semicolon, NULL)) {
            if (read_global_record_declarator(block, type, is_const, is_static,
                                              is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_record_declarator(block, type, is_const, is_static,
                                              is_extern);
        }
        lex_expect(T_semicolon);
    } else if (lex_accept(T_enum)) {
        /* An enum definition is a declaration in its own right; it need not
         * introduce a typedef. Its enumerators are integer constants and may
         * use the same integer constant expressions accepted for array bounds
         * and case labels.
         */
        int val = 0;
        bool has_tag = false;
        type_t *type;

        if (lex_peek(T_identifier, token)) {
            lex_expect(T_identifier);
            has_tag = true;
        }
        if (!lex_peek(T_open_curly, NULL)) {
            if (!has_tag)
                error_at("Expected enum tag or definition", cur_token_loc());
            type = find_type(token, true);
            if (!type)
                error_at("Unknown enum type", cur_token_loc());
            if (read_global_declarator(block, type, is_const, is_static,
                                       is_volatile, is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_declarator(block, type, is_const, is_static,
                                       is_volatile, is_extern);
            lex_expect(T_semicolon);
            return;
        }
        type = has_tag ? find_type(token, true) : NULL;
        if (!type)
            type = add_type();

        initialize_enum_type(type);
        if (has_tag)
            set_type_name(type, token);
        lex_expect(T_open_curly);
        bool first = true;
        do {
            lex_ident(T_identifier, token);
            if (!first && !lex_peek(T_assign, NULL))
                val = next_enum_value(val);
            if (lex_accept(T_assign))
                val = read_enum_constant(block);
            add_constant(token, val);
            first = false;
        } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
        lex_expect(T_close_curly);
        if (!lex_peek(T_semicolon, NULL)) {
            if (read_global_declarator(block, type, is_const, is_static,
                                       is_volatile, is_extern))
                return;
            while (lex_accept(T_comma))
                read_global_declarator(block, type, is_const, is_static,
                                       is_volatile, is_extern);
        }
        lex_expect(T_semicolon);
    } else if (lex_accept(T_typedef)) {
        if (lex_accept(T_enum)) {
            int val = 0;
            type_t *type = add_type();

            initialize_enum_type(type);
            lex_expect(T_open_curly);
            bool first = true;
            do {
                lex_ident(T_identifier, token);
                if (!first && !lex_peek(T_assign, NULL))
                    val = next_enum_value(val);
                if (lex_accept(T_assign))
                    val = read_enum_constant(block);
                add_constant(token, val);
                first = false;
            } while (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL));
            lex_expect(T_close_curly);
            lex_ident(T_identifier, token);
            set_type_name(type, token);
            lex_expect(T_semicolon);
        } else if (lex_accept(T_struct)) {
            int i = 0, size = 0, alignment = 1;
            bitfield_layout_t bits = {0};
            bool has_struct_def = false;
            bool has_flexible_array_member = false;
            type_t *tag = NULL, *type = add_type();

            /* is struct definition? */
            if (lex_peek(T_identifier, token)) {
                lex_expect(T_identifier);

                /* is existent? */
                tag = find_type(token, 2);
                if (!tag) {
                    tag = add_type();
                    tag->base_type = TYPE_struct;
                    set_type_name(tag, token);
                }
            }

            /* typedef with struct definition */
            if (lex_accept(T_open_curly)) {
                has_struct_def = true;
                do {
                    var_t *v = type_add_field(type, &i);
                    var_t *last = v;
                    read_full_var_decl(v, false, false, true);
                    read_bitfield_width(v, block);
                    reject_flexible_array_member_container(v);
                    mark_flexible_array_member(v, false);
                    size =
                        is_bitfield(v)
                            ? layout_bitfield_field(size, v, &alignment, &bits)
                            : layout_struct_field(
                                  flush_bitfield_layout(size, &bits), v,
                                  &alignment);

                    /* Handle multiple variable declarations with same base type
                     */
                    while (lex_accept(T_comma)) {
                        if (last->is_flexible_array_member)
                            error_at(
                                "Flexible array member must be the final "
                                "struct member",
                                cur_token_loc());
                        var_t *nv = type_add_field(type, &i);
                        initialize_struct_field(nv, v, 0);
                        read_inner_var_decl(nv, false, false, true);
                        read_bitfield_width(nv, block);
                        reject_flexible_array_member_container(nv);
                        mark_flexible_array_member(nv, false);
                        last = nv;
                        size = is_bitfield(nv)
                                   ? layout_bitfield_field(size, nv, &alignment,
                                                           &bits)
                                   : layout_struct_field(
                                         flush_bitfield_layout(size, &bits), nv,
                                         &alignment);
                    }

                    lex_expect(T_semicolon);
                    if (last->is_flexible_array_member) {
                        if (!lex_peek(T_close_curly, NULL))
                            error_at(
                                "Flexible array member must be the final "
                                "struct member",
                                cur_token_loc());
                        if (i == 1)
                            error_at(
                                "Struct needs a named member before its "
                                "flexible array member",
                                cur_token_loc());
                        has_flexible_array_member = true;
                    }
                } while (!lex_accept(T_close_curly));
            }

            while (lex_accept(T_asterisk)) {
                type->ptr_level++;
                type->size = PTR_SIZE;
            }
            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
            size = flush_bitfield_layout(size, &bits);
            type->alignment = type->ptr_level ? PTR_SIZE : alignment;
            type->size = type->ptr_level ? PTR_SIZE : ALIGN_UP(size, alignment);
            type->num_fields = i;
            type->base_type = TYPE_typedef;
            type->has_flexible_array_member = has_flexible_array_member;

            if (tag && has_struct_def == 1) {
                strcpy(token, tag->type_name);
                memcpy(tag, type, sizeof(type_t));
                tag->base_type = TYPE_struct;
                set_type_name(tag, token);
            } else {
                /* If it is a forward declaration, build a connection between
                 * structure tag and alias. In 'find_type', it will retrieve
                 * infomation from base structure for alias.
                 */
                type->base_struct = tag;
            }

            lex_expect(T_semicolon);
        } else if (lex_accept(T_union)) {
            int i = 0, max_size = 0, alignment = 1;
            bool has_union_def = false;
            bool has_flexible_array_member = false;
            type_t *tag = NULL, *type = add_type();

            /* is union definition? */
            if (lex_peek(T_identifier, token)) {
                lex_expect(T_identifier);

                /* is existent? */
                tag = find_type(token, 2);
                if (!tag) {
                    tag = add_type();
                    tag->base_type = TYPE_union;
                    set_type_name(tag, token);
                }
            }

            /* typedef with union definition */
            if (lex_accept(T_open_curly)) {
                has_union_def = true;
                do {
                    var_t *v = type_add_field(type, &i);
                    read_full_var_decl(v, false, false, true);
                    read_bitfield_width(v, block);
                    has_flexible_array_member |=
                        is_flexible_array_member_container(v);
                    mark_flexible_array_member(v, true);
                    v->offset = 0; /* All union fields start at offset 0 */
                    int field_size =
                        is_bitfield(v)
                            ? (v->bit_width ? v->bit_storage_size : 0)
                            : size_var(v);
                    if (field_size > max_size)
                        max_size = field_size;
                    if (alignment_var(v) > alignment)
                        alignment = alignment_var(v);

                    /* Handle multiple variable declarations with same base type
                     */
                    while (lex_accept(T_comma)) {
                        var_t *nv = type_add_field(type, &i);
                        /* All union fields start at offset 0 */
                        initialize_struct_field(nv, v, 0);
                        read_inner_var_decl(nv, false, false, true);
                        read_bitfield_width(nv, block);
                        has_flexible_array_member |=
                            is_flexible_array_member_container(nv);
                        mark_flexible_array_member(nv, true);
                        field_size =
                            is_bitfield(nv)
                                ? (nv->bit_width ? nv->bit_storage_size : 0)
                                : size_var(nv);
                        if (field_size > max_size)
                            max_size = field_size;
                        if (alignment_var(nv) > alignment)
                            alignment = alignment_var(nv);
                    }

                    lex_expect(T_semicolon);
                } while (!lex_accept(T_close_curly));
            }

            while (lex_accept(T_asterisk)) {
                type->ptr_level++;
                type->size = PTR_SIZE;
            }
            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
            type->alignment = type->ptr_level ? PTR_SIZE : alignment;
            type->size =
                type->ptr_level ? PTR_SIZE : ALIGN_UP(max_size, alignment);
            type->num_fields = i;
            type->base_type = TYPE_typedef;
            type->is_union = true;
            type->has_flexible_array_member = has_flexible_array_member;

            if (tag && has_union_def == 1) {
                strcpy(token, tag->type_name);
                memcpy(tag, type, sizeof(type_t));
                tag->base_type = TYPE_union;
                set_type_name(tag, token);
            } else {
                /* If it is a forward declaration, build a connection between
                 * union tag and alias. In 'find_type', it will retrieve
                 * information from base union for alias.
                 */
                type->base_struct = tag;
            }

            lex_expect(T_semicolon);
        } else {
            char base_type[MAX_ID_LEN];
            const type_t *base;
            type_t *type = add_type();
            bool typedef_const = false;
            bool is_signed = false;
            bool is_unsigned = false;
            bool is_long = false;
            bool is_long_long = false;
            type_t *leading_scalar_type = NULL;

            if (lex_peek(T_identifier, base_type) &&
                (!strcmp(base_type, "char") || !strcmp(base_type, "short") ||
                 !strcmp(base_type, "int"))) {
                token_t *after_base = cur_token->next->next;

                while (after_base && after_base->kind == T_const)
                    after_base = after_base->next;
                if (after_base && (after_base->kind == T_signed ||
                                   after_base->kind == T_unsigned)) {
                    lex_expect(T_identifier);
                    leading_scalar_type = find_type(base_type, true);
                }
            }

            /* Typedef declarations use the same freely ordered scalar specifier
             * set as object declarations. Keeping this in a loop admits C99
             * spellings such as `long unsigned long` rather than treating the
             * second specifier as the typedef name.
             */
            while (lex_peek(T_const, NULL) || lex_peek(T_signed, NULL) ||
                   lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL)) {
                if (lex_accept(T_const))
                    typedef_const = true;
                else if (lex_accept(T_signed)) {
                    if (is_signed)
                        error_at("duplicate signed type specifier",
                                 cur_token_loc());
                    is_signed = true;
                } else if (lex_accept(T_unsigned)) {
                    if (is_unsigned)
                        error_at("duplicate unsigned type specifier",
                                 cur_token_loc());
                    is_unsigned = true;
                } else {
                    lex_expect(T_long);
                    if (is_long_long)
                        error_at("too many long type specifiers",
                                 cur_token_loc());
                    if (is_long)
                        is_long_long = true;
                    else
                        is_long = true;
                }
            }
            if (is_signed && is_unsigned)
                error_at("both signed and unsigned specified", cur_token_loc());
            if (leading_scalar_type == TY_int &&
                lex_peek(T_identifier, base_type) && !strcmp(base_type, "char"))
                error_at("int cannot be combined with char", cur_token_loc());

            bool is_float = lex_accept(T_float);
            bool is_double = lex_accept(T_double);

            if (is_float) {
                if (is_signed || is_unsigned || is_long)
                    error_at("invalid float type specifiers", cur_token_loc());
                base = TY_float;
            } else if (is_double) {
                if (is_signed || is_unsigned || is_long_long)
                    error_at("invalid double type specifiers", cur_token_loc());
                base = is_long ? TY_long_double : TY_double;
            } else if (is_long) {
                if (lex_peek(T_identifier, base_type) &&
                    !strcmp(base_type, "int"))
                    lex_expect(T_identifier);
                if (is_long_long)
                    base = is_unsigned ? TY_ulong_long : TY_long_long;
                else
                    base = is_unsigned ? TY_ulong : TY_long;
            } else if (is_unsigned) {
                if (leading_scalar_type == TY_char) {
                    base = TY_uchar;
                } else if (leading_scalar_type == TY_short) {
                    if (lex_peek(T_identifier, base_type) &&
                        !strcmp(base_type, "int"))
                        lex_expect(T_identifier);
                    base = TY_ushort;
                } else {
                    if (lex_peek(T_identifier, base_type) &&
                        (!strcmp(base_type, "int") ||
                         !strcmp(base_type, "char") ||
                         !strcmp(base_type, "short"))) {
                        lex_expect(T_identifier);
                        if (!strcmp(base_type, "char"))
                            base = TY_uchar;
                        else if (!strcmp(base_type, "short"))
                            base = TY_ushort;
                        else
                            base = TY_uint;
                    } else {
                        base = TY_uint;
                    }
                }
            } else if (is_signed && lex_peek(T_identifier, base_type) &&
                       (!strcmp(base_type, "char") ||
                        !strcmp(base_type, "short") ||
                        !strcmp(base_type, "int"))) {
                lex_expect(T_identifier);
                base = !strcmp(base_type, "char")
                           ? TY_schar
                           : (!strcmp(base_type, "short") ? TY_short : TY_int);
            } else if (is_signed && (leading_scalar_type == TY_char ||
                                     leading_scalar_type == TY_short)) {
                if (leading_scalar_type == TY_short &&
                    lex_peek(T_identifier, base_type) &&
                    !strcmp(base_type, "int"))
                    lex_expect(T_identifier);
                base = leading_scalar_type == TY_char ? TY_schar
                                                      : leading_scalar_type;
            } else if (is_signed && (!lex_peek(T_identifier, base_type) ||
                                     (strcmp(base_type, "int") &&
                                      strcmp(base_type, "char") &&
                                      strcmp(base_type, "short")))) {
                base = TY_int;
            } else if (leading_scalar_type) {
                base = leading_scalar_type;
            } else {
                lex_ident(T_identifier, base_type);
                base = find_type(base_type, true);
            }
            if (!base)
                error_at("Unable to find base type", cur_token_loc());
            type->base_type = base->base_type;
            type->size = base->size;

            /* A typedef of a record typedef remains a record type. Sharing its
             * immutable member table preserves ordinary `alias.member` and
             * `pointer_alias->member` lookup instead of turning the alias into
             * a scalar descriptor with no fields.
             */
            type->fields = base->fields;
            type->num_fields = base->num_fields;
            type->base_struct = base->base_struct;
            if (base->base_type == TYPE_typedef && base->num_fields &&
                !type->base_struct)
                type->base_struct = (type_t *) base;
            type->alignment = base->alignment;
            type->is_union = base->is_union;
            type->has_flexible_array_member = base->has_flexible_array_member;
            type->ptr_level = base->ptr_level;
            type->pointer_const_mask = base->pointer_const_mask;
            type->is_const_qualified =
                typedef_const || base->is_const_qualified;
            type->is_unsigned = base->is_unsigned;
            type->is_floating = base->is_floating;
            type->is_signed_char = base->is_signed_char;
            type->is_bool = base->is_bool;
            type->array_size = base->array_size;
            type->array_dim2 = base->array_dim2;
            type->array_dim3 = base->array_dim3;
            type->array_dim4 = base->array_dim4;
            type->array_element_ptr_level = base->array_element_ptr_level;
            type->array_element_type = base->array_element_type;
            type->func_signature = base->func_signature;

            /* Handle pointer types in typedef: typedef char *string; */
            while (lex_accept(T_asterisk)) {
                type->ptr_level++;
                type->size = PTR_SIZE;
                while (true) {
                    if (lex_accept(T_const)) {
                        if (type->ptr_level <= 32)
                            type->pointer_const_mask |=
                                1U << (type->ptr_level - 1);
                    } else if (lex_accept(T_volatile) ||
                               lex_accept(T_restrict)) {
                        ;
                    } else
                        break;
                }
            }

            /* A parenthesized declarator is the function-pointer form: `typedef
             * int (*callback_t)(int)`. Parse it through the normal declarator
             * reader so its prototype has exactly the same shape as an object
             * declaration, then retain that syntax-only signature on the alias
             * for each later object or parameter declaration.
             */
            if (lex_peek(T_open_bracket, NULL)) {
                var_t declarator = {0};
                bool saved_sizeof_signature = parsing_sizeof_function_signature;

                declarator.type = (type_t *) base;
                declarator.scope = block;
                declarator.ptr_level = type->ptr_level;

                /* A callback typedef carries only a function signature. Its
                 * floating parameters do not materialize values until a call,
                 * which remains rejected by the ordinary floating gates.
                 */
                parsing_sizeof_function_signature = true;
                read_inner_var_decl(&declarator, false, false, false);
                parsing_sizeof_function_signature = saved_sizeof_signature;
                if (!declarator.is_func)
                    error_at(
                        "Typedef parenthesized declarator must be a function "
                        "pointer",
                        cur_token_loc());
                strncpy(type->type_name, declarator.var_name, MAX_TYPE_LEN - 1);
                type->type_name[MAX_TYPE_LEN - 1] = '\0';
                type->size = PTR_SIZE;
                type->alignment = PTR_SIZE;
                type->func_signature = declarator.func_signature;

                /* `typedef int (*row[2])(int)` is an array typedef whose
                 * elements are callback pointers. The signature describes each
                 * element, while the bounds are needed later when a
                 * pointer-to-row is indexed (including after a call result).
                 */
                type->array_size = declarator.array_size;
                type->array_dim2 = declarator.array_dim2;
                type->array_dim3 = declarator.array_dim3;
                type->array_dim4 = declarator.array_dim4;
                type->array_element_ptr_level = declarator.array_size ? 1 : 0;

                /* Stars before the parenthesized callback declarator belong to
                 * the callback's return type. That depth is retained in its
                 * parsed signature; the typedef alias itself is the
                 * pointer-sized callback object, not a derived pointer alias.
                 */
                type->ptr_level = 0;
                type->pointer_const_mask = declarator.pointer_const_mask;
                type->is_volatile_qualified = declarator.is_volatile;
                lex_expect(T_semicolon);
                return;
            }

            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);

            /* A typedef declarator may wrap an existing array typedef: `typedef
             * row matrix[2]`. Gather its leading bounds first, then prepend
             * them to the base alias's bounds instead of overwriting the inner
             * extent.
             */
            fixed_array_shape_t base_shape = fixed_array_shape_from_type(type);
            fixed_array_shape_t decl_shape = {0};
            while (lex_accept(T_open_square)) {
                int bound;

                if (decl_shape.rank >= MAX_FIXED_ARRAY_RANK)
                    error_at(
                        "Array declarators support at most four dimensions",
                        cur_token_loc());
                if (lex_peek(T_close_square, NULL))
                    error_at("Typedef array needs a positive bound",
                             cur_token_loc());
                bound = read_const_expr(block);
                if (bound <= 0)
                    error_at("Typedef array needs a positive bound",
                             cur_token_loc());
                decl_shape.bounds[decl_shape.rank++] = bound;
                lex_expect(T_close_square);
            }
            if (decl_shape.rank) {
                fixed_array_shape_t shape =
                    fixed_array_shape_prepend(&decl_shape, &base_shape);

                fixed_array_shape_to_type(type, &shape);

                /* At the point an array typedef is introduced, ptr_level
                 * describes each array element. A later alias may add a pointer
                 * to the whole array, so preserve this separately.
                 */
                type->array_element_ptr_level = type->ptr_level;
                type->array_element_type =
                    base->array_element_type
                        ? base->array_element_type
                        : (base->ptr_level ? pointee_type_from_pointer_typedef(
                                                 (type_t *) base)
                                           : (type_t *) base);
            }
            lex_expect(T_semicolon);
        }
    } else if (lex_peek(T_identifier, NULL) || lex_peek(T_signed, NULL) ||
               lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL)) {
        read_global_decl(block, is_const, is_static, is_extern, is_inline,
                         is_volatile);
    } else
        error_at("Syntax error in global statement", next_token_loc());
}

void parse_internal(void)
{
    /* set starting point of global stack manually */
    GLOBAL_FUNC = add_func("", true);

    /* The first global slot retains the synthetic global-frame pointer. It must
     * occupy a full target pointer, not the historic 32-bit word.
     */
    GLOBAL_FUNC->stack_size = PTR_SIZE;
    GLOBAL_FUNC->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
    GLOBAL_FUNC->bbs->belong_to = GLOBAL_FUNC; /* Prevent nullptr deref in RA */
    GLOBAL_FUNC->bbs->elf_offset = -1;         /* not yet emitted */

    /* built-in types */
    TY_void = add_named_type("void");
    TY_void->base_type = TYPE_void;
    TY_void->size = 0;

    TY_char = add_named_type("char");
    TY_char->base_type = TYPE_char;
    TY_char->size = 1;

    TY_schar = add_named_type("signed char");
    TY_schar->base_type = TYPE_char;
    TY_schar->size = 1;
    TY_schar->is_signed_char = true;

    TY_uchar = add_named_type("unsigned char");
    TY_uchar->base_type = TYPE_char;
    TY_uchar->size = 1;
    TY_uchar->is_unsigned = true;

    TY_int = add_named_type("int");
    TY_int->base_type = TYPE_int;
    TY_int->size = 4;

    TY_uint = add_named_type("unsigned int");
    TY_uint->base_type = TYPE_int;
    TY_uint->size = 4;
    TY_uint->is_unsigned = true;

    /* Keep C99 floating scalar identities in the type table before their IR and
     * ABI lowering are admitted. The LP64 targets use their ABI's 16-byte
     * long-double object slot; the current 32-bit soft-float targets use an
     * 8-byte long double until their target-specific representation is
     * implemented.
     */
    TY_float = add_named_type("float");
    TY_float->base_type = TYPE_float;
    TY_float->size = 4;
    TY_float->is_floating = true;

    TY_double = add_named_type("double");
    TY_double->base_type = TYPE_double;
    TY_double->size = 8;
    TY_double->is_floating = true;

    TY_long_double = add_named_type("long double");
    TY_long_double->base_type = TYPE_long_double;
    TY_long_double->size = PTR_SIZE == 8 ? 16 : 8;
    TY_long_double->is_floating = true;

    /* long has the same current ABI width as int, but it remains a distinct C
     * type: redeclarations and the usual arithmetic conversions depend on rank,
     * not just representation size.
     */
    TY_long = add_named_type("long");
    TY_long->base_type = TYPE_long;
    TY_long->size = 4;

    TY_ulong = add_named_type("unsigned long");
    TY_ulong->base_type = TYPE_long;
    TY_ulong->size = 4;
    TY_ulong->is_unsigned = true;

    TY_short = add_named_type("short");
    TY_short->base_type = TYPE_short;
    TY_short->size = 2;

    TY_ushort = add_named_type("unsigned short");
    TY_ushort->base_type = TYPE_short;
    TY_ushort->size = 2;
    TY_ushort->is_unsigned = true;

    /* Unlike `long`, which deliberately shares the current 32-bit int ABI, long
     * long has a distinct type and an eight-byte object representation. Parser
     * admission and target lowering are staged separately so 32-bit backends
     * never silently truncate it.
     */
    TY_long_long = add_named_type("long long");
    TY_long_long->base_type = TYPE_long_long;
    TY_long_long->size = 8;

    TY_ulong_long = add_named_type("unsigned long long");
    TY_ulong_long->base_type = TYPE_long_long;
    TY_ulong_long->size = 8;
    TY_ulong_long->is_unsigned = true;

    /* C99's <stddef.h> names target-sized integer typedefs. Keep them in the
     * builtin type table so declarations, casts, sizeof, and prototypes use the
     * same pointer-width representation as the rest of the compiler.
     */
    type_t *TY_size = add_named_type("size_t");
    TY_size->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_size->size = PTR_SIZE;
    TY_size->is_unsigned = true;

    type_t *TY_ptrdiff = add_named_type("ptrdiff_t");
    TY_ptrdiff->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_ptrdiff->size = PTR_SIZE;

    /* <stdarg.h> belongs to C99's freestanding library. Its va_list is the
     * compiler ABI's int-based cursor, so retain that scalar base while
     * recording the pointer depth directly in the builtin typedef.
     */
    type_t *TY_va_list = add_named_type("va_list");
    TY_va_list->base_type = TYPE_int;
    TY_va_list->ptr_level = 1;
    TY_va_list->size = PTR_SIZE;

    /* The execution wide-character type is int until wide literal lowering is
     * implemented; declaring the C99 typedef remains useful independently.
     */
    type_t *TY_wchar = add_named_type("wchar_t");
    TY_wchar->base_type = TYPE_int;
    TY_wchar->size = TY_int->size;

    type_t *TY_sig_atomic = add_named_type("sig_atomic_t");
    TY_sig_atomic->base_type = TYPE_int;
    TY_sig_atomic->size = TY_int->size;

    type_t *TY_wint = add_named_type("wint_t");
    TY_wint->base_type = TYPE_int;
    TY_wint->size = TY_int->size;
    TY_wint->is_unsigned = true;

    /* C99 <stdint.h> aliases share the target scalar representations. Keep them
     * named in the builtin table so declarations, casts, sizeof, and prototypes
     * use the same ABI metadata as their underlying types.
     */
    type_t *TY_int8 = add_named_type("int8_t");
    TY_int8->base_type = TYPE_char;
    TY_int8->size = 1;
    TY_int8->is_signed_char = true;
    type_t *TY_uint8 = add_named_type("uint8_t");
    TY_uint8->base_type = TYPE_char;
    TY_uint8->size = 1;
    TY_uint8->is_unsigned = true;
    type_t *TY_int16 = add_named_type("int16_t");
    TY_int16->base_type = TYPE_short;
    TY_int16->size = 2;
    type_t *TY_uint16 = add_named_type("uint16_t");
    TY_uint16->base_type = TYPE_short;
    TY_uint16->size = 2;
    TY_uint16->is_unsigned = true;
    type_t *TY_int32 = add_named_type("int32_t");
    TY_int32->base_type = TYPE_int;
    TY_int32->size = 4;
    type_t *TY_uint32 = add_named_type("uint32_t");
    TY_uint32->base_type = TYPE_int;
    TY_uint32->size = 4;
    TY_uint32->is_unsigned = true;
    type_t *TY_int64 = add_named_type("int64_t");
    TY_int64->base_type = TYPE_long_long;
    TY_int64->size = 8;
    type_t *TY_uint64 = add_named_type("uint64_t");
    TY_uint64->base_type = TYPE_long_long;
    TY_uint64->size = 8;
    TY_uint64->is_unsigned = true;
    type_t *TY_intmax = add_named_type("intmax_t");
    TY_intmax->base_type = TYPE_long_long;
    TY_intmax->size = 8;
    type_t *TY_uintmax = add_named_type("uintmax_t");
    TY_uintmax->base_type = TYPE_long_long;
    TY_uintmax->size = 8;
    TY_uintmax->is_unsigned = true;
    type_t *TY_intptr = add_named_type("intptr_t");
    TY_intptr->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_intptr->size = PTR_SIZE;
    type_t *TY_uintptr = add_named_type("uintptr_t");
    TY_uintptr->base_type = PTR_SIZE == 8 ? TYPE_long_long : TYPE_long;
    TY_uintptr->size = PTR_SIZE;
    TY_uintptr->is_unsigned = true;

    type_t *TY_int_least8 = add_named_type("int_least8_t");
    TY_int_least8->base_type = TY_int8->base_type;
    TY_int_least8->size = TY_int8->size;
    TY_int_least8->is_signed_char = true;
    type_t *TY_uint_least8 = add_named_type("uint_least8_t");
    TY_uint_least8->base_type = TY_uint8->base_type;
    TY_uint_least8->size = TY_uint8->size;
    TY_uint_least8->is_unsigned = true;
    type_t *TY_int_least16 = add_named_type("int_least16_t");
    TY_int_least16->base_type = TYPE_short;
    TY_int_least16->size = 2;
    type_t *TY_uint_least16 = add_named_type("uint_least16_t");
    TY_uint_least16->base_type = TYPE_short;
    TY_uint_least16->size = 2;
    TY_uint_least16->is_unsigned = true;
    type_t *TY_int_least32 = add_named_type("int_least32_t");
    TY_int_least32->base_type = TYPE_int;
    TY_int_least32->size = 4;
    type_t *TY_uint_least32 = add_named_type("uint_least32_t");
    TY_uint_least32->base_type = TYPE_int;
    TY_uint_least32->size = 4;
    TY_uint_least32->is_unsigned = true;
    type_t *TY_int_least64 = add_named_type("int_least64_t");
    TY_int_least64->base_type = TYPE_long_long;
    TY_int_least64->size = 8;
    type_t *TY_uint_least64 = add_named_type("uint_least64_t");
    TY_uint_least64->base_type = TYPE_long_long;
    TY_uint_least64->size = 8;
    TY_uint_least64->is_unsigned = true;
    type_t *TY_int_fast8 = add_named_type("int_fast8_t");
    TY_int_fast8->base_type = TYPE_int;
    TY_int_fast8->size = 4;
    type_t *TY_uint_fast8 = add_named_type("uint_fast8_t");
    TY_uint_fast8->base_type = TYPE_int;
    TY_uint_fast8->size = 4;
    TY_uint_fast8->is_unsigned = true;
    type_t *TY_int_fast16 = add_named_type("int_fast16_t");
    TY_int_fast16->base_type = TYPE_int;
    TY_int_fast16->size = 4;
    type_t *TY_uint_fast16 = add_named_type("uint_fast16_t");
    TY_uint_fast16->base_type = TYPE_int;
    TY_uint_fast16->size = 4;
    TY_uint_fast16->is_unsigned = true;
    type_t *TY_int_fast32 = add_named_type("int_fast32_t");
    TY_int_fast32->base_type = TYPE_int;
    TY_int_fast32->size = 4;
    type_t *TY_uint_fast32 = add_named_type("uint_fast32_t");
    TY_uint_fast32->base_type = TYPE_int;
    TY_uint_fast32->size = 4;
    TY_uint_fast32->is_unsigned = true;
    type_t *TY_int_fast64 = add_named_type("int_fast64_t");
    TY_int_fast64->base_type = TYPE_long_long;
    TY_int_fast64->size = 8;
    type_t *TY_uint_fast64 = add_named_type("uint_fast64_t");
    TY_uint_fast64->base_type = TYPE_long_long;
    TY_uint_fast64->size = 8;
    TY_uint_fast64->is_unsigned = true;

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;
    TY_bool->is_bool = true;

    GLOBAL_BLOCK = add_block(NULL, NULL); /* global block */
    elf_add_symbol("", 0);                /* undef symbol */

    if (dynlink) {
        /* In dynamic mode, __syscall won't be implemented.
         *
         * Simply declare a 'syscall' function as follows if the program needs
         * to use 'syscall':
         *
         * int syscall(int number, ...);
         *
         * shecc will treat it as an external function, and the compiled program
         * will eventually use the implementation provided by the external C
         * library.
         *
         * If shecc supports the 'long' data type in the future, it would be
         * better to declare syscall using its original prototype:
         *
         * long syscall(long number, ...);
         */
    } else {
        /* Linux syscall */
        func_t *func = add_func("__syscall", true);
        func->return_def.type = TY_int;
        func->num_params = 0;
        func->va_args = 1;
        func->bbs = NULL;
        /* Otherwise, allocate a basic block to implement in static mode. */
        func->bbs = arena_calloc(BB_ARENA, 1, sizeof(basic_block_t));
        func->bbs->elf_offset = -1; /* not yet emitted */
    }

    /* Add a global object to the .data section.
     *
     * This object saves the global stack pointer, so it is written back as a
     * pointer and must reserve a full one: on an LP64 target the historic
     * 32-bit word left four bytes belonging to the next global.
     */
    elf_write_ptr(elf_data, 0);

    /* lexer initialization */
    do {
        read_global_statement();
    } while (!lex_accept(T_eof));

    /* Aggregate returns use shecc's internal destination-pointer convention,
     * not the platform ABI's aggregate classification. A direct call to a
     * declaration-only function would otherwise quietly cross that boundary
     * with incompatible arguments. Indirect calls separately require tracked
     * provenance proving that their target is shecc-defined.
     */
    for (func_t *func = FUNC_LIST.head; func; func = func->next)
        if (func->aggregate_call_used && !func->bbs)
            error_at("aggregate-return call requires a shecc-defined function",
                     cur_token_loc());
}

void parse(token_t *tk)
{
    token_t head;
    head.kind = T_start;
    head.next = tk;
    cur_token = &head;

    parse_internal();
}
