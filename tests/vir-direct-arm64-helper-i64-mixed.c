static long long helper(long long value, int extra);

int main(int argc)
{
    return (int) (helper((long long) argc << 32, argc) >> 32);
}
static long long helper(long long value, int extra)
{
    return value + extra;
}
