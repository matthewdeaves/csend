#!/bin/bash

# Check for required dependencies and install them if needed
check_dependencies() {
    echo "Checking dependencies..."
    
    # Check for Docker
    if ! command -v docker &> /dev/null; then
        echo "Docker not found. Would you like to install it? (y/n)"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            echo "Installing Docker..."
            sudo apt-get update
            sudo apt-get install -y docker.io
            sudo systemctl enable docker
            sudo systemctl start docker
            sudo usermod -aG docker "$USER"
            echo "Docker installed. Please log out and log back in for group changes to take effect."
            echo "Then run this script again."
            exit 0
        else
            echo "Docker is required to run this script."
            exit 1
        fi
    fi
    
    # Check for Docker Compose
    if ! command -v docker-compose &> /dev/null; then
        echo "Docker Compose not found. Would you like to install it? (y/n)"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            echo "Installing Docker Compose..."
            sudo apt-get update
            sudo apt-get install -y docker-compose
        else
            echo "Docker Compose is required to run this script."
            exit 1
        fi
    fi
    
    # Check for a terminal emulator
    if ! command -v gnome-terminal &> /dev/null && ! command -v xterm &> /dev/null && ! command -v konsole &> /dev/null; then
        echo "No supported terminal emulator found. Would you like to install gnome-terminal? (y/n)"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            echo "Installing gnome-terminal..."
            sudo apt-get update
            sudo apt-get install -y gnome-terminal
        else
            echo "A terminal emulator (gnome-terminal, xterm, or konsole) is required."
            exit 1
        fi
    fi
}

# Check Docker context and offer to switch if needed
check_docker_context() {
    # Get current context
    current_context=$(docker context ls --format '{{if .Current}}{{.Name}}{{end}}')
    
    # Check if Docker daemon is accessible
    if ! docker info &> /dev/null; then
        echo "Cannot connect to Docker daemon with current context: $current_context"
        echo "Would you like to switch to the default Docker context? (y/n)"
        read -r answer
        if [[ "$answer" =~ ^[Yy]$ ]]; then
            # Check if default context exists
            if docker context ls | grep -q "default"; then
                docker context use default
                echo "Switched to default Docker context."
            else
                echo "Creating and switching to default Docker context..."
                docker context create default --docker "host=unix:///var/run/docker.sock"
                docker context use default
            fi
            
            # Check if Docker daemon is now accessible
            if ! docker info &> /dev/null; then
                echo "Still cannot connect to Docker daemon. Is Docker running?"
                echo "Try: sudo systemctl start docker"
                exit 1
            fi
        else
            echo "Docker daemon is not accessible with the current context."
            exit 1
        fi
    fi
}

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
    check_dependencies
    check_docker_context
    
    echo "Building and starting all peer containers..."
    docker-compose up --build -d
    
    # Check if containers started successfully
    if [ $? -ne 0 ]; then
        echo "Failed to start containers. Check the error messages above."
        exit 1
    fi
    
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
    check_docker_context
    
    echo "Stopping all peer containers..."
    docker-compose down
}

# Show status of all peers
status() {
    check_docker_context
    
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