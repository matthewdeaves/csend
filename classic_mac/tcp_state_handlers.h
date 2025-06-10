#ifndef TCP_STATE_HANDLERS_H
#define TCP_STATE_HANDLERS_H

#include "messaging.h"
#include "network_abstraction.h"
#include <MacTCP.h>

/* State handler function type */
typedef void (*tcp_state_handler_func)(GiveTimePtr giveTime);

/* State handler entry structure */
typedef struct {
    TCPStreamState state;
    tcp_state_handler_func handler;
    const char *description;
} tcp_state_handler_t;

/* Listen stream state handlers */
void handle_listen_idle_state(GiveTimePtr giveTime);
void handle_listen_listening_state(GiveTimePtr giveTime);
void handle_listen_connected_in_state(GiveTimePtr giveTime);
void handle_listen_unexpected_state(GiveTimePtr giveTime);

/* Helper functions for listen stream */
/* REMOVED: Boolean should_wait_for_stream_reset(void); - reset logic eliminated */
void process_listen_async_completion(GiveTimePtr giveTime);
void check_for_incoming_data(GiveTimePtr giveTime);
void handle_connection_accepted(ip_addr remote_ip, tcp_port remote_port, GiveTimePtr giveTime);

/* Send stream state handlers are in messaging.c as ProcessSendStateMachine */
/* Message queue processing is in messaging.c as ProcessMessageQueue */

/* State machine dispatcher */
void dispatch_listen_state_handler(TCPStreamState state, GiveTimePtr giveTime);

#endif /* TCP_STATE_HANDLERS_H */