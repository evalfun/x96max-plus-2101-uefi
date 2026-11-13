# Hardware notes (X96 Max+ / S905X3)

## Serial console

* UART on the board header, 3.3 V TTL, **115200 8N1**. TX→RX crossed, GND
  common; do not connect the 3.3 V pin of your adapter.
* In the vendor chain, the same console carries BL2 → BL31 → U-Boot output.
  Under our firmware it carries the EDK2 console (DEBUG builds are very
  chatty; RELEASE builds print only the boot banner and the SD diagnostics
  line).
* This console is how every root cause in these docs was found. Without it,
  expect blind debugging; the HDMI console helps but cannot show early
  failures.

## HDMI

`MesonDisplayDxe` (ported from the VIM3 G12B sources, DEPEX reduced to
`gEfiCpuArchProtocolGuid`, no ACPI tables) drives the VPU; GraphicsConsole
renders the UEFI menu. Menu text on a TV is legible at 1080p. The kernel
takes over the display afterwards (meson DR driver), no handoff needed from
our side beyond the DTB framebuffer node.

## USB

The X96 rails are hardwired (no VBUS regulator GPIO), unlike the VIM3 board
policy we inherited — removing the VIM3 GPIO rail logic made keyboards
enumerate reliably. Mass-storage enumeration remains intermittent: the DWC3
controller's one-shot xHCI bring-up is timing-marginal under RELEASE builds
(see `PlatformBootManagerAfterConsole` for the retry loop that mostly tames
it). Deferred until it actually matters.

## SD card

* Card detect is board-specific and untrustworthy on this unit family; the
  driver always reports "present" and lets identification decide.
* The unit under test trained its identification clock at co=0/tx=0/400 kHz
  on the first phase attempt — see `docs/sd-root-cause.md` for the
  instrumentation.
* Units of the same model behave differently at the card-detect pad; do not
  assume a twin box is a twin board.

## eMMC

* Device: 64 GiB HS200, boot0 == boot1 (vendor mirrors).
* The kernel names it `mmcblk2` on this board's DT; the SD card is
  `mmcblk1`. The same numbering holds under our firmware because we hand the
  kernel the vendor-derived DTB.
* Boot partitions are read-only from Linux by default; unlock with
  `echo 0 > /sys/block/mmcblk2boot0/force_ro` before writing.
* See `docs/eMMC-flash-in.md` for the boot-partition layout and the flash-in
  attempts.

## Model variants

The X96 Max+ exists in several firmware builds (2 GB/4 GB, different DDR
configurations, "2101" and later boards). Our DTB was captured from a known
good Armbian boot on a 2101 board with 4 GB LPDDR4 that trains DDR4 in
fallback mode. Other revisions may need a different DTB or DDR profile —
which is a BL2/ACS concern, not ours.
