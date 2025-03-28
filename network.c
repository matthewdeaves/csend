// Include the header file for this module, declaring the functions defined here.
// It also brings in necessary types like app_state_t and size_t.
#include "network.h"

// --- System Headers ---
// Required for getifaddrs() and freeifaddrs() to query network interface addresses.
#include <ifaddrs.h>
// Required for network address conversion functions (inet_ntop, inet_pton)
// and socket-related functions/structures.
#include <arpa/inet.h>
// Core socket functions and structures (socket, bind, listen, accept, connect, send, recv, setsockopt).
#include <sys/socket.h>
// Defines internet domain address structures (struct sockaddr_in) and constants (AF_INET, INADDR_ANY).
#include <netinet/in.h> // Often included by arpa/inet.h or sys/socket.h, but good practice to be explicit.

// Standard C library for input/output, primarily used here for perror.
#include <stdio.h>
// Standard C library for string manipulation (strncpy, strncmp, memset, strlen).
#include <string.h>
// Defines error constants like errno and EINTR (Interrupted system call).
#include <errno.h>
// Required for struct timeval, used by select() and set_socket_timeout().
#include <sys/time.h>

/**
 * @brief Retrieves the primary non-loopback IPv4 address of the local machine.
 * @details Iterates through the network interfaces using `getifaddrs`. It looks for
 *          the first active IPv4 interface that is not the loopback interface
 *          (i.e., not starting with "127.").
 * @param buffer A character buffer where the retrieved IP address string will be stored.
 * @param size The size of the provided buffer. `strncpy` is used to prevent overflow.
 * @return 0 on success (IP address found and copied to buffer).
 * @return -1 on failure (e.g., `getifaddrs` failed, or no suitable IPv4 address found).
 *         Error details for `getifaddrs` are printed via `perror`.
 */
int get_local_ip(char *buffer, size_t size) {
    // Pointers for iterating through the linked list of interface addresses.
    struct ifaddrs *ifaddr, *ifa;
    // Variable to store the address family (e.g., AF_INET for IPv4).
    int family;

    // Get a linked list of all network interfaces on the system.
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs failed");
        return -1; // Return failure if unable to get interface list.
    }

    // Iterate through the linked list of interfaces.
    // `ifa` points to the current interface structure in the list.
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        // Skip interfaces that don't have an address structure associated with them.
        if (ifa->ifa_addr == NULL)
            continue;

        // Get the address family of the current interface.
        family = ifa->ifa_addr->sa_family;

        // Check if it's an IPv4 address.
        if (family == AF_INET) {
            // Buffer to hold the string representation of the IP address.
            char ip_str[INET_ADDRSTRLEN]; // Max length for IPv4 string + null terminator.
            // Cast the generic sockaddr to the specific sockaddr_in for IPv4.
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)ifa->ifa_addr;

            // Convert the binary IPv4 address to a human-readable string.
            // AF_INET: Address family.
            // &(ipv4->sin_addr): Pointer to the binary IP address.
            // ip_str: Destination buffer for the string.
            // INET_ADDRSTRLEN: Size of the destination buffer.
            inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);

            // Check if the IP address is a loopback address (starts with "127.").
            // We want the primary *external* or *LAN* IP, not the internal loopback.
            if (strncmp(ip_str, "127.", 4) == 0) {
                continue; // Skip this interface and move to the next.
            }

            // Found a non-loopback IPv4 address. Copy it to the output buffer.
            // Use strncpy for safety, ensuring we don't write past the buffer end.
            strncpy(buffer, ip_str, size);
            // Explicitly null-terminate the buffer, as strncpy might not if size is reached.
            buffer[size - 1] = '\0';

            // Free the memory allocated by getifaddrs.
            freeifaddrs(ifaddr);
            return 0; // Return success.
        }
        // Note: Could add an 'else if (family == AF_INET6)' block here to handle IPv6 if needed.
    }

    // If the loop finishes without finding a suitable address, free the memory.
    freeifaddrs(ifaddr);
    // Return failure code indicating no suitable IP was found.
    return -1;
}

/**
 * @brief Sets receive and send timeouts for a given socket.
 * @details Configures the `SO_RCVTIMEO` and `SO_SNDTIMEO` socket options.
 *          If a receive or send operation on the socket takes longer than the
 *          specified timeout, the operation will fail with `errno` set to
 *          `EAGAIN` or `EWOULDBLOCK`.
 * @param socket The file descriptor of the socket to configure.
 * @param seconds The timeout duration in seconds. Fractional seconds are not supported here.
 */
void set_socket_timeout(int socket, int seconds) {
    // Structure to define the timeout duration.
    struct timeval tv;
    // Set the seconds part of the timeout.
    tv.tv_sec = seconds;
    // Set the microseconds part of the timeout (0 in this case).
    tv.tv_usec = 0;

    // Set the receive timeout option.
    // SOL_SOCKET: Option level (general socket options).
    // SO_RCVTIMEO: Option name for receive timeout.
    // &tv: Pointer to the timeval structure containing the timeout value.
    // sizeof(tv): Size of the timeval structure.
    // Note: Error checking for setsockopt is omitted here for brevity, but should ideally be included.
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    // Set the send timeout option.
    // SO_SNDTIMEO: Option name for send timeout.
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
}