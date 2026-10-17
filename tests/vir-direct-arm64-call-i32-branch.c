int helper(int value);

int main(int argc)
{
    if (argc == 1)
        return helper(argc);
    return helper(2);
}
int helper(int value)
{
    return value + 1;
}
