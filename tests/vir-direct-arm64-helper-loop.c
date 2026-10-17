static int helper(int value);

int main(int argc)
{
    return helper(argc);
}
static int helper(int value)
{
    int i = 0;

    while (i < value)
        i++;
    return i;
}
