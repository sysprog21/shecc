int helper(_Bool value)
{
    return value == 0 ? 7 : 42;
}

int main(int argc)
{
    return helper(argc == 1);
}
