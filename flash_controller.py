#!/usr/bin/env python3
"""
Z1 Onyx - Flash Controller Firmware via USB
Flashes controller firmware to a Pico connected in BOOTSEL mode
"""

import sys
import os
import subprocess
import shutil
from pathlib import Path
import argparse

# ANSI colors
GREEN = '\033[92m'
YELLOW = '\033[93m'
RED = '\033[91m'
CYAN = '\033[96m'
RESET = '\033[0m'

def find_picotool():
    """Find picotool in PATH or build_tools"""
    # Check build_tools first
    project_root = Path(__file__).parent
    local_picotool = project_root / "build_tools" / "picotool.exe"
    if local_picotool.exists():
        return str(local_picotool)
    
    # Check PATH
    tool = shutil.which("picotool")
    if tool:
        return tool
    
    return None

def find_firmware(hw_version):
    """Find controller firmware file for specified hardware version"""
    project_root = Path(__file__).parent
    
    if hw_version == "v2":
        firmware_dir = project_root / "FirmwareReleases" / "16node"
        firmware_file = firmware_dir / "controller_16.uf2"
    else:
        firmware_dir = project_root / "FirmwareReleases" / "12node"
        firmware_file = firmware_dir / "controller_12.uf2"
    
    return firmware_file if firmware_file.exists() else None

def main():
    parser = argparse.ArgumentParser(description='Flash Z1 Onyx controller firmware via USB')
    parser.add_argument('--hw-v1', action='store_true', help='Use V1 hardware (12 nodes)')
    parser.add_argument('--hw-v2', action='store_true', help='Use V2 hardware (16 nodes)')
    args = parser.parse_args()
    
    # Default to V2 if not specified
    if not args.hw_v1 and not args.hw_v2:
        args.hw_v2 = True
    
    hw_version = "v1" if args.hw_v1 else "v2"
    hw_label = "V1 (12-node)" if args.hw_v1 else "V2 (16-node)"
    
    print(f"\n{CYAN}{'='*60}{RESET}")
    print(f"{CYAN}Z1 Onyx Controller Firmware Flash Utility{RESET}")
    print(f"{CYAN}Hardware: {hw_label}{RESET}")
    print(f"{CYAN}{'='*60}{RESET}\n")
    
    # Find picotool
    picotool = find_picotool()
    if not picotool:
        print(f"{RED}[ERROR] picotool not found in PATH or build_tools!{RESET}")
        print(f"\nInstall picotool or add it to PATH:")
        print(f"  https://github.com/raspberrypi/picotool\n")
        return 1
    
    print(f"{GREEN}[OK] Found picotool: {picotool}{RESET}")
    
    # Find firmware
    firmware_file = find_firmware(hw_version)
    if not firmware_file:
        print(f"{RED}[ERROR] Controller firmware not found{RESET}")
        if args.hw_v1:
            print(f"\nRun: python build.py --hw-v1\n")
        else:
            print(f"\nRun: python build.py (or python build_dual.py for OTA support)\n")
        return 1
    
    print(f"{GREEN}[OK] Firmware: {firmware_file.relative_to(Path(__file__).parent)}{RESET}\n")
    
    # Step 1: Reboot to BOOTSEL
    print(f"{CYAN}Step 1: Rebooting device to BOOTSEL mode...{RESET}")
    try:
        result = subprocess.run([picotool, "reboot", "-f", "-u"], 
                              capture_output=True, text=True, timeout=10)
        if result.returncode != 0:
            print(f"{YELLOW}[WARN] Device may already be in BOOTSEL mode{RESET}")
    except subprocess.TimeoutExpired:
        print(f"{YELLOW}[WARN] Reboot command timed out (device may already be in BOOTSEL){RESET}")
    except Exception as e:
        print(f"{YELLOW}[WARN] Could not reboot device: {e}{RESET}")
        print(f"{YELLOW}Please manually enter BOOTSEL mode (hold BOOTSEL button, connect USB){RESET}")
        input(f"\nPress Enter when device is in BOOTSEL mode...")
    
    # Wait for device to enumerate in BOOTSEL mode
    import time
    print(f"{CYAN}Waiting for device to enter BOOTSEL mode...{RESET}")
    time.sleep(3)
    
    # Step 2: Load firmware
    print(f"\n{CYAN}Step 2: Loading firmware...{RESET}")
    try:
        result = subprocess.run([picotool, "load", str(firmware_file), "-x"], 
                              capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            print(f"{RED}[ERROR] Failed to load firmware:{RESET}")
            print(result.stderr)
            return 1
        
        print(f"\n{GREEN}{'='*60}{RESET}")
        print(f"{GREEN}SUCCESS! Controller firmware flashed and running.{RESET}")
        print(f"{GREEN}{'='*60}{RESET}\n")
        return 0
        
    except subprocess.TimeoutExpired:
        print(f"{RED}[ERROR] Flash operation timed out{RESET}")
        return 1
    except Exception as e:
        print(f"{RED}[ERROR] Failed to flash firmware: {e}{RESET}")
        return 1

if __name__ == "__main__":
    sys.exit(main())
