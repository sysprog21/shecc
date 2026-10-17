_Bool helper(int value);

int main(void)
{
    _Bool positive = helper(1);
    _Bool negative = helper(0);

    return positive && !negative ? 0 : 1;
}
_Bool helper(int value)
{
    return value != 0;
}
