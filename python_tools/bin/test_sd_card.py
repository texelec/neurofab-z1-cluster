#!/usr/bin/env python3
"""
Z1 Onyx - SD Card API Test Script

Tests SD card functionality via HTTP API:
1. SD card status and health
2. Configuration file read/write
3. File upload/list/delete operations
4. Directory operations

Usage:
    python test_sd_card.py [controller_ip]
    Default IP: 192.168.1.201
"""

import sys
import requests
import json
from pathlib import Path

# ANSI colors
GREEN = '\033[92m'
BLUE = '\033[94m'
YELLOW = '\033[93m'
RED = '\033[91m'
RESET = '\033[0m'

# Symbols
CHECK = '[OK]'
CROSS = '[FAIL]'
SKIP = '[SKIP]'

def test_sd_status(controller_ip):
    """Test 1: Check SD card status"""
    print(f"\n{BLUE}Test 1: SD Card Status{RESET}")
    try:
        r = requests.get(f"http://{controller_ip}/api/sd/status", timeout=5)
        if r.status_code == 200:
            data = r.json()
            if data.get("mounted"):
                free_mb = data.get("free_mb", 0)
                print(f"{GREEN}{CHECK} SD card mounted, {free_mb} MB free{RESET}")
                return True
            else:
                error = data.get("error", "Unknown error")
                print(f"{YELLOW}{SKIP} SD card not mounted: {error}{RESET}")
                return False
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def test_config_read(controller_ip):
    """Test 2: Read configuration"""
    print(f"\n{BLUE}Test 2: Read Configuration{RESET}")
    try:
        r = requests.get(f"http://{controller_ip}/api/config", timeout=5)
        if r.status_code == 200:
            data = r.json()
            ip = data.get("ip_address", "unknown")
            engine = data.get("current_engine", "unknown")
            print(f"{GREEN}{CHECK} Config: IP={ip}, Engine={engine}{RESET}")
            return True
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def test_config_write(controller_ip):
    """Test 3: Write configuration"""
    print(f"\n{BLUE}Test 3: Update Configuration{RESET}")
    try:
        config = {"current_engine": "test_engine"}
        r = requests.post(f"http://{controller_ip}/api/config", 
                         json=config, timeout=5)
        if r.status_code == 200:
            data = r.json()
            if data.get("success"):
                print(f"{GREEN}{CHECK} Config updated successfully{RESET}")
                
                # Verify by reading back
                r2 = requests.get(f"http://{controller_ip}/api/config", timeout=5)
                if r2.status_code == 200:
                    data2 = r2.json()
                    if data2.get("current_engine") == "test_engine":
                        print(f"{GREEN}{CHECK} Config verified: engine=test_engine{RESET}")
                        return True
                print(f"{YELLOW}{SKIP} Config not verified{RESET}")
                return False
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def test_file_upload(controller_ip):
    """Test 4: Upload file"""
    print(f"\n{BLUE}Test 4: Upload File{RESET}")
    try:
        test_data = b"Hello Z1 Onyx!\nTest file from Python.\n" * 10
        r = requests.put(f"http://{controller_ip}/api/files/engines/test.txt",
                        data=test_data, timeout=5)
        if r.status_code == 200:
            data = r.json()
            if data.get("success"):
                size = data.get("size", 0)
                print(f"{GREEN}{CHECK} Uploaded test.txt ({size} bytes){RESET}")
                return True
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def test_file_list(controller_ip):
    """Test 5: List directory"""
    print(f"\n{BLUE}Test 5: List Files{RESET}")
    try:
        r = requests.get(f"http://{controller_ip}/api/files/engines", timeout=5)
        if r.status_code == 200:
            data = r.json()
            files = data.get("files", [])
            count = data.get("count", 0)
            print(f"{GREEN}{CHECK} Found {count} files in engines/:{RESET}")
            for f in files[:5]:  # Show first 5
                name = f.get("name", "?")
                size = f.get("size", 0)
                print(f"  - {name} ({size} bytes)")
            if count > 5:
                print(f"  ... and {count - 5} more")
            return True
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def test_file_delete(controller_ip):
    """Test 6: Delete file"""
    print(f"\n{BLUE}Test 6: Delete File{RESET}")
    try:
        r = requests.delete(f"http://{controller_ip}/api/files/engines/test.txt",
                           timeout=5)
        if r.status_code == 200:
            data = r.json()
            if data.get("success"):
                print(f"{GREEN}{CHECK} Deleted test.txt{RESET}")
                
                # Verify by listing
                r2 = requests.get(f"http://{controller_ip}/api/files/engines", timeout=5)
                if r2.status_code == 200:
                    data2 = r2.json()
                    files = data2.get("files", [])
                    if not any(f.get("name") == "test.txt" for f in files):
                        print(f"{GREEN}{CHECK} File deleted verified{RESET}")
                        return True
                print(f"{YELLOW}{SKIP} Delete not verified{RESET}")
                return False
        else:
            print(f"{RED}{CROSS} HTTP {r.status_code}: {r.text}{RESET}")
            return False
    except Exception as e:
        print(f"{RED}{CROSS} Request failed: {e}{RESET}")
        return False

def main():
    print(f"\n{BLUE}{'='*60}{RESET}")
    print(f"{BLUE}Z1 Onyx - SD Card API Test{RESET}")
    print(f"{BLUE}{'='*60}{RESET}")
    
    controller_ip = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.201"
    print(f"Controller IP: {controller_ip}\n")
    
    tests = [
        ("SD Status", test_sd_status),
        ("Config Read", test_config_read),
        ("Config Write", test_config_write),
        ("File Upload", test_file_upload),
        ("File List", test_file_list),
        ("File Delete", test_file_delete),
    ]
    
    results = {}
    for name, test_func in tests:
        try:
            results[name] = test_func(controller_ip)
        except KeyboardInterrupt:
            print(f"\n{YELLOW}Test interrupted by user{RESET}")
            break
    
    # Summary
    print(f"\n{BLUE}{'='*60}{RESET}")
    print(f"{BLUE}Test Summary{RESET}")
    print(f"{BLUE}{'='*60}{RESET}")
    
    passed = sum(1 for v in results.values() if v)
    total = len(results)
    
    for name, result in results.items():
        status = f"{GREEN}{CHECK}" if result else f"{RED}{CROSS}"
        print(f"{status} {name}{RESET}")
    
    print(f"\n{BLUE}Result: {passed}/{total} tests passed{RESET}")
    
    if passed == total:
        print(f"{GREEN}All tests passed!{RESET}\n")
        return 0
    else:
        print(f"{YELLOW}Some tests failed{RESET}\n")
        return 1

if __name__ == "__main__":
    sys.exit(main())
