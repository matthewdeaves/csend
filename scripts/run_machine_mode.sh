#!/bin/bash
# Enhanced launcher for CSend machine mode

# Default values
USERNAME="${1:-claude}"
MODE="json"
LOG_LEVEL="info"
VENV_DIR="csend_venv"

# Function to setup virtual environment
setup_venv() {
    if [ ! -d "$VENV_DIR" ]; then
        echo "Setting up Python virtual environment..."
        python3 -m venv "$VENV_DIR"
        if [ $? -ne 0 ]; then
            echo "Error: Failed to create virtual environment"
            echo "Make sure python3-venv is installed: sudo apt install python3-venv"
            exit 1
        fi
        echo "Virtual environment created successfully!"
    fi
    
    # Always activate the virtual environment
    echo "Activating virtual environment..."
    source "$VENV_DIR/bin/activate"
    
    # Check if anthropic is installed
    if ! python3 -c "import anthropic" 2>/dev/null; then
        echo "Installing required packages..."
        pip install anthropic
        if [ $? -ne 0 ]; then
            echo "Error: Failed to install anthropic package"
            exit 1
        fi
        echo "Package installation complete!"
    fi
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --help|-h)
            echo "Usage: $0 [username] [options]"
            echo ""
            echo "Options:"
            echo "  --help, -h          Show this help message"
            echo "  --test              Run automated tests"
            echo "  --interactive       Run interactive test mode"
            echo "  --chatbot           Run Claude Haiku chatbot (requires ANTHROPIC_API_KEY)"
            echo "  --debug             Enable debug logging"
            echo "  --log-level LEVEL   Set log level (debug, info, warn, error)"
            echo ""
            echo "Environment variables:"
            echo "  ANTHROPIC_API_KEY   API key for Claude Haiku chatbot mode"
            echo "  CSEND_LOG_LEVEL     Log level (overridden by --log-level)"
            echo ""
            echo "Examples:"
            echo "  $0                  # Run as 'claude' in JSON mode"
            echo "  $0 alice            # Run as 'alice' in JSON mode"
            echo "  $0 --test           # Run automated tests"
            echo "  $0 --chatbot        # Run Claude chatbot"
            echo "  $0 bot1 --debug     # Run as 'bot1' with debug logging"
            exit 0
            ;;
        --test)
            echo "Running CSend machine mode tests..."
            setup_venv
            python3 ../tools/test_machine_mode.py
            exit $?
            ;;
        --interactive)
            echo "Running CSend in interactive test mode..."
            setup_venv
            python3 test_machine_mode.py --interactive
            exit $?
            ;;
        --chatbot)
            # Source bashrc to get environment variables
            if [ -f ~/.bashrc ]; then
                source ~/.bashrc
            fi
            
            if [ -z "$ANTHROPIC_API_KEY" ]; then
                echo "Error: ANTHROPIC_API_KEY environment variable not set"
                echo "Please add it to your ~/.bashrc: export ANTHROPIC_API_KEY=your_key_here"
                exit 1
            fi
            # Use default username if not specified
            if [ "$USERNAME" = "--chatbot" ]; then
                USERNAME="Claude"
            fi
            echo "Starting Claude Haiku chatbot..."
            setup_venv
            python3 ../tools/csend_chatbot.py "$USERNAME"
            exit $?
            ;;
        --debug)
            LOG_LEVEL="debug"
            shift
            ;;
        --log-level)
            LOG_LEVEL="$2"
            shift 2
            ;;
        *)
            USERNAME="$1"
            shift
            ;;
    esac
done

# Use environment variable if set
LOG_LEVEL="${CSEND_LOG_LEVEL:-$LOG_LEVEL}"

# Build the application if needed
if [ ! -f "./build/posix/csend_posix" ]; then
    echo "Building CSend..."
    make clean && make
    if [ $? -ne 0 ]; then
        echo "Build failed!"
        exit 1
    fi
fi

# Check if another instance is running on the same port
if lsof -i :8080 >/dev/null 2>&1; then
    echo "Warning: Port 8080 is already in use. Another CSend instance may be running."
    echo "You can kill it with: pkill -f csend_posix"
    read -p "Continue anyway? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# Run CSend in machine mode
echo "Starting CSend in JSON machine mode as '$USERNAME'"
echo "Log level: $LOG_LEVEL"
echo "Commands: /list, /send <id> <msg>, /broadcast <msg>, /status, /stats, /history, /version, /quit"
echo "Use --id=<correlation_id> to track request/response pairs"
echo "----------------------------------------"

# Export log level and run
export CSEND_LOG_LEVEL="$LOG_LEVEL"
exec ./build/posix/csend_posix --machine-mode "$USERNAME"