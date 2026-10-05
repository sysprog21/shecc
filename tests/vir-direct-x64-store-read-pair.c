int main(void)
{
    int left = 0;
    int right = 0;

    *(&left) = 20;
    *(&right) = 9;
    return left + right;
}
