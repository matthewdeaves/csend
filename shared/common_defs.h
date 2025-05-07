#ifndef COMMON_DEFS_H
#define COMMON_DEFS_H 
#define BUFFER_SIZE 1024
#define INET_ADDRSTRLEN 16
#define PORT_TCP 8080
#define PORT_UDP 8081
#define MAX_PEERS 10
#define DISCOVERY_INTERVAL 10
#define PEER_TIMEOUT 30
typedef struct {
    char ip[INET_ADDRSTRLEN];
    char username[32];
    unsigned long last_seen;
    int active;
} peer_t;
#endif
