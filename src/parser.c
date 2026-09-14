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
static bool fixed_array_shape_drop_outer(fixed_array_shape_t *shape);

/* Elements in one outer element of @var's direct array. Only the inner bounds
 * matter, which is what keeps this right while an unsized outer bound is still
 * being inferred from its initializer and array_size is not yet known.
 */
static int fixed_array_inner_count(const var_t *var)
{
    int count = 1;

    if (var->array_dim2)
        count *= var->array_dim2;
    if (var->array_dim3)
        count *= var->array_dim3;
    if (var->array_dim4)
        count *= var->array_dim4;
    return count;
}

/* Rewrite @var's direct array bounds to those of one element of the array: a
 * matrix becomes a row, a row becomes a scalar.
 */
static void fixed_array_var_drop_outer(var_t *var)
{
    fixed_array_shape_t shape = fixed_array_shape_from_var(var);

    fixed_array_shape_drop_outer(&shape);
    fixed_array_shape_to_var(var, &shape);
}

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
bool is_record_type(const type_t *type);
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
void parse_array_init(var_t *var, block_t *parent, basic_block_t **bb);
void parse_string_array_init(var_t *var, block_t *parent, basic_block_t **bb);
void parse_wstring_array_init(var_t *var, block_t *parent, basic_block_t **bb);
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
var_t *read_bitfield_value(block_t *parent,
                           basic_block_t **bb,
                           var_t *address,
                           const var_t *field);
void mark_value_reference(var_t *value, var_t *address, var_t *bitfield);
void push_object_at(block_t *parent,
                    basic_block_t **bb,
                    var_t *value,
                    var_t *address,
                    int size);
var_t *lower_reference_update(var_t *object,
                              opcode_t op,
                              block_t *parent,
                              basic_block_t **bb);
void lower_member_postfix(var_t *address,
                          type_t *record_type,
                          bool arrow_first,
                          bool is_lvalue,
                          block_t *parent,
                          basic_block_t **bb);
void lower_postfix_operators(block_t *parent, basic_block_t **bb);
void push_dereference(block_t *parent, basic_block_t **bb, var_t *rs1);
bool is_swapped_subscript_base(const var_t *var);
void reject_record_operand(const var_t *var);
void mark_var_mutated(var_t *var);

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

/* Accept a struct or union keyword.
 *
 * Return the kind of record it names, or TYPE_void, which is zero, when neither
 * keyword comes next.
 */
base_type_t accept_record_keyword(void)
{
    if (lex_accept(T_struct))
        return TYPE_struct;
    return lex_accept(T_union) ? TYPE_union : TYPE_void;
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
    if (!next ||
        !((next->kind == T_identifier && find_type(next->literal, true)) ||
          next->kind == T_struct || next->kind == T_union ||
          next->kind == T_signed || next->kind == T_unsigned ||
          next->kind == T_long))
        return false;

    /* Only a brace after the type name makes a compound literal; otherwise the
     * parenthesized type name is a cast.
     */
    for (int depth = 1; next; next = next->next) {
        if (next->kind == T_open_bracket)
            depth++;
        else if (next->kind == T_close_bracket && --depth == 0)
            return next->next && next->next->kind == T_open_curly;
    }
    return false;
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

/* The record a typedef names through base_struct, or @type itself. Unlike
 * is_plain_record_alias() below this does not look at the typedef's own
 * declarator, so callers apply it once they know they have a record object.
 */
type_t *resolve_record_type(type_t *type)
{
    if (type->base_type == TYPE_typedef && type->base_struct)
        return type->base_struct;
    return type;
}

/* A typedef naming a struct or union type with no derived declarator of its
 * own, as in "typedef volatile struct S vs_t;".
 */
static bool is_plain_record_alias(const type_t *type)
{
    return type && type->base_type == TYPE_typedef && type->base_struct &&
           !type->ptr_level && !type->array_size && !type->pointee_array_size &&
           !type->func_signature && !type->pointee_func_signature;
}

bool compatible_decl_type(const type_t *left, const type_t *right)
{
    /* A typedef of a struct or union tag denotes the tag's type. Its own
     * qualifiers were copied into each declarator, which the callers compare.
     */
    while (is_plain_record_alias(left))
        left = left->base_struct;
    while (is_plain_record_alias(right))
        right = right->base_struct;
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
 * The record the alias reaches through base_struct has the padded size, which
 * the end of the last member falls short of.
 */
int pointer_typedef_pointee_size(type_t *type, type_t *pointee)
{
    if (!type || !type->ptr_level || type->base_type != TYPE_typedef ||
        !type->num_fields)
        return pointee == TY_void ? 1 : pointee->size;
    if (type->ptr_level == 1 && type->base_struct)
        return type->base_struct->size;

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
    unsigned int value;
    unsigned int value_hi;
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

/* A _Bool object, reached directly or through a typedef. A typedef of a pointer
 * to _Bool or of a _Bool array keeps is_bool but is not itself boolean.
 */
bool is_bool_scalar(const type_t *type, int ptr_level)
{
    return is_bool_type(type) && !ptr_level && !type->ptr_level &&
           !type->array_size;
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

bool is_record_type(const type_t *type);

/* Pop a value whose expression was evaluated only for its side effects: an
 * expression statement, the left operand of a comma, or a for clause.
 *
 * A named object stands on the operand stack for its own value, and nothing
 * reads it until something uses that value. Reading a volatile object is a side
 * effect in itself, though (C99 5.1.2.3p2, 6.7.3p6), so "status;" still owes
 * the object one read. A copy into a temporary is that read.
 *
 * Only an object no instruction has read since the expression named it is owed
 * one: "x = status;" leaves status on the stack as the assignment's value after
 * the assignment has read it.
 */
void discard_operand(block_t *parent, basic_block_t *bb)
{
    var_t *var = opstack_pop();
    bool unread = var && var == unread_volatile_object;

    unread_volatile_object = NULL;
    if (!unread || var->is_func || var->array_size ||
        (!var->ptr_level && is_record_type(var->type)))
        return;

    var_t *copy = require_typed_ptr_var(parent, var->type, var->ptr_level);
    copy->var_name = gen_name();
    add_insn(parent, bb, OP_assign, copy, var, NULL, 0, NULL);
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

/* Write a decoded string literal of @length bytes, which may include an
 * embedded null character, followed by its terminating null character.
 */
int write_string_symbol(const char *data, int length)
{
    const int start_len = elf_rodata->size;

    elf_write_blk(elf_rodata, data, length);
    elf_write_byte(elf_rodata, 0);
    return start_len;
}

/* Write the null-terminated string @data to .rodata. */
int write_symbol(const char *data)
{
    return write_string_symbol(data, strlen(data));
}

int write_wide_symbol(const int *data, int length)
{
    int start_len = elf_rodata->size;

    for (int i = 0; i < length; i++)
        elf_write_int(elf_rodata, data[i]);
    elf_write_int(elf_rodata, 0);
    return start_len;
}

/* The value of a wide character constant comes from wide_character_constant();
 * report one whose escape does not fit an execution-wide unit.
 */
int parse_wide_character_constant(const char *literal)
{
    int value;

    if (!wide_character_constant(literal, &value))
        error_at("Invalid wide character escape sequence", cur_token_loc());
    return value;
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

    if (is_bool_scalar(var->type, var->ptr_level))
        return var;
    if (var->is_const && !var->ptr_level) {
        rd = require_typed_var(block, TY_bool);
        rd->var_name = gen_name();
        rd->init_val = var->init_val || var->init_val_hi;
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

/* The value an OP_write of a scalar object of @type should store. A store
 * narrows to the object width by itself, which is the conversion C wants for
 * every integer type except _Bool: 0x100 has to become 1, not its low byte 0.
 * Paths that write a plain assignment through an address share this rather than
 * each resizing their value.
 */
var_t *convert_stored_value(block_t *block,
                            basic_block_t **bb,
                            var_t *value,
                            type_t *type,
                            int ptr_level)
{
    if (!is_bool_scalar(type, ptr_level))
        return value;
    return normalize_bool(block, bb, value);
}

var_t *resize_var(block_t *block, basic_block_t **bb, var_t *from, var_t *to)
{
    /* A function designator and a function pointer are addresses too, whatever
     * the return type their signature spells: a bool (*)(int) argument must not
     * be converted to _Bool.
     */
    bool is_from_ptr = from->ptr_level || from->array_size || from->is_func ||
                       from->func_signature,
         is_to_ptr = to->ptr_level || to->array_size || to->is_func ||
                     to->func_signature ||
                     (to->type && to->type->ptr_level > 0);

    if (is_from_ptr && is_to_ptr)
        return from;

    if (!is_to_ptr && is_bool_scalar(to->type, 0))
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
     *
     * A change of signedness needs the conversion on every target, because the
     * result is also the value of an assignment expression. Returning the
     * source unchanged kept its type: `(long long)(i = u)` zero-extended a
     * negative int, and `(long long)(u = -5)` sign-extended an unsigned one. On
     * a 32-bit target the same-width truncation is a plain move.
     */
    if (!is_from_ptr && !is_to_ptr && to->type &&
        ((to->type->is_unsigned && to_size < PTR_SIZE) ||
         (from->type && to_size <= TY_int->size &&
          to->type->is_unsigned != from->type->is_unsigned)))
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

/* C99 6.5.16.1p1 lets an integer become a pointer without a cast only when it
 * is a null pointer constant. @to is the pointer object, parameter or return
 * type receiving @from; with @array_is_pointer an array declarator there is the
 * pointer that a parameter declared as an array is adjusted to.
 */
void diagnose_integer_to_pointer_conversion(var_t *from,
                                            const var_t *to,
                                            bool array_is_pointer)
{
    if (!from || !to || !from->type || !to->type)
        return;
    if (!effective_pointer_depth(to) &&
        !(array_is_pointer && (to->array_size || to->has_unsized_array)))
        return;
    if (!array_is_pointer && (to->array_size || to->has_unsized_array))
        return;
    if (effective_pointer_depth(from) || from->array_size ||
        from->has_unsized_array || from->is_func || from->func_signature ||
        from->pointee_func_signature || from->is_string_literal ||
        from->type == TY_void || is_record_type(from->type) ||
        is_null_pointer_constant(from))
        return;
    error_at("integer converted to pointer without a cast", cur_token_loc());
}

/* The function type @var points to, when @var is a function pointer object or
 * value or a function designator; NULL otherwise. Callback arrays and slots,
 * pointers to function pointers, are not function pointers here.
 */
static const func_t *pointed_function_type(const var_t *var)
{
    if (!var || !var->type || var->pointee_func_signature || var->array_size ||
        var->has_unsized_array)
        return NULL;
    if (var->func_signature)
        return var->func_signature;
    if (var->type->func_signature && !var->type->is_direct_function_type &&
        !var->ptr_level)
        return var->type->func_signature;
    if (var->is_func && var->var_name)
        return find_func(var->var_name);
    return NULL;
}

/* C99 6.5.16.1p1 converts between function pointers only when the functions
 * have compatible types. A function without a prototype is compatible with a
 * prototype that has no ellipsis and no parameter the default argument
 * promotions change (6.7.5.3p15).
 */
static bool compatible_function_conversion(const func_t *from, const func_t *to)
{
    const func_t *prototyped = from->has_prototype ? from : to;

    if (!compatible_function_signature(from, to))
        return false;
    if (from->has_prototype == to->has_prototype)
        return true;
    if (prototyped->va_args)
        return false;
    for (int i = 0; i < prototyped->num_params; i++)
        if (parameter_changes_under_default_promotion(
                &prototyped->param_defs[i]))
            return false;
    return true;
}

/* Diagnose a conversion of @from to the function pointer @to, or of a function
 * pointer @from to the object pointer @to, in an initializer, assignment,
 * argument or return. Only a null pointer constant converts to a function
 * pointer from anything but a compatible function pointer or designator.
 */
void diagnose_function_pointer_conversion(var_t *from, const var_t *to)
{
    const func_t *from_function = pointed_function_type(from);
    const func_t *to_function = pointed_function_type(to);

    if (!from || !to || !from->type || !to->type ||
        from->pointee_func_signature || to->pointee_func_signature)
        return;
    if (to_function && from_function) {
        if (!compatible_function_conversion(from_function, to_function))
            error_at("incompatible function pointer types", cur_token_loc());
        return;
    }
    if (to_function) {
        if (is_null_pointer_constant(from) || from->is_void_null_pointer ||
            from->array_size || from->has_unsized_array)
            return;
        if (effective_pointer_depth(from))
            error_at("incompatible function pointer types", cur_token_loc());
        if (!is_record_type(from->type) && from->type != TY_void)
            error_at("integer converted to pointer without a cast",
                     cur_token_loc());
        return;
    }
    if (from_function && effective_pointer_depth(to) && !to->array_size &&
        !to->has_unsized_array)
        error_at("incompatible function pointer types", cur_token_loc());
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

    /* A value cast to a callback typedef, `(callback_t) f`, carries its own
     * signature: it is the callback, not a pointer to a callback slot.
     */
    if (!from_signature && !from->func_signature &&
        effective_pointer_depth(from) > 0 && from->type &&
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

/* The rest of the parser, in the order it was written. Each file depends on
 * what the ones before it define, so the order below is load-bearing and must
 * not be sorted.
 */
/* clang-format off */
#include "parser-init.c"
#include "parser-decl.c"
#include "parser-call.c"
#include "parser-sizeof.c"
#include "parser-expr.c"
#include "parser-const.c"
#include "parser-stmt.c"
#include "parser-global.c"
/* clang-format on */
