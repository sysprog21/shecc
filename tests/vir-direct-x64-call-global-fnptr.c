static int shared(int value);
int caller(int value);

int (*saved)(int) = shared;


int main(void)
{
    return shared(10) + caller(18);
}
static int shared(int value)
{
    return value + 1;
}
int caller(int value)
{
    return saved(value);
}
