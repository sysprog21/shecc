int choose(int value)
{
    if (value < 0)
        return 7;
    return 3;
}

int main(void)
{
    return choose(-1) + choose(1);
}
