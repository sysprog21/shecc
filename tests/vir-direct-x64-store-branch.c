/* Volatile, or the slot is never read after forwarding and the stores go. */
int main(int argc)
{
    volatile int value = 29;

    if (argc)
        return *(&value);
    return 7;
}
