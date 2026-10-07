int main(int argc)
{
    int left = 1;
    int right = 2;
    int *selected;

    if (argc > 1)
        selected = &left;
    else
        selected = &right;
    *selected = argc + 20;
    return left * 10 + right;
}
