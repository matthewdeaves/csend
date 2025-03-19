#ifndef PEER_H
#define PEER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/select.h>

#define PORT_TCP 8080
#define PORT_UDP 8081
#define BUFFER_SIZE 1024
#define MAX_PEERS 10
#define DISCOVERY_INTERVAL 10 // seconds
#define PEER_TIMEOUT 30 // seconds

// Message types
#define MSG_DISCOVERY "DISCOVERY"
#define MSG_DISCOVERY_RESPONSE "DISCOVERY_RESPONSE"
#define MSG_TEXT "TEXT"
#define MSG_QUIT "QUIT"

// Peer structure
typedef struct {
    char ip[INET_ADDRSTRLEN];
    char username[32];
    time_t last_seen;
    int active;
} peer_t;

// Global state
typedef struct {
    volatile sig_atomic_t running;  // Flag indicating if the application is running; volatile ensures visibility across threads
                                    // and sig_atomic_t guarantees atomic access from signal handlers
    
    peer_t peers[MAX_PEERS];        // Array of peer structures containing information about connected peers
                                    // Limited to MAX_PEERS entries to prevent unbounded memory usage
    
    int tcp_socket;                 // File descriptor for the TCP socket used for reliable message exchange
                                    // Negative value indicates an uninitialized or closed socket
    
    int udp_socket;                 // File descriptor for the UDP socket used for peer discovery broadcasts
                                    // Negative value indicates an uninitialized or closed socket
    
    char username[32];              // User's chosen display name, limited to 31 characters plus null terminator
                                    // Used to identify this peer in messages to others
    
    pthread_mutex_t peers_mutex;    // Mutex to protect concurrent access to the peers array
                                    // Ensures thread safety when multiple threads read/write peer information
                                    // Basically means r/w to this var can not be interrupted. Threads call for to
                                    // hold the lock on this var and will be blocked until the lock is released by another
} app_state_t;

// Function declarations

// Main components
void init_app_state(app_state_t *state, const char *username);
void cleanup_app_state(app_state_t *state);

// Network functions
int init_listener(app_state_t *state);
int init_discovery(app_state_t *state);
void *listener_thread(void *arg);
void *discovery_thread(void *arg);
void *user_input_thread(void *arg);
int send_message(const char *ip, const char *message, const char *msg_type);
int broadcast_discovery(app_state_t *state);

// Protocol functions
int parse_message(const char *buffer, char *sender_ip, char *sender_username, char *msg_type, char *content);
int format_message(char *buffer, int buffer_size, const char *msg_type, 
                  const char *sender, const char *content);

// Utility functions
void handle_signal(int sig);
void log_message(const char *format, ...);
int add_peer(app_state_t *state, const char *ip, const char *username);
void print_peers(app_state_t *state);
void set_socket_timeout(int socket, int seconds);
int get_local_ip(char *buffer, size_t size);

// Global state for signal handler
extern app_state_t *g_state;

#endif // PEER_H