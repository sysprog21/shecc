static int inner(void);
static int outer(void);


int main(void)
{
    return outer();
}
static int inner(void)
{
    return 29;
}
static int outer(void)
{
    return inner();
}
