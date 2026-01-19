# Z1 Cluster Test Deployment Guide

**Last Updated:** January 18, 2026

## Overview

The `test_deployment.py` script provides a comprehensive automated test suite for validating the complete Z1 cluster deployment workflow. It tests all critical operations from node discovery through topology deployment, spike injection, monitoring, and graceful shutdown.

This guide covers how to use the test script, what it validates, and how to interpret results.

---

## Prerequisites

Before running the test deployment, ensure:

1. **Hardware Setup**
   - Z1 controller board powered and connected to network
   - At least 2 compute nodes connected and powered
   - Controller accessible via IP address (default: 192.168.1.222)

2. **Software Requirements**
   - Python 3.7 or later
   - Z1 Python tools installed (see `python_tools/COMPREHENSIVE_TOOLS_REFERENCE.md`)
   - Working network connection to controller

3. **Firmware**
   - Controller and nodes running compatible firmware versions
   - All nodes successfully booted and initialized
   - For OTA-capable firmware: Use dual-partition builds (`node_dual_16.uf2` or `node_dual_12_N.uf2`)

4. **Network Configuration**
   - Controller uses default IP: **192.168.1.222**
   - To change: Use `zconfig` tool (see [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md))
   - Or use `-c <IP>` flag to specify different controller IP

---

## Quick Start

### Basic Test (Default Configuration)

```bash
python test_deployment.py
```

This runs with defaults:
- Controller IP: 192.168.1.201 (matches firmware default)
- Topology: `python_tools/examples/xor_working.json`
- Spike count: 2500
- Verbose output enabled

### Custom Configuration

```bash
# Use different controller IP
python test_deployment.py -c 192.168.1.100

# Use custom topology file
python test_deployment.py -t python_tools/examples/mnist_snn.json

# Inject different spike count
python test_deployment.py -s 5000

# Quiet mode (hide detailed command output)
python test_deployment.py -q

# Combined options
python test_deployment.py -c 192.168.1.100 -t python_tools/examples/xor_simple.json -s 1000 -q
```

---

## Command-Line Options

| Option | Short | Description | Default |
|--------|-------|-------------|---------|
| `--controller` | `-c` | Controller IP address | 192.168.1.201 |
| `--topology` | `-t` | Path to topology JSON file | python_tools/examples/xor_working.json |
| `--spikes` | `-s` | Number of spikes to inject | 2500 |
| `--quiet` | `-q` | Quiet mode (suppress detailed output) | Verbose |

---

## Test Sequence

The script executes the following tests in order:

### 1. Node Discovery (`nls`)
- **Purpose**: Verify controller is reachable and detect available compute nodes
- **Command**: `python python_tools/bin/nls -c <controller_ip>`
- **Success Criteria**: At least 2 nodes detected and online
- **Critical**: Test aborts if this fails (cannot proceed without nodes)

### 2. Topology Deployment (`nsnn deploy`)
- **Purpose**: Deploy SNN topology configuration to cluster
- **Command**: `python python_tools/bin/nsnn deploy <topology_file> -c <controller_ip>`
- **Success Criteria**: Topology successfully uploaded and distributed
- **Critical**: Test aborts if this fails (cannot run SNN without topology)

### 3. Node Status Check (`nstat`)
- **Purpose**: Verify all nodes are ready and responsive
- **Command**: `python python_tools/bin/nstat -c <controller_ip>`
- **Success Criteria**: Status retrieved successfully
- **Non-Critical**: Test continues even if this fails

### 4. Start SNN (`nsnn start`)
- **Purpose**: Activate the spiking neural network
- **Command**: `python python_tools/bin/nsnn start -c <controller_ip>`
- **Success Criteria**: SNN engine starts successfully
- **Critical**: Test aborts if this fails (cannot inject spikes)

### 5. Inject Spikes (`nsnn inject`)
- **Purpose**: Feed input stimulus to the network
- **Command**: `python python_tools/bin/nsnn inject <spike_pattern.json> -c <controller_ip>`
- **Pattern**: Temporary JSON file with spike distribution across input neurons
- **Success Criteria**: Spikes accepted and queued for injection
- **Non-Critical**: Test continues even if this fails

**Default spike pattern:**
```json
{
  "spikes": [
    {"neuron_id": 0, "count": <spike_count/2>},
    {"neuron_id": 1, "count": <spike_count/2>}
  ]
}
```

### 6. SNN Statistics (`nstat -s`)
- **Purpose**: Retrieve network activity statistics
- **Command**: `python python_tools/bin/nstat -c <controller_ip> -s`
- **Output**: Displays per-node spike counts, processing rates, memory usage
- **Success Criteria**: Statistics retrieved and contain valid data
- **Non-Critical**: Test continues even if this fails

### 7. SNN Status (`nsnn status`)
- **Purpose**: Check current state of SNN engine
- **Command**: `python python_tools/bin/nsnn status -c <controller_ip>`
- **Success Criteria**: Status retrieved successfully
- **Non-Critical**: Test continues even if this fails

### 8. Stop SNN (`nsnn stop`)
- **Purpose**: Gracefully halt the spiking neural network
- **Command**: `python python_tools/bin/nsnn stop -c <controller_ip>`
- **Success Criteria**: SNN stopped cleanly
- **Non-Critical**: Test continues even if this fails

---

## Output Format

### Verbose Mode (Default)

```
=== Z1 Cluster Comprehensive Test ===

Controller IP: 192.168.1.201
Topology: python_tools/examples/xor_working.json
Spike count: 2500

Running: Node discovery (nls)...

=== Node Discovery ===
0   online  16384 KB  uptime: 0d 2h 15m
1   online  16384 KB  uptime: 0d 2h 15m
2   online  16384 KB  uptime: 0d 2h 15m
======================================================================

Running: Deploy topology (nsnn deploy)...

=== Topology Deployment ===
Deploying topology to 192.168.1.201...
✓ Topology deployed successfully
======================================================================

[... additional test output ...]

=== Test Summary ===

[OK] PASS - nls                  3 nodes
[OK] PASS - nsnn deploy          Deployed
[OK] PASS - nstat                OK
[OK] PASS - nsnn start           Running
[OK] PASS - nsnn inject          2500 spikes
[OK] PASS - nstat -s             Available
[OK] PASS - nsnn status          OK
[OK] PASS - nsnn stop            Stopped

======================================================================
[OK] ALL TESTS PASSED (8/8)
```

### Quiet Mode (`-q`)

Suppresses detailed command output, shows only test results:

```
=== Z1 Cluster Comprehensive Test ===

Controller IP: 192.168.1.201
Topology: python_tools/examples/xor_working.json
Spike count: 2500

Running: Node discovery (nls)...
Running: Deploy topology (nsnn deploy)...
Running: Node status (nstat)...
Running: Start SNN (nsnn start)...
Running: Inject 2500 spikes (nsnn inject)...
Running: SNN statistics (nstat -s)...
Running: SNN status (nsnn status)...
Running: Stop SNN (nsnn stop)...

=== Test Summary ===

[OK] PASS - nls                  3 nodes
[OK] PASS - nsnn deploy          Deployed
[OK] PASS - nstat                OK
[OK] PASS - nsnn start           Running
[OK] PASS - nsnn inject          2500 spikes
[OK] PASS - nstat -s             Available
[OK] PASS - nsnn status          OK
[OK] PASS - nsnn stop            Stopped

======================================================================
[OK] ALL TESTS PASSED (8/8)
```

---

## Exit Codes

The script returns different exit codes based on test results:

| Exit Code | Condition | Description |
|-----------|-----------|-------------|
| 0 | All tests passed | 100% success rate OR ≥70% success rate |
| 1 | Critical failure | Node discovery failed, deployment failed, or SNN start failed |
| 1 | Multiple failures | <70% success rate |

**Critical tests** (test aborts if these fail):
1. Node discovery (must find ≥2 nodes)
2. Topology deployment (must upload successfully)
3. Start SNN (must activate engine)

**Non-critical tests** (failures logged but test continues):
- Node status check
- Spike injection
- Statistics retrieval
- Status check
- Stop command

---

## Interpreting Results

### All Tests Passed
```
[OK] ALL TESTS PASSED (8/8)
```
- Full functionality confirmed
- Cluster ready for production workloads
- All communication paths working

### Partial Success
```
[SKIP] PARTIAL SUCCESS (6/8)
```
- Core functionality working (≥70% pass rate)
- Some optional features may have issues
- Review failed tests:
  - Statistics/monitoring failures: Non-critical, network still functional
  - Injection failures: Check spike pattern format and node capacity
  - Stop failures: Network may still be running (check with `nstat`)

### Multiple Failures
```
[FAIL] MULTIPLE FAILURES (4/8)
```
- Significant issues detected
- Check:
  1. Controller network connectivity
  2. Firmware versions on controller and nodes
  3. Node power and initialization
  4. Topology file format

### Critical Abort
```
[FAIL] ABORT - Cannot discover nodes
```
- Test cannot proceed
- Possible causes:
  - Controller offline or unreachable
  - Wrong IP address
  - Network configuration issues
  - No nodes powered or initialized

---

## Topology Files

The test uses topology JSON files from `python_tools/examples/`. Common options:

### XOR Networks
- `xor_working.json` - Default, simple XOR network (recommended for testing)
- `xor_simple.json` - Minimal XOR configuration
- `xor_snn.json` - XOR with STDP learning

### MNIST Networks
- `mnist_snn.json` - MNIST digit recognition network
- `mnist_stdp.json` - MNIST with spike-timing-dependent plasticity
- `mnist_optimized.json` - Performance-optimized MNIST

### Custom Topologies
Create custom topology files following this structure:

```json
{
  "neurons": [
    {
      "id": 0,
      "node_id": 0,
      "threshold": 100,
      "reset_potential": 0,
      "leak_rate": 5
    }
  ],
  "synapses": [
    {
      "pre_id": 0,
      "post_id": 1,
      "weight": 50,
      "delay_ms": 1
    }
  ]
}
```

See `API_REFERENCE.md` for complete topology format specification.

---

## Troubleshooting

### Test hangs or times out
- **Cause**: Controller not responding
- **Solution**: Verify controller IP, check network connectivity, confirm firmware is running

### "Only 0 nodes found"
- **Cause**: No compute nodes detected
- **Solution**: Check node power, verify node firmware is loaded, confirm Matrix bus connections

### Deployment fails
- **Cause**: Invalid topology format or resource limits exceeded
- **Solution**: Validate topology JSON, reduce network size if cluster is small, check controller logs

### Spike injection fails
- **Cause**: SNN not running or neuron IDs invalid
- **Solution**: Verify `nsnn start` succeeded, check neuron IDs in topology file match spike pattern

### Statistics show no activity
- **Cause**: Network not processing spikes (likely configuration issue)
- **Solution**: Verify topology has valid synaptic connections, check neuron thresholds, increase spike count

---

## Integration with CI/CD

The test script is designed for automated testing:

```bash
# Jenkins / GitHub Actions example
python test_deployment.py -c $CLUSTER_IP -t topologies/production.json -q
if [ $? -eq 0 ]; then
  echo "Deployment validation passed"
else
  echo "Deployment validation failed"
  exit 1
fi
```

---

## Related Documentation

- **Python Tools**: `python_tools/README.md` - CLI tool reference
- **API Reference**: `API_REFERENCE.md` - HTTP API and topology format
- **Build Guide**: `BUILD_INSTRUCTIONS.md` - Firmware building and network configuration

---

## Support

For issues or questions:
1. Check controller firmware logs (accessible via UART)
2. Review node status with `python python_tools/bin/nstat -c <ip> -v`
3. Verify network topology file format
4. Consult `API_REFERENCE.md` for protocol details

