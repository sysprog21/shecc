int main(void)
{
    signed char bytes[9];
    unsigned short words[3];

    bytes[8] = -5;
    words[2] = 50000;
    return bytes[8] + words[2] - 49766;
}
