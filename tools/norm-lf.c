/*
 * Convert all line endings to LF (Unix style)
 *
 * This tool ensures consistent line endings before processing with inliner.
 * It converts CR-only (old Mac) and CRLF (Windows) to LF (Unix).
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }

    FILE *input = fopen(argv[1], "rb");
    if (!input) {
        fprintf(stderr, "Error: Cannot open input file '%s'\n", argv[1]);
        return 1;
    }

    FILE *output = fopen(argv[2], "wb");
    if (!output) {
        fprintf(stderr, "Error: Cannot create output file '%s'\n", argv[2]);
        fclose(input);
        return 1;
    }

    int c;
    int prev_cr = 0;
    bool has_crlf = false;
    bool has_lf = false;
    bool has_cr_only = false;

    while ((c = fgetc(input)) != EOF) {
        if (c == '\r') {
            /* Mark that we saw a CR, but don't output it yet */
            prev_cr = 1;
        } else if (c == '\n') {
            if (prev_cr) {
                /* CRLF sequence - output single LF */
                has_crlf = true;
            } else {
                /* LF only */
                has_lf = true;
            }
            fputc('\n', output);
            prev_cr = 0;
        } else {
            if (prev_cr) {
                /* CR not followed by LF - convert to LF */
                fputc('\n', output);
                has_cr_only = true;
            }
            fputc(c, output);
            prev_cr = 0;
        }
    }

    /* Handle CR at end of file */
    if (prev_cr) {
        fputc('\n', output);
        has_cr_only = true;
    }

    fclose(input);
    fclose(output);

    /* Report what was found and converted */
    if (has_cr_only) {
        fprintf(stderr,
                "Warning: Converted CR-only line endings to LF in '%s'\n",
                argv[1]);
    }
    if ((has_crlf && has_lf) || (has_crlf && has_cr_only) ||
        (has_lf && has_cr_only)) {
        fprintf(stderr, "Warning: Converted mixed line endings to LF in '%s'\n",
                argv[1]);
    }

    return 0;
}
