static int helper(int x)
{
    return x + 0x40000001;
}

int main(int argc)
{
    return helper(argc);
}
