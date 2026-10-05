/*
 * The loop index is a block parameter. Treating it as the constant 0 used to
 * drop the pointer addition, so every iteration copied element 0.
 */
int copy_bytes(char *dst, char *src, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = src[i];
    return n;
}

int main(void)
{
    char src[4];
    char dst[4];

    src[0] = 5;
    src[1] = 6;
    src[2] = 7;
    src[3] = 8;
    dst[3] = 0;
    return copy_bytes(dst, src, 4) + dst[3] + dst[1];
}
