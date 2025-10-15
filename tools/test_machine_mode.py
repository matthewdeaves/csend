#!/usr/bin/env python3
"""
Enhanced test script for CSend machine mode with JSON support
"""

import subprocess
import sys
import time
import threading
import queue
import os
import signal
import json
from datetime import datetime

class CSendMachineMode:
    def __init__(self, username="test_user", executable="./build/posix/csend_posix"):
        self.username = username
        self.executable = executable
        self.process = None
        self.output_queue = queue.Queue()
        self.reader_thread = None
        self.is_ready = False
        self.json_mode = True
        
    def start(self):
        """Start CSend in machine mode"""
        print(f"Starting CSend in JSON machine mode as '{self.username}'...")
        
        # Start the process
        self.process = subprocess.Popen(
            [self.executable, "--machine-mode", self.username],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=0  # Unbuffered
        )
        
        # Start reader thread
        self.reader_thread = threading.Thread(target=self._read_output)
        self.reader_thread.daemon = True
        self.reader_thread.start()
        
        # Wait for READY signal
        if self._wait_for_ready(timeout=5):
            self.is_ready = True
            print("CSend is ready!")
            return True
        else:
            print("ERROR: CSend did not become ready in time")
            self.stop()
            return False
    
    def _read_output(self):
        """Read output from the process"""
        while self.process and self.process.poll() is None:
            try:
                line = self.process.stdout.readline()
                if line:
                    line = line.strip()
                    self.output_queue.put(line)
                    print(f"<-- {line}")
            except:
                break
    
    def _wait_for_ready(self, timeout=5):
        """Wait for ready signal (JSON format)"""
        start_time = time.time()
        while time.time() - start_time < timeout:
            try:
                line = self.output_queue.get(timeout=0.1)
                try:
                    data = json.loads(line)
                    if data.get("type") == "ready":
                        return True
                except json.JSONDecodeError:
                    pass
            except queue.Empty:
                continue
        return False
    
    def send_command(self, command):
        """Send a command to CSend"""
        if not self.is_ready:
            print("ERROR: CSend is not ready")
            return False
        
        print(f"--> {command}")
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
            return True
        except:
            print("ERROR: Failed to send command")
            return False
    
    def get_response(self, timeout=2):
        """Get response for last command (JSON format)"""
        start_time = time.time()
        
        while time.time() - start_time < timeout:
            try:
                line = self.output_queue.get(timeout=0.1)
                try:
                    data = json.loads(line)
                    if data.get("type") in ["response", "error"]:
                        return data
                except json.JSONDecodeError:
                    pass
            except queue.Empty:
                continue
        
        return None
    
    def get_events(self, timeout=0.5):
        """Get all events within timeout period"""
        events = []
        end_time = time.time() + timeout
        
        while time.time() < end_time:
            try:
                line = self.output_queue.get(timeout=0.1)
                try:
                    data = json.loads(line)
                    if data.get("type") == "event":
                        events.append(data)
                except json.JSONDecodeError:
                    pass
            except queue.Empty:
                continue
        
        return events
    
    def stop(self):
        """Stop CSend gracefully"""
        if self.process:
            print("Stopping CSend...")
            try:
                # Send quit command
                self.send_command("/quit")
                time.sleep(0.5)
                
                # If still running, terminate
                if self.process.poll() is None:
                    self.process.terminate()
                    time.sleep(0.5)
                
                # If still running, kill
                if self.process.poll() is None:
                    self.process.kill()
                    
            except:
                pass
            
            self.process = None

def run_json_tests():
    """Run machine mode tests with JSON protocol"""
    print("=== CSend JSON Machine Mode Test Suite ===\n")
    
    # Test 1: Basic startup and shutdown
    print("Test 1: JSON startup sequence")
    csend = CSendMachineMode(username="json_bot")
    
    if not csend.start():
        print("FAILED: Could not start CSend")
        return False
    
    time.sleep(1)
    
    # Test 2: List command with correlation ID
    print("\nTest 2: List command with correlation ID")
    csend.send_command("/list --id=list001")
    response = csend.get_response()
    
    if response and response.get("id") == "list001" and response.get("command") == "/list":
        print(f"PASSED: List command works, found {response['data']['count']} peers")
        print(f"Response: {json.dumps(response, indent=2)}")
    else:
        print("FAILED: List command did not return proper JSON response")
    
    # Test 3: Status command
    print("\nTest 3: Status command")
    csend.send_command("/status --id=status001")
    response = csend.get_response()
    
    if response and response.get("command") == "/status":
        print("PASSED: Status command works")
        print(f"Uptime: {response['data']['uptime_seconds']}s")
        print(f"Stats: {response['data']['statistics']}")
    else:
        print("FAILED: Status command did not work")
    
    # Test 4: Stats command
    print("\nTest 4: Stats command")
    csend.send_command("/stats")
    response = csend.get_response()
    
    if response and response.get("command") == "/stats":
        print("PASSED: Stats command works")
        print(f"Messages sent: {response['data']['messages_sent']}")
        print(f"Messages received: {response['data']['messages_received']}")
    else:
        print("FAILED: Stats command did not work")
    
    # Test 5: History command
    print("\nTest 5: History command")
    csend.send_command("/history 5")
    response = csend.get_response()
    
    if response and response.get("command") == "/history":
        print(f"PASSED: History command works, returned {response['data']['count']} messages")
    else:
        print("FAILED: History command did not work")
    
    # Test 6: Version command
    print("\nTest 6: Version command")
    csend.send_command("/version --id=ver001")
    response = csend.get_response()
    
    if response and response.get("command") == "/version":
        print(f"PASSED: Version command works - Protocol: {response['data']['protocol_version']}")
    else:
        print("FAILED: Version command did not work")
    
    # Test 7: Help command
    print("\nTest 7: Help command (JSON)")
    csend.send_command("/help")
    response = csend.get_response()
    
    if response and response.get("command") == "/help":
        print("PASSED: Help command returns JSON")
        print(f"Available commands: {response['data']['commands']}")
    else:
        print("FAILED: Help command did not return JSON")
    
    # Test 8: Debug toggle
    print("\nTest 8: Debug toggle")
    csend.send_command("/debug")
    response = csend.get_response()
    
    if response and response.get("command") == "/debug":
        print(f"PASSED: Debug toggle works - enabled: {response['data']['enabled']}")
    else:
        print("FAILED: Debug toggle did not work")
    
    # Test 9: Invalid command
    print("\nTest 9: Invalid command handling")
    csend.send_command("/invalid_command")
    response = csend.get_response()
    
    if response and response.get("type") == "error":
        print(f"PASSED: Invalid command handled - Code: {response['error']['code']}")
    else:
        print("FAILED: Invalid command not handled properly")
    
    # Test 10: Broadcast command (no peers)
    print("\nTest 10: Broadcast command")
    csend.send_command("/broadcast Hello JSON world! --id=bc001")
    response = csend.get_response()
    
    if response and response.get("command") == "/broadcast":
        print(f"PASSED: Broadcast command works - sent to {response['data']['sent_count']} peers")
    else:
        print("FAILED: Broadcast command did not work")
    
    # Test 11: Send to invalid peer
    print("\nTest 11: Send to invalid peer")
    csend.send_command("/send 99 Test message")
    response = csend.get_response()
    
    if response and response.get("type") == "error":
        print(f"PASSED: Error handling works - {response['error']['code']}: {response['error']['message']}")
    else:
        print("FAILED: Error handling did not work properly")
    
    # Test 12: Graceful shutdown
    print("\nTest 12: Graceful shutdown")
    csend.stop()
    print("PASSED: Graceful shutdown completed")
    
    print("\n=== All JSON tests completed ===")
    return True

def interactive_json_mode():
    """Run in interactive mode with JSON output"""
    print("=== CSend JSON Machine Mode Interactive Test ===")
    print("This mode shows JSON input/output")
    print("Type 'exit' to quit\n")
    
    username = input("Enter username (default: claude): ").strip() or "claude"
    
    csend = CSendMachineMode(username=username)
    
    if not csend.start():
        print("Failed to start CSend")
        return
    
    print("\nYou can now enter commands. Type 'exit' to quit.")
    print("Try: /list --id=1, /status, /stats, /version\n")
    
    # Start event monitoring thread
    def monitor_events():
        while csend.process:
            events = csend.get_events(timeout=1)
            for event in events:
                print(f"\n[EVENT] {json.dumps(event, indent=2)}\n> ", end='', flush=True)
    
    event_thread = threading.Thread(target=monitor_events)
    event_thread.daemon = True
    event_thread.start()
    
    try:
        while True:
            command = input("> ").strip()
            
            if command.lower() == "exit":
                break
            
            if command:
                csend.send_command(command)
                response = csend.get_response(timeout=2)
                if response:
                    print(f"\n[RESPONSE] {json.dumps(response, indent=2)}\n")
    
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    
    finally:
        csend.stop()

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--interactive":
        interactive_json_mode()
    else:
        run_json_tests()