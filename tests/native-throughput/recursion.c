unsigned int sink;
unsigned int fib(unsigned int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}
int main(void)
{
    unsigned int s = 0;
    for (unsigned int i = 0; i < 40; i++)
        s += fib(27);
    sink = s;
    return sink != 7856720u;
}
