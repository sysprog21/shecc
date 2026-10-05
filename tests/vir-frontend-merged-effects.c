static int left;
static int right;

static int merged_effect(int *first, int *second, int select)
{
    int *address;

    if (select)
        address = first;
    else
        address = second;
    *address = 29;
    return *address;
}

int main(void)
{
    return merged_effect(&left, &right, 1);
}
