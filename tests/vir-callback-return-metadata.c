typedef volatile int *volatile_pointer_t;
typedef volatile_pointer_t (*volatile_getter_t)(void);
static volatile int number = 7;
volatile int *get_number(void)
{
    return &number;
}
static volatile_getter_t number_getter = get_number;
int touch(void)
{
    volatile_getter_t *slot = &number_getter;
    number_getter = get_number;
    *slot = get_number;
    number_getter;
    *slot;
    *number_getter();
    return *number_getter();
}
typedef const int *const_pointer_t;
typedef const_pointer_t (*const_getter_t)(void);
static const int constant = 11;
const int *get_constant(void)
{
    return &constant;
}
static const_getter_t constant_getter = get_constant;
int constant_assignments(void)
{
    const_getter_t *slot = &constant_getter;
    constant_getter = get_constant;
    *slot = get_constant;
    return *constant_getter();
}
typedef int (*callback_t)(int);
typedef int (**slot_t)(int);
typedef slot_t (*slot_getter_t)(void);
typedef callback_t *callback_pointer_t;
typedef callback_pointer_t (*alias_slot_getter_t)(void);
int add(int x)
{
    return x + 3;
}
static callback_t callback = add;
slot_t get_slot(void)
{
    return &callback;
}
callback_pointer_t get_alias_slot(void)
{
    return &callback;
}
static slot_getter_t slot_getter = get_slot;
static alias_slot_getter_t alias_slot_getter = get_alias_slot;
typedef int (*const constant_callback_t)(int);
typedef constant_callback_t (*constant_callback_getter_t)(void);
constant_callback_t get_constant_callback(void)
{
    return add;
}
static constant_callback_getter_t constant_callback_getter =
    get_constant_callback;
int constant_callback_assignments(void)
{
    constant_callback_getter_t *slot = &constant_callback_getter;
    constant_callback_getter = get_constant_callback;
    *slot = get_constant_callback;
    return constant_callback_getter()(6);
}
typedef int (*volatile volatile_callback_t)(int);
typedef volatile_callback_t (*volatile_callback_getter_t)(void);
volatile_callback_t get_volatile_callback(void)
{
    return add;
}
static volatile_callback_getter_t volatile_callback_getter =
    get_volatile_callback;
int volatile_callback_assignments(void)
{
    volatile_callback_getter_t *slot = &volatile_callback_getter;
    volatile_callback_getter = get_volatile_callback;
    *slot = get_volatile_callback;
    volatile_callback_getter;
    *slot;
    return volatile_callback_getter()(8);
}
int main(void)
{
    return touch() != 7 || constant_assignments() != 11 ||
           constant_callback_assignments() != 9 ||
           volatile_callback_assignments() != 11 || (*slot_getter())(2) != 5 ||
           (*alias_slot_getter())(3) != 6;
}
