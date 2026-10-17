long long helper(long long value);

int main(void)
{
    return (int) (helper(0x123456789abcdef0LL) >> 32);
}
long long helper(long long value)
{
    return value + 1LL;
}
