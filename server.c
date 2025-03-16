#define _POSIX_C_SOURCE 200809L // Stops any VSCode warnings re signal handler setup

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/select.h>

#define PORT 8080
#define BUFFER_SIZE 1024

// Global variables to be accessible in signal handler
int server_fd = -1; // Server socket file descriptor
int client_fd = -1; // Client socket file descriptor
// Special variable, guaranteed read/write in single uninteruptable operation
// Volatile is a compiler directive - do not optimise access to this variabl/do not cache
volatile sig_atomic_t keep_running = 1; 

// Signal handler for graceful shutdown
void handle_signal(int sig) {
    printf("\nReceived signal %d. Shutting down gracefully...\n", sig);
    keep_running = 0;
}

void cleanup() {
    // Close any open client socket
    if (client_fd >= 0) {
        printf("Closing client connection...\n");
        close(client_fd);
        client_fd = -1;
    }
    
    // Close server socket
    if (server_fd >= 0) {
        printf("Closing server socket...\n");
        close(server_fd);
        server_fd = -1;
    }
    
    printf("Cleanup complete.\n");
}

int main() {
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};
    
    // Set up signal handlers for graceful termination
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT, &sa, NULL);  // register a Ctrl+C
    sigaction(SIGTERM, &sa, NULL); // register a kill command
    
    // Creating socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options, trying to be as portable as possible
    #ifdef SO_REUSEPORT
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
            perror("setsockopt");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
    #else
        if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
            perror("setsockopt");
            close(server_fd);
            exit(EXIT_FAILURE);
        }
    #endif
    
    // Configure server address. IPv4, accept connections on any interface on a port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // Bind the socket to the port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
        
    printf("Server listening on port %d...\n", PORT);
    
    // Main server loop - continue until signaled to stop
    while (keep_running) {
        // Set accept to be non-blocking so we can check keep_running flag
        // Create a file descriptor (a reference to an open file, socket, pipe or IO resource)
        fd_set readfds;
        FD_ZERO(&readfds); // Using a macro to have zero bits set on readfds
        FD_SET(server_fd, &readfds); // Add server socket to the set of file descriptors to monitor for read readiness
        
        // Use select with a timeout to make accept non-blocking
        // We listen for a second, check if we got signal to quit, then listen again
        struct timeval timeout;
        timeout.tv_sec = 1;  // 1 second timeout
        timeout.tv_usec = 0;
        
        // This will block until readfds becomes ready for reading, the timeout is reached or we get a signal to stop
        // The timeout allows for us to check if signal to stop was received and cleanup, otherwise go back to listening
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &timeout);
        
        // errno is a global var set by system calls via errno.h
        if (activity < 0 && errno != EINTR) {
            perror("select error");
            break;
        }
        
        // If we were interrupted or timed out, check if we should keep running
        if (activity <= 0) {
            continue;
        }
        
        // Accept a connection if one is pending
        if (FD_ISSET(server_fd, &readfds)) {
            // accept() a new client connection, return a new socket file descriptor for that client
            if ((client_fd = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
                if (errno != EINTR) {
                    perror("accept");
                }
                continue;
            }
            
            printf("New client connected\n");
            
            // Read message from client. memset() fills the buffer with 0s up to size of buffer
            memset(buffer, 0, BUFFER_SIZE);
            int bytes_read = read(client_fd, buffer, BUFFER_SIZE);
            if (bytes_read > 0) {
                printf("Message from client: %s\n", buffer);
                
                // Send response to client
                char *message = "Hello from server";
                send(client_fd, message, strlen(message), 0);
                printf("Response sent to client\n");
            }
            
            // Close the client socket
            close(client_fd);
            client_fd = -1;
            printf("Client connection closed\n");
        }
    }
    
    // Clean up resources
    cleanup();
    
    printf("Server terminated.\n");
    return 0;
}
