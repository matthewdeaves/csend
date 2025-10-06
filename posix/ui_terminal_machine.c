#include "ui_interface.h"
#include "ui_factory.h"
#include "logging.h"
#include "../shared/logging.h"
#include "../shared/common_defs.h"
#include "../shared/peer_wrapper.h"
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/* Machine mode implementation with JSON output */

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static char current_command_id[64] = {0};
static time_t start_time = 0;

/* Message history for /history command */
#define MAX_HISTORY 100
typedef struct {
    char from_username[32];
    char from_ip[32];
    char content[256];
    time_t timestamp;
} message_history_t;

static message_history_t message_history[MAX_HISTORY];
static int history_index = 0;
static int history_count = 0;
static pthread_mutex_t history_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Statistics tracking */
typedef struct {
    unsigned int messages_sent;
    unsigned int messages_received;
    unsigned int broadcasts_sent;
    unsigned int peers_seen;
} stats_t;

static stats_t stats = {0};

/* Get current ISO8601 timestamp */
static void get_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

/* Thread-safe JSON output function */
static void json_output(const char *json)
{
    pthread_mutex_lock(&output_mutex);
    printf("%s\n", json);
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}

/* Escape string for JSON */
static void json_escape(char *dest, const char *src, size_t dest_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        switch (src[i]) {
        case '"':
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = '"';
            }
            break;
        case '\\':
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = '\\';
            }
            break;
        case '\n':
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = 'n';
            }
            break;
        case '\r':
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = 'r';
            }
            break;
        case '\t':
            if (j < dest_size - 2) {
                dest[j++] = '\\';
                dest[j++] = 't';
            }
            break;
        default:
            dest[j++] = src[i];
        }
        i++;
    }
    dest[j] = '\0';
}


static void machine_init(void *context)
{
    (void)context;
    /* Set stdout to line buffered mode for better reliability */
    setvbuf(stdout, NULL, _IOLBF, 0);
    /* Set stdin to line buffered mode */
    setvbuf(stdin, NULL, _IOLBF, 0);

    /* Initialize start time */
    start_time = time(NULL);
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
    char timestamp[32];
    char escaped_content[512];
    char json[1024];

    get_timestamp(timestamp, sizeof(timestamp));
    json_escape(escaped_content, content, sizeof(escaped_content));

    /* Add to history */
    pthread_mutex_lock(&history_mutex);
    strncpy(message_history[history_index].from_username, from_username, 31);
    strncpy(message_history[history_index].from_ip, from_ip, 31);
    strncpy(message_history[history_index].content, content, 255);
    message_history[history_index].timestamp = time(NULL);
    history_index = (history_index + 1) % MAX_HISTORY;
    if (history_count < MAX_HISTORY) history_count++;
    pthread_mutex_unlock(&history_mutex);

    stats.messages_received++;

    snprintf(json, sizeof(json),
             "{\"type\":\"event\",\"event\":\"message\",\"timestamp\":\"%s\","
             "\"data\":{\"from\":{\"username\":\"%s\",\"ip\":\"%s\"},"
             "\"content\":\"%s\",\"message_id\":\"msg_%u\"}}",
             timestamp, from_username, from_ip, escaped_content, stats.messages_received);

    json_output(json);
}

static void machine_display_app_message(void *context, const char *format, va_list args)
{
    (void)context;
    /* In machine mode, app messages are logged only */
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    log_debug_cat(LOG_CAT_UI, "App message: %s", buffer);
}

static void machine_display_error(void *context, const char *format, va_list args)
{
    (void)context;
    char timestamp[32];
    char error_msg[512];
    char json[1024];

    get_timestamp(timestamp, sizeof(timestamp));
    vsnprintf(error_msg, sizeof(error_msg), format, args);

    snprintf(json, sizeof(json),
             "{\"type\":\"error\",\"timestamp\":\"%s\","
             "\"error\":{\"code\":\"INTERNAL_ERROR\",\"message\":\"%s\"}}",
             timestamp, error_msg);

    json_output(json);
}

static void machine_display_peer_list(void *context, app_state_t *state)
{
    (void)context;
    (void)state;
    char timestamp[32];
    char json[4096];
    char peers_json[3072] = "[";
    int first = 1;

    get_timestamp(timestamp, sizeof(timestamp));

    int active_count = pw_get_active_peer_count();

    for (int i = 0; i < active_count; i++) {
        peer_t peer;
        pw_get_peer_by_index(i, &peer);

        char peer_timestamp[32];
        time_t last_seen_time = peer.last_seen;
        struct tm *tm_info = gmtime(&last_seen_time);
        strftime(peer_timestamp, sizeof(peer_timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);

        char peer_json[256];
        snprintf(peer_json, sizeof(peer_json),
                 "%s{\"id\":%d,\"username\":\"%s\",\"ip\":\"%s\",\"last_seen\":\"%s\",\"status\":\"active\"}",
                 first ? "" : ",",
                 i + 1,
                 peer.username,
                 peer.ip,
                 peer_timestamp);

        strcat(peers_json, peer_json);
        first = 0;
    }

    strcat(peers_json, "]");

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/list\",\"data\":{\"peers\":%s,\"count\":%d}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, peers_json, active_count);

    json_output(json);
}

static void machine_display_help(void *context)
{
    (void)context;
    char timestamp[32];
    char json[1024];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/help\",\"data\":{\"commands\":["
             "\"/list\",\"/send <id> <msg>\",\"/broadcast <msg>\","
             "\"/status\",\"/stats\",\"/history [count]\","
             "\"/peers --filter <pattern>\",\"/version\","
             "\"/debug\",\"/quit\",\"/help\"]}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp);

    json_output(json);
}

static void machine_notify_send_result(void *context, int success, int peer_num, const char *peer_ip)
{
    (void)context;
    char timestamp[32];
    char json[512];

    get_timestamp(timestamp, sizeof(timestamp));
    stats.messages_sent++;

    if (success) {
        snprintf(json, sizeof(json),
                 "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
                 "\"command\":\"/send\",\"data\":{\"success\":true,"
                 "\"peer\":{\"id\":%d,\"ip\":\"%s\"},\"message_id\":\"msg_%u\"}}",
                 current_command_id[0] ? current_command_id : "null",
                 timestamp, peer_num, peer_ip, stats.messages_sent);
    } else {
        const char *error_code = (peer_num < 0) ? "PEER_NOT_FOUND" : "NETWORK_ERROR";
        const char *error_msg = (peer_num < 0) ? "Invalid peer number" : "Failed to send message";

        snprintf(json, sizeof(json),
                 "{\"type\":\"error\",\"id\":\"%s\",\"timestamp\":\"%s\","
                 "\"error\":{\"code\":\"%s\",\"message\":\"%s\","
                 "\"details\":{\"peer_id\":%d}}}",
                 current_command_id[0] ? current_command_id : "null",
                 timestamp, error_code, error_msg, peer_num);
    }

    json_output(json);
}

static void machine_notify_broadcast_result(void *context, int sent_count)
{
    (void)context;
    char timestamp[32];
    char json[512];

    get_timestamp(timestamp, sizeof(timestamp));
    stats.broadcasts_sent++;

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/broadcast\",\"data\":{\"sent_count\":%d}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, sent_count);

    json_output(json);
}

static void machine_notify_command_unknown(void *context, const char *command)
{
    (void)context;
    char timestamp[32];
    char json[512];
    char escaped_cmd[128];

    get_timestamp(timestamp, sizeof(timestamp));
    json_escape(escaped_cmd, command, sizeof(escaped_cmd));

    snprintf(json, sizeof(json),
             "{\"type\":\"error\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"error\":{\"code\":\"UNKNOWN_COMMAND\","
             "\"message\":\"Command not recognized\","
             "\"details\":{\"command\":\"%s\"}}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, escaped_cmd);

    json_output(json);
}

static void machine_notify_peer_update(void *context)
{
    (void)context;
    char timestamp[32];
    char json[256];

    get_timestamp(timestamp, sizeof(timestamp));
    stats.peers_seen++;

    snprintf(json, sizeof(json),
             "{\"type\":\"event\",\"event\":\"peer_update\","
             "\"timestamp\":\"%s\",\"data\":{\"action\":\"changed\"}}",
             timestamp);

    json_output(json);
}

static void machine_notify_debug_toggle(void *context, int enabled)
{
    (void)context;
    char timestamp[32];
    char json[256];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/debug\",\"data\":{\"enabled\":%s}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, enabled ? "true" : "false");

    json_output(json);
}

static void machine_show_prompt(void *context)
{
    (void)context;
    /* No prompt in machine mode */
}

static void machine_handle_command_start(void *context, const char *command)
{
    (void)context;
    /* Extract correlation ID if present */
    const char *id_start = strstr(command, "--id=");
    if (id_start) {
        id_start += 5;
        const char *id_end = strchr(id_start, ' ');
        size_t id_len = id_end ? (size_t)(id_end - id_start) : strlen(id_start);
        if (id_len < sizeof(current_command_id)) {
            strncpy(current_command_id, id_start, id_len);
            current_command_id[id_len] = '\0';
        }
    } else {
        current_command_id[0] = '\0';
    }
}

static void machine_handle_command_complete(void *context)
{
    (void)context;
    /* Clear command ID after completion */
    current_command_id[0] = '\0';
}

static void machine_notify_startup(void *context, const char *username)
{
    (void)context;
    char timestamp[32];
    char json[256];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"start\",\"version\":\"2.0\","
             "\"username\":\"%s\",\"timestamp\":\"%s\"}",
             username, timestamp);

    json_output(json);
}

static void machine_notify_shutdown(void *context)
{
    (void)context;
    char timestamp[32];
    char json[128];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"shutdown\",\"timestamp\":\"%s\"}",
             timestamp);

    json_output(json);
}

static void machine_notify_ready(void *context)
{
    (void)context;
    char timestamp[32];
    char json[128];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"ready\",\"timestamp\":\"%s\"}",
             timestamp);

    json_output(json);
}

/* Extended command handlers */
static void machine_notify_status(void *context, app_state_t *state)
{
    (void)context;
    char timestamp[32];
    char json[1024];

    get_timestamp(timestamp, sizeof(timestamp));
    time_t uptime = time(NULL) - start_time;

    int active_peers = pw_get_active_peer_count();

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/status\",\"data\":{"
             "\"uptime_seconds\":%ld,\"version\":\"2.0\","
             "\"username\":\"%s\",\"network\":{"
             "\"tcp_port\":%d,\"udp_port\":%d},"
             "\"statistics\":{"
             "\"messages_sent\":%u,\"messages_received\":%u,"
             "\"broadcasts_sent\":%u,\"active_peers\":%d}}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, uptime, state->username,
             PORT_TCP, PORT_UDP,
             stats.messages_sent, stats.messages_received,
             stats.broadcasts_sent, active_peers);

    json_output(json);
}

static void machine_notify_stats(void *context, app_state_t *state)
{
    (void)context;
    (void)state;
    char timestamp[32];
    char json[512];

    get_timestamp(timestamp, sizeof(timestamp));

    int total_peers = pw_get_active_peer_count();

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/stats\",\"data\":{"
             "\"messages_sent\":%u,\"messages_received\":%u,"
             "\"broadcasts_sent\":%u,\"total_peers_seen\":%u,"
             "\"current_active_peers\":%d}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp,
             stats.messages_sent, stats.messages_received,
             stats.broadcasts_sent, stats.peers_seen, total_peers);

    json_output(json);
}

static void machine_notify_history(void *context, int count)
{
    (void)context;
    char timestamp[32];
    char json[8192];  /* Increased buffer size */
    char history_json[6144] = "[";  /* Increased buffer size */
    int first = 1;
    size_t history_len = 1;  /* Track current length */

    get_timestamp(timestamp, sizeof(timestamp));

    pthread_mutex_lock(&history_mutex);
    int start_idx = (history_count > count) ?
                    (history_index - count + MAX_HISTORY) % MAX_HISTORY :
                    (history_index - history_count + MAX_HISTORY) % MAX_HISTORY;

    int items_to_show = (count > history_count) ? history_count : count;

    for (int i = 0; i < items_to_show; i++) {
        int idx = (start_idx + i) % MAX_HISTORY;
        if (message_history[idx].timestamp > 0) {
            char msg_timestamp[32];
            struct tm *tm_info = gmtime(&message_history[idx].timestamp);
            strftime(msg_timestamp, sizeof(msg_timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);

            char escaped_content[512];
            json_escape(escaped_content, message_history[idx].content, sizeof(escaped_content));

            char msg_json[1024];  /* Increased buffer size */
            int written = snprintf(msg_json, sizeof(msg_json),
                                   "%s{\"timestamp\":\"%s\",\"from\":\"%s\",\"content\":\"%s\"}",
                                   first ? "" : ",",
                                   msg_timestamp,
                                   message_history[idx].from_username,
                                   escaped_content);

            /* Check if we have room to append */
            if (written > 0 && history_len + written < sizeof(history_json) - 2) {
                strcat(history_json, msg_json);
                history_len += written;
                first = 0;
            }
        }
    }
    pthread_mutex_unlock(&history_mutex);

    strcat(history_json, "]");

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/history\",\"data\":{\"messages\":%s,\"count\":%d}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, history_json, items_to_show);

    json_output(json);
}

static void machine_notify_version(void *context)
{
    (void)context;
    char timestamp[32];
    char json[256];

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/version\",\"data\":{"
             "\"protocol_version\":\"2.0\",\"app_version\":\"1.0\"}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp);

    json_output(json);
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
    .notify_status = machine_notify_status,
    .notify_stats = machine_notify_stats,
    .notify_history = machine_notify_history,
    .notify_version = machine_notify_version,
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