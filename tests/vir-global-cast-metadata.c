typedef int (*callback_t)(void);
struct holder {
    int (**slots)(void);
    int n;
};
int five(void)
{
    return 5;
}
int (*fs[2])(void) = {five, five};
int (**s)(void) = (int (**)(void)) 4;
int (***r)(void) = (int (***)(void)) 20;
int (**q)(void) = (int (**)(void)) fs;
callback_t *p = (callback_t *) 16;
callback_t *pa = (int (**)(void)) 24;
struct holder g = {(int (**)(void)) 12, 3};
int (**t[2])(void) = {(int (**)(void)) 4, 0};
int check_cast_depth(void)
{
    static int (**ls)(void) = (int (**)(void)) 8;
    static struct holder lg = {(int (**)(void)) 28, 1};
    if (s != (int (**)(void)) 4 || r != (int (***)(void)) 20)
        return 1;
    if ((*q[1])() != 5 || (void *) p != (void *) 16 ||
        (void *) pa != (void *) 24)
        return 2;
    if (g.slots != (int (**)(void)) 12 || g.n != 3)
        return 3;
    if (t[0] != (int (**)(void)) 4 || t[1] != 0)
        return 4;
    if (ls != (int (**)(void)) 8 || lg.slots != (int (**)(void)) 28)
        return 5;
    return 0;
}

int inc(int x)
{
    return x + 1;
}
int (*slot)(int) = inc;
int (**get(void))(int)
{
    return &slot;
}
int (**(*pg)(void) )(int) = get;
int check_spelled_return(void)
{
    int (**s)(int) = get();
    return (*s)(1) != 2 || (*get())(2) != 3 || (*pg())(3) != 4;
}

typedef int (*UP)(int);
int inc_alias(int x)
{
    return x + 1;
}
UP alias_slot = inc_alias;
UP *get_alias(void)
{
    return &alias_slot;
}
UP *(*pg_alias)(void) = get_alias;
int check_alias_return(void)
{
    UP *s = get_alias();
    return (*s)(1) != 2 || (*get_alias())(2) != 3 || (*pg_alias())(3) != 4;
}

typedef char *(*text_cb_t)(void);
typedef char *text_fn_t(void);
char *text_value(void)
{
    return "x";
}
char *(*text_functions[1])(void) = {text_value};
char *(**text_slot)(void) = (text_cb_t *) text_functions;
char *(**text_function_slot)(void) = (text_fn_t **) text_functions;
typedef UP *slot_alias_t;
int check_alias_slot_array(void)
{
    UP callback = inc_alias;
    slot_alias_t slots[1] = {&callback};
    return (*slots[0])(4) != 5;
}
int grouped_object;
char *grouped_address = (char *) (&grouped_object);
char *grouped_offset = (char *) (&grouped_object) + 1;
char *grouped_nested = (char *) ((void *) (&grouped_object));
char *grouped_inner = (char *) (int *) (&grouped_object + 1);
char *grouped_inner_cast = (char *) (void *) ((int *) &grouped_object + 1);
char *grouped_sum[1] = {2 + (char *) (&grouped_object)};
int grouped_truth[1] = {0 || (char *) (&grouped_object) + 1};
char *grouped_choice[1] = {1 ? (char *) (&grouped_object) + 1 : 0};
int scoped_shadow;
int check_static_cast_scope(void)
{
    static int scoped_shadow;
    static int scoped_local;
    static char *shadow_address = (char *) (&scoped_shadow);
    static char *local_address = (char *) (&scoped_local);
    static char *local_offset = (char *) (&scoped_local) + 1;
    return shadow_address != (char *) &scoped_shadow ||
           local_address != (char *) &scoped_local ||
           local_offset != (char *) &scoped_local + 1;
}
int *cast_string_offset = (int *) "abcdefgh" + 1;
int *cast_string_element = (int *) &"abcdefgh"[1];
char **cast_string_sum[1] = {1 + (char **) "abcdefghijklmnop"};
char *cast_wide_high = (char *) ((char *) 0x100000000ULL);
char *cast_wide_sign = (char *) ((char *) 0x80000000ULL);
int main(void)
{
    return check_cast_depth() || check_spelled_return() ||
           check_alias_return() || check_alias_slot_array() ||
           grouped_address != (char *) &grouped_object ||
           grouped_offset != (char *) &grouped_object + 1 ||
           grouped_nested != (char *) &grouped_object ||
           grouped_inner != (char *) &grouped_object + sizeof(grouped_object) ||
           grouped_inner_cast !=
               (char *) &grouped_object + sizeof(grouped_object) ||
           grouped_sum[0] != (char *) &grouped_object + 2 ||
           grouped_truth[0] != 1 ||
           grouped_choice[0] != (char *) &grouped_object + 1 ||
           check_static_cast_scope() || *(char *) cast_string_offset != 'e' ||
           *(char *) cast_string_element != 'b' ||
           *(char *) cast_string_sum[0] != "abcdefghijklmnop"[sizeof(void *)] ||
           cast_wide_high != (char *) 0x100000000ULL ||
           cast_wide_sign != (char *) 0x80000000ULL ||
           (*text_slot)()[0] != 'x' || (*text_function_slot)()[0] != 'x';
}
