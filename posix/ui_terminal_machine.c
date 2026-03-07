#include "ui_interface.h"
#include "ui_factory.h"
#include "peertalk_bridge.h"
#include "clog.h"
#include "../shared/common_defs.h"
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>

static pthread_mutex_t output_mutex = PTHREAD_MUTEX_INITIALIZER;
static char current_command_id[64] = {0};
static time_t start_time = 0;

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

typedef struct {
    unsigned int messages_sent;
    unsigned int messages_received;
    unsigned int broadcasts_sent;
    unsigned int peers_seen;
} stats_t;

static stats_t stats = {0, 0, 0, 0};

static void get_timestamp(char *buffer, size_t size)
{
    time_t now = time(NULL);
    const struct tm *tm_info = gmtime(&now);
    strftime(buffer, size, "%Y-%m-%dT%H:%M:%SZ", tm_info);
}

static void json_output(const char *json)
{
    pthread_mutex_lock(&output_mutex);
    printf("%s\n", json);
    fflush(stdout);
    pthread_mutex_unlock(&output_mutex);
}

static void json_escape(char *dest, const char *src, size_t dest_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j < dest_size - 1) {
        switch (src[i]) {
        case '"':
            if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = '"'; }
            break;
        case '\\':
            if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = '\\'; }
            break;
        case '\n':
            if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'n'; }
            break;
        case '\r':
            if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 'r'; }
            break;
        case '\t':
            if (j < dest_size - 2) { dest[j++] = '\\'; dest[j++] = 't'; }
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
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stdin, NULL, _IOLBF, 0);
    start_time = time(NULL);
}

static void machine_cleanup(void *context)
{
    (void)context;
    fflush(stdout);
}

static void machine_display_message(void *context, const char *from_username,
                                    const char *from_ip, const char *content)
{
    char timestamp[32];
    char escaped_content[512];
    char json[1024];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    json_escape(escaped_content, content, sizeof(escaped_content));

    pthread_mutex_lock(&history_mutex);
    strncpy(message_history[history_index].from_username, from_username, 31);
    message_history[history_index].from_username[31] = '\0';
    strncpy(message_history[history_index].from_ip, from_ip, 31);
    message_history[history_index].from_ip[31] = '\0';
    strncpy(message_history[history_index].content, content, 255);
    message_history[history_index].content[255] = '\0';
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
    char buffer[1024];
    (void)context;
    vsnprintf(buffer, sizeof(buffer), format, args);
    CLOG_DEBUG("App message: %s", buffer);
}

static void machine_display_error(void *context, const char *format, va_list args)
{
    char timestamp[32];
    char error_msg[512];
    char json[1024];
    (void)context;

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
    char timestamp[32];
    char json[4096];
    char peers_json[3072];
    int first = 1;
    int i, total, connected_num;
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    strcpy(peers_json, "[");
    connected_num = 0;

    total = PT_GetPeerCount(state->pt_ctx);

    for (i = 0; i < total; i++) {
        PT_Peer *peer = PT_GetPeer(state->pt_ctx, i);
        if (peer && PT_GetPeerState(peer) == PT_PEER_CONNECTED) {
            char peer_json[256];
            connected_num++;

            snprintf(peer_json, sizeof(peer_json),
                     "%s{\"id\":%d,\"username\":\"%s\",\"ip\":\"%s\",\"status\":\"connected\"}",
                     first ? "" : ",",
                     connected_num,
                     PT_PeerName(peer),
                     PT_PeerAddress(peer));

            strcat(peers_json, peer_json);
            first = 0;
        }
    }

    strcat(peers_json, "]");

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/list\",\"data\":{\"peers\":%s,\"count\":%d}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, peers_json, connected_num);

    json_output(json);
}

static void machine_display_help(void *context)
{
    char timestamp[32];
    char json[1024];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/help\",\"data\":{\"commands\":["
             "\"/list\",\"/send <id> <msg>\",\"/broadcast <msg>\","
             "\"/status\",\"/stats\",\"/history [count]\","
             "\"/version\",\"/debug\",\"/test\",\"/quit\",\"/help\"]}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp);

    json_output(json);
}

static void machine_notify_send_result(void *context, int success, int peer_num, const char *peer_ip)
{
    char timestamp[32];
    char json[512];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    stats.messages_sent++;

    if (success) {
        snprintf(json, sizeof(json),
                 "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
                 "\"command\":\"/send\",\"data\":{\"success\":true,"
                 "\"peer\":{\"id\":%d,\"ip\":\"%s\"},\"message_id\":\"msg_%u\"}}",
                 current_command_id[0] ? current_command_id : "null",
                 timestamp, peer_num, peer_ip ? peer_ip : "", stats.messages_sent);
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
    char timestamp[32];
    char json[512];
    (void)context;

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
    char timestamp[32];
    char json[512];
    char escaped_cmd[128];
    (void)context;

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
    char timestamp[32];
    char json[256];
    (void)context;

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
    char timestamp[32];
    char json[256];
    (void)context;

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
}

static void machine_handle_command_start(void *context, const char *command)
{
    const char *id_start;
    (void)context;

    id_start = strstr(command, "--id=");
    if (id_start) {
        const char *id_end;
        size_t id_len;
        id_start += 5;
        id_end = strchr(id_start, ' ');
        id_len = id_end ? (size_t)(id_end - id_start) : strlen(id_start);
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
    current_command_id[0] = '\0';
}

static void machine_notify_startup(void *context, const char *username)
{
    char timestamp[32];
    char json[256];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"start\",\"version\":\"2.0\","
             "\"username\":\"%s\",\"timestamp\":\"%s\"}",
             username, timestamp);

    json_output(json);
}

static void machine_notify_shutdown(void *context)
{
    char timestamp[32];
    char json[128];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"shutdown\",\"timestamp\":\"%s\"}", timestamp);

    json_output(json);
}

static void machine_notify_ready(void *context)
{
    char timestamp[32];
    char json[128];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"ready\",\"timestamp\":\"%s\"}", timestamp);

    json_output(json);
}

static void machine_notify_status(void *context, app_state_t *state)
{
    char timestamp[32];
    char json[1024];
    int active_peers;
    time_t uptime;
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    uptime = time(NULL) - start_time;
    active_peers = bridge_get_peer_count(state);

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/status\",\"data\":{"
             "\"uptime_seconds\":%ld,\"version\":\"2.0\","
             "\"username\":\"%s\","
             "\"statistics\":{"
             "\"messages_sent\":%u,\"messages_received\":%u,"
             "\"broadcasts_sent\":%u,\"active_peers\":%d}}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp, (long)uptime, state->username,
             stats.messages_sent, stats.messages_received,
             stats.broadcasts_sent, active_peers);

    json_output(json);
}

static void machine_notify_stats(void *context, app_state_t *state)
{
    char timestamp[32];
    char json[512];
    int total_peers;
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    total_peers = bridge_get_peer_count(state);

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
    char timestamp[32];
    char json[8192];
    char history_json[6144];
    int first = 1;
    size_t history_len = 1;
    int start_idx, items_to_show, i;
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));
    strcpy(history_json, "[");

    pthread_mutex_lock(&history_mutex);
    start_idx = (history_count > count) ?
                (history_index - count + MAX_HISTORY) % MAX_HISTORY :
                (history_index - history_count + MAX_HISTORY) % MAX_HISTORY;

    items_to_show = (count > history_count) ? history_count : count;

    for (i = 0; i < items_to_show; i++) {
        int idx = (start_idx + i) % MAX_HISTORY;
        if (message_history[idx].timestamp > 0) {
            char msg_timestamp[32];
            char escaped_content[512];
            char msg_json[1024];
            int written;
            const struct tm *tm_info = gmtime(&message_history[idx].timestamp);
            strftime(msg_timestamp, sizeof(msg_timestamp), "%Y-%m-%dT%H:%M:%SZ", tm_info);

            json_escape(escaped_content, message_history[idx].content, sizeof(escaped_content));

            written = snprintf(msg_json, sizeof(msg_json),
                               "%s{\"timestamp\":\"%s\",\"from\":\"%s\",\"content\":\"%s\"}",
                               first ? "" : ",",
                               msg_timestamp,
                               message_history[idx].from_username,
                               escaped_content);

            if (written > 0 && history_len + (size_t)written < sizeof(history_json) - 2) {
                strcat(history_json, msg_json);
                history_len += (size_t)written;
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
    char timestamp[32];
    char json[256];
    (void)context;

    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(json, sizeof(json),
             "{\"type\":\"response\",\"id\":\"%s\",\"timestamp\":\"%s\","
             "\"command\":\"/version\",\"data\":{"
             "\"protocol_version\":\"2.0\",\"app_version\":\"2.0\"}}",
             current_command_id[0] ? current_command_id : "null",
             timestamp);

    json_output(json);
}

static ui_operations_t machine_ops = {
    machine_init,
    machine_cleanup,
    machine_display_message,
    machine_display_app_message,
    machine_display_error,
    machine_display_peer_list,
    machine_display_help,
    machine_notify_send_result,
    machine_notify_broadcast_result,
    machine_notify_command_unknown,
    machine_notify_peer_update,
    machine_notify_debug_toggle,
    machine_notify_status,
    machine_notify_stats,
    machine_notify_history,
    machine_notify_version,
    machine_show_prompt,
    machine_handle_command_start,
    machine_handle_command_complete,
    machine_notify_startup,
    machine_notify_shutdown,
    machine_notify_ready
};

ui_operations_t *ui_terminal_machine_ops(void)
{
    return &machine_ops;
}
