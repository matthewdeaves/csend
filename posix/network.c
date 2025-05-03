#include "network.h"
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
int get_local_ip(char *buffer, size_t size) {
    struct ifaddrs *ifaddr, *ifa;
    int family;
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs failed");
        return -1;
    }
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL)
            continue;
        family = ifa->ifa_addr->sa_family;
        if (family == AF_INET) {
            char ip_str[INET_ADDRSTRLEN];
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
            if (strncmp(ip_str, "127.", 4) == 0) {
                continue;
            }
            strncpy(buffer, ip_str, size);
            buffer[size - 1] = '\0';
            freeifaddrs(ifaddr);
            return 0;
        }
    }
    freeifaddrs(ifaddr);
    return -1;
}
void set_socket_timeout(int socket, int seconds) {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}
