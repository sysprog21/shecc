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

/* Forward declarations */
source_location_t *cur_token_loc(void);
source_location_t *next_token_loc(void);

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb);
void perform_side_effect(block_t *parent, basic_block_t *bb);
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

bool global_compound_literal_starts_here(void)
{
    token_t *next;

    if (!lex_peek(T_open_bracket, NULL))
        return false;
    next = cur_token->next->next;
    return next && (next->kind == T_identifier || next->kind == T_struct ||
                    next->kind == T_union);
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
    default:
        return type;
    }
}

/* Scalar typedefs have their own descriptor, but C declaration compatibility is
 * based on the represented type. Records retain nominal identity.
 */
bool compatible_decl_type(const type_t *left, const type_t *right)
{
    if (left == right)
        return true;
    if (!left || !right || left->base_type != right->base_type ||
        left->size != right->size || left->ptr_level != right->ptr_level ||
        left->is_unsigned != right->is_unsigned ||
        left->is_signed_char != right->is_signed_char)
        return false;
    if (left->base_type == TYPE_struct || left->base_type == TYPE_union)
        return false;
    if (left->base_type == TYPE_typedef &&
        (left->num_fields || right->num_fields))
        return left->base_struct == right->base_struct;
    return true;
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

void read_parameter_list_decl(func_t *func, bool anon);
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
    } else {
        src = resize_var(parent, bb, src, dest);
        add_insn(parent, *bb, OP_assign, dest, src, NULL, 0, NULL);
    }
}

var_t *parse_global_constant_value(block_t *parent, basic_block_t **bb)
{
    var_t *val = NULL;

    if (lex_peek(T_numeric, NULL) || lex_peek(T_minus, NULL)) {
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
    } else if (lex_peek(T_char, NULL)) {
        char chtok[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN];
        lex_ident(T_char, chtok);
        unescape_string(chtok, unescaped, MAX_TOKEN_LEN);

        val = require_typed_var(parent, TY_int);
        val->var_name = gen_name();
        val->init_val = parse_character_constant(chtok);
        add_insn(parent, *bb, OP_load_constant, val, NULL, NULL, 0, NULL);
    } else if (lex_peek(T_string, NULL)) {
        lex_accept(T_string);

        /* TODO: String fields in structs not yet supported - requires proper
         * handling of string literals as initializers
         */
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
    } else if (lex_peek(T_char, NULL)) {
        lex_accept(T_char);
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

void parse_array_field_row_init(block_t *parent,
                                basic_block_t **bb,
                                const var_t *field,
                                var_t *target_addr,
                                int start,
                                bool emit_code)
{
    int count = 0;
    int elem_size = field->ptr_level ? PTR_SIZE : field->type->size;

    lex_expect(T_open_curly);
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
            var_t *stored =
                resize_to(parent, bb, value, field->type, field->ptr_level);
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
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN],
        combined[MAX_LINE_LEN];
    int len;

    lex_ident(T_string, literal);
    unescape_string(literal, combined, MAX_LINE_LEN);
    while (lex_peek(T_string, NULL)) {
        int used = strlen(combined);

        lex_ident(T_string, literal);
        unescape_string(literal, unescaped, MAX_LINE_LEN - used);
        if (used + (int) strlen(unescaped) >= MAX_LINE_LEN - 1)
            error_at("Concatenated string literal too long", cur_token_loc());
        strcpy(combined + used, unescaped);
    }

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

void parse_array_field_init(block_t *parent,
                            basic_block_t **bb,
                            const var_t *field,
                            var_t *target_addr,
                            bool emit_code)
{
    int count = 0;
    int elem_size = field->ptr_level ? PTR_SIZE : field->type->size;

    lex_expect(T_open_curly);
    while (!lex_peek(T_close_curly, NULL)) {
        var_t *value = NULL;

        if (count >= field->array_size)
            error_at("Too many elements in array initializer",
                     next_token_loc());

        var_t *elem_addr =
            compute_element_address(parent, bb, target_addr, count, elem_size);
        if (field->array_dim2 && lex_peek(T_open_curly, NULL)) {
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
            var_t *stored =
                resize_to(parent, bb, value, field->type, field->ptr_level);
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
    var_t pending_array_element;
    var_t *pending_array_base = NULL;
    int pending_array_index = 0;
    int pending_array_count = 0;
    int pending_array_elem_size = 0;
    bool has_pending_array_element = false;

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
                var_t array_element;
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
                        int index, element_count, linear_index;
                        int total_element_count;
                        int elem_size;
                        bool direct_array_member = first;
                        var_t *array_base_addr = designated_field_addr;

                        if (!field->array_size)
                            error_at(
                                "Array designator requires an array member",
                                cur_token_loc());
                        index = read_const_expr(parent);
                        lex_expect(T_close_square);
                        total_element_count = field->array_size;
                        element_count = total_element_count;
                        if (field->array_dim2)
                            element_count /= field->array_dim2;
                        if (index < 0 || index >= element_count)
                            error_at("Array designator index is out of bounds",
                                     cur_token_loc());
                        elem_size =
                            field->ptr_level ? PTR_SIZE : field->type->size;
                        if (field->array_dim2)
                            index *= field->array_dim2;
                        linear_index = index;
                        designated_field_addr = compute_element_address(
                            parent, bb, designated_field_addr, index,
                            elem_size);
                        memcpy(&array_element, field, sizeof(var_t));
                        array_element.array_size = field->array_dim2;
                        array_element.array_dim2 = 0;
                        field = &array_element;

                        /* A first subscript of a two-dimensional member names a
                         * row; a second selects its scalar element. Leaving it
                         * as an array supports `.cells[row] = {...}`.
                         */
                        if (field->array_size && lex_accept(T_open_square)) {
                            index = read_const_expr(parent);
                            lex_expect(T_close_square);
                            if (index < 0 || index >= field->array_size)
                                error_at(
                                    "Array designator index is out of bounds",
                                    cur_token_loc());
                            designated_field_addr = compute_element_address(
                                parent, bb, designated_field_addr, index,
                                elem_size);
                            linear_index += index;
                            array_element.array_size = 0;
                        }
                        if (direct_array_member && !field->array_size) {
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

            if (field_idx >= struct_type->num_fields ||
                ((struct_type->base_type == TYPE_union ||
                  struct_type->is_union) &&
                 initializer_count > 0))
                error_at("Too many elements in record initializer",
                         next_token_loc());

            if (!field && field_idx < struct_type->num_fields)
                field = &struct_type->fields[field_idx];

            if (field && field->array_size && !field->ptr_level &&
                field->type == TY_char && lex_peek(T_string, NULL)) {
                var_t *field_addr =
                    designated_field_addr
                        ? designated_field_addr
                        : compute_field_address(parent, bb, target_addr, field);
                parse_string_field_init(parent, bb, field, field_addr,
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
                } else {
                    var_t *field_val = resize_to(parent, bb, field_val_raw,
                                                 field->type, field->ptr_level);
                    int field_size = size_var(field);
                    add_insn(parent, *bb, OP_write, NULL, field_addr, field_val,
                             field_size, NULL);
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
        add_insn(parent, bb, OP_return, NULL, NULL, NULL, 0, NULL);
        bb_connect(bb, parent->func->exit, NEXT);
        return NULL;
    }

    read_expr(parent, &bb);
    read_ternary_operation(parent, &bb);
    lex_expect(T_semicolon);

    var_t *rs1 = opstack_pop();

    /* Handle array compound literals in return context. Convert array compound
     * literals to their first element value.
     */
    if (rs1 && rs1->array_size > 0 && rs1->var_name[0] == '.') {
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

    /* A return expression is converted to the function's declared type just
     * like an assignment. This is particularly important for _Bool: a pointer
     * return value must become 0 or 1 before it crosses the ABI boundary,
     * rather than leaving an address in the low return byte.
     */
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
    read_expr(parent, &bb);
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
    read_expr(parent, &bb);
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
    bool is_implicit = (var->array_size == 0);
    block_t *initializer_scope = parent;

    if (parent == GLOBAL_BLOCK && var->scope && var->scope != GLOBAL_BLOCK)
        initializer_scope = var->scope;

    /* Elements of a pointer array are pointer-sized. Using the base type's
     * width strided "char *a[2] = {...}" by one byte, so every element but the
     * first got a bogus address. An implicit-size array reaches this with
     * ptr_level set as a marker rather than as a real pointer type, so only an
     * explicitly sized array is treated this way.
     */
    int elem_size = var->type->size;
    if (!is_implicit && var->ptr_level > 0)
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
    if (!lex_peek(T_close_curly, NULL)) {
        for (;;) {
            var_t *val = NULL;
            int prior_count = count;

            if (lex_accept(T_open_square)) {
                count = read_const_expr(initializer_scope);
                lex_expect(T_close_square);
                lex_expect(T_assign);
            }

            if (!is_implicit && count >= var->array_size)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (lex_peek(T_open_curly, NULL) && is_record_type(var->type)) {
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
                    initializer_scope != GLOBAL_BLOCK &&
                    !lex_peek(T_string, NULL)) {
                    /* Storage for a block-scope static lives globally, while
                     * its initializer is an integer constant expression in the
                     * surrounding block. Resolve local enumerators before
                     * emitting the global setup-store value.
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

                        if (!lex_peek(T_numeric, NULL) &&
                            !lex_peek(T_minus, NULL) &&
                            !lex_peek(T_string, NULL) &&
                            !lex_peek(T_char, NULL) && !enum_constant)
                            error_at(
                                "Global array initialization requires constant "
                                "values",
                                next_token_loc());
                    }

                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    val = opstack_pop();
                }
            }

            if (is_implicit && count >= MAX_IMPLICIT_ARRAY)
                error_at("Too many elements in array initializer",
                         next_token_loc());

            if (val && emit_code && (is_implicit || count < var->array_size)) {
                var_t *v = resize_to(parent, bb, val, var->type, 0);

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
        if (var->ptr_level > 0)
            var->ptr_level = 0;
        var->array_size = inferred_size;
        var->has_unsized_array = false;
    }
}

void parse_array_compound_literal(var_t *var,
                                  block_t *parent,
                                  basic_block_t **bb)
{
    int elem_size = var->type->size;
    int count = 0;

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
            } else if (lex_accept(T_const) || lex_accept(T_volatile) ||
                       lex_accept(T_restrict)) {
                ;
            } else if (lex_peek(T_identifier, token)) {
                type_t *candidate = find_type(token, true);

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
        if (long_count) {
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
    lex_expect(T_close_bracket);
    if (ptr_level)
        return PTR_SIZE;
    if (type->size)
        return type->size;
    return type->base_struct->size;
}

int read_const_expr_operand(block_t *scope)
{
    char buffer[MAX_TOKEN_LEN];

    if (lex_accept(T_minus))
        return -read_const_expr_operand(scope);
    if (lex_accept(T_plus))
        return read_const_expr_operand(scope);
    if (lex_accept(T_bit_not))
        return ~read_const_expr_operand(scope);
    if (lex_accept(T_log_not))
        return !read_const_expr_operand(scope);
    if (lex_accept(T_sizeof))
        return read_const_sizeof_type(scope);

    if (lex_accept(T_open_bracket)) {
        int res = read_const_expr(scope);
        lex_expect(T_close_bracket);
        return res;
    }
    if (lex_peek(T_numeric, buffer)) {
        lex_expect(T_numeric);
        return parse_numeric_constant(buffer);
    }
    if (lex_peek(T_char, buffer)) {
        char unescaped[MAX_TOKEN_LEN];
        lex_expect(T_char);
        if (unescape_string(buffer, unescaped, MAX_TOKEN_LEN) < 0)
            error_at("Invalid escape sequence", cur_token_loc());
        return parse_character_constant(buffer);
    }
    if (lex_peek(T_identifier, buffer)) {
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
            int then_val = read_const_expr(scope);
            lex_expect(T_colon);
            int else_val = read_const_expr(scope);
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

    /* is it function pointer declaration? */
    if (lex_accept(T_open_bracket)) {
        func_t *func = arena_alloc_func();
        char temp_name[MAX_VAR_LEN];
        lex_expect(T_asterisk);
        while (true) {
            if (lex_accept(T_const))
                vd->is_const_pointer = true;
            else if (lex_accept(T_volatile))
                vd->is_volatile = true;
            else if (lex_accept(T_restrict))
                ; /* restrict is an aliasing contract, not storage state. */
            else
                break;
        }
        lex_ident(T_identifier, temp_name);
        vd->var_name = intern_string(temp_name);
        lex_expect(T_close_bracket);

        /* The return declaration was parsed before the parenthesized
         * declarator. Copy it into the syntax-only signature before the
         * function-pointer marker is set on vd.
         */
        memcpy(&func->return_def, vd, sizeof(var_t));
        read_parameter_list_decl(func, true);
        vd->func_signature = func;
        vd->is_func = true;
    } else {
        if (!anon) {
            char temp_name[MAX_VAR_LEN];
            lex_ident(T_identifier, temp_name);
            vd->var_name = intern_string(temp_name);
            if (!lex_peek(T_open_bracket, NULL) && !is_param) {
                if (vd->is_global) {
                    opstack_push(vd);
                }
            }
        }
        if (!lex_peek(T_open_square, NULL)) {
            vd->array_size = 0;
            vd->array_dim2 = 0;
        }

        /* Every dimension multiplies into array_size, so "int matrix[3][4]"
         * becomes an array of 12 elements. The second dimension is kept
         * separately as well, because indexing needs the row length; further
         * dimensions only contribute to the total. A dimension left empty
         * contributes no size.
         */
        bool first_dim_empty = false;
        int dims = 0;

        for (int dim = 0; lex_accept(T_open_square); dim++) {
            if (lex_peek(T_close_square, NULL)) {
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

                if (dim == 0) {
                    vd->array_size = next_dim;
                } else {
                    if (dim == 1)
                        vd->array_dim2 = next_dim;
                    if (vd->array_size > 0)
                        vd->array_size *= next_dim;
                    else
                        vd->array_size = next_dim;
                }
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
        } else if (first_dim_empty && dims == 1) {
            /* An array parameter adjusts to a pointer, but an object declarator
             * such as `char text[] = "hi"` has an inferred array bound. Keeping
             * those cases distinct lets its initializer create writable array
             * storage instead of a pointer to string data.
             */
            if (is_param)
                vd->ptr_level++;
            else
                vd->has_unsized_array = true;
        }

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
        vd->is_func = false;
    }

    /* The legacy flag remains the outermost pointer qualifier for lvalue
     * writes. Intermediate qualifiers are retained in pointer_const_mask.
     */
    if (vd->ptr_level > 0 && vd->ptr_level <= 32)
        vd->is_const_pointer =
            vd->pointer_const_mask & (1U << (vd->ptr_level - 1));
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
    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
    }
    type_t *type;

    /* `signed` has the existing signed scalar semantics. C permits its `int`
     * spelling to be omitted, while `signed char` and `signed short` retain
     * their explicit base type.
     */
    if (is_enum_type) {
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
        type = find_type(type_name, find_type_flag);
    }

    if (!type) {
        printf("Could not find type %s%s\n",
               find_type_flag == 2 ? "struct/union " : "", type_name);
        fflush(stdout); /* see fatal() */
        abort();
    }

    vd->type = type;
    vd->is_const_qualified = type->is_const_qualified;
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
        else if (lex_accept(T_inline)) {
            if (is_inline || declaration_inline)
                error_at("duplicate inline function specifier",
                         cur_token_loc());
            is_inline = true;
        } else
            break;
    }
    vd->is_inline = declaration_inline || is_inline;
    vd->is_volatile = declaration_volatile || is_volatile;
    if (is_const || declaration_const) {
        if (type->ptr_level)
            vd->is_const_pointer = true;
        else
            vd->is_const_qualified = true;
    }

    read_inner_var_decl(vd, anon, is_param, is_record_member);

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
    if (lex_peek(T_identifier, token) && !strncmp(token, "void", 4)) {
        lex_next();
        if (lex_accept(T_close_bracket))
            return;
        func->param_defs[vn].type = TY_void;
        read_inner_var_decl(&func->param_defs[vn], anon, true, false);
        if (!func->param_defs[vn].ptr_level && !func->param_defs[vn].is_func &&
            !func->param_defs[vn].array_size)
            error_at("'void' must be the only parameter and unnamed",
                     cur_token_loc());
        vn++;
        lex_accept(T_comma);
    }

    while (lex_peek(T_identifier, NULL) || lex_peek(T_const, NULL) ||
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
        lex_accept(T_comma);
    }
    func->num_params = vn;

    /* Up to 'MAX_PARAMS' parameters are accepted for the variadic function. */
    if (lex_accept(T_elipsis))
        func->va_args = 1;

    lex_expect(T_close_bracket);
}

void read_literal_param(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN],
        combined[MAX_LINE_LEN];
    int combined_len = 0;

    /* Read first string literal */
    lex_ident(T_string, literal);
    unescape_string(literal, combined, MAX_LINE_LEN);
    combined_len = strlen(combined);

    /* Check for adjacent string literals and concatenate them */
    while (lex_peek(T_string, NULL)) {
        lex_ident(T_string, literal);
        unescape_string(literal, unescaped, MAX_LINE_LEN - combined_len);
        int unescaped_len = strlen(unescaped);
        if (combined_len + unescaped_len >= MAX_LINE_LEN - 1)
            error_at("Concatenated string literal too long", cur_token_loc());

        strcpy(combined + combined_len, unescaped);
        combined_len += unescaped_len;
    }

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
    char literal[MAX_TOKEN_LEN], unescaped[MAX_TOKEN_LEN],
        combined[MAX_LINE_LEN];
    int len;

    lex_ident(T_string, literal);
    unescape_string(literal, combined, MAX_LINE_LEN);
    while (lex_peek(T_string, NULL)) {
        int used = strlen(combined);

        lex_ident(T_string, literal);
        unescape_string(literal, unescaped, MAX_LINE_LEN - used);
        if (used + (int) strlen(unescaped) >= MAX_LINE_LEN - 1)
            error_at("Concatenated string literal too long", cur_token_loc());
        strcpy(combined + used, unescaped);
    }

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

    var_t *vd = require_var(parent);
    vd->var_name = gen_name();
    if (is_long_long || value_hi ||
        (is_decimal && !has_unsigned_suffix && !is_neg &&
         numeric_literal_needs_wide_path(token)) ||
        (is_decimal && !has_unsigned_suffix &&
         (value > 0x80000000U || (value == 0x80000000U && !is_neg)))) {
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

    /* Keep the exact 2^31 magnitude as an int bit pattern: C spells INT_MIN as
     * unary minus plus that token, and the unary operator is parsed after the
     * literal. Larger decimal values really need long long here.
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

void read_logical(opcode_t op, block_t *parent, basic_block_t **bb);
void read_func_parameters(func_t *func, block_t *parent, basic_block_t **bb)
{
    int param_num = 0;
    var_t *params[MAX_PARAMS], *param;

    lex_expect(T_open_bracket);
    while (!lex_accept(T_close_bracket)) {
        read_expr(parent, bb);
        read_ternary_operation(parent, bb);

        param = opstack_pop();

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
            diagnose_const_pointer_conversion(param, target);
            if (is_record_type(target->type) && !target->ptr_level) {
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
            } else if (!target->ptr_level && !target->array_size) {
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
            if (!is_pointer_like_value(param))
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
        lex_accept(T_comma);
    }

    for (int i = 0; i < param_num; i++) {
        /* The operand should keep alive before calling function. Pass the
         * number of remained parameters to allocator to extend their liveness.
         */
        add_insn(parent, *bb, OP_push, NULL, params[i], NULL, param_num - i,
                 NULL);
    }
}

void read_func_call(func_t *func, block_t *parent, basic_block_t **bb)
{
    /* direct function call */
    read_func_parameters(func, parent, bb);

    add_insn(parent, *bb, OP_call, NULL, NULL, NULL, 0,
             func->return_def.var_name);
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

void read_indirect_call(var_t *callee, block_t *parent, basic_block_t **bb)
{
    /* A function pointer carries its parsed prototype on the declaration. This
     * makes indirect calls obey the same record-by-value lowering and scalar
     * conversions as a direct call. Legacy/unprototyped pointers keep the old
     * generic behaviour. The callee was evaluated before its argument list.
     * Materialize a distinct SSA value now: lowering a by-value record argument
     * emits a caller-side copy and can otherwise evict the untracked
     * operand-stack value before OP_indirect consumes it.
     */
    var_t *target = opstack_pop();
    var_t *saved_target = require_var(parent);
    saved_target->var_name = gen_name();
    add_insn(parent, *bb, OP_assign, saved_target, target, NULL, 0, NULL);
    opstack_push(saved_target);

    func_t *signature = get_func_signature(callee);
    read_func_parameters(signature, parent, bb);

    add_insn(parent, *bb, OP_indirect, NULL, opstack_pop(), NULL, 0, NULL);
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
    var_t *var = find_var(token, parent);
    if (var && var->is_register)
        error_at("cannot take address of register object", next_token_loc());
    read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);

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
        opstack_push(vd);
        add_insn(parent, *bb, OP_address_of, vd, rs1, NULL, 0, NULL);
    }
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
        vd->pointer_const_mask = rs1->pointer_const_mask;
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
        opstack_push(vd);
        add_insn(parent, *bb, OP_read, vd, rs1, NULL, sz, NULL);
    } else {
        /* Handle simple identifier dereference: *var */
        char token[MAX_VAR_LEN];
        lvalue_t lvalue;

        if (!lex_peek(T_identifier, token))
            error_at("Expected an identifier", next_token_loc());
        var_t *var = find_var(token, parent);
        read_lvalue(&lvalue, var, parent, bb, true, OP_generic, false);

        rs1 = opstack_pop();
        vd = require_deref_var(parent, var->type, var->ptr_level);
        if (var->ptr_level + var->type->ptr_level > 1)
            sz = PTR_SIZE;
        else {
            /* For typedef pointers, get the size of the pointed-to type */
            if (lvalue.type && lvalue.type->ptr_level > 0) {
                /* This is a typedef pointer */
                switch (lvalue.type->base_type) {
                case TYPE_char:
                    sz = TY_char->size;
                    break;
                case TYPE_short:
                    sz = TY_short->size;
                    break;
                case TYPE_int:
                    sz = TY_int->size;
                    break;
                case TYPE_void:
                    sz = 1;
                    break;
                default:
                    sz = lvalue.type->size;
                    break;
                }
            } else {
                sz = lvalue.type->size;
            }
        }
        vd->var_name = gen_name();
        vd->is_const_qualified = var->is_const_qualified;
        vd->pointer_const_mask = var->pointer_const_mask;
        vd->is_const_pointer =
            vd->ptr_level > 0 && vd->ptr_level <= 32 &&
            (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
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
            vd = require_deref_var(
                parent, var->type,
                lvalue.ptr_level > i ? lvalue.ptr_level - i - 1 : 0);
            if (lvalue.ptr_level > i + 1)
                sz = PTR_SIZE;
            else {
                /* For typedef pointers, get the size of the pointed-to type */
                if (lvalue.type && lvalue.type->ptr_level > 0 &&
                    i == deref_count - 1) {
                    /* This is a typedef pointer on the final dereference */
                    switch (lvalue.type->base_type) {
                    case TYPE_char:
                        sz = TY_char->size;
                        break;
                    case TYPE_short:
                        sz = TY_short->size;
                        break;
                    case TYPE_int:
                        sz = TY_int->size;
                        break;
                    case TYPE_void:
                        sz = 1;
                        break;
                    default:
                        sz = lvalue.type->size;
                        break;
                    }
                } else {
                    sz = lvalue.type->size;
                }
            }
            vd->var_name = gen_name();
            vd->is_const_qualified = rs1->is_const_qualified;
            vd->pointer_const_mask = rs1->pointer_const_mask;
            vd->is_const_pointer =
                vd->ptr_level > 0 && vd->ptr_level <= 32 &&
                (vd->pointer_const_mask & (1U << (vd->ptr_level - 1)));
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
    char literal[MAX_TOKEN_LEN];
    char unescaped[MAX_TOKEN_LEN];
    int size = 1;

    do {
        lex_ident(T_string, literal);
        unescape_string(literal, unescaped, MAX_TOKEN_LEN);
        size += strlen(unescaped);
    } while (lex_peek(T_string, NULL));

    return size;
}

int sizeof_array_object(const var_t *array)
{
    int element_size = array->type->size;

    if (array->ptr_level || array->type->ptr_level || array->is_func)
        element_size = PTR_SIZE;
    return array->array_size * element_size;
}

void handle_sizeof_operator(block_t *parent, basic_block_t **bb)
{
    char token[MAX_ID_LEN];
    int ptr_cnt = 0;
    int array_size = 0;
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

    if (has_enum_type) {
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
        type = find_type(token, find_type_flag);
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

    if (!type) {
        /* sizeof(expression) - parse the expression and get its type */
        basic_block_t *unevaluated_bb = bb_create(parent);
        int saved_side_effects = se_idx;
        read_expr(parent, &unevaluated_bb);
        read_ternary_operation(parent, &unevaluated_bb);
        se_idx = saved_side_effects;
        var_t *expr_var = opstack_pop();
        type = expr_var->type;
        ptr_cnt = expr_var->ptr_level;
        array_size = expr_var->array_size;
        is_function = expr_var->is_func;
    }

    if (!type)
        error_at("Unable to determine type in sizeof", &sizeof_tk->location);
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
    lex_expect(T_close_bracket);
    add_insn(parent, *bb, OP_load_constant, vd, NULL, NULL, 0, NULL);
}

void read_expr_operand(block_t *parent, basic_block_t **bb)
{
    var_t *vd, *rs1;
    bool is_neg = false;

    if (lex_accept(T_plus)) {
        read_expr_operand(parent, bb);
        rs1 = opstack_pop();
        if (is_pointer_like_value(rs1) || rs1->is_func)
            error_at("unary plus requires an arithmetic operand",
                     cur_token_loc());
        rs1 = integer_promote_operand(parent, bb, rs1);
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
    else if (lex_peek(T_char, NULL))
        read_char_param(parent, *bb);

    else if (lex_peek(T_numeric, NULL))
        read_numeric_param(parent, *bb, is_neg);
    else if (lex_accept(T_log_not)) {
        read_expr_operand(parent, bb);

        rs1 = opstack_pop();

        /* Constant folding for logical NOT */
        if (rs1 && rs1->is_const && !rs1->ptr_level && !rs1->is_global) {
            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->is_const = true;
            vd->init_val = !rs1->init_val;
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
        bool cast_const_qualified = false;
        bool cast_const_pointer = false;
        unsigned int cast_pointer_const_mask = 0;
        bool cast_volatile_qualified = false;

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

                /* Check for array brackets: [size] or [] */
                bool is_array = false;
                if (lex_accept(T_open_square)) {
                    is_array = true;

                    /* Preserve a declared bound for compound literals. */
                    if (lex_peek(T_numeric, NULL)) {
                        char bound[MAX_TOKEN_LEN];
                        lex_ident_n(T_numeric, bound, MAX_TOKEN_LEN);
                        cast_array_size = parse_numeric_constant(bound);
                        if (cast_array_size <= 0)
                            error_at(
                                "Array compound literal needs a positive bound",
                                next_token_loc());
                    }
                    lex_expect(T_close_square);
                }

                /* Check what follows the closing ) */
                if (lex_accept(T_close_bracket)) {
                    if (lex_peek(T_open_curly, NULL)) {
                        /* (type){...} - compound literal */
                        is_compound_literal = true;
                        cast_or_literal_type = type;
                        cast_ptr_level = ptr_level;

                        /* Store is_array flag in cast_ptr_level if it's an
                         * array
                         */
                        if (is_array) {
                            /* Special marker for array compound literal */
                            cast_ptr_level = -1;
                        }
                    } else {
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

            /* Create variable for cast result */
            var_t *cast_var = require_typed_ptr_var(
                parent, cast_or_literal_type, cast_ptr_level);
            cast_var->var_name = gen_name();
            cast_var->is_const_qualified = cast_const_qualified;
            cast_var->is_const_pointer = cast_const_pointer;
            cast_var->pointer_const_mask = cast_pointer_const_mask;
            cast_var->is_volatile = cast_volatile_qualified;

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
            /* Process compound literal */
            lex_expect(T_open_curly);

            /* Create variable for compound literal result */
            var_t *compound_var =
                require_typed_var(parent, cast_or_literal_type);
            compound_var->var_name = gen_name();
            compound_var->is_compound_literal = true;

            /* Check if this is an array compound literal (int[]){...} */
            bool is_array_literal = (cast_ptr_level == -1);
            if (is_array_literal)
                cast_ptr_level = 0; /* Reset for normal processing */
            bool consumed_close_brace = false;
            /* Check if this is a pointer compound literal */
            if (is_array_literal) {
                compound_var->array_size = cast_array_size;
                add_insn(parent, *bb, OP_allocat, compound_var, NULL, NULL, 0,
                         NULL);
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
                compound_var->ptr_level = cast_ptr_level;

                /* Parse the pointer value (should be an address) */
                if (!lex_peek(T_close_curly, NULL)) {
                    read_expr(parent, bb);
                    read_ternary_operation(parent, bb);
                    const var_t *ptr_val = opstack_pop();

                    /* For pointer compound literals, store the address */
                    compound_var->init_val = ptr_val->init_val;

                    if (lex_accept(T_comma) && !lex_peek(T_close_curly, NULL))
                        error_at("Too many elements in scalar compound literal",
                                 cur_token_loc());
                } else {
                    /* Empty pointer compound literal: (int*){} */
                    compound_var->init_val = 0; /* NULL pointer */
                }

                /* Generate code for pointer compound literal */
                opstack_push(compound_var);
                add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                         NULL, 0, NULL);
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
                opstack_push(compound_var);
            } else if (cast_or_literal_type->base_type == TYPE_int ||
                       cast_or_literal_type->base_type == TYPE_short ||
                       cast_or_literal_type->base_type == TYPE_char) {
                /* Handle empty compound literals */
                if (lex_peek(T_close_curly, NULL)) {
                    /* Empty compound literal: (int){} */
                    compound_var->init_val = 0;
                    compound_var->array_size = 0;
                    opstack_push(compound_var);
                    add_insn(parent, *bb, OP_load_constant, compound_var, NULL,
                             NULL, 0, NULL);
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
                        compound_var = opstack_pop();
                        opstack_push(compound_var);
                    }
                }
            }

            if (!consumed_close_brace)
                lex_expect(T_close_curly);
        } else {
            /* Regular parenthesized expression */
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);
            lex_expect(T_close_bracket);
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
        func_t *func = find_func(token);

        /* A block-scope function prototype shadows an automatic object, but its
         * designator is the file-scope function rather than an indirect call
         * through that object.
         */
        if (var && var->is_extern_function_alias)
            var = NULL;

        if (!strcmp(token, "__func__")) {
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
            read_lvalue(&lvalue, var, parent, bb, true, prefix_op, true);

            /* is it an indirect call with function pointer? */
            if (lex_peek(T_open_bracket, NULL)) {
                read_indirect_call(lvalue.decl, parent, bb);

                func_t *signature = get_func_signature(lvalue.decl);
                if (signature)
                    vd = require_typed_ptr_var(parent,
                                               signature->return_def.type,
                                               signature->return_def.ptr_level);
                else
                    vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, vd, NULL, NULL, 0, NULL);
            }
        } else if (func) {
            lex_expect(T_identifier);

            if (lex_peek(T_open_bracket, NULL)) {
                read_func_call(func, parent, bb);

                vd = require_typed_ptr_var(parent, func->return_def.type,
                                           func->return_def.ptr_level);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_func_ret, vd, NULL, NULL, 0, NULL);
            } else {
                /* indirective function pointer assignment */
                vd = require_func_symbol_var(parent);
                vd->is_func = true;
                vd->var_name = intern_string(token);
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
    if (!ptr_var || !ptr_var->type)
        return PTR_SIZE; /* Default to pointer size */

    /* An array of pointers decays to a pointer-to-pointer. The declaration
     * records its element's indirection level, so account for the decay before
     * deriving the pointed-to object size.
     */
    if (ptr_var->array_size && ptr_var->ptr_level)
        return PTR_SIZE;

    /* Direct pointer with type info.
     *
     * Only a single level of indirection points at the base type. For deeper
     * pointers (int **, char ***, ...) the element is itself a pointer, so the
     * step is PTR_SIZE. Returning the base type size there makes "q + 1"
     * advance by 4 instead of 8 on LP64 and drops a level of type information
     * from the result.
     */
    if (ptr_var->ptr_level && ptr_var->type) {
        if (ptr_var->ptr_level > 1)
            return PTR_SIZE;
        return ptr_var->type->size;
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

            if (!compatible_decl_type(left_pointee, right_pointee) ||
                left_depth != right_depth)
                error_at(
                    "Pointer subtraction requires compatible pointed-to types",
                    cur_token_loc());

            element_size = PTR_SIZE; /* Default */

            element_size = get_pointer_element_size(orig_rs1);

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
        element_size = get_pointer_element_size(rs1);
    } else if (is_pointer_like_value(rs2)) {
        /* Only for addition (p + n == n + p) */
        if (op == OP_add) {
            ptr_var = rs2;
            int_var = rs1;
            element_size = get_pointer_element_size(rs2);
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
        vd->ptr_level = ptr_var->ptr_level;
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
    if (var)
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
    bool is_address_got = false;
    bool is_member = false;
    int subscript_depth = 0;

    /* Callers pass a find_var() result, which is NULL for a name that was never
     * declared.
     */
    if (!var)
        error_at("Undeclared identifier", next_token_loc());

    /* already peeked and have the variable */
    lex_expect(T_identifier);

    lvalue->type = var->type;
    lvalue->decl = var;
    lvalue->size = get_size(var);
    lvalue->ptr_level = var->ptr_level;
    lvalue->value_ptr_level = var->ptr_level + var->type->ptr_level;
    lvalue->is_func = var->is_func;
    lvalue->is_reference = false;

    /* A pointer hidden in a typedef keeps its depth on the type rather than the
     * declarator. Its outer const still makes the pointer object read-only,
     * just as for an explicitly spelled `T * const`.
     */
    lvalue->is_const_qualified =
        (var->ptr_level || (var->type && var->type->ptr_level))
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

            /* if subscripted member's is not yet resolved, dereference to
             * resolve base address. e.g., dereference of "->" in "data->raw[0]"
             * would be performed here.
             */
            if (lvalue->is_reference && lvalue->ptr_level && is_member) {
                rs1 = opstack_pop();
                vd = require_var(parent);
                vd->var_name = gen_name();
                opstack_push(vd);
                add_insn(parent, *bb, OP_read, vd, rs1, NULL, PTR_SIZE, NULL);
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
            lvalue->value_ptr_level =
                indexed_ptr_level ? indexed_ptr_level - 1 : 0;

            /* if nested pointer, still pointer Also handle typedef pointers
             * which have ptr_level == 0
             */
            if ((var->ptr_level <= 1 || is_typedef_pointer) &&
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

            /* multiply by element size For 2D arrays, check if this is the
             * first or second dimension
             */
            int multiplier = lvalue->size;

            /* If this is the first index of a 2D array, multiply by dim2 *
             * element_size
             */
            if (subscript_depth == 0 && var->array_dim2 > 0)
                multiplier = var->array_dim2 * lvalue->size;

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
            vd->ptr_level = lvalue->ptr_level ? lvalue->ptr_level : 1;
            vd->var_name = gen_name();
            opstack_push(vd);
            add_insn(parent, *bb, OP_add, vd, rs1, rs2, 0, NULL);

            lex_expect(T_close_square);
            is_address_got = true;
            is_member = true;
            subscript_depth++;
            lvalue->is_reference = true;

            /* A subscript designates the pointee, whose qualification is the
             * declaration's base qualification rather than `* const`.
             */
            lvalue->is_const_qualified = var->is_const_qualified;
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
    if (allow_ptr_arith && lex_peek(T_plus, NULL) &&
        (var->ptr_level || is_array_declarator(var)) && !lvalue->is_reference) {
        if (!is_array_declarator(var) &&
            is_direct_void_pointer_type(lvalue->type, lvalue->ptr_level))
            error_at("Pointer arithmetic on void* is invalid", cur_token_loc());
        while (lex_peek(T_plus, NULL) &&
               (var->ptr_level || is_array_declarator(var))) {
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
            if (var->ptr_level > 1 ||
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
            if (var->ptr_level || is_array_declarator(var)) {
                vd->type = lvalue->type;
                vd->ptr_level = var->ptr_level + !!is_array_declarator(var);
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
            t->type = pointee_type_from_pointer_typedef(lvalue->type);
            t->ptr_level = lvalue->value_ptr_level;
            opstack_push(t);
            add_insn(parent, *bb, OP_read, t, rs1, NULL, lvalue->size, NULL);
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
            if (lvalue->ptr_level > 1)
                vd->init_val = PTR_SIZE;
            else if (lvalue->ptr_level)
                vd->init_val = lvalue->type->size;
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
            if (lvalue->ptr_level > 1 && !lvalue->is_reference) {
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
    bb_connect(then_, end_ternary, NEXT);
    bb_connect(else_, end_ternary, NEXT);

    /* true branch */
    read_expr(parent, &then_);
    bb_connect(*bb, then_, THEN);

    if (!lex_accept(T_colon)) {
        /* ternary operator in standard C needs three operands */
        error_at("Expected ':' in conditional expression", next_token_loc());
    }

    var_t *true_val = opstack_pop();

    /* false branch */
    read_expr(parent, &else_);
    bb_connect(*bb, else_, ELSE);
    var_t *false_val = opstack_pop();
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

    vd = require_var(parent);
    vd->var_name = gen_name();
    if (!true_ptr_like && !false_ptr_like && !true_val->ptr_level &&
        !false_val->ptr_level) {
        true_val = integer_promote_operand(parent, &then_, true_val);
        false_val = integer_promote_operand(parent, &else_, false_val);
        vd->type = integer_binary_result_type(OP_add, true_val, false_val);
        true_val = resize_to(parent, &then_, true_val, vd->type, 0);
        false_val = resize_to(parent, &else_, false_val, vd->type, 0);
    }
    add_insn(parent, then_, OP_assign, vd, true_val, NULL, 0, NULL);
    add_insn(parent, else_, OP_assign, vd, false_val, NULL, 0, NULL);

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
                          basic_block_t **bb)
{
    var_t *var = find_local_var(token, parent), *vd, *rs1, *rs2, *t;
    if (!var)
        var = find_global_var(token);

    if (var) {
        int one = 0;
        opcode_t op = OP_generic;
        lvalue_t lvalue;
        int size = 0;

        /* has memory address that we want to set */
        read_lvalue(&lvalue, var, parent, bb, false, OP_generic, true);
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

            read_indirect_call(lvalue.decl, parent, bb);
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
            if (lvalue.ptr_level > 1 && !lvalue.is_reference)
                increment_size = PTR_SIZE;
            else if (lvalue.ptr_level && !lvalue.is_reference)
                increment_size = lvalue.type->size;
            /* Also check for typedef pointers which have is_ptr == 0 */
            else if (!lvalue.is_reference && lvalue.type &&
                     lvalue.type->ptr_level > 0) {
                /* This is a typedef pointer, get the base type size */
                switch (lvalue.type->base_type) {
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
                    /* void pointers treated as byte pointers */
                    increment_size = 1;
                    break;
                default:
                    /* For struct pointers and other types */
                    increment_size = lvalue.type->size;
                    break;
                }
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
                    add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                             NULL);
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
                    add_insn(parent, *bb, OP_write, NULL, t, vd, size, NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    mark_var_mutated(t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
            } else {
                if (lvalue.is_reference) {
                    t = opstack_pop();
                    vd = require_var(parent);
                    vd->var_name = gen_name();
                    vd->type = pointee_type_from_pointer_typedef(lvalue.type);
                    vd->ptr_level = lvalue.value_ptr_level;
                    opstack_push(vd);
                    add_insn(parent, *bb, OP_read, vd, t, NULL, lvalue.size,
                             NULL);
                } else
                    t = operand_stack[operand_stack_idx - 1];

                read_expr(parent, bb);

                /* read_expr stops before `?`; compound assignment has the same
                 * conditional-expression RHS grammar as ordinary assignment.
                 */
                read_ternary_operation(parent, bb);

                var_t *rhs_val = opstack_pop();
                rhs_val = scalarize_array_literal_if_needed(
                    parent, bb, rhs_val, lvalue.type,
                    !lvalue.ptr_level && !lvalue.is_reference);
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
                    add_insn(parent, *bb, OP_write, NULL, t, vd, lvalue.size,
                             NULL);
                } else {
                    vd = resize_var(parent, bb, vd, t);
                    add_insn(parent, *bb, OP_assign, t, vd, NULL, 0, NULL);
                }
            }
        } else {
            read_expr(parent, bb);
            read_ternary_operation(parent, bb);

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
            } else if (lvalue.is_reference) {
                rs2 = opstack_pop();
                rs1 = opstack_pop();
                add_insn(parent, *bb, OP_write, NULL, rs1, rs2, size, NULL);
            } else {
                rs1 = opstack_pop();
                vd = opstack_pop();
                if (is_record_object(vd) && is_record_object(rs1)) {
                    emit_record_copy(parent, bb, vd, rs1);
                } else if (incompatible_character_pointer_conversion(rs1, vd)) {
                    error_at(
                        "incompatible character pointer types in assignment",
                        cur_token_loc());
                } else if (incompatible_const_pointer_conversion(rs1, vd)) {
                    diagnose_const_pointer_conversion(rs1, vd);
                } else {
                    rs1 = resize_var(parent, bb, rs1, vd);
                    mark_var_mutated(vd);
                    add_insn(parent, *bb, OP_assign, vd, rs1, NULL, 0, NULL);
                }
            }
        }
        return true;
    }
    return false;
}

int read_primary_constant(block_t *scope)
{
    /* return signed constant */
    int isneg = 0, res;
    char buffer[MAX_TOKEN_LEN];
    if (lex_accept(T_minus))
        isneg = 1;
    if (lex_accept(T_open_bracket)) {
        res = read_primary_constant(scope);
        lex_expect(T_close_bracket);
    } else if (lex_peek(T_numeric, buffer)) {
        res = parse_numeric_constant(buffer);
        lex_expect(T_numeric);
    } else if (lex_peek(T_char, buffer)) {
        char unescaped[MAX_TOKEN_LEN];
        unescape_string(buffer, unescaped, MAX_TOKEN_LEN);
        res = parse_character_constant(buffer);
        lex_expect(T_char);
    } else if (lex_peek(T_identifier, buffer)) {
        constant_t *con;

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
        res = op1 + op2;
        break;
    case OP_sub:
        res = op1 - op2;
        break;
    case OP_mul:
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
bool wide_global_literal_appears_before_initializer_end(token_t *token)
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
                 numeric_literal_needs_wide_path(token->literal))
            return true;
    }
    return false;
}

var_t *read_wide_global_literal_expression(block_t *parent, basic_block_t *bb);

/* A grouped primary is lowered into the same global setup block as its parent.
 * The caller owns the closing parenthesis, so get_operator() naturally stops an
 * inner precedence stack without consuming its delimiter.
 */
var_t *read_wide_global_literal_primary(block_t *parent, basic_block_t *bb)
{
    char literal[MAX_TOKEN_LEN];
    var_t *value;

    /* Keep wide unary operators out of the legacy word-sized constant
     * evaluator. Besides making `~0ULL` usable in a static initializer, the
     * recursive form gives grouped operands and repeated unary operators the
     * same semantics as ordinary expression parsing.
     */
    if (lex_accept(T_plus))
        return read_wide_global_literal_primary(parent, bb);
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
        value = read_wide_global_literal_primary(parent, bb);
        zero->var_name = gen_name();
        zero->type = value->type;
        zero->init_val = 0;
        add_insn(parent, bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        result = require_var(parent);
        result->var_name = gen_name();
        result->type = value->type;
        add_insn(parent, bb, OP_sub, result, zero, value, 0, NULL);
        return result;
    }
    if (lex_accept(T_bit_not)) {
        var_t *result;

        value = read_wide_global_literal_primary(parent, bb);
        result = require_var(parent);
        result->var_name = gen_name();
        result->type = value->type;
        add_insn(parent, bb, OP_bit_not, result, value, NULL, 0, NULL);
        return result;
    }
    if (lex_accept(T_log_not)) {
        var_t *result;

        value = read_wide_global_literal_primary(parent, bb);
        result = require_typed_var(parent, TY_int);
        result->var_name = gen_name();
        add_insn(parent, bb, OP_log_not, result, value, NULL, 0, NULL);
        return result;
    }
    if (lex_accept(T_open_bracket)) {
        value = read_wide_global_literal_expression(parent, bb);
        lex_expect(T_close_bracket);
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
var_t *read_wide_global_literal_expression(block_t *parent, basic_block_t *bb)
{
    opcode_t op_stack[MAX_OPERATOR_STACK_SIZE];
    var_t *val_stack[MAX_OPERATOR_STACK_SIZE];
    int op_stack_index = 0, val_stack_index = 0;
    opcode_t op;

    val_stack[val_stack_index++] = read_wide_global_literal_primary(parent, bb);
    op = get_operator();
    while (op != OP_generic) {
        if (op == OP_ternary || is_logical(op))
            error_at("Wide global initializer needs arithmetic literals",
                     cur_token_loc());
        while (op_stack_index > 0 &&
               get_operator_prio(op_stack[op_stack_index - 1]) >=
                   get_operator_prio(op)) {
            var_t *right = val_stack[--val_stack_index];
            var_t *left = val_stack[--val_stack_index];
            var_t *result = require_var(parent);

            result->var_name = gen_name();
            result->type = integer_binary_result_type(
                op_stack[--op_stack_index], left, right);
            add_insn(parent, bb, op_stack[op_stack_index], result, left, right,
                     0, NULL);
            val_stack[val_stack_index++] = result;
        }
        if (op_stack_index >= MAX_OPERATOR_STACK_SIZE ||
            val_stack_index >= MAX_OPERATOR_STACK_SIZE)
            fatal("Wide global initializer is too complex");
        op_stack[op_stack_index++] = op;
        val_stack[val_stack_index++] =
            read_wide_global_literal_primary(parent, bb);
        op = get_operator();
    }
    while (op_stack_index > 0) {
        var_t *right = val_stack[--val_stack_index];
        var_t *left = val_stack[--val_stack_index];
        var_t *result = require_var(parent);

        result->var_name = gen_name();
        result->type =
            integer_binary_result_type(op_stack[--op_stack_index], left, right);
        add_insn(parent, bb, op_stack[op_stack_index], result, left, right, 0,
                 NULL);
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

    if ((var->array_size > 0 || var->has_unsized_array) && !var->ptr_level &&
        var->type == TY_char && lex_peek(T_string, NULL)) {
        parse_string_array_init(var, parent, &bb);
        return true;
    }

    /* global initialization must be constant */
    {
        /* A function designator is a valid address constant. Keep it as the
         * function symbol until lowering: OP_address_of_func has the deferred
         * relocation needed because the target function's code offset is not
         * known while global initializers are parsed.
         */
        bool explicit_address = lex_accept(T_ampersand);
        char token[MAX_ID_LEN];
        if (lex_peek(T_identifier, token)) {
            func_t *func = find_func(token);
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
                var_t *object_addr =
                    require_ref_var(parent, object->type, object->ptr_level);

                object_addr->var_name = gen_name();
                lex_expect(T_identifier);
                add_insn(parent, bb, OP_address_of, object_addr, object, NULL,
                         0, NULL);
                if (!explicit_address && object->array_size &&
                    lex_accept(T_plus)) {
                    int index = read_primary_constant(scope);
                    var_t *byte_offset = require_var(parent);
                    var_t *offset_addr = require_ref_var(parent, object->type,
                                                         object->ptr_level);

                    byte_offset->var_name = gen_name();
                    byte_offset->init_val = index * object->type->size;
                    add_insn(parent, bb, OP_load_constant, byte_offset, NULL,
                             NULL, 0, NULL);
                    offset_addr->var_name = gen_name();
                    add_insn(parent, bb, OP_add, offset_addr, object_addr,
                             byte_offset, 0, NULL);
                    object_addr = offset_addr;
                }
                while (explicit_address) {
                    if (lex_accept(T_dot)) {
                        char field_name[MAX_ID_LEN];
                        var_t *field;

                        lex_ident(T_identifier, field_name);
                        field = find_member(field_name, object->type);
                        if (!field)
                            error_at("Unknown struct or union member",
                                     cur_token_loc());
                        object_addr = compute_field_address(parent, &bb,
                                                            object_addr, field);
                        object = field;
                    } else if (object->array_size &&
                               lex_accept(T_open_square)) {
                        int index = read_primary_constant(scope);

                        lex_expect(T_close_square);
                        object_addr =
                            compute_element_address(parent, &bb, object_addr,
                                                    index, object->type->size);
                    } else
                        break;
                }

                /* An address constant may be offset after an explicitly
                 * addressed member or element too. Array decay had this
                 * handling above, but forms such as "&record.items[0] + 1"
                 * stopped at the plus token. Keep the offset byte-scaled here
                 * so global setup receives a literal byte offset.
                 */
                if (explicit_address &&
                    (lex_peek(T_plus, NULL) || lex_peek(T_minus, NULL))) {
                    /* read_const_expr accepts the leading binary sign as a
                     * unary sign here and consumes the entire integer constant
                     * expression, including grouping and enum constants.
                     */
                    int index = read_const_expr(scope);
                    int elem_size =
                        object->ptr_level ? PTR_SIZE : object->type->size;

                    object_addr = compute_element_address(
                        parent, &bb, object_addr, index, elem_size);
                }
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
        if (wide_global_literal_appears_before_initializer_end(
                cur_token->next)) {
            rs1 = read_wide_global_literal_expression(parent, bb);
            add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
            return true;
        }
        if (lex_peek(T_string, NULL)) {
            /* String literal global initialization: String literals are now
             * stored in .rodata section. TODO: Implement compile-time address
             * resolution for global pointer initialization with rodata
             * addresses (e.g., char *p = "str";)
             */
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
            vd = opstack_pop();
            emit_global_scalar_assignment(parent, bb, vd, rs1);
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
            vd = opstack_pop();
            add_insn(parent, bb, OP_assign, vd, rs1, NULL, 0, NULL);
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
                    vd = opstack_pop();
                    emit_global_scalar_assignment(parent, bb, vd, rs1);
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
            vd = opstack_pop();
            emit_global_scalar_assignment(parent, GLOBAL_FUNC->bbs, vd, rs1);
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

/* A switch, its cases, and the block they break out of. */
basic_block_t *handle_switch_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    var_t *vd;
    var_t *rs1;
    var_t *rs2;

    bool is_default = false;

    basic_block_t *n = bb_create(parent);
    bb_connect(bb, n, NEXT);
    bb = n;

    lex_expect(T_open_bracket);
    read_expr(parent, &bb);
    lex_expect(T_close_bracket);

    /* create exit jump for breaks */
    basic_block_t *switch_end = bb_create(parent);
    break_bb_push(switch_end);
    basic_block_t *true_body_ = bb_create(parent);

    lex_expect(T_open_curly);
    while (lex_peek(T_default, NULL) || lex_peek(T_case, NULL)) {
        if (lex_accept(T_default))
            is_default = true;
        else {
            int case_val;

            lex_accept(T_case);
            char literal[MAX_TOKEN_LEN];

            if (lex_peek_n(T_numeric, literal, MAX_TOKEN_LEN)) {
                case_val = parse_numeric_constant(literal);
                lex_expect(T_numeric);
            } else if (lex_peek_n(T_char, literal, MAX_TOKEN_LEN)) {
                char unescaped[MAX_TOKEN_LEN];
                if (unescape_string(literal, unescaped, MAX_TOKEN_LEN) < 0)
                    error_at("Invalid escape sequence", next_token_loc());
                case_val = parse_character_constant(literal);
                lex_expect(T_char);
            } else if (lex_peek(T_identifier, token)) {
                const constant_t *cd = find_scoped_constant(token, parent);
                if (!cd)
                    error_at("Unknown constant in case label", cur_token_loc());
                case_val = cd->value;
                lex_expect(T_identifier);
            } else {
                fatal("Not a valid case value");
            }

            vd = require_var(parent);
            vd->var_name = gen_name();
            vd->init_val = case_val;
            opstack_push(vd);
            add_insn(parent, bb, OP_load_constant, vd, NULL, NULL, 0, NULL);

            vd = require_var(parent);
            vd->var_name = gen_name();
            rs1 = opstack_pop();
            rs2 = operand_stack[operand_stack_idx - 1];
            add_insn(parent, bb, OP_eq, vd, rs1, rs2, 0, NULL);

            add_insn(parent, bb, OP_branch, NULL, vd, NULL, 0, NULL);
        }
        lex_expect(T_colon);

        if (is_default)
            /* there's no condition if it is a default label */
            bb_connect(bb, true_body_, NEXT);
        else
            bb_connect(bb, true_body_, THEN);

        int control = 0;

        while (!lex_peek(T_case, NULL) && !lex_peek(T_close_curly, NULL) &&
               !lex_peek(T_default, NULL)) {
            true_body_ = read_body_statement(parent, true_body_);
            control = 1;
        }

        if (control && true_body_) {
            /* Create a new body block for next case, and connect the last body
             * block which lacks 'break' to it to make that one ignore the
             * upcoming cases.
             */
            n = bb_create(parent);
            bb_connect(true_body_, n, NEXT);
            true_body_ = n;
        }

        if (!lex_peek(T_close_curly, NULL)) {
            if (is_default)
                error_at("Label default should be the last one",
                         next_token_loc());

            /* create a new conditional block for next case */
            n = bb_create(parent);
            bb_connect(bb, n, ELSE);
            bb = n;

            /* create a new body block for next case if the last body block
             * exits 'switch'.
             */
            if (!true_body_)
                true_body_ = bb_create(parent);
        } else if (!is_default) {
            /* handle missing default label */
            bb_connect(bb, switch_end, ELSE);
        }
    }

    /* remove the expression in switch() */
    opstack_pop();
    lex_expect(T_close_curly);

    if (true_body_)
        /* if the last label has no explicit break, connect it to the end */
        bb_connect(true_body_, switch_end, NEXT);

    break_exit_idx--;

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
    opcode_t prefix_op = OP_generic;
    bool is_const = false;
    bool is_static = false;
    bool is_extern = false;
    bool is_register = false;
    bool is_volatile = false;
    bool for_decl_semicolon_consumed = false;

    lex_expect(T_open_bracket);

    /* synthesize for loop block */
    block_t *blk = add_block(parent, parent->func);

    /* setup - execute once */
    basic_block_t *setup = bb_create(blk);
    bb_connect(bb, setup, NEXT);

    if (!lex_accept(T_semicolon)) {
        while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
               lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
               lex_peek(T_register, NULL)) {
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
            } else if (lex_accept(T_volatile)) {
                is_volatile = true;
            } else {
                lex_expect(T_const);
                is_const = true;
            }
        }
        if ((is_static && is_register) || (is_static && is_extern) ||
            (is_register && is_extern))
            error_at("incompatible storage class specifiers", cur_token_loc());

        bool has_builtin_type = lex_peek(T_signed, NULL) ||
                                lex_peek(T_unsigned, NULL) ||
                                lex_peek(T_long, NULL);
        if (!has_builtin_type && !lex_peek(T_identifier, token) &&
            !lex_peek(T_struct, NULL) && !lex_peek(T_union, NULL))
            error_at("Unexpected token when parsing for loop",
                     next_token_loc());

        int find_type_flag = lex_accept(T_struct) ? 2 : 1;
        if (find_type_flag == 1 && lex_accept(T_union)) {
            find_type_flag = 2;
        }
        type = has_builtin_type ? TY_int : find_type(token, find_type_flag);
        if (type) {
            var = require_typed_var(blk, type);
            var->is_static = is_static;
            var->is_register = is_register;
            var->is_global = is_static;
            var->is_const_qualified = is_const;
            var->is_volatile = is_volatile;
            read_full_var_decl(var, false, false, false);
            if (is_extern) {
                if (var->is_func || lex_peek(T_open_bracket, NULL)) {
                    /* This helper consumes the declaration's semicolon. */
                    blk->locals.size--;
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
                add_insn(is_static ? GLOBAL_BLOCK : blk,
                         is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat, var,
                         NULL, NULL, 0, NULL);
                add_symbol(setup, var);
                if (lex_accept(T_assign)) {
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
                    } else if (var->has_unsized_array && !var->ptr_level &&
                               var->type == TY_char &&
                               lex_peek(T_string, NULL)) {
                        parse_string_array_init(var, blk, &setup);
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
                    add_insn(is_static ? GLOBAL_BLOCK : blk,
                             is_static ? GLOBAL_FUNC->bbs : setup, OP_allocat,
                             nv, NULL, NULL, 0, NULL);
                    add_symbol(setup, nv);
                    if (lex_accept(T_assign)) {
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
                        } else if (nv->has_unsized_array && !nv->ptr_level &&
                                   nv->type == TY_char &&
                                   lex_peek(T_string, NULL)) {
                            parse_string_array_init(nv, blk, &setup);
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
            read_body_assignment(token, blk, OP_generic, &setup);
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
        read_expr(blk, &cond_);
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
        if (lex_accept(T_increment))
            prefix_op = OP_add;
        else if (lex_accept(T_decrement))
            prefix_op = OP_sub;
        lex_peek(T_identifier, token);
        read_body_assignment(token, blk, prefix_op, &inc_);
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
    read_expr(parent, &cond_);
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
basic_block_t *handle_record_statement(block_t *parent, basic_block_t *bb)
{
    char token[MAX_ID_LEN];
    type_t *type;
    var_t *var;
    bool is_const = false;

    int find_type_flag = lex_accept(T_struct) ? 2 : 1;
    if (find_type_flag == 1 && lex_accept(T_union)) {
        find_type_flag = 2;
    }
    lex_ident(T_identifier, token);
    type = find_type(token, find_type_flag);
    if (type) {
        var = require_typed_var(parent, type);
        var->is_const_qualified = is_const;
        read_partial_var_decl(var, NULL);
        add_insn(parent, bb, OP_allocat, var, NULL, NULL, 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
            if (lex_peek(T_open_curly, NULL) &&
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
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);

                var_t *rhs = opstack_pop();
                rhs = scalarize_array_literal_if_needed(
                    parent, &bb, rhs, var->type,
                    !var->ptr_level && var->array_size == 0);

                emit_object_assignment(parent, &bb, var, rhs);
            }
        }
        while (lex_accept(T_comma)) {
            var_t *nv;

            /* add sequence point at T_comma */
            perform_side_effect(parent, bb);

            /* multiple (partial) declarations */
            nv = require_typed_var(parent, type);
            read_inner_var_decl(nv, false, false, false);
            add_insn(parent, bb, OP_allocat, nv, NULL, NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
                if (lex_peek(T_open_curly, NULL) &&
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
                        !nv->ptr_level && nv->array_size == 0);

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
            if (is_static) {
                if (lex_peek(T_open_curly, NULL) &&
                    (var->array_size > 0 || var->has_unsized_array ||
                     var->ptr_level > 0)) {
                    parse_array_init(var, GLOBAL_BLOCK, &GLOBAL_FUNC->bbs,
                                     true);
                } else {
                    read_global_assignment_var(var);
                }
            } else if (var->has_unsized_array && !var->ptr_level &&
                       var->type == TY_char && lex_peek(T_string, NULL)) {
                parse_string_array_init(var, parent, &bb);
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
                    !var->ptr_level && var->array_size == 0);
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
    type->base_type = TYPE_int;
    type->size = TY_int->size;
    if (has_tag) {
        set_type_name(type, token);
        if (!find_local_type_tag(token, parent))
            add_type_tag(parent, token, type);
    }
    lex_expect(T_open_curly);
    do {
        lex_ident(T_identifier, token);
        if (lex_accept(T_assign))
            val = read_const_expr(parent);
        add_scoped_constant(parent, token, val++);
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
    bool is_volatile = false;

    while (lex_peek(T_static, NULL) || lex_peek(T_extern, NULL) ||
           lex_peek(T_const, NULL) || lex_peek(T_volatile, NULL) ||
           lex_peek(T_register, NULL)) {
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
        } else if (lex_accept(T_volatile)) {
            is_volatile = true;
        } else {
            lex_expect(T_const);
            is_const = true;
        }
    }
    if ((is_static && is_register) || (is_static && is_extern) ||
        (is_register && is_extern))
        error_at("incompatible storage class specifiers", cur_token_loc());

    if (lex_peek(T_enum, NULL))
        return handle_enum_statement(parent, bb, is_const, is_static);

    /* statement with prefix */
    if (!is_const && !is_static && lex_accept(T_increment))
        prefix_op = OP_add;
    else if (!is_const && !is_static && lex_accept(T_decrement))
        prefix_op = OP_sub;
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
            type = find_type(next_ident, 0);
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
            type = find_type(token, find_type_flag);
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
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());
        if (is_extern) {
            if (var->is_func || lex_peek(T_open_bracket, NULL)) {
                /* The global helper owns function redeclaration compatibility
                 * and parameter parsing. Unlike an object declaration it
                 * consumes the trailing semicolon itself.
                 */
                parent->locals.size--;
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
        add_insn(is_static ? GLOBAL_BLOCK : parent,
                 is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, var, NULL, NULL,
                 0, NULL);
        add_symbol(bb, var);
        if (lex_accept(T_assign)) {
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
            } else if (var->has_unsized_array && !var->ptr_level &&
                       var->type == TY_char && lex_peek(T_string, NULL)) {
                parse_string_array_init(var, parent, &bb);
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
                read_expr(parent, &bb);
                read_ternary_operation(parent, &bb);

                var_t *expr_result = opstack_pop();

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
            add_insn(is_static ? GLOBAL_BLOCK : parent,
                     is_static ? GLOBAL_FUNC->bbs : bb, OP_allocat, nv, NULL,
                     NULL, 0, NULL);
            add_symbol(bb, nv);
            if (lex_accept(T_assign)) {
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
                } else if (nv->has_unsized_array && !nv->ptr_level &&
                           nv->type == TY_char && lex_peek(T_string, NULL)) {
                    parse_string_array_init(nv, parent, &bb);
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
                    read_expr(parent, &bb);

                    emit_object_assignment(parent, &bb, nv, opstack_pop());
                }
            }
            if (is_static)
                discard_global_declarator_operand(nv);
        }
        lex_expect(T_semicolon);
        return bb;
    }

    /* is a function call? Skip function call check when has_asterisk is true */
    var_t *local = find_local_var(token, parent);
    if (!has_asterisk && (!local || local->is_extern_function_alias)) {
        func = find_func(token);
        if (func) {
            lex_expect(T_identifier);
            read_func_call(func, parent, &bb);
            perform_side_effect(parent, bb);
            lex_expect(T_semicolon);
            return bb;
        }
    }

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

            if (addr->is_const_qualified ||
                (addr->ptr_level > 1 && addr->ptr_level <= 32 &&
                 (addr->pointer_const_mask & (1U << (addr->ptr_level - 2)))))
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
    if (read_body_assignment(token, parent, prefix_op, &bb)) {
        perform_side_effect(parent, bb);
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

basic_block_t *read_body_statement(block_t *parent, basic_block_t *bb)
{
    if (!bb)
        printf("Warning: unreachable code detected\n");

    /* statement can be:
     *   function call, variable declaration, assignment operation,
     *   keyword, block
     */

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

    /* empty statement */
    if (lex_accept(T_semicolon))
        return bb;

    /* struct/union variable declaration */
    if (lex_peek(T_struct, NULL) || lex_peek(T_union, NULL))
        return handle_record_statement(parent, bb);

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

    if (func) {
        memcpy(&func_tmp, func, sizeof(func_t));
        check_decl = true;
    } else {
        func = add_func(var->var_name, false);
    }

    memcpy(&func->return_def, var, sizeof(var_t));
    if (check_decl && !func_tmp.is_static && is_static)
        error_at("static declaration follows non-static declaration",
                 next_token_loc());
    func->is_static = check_decl && func_tmp.is_static ? true : is_static;
    var_reset_subscripts(&func->return_def);
    block->locals.size--;
    read_parameter_list_decl(func, 0);

    if (check_decl) {
        if (!compatible_decl_type(func->return_def.type,
                                  func_tmp.return_def.type) ||
            func->return_def.ptr_level != func_tmp.return_def.ptr_level ||
            func->return_def.is_const_qualified !=
                func_tmp.return_def.is_const_qualified)
            error_at("conflicting types for function declaration",
                     next_token_loc());
        if (func->num_params != func_tmp.num_params ||
            func->va_args != func_tmp.va_args)
            error_at("conflicting types for function declaration",
                     next_token_loc());
        for (int i = 0; i < func->num_params; i++) {
            const var_t *now = &func->param_defs[i];
            const var_t *before = &func_tmp.param_defs[i];

            if (!compatible_decl_type(now->type, before->type) ||
                now->ptr_level != before->ptr_level ||
                now->is_const_qualified != before->is_const_qualified)
                error_at("conflicting types for function declaration",
                         next_token_loc());
        }
    }

    if (lex_peek(T_open_curly, NULL)) {
        if (check_decl && func_tmp.bbs)
            error_at("redefinition of function", next_token_loc());
        read_func_body(func);
        return;
    }
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
        previous->ptr_level != var->ptr_level ||
        previous->array_size != var->array_size ||
        previous->array_dim2 != var->array_dim2 ||
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
                            bool is_volatile)
{
    bool is_redeclaration;
    var_t *nv = require_typed_var(block, decl_type);
    nv->is_global = true;
    nv->is_static = is_static;
    nv->is_const_qualified = is_const;
    nv->is_volatile = is_volatile;
    read_inner_var_decl(nv, false, false, false);
    if (lex_peek(T_open_bracket, NULL)) {
        read_global_function_declarator(block, nv, is_static);
        return true;
    }
    nv = resolve_global_declarator(block, nv, is_static, &is_redeclaration);
    if (!is_redeclaration)
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, nv, NULL, NULL, 0, NULL);
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
    if (record_type->base_type == TYPE_typedef && record_type->base_struct)
        record_type = record_type->base_struct;

    lex_expect(T_open_curly);
    parse_struct_field_init(block, &GLOBAL_FUNC->bbs, record_type, var, true);
    lex_expect(T_close_curly);
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

    lex_expect(T_open_bracket);
    if (lex_accept(T_struct) || lex_accept(T_union)) {
        find_type_flag = 2;
        lex_ident(T_identifier, type_name);
    } else {
        lex_ident(T_identifier, type_name);
    }
    element_type = find_type(type_name, find_type_flag);
    lex_expect(T_open_square);

    array = require_typed_var(GLOBAL_BLOCK, element_type);
    array->var_name = gen_name();
    array->is_global = true;
    if (lex_peek(T_numeric, NULL)) {
        char bound[MAX_TOKEN_LEN];
        lex_ident_n(T_numeric, bound, MAX_TOKEN_LEN);
        array->array_size = parse_numeric_constant(bound);
        if (array->array_size <= 0)
            error_at("Array compound literal needs a positive bound",
                     cur_token_loc());
    }
    lex_expect(T_close_square);
    lex_expect(T_close_bracket);

    if (!element_type || element_type != var->type)
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
void read_global_record_declarator(block_t *block,
                                   type_t *decl_type,
                                   bool is_const,
                                   bool is_static)
{
    bool is_redeclaration;
    var_t *var = require_typed_var(block, decl_type);
    var->is_global = true;
    var->is_static = is_static;
    var->is_const_qualified = is_const;
    read_inner_var_decl(var, false, false, false);
    var = resolve_global_declarator(block, var, is_static, &is_redeclaration);
    if (!is_redeclaration)
        add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0, NULL);

    if (!lex_accept(T_assign)) {
        discard_global_declarator_operand(var);
        return;
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
}

void read_global_decl(block_t *block,
                      bool is_const,
                      bool is_static,
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

    if (lex_peek(T_open_bracket, NULL)) {
        read_global_function_declarator(block, var, is_static);
        return;
    } else {
        if (var->is_inline)
            error_at("inline specifier requires a function declarator",
                     next_token_loc());
        var =
            resolve_global_declarator(block, var, is_static, &is_redeclaration);
        if (!is_redeclaration)
            add_insn(block, GLOBAL_FUNC->bbs, OP_allocat, var, NULL, NULL, 0,
                     NULL);
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
                               is_static, var->is_volatile);

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
            } else if (lex_peek(T_char, NULL)) {
                lex_accept(T_char);
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

    if (lex_accept(T_struct)) {
        int i = 0, size = 0;
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);
        token_t *id_tk = cur_token;

        /* variable declaration using existing struct tag? */
        if (!lex_peek(T_open_curly, NULL)) {
            type_t *decl_type = find_type(token, 2);
            if (!decl_type)
                error_at("Unknown struct type", &id_tk->location);

            read_global_record_declarator(block, decl_type, is_const,
                                          is_static);
            while (lex_accept(T_comma))
                read_global_record_declarator(block, decl_type, is_const,
                                              is_static);
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
            reject_flexible_array_member_container(v);
            mark_flexible_array_member(v, false);
            v->offset = size;
            size += size_var(v);

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                if (last->is_flexible_array_member)
                    error_at(
                        "Flexible array member must be the final struct member",
                        cur_token_loc());
                var_t *nv = type_add_field(type, &i);
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                reject_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, false);
                last = nv;
                nv->offset = size;
                size += size_var(nv);
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

        type->size = size;
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        /* A record definition may be followed by its declarators, as in "struct
         * pair { int x, y; } first, *second;".
         */
        if (!lex_peek(T_semicolon, NULL)) {
            read_global_record_declarator(block, type, is_const, is_static);
            while (lex_accept(T_comma))
                read_global_record_declarator(block, type, is_const, is_static);
        }
        lex_expect(T_semicolon);
    } else if (lex_accept(T_union)) {
        int i = 0, max_size = 0;
        bool has_flexible_array_member = false;

        lex_ident(T_identifier, token);

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
            has_flexible_array_member |= is_flexible_array_member_container(v);
            mark_flexible_array_member(v, true);
            v->offset = 0; /* All union fields start at offset 0 */
            int field_size = size_var(v);
            if (field_size > max_size)
                max_size = field_size;

            /* Handle multiple variable declarations with same base type */
            while (lex_accept(T_comma)) {
                var_t *nv = type_add_field(type, &i);
                /* All union fields start at offset 0 */
                initialize_struct_field(nv, v, 0);
                read_inner_var_decl(nv, false, false, true);
                has_flexible_array_member |=
                    is_flexible_array_member_container(nv);
                mark_flexible_array_member(nv, true);
                field_size = size_var(nv);
                if (field_size > max_size)
                    max_size = field_size;
            }

            lex_expect(T_semicolon);
        } while (!lex_accept(T_close_curly));

        type->size = max_size;
        type->num_fields = i;
        type->has_flexible_array_member = has_flexible_array_member;

        if (!lex_peek(T_semicolon, NULL)) {
            read_global_record_declarator(block, type, is_const, is_static);
            while (lex_accept(T_comma))
                read_global_record_declarator(block, type, is_const, is_static);
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
                                       is_volatile))
                return;
            while (lex_accept(T_comma))
                read_global_declarator(block, type, is_const, is_static,
                                       is_volatile);
            lex_expect(T_semicolon);
            return;
        }
        type = has_tag ? find_type(token, true) : NULL;
        if (!type)
            type = add_type();

        type->base_type = TYPE_int;
        type->size = 4;
        if (has_tag)
            set_type_name(type, token);
        lex_expect(T_open_curly);
        do {
            lex_ident(T_identifier, token);
            if (lex_accept(T_assign))
                val = read_const_expr(block);
            add_constant(token, val++);
        } while (lex_accept(T_comma));
        lex_expect(T_close_curly);
        if (!lex_peek(T_semicolon, NULL)) {
            if (read_global_declarator(block, type, is_const, is_static,
                                       is_volatile))
                return;
            while (lex_accept(T_comma))
                read_global_declarator(block, type, is_const, is_static,
                                       is_volatile);
        }
        lex_expect(T_semicolon);
    } else if (lex_accept(T_typedef)) {
        if (lex_accept(T_enum)) {
            int val = 0;
            type_t *type = add_type();

            type->base_type = TYPE_int;
            type->size = 4;
            lex_expect(T_open_curly);
            do {
                lex_ident(T_identifier, token);
                if (lex_accept(T_assign))
                    val = read_const_expr(block);
                add_constant(token, val++);
            } while (lex_accept(T_comma));
            lex_expect(T_close_curly);
            lex_ident(T_identifier, token);
            set_type_name(type, token);
            lex_expect(T_semicolon);
        } else if (lex_accept(T_struct)) {
            int i = 0, size = 0;
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
                    reject_flexible_array_member_container(v);
                    mark_flexible_array_member(v, false);
                    v->offset = size;
                    size += size_var(v);

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
                        reject_flexible_array_member_container(nv);
                        mark_flexible_array_member(nv, false);
                        last = nv;
                        nv->offset = size;
                        size += size_var(nv);
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
            type->size = type->ptr_level ? PTR_SIZE : size;
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
            int i = 0, max_size = 0;
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
                    has_flexible_array_member |=
                        is_flexible_array_member_container(v);
                    mark_flexible_array_member(v, true);
                    v->offset = 0; /* All union fields start at offset 0 */
                    int field_size = size_var(v);
                    if (field_size > max_size)
                        max_size = field_size;

                    /* Handle multiple variable declarations with same base type
                     */
                    while (lex_accept(T_comma)) {
                        var_t *nv = type_add_field(type, &i);
                        /* All union fields start at offset 0 */
                        initialize_struct_field(nv, v, 0);
                        read_inner_var_decl(nv, false, false, true);
                        has_flexible_array_member |=
                            is_flexible_array_member_container(nv);
                        mark_flexible_array_member(nv, true);
                        field_size = size_var(nv);
                        if (field_size > max_size)
                            max_size = field_size;
                    }

                    lex_expect(T_semicolon);
                } while (!lex_accept(T_close_curly));
            }

            while (lex_accept(T_asterisk)) {
                type->ptr_level++;
                type->size = PTR_SIZE;
            }
            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
            type->size = type->ptr_level ? PTR_SIZE : max_size;
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

            if (is_long) {
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
            type->num_fields = 0;
            type->ptr_level = base->ptr_level;
            type->pointer_const_mask = base->pointer_const_mask;
            type->is_const_qualified =
                typedef_const || base->is_const_qualified;
            type->is_unsigned = base->is_unsigned;
            type->is_signed_char = base->is_signed_char;

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

            lex_ident_n(T_identifier, type->type_name, MAX_TYPE_LEN);
            lex_expect(T_semicolon);
        }
    } else if (lex_peek(T_identifier, NULL) || lex_peek(T_signed, NULL) ||
               lex_peek(T_unsigned, NULL) || lex_peek(T_long, NULL)) {
        read_global_decl(block, is_const, is_static, is_inline, is_volatile);
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

    /* builtin type _Bool was introduced in C99 specification, it is more
     * well-known as macro type bool, which is defined in <std_bool.h> (in
     * shecc, it is defined in 'lib/c.c').
     */
    TY_bool = add_named_type("_Bool");
    TY_bool->base_type = TYPE_char;
    TY_bool->size = 1;

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
}

void parse(token_t *tk)
{
    token_t head;
    head.kind = T_start;
    head.next = tk;
    cur_token = &head;

    parse_internal();
}
