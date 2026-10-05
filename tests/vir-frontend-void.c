int total;

void set_explicit(int x)
{
    total = x;
    return;
}

void add_implicit(int x)
{
    total = total + x;
}

void add_early(int x)
{
    if (x < 0)
        return;
    total = total + x;
}

/* No values at all: empty blocks still need valid CFG representation. */
void do_nothing(void) {}

void copy_bytes(char *dst, char *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = src[i];
}

int main(void)
{
    char src[4];
    char dst[4];

    src[0] = 5;
    src[1] = 6;
    src[2] = 7;
    src[3] = 8;
    do_nothing();
    set_explicit(10);
    add_implicit(3);
    add_early(-4);
    add_early(4);
    copy_bytes(dst, src, 4);
    return total + dst[3];
}
