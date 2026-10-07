int helper(_Bool a,
           _Bool b,
           _Bool c,
           _Bool d,
           _Bool e,
           _Bool f,
           _Bool g,
           _Bool h,
           _Bool i)
{
    return a && b && c && d && e && f && g && h ? (i ? 42 : 7) : 1;
}

int main(int argc)
{
    return helper(1, 1, 1, 1, 1, 1, 1, 1, argc == 2);
}
