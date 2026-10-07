static signed char pass_char(signed char value)
{
    return value;
}

static short pass_short(short value)
{
    return value;
}

static unsigned char pass_uchar(unsigned char value)
{
    return value;
}

static unsigned short pass_ushort(unsigned short value)
{
    return value;
}

int main(void)
{
    return pass_char(-7) + pass_short(36) + (pass_uchar(0x80) == 128) +
           (pass_ushort(0xffff) == 65535);
}
