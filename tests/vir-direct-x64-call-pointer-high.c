int sum_pointer_slots(int a, int b, int c, int d, char **left, char **right)
{
    return left[0] != (char *) 0 && right[0] != (char *) 0 ? a + b + c + d : 0;
}

int main(int argc, char **argv)
{
    return sum_pointer_slots(1, 2, 3, 4, argv, argv) + argc;
}
