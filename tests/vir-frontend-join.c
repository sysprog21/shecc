int choose(int flag, int left, int right)
{
    int value;

    if (flag < 0)
        value = left + right;
    else
        value = left - right;
    return value;
}

int main(void)
{
    return choose(-1, 7, 2) + choose(1, 7, 2);
}
