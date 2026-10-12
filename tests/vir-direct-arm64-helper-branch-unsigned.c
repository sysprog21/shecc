static int helper(unsigned int value)
{
    if (value < 2U)
        return 23;
    return 47;
}

int main(int argc)
{
    return helper(argc) + helper(argc - 2);
}
