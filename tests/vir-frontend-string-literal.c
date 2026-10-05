/*
 * String literals as values: passed to a call, chosen between by a branch, and
 * walked through by the callee.
 */
int count(char *s, char c)
{
    int n = 0;

    while (*s) {
        if (*s == c)
            n++;
        s++;
    }
    return n;
}

int greet(int k)
{
    char *m = k ? "hello, world" : "bye";

    return count(m, 'o') + count("xoxo", 'o') * 10;
}

int main()
{
    return greet(1) + greet(0);
}
