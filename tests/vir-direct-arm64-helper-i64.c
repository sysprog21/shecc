static long long helper(long long value)
{
    return value + 0x100000000LL;
}

int main(int argc)
{
    return (int) (helper((long long) argc << 32) >> 32);
}
