volatile int selector;

int signed_sccp_i32(void)
{
    int value;

    if (selector)
        value = -7;
    else
        value = -7;
    return value / 3 == -2 && value % 3 == -1;
}

int unsigned_sccp_i32(void)
{
    unsigned int value;

    if (selector)
        value = 0xffffffffU;
    else
        value = 0xffffffffU;
    return value / 3U == 0x55555555U && value % 3U == 0;
}

int main(void)
{
    return signed_sccp_i32() && unsigned_sccp_i32() ? 0 : 1;
}
