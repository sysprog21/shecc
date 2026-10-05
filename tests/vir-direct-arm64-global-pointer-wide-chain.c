long long value = 0x100000001LL;
long long *p = &value;
long long **pp = &p;

int main(int argc)
{
    **pp = (long long) argc + 0x300000000LL;
    return (int) (**pp >> 32);
}
