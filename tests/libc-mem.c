/*
 * memcpy and memset move whole words when the addresses allow it. Check every
 * start alignment, many lengths around the word and unroll sizes, fill bytes
 * with the high bit set, and that nothing outside the range is touched.
 */
char src[96];
char dst[96];
char want[96];

int main(void)
{
    int bad = 0;

    for (int i = 0; i < 96; i++)
        src[i] = (char) (i * 7 + 1);

    for (int so = 0; so < 8; so++)
        for (int doff = 0; doff < 8; doff++)
            for (int n = 0; n < 40; n++) {
                for (int i = 0; i < 96; i++)
                    want[i] = dst[i] = (char) (255 - i);
                for (int i = 0; i < n; i++)
                    want[doff + i] = src[so + i];
                memcpy(dst + doff, src + so, n);
                for (int i = 0; i < 96; i++)
                    bad += dst[i] != want[i];
            }

    for (int off = 0; off < 8; off++)
        for (int n = 0; n < 40; n++)
            for (int c = 0; c < 4; c++) {
                int value = c == 0 ? 0 : c == 1 ? 0x7f : c == 2 ? 0x80 : -1;
                for (int i = 0; i < 96; i++)
                    want[i] = dst[i] = (char) i;
                for (int i = 0; i < n; i++)
                    want[off + i] = (char) value;
                memset(dst + off, value, n);
                for (int i = 0; i < 96; i++)
                    bad += dst[i] != want[i];
            }

    /* Forward copy with dest below src and the ranges overlapping. */
    for (int i = 0; i < 96; i++)
        dst[i] = (char) i;
    memcpy(dst, dst + 3, 60);
    for (int i = 0; i < 60; i++)
        bad += dst[i] != (char) (i + 3);

    return bad != 0;
}
