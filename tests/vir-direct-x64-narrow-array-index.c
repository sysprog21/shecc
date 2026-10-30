/* Defined after main, so the call stays and keeps the loads from being
 * forwarded the stored values.
 */
void barrier(void);
int main(int argc)
{
    signed char bytes[9];
    unsigned short words[3];
    int byte_index = argc + 6;
    int word_index = argc;

    bytes[byte_index] = -5;
    words[word_index] = 50000;
    barrier();
    return bytes[byte_index] + words[word_index] - 49766;
}
void barrier(void) {}
