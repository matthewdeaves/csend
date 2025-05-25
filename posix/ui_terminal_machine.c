#include "ui_interface.h"
#include "ui_factory.h"
#include "logging.h"
#include "../shared/logging.h"
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Machine mode implementation with improved I/O handling */

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe output function with proper flushing */
static void machine_output(const char *format, ...)
{
    pthread_mutex_lock(&output_mutex);
    
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    printf("\n");
    fflush(stdout);
    
    pthread_mutex_unlock(&output_mutex);
}


static void machine_init(void *context)
{
    (void)context;
    /* Set stdout to line buffered mode for better reliability */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Set stdin to line buffered mode */
    setvbuf(stdin, NULL, _IOLBF, 0);
}

static void machine_cleanup(void *context)
{
    (void)context;
    /* Ensure all output is flushed before exit */
    fflush(stdout);
}

static void machine_display_message(void *context, const char *from_username, 
                                   const char *from_ip, const char *content)
{
    (void)context;
    machine_output("MESSAGE:from=%s:ip=%s:content=%s", from_username, from_ip, content);
}

static void machine_display_app_message(void *context, const char *format, va_list args)
{
    (void)context;
    /* In machine mode, most app messages are suppressed */
    /* Only log them, don't display */
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    log_debug("App message (suppressed in machine mode): %s", buffer);
}

static void machine_display_error(void *context, const char *format, va_list args)
{
    (void)context;
    pthread_mutex_lock(&output_mutex);
    
    printf("ERROR:");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    
    pthread_mutex_unlock(&output_mutex);
}

static void machine_display_peer_list(void *context, app_state_t *state)
{
    (void)context;
    pthread_mutex_lock(&state->peers_mutex);
    time_t now = time(NULL);
    int active_count = 0;
    
    machine_output("PEER_LIST_START");
    
    for (int i = 0; i < MAX_PEERS; i++) {
        if (state->peer_manager.peers[i].active) {
            if (difftime(now, state->peer_manager.peers[i].last_seen) > PEER_TIMEOUT) {
                log_debug("Peer %s@%s timed out (detected in print_peers).",
                          state->peer_manager.peers[i].username,
                          state->peer_manager.peers[i].ip);
                state->peer_manager.peers[i].active = 0;
                continue;
            }
            
            machine_output("PEER:%d:%s:%s:%ld",
                   ++active_count,
                   state->peer_manager.peers[i].username,
                   state->peer_manager.peers[i].ip,
                   (long)(now - state->peer_manager.peers[i].last_seen));
        }
    }
    
    machine_output("PEER_LIST_END:count=%d", active_count);
    
    pthread_mutex_unlock(&state->peers_mutex);
}

static void machine_display_help(void *context)
{
    (void)context;
    /* Help is not shown in machine mode */
}

static void machine_notify_send_result(void *context, int success, int peer_num, const char *peer_ip)
{
    (void)context;
    if (success) {
        machine_output("SEND_RESULT:success:peer=%d:ip=%s", peer_num, peer_ip);
    } else {
        if (peer_num < 0) {
            machine_output("SEND_RESULT:error:invalid_peer");
        } else {
            machine_output("SEND_RESULT:error:failed_to_send");
        }
    }
}

static void machine_notify_broadcast_result(void *context, int sent_count)
{
    (void)context;
    machine_output("BROADCAST_RESULT:sent_count=%d", sent_count);
}

static void machine_notify_command_unknown(void *context, const char *command)
{
    (void)context;
    machine_output("COMMAND_ERROR:unknown:%s", command);
}

static void machine_notify_peer_update(void *context)
{
    (void)context;
    machine_output("PEER_UPDATE");
}

static void machine_notify_debug_toggle(void *context, int enabled)
{
    (void)context;
    machine_output("DEBUG_TOGGLE:%s", enabled ? "enabled" : "disabled");
}

static void machine_show_prompt(void *context)
{
    (void)context;
    /* No prompt in machine mode */
}

static void machine_handle_command_start(void *context, const char *command)
{
    (void)context;
    machine_output("CMD:%s", command);
}

static void machine_handle_command_complete(void *context)
{
    (void)context;
    machine_output("CMD_COMPLETE");
}

static void machine_notify_startup(void *context, const char *username)
{
    (void)context;
    machine_output("START:username=%s", username);
}

static void machine_notify_shutdown(void *context)
{
    (void)context;
    machine_output("SHUTDOWN");
}

static void machine_notify_ready(void *context)
{
    (void)context;
    machine_output("READY");
}

/* Static operations table */
static ui_operations_t machine_ops = {
    .init = machine_init,
    .cleanup = machine_cleanup,
    .display_message = machine_display_message,
    .display_app_message = machine_display_app_message,
    .display_error = machine_display_error,
    .display_peer_list = machine_display_peer_list,
    .display_help = machine_display_help,
    .notify_send_result = machine_notify_send_result,
    .notify_broadcast_result = machine_notify_broadcast_result,
    .notify_command_unknown = machine_notify_command_unknown,
    .notify_peer_update = machine_notify_peer_update,
    .notify_debug_toggle = machine_notify_debug_toggle,
    .show_prompt = machine_show_prompt,
    .handle_command_start = machine_handle_command_start,
    .handle_command_complete = machine_handle_command_complete,
    .notify_startup = machine_notify_startup,
    .notify_shutdown = machine_notify_shutdown,
    .notify_ready = machine_notify_ready
};

ui_operations_t *ui_terminal_machine_ops(void)
{
    return &machine_ops;
}