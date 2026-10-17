static int combine(int a, int b, int c, int d, int e, int f);

int main(int argc, char **argv)
{
    return combine(1, 2, 3, 4, 5, 6);
}
static int combine(int a, int b, int c, int d, int e, int f)
{
    return (a * b) + (e + f);
}
