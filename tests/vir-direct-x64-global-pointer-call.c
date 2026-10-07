static int left = 0;
static int right = 0;
int *selected = &left;

static int select_right(void)
{
    selected = &right;
    return 1;
}

int main(int argc)
{
    if (argc > 1)
        argc = select_right();
    *selected = 7;
    return left + 2 * right;
}
