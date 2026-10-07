static int left = 0;
static int right = 0;
static int *selected = &left;

int main(int argc)
{
    if (argc > 1)
        selected = &right;
    *selected = 7;
    return left + 2 * right;
}
