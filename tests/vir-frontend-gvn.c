int gvn_cross(int flag, int a, int b)
{
    int value = a + b;
    int result;

    if (flag < 0)
        result = value;
    else
        result = a + b;
    return result;
}

int main(void)
{
    return gvn_cross(0, 9, 12);
}
