struct item {
    int value;
};
static int fixed(int p[2], int *replacement)
{
    int **alias = &p;
    *alias = replacement;
    return p[0];
}
static int unsized(int p[], int *replacement)
{
    int **alias = &p;
    *alias = replacement;
    return p[0];
}
static int record(struct item p[], struct item *replacement)
{
    struct item **alias = &p;
    *alias = replacement;
    return p[0].value;
}
static int ordinary(int p[])
{
    return p[0];
}
int main(void)
{
    int first[2] = {1, 2}, second[2] = {7, 8};
    struct item a[1] = {{3}}, b[1] = {{9}};
    if (fixed(first, second) != 7)
        return 1;
    if (unsized(first, second) != 7)
        return 2;
    if (record(a, b) != 9)
        return 3;
    if (ordinary(first) != 1)
        return 4;
    return 0;
}
