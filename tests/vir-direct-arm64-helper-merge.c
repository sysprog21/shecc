static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    return value < 2 ? 23 : 47;
}
