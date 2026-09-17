/*
 * A parameter whose address is taken only after an expression has read it is
 * still in the register it arrived in, and its stack slot may not exist yet.
 * Reloading it from that slot read whatever sat at offset zero of the frame.
 * Keep the `&` after the arithmetic and the pointers in place: both are what
 * make this path reachable.
 */

int sub_then_escape(int a, int b)
{
    int x = a - b;
    int *p = &b;
    return x;
}

int add_then_escape(int a, int b)
{
    int x = a + b;
    int *p = &b;
    return x;
}

int escape_first_operand(int a, int b)
{
    int x = a - b;
    int *p = &a;
    return x;
}

int main()
{
    if (sub_then_escape(5, 2) != 3)
        return 1;
    if (add_then_escape(1, 2) != 3)
        return 2;
    if (escape_first_operand(5, 2) != 3)
        return 3;
    return 0;
}
