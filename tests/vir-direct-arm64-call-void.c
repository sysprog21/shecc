void helper(int *value);

int main(void)
{
    int value = 0;

    helper(&value);
    return value + 1;
}
void helper(int *value)
{
    *value = 41;
}
