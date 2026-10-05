long long value = 0x100000001LL;
long long *p = &value;

int main(int argc)
{
    *p = (long long) argc + 0x200000000LL;
    return (int) (*p >> 32);
}
