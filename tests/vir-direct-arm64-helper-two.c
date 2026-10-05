static int helper(int left, int right)
{
    return left * 17 + right;
}

int main(int argc)
{
    return helper(argc, argc + 1);
}
