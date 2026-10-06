/*
 * shecc - Self-Hosting and Educational C Compiler.
 *
 * shecc is freely redistributable under the BSD 2 clause license. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/* Evaluation of constant expressions and address constants in file-scope and
 * static initializers.
 *
 * A fragment of the parser: parser.c includes it in order, so it sees every
 * definition that precedes it there and cannot be compiled on its own.
 */

/* Use the ordinary sizeof expression parser in a detached block. Global
 * initializers need the operand's type, never its generated instructions or
 * side effects. cur_token remains the already-consumed sizeof token.
 */
static int read_global_sizeof_expression(block_t *scope)
{
    basic_block_t *unevaluated_bb = bb_create(scope);
    var_t *result;

    handle_sizeof_operator(scope, &unevaluated_bb);
    result = opstack_pop();
    return result->init_val;
}

int read_const_wstring_size(void)
{
    int values[MAX_STRING_LEN];
    type_t *wide_type = find_builtin_type("wchar_t");
    return (read_wstring_units(values, MAX_STRING_LEN) + 1) * wide_type->size;
}

/* Evaluate a sizeof operator, already consumed, in an integer constant
 * expression: a file-scope or static initializer, an array bound, a case label
 * or an enumerator. A parenthesized type name goes through the declaration-only
 * evaluator. Every other operand goes through the ordinary sizeof parser in a
 * detached block, which keeps a declared extent through grouping, members,
 * subscripts, '*' and '&' exactly as a block-scope sizeof does.
 */
int read_sizeof_constant(block_t *scope)
{
    /* The detached expression parser needs a block to hold its values. */
    if (!scope)
        scope = GLOBAL_BLOCK;
    if (lex_peek(T_open_bracket, NULL) &&
        sizeof_cast_starts_at(scope, cur_token->next->next))
        return read_const_sizeof_type(scope);
    return read_global_sizeof_expression(scope);
}

int eval_expression_imm(opcode_t op, int op1, int op2)
{
    int res = 0;

    if (checking_enum_constant) {
        if (op == OP_add && ((op2 > 0 && op1 > INT_MAX - op2) ||
                             (op2 < 0 && op1 < INT_MIN - op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        if (op == OP_sub && ((op2 < 0 && op1 > INT_MAX + op2) ||
                             (op2 > 0 && op1 < INT_MIN + op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        if (op == OP_mul && op1 && op2 &&
            ((op1 > 0 && op2 > 0 && op1 > INT_MAX / op2) ||
             (op1 > 0 && op2 < 0 && op2 < INT_MIN / op1) ||
             (op1 < 0 && op2 > 0 && op1 < INT_MIN / op2) ||
             (op1 < 0 && op2 < 0 && op1 < INT_MAX / op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
        if (op == OP_lshift &&
            (op2 < 0 || op2 >= 32 || op1 < 0 || op1 > (INT_MAX >> op2)))
            error_at("Enumerator value exceeds int range", cur_token_loc());
    }
    if (op == OP_div || op == OP_mod) {
        if (!op2)
            error_at(op == OP_div ? "Division by zero in constant expression"
                                  : "Modulo by zero in constant expression",
                     cur_token_loc());
        if (op1 == INT_MIN && op2 == -1)
            error_at("Overflow in constant expression", cur_token_loc());
    }
    if ((op == OP_lshift || op == OP_rshift) && (op2 < 0 || op2 >= 32))
        error_at("Shift count out of range in constant expression",
                 cur_token_loc());
    if (!vir_frontend_fold_i32(op, op1, op2, &res))
        error_at("The requested operation is not supported.", cur_token_loc());
    return res;
}

bool read_global_assignment_var(var_t *var);

/* The integer evaluators below yield constants only, so a nonzero value for a
 * pointer object, a function pointer among them, is an integer converted
 * without a cast. Explicit pointer casts are retained on the source value.
 */
static void reject_global_integer_pointer(const var_t *dest, const var_t *src)
{
    if ((effective_pointer_depth(dest) || dest->is_func) && !dest->array_size) {
        if (!is_pointer_like_value((var_t *) src) && !src->is_func &&
            !src->is_string_literal && (src->init_val || src->init_val_hi))
            error_at("integer converted to pointer without a cast",
                     cur_token_loc());
    }
}

void emit_global_scalar_assignment(block_t *parent,
                                   basic_block_t *bb,
                                   var_t *dest,
                                   var_t *src)
{
    reject_global_integer_pointer(dest, src);
    if (is_bool_scalar(dest->type, dest->ptr_level))
        src->init_val = src->init_val != 0;
    add_insn(parent, bb, OP_assign, dest, src, NULL, 0, NULL);
}

/* Return whether token, an opening parenthesis, begins a cast to an integer
 * type: nothing but integer type specifiers and qualifiers before its closing
 * parenthesis.
 */
static bool global_integer_cast_starts_at(token_t *token, block_t *scope)
{
    bool has_type = false;

    if (!token || token->kind != T_open_bracket)
        return false;
    for (token = token->next; token && token->kind != T_close_bracket;
         token = token->next) {
        if (token->kind == T_signed || token->kind == T_unsigned ||
            token->kind == T_long) {
            has_type = true;
        } else if (token->kind == T_identifier) {
            type_t *type = find_visible_type(token->literal, scope);

            if (!type || type->ptr_level || type->array_size ||
                type->is_floating || type->func_signature ||
                type->is_direct_function_type || is_record_type(type) ||
                type == TY_void)
                return false;
            has_type = true;
        } else if (token->kind != T_const && token->kind != T_volatile) {
            return false;
        }
    }
    return token && has_type;
}

/* Whether the initializer that starts at @token needs the two-word reader: some
 * token of it, before the ',' or ';' that ends it outside any parenthesis it
 * contains or the ')' that closes an enclosing one, is a wide or typed literal,
 * or with @casts an integer cast in @scope, of which the word-sized evaluator
 * has no notion.
 */
static bool initializer_needs_wide_reader(token_t *token,
                                          block_t *scope,
                                          bool casts)
{
    int depth = 0;

    for (; token; token = token->next) {
        if ((token->kind == T_numeric &&
             numeric_literal_needs_global_reader(token->literal)) ||
            (casts && global_integer_cast_starts_at(token, scope)))
            return true;
        if (!initializer_scan_continue(token, &depth, false))
            return false;
    }
    return false;
}

/* Keep the legacy word-sized evaluator for ordinary constants and casts, but
 * select the two-word path whenever a literal-only initializer contains a wide
 * token. Looking past leading narrow operands matters for expressions such as
 * `(3 + 0x100000000LL)`: the old evaluator would consume the later token before
 * it had a chance to preserve its high word.
 */
bool typed_global_literal_appears_before_initializer_end(token_t *token)
{
    return initializer_needs_wide_reader(token, NULL, false);
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

/* The high word an int-sized value of this type has once it is widened: a
 * signed source sign-extends and an unsigned source zero-extends.
 */
static unsigned int wide_global_narrow_high(unsigned int lo, bool is_unsigned)
{
    return is_unsigned || !(lo & 0x80000000U) ? 0 : 0xffffffffU;
}

/* Whether @value is an address constant: a string literal, the address of a
 * global object or a function designator. Such a value is never null, so the
 * operators that only test it know its truth without its address.
 */
static bool wide_global_address_operand(const var_t *value)
{
    return value->is_string_literal || value->is_global_address ||
           value->is_func;
}

static bool wide_global_operand_is_true(const var_t *value)
{
    return wide_global_address_operand(value) ||
           (value->init_val || value->init_val_hi);
}

/* Record a folded result. An int-sized result keeps only its low word, so a
 * carry, borrow or product overflow computed in the high word cannot leak into
 * a later wide reduction or a truth test of this value.
 */
static void store_wide_global_word_result(block_t *parent,
                                          basic_block_t *bb,
                                          var_t *result,
                                          unsigned int lo,
                                          unsigned int hi)
{
    if (is_pointer_like_value(result))
        hi = PTR_SIZE == 8 ? hi : 0;
    else if (result->type && result->type->size <= TY_int->size)
        hi = wide_global_narrow_high(lo, result->type->is_unsigned);
    result->init_val = lo;
    result->init_val_hi = hi;
    result->is_const = true;
    add_insn(parent, bb, OP_load_constant, result, NULL, NULL, 0, NULL);
}

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

    if ((op == OP_lshift || op == OP_rshift) &&
        (rhs_hi || rhs_lo >= (left->type->size <= TY_int->size ? 32u : 64u)))
        error_at("Shift count out of range in constant expression",
                 cur_token_loc());

    /* An address constant plus or minus an integer constant is itself an
     * address constant (C99 6.6p7), with the integer first in a sum as well.
     */
    if ((op == OP_add || op == OP_sub) && right) {
        var_t *address =
            (wide_global_address_operand(left) || is_pointer_like_value(left))
                ? left
            : op == OP_add ? right
                           : NULL;
        var_t *index = address == left ? right : left;
        int stride = !address || address->is_func ? 0
                     : address->address_stride    ? address->address_stride
                     : address->is_string_literal ? address->type->size
                                                  : 0;

        if (stride &&
            (wide_global_address_operand(address) ||
             is_pointer_like_value(address)) &&
            !wide_global_address_operand(index) &&
            !is_pointer_like_value(index)) {
            int step = narrow_wide_global_address_offset(index);

            if (step > INT_MAX / stride || step < -(INT_MAX / stride))
                error_at(
                    "Global address offset exceeds supported integer range",
                    cur_token_loc());
            int delta = step * (op == OP_sub ? -stride : stride);
            result->type = address->type;
            result->ptr_level = address->ptr_level;
            result->is_global_address = address->is_global_address;
            result->is_string_literal = address->is_string_literal;
            result->is_const_qualified = address->is_const_qualified;
            result->is_const_pointer = address->is_const_pointer;
            result->pointer_const_mask = address->pointer_const_mask;
            result->is_volatile = address->is_volatile;
            result->pointer_volatile_mask = address->pointer_volatile_mask;
            result->pointee_func_signature = address->pointee_func_signature;
            result->address_stride = address->address_stride;
            if (!wide_global_address_operand(address)) {
                unsigned int low = (unsigned int) address->init_val + delta;
                unsigned int high = (unsigned int) address->init_val_hi +
                                    (delta < 0 ? ~0U : 0) +
                                    (low < (unsigned int) address->init_val);
                store_wide_global_word_result(parent, bb, result, low, high);
            } else {
                var_t *offset = load_constant(parent, bb, delta, TY_int);
                add_insn(parent, bb, OP_add, result, address, offset, 0, NULL);
            }
            return true;
        }
    }

    /* An address constant is otherwise an operand only of the operators that
     * test it. Other arithmetic on one is not a constant expression here.
     */
    bool raw_pointer =
        is_pointer_like_value(left) || (right && is_pointer_like_value(right));
    bool comparison = op_is_comparison(op);
    if ((wide_global_address_operand(left) ||
         (right && wide_global_address_operand(right)) || raw_pointer) &&
        op != OP_log_and && op != OP_log_or &&
        !(raw_pointer && comparison && !wide_global_address_operand(left) &&
          (!right || !wide_global_address_operand(right))))
        error_at("Global initializer requires a constant value",
                 cur_token_loc());

    /* Materialize the usual arithmetic conversion in word form before any
     * operation reads a high word, rather than trusting the operand's stored
     * one: an enumeration constant never sets it. When the common type is
     * int-sized the operands convert to that type, so an int meeting an
     * unsigned int zero-extends; otherwise each narrow operand extends by its
     * own signedness. A shift converts only its left operand, by promotion.
     */
    if (right && op != OP_lshift && op != OP_rshift &&
        !is_pointer_like_value(left) && !is_pointer_like_value(right)) {
        type_t *common = integer_common_type(left, right);
        bool narrow_unsigned =
            common->size <= TY_int->size && common->is_unsigned;

        if (right->type->size <= TY_int->size)
            rhs_hi = wide_global_narrow_high(
                rhs_lo, right->type->is_unsigned || narrow_unsigned);
        if (left->type->size <= TY_int->size)
            hi = wide_global_narrow_high(
                lo, left->type->is_unsigned || narrow_unsigned);
    } else if (!is_pointer_like_value(left) &&
               left->type->size <= TY_int->size) {
        hi = wide_global_narrow_high(lo, left->type->is_unsigned);
    }

    /* The restricted global evaluator selects a conditional arm while parsing,
     * so comparison results must carry their constant payload rather than exist
     * only as setup-block IR. Keep this in the compiler's existing two-word
     * representation: it must also self-host on targets without a complete
     * host-level 64-bit value ABI.
     */
    if (comparison) {
        if (is_pointer_like_value(left) != is_pointer_like_value(right)) {
            var_t *integer = is_pointer_like_value(left) ? right : left;
            if (!integer->is_const || integer->init_val || integer->init_val_hi)
                error_at("Global pointer comparison requires a null constant",
                         cur_token_loc());
        }
        bool is_unsigned = is_pointer_like_value(left) ||
                           is_pointer_like_value(right) ||
                           integer_common_type(left, right)->is_unsigned;
        pp_integer_t lhs = {.lo = lo, .hi = hi, .is_unsigned = is_unsigned};
        pp_integer_t rhs = {
            .lo = rhs_lo, .hi = rhs_hi, .is_unsigned = is_unsigned};
        store_wide_global_word_result(parent, bb, result,
                                      pp_compare(op, &lhs, &rhs), 0);
        return true;
    }

    if (op == OP_log_and || op == OP_log_or) {
        bool left_true = wide_global_operand_is_true(left);
        bool right_true = right && wide_global_operand_is_true(right);

        store_wide_global_word_result(parent, bb, result,
                                      op == OP_log_and
                                          ? left_true && right_true
                                          : left_true || right_true,
                                      0);
        return true;
    }

    if (op == OP_div || op == OP_mod) {
        /* The usual arithmetic conversions choose the signedness: a signed long
         * long divided by an unsigned int stays signed long long.
         */
        type_t *common = integer_common_type(left, right);
        bool is_unsigned = common->is_unsigned;
        pp_integer_t dividend = {
            .lo = lo, .hi = hi, .is_unsigned = is_unsigned};
        pp_integer_t divisor = {
            .lo = rhs_lo, .hi = rhs_hi, .is_unsigned = is_unsigned};

        if (rhs_lo == 0 && rhs_hi == 0)
            error_at("division by zero in global constant expression",
                     cur_token_loc());

        /* The minimum of the signed common type divided by -1 has no
         * representable quotient, and its remainder is undefined as well.
         * Diagnose it as the word-sized evaluator does INT_MIN / -1.
         */
        if (!is_unsigned && rhs_lo == 0xffffffffU && rhs_hi == 0xffffffffU &&
            (common->size <= TY_int->size ? lo == 0x80000000U
                                          : hi == 0x80000000U && lo == 0))
            error_at("Overflow in constant expression", cur_token_loc());
        pp_divmod(&dividend, &divisor, op == OP_mod);
        store_wide_global_word_result(parent, bb, result, dividend.lo,
                                      dividend.hi);
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
    store_wide_global_word_result(parent, bb, result, out_lo, out_hi);
    return true;
}

/* Apply the integer promotions to @value, an operand of a unary arithmetic
 * operator or of a shift or other binary operator. A value narrower than int
 * already holds its converted bit pattern in the low word; retyping it as int
 * lets the negation, complement or shift that follows extend its result as an
 * int rather than as the unsigned char or short the cast named.
 */
static var_t *promote_wide_global_operand(block_t *parent,
                                          basic_block_t *bb,
                                          var_t *value)
{
    var_t *promoted;

    /* An address constant has no integer promotion to make: its type names the
     * pointee, so a char pointer must not be retyped as the int its target
     * width would suggest.
     */
    if (value->ptr_level || wide_global_address_operand(value) ||
        value->type->size >= TY_int->size)
        return value;
    promoted = name_var(require_typed_var(parent, TY_int));
    promoted->init_val = value->init_val;
    promoted->init_val_hi = wide_global_narrow_high(value->init_val, false);
    promoted->is_const = true;
    add_insn(parent, bb, OP_load_constant, promoted, NULL, NULL, 0, NULL);
    return promoted;
}

/* Parse discarded operands for their type without emitting startup effects. */
static var_t *read_unevaluated_global_value(block_t *parent,
                                            basic_block_t *bb,
                                            block_t *scope,
                                            void (*reader)(block_t *,
                                                           basic_block_t **))
{
    basic_block_t *scratch = bb_create(scope);
    int saved_depth = wide_global_unevaluated_depth;
    wide_global_unevaluated_depth = 0;
    unevaluated_expression_depth++;
    reader(scope, &scratch);
    unevaluated_expression_depth--;
    wide_global_unevaluated_depth = saved_depth;
    var_t *operand = opstack_pop();
    var_t *value = name_var(
        require_typed_ptr_var(parent, operand->type, operand->ptr_level));
    value->is_func = operand->is_func;
    value->func_signature = operand->func_signature;
    value->pointee_func_signature = operand->pointee_func_signature;
    copy_call_result_array_shape(value, operand);
    fixed_array_shape_t shape = fixed_array_shape_from_var(operand);
    fixed_array_shape_to_var(value, &shape);
    value->has_unsized_array = operand->has_unsized_array;
    value->is_const = operand->is_const;
    value->init_val = operand->is_const ? operand->init_val : 0;
    value->init_val_hi = operand->is_const ? operand->init_val_hi : 0;
    add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
    return value;
}

var_t *read_wide_global_literal_primary(block_t *parent,
                                        basic_block_t *bb,
                                        block_t *scope)
{
    char literal[MAX_TOKEN_LEN];
    var_t *value;

    if (wide_global_unevaluated_depth)
        return read_unevaluated_global_value(parent, bb, scope,
                                             read_expr_operand);

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
        value = promote_wide_global_operand(
            parent, bb, read_wide_global_literal_primary(parent, bb, scope));
        zero->var_name = gen_name();
        zero->type = value->type;
        zero->init_val = 0;
        add_insn(parent, bb, OP_load_constant, zero, NULL, NULL, 0, NULL);
        result = require_named_var(parent);
        result->type = value->type;
        emit_wide_global_word_arithmetic(parent, bb, result, OP_sub, zero,
                                         value);
        return result;
    }
    if (lex_accept(T_bit_not)) {
        var_t *result;

        value = promote_wide_global_operand(
            parent, bb, read_wide_global_literal_primary(parent, bb, scope));
        result = require_named_var(parent);
        result->type = value->type;
        emit_wide_global_word_arithmetic(parent, bb, result, OP_bit_not, value,
                                         NULL);
        return result;
    }
    if (lex_accept(T_log_not)) {
        var_t *result;

        value = read_wide_global_literal_primary(parent, bb, scope);
        result = name_var(require_typed_var(parent, TY_int));
        result->init_val = !wide_global_operand_is_true(value);
        result->init_val_hi = 0;
        result->is_const = true;
        add_insn(parent, bb, OP_log_not, result, value, NULL, 0, NULL);
        return result;
    }

    func_t *cast_signature = read_global_function_pointer_cast(scope);
    if (cast_signature)
        return read_global_function_cast_value(parent, bb, scope,
                                               cast_signature);

    /* An address constant may be an operand here, as the arms of `1 ? "a" :
     * "b"` are. Every address form belongs to the aggregate constant reader,
     * which reads this one operand rather than the whole expression it is part
     * of. A subscripted literal such as `"ab"[1]` is an integer and was read
     * above.
     */
    if (!subscripted_string_literal_starts_here() &&
        (lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL) ||
         global_address_operand_starts_here(scope))) {
        basic_block_t *address_bb = bb;
        var_t *address;

        global_constant_context_t ctx = {0};
        ctx.scope = scope;
        address = read_global_constant_primary(parent, &address_bb, &ctx);
        return address;
    }
    if (lex_accept(T_sizeof)) {
        value =
            name_var(require_typed_var(parent, find_builtin_type("size_t")));
        value->init_val = read_sizeof_constant(scope);
        value->init_val_hi = 0;
        value->is_const = true;
        add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        return value;
    }
    if (global_integer_cast_starts_at(cur_token->next, scope)) {
        basic_block_t *unevaluated_bb = bb_create(parent);
        token_t *cast_start = cur_token;
        var_t *operand;
        type_t *type;
        unsigned int hi;

        /* A cast of a constant operand is still an integer constant expression.
         * The ordinary operand parser owns type-name syntax, so let it name the
         * target type in a detached block. Then return to the operand and read
         * it as a wide primary, so a grouped operand keeps its high word.
         */
        read_expr_operand(parent, &unevaluated_bb);
        operand = opstack_pop();
        type = operand->type;
        cur_token = cast_start;
        lex_expect(T_open_bracket);
        while (!lex_peek(T_close_bracket, NULL))
            cur_token = cur_token->next;
        lex_expect(T_close_bracket);

        operand = read_wide_global_literal_primary(parent, bb, scope);
        hi = (unsigned int) operand->init_val_hi;
        if (!is_pointer_like_value(operand) &&
            operand->type->size <= TY_int->size)
            hi = wide_global_narrow_high(operand->init_val,
                                         operand->type->is_unsigned);
        value = name_var(require_typed_var(parent, type));
        fold_integer_constant_cast(value, (unsigned int) operand->init_val, hi);
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

        if (!constant) {
            int operand = read_const_expr_operand(scope);
            value = load_constant(parent, bb, operand, TY_int);
            value->is_const = true;
            value->init_val_hi = wide_global_narrow_high(operand, false);
            return value;
        }
        lex_expect(T_identifier);
        value = name_var(require_typed_var(parent, TY_int));

        /* Every operand this reader returns carries its high word, the
         * extension of an int-sized value, so no consumer rebuilds it.
         */
        value->init_val = constant->value;
        value->init_val_hi = wide_global_narrow_high(constant->value, false);
        value->is_const = true;
        add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        return value;
    }
    if (subscripted_string_literal_starts_here()) {
        int element;

        /* An element of a narrow literal is an arithmetic constant in an
         * initializer (C99 6.6p10), as gcc folds it; gcc rejects a wide one.
         */
        if (lex_peek(T_wstring, NULL))
            error_at("Wide string literal element is not a constant",
                     next_token_loc());
        read_string_literal_element(scope, &element);
        value = name_var(require_typed_var(parent, TY_int));
        value->init_val = element;
        value->init_val_hi = wide_global_narrow_high(element, false);
        value->is_const = true;
        add_insn(parent, bb, OP_load_constant, value, NULL, NULL, 0, NULL);
        return value;
    }
    if (lex_peek(T_char, NULL) || lex_peek(T_wchar, NULL)) {
        /* A character constant is an integer constant expression operand like
         * any number. Reuse the expression readers so its value and type match
         * an ordinary expression; the high word is its sign extension.
         */
        if (lex_peek(T_wchar, NULL))
            read_wchar_param(parent, bb);
        else
            read_char_param(parent, bb);
        value = opstack_pop();
        value->init_val_hi = wide_global_narrow_high(value->init_val, false);
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
static void reduce_wide_global_top_op(block_t *parent,
                                      basic_block_t *bb,
                                      opcode_t *op_stack,
                                      bool *protected_rhs,
                                      var_t **val_stack,
                                      int *op_count,
                                      int *value_count)
{
    var_t *right = val_stack[--*value_count];
    var_t *left = val_stack[--*value_count];
    int top = --*op_count;
    var_t *result = require_var(parent);

    if (protected_rhs[top])
        wide_global_unevaluated_depth--;
    result->var_name = gen_name();
    result->type = op_stack[top] == OP_log_and || op_stack[top] == OP_log_or
                       ? TY_int
                       : integer_binary_result_type(op_stack[top], left, right);
    if (!emit_wide_global_word_arithmetic(parent, bb, result, op_stack[top],
                                          left, right))
        add_insn(parent, bb, op_stack[top], result, left, right, 0, NULL);
    val_stack[(*value_count)++] = result;
}

static void reduce_wide_global_pending_ops(block_t *parent,
                                           basic_block_t *bb,
                                           opcode_t *op_stack,
                                           bool *protected_rhs,
                                           var_t **val_stack,
                                           int *op_count,
                                           int *value_count)
{
    while (*op_count > 0)
        reduce_wide_global_top_op(parent, bb, op_stack, protected_rhs,
                                  val_stack, op_count, value_count);
}

var_t *read_wide_global_literal_tail(block_t *parent,
                                     basic_block_t *bb,
                                     block_t *scope,
                                     var_t *first)
{
    opcode_t op_stack[MAX_OPERATOR_STACK_SIZE];
    bool protected_rhs[MAX_OPERATOR_STACK_SIZE] = {0};
    var_t *val_stack[MAX_OPERATOR_STACK_SIZE];
    int op_stack_index = 0, val_stack_index = 0;
    opcode_t op;

    val_stack[val_stack_index++] =
        promote_wide_global_operand(parent, bb, first);
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
            reduce_wide_global_pending_ops(parent, bb, op_stack, protected_rhs,
                                           val_stack, &op_stack_index,
                                           &val_stack_index);
            condition = val_stack[--val_stack_index];
            condition_true = wide_global_operand_is_true(condition);

            /* A global conditional expression is still an integer constant
             * expression, but both arms undergo the usual arithmetic
             * conversions before the constant condition selects one. Parse both
             * through the wide reader so a discarded high word can never leak
             * through the legacy int-only evaluator.
             */
            lex_expect(T_question);
            when_true =
                !condition_true
                    ? read_unevaluated_global_value(parent, bb, scope,
                                                    read_control_expression)
                    : read_wide_global_literal_expression(parent, bb, scope);
            lex_expect(T_colon);
            when_false =
                condition_true
                    ? read_unevaluated_global_value(
                          parent, bb, scope, read_assignment_or_expression)
                    : read_wide_global_literal_expression(parent, bb, scope);

            /* An address constant has no arithmetic conversion to make with the
             * other arm; the selected one is the value.
             */
            bool true_pointer = wide_global_address_operand(when_true) ||
                                is_pointer_like_value(when_true) ||
                                when_true->has_unsized_array;
            bool false_pointer = wide_global_address_operand(when_false) ||
                                 is_pointer_like_value(when_false) ||
                                 when_false->has_unsized_array;
            if (true_pointer != false_pointer &&
                !is_null_pointer_constant(true_pointer ? when_false
                                                       : when_true))
                error_at(
                    "Conditional pointer operands must be pointers or null",
                    cur_token_loc());
            if (true_pointer || false_pointer)
                return condition_true ? when_true : when_false;
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
                   get_operator_prio(op))
            reduce_wide_global_top_op(parent, bb, op_stack, protected_rhs,
                                      val_stack, &op_stack_index,
                                      &val_stack_index);
        if (op_stack_index >= MAX_OPERATOR_STACK_SIZE ||
            val_stack_index >= MAX_OPERATOR_STACK_SIZE)
            fatal("Wide global initializer is too complex");

        /* An address constant is true without a value of its own, so `array
         * || 1 / 0` never evaluates its right operand either.
         */
        bool lhs_true =
            wide_global_operand_is_true(val_stack[val_stack_index - 1]);

        protected_rhs[op_stack_index] =
            op == OP_log_and ? !lhs_true : op == OP_log_or && lhs_true;
        op_stack[op_stack_index++] = op;
        if (protected_rhs[op_stack_index - 1]) {
            wide_global_unevaluated_depth++;
        }
        val_stack[val_stack_index++] = promote_wide_global_operand(
            parent, bb, read_wide_global_literal_primary(parent, bb, scope));
        op = get_operator();
    }
    reduce_wide_global_pending_ops(parent, bb, op_stack, protected_rhs,
                                   val_stack, &op_stack_index,
                                   &val_stack_index);
    return val_stack[0];
}

var_t *read_wide_global_literal_expression(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope)
{
    return read_wide_global_literal_tail(
        parent, bb, scope, read_wide_global_literal_primary(parent, bb, scope));
}

static void validate_global_compound_address(const var_t *dest,
                                             const var_t *literal)
{
    if (literal->is_func) {
        func_t *slot = dest->pointee_func_signature;
        if (!slot && dest->ptr_level == 1 && dest->type->func_signature &&
            !dest->type->is_direct_function_type)
            slot = dest->type->func_signature;
        if (!slot || dest->array_size ||
            !compatible_function_signature(slot, literal->func_signature))
            error_at("Incompatible compound literal address", cur_token_loc());
    } else if (dest->array_size || dest->ptr_level != literal->ptr_level + 1 ||
               !(compatible_decl_type(dest->type, literal->type) ||
                 (dest->type == TY_void && !literal->ptr_level))) {
        error_at("Incompatible compound literal address", cur_token_loc());
    }
}

bool read_global_assignment_var(var_t *var)
{
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
    global_constant_context_t ctx = {0};
    ctx.scope = scope;
    ctx.dest = var;
    var_t *value = read_global_constant_value(parent, &bb, &ctx);
    if (!value->is_func)
        diagnose_function_pointer_conversion(value, var);
    diagnose_callback_slot_initializer(value, var);
    if (value->is_string_literal)
        diagnose_const_pointer_conversion(value, var);
    emit_global_scalar_assignment(parent, bb, var, value);
    return true;
}
