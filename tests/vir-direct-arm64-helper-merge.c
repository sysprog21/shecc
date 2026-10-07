static int helper(int value)
{
    return value < 2 ? 23 : 47;
}

int main(int argc)
{
    return helper(argc);
}
