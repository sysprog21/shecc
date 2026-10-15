int choose(int flag, int left, int right)
{
    int value;
    int result;

    if (flag < 0)
        value = left + right;
    else
        value = left + right;
    if (value < 6)
        result = 1;
    else
        result = 2;
    return result;
}

int main(void)
{
    return choose(-1, 7, 2) + choose(1, 7, 2);
}
