/*
 * A call whose result is unused as the last instruction of its block, and as
 * the last instruction of the function.
 */
static int total;

void bump(int n)
{
    total += n;
}

void bump_if(int n)
{
    if (n > 2)
        bump(n);
    bump(1);
}

int main()
{
    bump_if(5);
    bump_if(1);
    return total;
}
