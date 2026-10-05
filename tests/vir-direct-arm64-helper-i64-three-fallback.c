static long long helper(long long first, long long second, long long third)
{
    return first + (second >> 1) + (third >> 2);
}

int main(int argc)
{
    long long first = (long long) argc << 32;
    long long second = (long long) (argc + 3) << 32;
    long long third = (long long) (argc + 5) << 32;

    return (int) (helper(first, second, third) >> 32);
}
