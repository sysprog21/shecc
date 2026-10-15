int object;
int (*null_callbacks[2])(void) = {0, (void *) 0};
int (**null_slot)(void) = (void *) 0;
int (**object_slot)(void) = (void *) &object;
int (**integer_slot)(void) = (void *) 16;
int main(void)
{
    static int (**local_null)(void) = (void *) 0;
    static int (**local_object)(void) = (void *) &object;
    return null_callbacks[0] != 0 || null_callbacks[1] != 0 || null_slot != 0 ||
           local_null != 0 || (void *) object_slot != (void *) &object ||
           (void *) local_object != (void *) &object ||
           (void *) integer_slot != (void *) 16;
}
