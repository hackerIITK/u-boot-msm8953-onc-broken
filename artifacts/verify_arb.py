#!/usr/bin/env python3
"""
Qualcomm SecBoot v3 & Android Verified Boot 2.0 (AVB) Anti-Rollback Analyzer
Analyzes Vivo / Qualcomm firmware update ZIPs, extracted folders, or raw partition images.
"""

import os
import struct
import sys
import zipfile


def parse_secboot_arb(data):
    """Scan raw binary data for Qualcomm SecBoot v3 MBN metadata."""
    results = []
    p = struct.pack('<I', 0x9002)  # Vivo OEM ID
    pos = 0
    while True:
        pos = data.find(p, pos)
        if pos == -1:
            break
        # In Qualcomm SecBoot v3 metadata:
        # pos is offset of OEM_ID (0x50 from start of MBN header)
        # pos - 0x0c is AntiRollback_Version
        # pos - 0x10 is SW_TYPE
        # pos - 0x24 is Metadata Size (usually 0x78 = 120 bytes)
        if pos >= 0x50:
            meta_sz = struct.unpack('<I', data[pos - 0x24 : pos - 0x20])[0]
            if meta_sz in [0x78, 0x80, 0x50]:
                hdr_off = pos - 0x50
                img_id = struct.unpack('<I', data[hdr_off + 4 : hdr_off + 8])[0]
                sw_type = struct.unpack('<I', data[pos - 0x10 : pos - 0x0C])[0]
                anti_rb = struct.unpack('<I', data[pos - 0x0C : pos - 0x08])[0]
                results.append((hdr_off, img_id, sw_type, anti_rb))
                break
        pos += 4
    return results


def parse_avb_rollback(data):
    """Scan raw binary data for AVB 2.0 header and rollback index."""
    if len(data) >= 84 and data[:4] == b'AVB0':
        rb_idx = struct.unpack('>Q', data[72:80])[0]
        rb_loc = struct.unpack('>I', data[80:84])[0]
        return rb_idx, rb_loc
    return None


def analyze_zip(zip_path):
    print(f"\n=======================================================")
    print(f" Target ZIP: {zip_path}")
    print(f" Size: {os.path.getsize(zip_path):,} bytes")
    print(f"=======================================================")

    with zipfile.ZipFile(zip_path, 'r') as z:
        namelist = z.namelist()

        # 1. OTA Metadata
        print("\n[*] Android Build & Version Metadata:")
        if 'META-INF/com/android/metadata' in namelist:
            meta = z.read('META-INF/com/android/metadata').decode('latin1', errors='ignore')
            for line in meta.splitlines():
                if any(
                    line.startswith(k)
                    for k in [
                        'post-version=',
                        'post-sdk-level=',
                        'post-security-patch-level=',
                        'post-build=',
                        'post-timestamp=',
                        'hardware-device=',
                    ]
                ):
                    print(f"    {line}")
        else:
            print("    (No META-INF/com/android/metadata found)")

        # 2. Qualcomm SecBoot Hardware ARB
        print("\n[*] Qualcomm SecBoot v3 Hardware ARB (QFPROM eFuses):")
        found_mbn = False
        for name in sorted(namelist):
            if name.lower().endswith(('.elf', '.mbn')):
                data = z.read(name)
                arbs = parse_secboot_arb(data)
                for hdr_off, img_id, sw_type, anti_rb in arbs:
                    base_name = os.path.basename(name)
                    print(
                        f"    {base_name:<20} -> AntiRollback_Version = {anti_rb} "
                        f"(SW_TYPE: 0x{sw_type:02x}, ImgID: {img_id})"
                    )
                    found_mbn = True
        if not found_mbn:
            print("    (No signed Qualcomm MBN/ELF images detected)")

        # 3. Android Verified Boot 2.0
        print("\n[*] Android Verified Boot 2.0 (AVB / RPMB Rollback Index):")
        found_avb = False
        for name in sorted(namelist):
            if 'vbmeta' in name.lower() and name.lower().endswith('.img'):
                data = z.read(name)
                res = parse_avb_rollback(data)
                if res:
                    rb_idx, rb_loc = res
                    base_name = os.path.basename(name)
                    print(
                        f"    {base_name:<38} -> Rollback Index = {rb_idx} "
                        f"(Location: {rb_loc})"
                    )
                    found_avb = True
        if not found_avb:
            print("    (No AVB0 vbmeta images detected)")

    print("\n=======================================================\n")


def analyze_directory(dir_path):
    print(f"\n=======================================================")
    print(f" Target Directory: {dir_path}")
    print(f"=======================================================")

    print("\n[*] Qualcomm SecBoot v3 Hardware ARB (QFPROM eFuses):")
    for root, _, files in os.walk(dir_path):
        for f in sorted(files):
            if f.lower().endswith(('.elf', '.mbn')):
                p = os.path.join(root, f)
                with open(p, 'rb') as fp:
                    data = fp.read()
                arbs = parse_secboot_arb(data)
                for hdr_off, img_id, sw_type, anti_rb in arbs:
                    print(
                        f"    {f:<20} -> AntiRollback_Version = {anti_rb} "
                        f"(SW_TYPE: 0x{sw_type:02x}, ImgID: {img_id})"
                    )

    print("\n[*] Android Verified Boot 2.0 (AVB / RPMB Rollback Index):")
    for root, _, files in os.walk(dir_path):
        for f in sorted(files):
            if 'vbmeta' in f.lower() and f.lower().endswith('.img'):
                p = os.path.join(root, f)
                with open(p, 'rb') as fp:
                    data = fp.read()
                res = parse_avb_rollback(data)
                if res:
                    rb_idx, rb_loc = res
                    print(
                        f"    {f:<38} -> Rollback Index = {rb_idx} "
                        f"(Location: {rb_loc})"
                    )
    print("\n=======================================================\n")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <update.zip | firmware_directory | file.mbn>")
        sys.exit(1)

    target = sys.argv[1]
    if not os.path.exists(target):
        print(f"[-] Error: Target path not found: {target}")
        sys.exit(1)

    if os.path.isfile(target):
        if zipfile.is_zipfile(target):
            analyze_zip(target)
        else:
            with open(target, 'rb') as fp:
                data = fp.read()
            arbs = parse_secboot_arb(data)
            avb = parse_avb_rollback(data)
            print(f"Analysis for: {target}")
            if arbs:
                for hdr_off, img_id, sw_type, anti_rb in arbs:
                    print(f"  Qualcomm SecBoot ARB: {anti_rb} (SW_TYPE: 0x{sw_type:02x})")
            if avb:
                print(f"  AVB 2.0 Rollback Index: {avb[0]} (Location: {avb[1]})")
            if not arbs and not avb:
                print("  No Qualcomm SecBoot or AVB0 headers identified.")
    elif os.path.isdir(target):
        analyze_directory(target)


if __name__ == '__main__':
    main()
