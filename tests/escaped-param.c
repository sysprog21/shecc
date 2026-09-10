/* prepare_operand() reloads an address-taken variable from its stack slot
 * rather than reading the register it is already in, because a store through
 * the pointer would not have gone to the register. That reload needs the slot
 * to hold the value, and a parameter arrives in a register with no slot at
 * all: one is reserved the first time something spills it.
 *
 * Taking the address after the expression is what orders the two so the
 * reload comes first. Nothing has to be stored through the pointer -- the
 * value is already wrong by the time the address is taken.
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
