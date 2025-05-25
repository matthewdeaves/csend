# CSend Machine Mode Protocol

## Implementation Note

Machine mode has been refactored using the Strategy Pattern for better code organization:
- `ui_interface.h` - Defines the UI operations interface
- `ui_terminal_interactive.c` - Interactive terminal UI implementation
- `ui_terminal_machine.c` - Machine mode UI implementation with thread-safe I/O
- `ui_factory.c` - Factory for creating the appropriate UI

This design makes it easy to add new UI modes and keeps the core application logic separate from presentation concerns.

## Recent Improvements

Machine mode has been enhanced with:
1. **Signal handling** - SIGPIPE is ignored to prevent crashes on broken pipes
2. **Thread-safe output** - All output uses mutex-protected functions
3. **Line buffering** - Both stdin and stdout use line buffering for reliability
4. **Error recovery** - Input errors are handled gracefully with retry logic
5. **Proper shutdown** - Clean termination on EOF or quit command

## Overview

Machine mode allows programmatic interaction with the CSend POSIX application through structured input/output. This enables automated testing and integration with other tools (like Claude).

## Usage

Launch the application with the `--machine-mode` flag:

```bash
./build/posix/csend_posix --machine-mode [username]
```

## Protocol Format

### Startup Sequence

1. Application outputs: `START:username=<username>`
2. Application outputs: `READY` (indicates ready for commands)

### Commands

All commands are the same as interactive mode but without prompts:

- `/list` - List peers
- `/send <peer_num> <message>` - Send message to peer
- `/broadcast <message>` - Send to all peers
- `/quit` - Exit application
- `/help` - Show help
- `/debug` - Toggle debug output

### Output Format

#### Command Echo
Each command is echoed back:
```
CMD:<command>
```

#### Command Completion
After each command completes:
```
CMD_COMPLETE
```

#### Peer List
```
PEER_LIST_START
PEER:<number>:<username>:<ip>:<last_seen_seconds>
...
PEER_LIST_END:count=<total>
```

#### Send Result
Success:
```
SEND_RESULT:success:peer=<num>:ip=<ip>
```

Error:
```
SEND_RESULT:error:invalid_peer
SEND_RESULT:error:failed_to_send
```

#### Broadcast Result
```
BROADCAST_RESULT:sent_count=<num>
```

#### Incoming Message
```
MESSAGE:from=<username>:ip=<ip>:content=<message>
```

#### Peer Updates
When peer list changes:
```
PEER_UPDATE
```

## Example Session

```
START:username=claude
READY
/list
CMD:/list
PEER_LIST_START
PEER:1:matt:192.168.1.100:2
PEER_LIST_END:count=1
CMD_COMPLETE
/send 1 Hello from Claude!
CMD:/send 1 Hello from Claude!
SEND_RESULT:success:peer=1:ip=192.168.1.100
CMD_COMPLETE
MESSAGE:from=matt:ip=192.168.1.100:content=Hi Claude!
/quit
CMD:/quit
```

## Integration with Claude Code

### Quick Start for Claude Code

To use CSend in machine mode with Claude Code:

```bash
# Start CSend in machine mode
./build/posix/csend_posix --machine-mode claude > csend_output.txt 2>&1 < csend_input.txt &
echo $! > csend_pid.txt

# Wait for it to start
sleep 2

# Send commands via the input file
echo "/list" >> csend_input.txt

# Read output
tail -f csend_output.txt
```

### Python Integration Example

When using this with Claude Code, you can use the Bash tool to interact:

```python
# Start the application
process = subprocess.Popen(['./build/posix/csend_posix', '--machine-mode', 'claude'], 
                          stdin=subprocess.PIPE, 
                          stdout=subprocess.PIPE, 
                          stderr=subprocess.PIPE,
                          text=True,
                          bufsize=0)  # Unbuffered for real-time communication

# Wait for READY
while True:
    line = process.stdout.readline()
    if line.strip() == 'READY':
        break

# Send command
process.stdin.write('/list\n')
process.stdin.flush()

# Read response
while True:
    line = process.stdout.readline()
    print(line.strip())
    if line.strip() == 'CMD_COMPLETE':
        break
```

### Best Practices

1. **Always wait for READY** - Don't send commands until you receive the READY signal
2. **Handle PEER_UPDATE** - Peer list changes trigger PEER_UPDATE notifications
3. **Check for CMD_COMPLETE** - Every command ends with CMD_COMPLETE
4. **Use unbuffered I/O** - Set bufsize=0 for real-time communication
5. **Handle shutdown gracefully** - Send /quit command before terminating the process