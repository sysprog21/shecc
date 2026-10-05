typedef int *P;
typedef volatile int *VP;
typedef int *volatile PV;
static int value = 7;
static volatile int volvalue = 9;
static int *volatile own = &value;
static volatile int *pointee = &volvalue;
static volatile P aliasown = &value;
static VP aliaspointee = &volvalue;
static PV aliasstar = &value;
int check(void)
{
    own;
    aliasown;
    aliasstar;
    *pointee;
    *aliaspointee;
    return 0;
}
int main(void)
{
    return check();
}
