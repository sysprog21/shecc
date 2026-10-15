static long long add_wide(long long left, long long right)
{
    return left + right;
}

int main(int argc)
{
    return (int) (add_wide(0x100000000LL, 0x100000000LL + (long long) argc) >>
                  32);
}
