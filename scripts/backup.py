#!/usr/bin/env python3
"""
Z1 Onyx Cluster Backup Script
Creates timestamped backup of the entire project directory.
"""

import os
import shutil
from datetime import datetime
from pathlib import Path

def create_backup():
    # Project root is parent of scripts directory
    project_root = Path(__file__).parent.parent
    project_name = project_root.name
    
    # Backup directory is sibling to project
    backup_base = project_root.parent / "Backups"
    backup_base.mkdir(exist_ok=True)
    
    # Timestamp format: Z1Onyx_Code_2025-01-15_14-30-45
    timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    backup_name = f"{project_name}_{timestamp}"
    backup_path = backup_base / backup_name
    
    print(f"Creating backup: {backup_name}")
    print(f"Source: {project_root}")
    print(f"Destination: {backup_path}")
    print()
    
    # Directories to exclude
    exclude_dirs = {
        'build',
        '.git',
        '__pycache__',
        '.vscode',
        'FirmwareReleases'  # Generated binaries
    }
    
    def ignore_patterns(dir, files):
        """Return files/directories to ignore during copy"""
        ignored = set()
        for name in files:
            # Skip excluded directories
            if name in exclude_dirs:
                ignored.add(name)
            # Skip common temporary files
            elif name.endswith(('.pyc', '.pyo', '.o', '.elf', '.bin', '.hex', '.dis', '.uf2')):
                ignored.add(name)
            # Skip editor backups
            elif name.endswith(('~', '.bak', '.swp')):
                ignored.add(name)
        return ignored
    
    try:
        # Copy project directory
        shutil.copytree(project_root, backup_path, 
                       ignore=ignore_patterns,
                       dirs_exist_ok=False)
        
        print(f"✓ Backup complete: {backup_path}")
        print()
        
        # Show what was backed up
        total_files = sum(1 for _ in backup_path.rglob('*') if _.is_file())
        total_size = sum(f.stat().st_size for f in backup_path.rglob('*') if f.is_file())
        
        print(f"Files backed up: {total_files}")
        print(f"Total size: {total_size / 1024 / 1024:.2f} MB")
        
        return True
        
    except Exception as e:
        print(f"✗ Backup failed: {e}")
        return False

if __name__ == "__main__":
    success = create_backup()
    exit(0 if success else 1)
