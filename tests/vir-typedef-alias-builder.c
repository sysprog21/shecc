#include <stdarg.h>
typedef void void_alias_t;
typedef int integer_t, *pointer_t, row_t[2];
typedef row_t matrix_t[2];
typedef int callback_function_t(int);
typedef int (*callback_t)(int);
/* Retain the compiler's callable object alias extension at file scope. */
typedef callback_function_t callable_alias_t;
typedef int (**callback_slot_t)(int);
typedef int (*const *qualified_callback_slot_t)(int);
typedef int (*row_parameter_callback_t)(row_t rows[3]);
typedef callback_t callback_row_t[2];
typedef callback_row_t *callback_rows_t;
typedef pointer_t (*pointer_row_t)[2];
typedef const pointer_t constant_pointer_t;
typedef pointer_t pointer_array_t[2];
typedef char *text_t;
typedef text_t text_array_t[2];
struct incomplete;
typedef struct incomplete incomplete_t;
struct incomplete {
    int value;
};
typedef struct {
    int value;
} anonymous_t, *anonymous_pointer_t;
typedef int (*variadic_callback_t)(int, ...);
typedef struct incomplete (*record_callback_t)(void);
struct opaque;
typedef struct opaque (*opaque_callback_t)(void);
typedef int (*variadic_callbacks_t[2])(int, ...);
typedef struct incomplete (*record_callbacks_t[2])(void);
static int add(int x)
{
    return x + 3;
}
typedef int *volatile volatile_pointer_t;
typedef volatile_pointer_t pointer_realias_t;
typedef int (*volatile volatile_callback_t)(int);
typedef volatile_callback_t callback_realias_t;
typedef volatile_callback_t *volatile_callback_slot_t;
static pointer_realias_t qualified_pointer;
static callback_realias_t qualified_callback = add;
static callable_alias_t callable_alias = add;
int touch_alias_qualifiers(void)
{
    volatile_callback_slot_t slot = &qualified_callback;
    return *qualified_pointer + qualified_callback(1) + (*slot)(2);
}
static int check_pointer_array(int wanted, ...)
{
    va_list ap;
    va_start(ap, wanted);
    return *va_arg(ap, pointer_array_t *)[0][1] != wanted;
}
static int check_text_array(int ignored, ...)
{
    va_list ap;
    va_start(ap, ignored);
    return *va_arg(ap, text_array_t *)[0][1] != 'c';
}
typedef int *(**return_slot_t)(int);
typedef char *(*return_array_t[2])(void);
typedef pointer_t (*(*return_rows_t)[2])(int);
static int callback_return_value = 7;
static int *return_value(int ignored)
{
    return &callback_return_value;
}
static char *return_text(void)
{
    return "ok";
}
static row_t returned_row = {3, 4};
static row_t *return_row(void)
{
    return &returned_row;
}
typedef row_t *(*return_row_callback_t)(void);
typedef row_t *(*return_row_callbacks_t[2])(void);
typedef callback_t *(*callback_slot_rows_t)[2];
struct callback_result_byte {
    char value;
};
struct callback_result_wide {
    long long value;
};
typedef struct callback_result_byte (*byte_record_callback_t)(void);
typedef struct callback_result_wide (*wide_record_callback_t)(void);
struct byte_callback_holder {
    char tag;
    byte_record_callback_t callbacks[2];
};
struct wide_callback_holder {
    char tag;
    wide_record_callback_t callbacks[2];
};

static int check_callback_shapes(void)
{
    return_row_callback_t callback = return_row;
    return_row_callbacks_t callbacks = {return_row, return_row};
    callback_t item = add;
    callback_t *row[2] = {&item, &item};
    callback_slot_rows_t rows = &row;
    return_row_callback_t selected = callbacks[1];
    row_t *result = selected();
    return sizeof(struct byte_callback_holder) != 3 * sizeof(void *) ||
           sizeof(struct wide_callback_holder) != 3 * sizeof(void *) ||
           (*callback())[1] != 4 || (*result)[0] != 3 ||
           sizeof(return_row_callbacks_t) != 2 * sizeof(void *) ||
           (*(*rows)[1])(2) != 5;
}

static int check_callback_returns(void)
{
    int *(*callback)(int) = return_value;
    return_slot_t slot = &callback;
    return_array_t texts = {return_text, return_text};
    pointer_t (*row[2])(int) = {return_value, return_value};
    return_rows_t rows = &row;
    return *(*slot)(0) != 7 || texts[1]()[0] != 'o' ||
           sizeof(return_array_t) != 2 * sizeof(void *) || *(*rows)[1](0) != 7;
}

int main(void)
{
    integer_t value = 7;
    qualified_pointer = &value;
    if (touch_alias_qualifiers() != 16 || callable_alias(2) != 5)
        return 4;
    pointer_t pointer = &value;
    pointer_array_t pointers = {&value, &value};
    int pair[2] = {8, 5};
    pointer_array_t pairs = {&pair[1], &pair[0]};
    text_array_t words = {"ab", "cd"};
    if (check_pointer_array(7, &pointers) || check_pointer_array(8, &pairs) ||
        check_text_array(0, &words))
        return 7;
    void_alias_t *void_pointer = &value;
    if (!void_pointer)
        return 5;
    constant_pointer_t constant = &value;
    pointer_t row[2] = {&value, &value};
    pointer_row_t rows = &row;
    matrix_t matrix = {{1, 2}, {3, 4}};
    callback_t callback = add;
    callback_slot_t slot = &callback;
    qualified_callback_slot_t qualified_slot = &callback;
    if ((*qualified_slot)(2) != 5)
        return 6;
    callback_row_t callbacks = {add, add};
    callback_rows_t callback_rows = &callbacks;
    incomplete_t record = {9};
    anonymous_t anonymous = {11};
    anonymous_pointer_t anonymous_pointer = &anonymous;
    {
        typedef unsigned int integer_t, *local_pointer_t;
        integer_t local = 13;
        local_pointer_t local_pointer = &local;
        if (*local_pointer != 13)
            return 1;
    }
    {
        typedef int callback_function_t(int);
        typedef int (*callback_t)(int);
        typedef int row_t[2], *pointer_t;
        row_t local = {17, 19};
        pointer_t local_pointer = &local[1];
        callback_t local_callback = add;
        if (*local_pointer != 19 || local_callback(2) != 5)
            return 2;
    }
    if (sizeof(variadic_callback_t) != sizeof(void *) ||
        sizeof(record_callback_t) != sizeof(void *) ||
        sizeof(opaque_callback_t) != sizeof(void *) ||
        sizeof(variadic_callbacks_t) != 2 * sizeof(void *) ||
        sizeof(record_callbacks_t) != 2 * sizeof(void *))
        return 3;
    return check_callback_shapes() || check_callback_returns() ||
           *pointer != 7 || *constant != 7 || *(*rows)[1] != 7 ||
           sizeof(matrix_t) != 4 * sizeof(int) || matrix[1][1] != 4 ||
           (*slot)(2) != 5 || (*callback_rows)[1](3) != 6 ||
           record.value != 9 || anonymous_pointer->value != 11;
}
