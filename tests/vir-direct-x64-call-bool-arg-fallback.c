int helper(int value)
{
    return value == 1 ? 0 : 1;
}

int main(int argc)
{
    return helper(argc != 0);
}
