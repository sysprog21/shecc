typedef int (**slot_t)(int);
void accept_slot(const slot_t slot);
void accept_slot(const slot_t slot) {}
typedef int (*cb_t)(int);
typedef cb_t *cbp_t;
void accept_callback(const cbp_t slot) {}
void (*callback_acceptor)(const cbp_t) = accept_callback;
int increment(int value)
{
    return value + 1;
}
typedef int (*const const_cb_t)(int);
typedef int (*volatile volatile_cb_t)(int);
const_cb_t get_const(void)
{
    return increment;
}
volatile_cb_t get_volatile(void)
{
    return increment;
}
cb_t get_callback(void)
{
    return increment;
}
int main(void)
{
    typedef cb_t (*getter_t)(void);
    typedef const_cb_t (*const_getter_t)(void);
    typedef volatile_cb_t (*volatile_getter_t)(void);
    getter_t ordinary = get_callback;
    const_getter_t constant = get_const;
    volatile_getter_t varying = get_volatile;
    constant = get_const;
    varying = get_volatile;
    const_getter_t *constant_slot = &constant;
    volatile_getter_t *varying_slot = &varying;
    *constant_slot = get_const;
    *varying_slot = get_volatile;
    cb_t callback = increment;
    accept_slot(&callback);
    callback_acceptor(&callback);
    return ordinary()(1) != 2 || constant()(3) != 4 || varying()(5) != 6;
}
