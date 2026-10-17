static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    if (value < 2)
        return 23;
    return 47;
}
