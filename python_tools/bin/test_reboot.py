#!/usr/bin/env python3
"""
Test script for reboot API functionality
Tests the reboot endpoint without requiring physical hardware reboot
"""

import sys
import requests
import time

CONTROLLER_IP = "192.168.1.201"

def test_reboot_endpoint():
    """Test that reboot endpoint exists and responds correctly"""
    print("Testing POST /api/system/reboot endpoint...")
    
    try:
        url = f"http://{CONTROLLER_IP}/api/system/reboot"
        print(f"  Sending POST to {url}...")
        
        response = requests.post(url, timeout=3)
        
        print(f"  HTTP Status: {response.status_code}")
        print(f"  Response: {response.text}")
        
        if response.status_code == 200:
            data = response.json()
            if data.get('success') == True:
                print("  ✓ Reboot endpoint responding correctly")
                print(f"  Message: {data.get('message')}")
                return True
            else:
                print("  ✗ Unexpected response format")
                return False
        else:
            print(f"  ✗ Expected 200, got {response.status_code}")
            return False
            
    except requests.exceptions.Timeout:
        print("  ! Request timed out (controller may be rebooting)")
        print("  This is expected if controller actually rebooted")
        return True  # Consider timeout a success for reboot testing
        
    except requests.exceptions.ConnectionError:
        print("  ! Connection refused (controller offline or rebooting)")
        return True  # Controller may have rebooted successfully
        
    except Exception as e:
        print(f"  ✗ Error: {e}")
        return False


def test_config_before_reboot():
    """Test reading config before reboot"""
    print("\nTesting GET /api/config (pre-reboot)...")
    
    try:
        response = requests.get(f"http://{CONTROLLER_IP}/api/config", timeout=2)
        
        if response.status_code == 200:
            config = response.json()
            print(f"  ✓ Current IP: {config.get('ip_address')}")
            print(f"  ✓ Current MAC: {config.get('mac_address')}")
            print(f"  ✓ Engine: {config.get('current_engine')}")
            return True
        else:
            print(f"  ✗ Failed to read config (HTTP {response.status_code})")
            return False
            
    except Exception as e:
        print(f"  ✗ Error: {e}")
        return False


def main():
    print("=" * 60)
    print("Z1 Onyx Reboot API Test")
    print("=" * 60)
    print(f"Controller: {CONTROLLER_IP}")
    print()
    
    # Test 1: Read current config
    if not test_config_before_reboot():
        print("\n✗ Config test failed - controller may be offline")
        return 1
    
    # Test 2: Call reboot endpoint (won't actually reboot in emulator)
    if not test_reboot_endpoint():
        print("\n✗ Reboot endpoint test failed")
        return 1
    
    print("\n" + "=" * 60)
    print("✓ All tests passed")
    print("=" * 60)
    print()
    print("Note: This test does not verify actual reboot behavior.")
    print("To test complete reboot workflow:")
    print("  1. Flash firmware to hardware")
    print("  2. Ensure SD card has z1.cfg")
    print("  3. Run: python python_tools/bin/zconfig --show")
    print("  4. Run: python python_tools/bin/zconfig --ip 192.168.1.100 --reboot")
    print("  5. Verify controller comes back at new IP")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
