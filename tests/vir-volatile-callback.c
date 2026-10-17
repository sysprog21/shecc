typedef int (*F)(int);
typedef volatile F VF;
static int add(int v);
int touch(void);
static F volatile a = add;
static VF b = add;
static int (*volatile c)(int) = add;
int main(void)
{
    return touch();
}
static int add(int v)
{
    return v + 1;
}
int touch(void)
{
    a;
    b;
    c;
    return a(1) + b(2) + c(3) - 9;
}
