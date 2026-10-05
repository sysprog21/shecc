static long long helper(long long left, long long right)
{
    return left + (right >> 1);
}

int main(int argc)
{
    long long left = (long long) argc << 32;
    long long right = (long long) (argc + 3) << 32;

    return (int) (helper(left, right) >> 32);
}
