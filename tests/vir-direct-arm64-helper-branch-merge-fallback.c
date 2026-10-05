static int helper(int value)
{
    int result;

    if (value < 2)
        result = 23;
    else
        result = 47;
    return result;
}

int main(int argc)
{
    return helper(argc);
}
