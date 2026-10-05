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

/* Diagnose the conversion of the function designator @symbol to @var, or of the
 * enclosing cast to a function pointer type when there is one.
 */
static void diagnose_global_function_conversion(var_t *symbol, var_t *var)
{
    var_t cast = {0};

    if (!global_function_cast_signature) {
        diagnose_function_pointer_conversion(symbol, var);
        return;
    }
    cast.type = global_function_cast_signature->return_def.type;
    cast.ptr_level = 1;
    cast.func_signature = global_function_cast_signature;
    diagnose_function_pointer_conversion(&cast, var);
}

/* The integer evaluators below yield constants only, so a nonzero value for a
 * pointer object, a function pointer among them, is an integer converted
 * without a cast. Under a cast to a function pointer type, the conversion is
 * the explicit one the cast performs.
 */
static void reject_global_integer_pointer(const var_t *dest, const var_t *src)
{
    if ((effective_pointer_depth(dest) || dest->is_func) && !dest->array_size) {
        if (global_function_cast_signature)
            diagnose_global_function_conversion((var_t *) src, (var_t *) dest);
        else if (!is_pointer_like_value((var_t *) src) && !src->is_func &&
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

/* Whether a subscripted string literal, as in `"ab"[1]`, appears in the static
 * initializer that starts at @token. Only the literal expression reader folds
 * it; integer constant expressions such as enumerators do not admit it.
 */
bool string_element_appears_before_initializer_end(token_t *token)
{
    int depth = 0;

    for (; token; token = token->next) {
        if ((token->kind == T_string || token->kind == T_wstring) &&
            token->next && token->next->kind == T_open_square)
            return true;
        if (!initializer_scan_continue(token, &depth, true))
            return false;
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
    if (result->type && result->type->size <= TY_int->size)
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

    /* An address constant plus or minus an integer constant is itself an
     * address constant (C99 6.6p7), with the integer first in a sum as well.
     */
    if ((op == OP_add || op == OP_sub) && right) {
        var_t *address = wide_global_address_operand(left) ? left
                         : op == OP_add                    ? right
                                                           : NULL;
        var_t *index = address == left ? right : left;
        int stride = !address || address->is_func ? 0
                     : address->is_string_literal ? address->type->size
                                                  : address->address_stride;

        if (stride && wide_global_address_operand(address) &&
            !wide_global_address_operand(index)) {
            var_t *offset = require_var(parent);
            int step = narrow_wide_global_address_offset(index);

            if (step > INT_MAX / stride || step < -(INT_MAX / stride))
                error_at(
                    "Global address offset exceeds supported integer range",
                    cur_token_loc());
            offset->var_name = gen_name();
            offset->init_val = step * (op == OP_sub ? -stride : stride);
            add_insn(parent, bb, OP_load_constant, offset, NULL, NULL, 0, NULL);
            result->type = address->type;
            result->ptr_level = address->ptr_level;
            result->is_global_address = address->is_global_address;
            result->is_string_literal = address->is_string_literal;
            result->is_const_qualified = address->is_const_qualified;
            result->pointee_func_signature = address->pointee_func_signature;
            result->address_stride = address->address_stride;
            add_insn(parent, bb, OP_add, result, address, offset, 0, NULL);
            return true;
        }
    }

    /* An address constant is otherwise an operand only of the operators that
     * test it. Other arithmetic on one is not a constant expression here.
     */
    if ((wide_global_address_operand(left) ||
         (right && wide_global_address_operand(right))) &&
        op != OP_log_and && op != OP_log_or)
        error_at("Global initializer requires a constant value",
                 cur_token_loc());

    /* Materialize the usual arithmetic conversion in word form before any
     * operation reads a high word, rather than trusting the operand's stored
     * one: an enumeration constant never sets it. When the common type is
     * int-sized the operands convert to that type, so an int meeting an
     * unsigned int zero-extends; otherwise each narrow operand extends by its
     * own signedness. A shift converts only its left operand, by promotion.
     */
    if (right && op != OP_lshift && op != OP_rshift) {
        type_t *common = integer_common_type(left, right);
        bool narrow_unsigned =
            common->size <= TY_int->size && common->is_unsigned;

        if (right->type->size <= TY_int->size)
            rhs_hi = wide_global_narrow_high(
                rhs_lo, right->type->is_unsigned || narrow_unsigned);
        if (left->type->size <= TY_int->size)
            hi = wide_global_narrow_high(
                lo, left->type->is_unsigned || narrow_unsigned);
    } else if (left->type->size <= TY_int->size) {
        hi = wide_global_narrow_high(lo, left->type->is_unsigned);
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
        store_wide_global_word_result(parent, bb, result, comparison, 0);
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
        if (!wide_global_unevaluated_depth)
            add_insn(parent, bb, OP_log_not, result, value, NULL, 0, NULL);
        return result;
    }

    /* An address constant may be an operand here, as the arms of `1 ? "a" :
     * "b"` are. Every address form belongs to the aggregate constant reader,
     * which reads this one operand rather than the whole expression it is part
     * of. A subscripted literal such as `"ab"[1]` is an integer and was read
     * above.
     */
    if (!subscripted_string_literal_starts_here() &&
        (lex_peek(T_string, NULL) ||
         global_address_operand_starts_here(scope))) {
        basic_block_t *address_bb = bb;
        var_t *address;

        global_tested_operand_depth++;
        address = parse_global_constant_value(parent, &address_bb);
        global_tested_operand_depth--;
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
        if (operand->type->size <= TY_int->size)
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

        if (!constant)
            error_at("Typed global initializer needs a constant operand",
                     next_token_loc());
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
    result->type = integer_binary_result_type(op_stack[top], left, right);
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

var_t *read_wide_global_literal_expression(block_t *parent,
                                           basic_block_t *bb,
                                           block_t *scope)
{
    opcode_t op_stack[MAX_OPERATOR_STACK_SIZE];
    bool protected_rhs[MAX_OPERATOR_STACK_SIZE] = {0};
    var_t *val_stack[MAX_OPERATOR_STACK_SIZE];
    int op_stack_index = 0, val_stack_index = 0;
    opcode_t op;

    val_stack[val_stack_index++] = promote_wide_global_operand(
        parent, bb, read_wide_global_literal_primary(parent, bb, scope));
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

            /* An address constant has no arithmetic conversion to make with the
             * other arm; the selected one is the value.
             */
            if (wide_global_address_operand(when_true) ||
                wide_global_address_operand(when_false))
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

/* Skip the arm of a conditional expression that its constant condition
 * discards. The arm may itself hold grouped or nested conditionals, so count
 * the brackets and the '?' still waiting for their ':' rather than stopping at
 * the first ':'. A second operand ends at its matching ':'; a third ends at the
 * ',' or ';' ending the declarator, the ':' of an enclosing conditional or the
 * bracket closing an enclosing initializer.
 */
static void skip_discarded_conditional_arm(bool second_operand)
{
    int depth = 0;
    int pending = 0;

    for (;;) {
        token_t *next = cur_token->next;

        if (!next || next->kind == T_eof)
            return;
        if (next->kind == T_open_bracket || next->kind == T_open_square ||
            next->kind == T_open_curly) {
            depth++;
        } else if (next->kind == T_close_bracket ||
                   next->kind == T_close_square ||
                   next->kind == T_close_curly) {
            if (depth == 0)
                return;
            depth--;
        } else if (depth == 0 && next->kind == T_question) {
            pending++;
        } else if (depth == 0 && next->kind == T_colon) {
            if (pending == 0)
                return;
            pending--;
        } else if (depth == 0 && !second_operand &&
                   (next->kind == T_comma || next->kind == T_semicolon)) {
            return;
        }
        lex_next();
    }
}

void eval_ternary_imm(int cond, var_t *var)
{
    if (cond == 0) {
        skip_discarded_conditional_arm(true);
        lex_expect(T_colon);
        read_global_assignment_var(var);
    } else {
        read_global_assignment_var(var);
        lex_expect(T_colon);
        skip_discarded_conditional_arm(false);
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

    /* An address constant that an operator only tests, as in `"a" && 1`, is the
     * operand of a constant expression rather than this object's value.
     */
    if (global_tested_operand_starts_here(scope)) {
        var_t *tested = read_wide_global_literal_expression(parent, bb, scope);

        reject_global_integer_pointer(var, tested);
        add_insn(parent, bb, OP_assign, var, tested, NULL, 0, NULL);
        return true;
    }

    /* A cast to a function pointer type converts the function designator or
     * null pointer constant it applies to.
     */
    {
        func_t *cast_signature = read_global_function_pointer_cast(scope);

        if (cast_signature) {
            func_t *saved_signature = global_function_cast_signature;

            global_function_cast_signature =
                saved_signature ? saved_signature : cast_signature;
            read_global_assignment_var(var);
            global_function_cast_signature = saved_signature;
            return true;
        }
    }

    /* global initialization must be constant */
    if (global_pointer_cast_starts_here(scope)) {
        int saved_stride = global_pointer_cast_stride;
        func_t *slot_signature;
        int stride = read_global_pointer_cast(scope, &slot_signature);

        /* In a chain of casts the outermost one decides the stride. */
        if (saved_stride)
            stride = saved_stride;
        if (!global_address_operand_starts_here(scope)) {
            rs1 = read_global_cast_integer_address(parent, bb, scope, stride,
                                                   slot_signature);
            if (rs1->pointee_func_signature)
                diagnose_callback_slot_initializer(rs1, var);
            emit_global_scalar_assignment(parent, bb, var, rs1);
            return true;
        }
        global_pointer_cast_stride = stride;
        read_global_assignment_var(var);
        global_pointer_cast_stride = saved_stride;
        return true;
    }
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
            func_t *addressed = find_visible_func(
                address_dereference_identifier->literal, scope);
            if (!addressed)
                error_at("Function address requires a visible declaration",
                         cur_token_loc());
            if (!var->is_func && !var->ptr_level &&
                !(var->type && var->type->ptr_level))
                error_at("Function address requires a pointer initializer",
                         cur_token_loc());
            addr = name_var(require_ref_var(parent, var->type, var->ptr_level));
            symbol = function_designator_value(parent, addressed);
            diagnose_global_function_conversion(symbol, var);
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
                var_t *symbol = function_designator_value(parent, func);

                addr->var_name = gen_name();
                diagnose_global_function_conversion(symbol, var);
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
                var_t *object_addr = name_var(
                    require_ref_var(parent, object->type, object->ptr_level));
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
                if (!explicit_address && object->array_dim2 &&
                    lex_peek(T_open_square, NULL)) {
                    object_addr = read_global_address_designator(
                        scope, parent, &bb, &object, object_addr, true);
                } else if (!explicit_address && object->array_size &&
                           (lex_peek(T_plus, NULL) ||
                            lex_peek(T_minus, NULL))) {
                    int index = read_global_address_offset(scope, parent, bb);
                    int stride = object->ptr_level || object->is_func
                                     ? PTR_SIZE
                                     : object->type->size;
                    var_t *byte_offset = require_var(parent);
                    var_t *offset_addr = name_var(require_ref_var(
                        parent, object->type, object->ptr_level));

                    stride = global_pointer_cast_stride
                                 ? global_pointer_cast_stride
                                 : fixed_array_shape_stride(&shape, 0, stride);
                    byte_offset->var_name = gen_name();
                    byte_offset->init_val = index * stride;
                    add_insn(parent, bb, OP_load_constant, byte_offset, NULL,
                             NULL, 0, NULL);
                    offset_addr->is_global_address = true;
                    offset_addr->pointee_func_signature =
                        object_addr->pointee_func_signature;
                    add_insn(parent, bb, OP_add, offset_addr, object_addr,
                             byte_offset, 0, NULL);
                    object_addr = offset_addr;
                }
                if (explicit_address)
                    object_addr = read_global_address_designator(
                        scope, parent, &bb, &object, object_addr, false);
                diagnose_callback_slot_initializer(object_addr, var);
                add_insn(parent, bb, OP_assign, var, object_addr, NULL, 0,
                         NULL);
                return true;
            }
        }
        if (explicit_address && subscripted_string_literal_starts_here()) {
            rs1 = read_string_literal_element_address(parent, bb, scope);
            diagnose_const_pointer_conversion(rs1, var);
            emit_global_scalar_assignment(parent, bb, var, rs1);
            return true;
        }
        if (explicit_address && scope == GLOBAL_BLOCK &&
            global_compound_literal_starts_here()) {
            /* A compound literal at file scope has static storage (C99
             * 6.5.2.5p6), so `&(int){8}` is an address constant. Give the
             * literal an unnamed global and initialize the pointer with its
             * address.
             */
            type_t *literal_type;
            var_t return_decl;
            var_t *literal;
            var_t *literal_addr;
            int literal_ptr_level = 0;
            func_t *callback = NULL;

            lex_expect(T_open_bracket);
            read_type_name_decl(GLOBAL_BLOCK, &return_decl);
            literal_type = return_decl.type;
            literal_ptr_level = return_decl.ptr_level;

            /* `&(int (*)(int)){f}` and `&(callback_t){f}` address a callback
             * object; the pointer they initialize is a callback slot.
             */
            if (literal_type && abstract_function_pointer_follows()) {
                int callback_level;

                callback = read_abstract_function_pointer(&return_decl,
                                                          &callback_level);
                if (callback_level != 1)
                    error_at("Incompatible compound literal address",
                             cur_token_loc());
            } else if (literal_type && literal_type->func_signature &&
                       !literal_type->is_direct_function_type &&
                       !literal_type->array_size && !literal_ptr_level) {
                callback = literal_type->func_signature;
            }
            lex_expect(T_close_bracket);
            if (callback) {
                func_t *slot = var->pointee_func_signature;

                if (!slot && var->ptr_level == 1 && var->type->func_signature &&
                    !var->type->is_direct_function_type)
                    slot = var->type->func_signature;
                if (!slot || var->array_size ||
                    !compatible_function_signature(slot, callback))
                    error_at("Incompatible compound literal address",
                             cur_token_loc());
                literal =
                    name_var(require_typed_var(GLOBAL_BLOCK, literal_type));
                literal->is_global = true;
                literal->ptr_level =
                    literal_type->func_signature ? 0 : literal_ptr_level;
                literal->is_func = true;
                literal->func_signature = callback;
                add_insn(GLOBAL_BLOCK, bb, OP_allocat, literal, NULL, NULL, 0,
                         NULL);
                lex_expect(T_open_curly);
                if (lex_peek(T_close_curly, NULL))
                    error_at("Scalar compound literal needs an initializer",
                             next_token_loc());
                read_global_assignment_var(literal);
                lex_accept(T_comma);
                lex_expect(T_close_curly);
                literal_addr = name_var(
                    require_ref_var(parent, literal->type, literal->ptr_level));
                literal_addr->is_global_address = true;
                literal_addr->pointee_func_signature = callback;
                add_insn(parent, bb, OP_address_of, literal_addr, literal, NULL,
                         0, NULL);
                add_insn(parent, bb, OP_assign, var, literal_addr, NULL, 0,
                         NULL);
                return true;
            }
            if (!literal_type || literal_type->array_size ||
                literal_type->func_signature || var->array_size ||
                var->ptr_level != literal_ptr_level + 1 ||
                !(compatible_decl_type(var->type, literal_type) ||
                  (var->type == TY_void && !literal_ptr_level)))
                error_at("Incompatible compound literal address",
                         cur_token_loc());
            literal = name_var(require_typed_var(GLOBAL_BLOCK, literal_type));
            literal->is_global = true;
            literal->ptr_level = literal_ptr_level;
            add_insn(GLOBAL_BLOCK, bb, OP_allocat, literal, NULL, NULL, 0,
                     NULL);
            if (!literal_ptr_level && is_record_type(literal_type)) {
                parse_global_record_init(literal, GLOBAL_BLOCK);
            } else {
                lex_expect(T_open_curly);
                if (lex_peek(T_close_curly, NULL))
                    error_at("Scalar compound literal needs an initializer",
                             next_token_loc());
                read_global_assignment_var(literal);
                lex_accept(T_comma);
                lex_expect(T_close_curly);
            }
            literal_addr = name_var(
                require_ref_var(parent, literal->type, literal->ptr_level));
            literal_addr->is_global_address = true;
            add_insn(parent, bb, OP_address_of, literal_addr, literal, NULL, 0,
                     NULL);
            add_insn(parent, bb, OP_assign, var, literal_addr, NULL, 0, NULL);
            return true;
        }
        if (explicit_address)
            error_at("Expected a global object or function after '&'",
                     cur_token_loc());
        if (string_address_offset_starts_here()) {
            rs1 = read_string_address_offset(parent, bb, scope);
            diagnose_const_pointer_conversion(rs1, var);
            emit_global_scalar_assignment(parent, bb, var, rs1);
            return true;
        }

        /* The legacy global evaluator stores operands in int. Parse a wide
         * literal-only expression separately so its upper payload survives;
         * lower each reduction into the global setup block instead of trying to
         * narrow the expression through that evaluator.
         */
        if (initializer_needs_wide_reader(cur_token->next, scope, true) ||
            string_element_appears_before_initializer_end(cur_token->next)) {
            rs1 = read_wide_global_literal_expression(parent, bb, scope);
            reject_global_integer_pointer(var, rs1);
            add_insn(parent, bb, OP_assign, var, rs1, NULL, 0, NULL);
            return true;
        }
        if ((lex_peek(T_string, NULL) || lex_peek(T_wstring, NULL)) &&
            !subscripted_string_literal_starts_here()) {
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

        bool has_ternary;
        int value = read_const_expr_until_ternary(scope, &has_ternary);

        if (has_ternary) {
            lex_expect(T_question);
            eval_ternary_imm(value, var);
        } else {
            rs1 = load_constant(parent, bb, value, TY_int);
            emit_global_scalar_assignment(parent, bb, var, rs1);
        }
        return true;
    }
    return false;
}
