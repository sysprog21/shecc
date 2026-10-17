int combine(int left, int right);

int main(int argc, char **argv)
{
    return combine(argc, argc + 1) + argc;
}
int combine(int left, int right)
{
    return left * 10 + right;
}
