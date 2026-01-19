/**
 * Z1 Onyx Bus - Pin Definitions
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Source-Synchronous Multi-Master Bus Protocol
 * RP2350 (Pico 2) - 26 pins via PCIe x4 connector
 * 
 * Protocol Overview:
 * - Source-synchronous: TX drives CLK, RX samples on rising edge
 * - Deterministic arbitration: Max-ID tournament on SELECT lines
 * - Credit-based flow control with sliding windows
 * - CRC16 validation, sequence numbers
 * - PIO state machines for bit-accurate timing
 * 
 * Pin Usage:
 * - GPIO 6: BUS_CLK (floating on backplane, controller has pull-up when idle)
 * - GPIO 7-11: BUS_SELECT[4:0] (floating on backplane, controller has pull-downs)
 * - GPIO 12-27: BUS_DATA[15:0] (floating on backplane, controller has pull-ups when idle)
 * - GPIO 2-3: UNUSED (330Ω pull-ups on backplane, slow rise time, not suitable for high-speed)
 * - GPIO 4-5: UNUSED (floating on backplane, reserved for future use)
 * 
 * Backplane Pull Configuration:
 * - GPIO 2-3: 330Ω pull-ups (hardware limitation, slow for 10 MHz signaling)
 * - GPIO 4-27: Floating (controller must provide pull-ups/pull-downs as needed)
 */

#ifndef Z1_ONYX_BUS_PINS_H
#define Z1_ONYX_BUS_PINS_H

// ============================================================================
// Bus Clock (Source-Synchronous)
// ============================================================================
#define BUS_CLK_PIN    6    // Driven by active transmitter
                            // CONTROLLER ONLY: Weak pullup when idle (keeps high)
                            // NODES: Never enable pullup on this pin

// ============================================================================
// Bus Select Lines (GPIO 7-11) - Control Signals
// ============================================================================
// 
// CURRENT USAGE (Phase 3a - December 2025):
// -----------------------------------------
// SELECT0 (GPIO 7): Carrier sense (CSMA/CD collision avoidance)
//   - TX drives HIGH during entire frame transmission (~31μs)
//   - RX checks LOW before transmitting (bus idle indicator)
//   - Requires 5μs discharge delay after TX (bus capacitance settle)
//   - Controller provides pull-down when idle, nodes float
//
// SELECT1-4 (GPIO 8-11): INITIALIZED BUT NOT USED
//   - Configured as inputs with pull-downs
//   - Never driven by TX, never read by RX
//   - Reserved for future protocol enhancements
//
// FUTURE OPTIMIZATION (Phase 3c - SOP/EOP Hardware Sync):
// --------------------------------------------------------
// Current bottleneck: DISCARD path processes rejected frames in software
//   - Memory barrier optimization: 25.7μs per rejected frame (Dec 9, 2025)
//   - At 30K spikes/sec: 28K rejected × 25.7μs = 720ms/sec = 72% CPU
//   - Plus accepted frames: ~88% total CPU (still within single-core budget)
//   - Limitation: Cannot scale beyond 30K spikes/sec on current architecture
//
// SELECT1 (GPIO 8): Start-of-Packet (SOP) marker - PROPOSED
//   Implementation: C-controlled GPIO pulse (NO TX PIO changes)
//     gpio_put(BUS_SELECT1_PIN, 1);  // Pulse HIGH
//     busy_wait_us_32(0);            // 1 PIO clock min
//     gpio_put(BUS_SELECT1_PIN, 0);  // Pulse LOW
//   RX PIO: Beat-counter with SOP wait (simple, low-risk change)
//     .wrap_target
//       pull block         ; Get beat count (259)
//       mov x, osr         ; X = beats remaining
//       wait 1 gpio 8      ; Wait for SOP
//     loop:
//       wait 0 gpio 6      ; CLK falling
//       wait 1 gpio 6 [3]  ; CLK rising + setup
//       in pins, 16        ; Sample beat
//       jmp x-- loop       ; Auto-stop after 259 beats
//     .wrap                ; Back to waiting for next SOP
//   Benefits:
//     - Hardware frame sync (PIO auto-aligns to SOP, no software parsing)
//     - DISCARD elimination (PIO skips to next SOP in hardware, 0 CPU)
//     - Guaranteed alignment (no DMA race, no memory barrier needed)
//   Risk: LOW (no TX PIO changes, minimal RX PIO modification)
//
// SELECT2 (GPIO 9): End-of-Packet (EOP) marker - FUTURE
//   Implementation: gpio_put(BUS_SELECT2_PIN, 1/0) after DMA complete
//   Purpose: Frame timeout/recovery (only needed if desync issues observed)
//   Priority: DEFER until SNN testing shows need
//
// Performance Impact (if SOP implemented):
//   - DISCARD speed: 25.7μs → ~0.4μs (640× faster, PIO auto-skips)
//   - CPU @ 30K spikes/sec: 160% → 88% (within single-core budget)
//   - Scalability: Enables 100K+ spikes/sec (limited by DMA bandwidth)
//   - Reliability: Hardware sync (no race conditions, no timing-sensitive code)
//
// Implementation Recommendation:
//   1. TEST with real SNN workload FIRST (may be <1K spikes/sec)
//   2. If CPU >80% in real usage → implement SOP (Phase 3c-1)
//   3. If desync issues observed → add EOP (Phase 3c-2)
//   "Fast is nice, reliable is REQUIRED" - don't optimize prematurely
//
// See: docs/SOP_EOP_IMPLEMENTATION_ANALYSIS.md for full architecture walkthrough
//
#define BUS_SELECT0_PIN 7   // ACTIVE: Carrier sense (held HIGH during TX)
#define BUS_SELECT1_PIN 8   // UNUSED: Reserved for SOP marker (future)
#define BUS_SELECT2_PIN 9   // UNUSED: Reserved for EOP marker (future)
#define BUS_SELECT3_PIN 10  // UNUSED: Reserved
#define BUS_SELECT4_PIN 11  // UNUSED: Reserved

// ============================================================================
// Bus Data Lines (GPIO 12-27) - 16-bit Parallel
// ============================================================================
// CONTROLLER ONLY: Weak pullups when idle
// NODES: Never enable pullups on these pins
#define BUS_DATA0_PIN  12   // Data bit 0 (LSB)
#define BUS_DATA1_PIN  13
#define BUS_DATA2_PIN  14
#define BUS_DATA3_PIN  15
#define BUS_DATA4_PIN  16
#define BUS_DATA5_PIN  17
#define BUS_DATA6_PIN  18
#define BUS_DATA7_PIN  19
#define BUS_DATA8_PIN  20
#define BUS_DATA9_PIN  21
#define BUS_DATA10_PIN 22
#define BUS_DATA11_PIN 23
#define BUS_DATA12_PIN 24
#define BUS_DATA13_PIN 25
#define BUS_DATA14_PIN 26
#define BUS_DATA15_PIN 27   // Data bit 15 (MSB)

// Data line base pin for PIO calculations
#define BUS_DATA_BASE_PIN  12
#define BUS_DATA_PIN_COUNT 16

// ============================================================================
// Unused Pins (Reserved)
// ============================================================================
// GPIO 2-3: BUS_ATTN/ACK - DO NOT USE (slow 330Ω pullups on backplane)
// GPIO 4-5: BUS_WR/RD - Reserved for future out-of-band signals
#define BUS_ATTN_PIN_UNUSED  2  // Slow pullup - avoid
#define BUS_ACK_PIN_UNUSED   3  // Slow pullup - avoid
#define BUS_WR_PIN_UNUSED    4  // Reserved
#define BUS_RD_PIN_UNUSED    5  // Reserved

// ============================================================================
// Frame Format Constants (Protocol Spec Compliant)
// ============================================================================
// Beat 0 (SOP=1): BUS_DATA format (Phase 2 with sender ID)
//   Bits 15:14 = Type (2 bits: unicast/broadcast/ack/ctrl)
//   Bits 13:9  = Src (5 bits: sender node ID 0-31)
//   Bits 8:4   = Dest (5 bits: destination 0-31, broadcast=31)
//   Bits 3:0   = StreamID (4 bits: logical channel 0-15)
#define FRAME_TYPE_UNICAST   0  // Point-to-point with ACK
#define FRAME_TYPE_BROADCAST 1  // Fire-and-forget to all
#define FRAME_TYPE_ACK       2  // Acknowledgement frame
#define FRAME_TYPE_CTRL      3  // Control/credit frames

// Beat 1: Length in bytes (payload size)
// Beat 2..N-1: Payload data
// Beat N: CRC16 trailer (Phase 2)
// Final beat has EOP=1 on SELECT[1] (Phase 2)
// Final beat has EOP=1 on SELECT[1] (Phase 2)

// Destination addresses
#define DEST_CONTROLLER      16 // Controller node (not used in Dest field, only 0-15 fit)
#define DEST_BROADCAST       15 // All nodes (use Type=BROADCAST)
// Dest 0-15: Worker nodes (4-bit field)

// ============================================================================
// Node ID Allocation
// ============================================================================
#define Z1_CONTROLLER_NODE_ID  16  // Controller (max ID for arbitration priority)
// Worker nodes: 0-15 (auto-detected from GPIO 40-44 on node hardware)

// ============================================================================
// Protocol Timing Parameters (Initial Conservative Values)
// ============================================================================
#define BUS_CLK_FREQ_HZ      8000000   // 8 MHz initial bus clock
#define BUS_IDLE_TIME_US     1         // 1μs idle = 8 bit times @ 8MHz
#define BUS_ARBITRATION_SLOTS 32       // Tournament slot count
#define BUS_WINDOW_SIZE_BYTES 2048     // 2KB default window (128 beats)
#define BUS_RX_FIFO_DEPTH     4096     // 4KB RX buffer per node

#endif // Z1_ONYX_BUS_PINS_H
