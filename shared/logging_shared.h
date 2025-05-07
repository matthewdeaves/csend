#ifndef LOGGING_SHARED_H
#define LOGGING_SHARED_H 
#ifdef __MACOS__
#include <MacTypes.h>
#else
#ifndef __cplusplus
#include <stdbool.h>
#define Boolean bool
#endif
#endif
void set_debug_output_enabled(Boolean enabled);
Boolean is_debug_output_enabled(void);
#endif
