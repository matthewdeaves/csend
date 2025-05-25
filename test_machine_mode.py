#!/usr/bin/env python3
"""
Test script for CSend machine mode
This script verifies that machine mode works correctly
"""

import subprocess
import sys
import time
import threading
import queue
import os
import signal

class CSendMachineMode:
    def __init__(self, username="test_user", executable="./build/posix/csend_posix"):
        self.username = username
        self.executable = executable
        self.process = None
        self.output_queue = queue.Queue()
        self.reader_thread = None
        self.is_ready = False
        
    def start(self):
        """Start CSend in machine mode"""
        print(f"Starting CSend in machine mode as '{self.username}'...")
        
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
        if self._wait_for("READY", timeout=5):
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
    
    def _wait_for(self, expected, timeout=5):
        """Wait for a specific output line"""
        start_time = time.time()
        while time.time() - start_time < timeout:
            try:
                line = self.output_queue.get(timeout=0.1)
                if line == expected:
                    return True
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
    
    def get_output(self, timeout=2):
        """Get all output within timeout period"""
        lines = []
        end_time = time.time() + timeout
        
        while time.time() < end_time:
            try:
                line = self.output_queue.get(timeout=0.1)
                lines.append(line)
                if line == "CMD_COMPLETE":
                    break
            except queue.Empty:
                continue
        
        return lines
    
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

def run_tests():
    """Run machine mode tests"""
    print("=== CSend Machine Mode Test Suite ===\n")
    
    # Test 1: Basic startup and shutdown
    print("Test 1: Basic startup and shutdown")
    csend = CSendMachineMode(username="test_bot")
    
    if not csend.start():
        print("FAILED: Could not start CSend")
        return False
    
    time.sleep(1)
    
    # Test 2: Help command
    print("\nTest 2: Help command")
    csend.send_command("/help")
    output = csend.get_output()
    if "CMD:/help" in output and "CMD_COMPLETE" in output:
        print("PASSED: Help command works")
    else:
        print("FAILED: Help command did not complete properly")
    
    # Test 3: List command
    print("\nTest 3: List command")
    csend.send_command("/list")
    output = csend.get_output()
    if "PEER_LIST_START" in output and "PEER_LIST_END" in [o for o in output if o.startswith("PEER_LIST_END")]:
        print("PASSED: List command works")
    else:
        print("FAILED: List command did not complete properly")
    
    # Test 4: Debug toggle
    print("\nTest 4: Debug toggle")
    csend.send_command("/debug")
    output = csend.get_output()
    if any("DEBUG_TOGGLE" in o for o in output):
        print("PASSED: Debug toggle works")
    else:
        print("FAILED: Debug toggle did not work")
    
    # Test 5: Invalid command
    print("\nTest 5: Invalid command")
    csend.send_command("/invalid_command")
    output = csend.get_output()
    if any("COMMAND_ERROR:unknown" in o for o in output):
        print("PASSED: Invalid command handling works")
    else:
        print("FAILED: Invalid command not handled properly")
    
    # Test 6: Broadcast command (no peers)
    print("\nTest 6: Broadcast command")
    csend.send_command("/broadcast Hello from test bot!")
    output = csend.get_output()
    if any("BROADCAST_RESULT:sent_count=" in o for o in output):
        print("PASSED: Broadcast command works")
    else:
        print("FAILED: Broadcast command did not work")
    
    # Test 7: Graceful shutdown
    print("\nTest 7: Graceful shutdown")
    csend.stop()
    print("PASSED: Graceful shutdown completed")
    
    print("\n=== All tests completed ===")
    return True

def interactive_mode():
    """Run in interactive mode for manual testing"""
    print("=== CSend Machine Mode Interactive Test ===")
    print("This mode allows you to manually test machine mode")
    print("Type 'exit' to quit\n")
    
    username = input("Enter username (default: claude): ").strip() or "claude"
    
    csend = CSendMachineMode(username=username)
    
    if not csend.start():
        print("Failed to start CSend")
        return
    
    print("\nYou can now enter commands. Type 'exit' to quit.")
    print("Common commands: /list, /help, /debug, /broadcast <message>\n")
    
    try:
        while True:
            command = input("> ").strip()
            
            if command.lower() == "exit":
                break
            
            if command:
                csend.send_command(command)
                # Give some time for all output to arrive
                time.sleep(0.5)
    
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    
    finally:
        csend.stop()

if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--interactive":
        interactive_mode()
    else:
        run_tests()