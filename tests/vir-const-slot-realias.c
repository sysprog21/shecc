typedef int (**slot_t)(int);
typedef const slot_t const_slot_t;
int main(void)
{
    const_slot_t slot = 0;
    slot = 0;
    return 0;
}
