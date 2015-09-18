
#include <malloc.h>
void* (* volatile __malloc_hook)(size_t, const void*) = 0;
