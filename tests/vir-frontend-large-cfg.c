/*
 * Keep this workload deliberately branch-heavy: the frontend builds more than
 * forty blocks before O2 cleanup, exercising deferred large-CFG pruning through
 * LICM without relying on a target-specific backend form.
 */
static int vir_large_cfg(int x)
{
    int result = 0;

    if (x & 1)
        result += 1;
    if (x & 2)
        result += 2;
    if (x & 4)
        result += 3;
    if (x & 8)
        result += 4;
    if (x & 16)
        result += 5;
    if (x & 32)
        result += 6;
    if (x & 64)
        result += 7;
    if (x & 128)
        result += 8;
    if (x & 256)
        result += 9;
    if (x & 512)
        result += 10;
    if (x & 1024)
        result += 11;
    if (x & 2048)
        result += 12;
    if (x & 4096)
        result += 13;
    if (x & 8192)
        result += 14;
    if (x & 16384)
        result += 15;
    if (x & 32768)
        result += 16;
    if (x & 65536)
        result += 17;
    if (x & 131072)
        result += 18;
    if (x & 262144)
        result += 19;
    if (x & 524288)
        result += 20;
    if (x & 1048576)
        result += 21;

    /* Keep one locally provable branch so SCCP rewrites the large graph while
     * the other branches remain runtime-dependent.
     */
    if (x - x)
        result += 22;
    else
        result += 23;
    return result;
}

int main(void)
{
    return vir_large_cfg(0) != 23;
}
