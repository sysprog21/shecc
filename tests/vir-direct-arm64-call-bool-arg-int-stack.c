int helper(_Bool a,
           _Bool b,
           _Bool c,
           _Bool d,
           _Bool e,
           _Bool f,
           _Bool g,
           _Bool h,
           int value)
{
    return a && b && c && d && e && f && g && h && value == 1 ? 0 : 1;
}

int main(int argc)
{
    return helper(1, 1, 1, 1, 1, 1, 1, 1, argc != 0);
}
