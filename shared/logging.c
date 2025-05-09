#include "logging.h"
static Boolean g_show_debug_output = false;
void set_debug_output_enabled(Boolean enabled)
{
    g_show_debug_output = enabled;
}
Boolean is_debug_output_enabled(void)
{
    return g_show_debug_output;
}
