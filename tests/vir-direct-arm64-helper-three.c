static int helper(int first, int second, int third)
{
    return first * 17 + second * 5 + third * 2;
}

int main(int argc)
{
    return helper(argc, argc + 1, argc + 2);
}
