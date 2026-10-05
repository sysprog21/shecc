/*
 * Local arrays are represented as stack roots. Their backing storage comes from
 * OP_allocat, so each object must retain its full size and alignment.
 */
int local_arrays(int seed)
{
    int words[3];
    char bytes[5];
    int *last = words + 2;
    int scalar = seed;
    int *alias = &scalar;

    words[0] = seed;
    words[1] = seed + 1;
    *last = 7;
    bytes[4] = 9;
    *alias = *alias + words[2];
    return words[0] + words[1] + words[2] + bytes[4] + scalar;
}

/* The pointer is spilled while the store through it is pending; with only a
 * pointer slot reserved, that spill landed inside the array and the store
 * overwrote it.
 */
int last_element(void)
{
    int a[3];
    int *p = a + 2;

    *p = 7;
    return a[2] - 7;
}

int main(int argc)
{
    return local_arrays(argc) + last_element();
}
