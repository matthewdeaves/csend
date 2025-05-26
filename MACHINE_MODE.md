# CSend Machine Mode Protocol

## Overview

Machine mode provides JSON-based programmatic interaction with the CSend POSIX application, enabling seamless integration with LLMs and automation tools.

## Architecture

Machine mode uses the Strategy Pattern:
- `ui_interface.h` - UI operations interface
- `ui_terminal_machine.c` - Machine mode implementation with JSON output
- `ui_factory.c` - Factory for creating UI instances

Key features:
- Thread-safe JSON output
- Structured error handling
- Real-time event streaming
- Correlation ID support for request/response tracking

## Usage

```bash
# Basic usage
./build/posix/csend_posix --machine-mode [username]

# With environment variables
CSEND_USERNAME=bot1 ./build/posix/csend_posix --machine-mode

# Enable debug logging
CSEND_LOG_LEVEL=debug ./build/posix/csend_posix --machine-mode claude
```

## Protocol Format

### Startup Sequence

```json
{"type": "start", "version": "2.0", "username": "claude", "timestamp": "2024-01-15T10:30:00Z"}
{"type": "ready", "timestamp": "2024-01-15T10:30:01Z"}
```

### Response Structure

All responses follow this structure:
```json
{
  "type": "response|event|error",
  "id": "correlation_id",        // Optional, echoes request ID
  "timestamp": "ISO8601",
  "data": { ... }                 // Type-specific data
}
```

## Commands

### Core Commands
- `/list [--id=<id>]` - List active peers
- `/send <peer_id> <message> [--id=<id>]` - Send to specific peer
- `/broadcast <message> [--id=<id>]` - Send to all peers
- `/quit` - Exit application
- `/help` - Show commands (returns JSON)
- `/debug` - Toggle debug mode

### Extended Commands
- `/status [--id=<id>]` - Get application status
- `/stats [--id=<id>]` - Get detailed statistics
- `/history [count] [--id=<id>]` - Get message history
- `/peers --filter <pattern> [--id=<id>]` - Search peers
- `/version [--id=<id>]` - Get version info

## Response Examples

### List Command
```json
{
  "type": "response",
  "id": "list001",
  "timestamp": "2024-01-15T10:30:02Z",
  "command": "/list",
  "data": {
    "peers": [
      {
        "id": 1,
        "username": "alice",
        "ip": "192.168.1.100",
        "last_seen": "2024-01-15T10:29:57Z",
        "status": "active"
      }
    ],
    "count": 1
  }
}
```

### Send Command
```json
{
  "type": "response",
  "id": "send001",
  "timestamp": "2024-01-15T10:30:03Z",
  "command": "/send",
  "data": {
    "success": true,
    "peer": {
      "id": 1,
      "username": "alice",
      "ip": "192.168.1.100"
    },
    "message_id": "msg_789"
  }
}
```

### Status Command
```json
{
  "type": "response",
  "id": "status001",
  "timestamp": "2024-01-15T10:30:00Z",
  "command": "/status",
  "data": {
    "uptime_seconds": 3600,
    "version": "2.0",
    "username": "claude",
    "network": {
      "tcp_port": 2555,
      "udp_port": 2556
    },
    "statistics": {
      "messages_sent": 42,
      "messages_received": 38,
      "broadcasts_sent": 5,
      "active_peers": 3
    }
  }
}
```

## Event Examples

### Message Event
```json
{
  "type": "event",
  "event": "message",
  "timestamp": "2024-01-15T10:30:00Z",
  "data": {
    "from": {
      "id": 1,
      "username": "alice",
      "ip": "192.168.1.100"
    },
    "content": "Hello, Claude!",
    "message_id": "msg_123"
  }
}
```

### Peer Update Event
```json
{
  "type": "event",
  "event": "peer_update",
  "timestamp": "2024-01-15T10:30:00Z",
  "data": {
    "action": "joined",
    "peer": {
      "id": 2,
      "username": "bob",
      "ip": "192.168.1.101"
    }
  }
}
```

## Error Handling

### Error Response
```json
{
  "type": "error",
  "id": "cmd123",
  "timestamp": "2024-01-15T10:30:00Z",
  "error": {
    "code": "PEER_NOT_FOUND",
    "message": "Peer with ID 5 not found",
    "details": {
      "peer_id": 5,
      "available_peers": [1, 2, 3]
    }
  }
}
```

### Error Codes
- `UNKNOWN_COMMAND` - Command not recognized
- `INVALID_SYNTAX` - Command syntax error
- `PEER_NOT_FOUND` - Specified peer doesn't exist
- `NETWORK_ERROR` - Network operation failed
- `INTERNAL_ERROR` - Unexpected error

## Python Client Library

### Basic Usage
```python
from csend_client import CSendClient
import asyncio

async def main():
    client = CSendClient(username="my_bot")
    await client.connect()
    
    # List peers
    peers = await client.list_peers()
    
    # Send message
    if peers:
        await client.send_message(peers[0]['id'], "Hello!")
    
    # Handle incoming messages
    async def on_message(msg):
        print(f"{msg['from']['username']}: {msg['content']}")
    
    client.on_message(on_message)
    await client.run_forever()

asyncio.run(main())
```

## Claude Haiku Chatbot

### Prerequisites

The launcher script automatically handles Python package installation using a virtual environment. No manual setup required!

If you need to install packages manually:

1. **Automatic Setup (Recommended)**:
   ```bash
   # The launcher script handles everything
   ./run_machine_mode.sh --chatbot
   ```
   This will:
   - Create a virtual environment (`csend_venv`)
   - Install the anthropic package automatically
   - Activate the environment when running

2. **Manual Setup**:
   ```bash
   # Create and activate virtual environment
   python3 -m venv csend_venv
   source csend_venv/bin/activate
   
   # Install required packages
   pip install anthropic
   ```

3. **System-wide install (not recommended)**:
   ```bash
   # For older Python or non-Debian systems
   pip install anthropic
   
   # For Ubuntu/Debian with Python 3.12+
   pip3 install --break-system-packages anthropic
   ```

### Setting up the API Key

Before running the chatbot, you need to obtain and set your Anthropic API key:

1. **Get an API key** from [Anthropic Console](https://console.anthropic.com/)
2. **Set the environment variable**:
   ```bash
   export ANTHROPIC_API_KEY="your-api-key-here"
   ```
   
   Or add it to your shell profile (e.g., `~/.bashrc`, `~/.zshrc`):
   ```bash
   echo 'export ANTHROPIC_API_KEY="your-api-key-here"' >> ~/.bashrc
   source ~/.bashrc
   ```
   
   **Note**: The launcher script automatically sources `~/.bashrc`, so if you've added the API key there, it will be picked up automatically.

3. **Verify it's set**:
   ```bash
   echo $ANTHROPIC_API_KEY
   ```

### Quick Start

With the API key set, you can run the chatbot:

```bash
# Using the launcher script (recommended)
./run_machine_mode.sh --chatbot

# Or run directly
python3 csend_chatbot.py Claude
```

### Python Example
```python
from csend_chatbot import CSendChatbot
import os

# The API key is read from environment
api_key = os.getenv("ANTHROPIC_API_KEY")
if not api_key:
    print("Error: ANTHROPIC_API_KEY not set")
    exit(1)

bot = CSendChatbot(
    api_key=api_key,
    username="Claude",
    model="claude-3-haiku-20240307"
)

# Configure behavior
bot.config.update({
    "greeting": "Hi! I'm Claude, an AI assistant.",
    "max_context": 20,
    "commands": {
        "!help": "Show commands",
        "!peers": "List active peers"
    }
})

bot.run()
```

### Configuration
Create `chatbot_config.json`:
```json
{
  "username": "Claude",
  "model": "claude-3-haiku-20240307",
  "system_prompt": "You are a helpful AI assistant in a P2P chat.",
  "features": {
    "context_memory": true,
    "command_parsing": true,
    "auto_greet": true
  },
  "rate_limits": {
    "messages_per_minute": 20
  }
}
```

## Example Session

```json
{"type": "start", "version": "2.0", "username": "claude", "timestamp": "2024-01-15T10:30:00Z"}
{"type": "ready", "timestamp": "2024-01-15T10:30:01Z"}

> /list --id=1
{"type": "response", "id": "1", "timestamp": "2024-01-15T10:30:02Z", "command": "/list", "data": {"peers": [{"id": 1, "username": "alice", "ip": "192.168.1.100", "last_seen": "2024-01-15T10:29:57Z", "status": "active"}], "count": 1}}

> /send 1 "Hello!" --id=2
{"type": "response", "id": "2", "timestamp": "2024-01-15T10:30:03Z", "command": "/send", "data": {"success": true, "peer": {"id": 1, "username": "alice", "ip": "192.168.1.100"}, "message_id": "msg_001"}}

{"type": "event", "event": "message", "timestamp": "2024-01-15T10:30:04Z", "data": {"from": {"id": 1, "username": "alice", "ip": "192.168.1.100"}, "content": "Hi Claude!", "message_id": "msg_002"}}
```

## Best Practices

1. **Use correlation IDs** - Track async requests with `--id`
2. **Handle events separately** - Events can arrive anytime
3. **Implement reconnection** - Handle network failures gracefully
4. **Rate limit messages** - Prevent flooding the network
5. **Validate JSON** - Always parse responses safely
6. **Log errors** - Keep audit trail for debugging

## Troubleshooting

### Common Issues

**No output received**
- Check process is running: `ps aux | grep csend`
- Verify ports 8080-8081 are not blocked
- Enable debug mode: `CSEND_LOG_LEVEL=debug`

**JSON parsing errors**
- Ensure proper escaping in messages
- Check for UTF-8 encoding issues
- Validate against response schema

**Connection failures**
- Verify network interface is up
- Check firewall rules
- Ensure UDP broadcast is enabled

**Performance issues**
- Monitor system resources
- Check network latency
- Consider batching operations

### Claude Haiku Chatbot Issues

**API Key Not Found**
```bash
Error: ANTHROPIC_API_KEY environment variable not set
```
Solution:
- Ensure you've exported the API key: `export ANTHROPIC_API_KEY="sk-ant-..."`
- Check it's set: `echo $ANTHROPIC_API_KEY`
- If using sudo, preserve environment: `sudo -E python3 csend_chatbot.py`

**Invalid API Key**
```
Claude API error: 401 Unauthorized
```
Solution:
- Verify your API key is correct and active
- Check your Anthropic account has available credits
- Ensure the key has permissions for Claude Haiku model

**Rate Limiting**
```
Error: Rate limit exceeded
```
Solution:
- The bot has built-in rate limiting (20 messages/minute per user)
- For API rate limits, check your Anthropic plan limits
- Consider upgrading your plan if needed

**Model Not Available**
```
Error: Model claude-3-haiku-20240307 not found
```
Solution:
- Ensure your API key has access to Claude Haiku
- Check the model name is correct in your config
- Try using a different Claude model if needed

**Module Not Found**
```
ModuleNotFoundError: No module named 'anthropic'
```
Solution:
- Install the required package: `pip install anthropic` or `pip3 install anthropic`
- If using a virtual environment, ensure it's activated
- Check pip installation: `python3 -m pip install anthropic`