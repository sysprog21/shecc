static int divrem(int dividend, int divisor, int after)
{
    int quotient = dividend / divisor;
    int remainder = dividend % divisor;
    int constant_quotient = dividend / 3;
    int constant_remainder = dividend % 3;
    int negative_quotient = dividend / -divisor;
    int negative_remainder = dividend % -divisor;
    int nested_divisor = after / divisor;
    int nested_result = dividend / nested_divisor + dividend % nested_divisor;

    return quotient * 100 + remainder + after + constant_quotient * 10 +
           constant_remainder + negative_quotient * 10 + negative_remainder +
           nested_result;
}

int main(int argc, char **argv)
{
    int value = divrem(-17 * argc, argc + 2, 7);
    int expected = argc == 1 ? -508 : -862;

    (void) argv;
    return value != expected;
}
