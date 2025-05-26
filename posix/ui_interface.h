#ifndef UI_INTERFACE_H
#define UI_INTERFACE_H

#include "peer.h"
#include <stdarg.h>

/* UI operations interface - Strategy pattern for different UI implementations */
typedef struct ui_operations {
    /* Lifecycle */
    void (*init)(void *context);
    void (*cleanup)(void *context);
    
    /* Output operations */
    void (*display_message)(void *context, const char *from_username, const char *from_ip, 
                           const char *content);
    void (*display_app_message)(void *context, const char *format, va_list args);
    void (*display_error)(void *context, const char *format, va_list args);
    void (*display_peer_list)(void *context, app_state_t *state);
    void (*display_help)(void *context);
    
    /* Command result notifications */
    void (*notify_send_result)(void *context, int success, int peer_num, const char *peer_ip);
    void (*notify_broadcast_result)(void *context, int sent_count);
    void (*notify_command_unknown)(void *context, const char *command);
    void (*notify_peer_update)(void *context);
    void (*notify_debug_toggle)(void *context, int enabled);
    
    /* Extended command notifications */
    void (*notify_status)(void *context, app_state_t *state);
    void (*notify_stats)(void *context, app_state_t *state);
    void (*notify_history)(void *context, int count);
    void (*notify_version)(void *context);
    
    /* Input operations */
    void (*show_prompt)(void *context);
    void (*handle_command_start)(void *context, const char *command);
    void (*handle_command_complete)(void *context);
    
    /* Status notifications */
    void (*notify_startup)(void *context, const char *username);
    void (*notify_shutdown)(void *context);
    void (*notify_ready)(void *context);
} ui_operations_t;

/* UI context structure - implementations can extend this */
typedef struct ui_context {
    ui_operations_t *ops;
    void *impl_data;  /* Implementation-specific data */
} ui_context_t;

/* Helper macros for calling UI operations */
#define UI_CALL(ui, op, ...) \
    do { \
        if ((ui) && (ui)->ops && (ui)->ops->op) { \
            (ui)->ops->op((ui), ##__VA_ARGS__); \
        } \
    } while(0)

#define UI_CALL_VA(ui, op, fmt, args) \
    do { \
        if ((ui) && (ui)->ops && (ui)->ops->op) { \
            (ui)->ops->op((ui), fmt, args); \
        } \
    } while(0)

#endif /* UI_INTERFACE_H */