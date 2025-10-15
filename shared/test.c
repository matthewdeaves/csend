#include "test.h"
#include "logging.h"
#include "protocol.h"
#include <stdio.h>
#include <string.h>

/* Platform-specific timing */
#ifdef __CLASSIC_MAC__
#include <OSUtils.h> /* For TickCount() */
#else
#include <sys/time.h> /* For gettimeofday() - wall-clock time */
#endif

/* Defines the current phase of the asynchronous test */
typedef enum {
    TEST_PHASE_IDLE,
    TEST_PHASE_START_ROUND,
    TEST_PHASE_BROADCASTING,
    TEST_PHASE_START_DIRECT,
    TEST_PHASE_DIRECT_MESSAGING,
    TEST_PHASE_END_ROUND,
    TEST_PHASE_FINISHING
} test_phase_t;

/* Holds the entire state of the running test */
typedef struct {
    int is_running; /* Boolean flag (1 for true, 0 for false) */
    test_phase_t phase;
    test_config_t config;
    test_callbacks_t callbacks;
#ifdef __CLASSIC_MAC__
    unsigned long next_step_ticks;
    unsigned long start_time_ticks;
#else
    unsigned long next_step_ms;    /* Wall-clock time in milliseconds */
    unsigned long start_time_ms;   /* Wall-clock time in milliseconds */
#endif

    /* Current progress */
    int current_round;
    int current_broadcast_msg;
    int current_peer_index;
    int current_direct_msg;

    /* Stats */
    int peer_count_at_start;
    int total_messages;
    int failed_messages;
} test_state_t;

static test_state_t g_test_state;

/* Forward declaration */
static void schedule_next_step(int delay_ms);

#ifndef __CLASSIC_MAC__
/* Get current wall-clock time in milliseconds for POSIX platforms */
static unsigned long get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
#endif

test_config_t get_default_test_config(void)
{
    test_config_t config;
    config.broadcast_count = 3;
    config.direct_per_peer = 3;
    config.test_rounds = 4;
    config.delay_ms = 2000;  /* 2 seconds between messages */
    return config;
}

int start_automated_test(const test_config_t *config, const test_callbacks_t *callbacks)
{
    if (g_test_state.is_running) {
        log_app_event("Test: Cannot start, a test is already in progress.");
        return -1;
    }

    memset(&g_test_state, 0, sizeof(g_test_state));
    g_test_state.is_running = 1; /* Set to true */
    g_test_state.phase = TEST_PHASE_START_ROUND;
    g_test_state.config = *config;
    g_test_state.callbacks = *callbacks;

    log_app_event("========================================");
    log_app_event("AUTOMATED TEST START (Async)");
    log_app_event("Configuration: rounds=%d, broadcasts_per_round=%d, direct_per_peer=%d, delay=%dms",
                  config->test_rounds, config->broadcast_count, config->direct_per_peer, config->delay_ms);

    g_test_state.peer_count_at_start = g_test_state.callbacks.get_peer_count(g_test_state.callbacks.context);
    if (g_test_state.peer_count_at_start == 0) {
        log_app_event("Test: No peers available - test aborted");
        stop_automated_test();
        return 0;
    }
    log_app_event("Test: Found %d active peer(s)", g_test_state.peer_count_at_start);

#ifdef __CLASSIC_MAC__
    g_test_state.start_time_ticks = TickCount();
#else
    g_test_state.start_time_ms = get_time_ms();
#endif

    schedule_next_step(0); /* Start immediately */
    return 0;
}

void stop_automated_test(void)
{
    if (g_test_state.is_running) {
        unsigned long duration_ms = 0;
#ifdef __CLASSIC_MAC__
        unsigned long end_time_ticks = TickCount();
        unsigned long duration_ticks = end_time_ticks - g_test_state.start_time_ticks;
        duration_ms = (duration_ticks * 1000) / 60;
#else
        unsigned long end_time_ms = get_time_ms();
        duration_ms = end_time_ms - g_test_state.start_time_ms;
#endif

        log_app_event("========================================");
        log_app_event("AUTOMATED TEST END");
        log_app_event("Test Summary: %d total messages, %d failed, %d succeeded",
                      g_test_state.total_messages, g_test_state.failed_messages,
                      g_test_state.total_messages - g_test_state.failed_messages);
        log_app_event("Test Duration: %lu ms", duration_ms);
        log_app_event("========================================");
    }
    memset(&g_test_state, 0, sizeof(g_test_state));
    g_test_state.is_running = 0; /* Set to false */
    g_test_state.phase = TEST_PHASE_IDLE;
}

int is_automated_test_running(void)
{
    return g_test_state.is_running;
}

void process_automated_test(void)
{
    if (!g_test_state.is_running) {
        return;
    }

#ifdef __CLASSIC_MAC__
    if (TickCount() < g_test_state.next_step_ticks) {
        return; /* Not time for the next step yet */
    }
#else
    if (get_time_ms() < g_test_state.next_step_ms) {
        return; /* Not time for the next step yet */
    }
#endif

    char message[BUFFER_SIZE];
    peer_t peer;

    switch (g_test_state.phase) {
        case TEST_PHASE_START_ROUND:
            g_test_state.current_round++;
            if (g_test_state.current_round > g_test_state.config.test_rounds) {
                g_test_state.phase = TEST_PHASE_FINISHING;
            } else {
                log_app_event("----------------------------------------");
                log_app_event("Test Round %d/%d START", g_test_state.current_round, g_test_state.config.test_rounds);
                log_app_event("----------------------------------------");
                g_test_state.phase = TEST_PHASE_BROADCASTING;
                g_test_state.current_broadcast_msg = 0;
            }
            schedule_next_step(0);
            break;

        case TEST_PHASE_BROADCASTING:
            g_test_state.current_broadcast_msg++;
            if (g_test_state.current_broadcast_msg > g_test_state.config.broadcast_count) {
                g_test_state.phase = TEST_PHASE_START_DIRECT;
                schedule_next_step(0);
            } else {
                snprintf(message, sizeof(message), "TEST_R%d_BROADCAST_%d", g_test_state.current_round, g_test_state.current_broadcast_msg);
                log_app_event("Test Round %d: Broadcasting message %d/%d: '%s'",
                              g_test_state.current_round, g_test_state.current_broadcast_msg, g_test_state.config.broadcast_count, message);

                if (g_test_state.callbacks.send_broadcast(message, g_test_state.callbacks.context) != 0) {
                    log_app_event("Test Round %d: Broadcast %d FAILED", g_test_state.current_round, g_test_state.current_broadcast_msg);
                    g_test_state.failed_messages++;
                }
                g_test_state.total_messages++;
                /* Send next broadcast immediately - let production queue handle pacing */
                schedule_next_step(0);
            }
            break;

        case TEST_PHASE_START_DIRECT:
            log_app_event("Test Round %d - Phase 2: Sending %d direct message(s) to each peer",
                          g_test_state.current_round, g_test_state.config.direct_per_peer);
            g_test_state.phase = TEST_PHASE_DIRECT_MESSAGING;
            g_test_state.current_peer_index = 0;
            g_test_state.current_direct_msg = 0;
            schedule_next_step(0);
            break;

        case TEST_PHASE_DIRECT_MESSAGING:
            if (g_test_state.current_peer_index >= g_test_state.peer_count_at_start) {
                g_test_state.phase = TEST_PHASE_END_ROUND;
                schedule_next_step(0);
                break;
            }

            g_test_state.current_direct_msg++;
            if (g_test_state.current_direct_msg > g_test_state.config.direct_per_peer) {
                /* Move to next peer */
                g_test_state.current_peer_index++;
                g_test_state.current_direct_msg = 0;
                schedule_next_step(0);
            } else {
                if (g_test_state.callbacks.get_peer_by_index(g_test_state.current_peer_index, &peer, g_test_state.callbacks.context) != 0) {
                    log_app_event("Test Round %d: Failed to get peer %d", g_test_state.current_round, g_test_state.current_peer_index);
                    g_test_state.current_peer_index++; /* Skip to next peer */
                    g_test_state.current_direct_msg = 0;
                    schedule_next_step(0);
                    break;
                }

                if (g_test_state.current_direct_msg == 1) {
                     log_app_event("Test Round %d: Sending to peer %d: %s@%s",
                          g_test_state.current_round, g_test_state.current_peer_index + 1, peer.username, peer.ip);
                }

                snprintf(message, sizeof(message), "TEST_R%d_DIRECT_%d_TO_%s_MSG_%d",
                         g_test_state.current_round, g_test_state.current_peer_index + 1, peer.username, g_test_state.current_direct_msg);

                log_app_event("Test Round %d: Direct message %d/%d to %s: '%s'",
                              g_test_state.current_round, g_test_state.current_direct_msg, g_test_state.config.direct_per_peer, peer.username, message);

                if (g_test_state.callbacks.send_direct(peer.ip, message, g_test_state.callbacks.context) != 0) {
                    log_app_event("Test Round %d: Direct message to %s FAILED", g_test_state.current_round, peer.username);
                    g_test_state.failed_messages++;
                }
                g_test_state.total_messages++;
                /* Send next message immediately - let production queue handle pacing */
                schedule_next_step(0);
            }
            break;

        case TEST_PHASE_END_ROUND:
            log_app_event("----------------------------------------");
            log_app_event("Test Round %d/%d COMPLETE", g_test_state.current_round, g_test_state.config.test_rounds);
            log_app_event("----------------------------------------");
            g_test_state.phase = TEST_PHASE_START_ROUND;
            schedule_next_step(g_test_state.config.delay_ms * 2);
            break;

        case TEST_PHASE_FINISHING:
            stop_automated_test();
            break;

        case TEST_PHASE_IDLE:
            /* Should not happen */
            break;
    }
}

static void schedule_next_step(int delay_ms) {
#ifdef __CLASSIC_MAC__
    g_test_state.next_step_ticks = TickCount() + ((unsigned long)delay_ms * 60) / 1000;
#else
    g_test_state.next_step_ms = get_time_ms() + delay_ms;
#endif
}