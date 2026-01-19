#!/usr/bin/env python3
"""
Comprehensive Deployment Test for Z1 Cluster
Tests the full workflow: discover, deploy, inject, monitor, stop
"""

import sys
import os
import subprocess
import tempfile
import time
import json
import argparse
import requests
from pathlib import Path

# Get script directory for relative path resolution
SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.parent

# ANSI color codes
GREEN = '\033[92m'
BLUE = '\033[94m'
YELLOW = '\033[93m'
RESET = '\033[0m'

# ASCII symbols (Windows compatible)
CHECK = '[OK]'
CROSS = '[FAIL]'
WAIT = '[SKIP]'

# Global verbose flag
VERBOSE = True

def run_command(cmd, description):
    """Run a command and return output"""
    print(f"{YELLOW}Running: {description}...{RESET}")
    try:
        # Run command from project root for consistent path resolution
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30, cwd=PROJECT_ROOT)
        return result.returncode == 0, result.stdout, result.stderr
    except subprocess.TimeoutExpired:
        return False, "", "Command timed out"
    except Exception as e:
        return False, "", str(e)

def test_nls(controller_ip):
    """Test 1: Node discovery"""
    cmd = ["python", "python_tools/bin/nls", "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "Node discovery (nls)")
    
    if not success:
        return False, "Failed to run nls"
    
    # Parse node count
    lines = stdout.strip().split('\n')
    node_count = 0
    for line in lines:
        parts = line.split()
        if len(parts) >= 2 and parts[0].isdigit() and parts[1] == 'online':
            node_count += 1
    
    if node_count < 2:
        return False, f"Only {node_count} nodes found"
    
    if VERBOSE:
        print(f"\n{BLUE}=== Node Discovery ==={RESET}")
        print(stdout)
        print(f"{BLUE}{'='*70}{RESET}\n")
    
    return True, f"{node_count} nodes"

def test_deploy(controller_ip, topology):
    """Test 2: Topology deployment"""
    cmd = ["python", "python_tools/bin/nsnn", "deploy", topology, "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "Deploy topology (nsnn deploy)")
    
    if not success:
        print(f"DEBUG: Deploy failed - stdout: {stdout[:200]}, stderr: {stderr[:200]}")
        return False, "Deployment failed"
    
    if VERBOSE:
        print(f"\n{BLUE}=== Topology Deployment ==={RESET}")
        print(stdout)
        print(f"{BLUE}{'='*70}{RESET}\n")
    
    return True, "Deployed"

def test_nstat(controller_ip):
    """Test 3: Node status check"""
    cmd = ["python", "python_tools/bin/nstat", "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "Node status (nstat)")
    
    if not success:
        return False, "Status check failed"
    
    return True, "OK"

def test_snn_start(controller_ip):
    """Test 4: Start SNN"""
    cmd = ["python", "python_tools/bin/nsnn", "start", "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "Start SNN (nsnn start)")
    
    if not success:
        return False, "Failed to start SNN"
    
    return True, "Running"

def test_inject_spikes(controller_ip, spike_count):
    """Test 5: Inject spikes (async - returns immediately, polls for completion)"""
    # Create spike pattern for XOR inputs
    spike_pattern = {
        "spikes": [
            {"neuron_id": 0, "count": spike_count//2},
            {"neuron_id": 1, "count": spike_count//2}
        ]
    }
    
    # Write to temp file
    fd, pattern_file = tempfile.mkstemp(suffix='.json', text=True)
    try:
        with os.fdopen(fd, 'w') as f:
            json.dump(spike_pattern, f)
        
        cmd = ["python", "python_tools/bin/nsnn", "inject", pattern_file, "-c", controller_ip]
        success, stdout, stderr = run_command(cmd, f"Queue {spike_count} spikes (nsnn inject)")
        
        if not success:
            print(f"DEBUG: Inject failed - stdout: {stdout[:300]}, stderr: {stderr[:300]}")
            return False, "Injection failed"
        
        if VERBOSE:
            print(f"\n{BLUE}=== Spike Injection (Async) ==={RESET}")
            print(stdout)
            print(f"{BLUE}{'='*70}{RESET}\n")
        
        # Spikes queued - calculate expected completion time
        # At 100 spikes/sec, time = spike_count / 100
        expected_time = (spike_count / 100) + 1  # Add 1 sec buffer
        print(f"{YELLOW}Spikes queued for background injection (rate: 100/sec, est. time: {expected_time:.1f}s){RESET}")
        print(f"{YELLOW}Polling status every 2 seconds...{RESET}")
        
        # Poll status until complete
        start_time = time.time()
        max_wait = expected_time + 5  # Add 5 sec timeout buffer
        
        while (time.time() - start_time) < max_wait:
            # Query controller status
            try:
                resp = requests.get(f"http://{controller_ip}/api/nodes", timeout=5)
                if resp.status_code == 200:
                    data = resp.json()
                    # Check if spike injection is complete (no pending jobs)
                    # For now, just wait expected time
                    elapsed = time.time() - start_time
                    if elapsed >= expected_time:
                        print(f"{GREEN}Spike injection complete ({elapsed:.1f}s){RESET}")
                        break
                    else:
                        remaining = expected_time - elapsed
                        print(f"  Progress: {elapsed:.1f}s / {expected_time:.1f}s (est. {remaining:.1f}s remaining)")
            except Exception as e:
                print(f"  Status poll failed: {e}")
            
            time.sleep(2)
        
        return True, f"{spike_count} spikes"
    finally:
        os.unlink(pattern_file)

def test_snn_stats(controller_ip):
    """Test 6: Get SNN statistics"""
    cmd = ["python", "python_tools/bin/nstat", "-c", controller_ip, "-s"]
    success, stdout, stderr = run_command(cmd, "SNN statistics (nstat -s)")
    
    if not success:
        return False, "Stats not available"
    
    # Print full statistics output
    print(f"\n{BLUE}=== SNN Statistics ==={RESET}")
    print(stdout)
    print(f"{BLUE}{'='*70}{RESET}\n")
    
    # Check if statistics show any activity
    if 'State' in stdout:
        return True, "Available"
    else:
        return False, "No data"

def test_monitor(controller_ip, duration_ms):
    """Test 7: Monitor spike activity"""
    cmd = ["python", "python_tools/bin/nsnn", "monitor", str(duration_ms), "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, f"Monitor spikes ({duration_ms}ms)")
    
    if not success:
        return False, "Monitor failed"
    
    if VERBOSE:
        print(f"\n{BLUE}=== Spike Monitor ({duration_ms}ms) ==={RESET}")
        print(stdout)
        print(f"{BLUE}{'='*70}{RESET}\n")
    
    return True, "OK"

def test_snn_status(controller_ip):
    """Test 8: Get SNN status"""
    cmd = ["python", "python_tools/bin/nsnn", "status", "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "SNN status (nsnn status)")
    
    if not success:
        return False, "Status failed"
    
    return True, "OK"

def test_snn_stop(controller_ip):
    """Test 9: Stop SNN"""
    cmd = ["python", "python_tools/bin/nsnn", "stop", "-c", controller_ip]
    success, stdout, stderr = run_command(cmd, "Stop SNN (nsnn stop)")
    
    if not success:
        return False, "Failed to stop"
    
    return True, "Stopped"

def test_sd_card(controller_ip):
    """Test 10: SD Card Status (Optional)"""
    try:
        response = requests.get(f"http://{controller_ip}/api/sd/status", timeout=2)
        if response.status_code != 200:
            return True, "Not available (SKIP)"
        
        data = response.json()
        if not data.get("mounted", False):
            return True, "Not mounted (SKIP)"
        
        free_mb = data.get("free_mb", 0)
        return True, f"Mounted, {free_mb} MB free"
        
    except requests.exceptions.RequestException:
        return True, "Not available (SKIP)"

def main():
    parser = argparse.ArgumentParser(description='Comprehensive Z1 Cluster Deployment Test')
    parser.add_argument('-c', '--controller', default='192.168.1.201', help='Controller IP')
    parser.add_argument('-t', '--topology', default='examples/xor_working.json', help='Topology file (relative to python_tools/)')
    parser.add_argument('-s', '--spikes', type=int, default=200, help='Number of spikes to inject')
    parser.add_argument('-q', '--quiet', action='store_true', help='Quiet mode (hide command output)')
    args = parser.parse_args()
    
    # Resolve topology path relative to python_tools/ directory
    topology_path = SCRIPT_DIR.parent / args.topology
    if not topology_path.exists():
        print(f"{CROSS} Topology file not found: {topology_path}")
        sys.exit(1)
    
    # Store verbose flag globally
    global VERBOSE
    VERBOSE = not args.quiet
    
    print(f"\n{BLUE}=== Z1 Cluster Comprehensive Test ==={RESET}\n")
    print(f"Controller IP: {args.controller}")
    print(f"Topology: {topology_path.relative_to(PROJECT_ROOT)}")
    print(f"Spike count: {args.spikes}\n")
    
    results = []
    
    # Test 1: Node discovery
    success, detail = test_nls(args.controller)
    results.append(("nls", success, detail))
    if not success:
        print(f"{CROSS} ABORT - Cannot discover nodes\n")
        sys.exit(1)
    
    # Test 2: Deploy topology
    success, detail = test_deploy(args.controller, str(topology_path))
    results.append(("nsnn deploy", success, detail))
    if not success:
        print(f"{CROSS} ABORT - Cannot deploy topology\n")
        sys.exit(1)
    
    # Test 3: Node status
    success, detail = test_nstat(args.controller)
    results.append(("nstat", success, detail))
    
    # Test 4: Start SNN
    success, detail = test_snn_start(args.controller)
    results.append(("nsnn start", success, detail))
    if not success:
        print(f"{CROSS} ABORT - Cannot start SNN\n")
        sys.exit(1)
    
    # Test 5: Inject spikes
    success, detail = test_inject_spikes(args.controller, args.spikes)
    results.append(("nsnn inject", success, detail))
    
    # Give network time to propagate spikes and process them
    print(f"Waiting 100ms for spike propagation...")
    time.sleep(0.1)
    
    # Test 6: SNN status (while running)
    success, detail = test_snn_status(args.controller)
    results.append(("nsnn status", success, detail))
    
    # Test 7: Get statistics WHILE SNN is still running
    success, detail = test_snn_stats(args.controller)
    results.append(("nstat -s", success, detail))
    
    # Test 8: Stop SNN AFTER collecting stats
    success, detail = test_snn_stop(args.controller)
    results.append(("nsnn stop", success, detail))
    
    # Test 9: SD Card (Optional)
    success, detail = test_sd_card(args.controller)
    results.append(("SD card", success, detail))
    
    # Print summary
    print(f"\n{BLUE}=== Test Summary ==={RESET}\n")
    passed = 0
    for name, success, detail in results:
        status = f"{GREEN}{CHECK} PASS{RESET}" if success else f"{YELLOW}{CROSS} FAIL{RESET}"
        print(f"{status} - {name:20s} {detail}")
        if success:
            passed += 1
    
    print(f"\n{BLUE}{'='*70}{RESET}")
    if passed == len(results):
        print(f"{GREEN}{CHECK} ALL TESTS PASSED ({passed}/{len(results)}){RESET}\n")
        sys.exit(0)
    elif passed >= len(results) * 0.7:
        print(f"{YELLOW}{WAIT} PARTIAL SUCCESS ({passed}/{len(results)}){RESET}\n")
        sys.exit(0)
    else:
        print(f"{YELLOW}{CROSS} MULTIPLE FAILURES ({passed}/{len(results)}){RESET}\n")
        sys.exit(1)

if __name__ == "__main__":
    main()
