int nested_licm(int outer_limit, int inner_limit, int a, int b, int c)
{
    int i = 0;
    int total = 0;

    while (i < outer_limit) {
        int j = 0;

        while (j < inner_limit) {
            total += a * b + i + c;
            j++;
        }
        i++;
    }
    return total;
}

int main(void)
{
    return nested_licm(2, 3, 4, 5, 6);
}
