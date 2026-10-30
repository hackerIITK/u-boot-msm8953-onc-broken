#!/usr/bin/env python3
"""
Qualcomm Sahara Switch-to-EDL Tool
Directly executes the Sahara Mode-Switch packet (0c 00 00 00 0c 00 00 00 00 00 00 00)
to switch target from 900E (Memory Debug) to 9008 (EDL Image Transfer Mode).
"""

import sys
import time
import os
import struct
import subprocess

try:
    import serial
except ImportError:
    print("[-] pyserial not found! Run: pip install pyserial")
    sys.exit(1)

# Sahara Command IDs
SAHARA_CMD_HELLO               = 0x01
SAHARA_CMD_HELLO_RESP          = 0x02
SAHARA_CMD_READ_DATA           = 0x03
SAHARA_CMD_END_IMAGE_TX        = 0x04
SAHARA_CMD_RESET               = 0x07
SAHARA_CMD_RESET_RESP          = 0x08
SAHARA_CMD_CMD_READY           = 0x0B
SAHARA_CMD_SWITCH_MODE         = 0x0C

# Sahara Modes
SAHARA_MODE_IMAGE_TX_PENDING   = 0x00  # 9008 EDL Flash mode
SAHARA_MODE_MEMORY_DEBUG       = 0x02  # 900E RAM Dump mode
SAHARA_MODE_COMMAND            = 0x03  # Command mode

CMD_NAMES = {
    0x01: "SAHARA_CMD_HELLO",
    0x02: "SAHARA_CMD_HELLO_RESPONSE",
    0x03: "SAHARA_CMD_READ_DATA",
    0x04: "SAHARA_CMD_END_IMAGE_TX",
    0x07: "SAHARA_CMD_RESET",
    0x08: "SAHARA_CMD_RESET_RESP",
    0x0B: "SAHARA_CMD_COMMAND_READY",
    0x0C: "SAHARA_CMD_SWITCH_MODE",
}

def check_lsusb():
    try:
        out = subprocess.check_output(["lsusb"], stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            if "05c6:9008" in line:
                return "05c6:9008 (EDL Mode - SUCCESS!)"
            if "05c6:900e" in line:
                return "05c6:900e (Diagnostics / 900E)"
    except Exception:
        pass
    return "Disconnected"

def read_packet(ser, timeout=2.5):
    start = time.time()
    buf = bytearray()
    try:
        while time.time() - start < timeout:
            if ser.in_waiting > 0:
                buf.extend(ser.read(ser.in_waiting))
                if len(buf) >= 8:
                    cmd, length = struct.unpack("<2I", buf[:8])
                    if length > 4096 or length < 8:
                        return bytes(buf)
                    while len(buf) < length and (time.time() - start < timeout):
                        if ser.in_waiting > 0:
                            buf.extend(ser.read(ser.in_waiting))
                        time.sleep(0.005)
                    return bytes(buf[:length])
            time.sleep(0.01)
    except (OSError, serial.SerialException):
        return None
    return bytes(buf) if buf else None

def print_packet(pkt, direction="RX"):
    if not pkt or len(pkt) < 8:
        print(f"[{direction}] {pkt.hex(' ') if pkt else 'None'}")
        return
    cmd, length = struct.unpack("<2I", pkt[:8])
    cname = CMD_NAMES.get(cmd, f"0x{cmd:02X}")
    print(f"[{direction}] {cname} ({length} bytes) -> {pkt[:min(length, 48)].hex(' ')}")
    if cmd == SAHARA_CMD_HELLO and len(pkt) >= 24:
        cmd, length, ver, ver_supp, status, mode = struct.unpack("<6I", pkt[:24])
        mode_str = "IMAGE_TX (EDL 9008!)" if mode == 0 else ("COMMAND_MODE (0x03)" if mode == 3 else f"0x{mode:02X}")
        print(f"     Target Mode: {mode_str}")

def wait_for_port(port, timeout=30):
    if os.path.exists(port):
        return True
    print(f"[*] Waiting for {port} to appear...")
    start = time.time()
    while time.time() - start < timeout:
        if os.path.exists(port):
            time.sleep(0.15)
            print(f"[+] Detected {port}!")
            return True
        time.sleep(0.1)
    return False

def main():
    port = "/dev/ttyUSB0" if len(sys.argv) < 2 else sys.argv[1]
    
    if not wait_for_port(port, timeout=30):
        print(f"[-] Port {port} not found! Unplug USB, hold Power 20s, and replug.")
        sys.exit(1)

    print("=" * 65)
    print(" Qualcomm Sahara Direct EDL Mode Switcher")
    print(f" Port: {port} | Current USB: {check_lsusb()}")
    print("=" * 65)

    try:
        ser = serial.Serial(
            port=port,
            baudrate=115200,
            timeout=1.0,
            rtscts=False,
            dsrdtr=False
        )
    except Exception as e:
        print(f"[-] Error opening {port}: {e}")
        print("    Run with: sudo python3 sahara_switch_to_edl.py")
        sys.exit(1)

    # 1. Read incoming HELLO
    print("[*] Waiting for incoming SAHARA_CMD_HELLO...")
    hello = read_packet(ser, timeout=3.0)
    if not hello or len(hello) < 8:
        print("[*] Probing target...")
        ser.write(b"\x00")
        hello = read_packet(ser, timeout=2.0)

    if not hello or len(hello) < 24:
        print("[-] Did not receive valid HELLO packet.")
        ser.close()
        return

    print_packet(hello, "RX")

    # 2. Enter Command Mode (Mode 0x03)
    print("\n[Step 1] Responding with HELLO_RESP (Mode 0x03: Command Mode)...")
    hello_resp_mode3 = struct.pack("<12I", 
        SAHARA_CMD_HELLO_RESP, 0x30, 0x02, 0x01, 0x00,
        SAHARA_MODE_COMMAND, 0, 0, 0, 0, 0, 0
    )
    print_packet(hello_resp_mode3, "TX")
    ser.write(hello_resp_mode3)
    ser.flush()

    cmd_ready = read_packet(ser, timeout=2.0)
    if not cmd_ready:
        print("[-] Target did not send COMMAND_READY.")
        ser.close()
        return

    print_packet(cmd_ready, "RX")
    r_cmd = struct.unpack("<I", cmd_ready[:4])[0]
    if r_cmd != SAHARA_CMD_CMD_READY:
        print(f"[-] Expected SAHARA_CMD_COMMAND_READY (0x0B), got 0x{r_cmd:02X}")
        ser.close()
        return

    # 3. Send SWITCH_MODE to Mode 0x00 (IMAGE_TX_PENDING / EDL 9008)
    print("\n[Step 2] Sending SAHARA_CMD_SWITCH_MODE to Mode 0x00 (EDL 9008)...")
    # 0c 00 00 00 0c 00 00 00 00 00 00 00
    switch_to_edl = struct.pack("<3I", SAHARA_CMD_SWITCH_MODE, 0x0C, SAHARA_MODE_IMAGE_TX_PENDING)
    print_packet(switch_to_edl, "TX")
    ser.write(switch_to_edl)
    ser.flush()

    new_hello = read_packet(ser, timeout=3.0)
    if new_hello:
        print_packet(new_hello, "RX")
        if len(new_hello) >= 24:
            new_mode = struct.unpack("<I", new_hello[20:24])[0]
            if new_mode == SAHARA_MODE_IMAGE_TX_PENDING:
                print("\n[🎉 SUCCESS!] Target transitioned to SAHARA_MODE_IMAGE_TX_PENDING (EDL Mode)!")
                
                # Acknowledge the EDL HELLO
                print("[Step 3] Acknowledging EDL mode handshake...")
                hello_resp_edl = struct.pack("<12I", 
                    SAHARA_CMD_HELLO_RESP, 0x30, 0x02, 0x01, 0x00,
                    SAHARA_MODE_IMAGE_TX_PENDING, 0, 0, 0, 0, 0, 0
                )
                print_packet(hello_resp_edl, "TX")
                ser.write(hello_resp_edl)
                ser.flush()
                
                read_data = read_packet(ser, timeout=2.0)
                if read_data:
                    print_packet(read_data, "RX")
                    print("\n[🎉 TARGET READY FOR FLASHING] Sahara is waiting for Firehose programmer!")
                ser.close()
                return

    ser.close()
    print("\n" + "=" * 65)
    print("Checking USB State:")
    for _ in range(6):
        time.sleep(0.5)
        st = check_lsusb()
        if "9008" in st:
            print(f"[🎉 DETECTED] {st}")
            return
    print(f"Current State: {check_lsusb()}")
    print("=" * 65)

if __name__ == "__main__":
    main()
