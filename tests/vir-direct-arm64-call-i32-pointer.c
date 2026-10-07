int helper(int *value)
{
    return *value + 1;
}

int main(void)
{
    int value = 41;

    return helper(&value);
}
