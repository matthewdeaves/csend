#!/bin/bash

# Create a function to open a terminal for a specific peer
open_peer_terminal() {
    local peer_num=$1
    local terminal_cmd
    
    # Detect the available terminal emulator
    if command -v gnome-terminal &> /dev/null; then
        gnome-terminal -- bash -c "docker attach peer$peer_num; exec bash"
    elif command -v xterm &> /dev/null; then
        xterm -e "docker attach peer$peer_num; exec bash" &
    elif command -v konsole &> /dev/null; then
        konsole -e "docker attach peer$peer_num; exec bash" &
    elif command -v terminal &> /dev/null; then  # macOS
        terminal -e "docker attach peer$peer_num; exec bash" &
    else
        echo "No supported terminal emulator found. Please install gnome-terminal, xterm, or konsole."
        exit 1
    fi
}

# Build and start all containers
start_all() {
    echo "Building and starting all peer containers..."
    docker-compose up --build -d
    
    # Give containers a moment to start
    sleep 2
    
    echo "Opening terminal windows for each peer..."
    for i in {1..3}; do
        open_peer_terminal $i
        sleep 1  # Small delay between opening terminals
    done
    
    echo "All peers are running. You can interact with each peer in its own terminal window."
    echo "Note: Use Ctrl+P, Ctrl+Q to detach from a terminal without stopping the container."
}

# Stop and remove all containers
stop_all() {
    echo "Stopping all peer containers..."
    docker-compose down
}

# Show status of all peers
status() {
    echo "Current status of peer containers:"
    docker-compose ps
}

# Main script logic
case "$1" in
    start)
        start_all
        ;;
    stop)
        stop_all
        ;;
    status)
        status
        ;;
    *)
        echo "Usage: $0 {start|stop|status}"
        echo ""
        echo "Commands:"
        echo "  start    # Start all peer containers and open terminal windows"
        echo "  stop     # Stop all peer containers"
        echo "  status   # Show status of all containers"
        echo ""
        echo "Note: When in a terminal, use Ctrl+P, Ctrl+Q to detach without stopping the container"
        ;;
esac