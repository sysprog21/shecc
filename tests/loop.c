int main(int argc, char *argv[])
{
    int i;
    for (i = 1; i <= 5; i++)
        printf("%d ", i);
    printf("\n");
    i = 7;
    for (; i > -2;) {
        printf("%d ", i);
        i--;
    }
    printf("\n");
    return 0;
}
