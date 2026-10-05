int not_equal(int a, int b)
{
    return a != b;
}

int less_equal(int a, int b)
{
    return a <= b;
}

int greater_equal(int a, int b)
{
    return a >= b;
}

int greater(int a, int b)
{
    return a > b;
}

int logical_not(int a)
{
    return !a;
}

int branch_not(int a)
{
    if (!a)
        return 1;
    return 0;
}

int logical_arith(int a)
{
    return !a + 3;
}

int comparison_arith(int a, int b)
{
    return (a < b) + 5;
}

int main(void)
{
    if (not_equal(3, 7) + less_equal(3, 7) + greater_equal(7, 3) +
                greater(7, 3) + logical_not(0) + branch_not(0) ==
            6 &&
        logical_arith(0) + comparison_arith(3, 7) == 10)
        return 0;
    return 1;
}
