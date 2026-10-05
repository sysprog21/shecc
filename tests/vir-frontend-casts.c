int signed_char_promote(signed char value)
{
    return value + 1;
}

int unsigned_char_promote(unsigned char value)
{
    return value + 1;
}

int signed_short_promote(short value)
{
    return value + 1;
}

int unsigned_short_promote(unsigned short value)
{
    return value + 1;
}

int narrow_then_extend(int value)
{
    signed char narrowed = (signed char) value;

    return narrowed;
}

int main(void)
{
    return signed_char_promote(-2) + unsigned_char_promote(254) +
           signed_short_promote(-2) + unsigned_short_promote(2) +
           narrow_then_extend(258);
}
