#!/usr/bin/env python3
"""
Extract specific register values from BAR memory dumps.
Usage: python3 extract_bar_values.py bar1_windows.bin bar2_windows.bin
"""

import sys
import struct

def read_u32(data, offset):
    """Read a 32-bit value at offset (little-endian)"""
    if offset + 4 > len(data):
        return None
    return struct.unpack('<I', data[offset:offset+4])[0]

def dump_bar_registers(bar_data, bar_num, offsets):
    """Dump register values at specified offsets"""
    print(f"\n{'='*60}")
    print(f"BAR{bar_num} Register Values")
    print(f"{'='*60}")

    for offset, comment in offsets:
        value = read_u32(bar_data, offset)
        if value is not None:
            print(f"  BAR{bar_num} + 0x{offset:04X} = 0x{value:08X}  # {comment}")
        else:
            print(f"  BAR{bar_num} + 0x{offset:04X} = <out of range>  # {comment}")

def main():
    if len(sys.argv) != 3:
        print("Usage: python3 extract_bar_values.py bar1_windows.bin bar2_windows.bin")
        print("  (BAR1 corresponds to BAR0 in Linux, BAR2 to BAR1)")
        sys.exit(1)

    bar1_file = sys.argv[1]
    bar2_file = sys.argv[2]

    # Read BAR dumps
    try:
        with open(bar1_file, 'rb') as f:
            bar1_data = f.read()
        print(f"Read {len(bar1_data)} bytes from {bar1_file}")
    except Exception as e:
        print(f"Error reading {bar1_file}: {e}")
        sys.exit(1)

    try:
        with open(bar2_file, 'rb') as f:
            bar2_data = f.read()
        print(f"Read {len(bar2_data)} bytes from {bar2_file}")
    except Exception as e:
        print(f"Error reading {bar2_file}: {e}")
        sys.exit(1)

    # BAR1 (Linux BAR0) - 1MB
    bar1_offsets = [
        # Chip identification
        (0x0000, "Chip ID (SC0710 expects 0x10ee7021)"),
        (0x0004, "Version field"),
        (0x0008, "Firmware date"),

        # Video capture control
        (0x00A8, "Runtime status (height<<16 when streaming)"),
        (0x00AC, "Runtime status 2"),
        (0x00C4, "Fixed control (0x000f0000)"),
        (0x00C8, "Input dimensions (width<<16 | height)"),
        (0x00D0, "DMA control (0x4100 idle, 0x4101 streaming)"),
        (0x00D4, "Runtime counter"),
        (0x00D8, "Output height (scaler target)"),
        (0x00DC, "Unknown"),
        (0x00E4, "Streaming enable (1=active, 0=idle)"),

        # I2C controller
        (0x3100, "I2C control"),
        (0x3104, "I2C bus status"),
        (0x3108, "I2C TX FIFO"),
        (0x310C, "I2C RX FIFO"),
        (0x3120, "I2C interrupt enable"),
    ]

    # BAR2 (Linux BAR1) - 64KB - DMA engine registers
    bar2_offsets = [
        # DMA channel 0 (video) - base 0x1000
        (0x1000, "CH0 DMA identifier/control"),
        (0x1004, "CH0 DMA control"),
        (0x1008, "CH0 DMA control W1S"),
        (0x100C, "CH0 DMA control W1C"),
        (0x1040, "CH0 DMA status 1"),
        (0x1044, "CH0 DMA status 2"),
        (0x1048, "CH0 DMA completed descriptor count"),
        (0x1080, "CH0 DMA poll write-back addr low"),
        (0x1084, "CH0 DMA poll write-back addr high"),
        (0x1088, "CH0 DMA poll interval"),
        (0x108C, "CH0 DMA poll mode"),
        (0x1094, "CH0 DMA control (card setup)"),

        # SG controller for channel 0 - base 0x5000
        (0x5080, "CH0 SG start address low"),
        (0x5084, "CH0 SG start address high"),
        (0x5088, "CH0 SG adj"),
        (0x508C, "CH0 SG credits"),

        # DMA channel 1 (audio) - base 0x1100
        (0x1100, "CH1 DMA identifier/control"),
        (0x1104, "CH1 DMA control"),
        (0x1108, "CH1 DMA control W1S"),
        (0x110C, "CH1 DMA control W1C"),
        (0x1140, "CH1 DMA status 1"),
        (0x1144, "CH1 DMA status 2"),
        (0x1148, "CH1 DMA completed descriptor count"),
        (0x1194, "CH1 DMA control (card setup)"),

        # SG controller for channel 1 - base 0x5100
        (0x5180, "CH1 SG start address low"),
        (0x5184, "CH1 SG start address high"),
        (0x5188, "CH1 SG adj"),
        (0x518C, "CH1 SG credits"),

        # Additional channel descriptors (0x0000, 0x0100 base)
        (0x0000, "AXI channel 0 base"),
        (0x0008, "AXI channel 0 W1S (card setup)"),
        (0x0094, "AXI channel 0 reg (card setup)"),
        (0x0100, "AXI channel 1 base"),
        (0x0108, "AXI channel 1 W1S (card setup)"),
        (0x0194, "AXI channel 1 reg (card setup)"),

        # Unknown block
        (0x2080, "Unknown (driver sets to 0)"),
        (0x2084, "Unknown (driver sets to 0)"),
        (0x2088, "Unknown (driver sets to 0)"),
        (0x208C, "Unknown (driver sets to 0)"),
        (0x20A0, "Unknown (driver sets to 0)"),
        (0x20A4, "Unknown (driver sets to 0)"),
    ]

    dump_bar_registers(bar1_data, 1, bar1_offsets)
    dump_bar_registers(bar2_data, 2, bar2_offsets)

    # Summary section
    print(f"\n{'='*60}")
    print("Key Findings Summary")
    print(f"{'='*60}")

    chip_id = read_u32(bar1_data, 0x0000)
    if chip_id is not None:
        if chip_id == 0x10ee7021:
            print(f"  Chip ID: 0x{chip_id:08X} = SC0710 (as expected)")
        else:
            print(f"  Chip ID: 0x{chip_id:08X} = UNKNOWN CHIP (expected 0x10ee7021 for SC0710!)")

    ch0_desc_count = read_u32(bar2_data, 0x1048)
    if ch0_desc_count is not None:
        print(f"  CH0 descriptor count: {ch0_desc_count} ({'active' if ch0_desc_count > 0 else 'IDLE - DMA not running'})")

    streaming = read_u32(bar1_data, 0x00E4)
    if streaming is not None:
        print(f"  Streaming enable (0x00E4): 0x{streaming:08X} ({'STREAMING' if streaming else 'IDLE'})")

    c8 = read_u32(bar1_data, 0x00C8)
    if c8 is not None:
        width  = (c8 >> 16) & 0xFFFF
        height =  c8        & 0xFFFF
        print(f"  Capture dimensions (0x00C8): 0x{c8:08X} = {width}x{height}")

    print(f"\n{'='*60}")
    print("Driver card_setup() values to verify:")
    print(f"{'='*60}")
    val_c4 = read_u32(bar1_data, 0x00C4)
    if val_c4 is not None:
        print(f"sc_write(dev, 0, BAR0_00C4, 0x{val_c4:08X}); /* Windows: 0x{val_c4:08X}, Driver: 0x000f0000 */")
    for offset, _ in [(0x0094, ''), (0x0008, ''), (0x0194, ''), (0x0108, ''),
                      (0x1094, ''), (0x1008, ''), (0x1194, ''), (0x1108, '')]:
        val = read_u32(bar2_data, offset)
        if val is not None:
            print(f"sc_write(dev, 1, BAR1_{offset:04X}, 0x{val:08X});")

if __name__ == '__main__':
    main()
