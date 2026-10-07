static int helper(int value)
{
    if (value < 2)
        return 23;
    return 47;
}

int main(int argc)
{
    return helper(argc);
}
