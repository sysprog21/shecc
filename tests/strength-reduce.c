/*
 * The address of a[i * 4] changes by a fixed amount on each trip through these
 * loops. Their explicit goto back edges give the strength-reduction pass a
 * latch with an existing SSA jump: it must advance the address before that
 * jump, not append the advance after it.
 */
int main()
{
    int a[16];
    int i;
    int sum = 0;

    i = 0;
write_loop:
    a[i * 4] = i + 1;
    i = i + 1;
    if (i == 4)
        goto write_done;
    goto write_loop;

write_done:
    i = 0;
read_loop:
    sum = sum + a[i * 4];
    i = i + 1;
    if (i == 4)
        goto read_done;
    goto read_loop;

read_done:
    return sum != 10;
}
