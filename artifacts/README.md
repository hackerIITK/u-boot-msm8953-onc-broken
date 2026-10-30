# Xiaomi Redmi 7 (onclite / onc / SDM632) Reference Artifacts

This directory contains stock binaries, firmware partitions, decompiled device trees, and recovery/packaging scripts used during the U-Boot and Little Kernel (lk2nd) bringup on the Xiaomi Redmi 7 (`onclite` / `onc`, Qualcomm Snapdragon 632 / MSM8953-family).

## Firmware Binaries & Dumps

| Filename | Size | MD5 Checksum | Description |
| :--- | :--- | :--- | :--- |
| `emmc_appsboot.mbn` | 755 KiB | `a5c29bd7b330b28937c00b583657df6b` | Stock Xiaomi Little Kernel / ABOOT (AArch32, ELF) |
| `sbl1.bin` | 1.0 MiB | `11151c0a0ebe8b05ec8e78091e9131cd` | Dump of SBL1 partition (Secondary Boot Loader, ELF) |
| `sbl1_eng.mbn` | 451 KiB | `26b84efd6ce34496b848a112d434bbe2` | Engineering/unsigned SBL1 binary for EDL unbrick |
| `boot.img` | 20 MiB | `40b94d84f32fb30b4ed35c40e7d9ab3d` | Stock Android boot image (kernel, ramdisk, base DTBs) |
| `dtbo.img` | 8.0 MiB | `51e6d91ea997dc562a737a39f662e73e` | Stock Xiaomi DTBO partition image (panel and touch overlays) |
| `prog_emmc_firehose_8953_ddr.mbn` | 435 KiB | `fd5d784a6a471831ce43a3f4f85e799a` | Qualcomm Sahara/Firehose EDL programmer for MSM8953/SDM632 |

## Device Tree References

- `sdm632_49.dts`: Decompiled stock kernel 4.9 device tree extracted from `boot.img`.
- `sdm632_71.dts`: Decompiled stock kernel 7.1/CAF device tree.

## Scripts & Tools

- `package_images.py`: Packages U-Boot binaries (`u-boot-nodtb.bin` + `u-boot.dtb`) into Android boot images (lk2nd-wrapped, retrofit, and stockboot formats).
- `verify_arb.py`: Verifies Anti-Rollback (ARB) indices on Qualcomm MBN/ELF firmware partitions.
- `sahara_switch_to_edl.py` & `qcom_900e_to_9008.py`: Tools for forcing Sahara crashdumps / 900E diagnostic mode back to 9008 Emergency Download (EDL) mode.
