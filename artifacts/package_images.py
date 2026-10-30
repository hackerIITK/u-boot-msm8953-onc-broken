#!/usr/bin/env python3
import sys, os, struct, gzip, subprocess, hashlib

def package(version):
    top_dir = '/workspaces/cirrus-ci/onc'
    uboot_dir = os.path.join(top_dir, 'u-boot')
    mkbootimg = os.path.join(top_dir, 'lk2nd-msm8953/lk2nd/scripts/mkbootimg')
    ramdisk_empty = '/tmp/ramdisk_empty'
    if not os.path.exists(ramdisk_empty):
        with open(ramdisk_empty, 'wb') as f:
            pass

    nodtb_path = os.path.join(uboot_dir, 'u-boot-nodtb.bin')
    dtb_path = os.path.join(uboot_dir, 'u-boot.dtb')

    with open(nodtb_path, 'rb') as f:
        nodtb = f.read()
    with open(dtb_path, 'rb') as f:
        dtb = f.read()

    # Patch AArch64 image size header field at offset 0x2c
    nodtb_patched = bytearray(nodtb)
    struct.pack_into('<I', nodtb_patched, 0x2c, len(nodtb))
    uboot_bin = bytes(nodtb_patched) + dtb
    with open(os.path.join(uboot_dir, 'u-boot.bin'), 'wb') as f:
        f.write(uboot_bin)

    results = []

    # 1. lk2nd image
    out_lk2nd = os.path.join(top_dir, f'u-boot_msm8953_onclite_lk2nd_{version}.img')
    subprocess.check_call([
        mkbootimg,
        '--kernel', os.path.join(uboot_dir, 'u-boot.bin'),
        '--ramdisk', ramdisk_empty,
        '--base', '0x80000000',
        '--kernel_offset', '0x00080000',
        '--ramdisk_offset', '0x02000000',
        '--tags_offset', '0x00000100',
        '--pagesize', '2048',
        '--output', out_lk2nd
    ])
    results.append(('lk2nd', out_lk2nd))

    # 2. stockboot image
    stock_dtb_path = '/tmp/stock_dtb_table.bin'
    if os.path.exists(stock_dtb_path):
        uboot_gz = gzip.compress(uboot_bin, compresslevel=9)
        with open(stock_dtb_path, 'rb') as f:
            stock_dtb_table = f.read()
        stock_payload = uboot_gz + stock_dtb_table
        payload_tmp = '/tmp/stock_kernel_payload.bin'
        with open(payload_tmp, 'wb') as f:
            f.write(stock_payload)

        out_stock = os.path.join(top_dir, f'u-boot_msm8953_onclite_stockboot_{version}.img')
        subprocess.check_call([
            mkbootimg,
            '--kernel', payload_tmp,
            '--ramdisk', ramdisk_empty,
            '--base', '0x80000000',
            '--kernel_offset', '0x00008000',
            '--ramdisk_offset', '0x01000000',
            '--tags_offset', '0x00000100',
            '--pagesize', '2048',
            '--cmdline', 'console=ttyMSM0,115200,n8 androidboot.hardware=qcom',
            '--output', out_stock
        ])
        with open(out_stock, 'ab') as f:
            f.write(b'SEANDROIDENFORCE')
        results.append(('stockboot', out_stock))

    for variant, path in results:
        size = os.path.getsize(path)
        md5 = hashlib.md5(open(path, 'rb').read()).hexdigest()
        print(f'{variant.upper()}: {path} ({size} bytes, MD5: {md5})')

if __name__ == '__main__':
    ver = sys.argv[1] if len(sys.argv) > 1 else 'v23'
    package(ver)
