int helper(int value)
{
    return value + 1;
}

int main(int argc)
{
    if (argc == 1)
        return helper(argc);
    return helper(2);
}
