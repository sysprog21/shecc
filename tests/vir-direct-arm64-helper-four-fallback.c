static int helper(int first, int second, int third, int fourth)
{
    return first + second * 3 + third * 5 + fourth * 7;
}

int main(int argc)
{
    return helper(argc, argc + 1, argc + 2, argc + 3);
}
