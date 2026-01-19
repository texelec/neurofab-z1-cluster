#!/usr/bin/env python3
"""
Z1 Onyx - Flash Node Firmware via USB
Flashes node firmware to a Pico connected in BOOTSEL mode
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
    """Find node firmware file for specified hardware version"""
    project_root = Path(__file__).parent
    
    if hw_version == "v2":
        # V2: Single node firmware with auto-detect
        firmware_dir = project_root / "FirmwareReleases" / "16node"
        firmware_file = firmware_dir / "node_dual_16.uf2"
    else:
        # V1: Show all available node firmwares
        firmware_dir = project_root / "FirmwareReleases" / "12node"
        print(f"\n{YELLOW}V1 hardware uses per-node firmware files:{RESET}")
        for i in range(12):
            fw = firmware_dir / f"node_dual_12_{i}.uf2"
            if fw.exists():
                print(f"  - node_dual_12_{i}.uf2 (for Node {i})")
        print(f"\n{CYAN}Please specify which node firmware to flash with --node <id>{RESET}")
        return None
    
    return firmware_file if firmware_file.exists() else None

def main():
    parser = argparse.ArgumentParser(description='Flash Z1 Onyx node firmware via USB')
    parser.add_argument('--hw-v1', action='store_true', help='Use V1 hardware (12 nodes)')
    parser.add_argument('--hw-v2', action='store_true', help='Use V2 hardware (16 nodes)')
    parser.add_argument('--node', type=int, help='Node ID (0-11 for V1)')
    args = parser.parse_args()
    
    # Default to V2 if not specified
    if not args.hw_v1 and not args.hw_v2:
        args.hw_v2 = True
    
    hw_version = "v1" if args.hw_v1 else "v2"
    hw_label = "V1 (12-node)" if args.hw_v1 else "V2 (16-node)"
    
    print(f"\n{CYAN}{'='*60}{RESET}")
    print(f"{CYAN}Z1 Onyx Node Firmware Flash Utility{RESET}")
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
    if args.hw_v1:
        if args.node is None:
            find_firmware("v1")  # Shows available files
            return 1
        
        if args.node < 0 or args.node > 11:
            print(f"{RED}[ERROR] Node ID must be 0-11 for V1 hardware{RESET}")
            return 1
        
        project_root = Path(__file__).parent
        firmware_dir = project_root / "FirmwareReleases" / "12node"
        firmware_file = firmware_dir / f"node_dual_12_{args.node}.uf2"
        
        if not firmware_file.exists():
            print(f"{RED}[ERROR] Firmware not found: {firmware_file}{RESET}")
            print(f"\nRun: python build.py --hw-v1\n")
            return 1
    else:
        firmware_file = find_firmware("v2")
        if not firmware_file:
            print(f"{RED}[ERROR] Firmware not found{RESET}")
            print(f"\nRun: python build_dual.py\n")
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
        print(f"{GREEN}SUCCESS! Node firmware flashed and running.{RESET}")
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
