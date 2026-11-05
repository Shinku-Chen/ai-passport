#include "yakata_flow.h"
#include <stdio.h>

/* Batched protocol so Python can compare the compiled C against the source
 * engine's condition/effect semantics, rather than reimplementing C in tests. */
int main(void)
{
    int kind, index, option;
    int32_t v[YAKATA_VAR_COUNT];
    while (scanf("%d%d%d", &kind, &index, &option) == 3) {
        for (int i=0; i<YAKATA_VAR_COUNT; ++i) {
            int value;
            if (scanf("%d", &value) != 1) return 2;
            v[i]=value;
        }
        int next=kind ? yakata_choose(index,option,v) : yakata_exit(index,v);
        printf("%d",next);
        for (int i=0; i<YAKATA_VAR_COUNT; ++i) printf(" %d",(int)v[i]);
        putchar('\n');
    }
    return 0;
}
