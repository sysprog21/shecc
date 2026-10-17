int *helper(int *value);

int main(void)
{
    int value = 41;
    int *result = helper(&value);

    return *result + 1;
}
int *helper(int *value)
{
    return value;
}
