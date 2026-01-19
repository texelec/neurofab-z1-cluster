#!/usr/bin/env python3
"""
Z1 Onyx Cluster - Dual Partition Build Script
Builds bootloader + application firmware for OTA-capable V2 (16-node) hardware
Uses tools from system PATH - no hardcoded paths
"""

import os
import sys
import subprocess
import shutil
from pathlib import Path

# Add build_tools to PATH immediately
PROJECT_ROOT = Path(__file__).parent
BUILD_TOOLS_PATH = str(PROJECT_ROOT / "build_tools")
os.environ['PATH'] = BUILD_TOOLS_PATH + os.pathsep + os.environ.get('PATH', '')

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
    print(f"  {Colors.GREEN}[OK] {msg}{Colors.RESET}")

def print_warning(msg):
    print(f"  {Colors.YELLOW}[WARN] {msg}{Colors.RESET}")

def print_error(msg):
    print(f"{Colors.RED}[ERROR] {msg}{Colors.RESET}")

def find_tool(name, required=False, check_build_tools=False):
    """Find tool in PATH or build_tools directory"""
    # First check build_tools directory if requested
    if check_build_tools:
        project_root = Path(__file__).parent
        build_tools_path = project_root / "build_tools" / f"{name}.exe"
        if build_tools_path.exists():
            print_success(f"Found {name}: {build_tools_path}")
            return str(build_tools_path)
    
    # Then check PATH
    tool = shutil.which(name)
    if tool:
        print_success(f"Found {name}: {tool}")
        return tool
    elif required:
        print_error(f"{name} not found in PATH or build_tools!")
        return None
    else:
        print_warning(f"{name} not found in PATH or build_tools")
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
    parser = argparse.ArgumentParser(description="Z1 Onyx Dual Partition Build Script")
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
    
    print_header(f"Z1 Onyx Dual Partition Build")
    if build_both:
        print(f"{Colors.CYAN}Building: V1 (12-node) + V2 (16-node){Colors.RESET}\n")
    else:
        print(f"{Colors.CYAN}Building: {hw_versions[0]}{Colors.RESET}\n")
    
    # Get project root
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir
    build_dir = project_root / "build"
    packages_dir = project_root / "packages"
    
    original_dir = os.getcwd()
    
    try:
        # Step 0: Check environment
        print_step(0, "Checking build environment")
        
        # Check PICO_SDK_PATH
        sdk_path = os.environ.get("PICO_SDK_PATH")
        if not sdk_path:
            raise RuntimeError("PICO_SDK_PATH environment variable not set!")
        print_success(f"PICO_SDK_PATH: {sdk_path}")
        
        # Check required tools
        cmake = find_tool("cmake", required=True)
        ninja = find_tool("ninja", required=True)
        python = find_tool("python3") or find_tool("python", required=True)
        
        if not all([cmake, ninja, python]):
            raise RuntimeError("Required build tools not found in PATH!")
        
        # Step 1: Check for pioasm
        print_step(1, "Checking for PIO headers")
        pioasm = find_tool("pioasm", check_build_tools=True)
        if not pioasm:
            print_warning("pioasm not found, using pre-generated headers")
        
        # Create directories
        build_dir.mkdir(exist_ok=True)
        packages_dir.mkdir(exist_ok=True)
        os.chdir(build_dir)
        
        # Build each hardware version
        for hw_version in hw_versions:
            is_v1 = (hw_version == "V1")
            
            print(f"\n{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}")
            print(f"{Colors.CYAN}{Colors.BOLD}Building {hw_version} Hardware{Colors.RESET}")
            print(f"{Colors.CYAN}{Colors.BOLD}{'='*70}{Colors.RESET}\n")
            
            release_dir = project_root / "FirmwareReleases" / ("12node" if is_v1 else "16node")
            release_dir.mkdir(parents=True, exist_ok=True)
        
            # Step 2: Configure CMake
            cmake_cache = build_dir / "CMakeCache.txt"
            needs_reconfigure = False
            
            if not (build_dir / "build.ninja").exists():
                needs_reconfigure = True
            elif cmake_cache.exists():
                # Check if hardware version matches
                cache_content = cmake_cache.read_text()
                current_v1 = "BUILD_HW_V1:BOOL=ON" in cache_content
                current_v2 = "BUILD_HW_V2:BOOL=ON" in cache_content
                if is_v1 and not current_v1:
                    needs_reconfigure = True
                    print_warning("Reconfiguring for V1 hardware...")
                elif not is_v1 and not current_v2:
                    needs_reconfigure = True
                    print_warning("Reconfiguring for V2 hardware...")
            
            if needs_reconfigure:
                print_step(2, f"Configuring build for {hw_version}")
                cmake_opts = [
                    cmake, "-G", "Ninja",
                    f"-DBUILD_HW_V1={'ON' if is_v1 else 'OFF'}",
                    f"-DBUILD_HW_V2={'OFF' if is_v1 else 'ON'}",
                    ".."
                ]
                if not run_command(cmake_opts):
                    raise RuntimeError(f"{hw_version} CMake configuration failed!")
                print_success(f"{hw_version} CMake configuration complete")
            else:
                print_step(2, f"Build already configured for {hw_version}")

            # Step 3: Build bootloader and app
            print_step(3, f"Building {hw_version} bootloader and application")
            
            if is_v1:
                # V1: bootloader_12 + node_app_12_0 through node_app_12_11
                targets = ["bootloader_12"] + [f"node_app_12_{i}" for i in range(12)]
            else:
                # V2: bootloader_16 + node_app_16
                targets = ["bootloader_16", "node_app_16"]
            
            if not run_command([ninja] + targets):
                raise RuntimeError(f"{hw_version} build failed!")

            print_success(f"{hw_version} build complete")
            
            # Step 4: Prepend app headers
            print_step(4, f"Adding {hw_version} app headers")
            
            header_script = project_root / "build_tools" / "prepend_app_header.py"
            
            if not header_script.exists():
                raise RuntimeError(f"Header script not found: {header_script}")
            
            if is_v1:
                # V1: Add headers to all 12 apps
                for node_id in range(12):
                    app_bin = build_dir / "node" / f"node_app_12_{node_id}.bin"
                    app_with_header = build_dir / "node" / f"node_app_12_{node_id}_header.bin"
                    
                    if not app_bin.exists():
                        raise RuntimeError(f"App binary not found: {app_bin}")
                    
                    # Prepend header
                    header_cmd = [python, str(header_script), str(app_bin), str(app_with_header), 
                                 f"Z1 Node {node_id}", "1.0.0"]
                    
                    if not run_command(header_cmd):
                        raise RuntimeError(f"Header prepend failed for node {node_id}!")
            else:
                # V2: Add header to single app
                app_bin = build_dir / "node" / "node_app_16.bin"
                app_with_header = build_dir / "node" / "node_app_16_header.bin"
                
                if not app_bin.exists():
                    raise RuntimeError(f"App binary not found: {app_bin}")
                
                # Prepend header
                header_cmd = [python, str(header_script), str(app_bin), str(app_with_header), 
                             "Z1 Node App", "1.0.0"]
                
                if not run_command(header_cmd):
                    raise RuntimeError("Header prepend failed!")
            
            print_success(f"{hw_version} app headers added")
            
            # Step 5: Merge into dual-partition UF2(s)
            print_step(5, f"Creating {hw_version} dual-partition UF2")
            
            merge_script = project_root / "build_tools" / "merge_dual_partition.py"
            
            if not merge_script.exists():
                raise RuntimeError(f"Merge script not found: {merge_script}")
            
            # Create apponly subdirectory
            apponly_dir = release_dir / "apponly"
            apponly_dir.mkdir(parents=True, exist_ok=True)
            
            if is_v1:
                # V1: Create 12 dual UF2 files (one per node) - go in root
                bootloader_uf2 = build_dir / "bootloader" / "bootloader_12.uf2"
                
                if not bootloader_uf2.exists():
                    raise RuntimeError(f"Bootloader UF2 not found: {bootloader_uf2}")
                
                print(f"\n{Colors.CYAN}Creating 12 dual-partition UF2 files for V1...{Colors.RESET}")
                
                for node_id in range(12):
                    app_bin = build_dir / "node" / f"node_app_12_{node_id}_header.bin"
                    output_uf2 = release_dir / f"node_dual_12_{node_id}.uf2"  # Root directory
                    
                    if not app_bin.exists():
                        raise RuntimeError(f"App binary not found: {app_bin}")
                    
                    # Merge bootloader UF2 + app
                    merge_cmd = [python, str(merge_script), str(bootloader_uf2), str(app_bin), str(output_uf2)]
                    
                    if not run_command(merge_cmd):
                        raise RuntimeError(f"Merge script failed for node {node_id}!")
                    
                    print_success(f"node_dual_12_{node_id}.uf2 → root")
            else:
                # V2: Create single dual UF2 - goes in root
                bootloader_uf2 = build_dir / "bootloader" / "bootloader_16.uf2"
                app_bin = build_dir / "node" / "node_app_16_header.bin"
                output_uf2 = release_dir / "node_dual_16.uf2"  # Root directory
                
                if not bootloader_uf2.exists():
                    raise RuntimeError(f"Bootloader UF2 not found: {bootloader_uf2}")
                
                if not app_bin.exists():
                    raise RuntimeError(f"App binary with header not found: {app_bin}")
                
                # Run merge script
                merge_cmd = [python, str(merge_script), str(bootloader_uf2), str(app_bin), str(output_uf2)]
                
                if not run_command(merge_cmd):
                    raise RuntimeError("Merge script failed!")
                
                print_success(f"node_dual_16.uf2 → root")
            
            # Step 6: Copy individual files to apponly/
            print_step(6, f"Copying {hw_version} individual binaries to apponly/")
            
            if is_v1:
                # Copy V1 bootloader to apponly
                bootloader_uf2 = build_dir / "bootloader" / "bootloader_12.uf2"
                if bootloader_uf2.exists():
                    shutil.copy2(bootloader_uf2, apponly_dir / "bootloader_12.uf2")
                    size_kb = bootloader_uf2.stat().st_size / 1024
                    print_success(f"bootloader_12.uf2 ({size_kb:.1f} KB) → apponly/")
                
                # Copy V1 app UF2s to apponly
                for node_id in range(12):
                    app_uf2 = build_dir / "node" / f"node_app_12_{node_id}.uf2"
                    if app_uf2.exists():
                        shutil.copy2(app_uf2, apponly_dir / f"node_app_12_{node_id}.uf2")
                
                print_success(f"node_app_12_0.uf2 through node_app_12_11.uf2 (12 files) → apponly/")
                
                # Copy V1 app binaries to packages/ for OTA
                for node_id in range(12):
                    app_bin = build_dir / "node" / f"node_app_12_{node_id}.bin"
                    if app_bin.exists():
                        app_dest = packages_dir / f"node_app_12_{node_id}.bin"
                        shutil.copy2(app_bin, app_dest)
                
                print_success(f"node_app_12_0.bin through node_app_12_11.bin (12 files) → packages/")
            else:
                # Copy V2 bootloader to apponly
                bootloader_uf2 = build_dir / "bootloader" / "bootloader_16.uf2"
                if bootloader_uf2.exists():
                    shutil.copy2(bootloader_uf2, apponly_dir / "bootloader_16.uf2")
                    size_kb = bootloader_uf2.stat().st_size / 1024
                    print_success(f"bootloader_16.uf2 ({size_kb:.1f} KB) → apponly/")
                
                # Copy V2 app UF2 to apponly
                app_uf2 = build_dir / "node" / "node_app_16.uf2"
                if app_uf2.exists():
                    shutil.copy2(app_uf2, apponly_dir / "node_app_16.uf2")
                    size_kb = app_uf2.stat().st_size / 1024
                    print_success(f"node_app_16.uf2 ({size_kb:.1f} KB) → apponly/")
                
                # Copy V2 app binary to packages/ for OTA
                app_bin = build_dir / "node" / "node_app_16.bin"
                if app_bin.exists():
                    app_dest = packages_dir / "node_app_16.bin"
                    shutil.copy2(app_bin, app_dest)
                    size_kb = app_bin.stat().st_size / 1024
                    print_success(f"node_app_16.bin ({size_kb:.1f} KB) → packages/")

        # Success!
        print_header("Dual Partition Build Complete!")
        
        print(f"\n{Colors.GREEN}Firmware ready:{Colors.RESET}")
        for hw_v in hw_versions:
            hw_subdir = "12node" if hw_v == "V1" else "16node"
            release_dir = project_root / "FirmwareReleases" / hw_subdir
            apponly_dir = release_dir / "apponly"
            
            if release_dir.exists():
                print(f"\n{Colors.CYAN}{hw_v} Hardware: {Colors.BOLD}FirmwareReleases/{hw_subdir}/{Colors.RESET}")
                
                # Root directory files (most commonly used)
                print(f"{Colors.CYAN}Root (commonly used):{Colors.RESET}")
                for file in sorted(release_dir.iterdir()):
                    if file.is_file() and file.suffix == ".uf2":
                        size_kb = file.stat().st_size / 1024
                        if "dual" in file.name:
                            print(f"  {Colors.GREEN}{Colors.BOLD}{file.name}{Colors.RESET} ({size_kb:.1f} KB) {Colors.YELLOW}<-- FLASH THIS{Colors.RESET}")
                        elif "controller" in file.name:
                            print(f"  {Colors.GREEN}{file.name}{Colors.RESET} ({size_kb:.1f} KB)")
                
                # apponly subdirectory (advanced/recovery)
                if apponly_dir.exists():
                    print(f"\n{Colors.CYAN}apponly/ (advanced/recovery):{Colors.RESET}")
                    apponly_files = list(apponly_dir.glob("*.uf2"))
                    if apponly_files:
                        for file in sorted(apponly_files)[:3]:  # Show first 3
                            size_kb = file.stat().st_size / 1024
                            print(f"  {file.name} ({size_kb:.1f} KB)")
                        if len(apponly_files) > 3:
                            print(f"  ... and {len(apponly_files) - 3} more files")
        
        # Show packages directory
        print(f"\n{Colors.CYAN}OTA Packages: {Colors.BOLD}packages/{Colors.RESET}")
        package_files = list(packages_dir.glob("node_app_*.bin"))
        if package_files:
            # Show first few and total count
            for pkg_file in sorted(package_files)[:3]:
                size_kb = pkg_file.stat().st_size / 1024
                print(f"  {pkg_file.name} ({size_kb:.1f} KB) - Ready for OTA deployment")
            if len(package_files) > 3:
                print(f"  ... and {len(package_files) - 3} more files")
        else:
            print(f"  (No package files found)")
        
        print(f"\n{Colors.CYAN}To flash:{Colors.RESET}")
        if "V1" in hw_versions:
            print(f"  V1: Drag node_dual_12_N.uf2 to BOOTSEL drive (N = node ID 0-11)")
        if "V2" in hw_versions:
            print(f"  V2: Drag node_dual_16.uf2 to BOOTSEL drive")

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
