int combine(int left, int right);

int main(int argc, char **argv)
{
    int result = combine(argc, 7);
    char *first = argv[0];

    return result + (first != (char *) 0 ? 10 : 0);
}
int combine(int left, int right)
{
    return left + right * 2;
}
