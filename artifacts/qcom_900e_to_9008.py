#!/usr/bin/env python3
"""
Qualcomm 900E Diagnostic & EDL Switcher
Pings Qualcomm HS-USB Diagnostics 900E (/dev/ttyUSB0) and commands a transition to 9008 (EDL).
"""

import sys
import time
import os
import subprocess

try:
    import serial
except ImportError:
    print("[-] pyserial not found! Install it with: pip install pyserial (or sudo apt install python3-serial)")
    sys.exit(1)

# Qualcomm DIAG CRC-16 (Reflected CCITT / Polynomial 0x8408)
def crc16_diag(data: bytes) -> bytes:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    crc ^= 0xFFFF
    return bytes([crc & 0xFF, (crc >> 8) & 0xFF])

# Qualcomm HDLC Packet Framing & Escaping
def hdlc_encode(payload: bytes) -> bytes:
    crc = crc16_diag(payload)
    raw = payload + crc
    escaped = bytearray()
    for b in raw:
        if b == 0x7E:
            escaped.extend([0x7D, 0x5E])
        elif b == 0x7D:
            escaped.extend([0x7D, 0x5D])
        else:
            escaped.append(b)
    escaped.append(0x7E)
    return bytes(escaped)

def hdlc_decode(frame: bytes) -> bytes:
    data = frame.strip(b"\x7e")
    unescaped = bytearray()
    i = 0
    while i < len(data):
        if data[i] == 0x7D and i + 1 < len(data):
            unescaped.append(data[i+1] ^ 0x20)
            i += 2
        else:
            unescaped.append(data[i])
            i += 1
    if len(unescaped) < 3:
        return bytes(unescaped)
    return bytes(unescaped[:-2])  # Strip CRC

# Qualcomm DIAG Command Codes & Errors
DIAG_ERRORS = {
    0x13: "DIAG_BAD_CMD_F (Command unrecognized or blocked by security policy)",
    0x14: "DIAG_BAD_PARM_F (Invalid parameter in command)",
    0x15: "DIAG_BAD_LEN_F (Invalid packet length)",
    0x16: "DIAG_BAD_MODE_F (Device in wrong state for command)",
    0x18: "DIAG_BAD_SPC_MODE_F (Security code / Auth required)",
}

def check_lsusb():
    try:
        out = subprocess.check_output(["lsusb"], stderr=subprocess.DEVNULL).decode()
        for line in out.splitlines():
            if "05c6:9008" in line:
                return "9008 (EDL Mode - SUCCESS!)"
            if "05c6:900e" in line:
                return "900e (Diagnostics / RAM Dump)"
    except Exception:
        pass
    return "Disconnected / Unknown"

def send_and_receive(ser, name, payload, timeout=1.5):
    pkt = hdlc_encode(payload)
    print(f"\n[>] Sending {name} (Hex: {pkt.hex(' ')})")
    
    ser.reset_input_buffer()
    ser.write(pkt)
    ser.flush()
    
    start = time.time()
    buf = bytearray()
    while time.time() - start < timeout:
        if ser.in_waiting > 0:
            b = ser.read(ser.in_waiting)
            buf.extend(b)
            if b"\x7e" in buf:
                break
        time.sleep(0.05)
    
    if not buf:
        print(f"[-] No response from device (Timeout: {timeout}s)")
        return None
    
    print(f"[<] Raw Response: {buf.hex(' ')}")
    decoded = hdlc_decode(buf)
    
    if len(decoded) > 0:
        cmd_code = decoded[0]
        if cmd_code in DIAG_ERRORS:
            print(f"[!] Device replied with Error: {DIAG_ERRORS[cmd_code]}")
        else:
            print(f"[+] Device ACK / Replied with Code: 0x{cmd_code:02X}")
            printable = "".join(chr(c) if 32 <= c <= 126 else "." for c in decoded)
            print(f"    ASCII Preview: {printable}")
    return decoded

def main():
    port = "/dev/ttyUSB0" if len(sys.argv) < 2 else sys.argv[1]
    
    if not os.path.exists(port):
        print(f"[-] Port {port} does not exist!")
        print("    Run: sudo modprobe usbserial vendor=0x05c6 product=0x900e")
        sys.exit(1)
        
    print("=" * 60)
    print(f" Qualcomm 900E Diag & EDL Switcher -> {port}")
    print(f" Initial USB State: {check_lsusb()}")
    print("=" * 60)
    
    try:
        ser = serial.Serial(
            port=port,
            baudrate=115200,
            bytesize=serial.EIGHTBITS,
            parity=serial.PARITY_NONE,
            stopbits=serial.STOPBITS_ONE,
            timeout=1.0,
            rtscts=False,
            dsrdtr=False
        )
    except Exception as e:
        print(f"[-] Failed to open {port}: {e}")
        print("    Try running with sudo!")
        sys.exit(1)

    # -------------------------------------------------------------
    # 1. PING & DIAGNOSTIC INQUIRY (Test if firmware is alive)
    # -------------------------------------------------------------
    print("\n--- PHASE 1: Health & Responsiveness Check ---")
    
    # 1A: Ping / Version (DIAG_VERNO_F: 0x00)
    resp = send_and_receive(ser, "Ping / Version Query (0x00)", bytes([0x00]))
    if resp is not None:
        print("[+++] FIRMWARE IS ACTIVE! CPU is listening to DIAG interrupts.")
    else:
        print("[!] No reply to 0x00. Testing Extended Build ID...")

    # 1B: Extended Build ID (0x7C)
    send_and_receive(ser, "Extended Build ID (0x7C)", bytes([0x7C]))

    # 1C: Status Inquiry (0x0C)
    send_and_receive(ser, "Status Inquiry (0x0C)", bytes([0x0C]))

    # -------------------------------------------------------------
    # 2. EDL SWITCH COMMANDS
    # -------------------------------------------------------------
    print("\n--- PHASE 2: Sending Mode-Switch Commands ---")
    
    commands = [
        ("Subsystem 0x4B Cmd 0x65 (Standard Qualcomm EDL Trigger)", bytes([0x4B, 0x65, 0x01, 0x00])),
        ("Command 0x3A (Legacy Qualcomm DLOAD / Reset)", bytes([0x3A])),
        ("Subsystem 0x4B Cmd 0x25 (Core DLOAD)", bytes([0x4B, 0x25, 0x01, 0x00])),
        ("Subsystem 0x4B Cmd 0x18 (Bootloader / Fastboot)", bytes([0x4B, 0x18, 0x01, 0x00])),
    ]
    
    for name, payload in commands:
        send_and_receive(ser, name, payload, timeout=1.0)
        time.sleep(0.5)
        state = check_lsusb()
        if "9008" in state:
            print(f"\n[🎉 SUCCESS] Device transitioned to {state}!")
            ser.close()
            return

    ser.close()
    
    print("\n--- PHASE 3: Final Verification ---")
    print("Waiting 3 seconds for USB re-enumeration...")
    for _ in range(6):
        time.sleep(0.5)
        state = check_lsusb()
        if "9008" in state:
            print(f"[🎉 SUCCESS] Device is now in {state}!")
            return
            
    print(f"[-] Device state after execution: {check_lsusb()}")

if __name__ == "__main__":
    main()
