int helper(_Bool a,
           _Bool b,
           _Bool c,
           _Bool d,
           _Bool e,
           _Bool f,
           _Bool g,
           _Bool h,
           _Bool i,
           _Bool j,
           _Bool k,
           _Bool l,
           _Bool m,
           _Bool n,
           _Bool o,
           _Bool p)
{
    return a && b && c && d && e && f && g && h && i && j && k && l && m && n &&
                   o
               ? (p ? 42 : 7)
               : 1;
}

int main(int argc)
{
    return helper(1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, argc == 2);
}
