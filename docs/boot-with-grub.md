# Booting Linux with GRUB through the EFI stub

Status: **working** on the X96 Max+ with our EDK2 build. GRUB hands the
kernel to its own EFI stub, and the kernel reports full UEFI runtime
services:

```
[    0.000000] efi: EFI v2.7 by TianoCore / X96MaxPlus EDK2
[    0.000000] efi: MEMATTR=0xf30b7418 INITRD=0xf2f81e98 MEMRESERVE=0xf2f81998
```

Both an ophub kernel (6.18.59-ophub) and a stock Debian kernel
(6.12.100+deb12-arm64) boot this way from the same card.

## The one thing the firmware must do

The Linux EFI stub and GRUB both obtain the platform Device Tree from the
standard EFI configuration table (`gFdtTableGuid`,
`b1b621d5-f19c-41a5-830b-d9152c69aae0`). Our firmware publishes its normalised
DTB there in `PlatformBootManagerBeforeConsole` — without that table GRUB
synthesises an *empty* FDT, the stub boots a kernel with no `/memory` node,
and the machine dies silently right after ExitBootServices. That "post-EBS
hang" cost us two weeks before the serial console exposed it.

## Building GRUB (on any arm64-capable Linux, or on the target's rootfs)

For an Armbian rootfs on the box itself:

```sh
apt-get install grub-efi-arm64-bin

# config that works, on the FAT partition whose root is the card's /boot:
cat > /boot/grub/grub.cfg <<'EOF'
set timeout=5
set default=0
menuentry "Armbian 6.18.59-ophub (EFI stub)" {
    linux /vmlinuz-6.18.59-ophub console=ttyAML0,115200n8 root=/dev/mmcblk1p2 rootwait rw
    initrd /initrd.img-6.18.59-ophub
}
menuentry "Debian 6.12.100+deb12 (EFI stub)" {
    linux /vmlinuz-6.12.100+deb12-arm64 console=ttyAML0,115200n8 root=/dev/mmcblk1p2 rootwait rw
    initrd /initrd.img-6.12.100+deb12-arm64
}
EOF

# single-file image, prefix pointed at the right path on the FAT:
grub-mkimage -O arm64-efi -o /boot/grubaa64.efi -d /usr/lib/grub/arm64-efi \
  -p '(hd1,msdos1)/grub' \
  linux fat part_msdos search search_fs_file search_fs_uuid search_label \
  echo ls test normal configfile minicmd boot gzio all_video efi_gop serial terminfo font
```

Sync it (`sync`!) and boot.

## Traps, all verified the hard way

1. **GRUB's path naming vs Linux's.** GRUB sees the card's FAT partition root
   as `(hd1,msdos1)/` — which is Linux's `/boot`. A config written by Linux
   to `/boot/grub/grub.cfg` therefore lives at `(hd1,msdos1)/grub/grub.cfg`
   from GRUB's point of view. Prefixes pointing at `.../boot/grub` silently
   miss and GRUB drops to its command prompt.
2. **`configfile` does not show a menu.** It registers the entries; the menu
   loop is `normal`. At a `grub>` prompt, `normal` (or `normal <configfile>`)
   is what actually draws the menu.
3. **grub-mkstandalone's memdisk config** did not auto-start in our build
   (it appeared as a tarfs device but no menu). The mkimage + FS-resident
   config above is the shape that works; keep it.
4. **Module availability differs per distro.** Debian's `arm64-efi` module
   directory has no `menu.mod` and no `initrd.mod` in 2.14 (initrd is part of
   `linux.mod`). List modules explicitly and let the linker complain.
5. **`reboot -f` from Linux without `sync`** can lose the FAT writes you just
   made. Always `sync` and read the files back before rebooting.

## Manual boot from the `grub>` prompt

The escape hatch when a config is missing (this is also exactly the sequence
that first proved the stub path):

```
grub> linux (hd1,msdos1)/vmlinuz-6.18.59-ophub console=ttyAML0,115200n8 root=/dev/mmcblk1p2 rootwait rw
grub> initrd (hd1,msdos1)/initrd.img-6.18.59-ophub
grub> boot
```

Device naming under this firmware: `hd0` = eMMC (no filesystem, vendor AML
table), `hd1` = the SD card.

## What the stub needs from the firmware

* FDT configuration table (see above) — provided.
* `ExitBootServices` that actually terminates and produces a sane memory map
  — provided (our loader exercises the same path).
* LoadFile2 initrd protocol — implemented **by GRUB** (the stub consumes
  it); nothing needed from us.
* `LoadImage`/`StartImage` on a memory-mapped device path — EDK2 stock,
  works.

The historical "chain wrappers" (X96ChainDebian.efi → X96LinuxBoot.efi) were
a workaround for the empty-FDT hang and are no longer needed for GRUB boots;
`X96LinuxBoot.efi` remains useful as a booti-style fallback that does not
require the kernel to have an EFI stub at all.
