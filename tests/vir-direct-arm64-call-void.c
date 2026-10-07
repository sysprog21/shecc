void helper(int *value)
{
    *value = 41;
}

int main(void)
{
    int value = 0;

    helper(&value);
    return value + 1;
}
