int helper(int value);

int main(int argc)
{
    return helper(argc) + argc;
}
int helper(int value)
{
    return value + 1;
}
