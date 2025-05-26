#!/usr/bin/env python3
"""
CSend Chatbot using Claude Haiku
A chatbot that integrates with CSend's machine mode to provide AI-powered chat
"""

import os
import sys
import json
import asyncio
import time
from typing import Dict, List, Optional, Callable
from collections import defaultdict
from datetime import datetime
import anthropic

from csend_client import CSendClient

class CSendChatbot:
    def __init__(self, api_key: str, username: str = "Claude", model: str = "claude-3-haiku-20240307"):
        self.api_key = api_key
        self.username = username
        self.model = model
        self.client = None
        self.anthropic_client = anthropic.Anthropic(api_key=api_key)
        
        # Configuration
        self.config = {
            "greeting": "Hello! I'm Claude, an AI assistant. How can I help you today?",
            "max_context": 20,
            "response_timeout": 30,
            "commands": {
                "!help": "Show available commands",
                "!peers": "List active peers",
                "!stats": "Show chat statistics",
                "!clear": "Clear conversation history",
                "!about": "About this bot"
            },
            "system_prompt": """You are Claude, a helpful AI assistant in a peer-to-peer chat network. 
You should be friendly, helpful, and concise. When users ask questions, provide clear and useful answers.
You can see when new peers join or leave the network and should greet them appropriately."""
        }
        
        # Conversation memory per user
        self.conversations = defaultdict(list)
        
        # Statistics
        self.stats = {
            "messages_received": 0,
            "messages_sent": 0,
            "users_helped": set(),
            "start_time": time.time()
        }
        
        # Rate limiting
        self.rate_limiter = defaultdict(list)
        self.rate_limit = 20  # messages per minute per user
    
    async def start(self):
        """Start the chatbot"""
        print(f"Starting CSend Chatbot as '{self.username}'...")
        
        # Initialize CSend client
        self.client = CSendClient(username=self.username)
        await self.client.connect()
        
        # Set up event handlers
        self.client.on_message(self.handle_message)
        self.client.on_peer_update(self.handle_peer_update)
        
        print("Chatbot is ready!")
        
        # Send initial greeting to any connected peers
        peers = await self.client.list_peers()
        if peers:
            await self.client.broadcast_message(self.config["greeting"])
    
    async def handle_message(self, message: Dict):
        """Handle incoming messages"""
        try:
            self.stats["messages_received"] += 1
            
            from_data = message.get("from", {})
            from_user = from_data.get("username", "unknown")
            from_id = from_data.get("id")
            content = message.get("content", "")
            
            # If no ID, try to find peer by username
            if from_id is None:
                peers = await self.client.list_peers()
                for peer in peers:
                    if peer.get("username") == from_user:
                        from_id = peer.get("id")
                        break
            
            if from_id is None:
                print(f"Warning: Could not find peer ID for {from_user}")
                return
            
            self.stats["users_helped"].add(from_user)
            
            # Check rate limit
            if not self.check_rate_limit(from_user):
                await self.client.send_message(
                    from_id, 
                    "Sorry, you're sending messages too quickly. Please wait a moment."
                )
                return
            
            # Handle commands
            if content.startswith("!"):
                await self.handle_command(from_id, from_user, content)
                return
            
            # Generate AI response
            try:
                response = await self.generate_response(from_user, content)
                await self.client.send_message(from_id, response)
                self.stats["messages_sent"] += 1
            except Exception as e:
                print(f"Error generating response: {e}")
                await self.client.send_message(
                    from_id, 
                    "Sorry, I encountered an error processing your message. Please try again."
                )
        except Exception as e:
            print(f"Error in handle_message: {e}")
    
    async def handle_command(self, peer_id: int, username: str, command: str):
        """Handle special commands"""
        cmd = command.split()[0].lower()
        
        if cmd == "!help":
            help_text = "Available commands:\n"
            for cmd, desc in self.config["commands"].items():
                help_text += f"  {cmd} - {desc}\n"
            await self.client.send_message(peer_id, help_text)
        
        elif cmd == "!peers":
            peers = await self.client.list_peers()
            if peers:
                peer_list = "Active peers:\n"
                for peer in peers:
                    peer_list += f"  • {peer['username']} ({peer['ip']})\n"
            else:
                peer_list = "No other peers currently connected."
            await self.client.send_message(peer_id, peer_list)
        
        elif cmd == "!stats":
            uptime = int(time.time() - self.stats["start_time"])
            stats_text = f"""Bot Statistics:
• Uptime: {uptime//3600}h {(uptime%3600)//60}m
• Messages received: {self.stats['messages_received']}
• Messages sent: {self.stats['messages_sent']}
• Users helped: {len(self.stats['users_helped'])}"""
            await self.client.send_message(peer_id, stats_text)
        
        elif cmd == "!clear":
            self.conversations[username] = []
            await self.client.send_message(peer_id, "Conversation history cleared.")
        
        elif cmd == "!about":
            about_text = """I'm Claude, an AI assistant powered by Anthropic's Claude 3 Haiku model.
I'm here to help answer questions, have conversations, and assist with various tasks.
I maintain context of our conversation and can help multiple users simultaneously."""
            await self.client.send_message(peer_id, about_text)
        
        else:
            await self.client.send_message(
                peer_id, 
                f"Unknown command: {cmd}. Type !help for available commands."
            )
    
    async def handle_peer_update(self, event: Dict):
        """Handle peer join/leave events"""
        try:
            data = event.get("data", {})
            action = data.get("action", "unknown")
            peer = data.get("peer")
            
            if action == "joined" and peer:
                # Greet new peer
                greeting = f"Welcome {peer['username']}! {self.config['greeting']}"
                await asyncio.sleep(1)  # Small delay to ensure peer is ready
                await self.client.send_message(peer["id"], greeting)
        except Exception as e:
            print(f"Error handling peer update: {e}")
    
    async def generate_response(self, username: str, message: str) -> str:
        """Generate AI response using Claude Haiku"""
        # Add to conversation history
        self.conversations[username].append({"role": "user", "content": message})
        
        # Trim conversation history if too long
        if len(self.conversations[username]) > self.config["max_context"] * 2:
            self.conversations[username] = self.conversations[username][-self.config["max_context"]:]
        
        try:
            # Call Claude API with correct format
            response = self.anthropic_client.messages.create(
                model=self.model,
                max_tokens=500,
                temperature=0.7,
                system=self.config["system_prompt"],  # System prompt as separate parameter
                messages=self.conversations[username]  # Only user/assistant messages
            )
            
            response_text = response.content[0].text
            
            # Add assistant response to history
            self.conversations[username].append({"role": "assistant", "content": response_text})
            
            return response_text
            
        except Exception as e:
            print(f"Claude API error: {e}")
            # Fallback response
            return "I'm having trouble connecting to my AI service right now. Please try again in a moment."
    
    def check_rate_limit(self, username: str) -> bool:
        """Check if user is within rate limits"""
        now = time.time()
        minute_ago = now - 60
        
        # Remove old entries
        self.rate_limiter[username] = [
            t for t in self.rate_limiter[username] if t > minute_ago
        ]
        
        # Check limit
        if len(self.rate_limiter[username]) >= self.rate_limit:
            return False
        
        # Add current request
        self.rate_limiter[username].append(now)
        return True
    
    async def run(self):
        """Main run loop"""
        await self.start()
        await self.client.run_forever()

class CSendClient:
    """Async client for CSend machine mode"""
    
    def __init__(self, username: str = "bot", executable: str = "./build/posix/csend_posix"):
        self.username = username
        self.executable = executable
        self.process = None
        self.reader = None
        self.writer = None
        self._message_handlers = []
        self._peer_update_handlers = []
        self._running = False
    
    async def connect(self):
        """Connect to CSend in machine mode"""
        # Start CSend process
        self.process = await asyncio.create_subprocess_exec(
            self.executable, "--machine-mode", self.username,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE
        )
        
        self.reader = self.process.stdout
        self.writer = self.process.stdin
        
        # Wait for ready signal
        while True:
            line = await self.reader.readline()
            if not line:
                raise Exception("CSend process terminated unexpectedly")
            
            try:
                data = json.loads(line.decode().strip())
                if data.get("type") == "ready":
                    self._running = True
                    return
            except json.JSONDecodeError:
                continue
    
    async def list_peers(self) -> List[Dict]:
        """Get list of active peers"""
        await self._send_command("/list --id=list_peers")
        response = await self._wait_for_response("list_peers")
        if response and response.get("command") == "/list":
            return response["data"]["peers"]
        return []
    
    async def send_message(self, peer_id: int, message: str):
        """Send message to specific peer"""
        # Escape message for command line
        escaped_msg = message.replace('"', '\\"')
        await self._send_command(f'/send {peer_id} "{escaped_msg}"')
    
    async def broadcast_message(self, message: str):
        """Broadcast message to all peers"""
        escaped_msg = message.replace('"', '\\"')
        await self._send_command(f'/broadcast "{escaped_msg}"')
    
    def on_message(self, handler: Callable):
        """Register message handler"""
        self._message_handlers.append(handler)
    
    def on_peer_update(self, handler: Callable):
        """Register peer update handler"""
        self._peer_update_handlers.append(handler)
    
    async def run_forever(self):
        """Run event loop"""
        while self._running:
            try:
                line = await self.reader.readline()
                if not line:
                    break
                
                data = json.loads(line.decode().strip())
                
                if data.get("type") == "event":
                    if data.get("event") == "message":
                        for handler in self._message_handlers:
                            await handler(data["data"])
                    elif data.get("event") == "peer_update":
                        for handler in self._peer_update_handlers:
                            await handler(data)
                            
            except json.JSONDecodeError:
                continue
            except Exception as e:
                print(f"Error in event loop: {e}")
    
    async def _send_command(self, command: str):
        """Send command to CSend"""
        self.writer.write(f"{command}\n".encode())
        await self.writer.drain()
    
    async def _wait_for_response(self, correlation_id: str, timeout: float = 2.0) -> Optional[Dict]:
        """Wait for response with specific ID"""
        start_time = asyncio.get_event_loop().time()
        
        while asyncio.get_event_loop().time() - start_time < timeout:
            try:
                line = await asyncio.wait_for(self.reader.readline(), timeout=0.1)
                if not line:
                    continue
                
                data = json.loads(line.decode().strip())
                if data.get("id") == correlation_id and data.get("type") in ["response", "error"]:
                    return data
                    
            except asyncio.TimeoutError:
                continue
            except json.JSONDecodeError:
                continue
        
        return None

async def main():
    """Main entry point"""
    # Check for API key
    api_key = os.getenv("ANTHROPIC_API_KEY")
    if not api_key:
        print("Error: ANTHROPIC_API_KEY environment variable not set")
        print("Please set it with: export ANTHROPIC_API_KEY=your_key_here")
        sys.exit(1)
    
    # Get username from command line or use default
    username = sys.argv[1] if len(sys.argv) > 1 else "Claude"
    
    # Create and run chatbot
    bot = CSendChatbot(api_key=api_key, username=username)
    
    # Load config file if it exists
    config_file = "chatbot_config.json"
    if os.path.exists(config_file):
        with open(config_file, 'r') as f:
            config = json.load(f)
            bot.config.update(config)
    
    try:
        await bot.run()
    except KeyboardInterrupt:
        print("\nShutting down chatbot...")
        # Graceful shutdown
        if bot.client:
            await bot.client.disconnect()
    except Exception as e:
        print(f"Error: {e}")
        # Graceful shutdown on error
        if bot.client:
            await bot.client.disconnect()

if __name__ == "__main__":
    asyncio.run(main())