#include "logging.h"
#include <stdio.h>
static Boolean g_show_debug_output = false;
static platform_display_debug_log_func_t g_platform_display_func_ptr = NULL;
void shared_logging_set_platform_callback(platform_display_debug_log_func_t display_func)
{
    g_platform_display_func_ptr = display_func;
}
void shared_logging_clear_platform_callback(void)
{
    g_platform_display_func_ptr = NULL;
}
platform_display_debug_log_func_t shared_logging_get_platform_callback(void)
{
    return g_platform_display_func_ptr;
}
void set_debug_output_enabled(Boolean enabled)
{
    g_show_debug_output = enabled;
}
Boolean is_debug_output_enabled(void)
{
    return g_show_debug_output;
}
