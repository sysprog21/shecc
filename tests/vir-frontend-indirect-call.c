typedef unsigned char byte_t;
typedef unsigned short word_t;
typedef int (*check_callback_t)(signed char, word_t, _Bool);
typedef byte_t (*byte_callback_t)(byte_t);
typedef word_t (*word_callback_t)(word_t);
typedef int (*bool_callback_t)(_Bool);
typedef _Bool (*bool_result_callback_t)(int);
typedef void (*void_callback_t)(int);
typedef int (*nullary_callback_t)(void);
typedef int *(*int_pointer_result_callback_t)(void);
typedef int (*pointer_callback_t)(void *);
typedef long long (*wide_callback_t)(long long, long long);
typedef int (*variadic_callback_t)(int, ...);

static int check_values(signed char a, word_t b, _Bool c)
{
    return a == -1 && b == 32768 && c;
}

static byte_t increment_byte(byte_t value)
{
    return value + 1;
}

static byte_t identity_byte(byte_t value)
{
    return value;
}

static word_t identity_word(word_t value)
{
    return value;
}

static int identity_bool(_Bool value)
{
    return value;
}

static _Bool truthy(int value)
{
    return value != 0;
}

static int invoke_check(check_callback_t callback,
                        signed char a,
                        word_t b,
                        _Bool c)
{
    return callback(a, b, c);
}

static byte_t invoke_byte(byte_callback_t callback, byte_t value)
{
    return callback(value);
}

static byte_t invoke_computed_byte(byte_callback_t callback, int value)
{
    return callback((byte_t) (value + 128));
}

static word_t invoke_computed_word(word_callback_t callback, int value)
{
    return callback((word_t) (value + 32768));
}

static int invoke_computed_bool(bool_callback_t callback, int value)
{
    return callback(value != 0);
}

static int invoke_bool_result(bool_result_callback_t callback, int value)
{
    return callback(value);
}

static int void_callback_result;

static void record_callback_value(int value)
{
    void_callback_result = value;
}

static int invoke_void(void_callback_t callback, int value)
{
    callback(value);
    return void_callback_result;
}

static int answer(void)
{
    return 42;
}

static int invoke_nullary(nullary_callback_t callback)
{
    return callback();
}

static int pointer_result_target[] = {73, 89};

static int *return_pointer_result(void)
{
    return pointer_result_target;
}

static int invoke_pointer_result(int_pointer_result_callback_t callback)
{
    return callback()[1];
}

static int invoke_variadic(variadic_callback_t callback)
{
    return callback(1, 2);
}

static int check_pointer(void *pointer)
{
    return pointer != 0;
}

static int invoke_pointer(pointer_callback_t callback, void *pointer)
{
    return callback(pointer);
}

static long long add_wide(long long left, long long right)
{
    return left + right;
}

static long long invoke_wide(wide_callback_t callback,
                             long long left,
                             long long right)
{
    return callback(left, right);
}

static int pointer_target;

int main(void)
{
    return !invoke_check(check_values, -1, 32768, 1) ||
           invoke_byte(increment_byte, 254) != 255 ||
           invoke_computed_byte(identity_byte, 127) != 255 ||
           invoke_computed_word(identity_word, 0) != 32768 ||
           invoke_computed_bool(identity_bool, 2) != 1 ||
           !invoke_bool_result(truthy, 1) || invoke_bool_result(truthy, 0) ||
           invoke_void(record_callback_value, 37) != 37 ||
           invoke_nullary(answer) != 42 ||
           invoke_pointer_result(return_pointer_result) != 89 ||
           !invoke_pointer(check_pointer, &pointer_target) ||
           invoke_wide(add_wide, 0x100000000LL, 5) != 0x100000005LL;
}
