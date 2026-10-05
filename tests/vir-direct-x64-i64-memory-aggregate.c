struct pair {
    long long value;
};

int main(int argc)
{
    struct pair pair;
    long long *p = &pair.value;

    *p = 0x100000000LL + (long long) argc;
    return (int) (pair.value >> 32);
}
