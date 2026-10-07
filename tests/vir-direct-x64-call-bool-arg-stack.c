_Bool helper(_Bool a, _Bool b, _Bool c, _Bool d, _Bool e, _Bool f, _Bool g)
{
    return a && b && c && d && e && f && g;
}

int main(int argc)
{
    _Bool value = argc != 0;
    _Bool stack_value = argc == 1;

    return helper(value, value, value, value, value, value, stack_value) ? 0
                                                                         : 1;
}
