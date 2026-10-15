int callee(int value)
{
    return value + 1;
}

int dce_chain(int value)
{
    int dead0 = value + 2;
    int dead1 = dead0 * 3;

    return callee(value);
}

int main(void)
{
    return dce_chain(12);
}
