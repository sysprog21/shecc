int *helper(int *value)
{
    return value;
}

int main(void)
{
    int value = 41;
    int *result = helper(&value);

    return *result + 1;
}
