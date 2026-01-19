# Z1 Onyx - System Documentation Index

Welcome to the Z1 Onyx distributed spiking neural network cluster documentation!

---

## 🚀 Quick Start

New to the project? Start here:

1. **[README.md](README.md)** - Project overview, quick start guide, hardware specs
2. **[BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md)** - How to build and flash firmware (V1 and V2 hardware)
3. **[TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md)** - Deploy and test your first SNN topology

---

## 📚 Complete Documentation

### Core System Documentation

| Document | Purpose | When to Read |
|----------|---------|--------------|
| [README.md](../README.md) | Project overview, hardware specs, quick start | First-time users |
| [PREREQUISITES.md](../PREREQUISITES.md) | Complete list of tools, libraries, dependencies | Setting up build environment |
| [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) | Build system, firmware compilation, flashing | Compiling firmware from source |
| [PIN_REFERENCE.md](PIN_REFERENCE.md) | Complete GPIO pin assignments for V1/V2 hardware | Hardware debugging, modifications |

### Network and Communication

| Document | Purpose | When to Read |
|----------|---------|--------------|
| [API_REFERENCE.md](API_REFERENCE.md) | Complete HTTP API, endpoints, request/response formats | Developing client tools, debugging |
| [FILE_MANAGEMENT_GUIDE.md](FILE_MANAGEMENT_GUIDE.md) | Complete file upload/download/list/delete system | Managing topology files on SD card |
| [STREAMING_FILE_SYSTEM.md](STREAMING_FILE_SYSTEM.md) | Technical details of streaming implementation | Understanding internals, debugging uploads |

### SNN and OTA Updates

| Document | Purpose | When to Read |
|----------|---------|--------------|
| [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md) | Deploy and test SNN topologies | Running your first neural network |
| [DUAL_PARTITION_GUIDE.md](DUAL_PARTITION_GUIDE.md) | Dual-partition bootloader architecture | Understanding OTA firmware layout |
| [OTA_WORKFLOWS.md](OTA_WORKFLOWS.md) | Direct HTTP vs SD-based OTA workflows | Choosing OTA update strategy |
| [OTA_TROUBLESHOOTING_GUIDE.md](OTA_TROUBLESHOOTING_GUIDE.md) | Common OTA issues and solutions | Debugging OTA failures |
| [ASYNC_SPIKE_INJECTION.md](ASYNC_SPIKE_INJECTION.md) | Background job queue for spike processing | Understanding spike injection internals |
| [SNN_ENGINE_SPEC_AND_FIXES.md](SNN_ENGINE_SPEC_AND_FIXES.md) | SNN engine implementation details | Understanding neural network execution |

---

## 🎯 Common Tasks

### Building Firmware

```bash
# V2 hardware (16 nodes, default)
python build.py

# V1 hardware (12 nodes, legacy)
python build.py --hw-v1
```

**Output**: `FirmwareReleases/16node/` or `FirmwareReleases/12node/`

**Documentation**: [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md)

### Managing Network Configuration

```bash
# View current IP address
python python_tools/bin/zconfig read -c 192.168.1.222

# Change IP address
python python_tools/bin/zconfig write --ip 192.168.1.100 -c 192.168.1.222

# Reboot controller
python python_tools/bin/zconfig reboot -c 192.168.1.222
```

**Documentation**: [README.md](README.md#network-configuration-zconfig)

### Managing Files (Topologies, Engines)

```bash
# List all files
python python_tools/bin/zengine list -c 192.168.1.222

# Upload file
python python_tools/bin/zengine upload myfile.json -c 192.168.1.222

# Download file
python python_tools/bin/zengine download myfile.json -c 192.168.1.222

# Delete file
python python_tools/bin/zengine delete myfile.json -c 192.168.1.222
```

**Documentation**: [FILE_MANAGEMENT_GUIDE.md](FILE_MANAGEMENT_GUIDE.md)

### Deploying SNN Topologies

```bash
# List available nodes
python python_tools/bin/nls -c 192.168.1.222

# Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.222

# Start SNN
python python_tools/bin/nsnn start -c 192.168.1.222

# Inject spike
python python_tools/bin/nsnn inject spike_pattern.json -c 192.168.1.222

# Check status
python python_tools/bin/nstat -c 192.168.1.222
```

**Documentation**: [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md)

### OTA Firmware Updates

```bash
# Build firmware
python build.py

# Update single node (nflash tool)
python python_tools/bin/nflash build/node/node_app_16.bin -n 0 -c 192.168.1.222

# Update multiple nodes sequentially
python python_tools/bin/nflash build/node/node_app_16.bin -n 0,1,2 -c 192.168.1.222

# Update all 16 nodes
python python_tools/bin/nflash build/node/node_app_16.bin --all -c 192.168.1.222
```

**Documentation**: [OTA_TROUBLESHOOTING_GUIDE.md](OTA_TROUBLESHOOTING_GUIDE.md), [OTA_WORKFLOWS.md](OTA_WORKFLOWS.md)

---

## 🛠️ Python Tools Reference

| Tool | Purpose | Documentation |
|------|---------|---------------|
| `nls` | List all compute nodes | [API_REFERENCE.md](API_REFERENCE.md#nls---node-list) |
| `nstat` | Node statistics and monitoring | [API_REFERENCE.md](API_REFERENCE.md#nstat---node-statistics) |
| `nsnn` | Deploy/start/stop SNN topologies | [API_REFERENCE.md](API_REFERENCE.md#nsnn---snn-management) |
| `nping` | Network latency testing | [API_REFERENCE.md](API_REFERENCE.md#nping---node-ping) |
| `nreset` | **Software reset nodes/controller** | [API_REFERENCE.md](API_REFERENCE.md#nreset---node-reset) |
| `nflash` | **OTA firmware updates (direct HTTP)** | [OTA_WORKFLOWS.md](OTA_WORKFLOWS.md#method-1-direct-http-update-current) |
| `zconfig` | **Network configuration** | [README.md](../README.md#network-configuration-zconfig) |
| `zengine` | **File upload/download/list/delete** | [FILE_MANAGEMENT_GUIDE.md](FILE_MANAGEMENT_GUIDE.md) |

**Location**: `python_tools/bin/`

---

## 🔬 Testing and Validation

### Test Deployment Script

Comprehensive validation of deployed cluster:

```bash
python test_deployment.py -c 192.168.1.222
```

**Tests**:
1. Node discovery
2. Topology deployment
3. SNN start
4. Spike injection
5. Statistics collection
6. Status verification
7. Clean shutdown

**Documentation**: [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md)

### File System Testing

Test files included for validation (2KB to 1MB):

```bash
# Upload test files
python python_tools/bin/zengine upload python_tools/examples/testignore*.json

# Verify with list
python python_tools/bin/zengine list

# Download and verify SHA256
python python_tools/bin/zengine download testignore1024K.json
```

**Documentation**: [FILE_MANAGEMENT_GUIDE.md](FILE_MANAGEMENT_GUIDE.md#testing)

---

## 📦 Directory Structure

```
Z1Onyx/Code/
├── README.md                       # ⭐ Start here
├── BUILD_INSTRUCTIONS.md           # Build and flash guide
├── FILE_MANAGEMENT_GUIDE.md        # ⭐ NEW! Complete file system guide
├── STREAMING_FILE_SYSTEM.md        # Technical streaming details
├── API_REFERENCE.md                # HTTP API reference
├── OTA_UPDATE_SPEC.md              # OTA firmware update spec
├── TEST_DEPLOYMENT_GUIDE.md        # Testing guide
├── ARCHITECTURE.md                 # System architecture
├── PIN_REFERENCE.md                # GPIO pin assignments
│
├── embedded_firmware/
│   ├── controller/                 # Controller firmware source
│   │   ├── controller_main.c
│   │   ├── w5500_eth.c            # ⭐ Streaming file handlers
│   │   ├── z1_http_api.c          # ⭐ Directory listing, file delete
│   │   └── controller_pins*.h     # V1/V2 pin definitions
│   │
│   ├── node/                       # Compute node firmware source
│   │   ├── node_main.c
│   │   └── z1_snn_engine.c        # LIF neuron implementation
│   │
│   └── common/                     # Shared libraries
│       ├── sd_card/               # ⭐ FatFS, directory listing
│       ├── z1_broker/             # Matrix bus broker
│       └── z1_onyx_bus/           # Physical layer driver
│
├── python_tools/
│   ├── bin/
│   │   ├── nls                    # List nodes
│   │   ├── nsnn                   # SNN control
│   │   ├── zconfig                # Network config
│   │   ├── zengine                # ⭐ NEW! File manager
│   │   └── z1pack                 # ⭐ NEW! Engine packager
│   │
│   ├── lib/                       # Shared Python libraries
│   └── examples/                  # Test topologies and files
│
├── packages/
│   ├── xor_snn_v1.0.0.z1app       # ⭐ Test engine binary
│   └── xor_snn_v1.0.0/            # Test package documentation
│
├── FirmwareReleases/
│   ├── 16node/                    # V2 firmware (UF2 files)
│   └── 12node/                    # V1 firmware (UF2 files)
│
└── build/                         # Build output (ELF, BIN, etc.)
```

---

## 🎓 Learning Path

### For New Users

1. Read [README.md](README.md) - Understand what Z1 Onyx is
2. Follow [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) - Build your first firmware
3. Flash firmware and connect to network
4. Run [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md) - Deploy XOR topology
5. Explore [API_REFERENCE.md](API_REFERENCE.md) - Understand HTTP endpoints

### For Developers Adding Features

1. Read [ARCHITECTURE.md](ARCHITECTURE.md) - System design overview
2. Review [PIN_REFERENCE.md](PIN_REFERENCE.md) - Hardware details
3. Study relevant source files (controller/ or node/)
4. Test with existing tools (nls, nsnn, zengine)
5. Document changes in appropriate .md files

### For OTA/File System Work

1. Read [FILE_MANAGEMENT_GUIDE.md](FILE_MANAGEMENT_GUIDE.md) - Complete system overview
2. Review [STREAMING_FILE_SYSTEM.md](STREAMING_FILE_SYSTEM.md) - Implementation details
3. Study [OTA_UPDATE_SPEC.md](OTA_UPDATE_SPEC.md) - OTA architecture
4. Examine test package in `packages/xor_snn_v1.0.0/`
5. Review streaming code in `controller/w5500_eth.c`

---

## 🆕 Project Status

**Development Ready - Functional Prototype** ⚙️ (as of January 18, 2026)

All core features implemented and tested. Further optimization may be needed for production use.

- ✅ Multi-node spike propagation working
- ✅ HTTP API fully functional  
- ✅ **OTA firmware updates** via HTTP (`nflash` tool)
- ✅ **Dual-partition bootloader** (512KB + 7.5MB app)
- ✅ **Streaming file system** (1MB+ topology upload/download)
- ✅ **Async spike injection** (background job queue)
- ✅ **Runtime network config** (no rebuild for IP/MAC changes)
- ✅ 13+ Python CLI tools operational
- ✅ Hardware variants (V1/V2) supported and validated
- ✅ Automated testing suite (9/9 passing)
- ✅ Build system validated
- ✅ Zero CRC errors over 200000+ frames
- ✅ 100% bus reliability
- ✅ **USB flash utilities** (`flash_node.py`, `flash_controller.py`)

---

## 📞 Support and Contribution

### Reporting Issues

When reporting issues, include:
1. Hardware version (V1 or V2)
2. Firmware version (check OLED display or serial output)
3. Complete error message or serial log
4. Steps to reproduce

### Documentation Updates

All documentation is in Markdown format. When updating:
- Keep consistent formatting
- Update this index if adding new documents
- Include examples and code snippets
- Test all command examples before committing

---

## 🔗 External References

- **Pico SDK**: https://github.com/raspberrypi/pico-sdk
- **GitHub Repository**: https://github.com/texelec/neurofab-z1-cluster

---

**Last Updated**: January 18, 2026

