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
    parser.add_argument("--hw-v1", action="store_true", help="Build ONLY V1 hardware (12 nodes)")
    parser.add_argument("--hw-v2", action="store_true", help="Build ONLY V2 hardware (16 nodes)")
    args = parser.parse_args()
    
    # Default: Build BOTH hardware versions if neither specified
    if not args.hw_v1 and not args.hw_v2:
        build_both = True
        hw_versions = ["V1", "V2"]
    elif args.hw_v1 and args.hw_v2:
        build_both = True
        hw_versions = ["V1", "V2"]
    elif args.hw_v1:
        build_both = False
        hw_versions = ["V1"]
    else:
        build_both = False
        hw_versions = ["V2"]
    
    # Paths
    project_root = Path(__file__).parent.absolute()
    build_dir = project_root / "build"
    release_dir = project_root / "FirmwareReleases"
    packages_dir = project_root / "packages"

    print_header(f"Z1 Onyx Cluster - Build Script")
    if build_both:
        print(f"{Colors.CYAN}Building: V1 (12-node) + V2 (16-node){Colors.RESET}\n")
    else:
        print(f"{Colors.CYAN}Building: {hw_versions[0]}{Colors.RESET}\n")
    
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
    packages_dir.mkdir(exist_ok=True)

    # Change to build directory
    original_dir = Path.cwd()
    os.chdir(build_dir)

    try:
        # Step 1: Regenerate PIO headers if pioasm is available (once for both versions)
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

        # Build each hardware version
        for hw_version in hw_versions:
            is_v1 = (hw_version == "V1")
            print(f"\n{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}")
            print(f"{Colors.CYAN}{Colors.BOLD}Building {hw_version} Hardware{Colors.RESET}")
            print(f"{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}\n")

            # Step 2: Configure with CMake
            # Step 2: Configure with CMake
            cmake_opts = [cmake, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"]
            if is_v1:
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
                
                if is_v1 and not current_v1:
                    needs_reconfigure = True
                    print_warning("Hardware variant changed to V1, reconfiguring...")
                elif not is_v1 and not current_v2:
                    needs_reconfigure = True
                    print_warning("Hardware variant changed to V2, reconfiguring...")
            
            if needs_reconfigure:
                print_step(2, f"Configuring build for {hw_version}...")
                if not run_command(cmake_opts):
                    raise RuntimeError("CMake configuration failed!")
                print_success("CMake configuration complete")
            else:
                print_step(2, f"Build already configured for {hw_version}")

            # Step 3: Build firmware
            print_step(3, f"Building {hw_version} firmware")

            if is_v1:
                # Build V1: controller_12 + bootloader_12 + node_app_12_N (N=0-11)
                targets = ["controller_12", "bootloader_12"] + [f"node_app_12_{i}" for i in range(12)]
            else:
                # Build V2: controller_16 + bootloader_16 + node_app_16
                targets = ["controller_16", "bootloader_16", "node_app_16"]
            
            if not run_command([ninja] + targets):
                raise RuntimeError(f"{hw_version} build failed!")

            print_success(f"{hw_version} build complete")
            
            # Step 4: Copy to release directory
            print_step(4, f"Copying {hw_version} firmware to releases...")

            hw_release_dir = release_dir / ("12node" if is_v1 else "16node")
            apponly_dir = hw_release_dir / "apponly"
            hw_release_dir.mkdir(parents=True, exist_ok=True)
            apponly_dir.mkdir(parents=True, exist_ok=True)

            copied_count = 0
            
            # Controller goes in root (always)
            controller_name = "controller_12" if is_v1 else "controller_16"
            src_uf2 = build_dir / f"controller/{controller_name}.uf2"
            if src_uf2.exists():
                dst_path = hw_release_dir / f"{controller_name}.uf2"
                shutil.copy2(src_uf2, dst_path)
                size_kb = src_uf2.stat().st_size / 1024
                print_success(f"{controller_name}.uf2 ({size_kb:.1f} KB) → root")
                copied_count += 1
            
            # Copy bootloader and node_app to apponly/ (both V1 and V2)
            if is_v1:
                # V1: bootloader_12 and node_app_12_N
                src_uf2 = build_dir / "bootloader/bootloader_12.uf2"
                if src_uf2.exists():
                    dst_path = apponly_dir / "bootloader_12.uf2"
                    shutil.copy2(src_uf2, dst_path)
                    size_kb = src_uf2.stat().st_size / 1024
                    print_success(f"bootloader_12.uf2 ({size_kb:.1f} KB) → apponly/")
                    copied_count += 1
                    
                for i in range(12):
                    src_uf2 = build_dir / f"node/node_app_12_{i}.uf2"
                    if src_uf2.exists():
                        dst_path = apponly_dir / f"node_app_12_{i}.uf2"
                        shutil.copy2(src_uf2, dst_path)
                        copied_count += 1
                print_success(f"node_app_12_0.uf2 through node_app_12_11.uf2 (12 files) → apponly/")
            else:
                # V2: bootloader_16 and node_app_16
                apponly_files = [
                    ("bootloader/bootloader_16", "bootloader_16"),
                    ("node/node_app_16", "node_app_16"),
                ]
                
                for src_base, dst_base in apponly_files:
                    src_uf2 = build_dir / f"{src_base}.uf2"
                    if src_uf2.exists():
                        dst_path = apponly_dir / f"{dst_base}.uf2"
                        shutil.copy2(src_uf2, dst_path)
                        size_kb = src_uf2.stat().st_size / 1024
                        print_success(f"{dst_base}.uf2 ({size_kb:.1f} KB) → apponly/")
                        copied_count += 1
                    copied_count += 1
                else:
                    print_warning(f"UF2 not found for {src_base}")

            if copied_count == 0:
                raise RuntimeError(f"No {hw_version} firmware files were copied!")

            # Step 5: Create dual-partition firmware (V2 only)
            if not is_v1:
                print_step(5, "Creating dual-partition firmware")
                
                # First, prepend app header to node binary
                header_script = project_root / "build_tools" / "prepend_app_header.py"
                app_bin = build_dir / "node" / "node_app_16.bin"
                app_with_header = build_dir / "node" / "node_app_16_header.bin"
                
                if header_script.exists() and app_bin.exists():
                    python = shutil.which("python3") or shutil.which("python") or shutil.which("py")
                    if python:
                        # Prepend .z1app header to binary
                        header_cmd = [python, str(header_script), str(app_bin), str(app_with_header)]
                        if not run_command(header_cmd, check=False):
                            print_warning("Failed to add app header (non-critical)")
                            app_with_header = app_bin  # Fall back to raw binary
                    else:
                        print_warning("Python not found - using raw binary without header")
                        app_with_header = app_bin
                else:
                    print_warning("Header script or app binary missing - using raw binary")
                    app_with_header = app_bin
                
                # Now merge bootloader + app (with header)
                merge_script = project_root / "build_tools" / "merge_dual_partition.py"
                if merge_script.exists():
                    bootloader_uf2 = build_dir / "bootloader" / "bootloader_16.uf2"
                    output_uf2 = hw_release_dir / "node_dual_16.uf2"
                    
                    if bootloader_uf2.exists() and app_with_header.exists():
                        python = shutil.which("python3") or shutil.which("python") or shutil.which("py")
                        if python:
                            merge_cmd = [python, str(merge_script), str(bootloader_uf2), 
                                        str(app_with_header), str(output_uf2)]
                            
                            if run_command(merge_cmd, check=False):
                                size_kb = output_uf2.stat().st_size / 1024
                                print_success(f"node_dual_16.uf2 ({size_kb:.1f} KB) → root")
                            else:
                                print_warning("Dual-partition merge failed (non-critical)")
                        else:
                            print_warning("Python not found - skipping dual-partition merge")
                    else:
                        print_warning("Skipping dual-partition (bootloader or app binary missing)")
                else:
                    print_warning("Merge script not found (skipping dual-partition)")
                
                # Step 6: Copy node_app.bin to packages directory for OTA deployment
                print_step(6, "Copying node app binary to packages")
                if app_bin.exists():
                    app_dest = packages_dir / "node_app_16.bin"
                    shutil.copy2(app_bin, app_dest)
                    size_kb = app_bin.stat().st_size / 1024
                    print_success(f"node_app_16.bin → packages/ ({size_kb:.1f} KB)")
                else:
                    print_warning("node_app_16.bin not found - skipping packages copy")
            
            else:
                # V1: Create dual-partition firmware for all 12 nodes
                print_step(5, "Creating V1 dual-partition firmware")
                
                merge_script = project_root / "build_tools" / "merge_dual_partition.py"
                if not merge_script.exists():
                    print_warning("Merge script not found - skipping dual-partition")
                    continue
                
                python = shutil.which("python3") or shutil.which("python") or shutil.which("py")
                if not python:
                    print_warning("Python not found - skipping dual-partition")
                    continue
                
                bootloader_uf2 = build_dir / "bootloader" / "bootloader_12.uf2"
                if not bootloader_uf2.exists():
                    print_warning("Bootloader not found - skipping dual-partition")
                    continue
                
                # Prepend headers and merge for each node
                header_script = project_root / "build_tools" / "prepend_app_header.py"
                for node_id in range(12):
                    app_bin = build_dir / "node" / f"node_app_12_{node_id}.bin"
                    if not app_bin.exists():
                        print_warning(f"node_app_12_{node_id}.bin not found")
                        continue
                    
                    # Prepend header
                    app_with_header = build_dir / "node" / f"node_app_12_{node_id}_header.bin"
                    if header_script.exists():
                        header_cmd = [python, str(header_script), str(app_bin), str(app_with_header)]
                        run_command(header_cmd, check=False)
                    else:
                        app_with_header = app_bin
                    
                    # Merge bootloader + app
                    output_uf2 = hw_release_dir / f"node_dual_12_{node_id}.uf2"
                    merge_cmd = [python, str(merge_script), str(bootloader_uf2), 
                                str(app_with_header), str(output_uf2)]
                    
                    if run_command(merge_cmd, check=False):
                        copied_count += 1
                
                if copied_count > 0:
                    print_success(f"node_dual_12_0.uf2 through node_dual_12_11.uf2 (12 files) → root")

        # Success!
        print_header("Build completed successfully!")
        
        print(f"\n{Colors.GREEN}Firmware ready:{Colors.RESET}")
        for hw_v in hw_versions:
            hw_subdir = "12node" if hw_v == "V1" else "16node"
            hw_dir = release_dir / hw_subdir
            if hw_dir.exists():
                print(f"\n{Colors.CYAN}{hw_v} Hardware: {Colors.BOLD}FirmwareReleases/{hw_subdir}/{Colors.RESET}")
                for file in sorted(hw_dir.iterdir()):
                    if file.is_file():
                        size_kb = file.stat().st_size / 1024
                        print(f"  {file.name} ({size_kb:.1f} KB)")
        
        # Show packages directory if node_app.bin was copied
        if (packages_dir / "node_app_16.bin").exists():
            print(f"\n{Colors.CYAN}OTA Package: {Colors.BOLD}packages/node_app_16.bin{Colors.RESET}")
            size_kb = (packages_dir / "node_app_16.bin").stat().st_size / 1024
            print(f"  node_app_16.bin ({size_kb:.1f} KB) - Ready for OTA deployment")

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
