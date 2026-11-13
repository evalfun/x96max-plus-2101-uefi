# X96 Max+ UEFI firmware (S905X3 / Amlogic SM1)

UEFI (EDK2) firmware for the **X96 Max+ TV box** (Amlogic S905X3, SM1 family).
EDK2 replaces the vendor U-Boot as the OS-facing bootloader; the vendor BL2,
BL30 and BL31 are retained, so the boot chain stays stock up to the point
where firmware takes over.

> **谁会闲的蛋疼在电视盒子上跑 UEFI 呢？**
> ——是的，没人会。这就是为什么这个仓库存在：一台 30 块钱的电视盒子，
> 板载 4GB LPDDR4、千兆网卡、HDMI、USB3，唯一的问题是它的 bootloader 是
> 一坨 2015 年的 U-Boot。现在它是一台正经的 UEFI 机器：GRUB 启动、
> EFI 变量、SMBIOS、内核看到完整的 EFI runtime services。
> 本仓库就是这份不理智的完整记录。

Deriving from [`gusbourg/vim3-edk2`](https://github.com/gusbourg/vim3-edk2)
(Khadas VIM3 / A311D), re-targeted to the SM1 silicon and the X96 Max+
platform, with the board-specific drivers written from scratch in this tree
(SD/eMMC host, HDMI display, USB power rails, boot manager, booti-style Linux
loader).

**This is a hobby port.** Exercised by one person, on two boards of the same
model, with one Linux distribution and one ophub kernel family. Your
particular X96 box may differ from mine (X96 clones vary wildly). Installing
this replaces your bootloader; recovery paths are documented below and you
should read them *before* flashing anything.

## What works

Verified end to end on the target hardware:

| Piece | State |
| --- | --- |
| Boot from SD card (vendor-chain gate) | ✅ the supported install path |
| Boot from eMMC boot partition (BL33 replacement) | ⚠️ blocked by vendor BL2 blob-size tables — see `docs/eMMC-flash-in.md` |
| UEFI console on HDMI (GraphicsConsole + GOP) | ✅ |
| UEFI console on serial (115200 8N1) | ✅ |
| SD card: identification, FAT mount, FS0 | ✅ (after four stacked root causes — `docs/sd-root-cause.md`) |
| USB keyboard | ✅ (USB mass storage enumeration: still flaky, deferred) |
| booti-style Linux loader (`X96LinuxBoot.efi`) | ✅ kernel + initrd + cmdline via Shell script |
| GRUB (grub-mkimage, arm64-efi) → Linux EFI stub | ✅ full boot, **with EFI runtime services** |
| Linux EFI runtime (`efi: EFI v2.7 by TianoCore`) | ✅ MEMATTR/INITRD/MEMRESERVE tables all exported |
| Zero-touch boot via `startup.nsh` on the card | ✅ — the boot policy is an editable text file, not firmware |
| eMMC/USB/NVMe boot media | untested (priority low; the card path is the point) |
| Capsule update, Secure Boot | not implemented |

The proof of life, from a real boot (Armbian 6.18.59-ophub through GRUB and
the kernel's own EFI stub):

```
[    0.000000] Machine model: AMedia X96 Max+
[    0.000000] efi: EFI v2.7 by TianoCore / X96MaxPlus EDK2
[    0.000000] efi: MEMATTR=0xf30b7418 INITRD=0xf2f81e98 MEMRESERVE=0xf2f81998
[    0.000000] Reserved memory: created CMA memory pool at 0x00000000d7400000, size 256 MiB
```

## Boot flow

```
BootROM ─▶ vendor BL2 (stock, untouched) ─▶ vendor BL30/BL31 ─▶ vendor U-Boot
                                                                │
                              SD card with boot_efi.flag + s905_autoscript
                                                                │
                                              fatload uefi.bin, `go`
                                                                │
                                                     EDK2 (this firmware)
                                                                │
                                          UEFI Shell ─▶ startup.nsh (editable)
                                                                │
                                          GRUB or X96LinuxBoot.efi ─▶ Linux
```

The firmware itself is never flashed into the box for the supported path:
the vendor U-Boot stays in eMMC and hands over to EDK2 from the SD card.
Removing the card boots whatever the vendor chain boots (Armbian/Android)
— a clean way to keep both worlds on one box.

## Build

Requirements: Linux x86_64 host, `aarch64-linux-gnu-gcc`, `python3`, `make`,
`git`, and the pinned submodules (`git submodule update --init --recursive`).

```sh
./scripts/build-x96.sh RELEASE      # → out/RELEASE/X96M_EFI.fd (0xE0000 bytes)
./scripts/build-x96.sh DEBUG       # → out/DEBUG/X96M_EFI.fd  (serial debug)

./scripts/package-x96-fip.sh RELEASE   # → out/RELEASE/fip/u-boot.bin.sd.bin (full FIP, for eMMC/recovery)
```

The build script:

* pins and patches `third_party/edk2` + `edk2-platforms` (see
  `patches/firmware/` — the submodules stay pristine upstream, our changes
  are `git apply`ed at build time),
* builds the firmware volume with our `Silicon/Amlogic/MesonSm1` and
  `Platform/Amlogic/X96MaxPlus` packages,
* wipes the FV work directory loudly every run, so nothing stale ever
  survives a link.

## Deploy (SD card, the supported way)

1. Format a card with a single FAT32 partition.
2. Copy from `out/RELEASE/`: `X96M_EFI.fd` → `uefi.bin` on the card.
3. Create an empty `boot_efi.flag` on the card.
4. Copy `s905_autoscript` (built by the vendor toolchain, see `scripts/`).
   This gate script is what the vendor U-Boot executes on boot: it
   `fatload`s `uefi.bin` and jumps to it.
5. Add your OS payload: either a `startup.nsh` (UEFI Shell script, the
   default boot), or an ESP-style layout for GRUB — see
   `docs/boot-with-grub.md` for a working GRUB recipe with both an
   ophub kernel and a stock Debian kernel.

A template card layout lives in the repo wiki / `docs/`.

## Repository layout

```
Silicon/Amlogic/MesonSm1       SM1 silicon: SD/eMMC host driver, HDMI, clocks
Platform/Amlogic/X96MaxPlus    Board: boot manager, PrePi, Linux loader, USB
Platform/Khadas/Vim3           the upstream VIM3 platform this derives from
third_party/edk2               pinned tianocore/edk2 submodule (patched at build)
third_party/edk2-platforms     pinned submodule (patched at build)
third_party/amlogic-boot-fip   vendor FIP reference material
patches/firmware/              the EDK2 patch set applied at build time
fip-board/x96max-plus          vendor blob inputs + FIP packaging recipe
scripts/                       build + package + deploy scripts
docs/                          war stories: root-cause analyses, recipes
```

## Documentation

* [`docs/sd-root-cause.md`](docs/sd-root-cause.md) — how four stacked bugs
  (board-specific card-detect gate, an ACMD41 ready-check typo, a card that
  assigns its own RCA, and a reversed long-response word order) conspired to
  make one SD card invisible, and the instrumentation that pinned each down.
* [`docs/boot-with-grub.md`](docs/boot-with-grub.md) — building a
  standalone GRUB for the card and booting mainline kernels through the
  EFI stub.
* [`docs/eMMC-flash-in.md`](docs/eMMC-flash-in.md) — what the eMMC boot
  partition layout looks like, why replacing BL33 wholesale is currently
  blocked, and what still works.
* [`docs/hardware-notes.md`](docs/hardware-notes.md) — serial console, HDMI,
  the USB rails, and other board trivia.

## License

Firmware sources in `Silicon/` and `Platform/`: BSD-2-Clause-Patent (EDK2
convention), as are the patches. Vendor blobs under `fip-board/` are
redistributed from the manufacturer's own firmware package and remain the
property of their owner; they are here because the FIP toolchain requires
them. Use of the firmware on hardware you do not own is your problem.
