int main(int argc)
{
    int x = argc + 2147483647;

    return (x < argc) * 10 + ((unsigned) x < (unsigned) argc) * 100 +
           ((x ^ argc) & 127);
}
