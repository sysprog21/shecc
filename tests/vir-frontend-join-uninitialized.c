int choose(int flag, int value)
{
    int result;

    if (flag < 0)
        result = value;
    return result + 1;
}

int main(void)
{
    return choose(-1, 7);
}
