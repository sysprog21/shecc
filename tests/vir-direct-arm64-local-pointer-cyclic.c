int main(int argc)
{
    int value = 29;
    int *p = &value;
    int **pp = &p;

    /* Make p and pp form a self-referential pointer cycle. */
    p = (int *) pp;
    return p == (int *) pp ? argc : value;
}
