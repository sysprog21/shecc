static int helper(int x);

int main(int argc)
{
    return helper(argc);
}
static int helper(int x)
{
    return x + 0x40000001;
}
