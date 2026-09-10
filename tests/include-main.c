#include "include-values.h"
#define FIRST_MACRO_HEADER SECOND_MACRO_HEADER
#define SECOND_MACRO_HEADER "include-macro.h"
#include FIRST_MACRO_HEADER

int main(void)
{
    return QUOTED_INCLUDE_BASE + QUOTED_INCLUDE_VALUE + MACRO_INCLUDE_VALUE;
}
