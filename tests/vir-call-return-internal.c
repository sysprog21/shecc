_Bool truth(int x)
{
    return x;
}
signed char signed_byte(int x)
{
    return x;
}
unsigned char unsigned_byte(int x)
{
    return x;
}
short signed_short(int x)
{
    return x;
}
unsigned short unsigned_short(int x)
{
    return x;
}
int main(void)
{
    return truth(0) || !truth(256) || signed_byte(0x123456ff) != -1 ||
           unsigned_byte(0x123456ff) != 255 || signed_short(0x1234fffe) != -2 ||
           unsigned_short(0x1234fffe) != 65534;
}
