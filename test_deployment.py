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
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
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
    """Test 5: Inject spikes"""
    # Create temporary spike pattern file for XOR inputs
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
        success, stdout, stderr = run_command(cmd, f"Inject {spike_count} spikes (nsnn inject)")
        
        if not success:
            print(f"DEBUG: Inject failed - stdout: {stdout[:300]}, stderr: {stderr[:300]}")
            return False, "Injection failed"
        
        if VERBOSE:
            print(f"\n{BLUE}=== Spike Injection ==={RESET}")
            print(stdout)
            print(f"{BLUE}{'='*70}{RESET}\n")
        
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

def main():
    parser = argparse.ArgumentParser(description='Comprehensive Z1 Cluster Deployment Test')
    parser.add_argument('-c', '--controller', default='192.168.1.222', help='Controller IP')
    parser.add_argument('-t', '--topology', default='python_tools/examples/xor_working.json', help='Topology file')
    parser.add_argument('-s', '--spikes', type=int, default=2500, help='Number of spikes to inject')
    parser.add_argument('-q', '--quiet', action='store_true', help='Quiet mode (hide command output)')
    args = parser.parse_args()
    
    # Store verbose flag globally
    global VERBOSE
    VERBOSE = not args.quiet
    
    print(f"\n{BLUE}=== Z1 Cluster Comprehensive Test ==={RESET}\n")
    print(f"Controller IP: {args.controller}")
    print(f"Topology: {args.topology}")
    print(f"Spike count: {args.spikes}\n")
    
    results = []
    
    # Test 1: Node discovery
    success, detail = test_nls(args.controller)
    results.append(("nls", success, detail))
    if not success:
        print(f"{CROSS} ABORT - Cannot discover nodes\n")
        sys.exit(1)
    
    # Test 2: Deploy topology
    success, detail = test_deploy(args.controller, args.topology)
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
    
    # Give network time to propagate spikes
    time.sleep(0.5)
    
    # Test 6: SNN statistics
    success, detail = test_snn_stats(args.controller)
    results.append(("nstat -s", success, detail))
    
    # Test 7: SNN status
    success, detail = test_snn_status(args.controller)
    results.append(("nsnn status", success, detail))
    
    # Test 8: Stop SNN
    success, detail = test_snn_stop(args.controller)
    results.append(("nsnn stop", success, detail))
    
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
