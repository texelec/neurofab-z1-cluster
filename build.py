#!/usr/bin/env python3
"""
Z1 Onyx Cluster - Build Script
Cross-platform build automation for controller and node firmware
Uses tools from system PATH - no hardcoded paths
"""

import os
import sys
import subprocess
import shutil
from pathlib import Path

# ANSI color codes
class Colors:
    CYAN = '\033[96m'
    GREEN = '\033[92m'
    YELLOW = '\033[93m'
    RED = '\033[91m'
    RESET = '\033[0m'
    BOLD = '\033[1m'

def print_header(msg):
    print(f"\n{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}")
    print(f"{Colors.CYAN}{Colors.BOLD}{msg}{Colors.RESET}")
    print(f"{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}\n")

def print_step(step, msg):
    print(f"{Colors.CYAN}Step {step}: {msg}...{Colors.RESET}")

def print_success(msg):
    print(f"  {Colors.GREEN} {msg}{Colors.RESET}")

def print_warning(msg):
    print(f"  {Colors.YELLOW} {msg}{Colors.RESET}")

def print_error(msg):
    print(f"{Colors.RED} {msg}{Colors.RESET}")

def find_tool(name, required=False):
    """Find tool in PATH"""
    tool = shutil.which(name)
    if tool:
        print_success(f"Found {name}: {tool}")
        return tool
    elif required:
        print_error(f"{name} not found in PATH!")
        return None
    else:
        print_warning(f"{name} not found in PATH")
        return None

def run_command(cmd, cwd=None, check=True):
    """Run shell command and return result"""
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            check=check,
            capture_output=False,
            shell=False
        )
        return result.returncode == 0
    except subprocess.CalledProcessError as e:
        if check:
            raise
        return False
    except FileNotFoundError:
        print_error(f"Command not found: {cmd[0]}")
        return False

def main():
    # Parse command line arguments
    import argparse
    parser = argparse.ArgumentParser(description="Z1 Onyx Cluster Build Script")
    parser.add_argument("--hw-v1", action="store_true", help="Build for V1 hardware (12 nodes)")
    parser.add_argument("--hw-v2", action="store_true", help="Build for V2 hardware (16 nodes)")
    args = parser.parse_args()
    
    # Default to V2 if neither specified
    if not args.hw_v1 and not args.hw_v2:
        args.hw_v2 = True
    
    hw_version = "V1" if args.hw_v1 else "V2"
    
    # Paths
    project_root = Path(__file__).parent.absolute()
    build_dir = project_root / "build"
    release_dir = project_root / "FirmwareReleases"

    print_header(f"Z1 Onyx Cluster - Build Script (Hardware {hw_version})")
    
    # Check required environment
    print_step(0, "Checking build environment")
    
    pico_sdk = os.environ.get('PICO_SDK_PATH')
    if not pico_sdk:
        print_error("PICO_SDK_PATH environment variable not set!")
        sys.exit(1)
    print_success(f"PICO_SDK_PATH: {pico_sdk}")
    
    cmake = find_tool("cmake", required=True)
    ninja = find_tool("ninja", required=True)
    
    if not cmake or not ninja:
        print_error("Required tools not found in PATH!")
        sys.exit(1)

    # Create directories
    build_dir.mkdir(exist_ok=True)
    release_dir.mkdir(exist_ok=True)

    # Change to build directory
    original_dir = Path.cwd()
    os.chdir(build_dir)

    try:
        # Step 1: Regenerate PIO headers if pioasm is available
        print_step(1, "Checking for PIO headers")
        pioasm = find_tool("pioasm")
        
        if pioasm:
            bus_dir = project_root / "common" / "z1_onyx_bus"
            run_command([
                pioasm, "-o", "c-sdk",
                str(bus_dir / "z1_bus_tx.pio"),
                str(bus_dir / "z1_bus_tx.pio.h")
            ])
            run_command([
                pioasm, "-o", "c-sdk",
                str(bus_dir / "z1_bus_rx.pio"),
                str(bus_dir / "z1_bus_rx.pio.h")
            ])
            print_success("PIO headers generated")
        else:
            print_warning("pioasm not found, using pre-generated headers")

        # Step 2: Configure with CMake
        cmake_opts = [cmake, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
        if args.hw_v1:
            cmake_opts.extend(["-DBUILD_HW_V1=ON", "-DBUILD_HW_V2=OFF"])
        else:
            cmake_opts.extend(["-DBUILD_HW_V1=OFF", "-DBUILD_HW_V2=ON"])
        cmake_opts.append("..")
        
        # Check if hardware variant has changed
        needs_reconfigure = False
        cmake_cache = build_dir / "CMakeCache.txt"
        
        if not (build_dir / "build.ninja").exists():
            needs_reconfigure = True
        elif cmake_cache.exists():
            # Check if hardware variant matches
            cache_content = cmake_cache.read_text()
            current_v1 = "BUILD_HW_V1:BOOL=ON" in cache_content
            current_v2 = "BUILD_HW_V2:BOOL=ON" in cache_content
            
            if args.hw_v1 and not current_v1:
                needs_reconfigure = True
                print_warning("Hardware variant changed to V1, reconfiguring...")
            elif args.hw_v2 and not current_v2:
                needs_reconfigure = True
                print_warning("Hardware variant changed to V2, reconfiguring...")
        
        if needs_reconfigure:
            print_step(2, "Configuring build with CMake...")
            if not run_command(cmake_opts):
                raise RuntimeError("CMake configuration failed!")
            print_success("CMake configuration complete")
        else:
            print_step(2, "Build already configured...")

        # Step 3: Build firmware
        print_step(3, "Building firmware")

        if args.hw_v1:
            # Build V1: controller_12 + node_12_0 through node_12_11
            targets = ["controller_12"] + [f"node_12_{i}" for i in range(12)]
        else:
            # Build V2: controller_16 + node_16
            targets = ["controller_16", "node_16"]
        
        if not run_command([ninja] + targets):
            raise RuntimeError("Build failed!")

        print_success("Build complete")
        
        # Step 4: Copy to release directory
        print_step(4, "Copying firmware to releases...")

        if args.hw_v1:
            hw_release_dir = release_dir / "12node"
            firmware_files = [("controller/controller_12", "z1_controller_12")]
            firmware_files.extend([
                (f"node/node_12_{i}", f"z1_node_12_{i}") 
                for i in range(12)
            ])
        else:
            hw_release_dir = release_dir / "16node"
            firmware_files = [
                ("controller/controller_16", "z1_controller_16"),
                ("node/node_16", "z1_node_16"),
            ]
        
        hw_release_dir.mkdir(parents=True, exist_ok=True)

        copied_count = 0
        for src_base, dst_base in firmware_files:
            # Only copy UF2 files to releases (hex/bin stay in build/)
            src_uf2 = build_dir / f"{src_base}.uf2"

            if src_uf2.exists():
                dst_path = hw_release_dir / f"{dst_base}.uf2"
                shutil.copy2(src_uf2, dst_path)
                size_kb = src_uf2.stat().st_size / 1024
                print_success(f"{dst_path.name} ({size_kb:.1f} KB)")
                copied_count += 1
            else:
                print_warning(f"UF2 not found for {src_base}")

        if copied_count == 0:
            raise RuntimeError("No firmware files were copied!")

        # Success!
        print_header("Build completed successfully!")
        hw_subdir = "12node" if args.hw_v1 else "16node"
        print(f"\n{Colors.GREEN}Firmware ready in: {Colors.BOLD}FirmwareReleases/{hw_subdir}/{Colors.RESET}")
        print(f"\n{Colors.CYAN}Files created:{Colors.RESET}")
        for file in sorted(hw_release_dir.iterdir()):
            if file.is_file():
                size_kb = file.stat().st_size / 1024
                print(f"  {file.name} ({size_kb:.1f} KB)")

    except Exception as e:
        print_header("Build FAILED!")
        print_error(str(e))
        import traceback
        traceback.print_exc()
        sys.exit(1)

    finally:
        os.chdir(original_dir)

if __name__ == "__main__":
    main()
