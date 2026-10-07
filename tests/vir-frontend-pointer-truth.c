/*
 * Testing a pointer for null: "!p", a comparison with a literal 0 on either
 * side, and a pointer used directly as a branch condition.
 */
int not_ptr(int *p)
{
    return !p;
}

int eq_null(int *p)
{
    return p == 0;
}

int ne_null(int *p)
{
    return 0 != p;
}

int branch_ptr(int *p)
{
    if (p)
        return 3;
    return 4;
}

int main()
{
    int x = 1;

    return not_ptr(&x) + eq_null(&x) * 2 + ne_null(&x) * 4 +
           branch_ptr(&x) * 8 + not_ptr(0) * 64 + ne_null(0) * 128;
}
