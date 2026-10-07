volatile long long signed_left = -1;
volatile long long signed_right = 1;
volatile unsigned long long unsigned_left = 0xffffffffffffffffULL;
volatile unsigned long long unsigned_right = 1;

int main(void)
{
    int signed_less = signed_left < signed_right;
    int unsigned_less = unsigned_left < unsigned_right;

    return signed_less != 1 || unsigned_less != 0;
}
