#!/usr/bin/env python3
"""
CSend Python Client Library
Provides high-level async interface for CSend machine mode
"""

import json
import asyncio
from typing import Dict, List, Optional, Callable
from collections import defaultdict

class CSendClient:
    """Async client for CSend machine mode with JSON protocol"""
    
    def __init__(self, username: str = "bot", executable: str = "./build/posix/csend_posix"):
        self.username = username
        self.executable = executable
        self.process = None
        self.reader = None
        self.writer = None
        self._message_handlers = []
        self._peer_update_handlers = []
        self._event_handlers = defaultdict(list)
        self._running = False
        self._response_futures = {}
        self._next_id = 1
    
    async def connect(self):
        """Connect to CSend in machine mode"""
        print(f"Connecting to CSend as '{self.username}'...")
        
        # Start CSend process
        self.process = await asyncio.create_subprocess_exec(
            self.executable, "--machine-mode", self.username,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )
        
        self.reader = self.process.stdout
        self.writer = self.process.stdin
        
        # Start background task to read output
        asyncio.create_task(self._read_output())
        
        # Wait for ready signal
        ready_future = asyncio.Future()
        self._event_handlers['ready'].append(lambda d: ready_future.set_result(True))
        
        try:
            await asyncio.wait_for(ready_future, timeout=5.0)
            self._running = True
            print("Connected to CSend!")
        except asyncio.TimeoutError:
            raise Exception("CSend did not become ready in time")
    
    async def disconnect(self):
        """Disconnect from CSend"""
        if self._running:
            await self._send_command("/quit")
            self._running = False
            
            if self.process:
                try:
                    await asyncio.wait_for(self.process.wait(), timeout=2.0)
                except asyncio.TimeoutError:
                    self.process.terminate()
                    await self.process.wait()
    
    async def list_peers(self) -> List[Dict]:
        """Get list of active peers"""
        response = await self._send_command_async("/list")
        if response and response.get("command") == "/list":
            return response["data"]["peers"]
        return []
    
    async def send_message(self, peer_id: int, message: str) -> bool:
        """Send message to specific peer"""
        # Escape message for command line
        escaped_msg = message.replace('"', '\\"').replace('\n', '\\n')
        response = await self._send_command_async(f'/send {peer_id} "{escaped_msg}"')
        return response and response.get("data", {}).get("success", False)
    
    async def broadcast_message(self, message: str) -> int:
        """Broadcast message to all peers"""
        escaped_msg = message.replace('"', '\\"').replace('\n', '\\n')
        response = await self._send_command_async(f'/broadcast "{escaped_msg}"')
        if response and response.get("command") == "/broadcast":
            return response["data"]["sent_count"]
        return 0
    
    async def get_status(self) -> Dict:
        """Get application status"""
        response = await self._send_command_async("/status")
        if response and response.get("command") == "/status":
            return response["data"]
        return {}
    
    async def get_stats(self) -> Dict:
        """Get statistics"""
        response = await self._send_command_async("/stats")
        if response and response.get("command") == "/stats":
            return response["data"]
        return {}
    
    async def get_history(self, count: int = 10) -> List[Dict]:
        """Get message history"""
        response = await self._send_command_async(f"/history {count}")
        if response and response.get("command") == "/history":
            return response["data"]["messages"]
        return []
    
    async def get_version(self) -> Dict:
        """Get version information"""
        response = await self._send_command_async("/version")
        if response and response.get("command") == "/version":
            return response["data"]
        return {}
    
    def on_message(self, handler: Callable):
        """Register message handler"""
        self._message_handlers.append(handler)
    
    def on_peer_update(self, handler: Callable):
        """Register peer update handler"""
        self._peer_update_handlers.append(handler)
    
    def on_event(self, event_type: str, handler: Callable):
        """Register generic event handler"""
        self._event_handlers[event_type].append(handler)
    
    async def run_forever(self):
        """Keep running until disconnected"""
        while self._running:
            await asyncio.sleep(1)
    
    async def _read_output(self):
        """Background task to read CSend output"""
        while self._running:
            try:
                line = await self.reader.readline()
                if not line:
                    break
                
                data = json.loads(line.decode().strip())
                await self._handle_message(data)
                
            except json.JSONDecodeError as e:
                print(f"JSON decode error: {e}")
            except Exception as e:
                print(f"Error reading output: {e}")
                break
        
        self._running = False
    
    async def _handle_message(self, data: Dict):
        """Handle incoming JSON message"""
        try:
            msg_type = data.get("type")
            
            if msg_type == "start":
                # Startup message
                pass
            
            elif msg_type == "ready":
                # Ready signal
                for handler in self._event_handlers.get('ready', []):
                    await self._call_handler(handler, data)
            
            elif msg_type == "response" or msg_type == "error":
                # Command response
                correlation_id = data.get("id")
                if correlation_id and correlation_id in self._response_futures:
                    self._response_futures[correlation_id].set_result(data)
            
            elif msg_type == "event":
                event_type = data.get("event")
                
                if event_type == "message":
                    # Incoming message
                    message_data = data.get("data", {})
                    if message_data:
                        for handler in self._message_handlers:
                            await self._call_handler(handler, message_data)
                
                elif event_type == "peer_update":
                    # Peer update
                    for handler in self._peer_update_handlers:
                        await self._call_handler(handler, data)
                
                # Generic event handlers
                for handler in self._event_handlers.get(event_type, []):
                    await self._call_handler(handler, data)
        
        except Exception as e:
            print(f"Error handling message: {e}")
    
    async def _call_handler(self, handler: Callable, data: Dict):
        """Call handler safely"""
        try:
            if asyncio.iscoroutinefunction(handler):
                await handler(data)
            else:
                handler(data)
        except Exception as e:
            print(f"Error in handler: {e}")
    
    async def _send_command(self, command: str):
        """Send command to CSend"""
        self.writer.write(f"{command}\n".encode())
        await self.writer.drain()
    
    async def _send_command_async(self, command: str, timeout: float = 2.0) -> Optional[Dict]:
        """Send command and wait for response"""
        # Generate correlation ID
        correlation_id = f"cmd_{self._next_id}"
        self._next_id += 1
        
        # Add ID to command if not present
        if "--id=" not in command:
            command += f" --id={correlation_id}"
        
        # Create future for response
        response_future = asyncio.Future()
        self._response_futures[correlation_id] = response_future
        
        # Send command
        await self._send_command(command)
        
        try:
            # Wait for response
            response = await asyncio.wait_for(response_future, timeout=timeout)
            return response
        except asyncio.TimeoutError:
            print(f"Timeout waiting for response to: {command}")
            return None
        finally:
            # Clean up
            self._response_futures.pop(correlation_id, None)

# Example usage
async def example():
    """Example of using CSendClient"""
    client = CSendClient(username="example_bot")
    
    try:
        # Connect
        await client.connect()
        
        # List peers
        peers = await client.list_peers()
        print(f"Found {len(peers)} peers")
        
        # Send a message
        if peers:
            success = await client.send_message(peers[0]['id'], "Hello from Python!")
            print(f"Message sent: {success}")
        
        # Get status
        status = await client.get_status()
        print(f"Status: {status}")
        
        # Set up message handler
        async def on_message(message):
            print(f"Received: {message['from']['username']}: {message['content']}")
        
        client.on_message(on_message)
        
        # Run for a while
        await asyncio.sleep(10)
        
    finally:
        # Disconnect
        await client.disconnect()

if __name__ == "__main__":
    asyncio.run(example())