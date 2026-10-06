volatile unsigned int element = 3;
volatile signed char bytes[8];
volatile unsigned char ubytes[8];
volatile short shorts[8];
volatile unsigned short ushorts[8];
volatile int words[8];
volatile unsigned int uwords[8];
volatile long long longs[8];
volatile unsigned long long ulongs[8];
volatile int far_words[4097];
int main(void)
{
    unsigned int i = element;
    bytes[i] = -100;
    ubytes[i] = 200;
    shorts[i] = -30000;
    ushorts[i] = 60000;
    words[i] = -2000000000;
    uwords[i] = 4000000000U;
    longs[i] = -0x100000001LL;
    ulongs[i] = 0x100000001ULL;
    far_words[4096] = 9;
    int sum = 0;
    for (unsigned int j = 0; j < 3; j++)
        sum += far_words[4096];
    return sum != 27 || bytes[i] != -100 || ubytes[i] != 200 ||
           shorts[i] != -30000 || ushorts[i] != 60000 ||
           words[i] != -2000000000 || uwords[i] != 4000000000U ||
           longs[i] != -0x100000001LL || ulongs[i] != 0x100000001ULL;
}
