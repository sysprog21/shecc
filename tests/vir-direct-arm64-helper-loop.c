static int helper(int value)
{
    int i = 0;

    while (i < value)
        i++;
    return i;
}

int main(int argc)
{
    return helper(argc);
}
