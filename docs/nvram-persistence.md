# NVRAM persistence without SPI NOR

The board has no SPI NOR flash.  EDK2's `VariableRuntimeDxe` therefore runs
in emulated non-volatile mode (`PcdEmuVariableNvModeEnable`): every variable
lives in RAM and dies at reset.  Out of the box that means boot options
authored in the firmware (`bcfg boot add`, the boot manager) evaporate, the
hard-coded fallback entries resurrect on every boot, and a deleted entry
comes back like a horror movie villain.

`X96NvVarsPersistDxe` fixes this: it checkpoints the NVRAM into a reserved
raw region of the boot card and replays it before BDS reads `BootOrder`,
the next time the firmware runs.

## Where the store lives

Raw, at **LBA 4..131 of the boot card** — the unformatted 64 KiB gap
between the MBR and the first partition.  Nothing is formatted there, so
neither `fsck` nor any filesystem can touch it; the firmware reaches it
through `EFI_BLOCK_IO` only, and only through the SD host's device path
(`VenHw(B072F2F9-...)`).  The eMMC is never touched.

Why raw and not a FAT file: the firmware's own FAT-driver write path on
this board loses writes silently, and even the load path hung at BDS time.
The one I/O path this platform exercises heavily and reliably is single-
block `BlockIo` (SD identification, FAT tables, GRUB reading megabytes);
the store uses single-block commands only, with per-sector read-back
verification on every write.

## Store format (little-endian)

At LBA 4, byte offset 2048:

    offset  size  field
    0       8     magic "X96NVRAM"
    8       4     version (1)
    12      4     record count
    16      4     data length (header excluded, padded to 4-byte multiple)
    20      4     CRC32 (EFI CRC-32 over the data region)
    24      8     reserved
    32      ...   records, back to back:

    record: EFI_GUID  VariableGuid          (16 bytes)
            UINT32       NameLength         (bytes, incl. L'\0')
            CHAR16       Name[NameLength/2] (UTF-16LE, NUL-terminated)
            UINT32       Attributes
            UINT32       DataLength
            UINT8        Data[DataLength]

Only non-volatile variables are stored.  The header's length and CRC
govern what a reader trusts; a shorter image over a longer one is
harmless.

## The load/replay protocol

Timing on this board is everything, and the driver encodes several
hard-won lessons:

* The store is **read and validated in the BDS load-event callback**
  (signaled after the USB retry block, serialized with controller
  connects).  The boot manager then calls the persist protocol's
  `Replay()` **directly, at `TPL_APPLICATION`**.

  Why: `SetVariable` issued from a `TPL_CALLBACK` notify context wedges
  the variable services on this board, and an event notified at
  `TPL_APPLICATION` is not a legal replacement — the UEFI spec rejects
  notify TPLs at or below `TPL_APPLICATION` (that mistake died in an
  `ASSERT` on first boot).

* Store records are walked through an **aligned buffer**.  Records after
  an odd-sized data field (`PlatformLang` is 3 bytes) start at odd image
  offsets; MdePkg asserts on unaligned `CHAR16` pointers (`String.c`),
  which dead-looped every replay of such a record.  Names are copied into
  an aligned buffer before any string operation or `SetVariable`.

* `VarErrorFlag` (the variable driver's FTW internal state) and `Lang`
  (the legacy ISO639-2 tag; BdsDxe re-derives it every boot via
  `InitializeLanguage`, and replaying `PlatformLang` triggers the same
  derivation inside the variable driver) are **never persisted or
  replayed**.

* Every enumeration passes starts with an **empty name buffer**:
  `GetNextVariableName` treats a pre-filled buffer as "continue after
  this name", and returns `EFI_INVALID_PARAMETER` when that name does
  not exist.

## Boot-option policy (the gating)

`X96BootManagerLib` consults the persist protocol after the replay: if a
store was restored **and** it contains at least one active boot option
that is not a `CATEGORY_APP` entry, NVRAM is authoritative and the
hard-coded fallback boot options (UEFI Shell default entry, SD
diagnostics) are *not* re-registered.  On a virgin store, a failed load,
or a restored store that cannot boot anything, the fallbacks come back so
the box always boots something.

## Saving changes made in the Shell

Checkpoints run automatically at ReadyToBoot (covers boot-manager changes)
and just before ExitBootServices (covers Shell changes that end by booting
something).  A Shell session that ends with a plain `reset` bypasses both,
so there are two extra channels:

* A **10-second periodic checkpoint** — re-enumerates the NVRAM, and only
  touches the card when the CRC changed (pure-enumeration cost otherwise).
* The **`nvsave` shell command** — immediate, deterministic save.

Both funnel through the same per-sector-verified write path, with a
reentrancy guard so overlapping checkpoint passes never interleave card
writes.

## Linux-side access

`scripts/x96-nvvars.py` reads and writes the same bytes from Linux:

    x96-nvvars.py list                     # inspect the store on /dev/mmcblk1
    x96-nvvars.py add <guid> <name> nv,bs,rt --data hello
    x96-nvvars.py del <guid> <name>
    x96-nvvars.py --file image.bin list    # or work on a file

The device default is `/dev/mmcblk1` (the boot card).  Records written
from Linux are replayed by the firmware on the next boot exactly like
firmware-authored ones — the CRC and the aligned-walk rules are the same,
and the tool writes the same image layout.  Caveat: variables saved after
the OS took over are not checkpointed by the firmware (boot services are
gone); use this tool to sync those.

## Recovery

If the box wedges and the store is suspected (bad image, stuck boot):
power off 10 seconds.  If it still misbehaves, erase the store from a
card reader host — the next boot treats the card as virgin, registers
the fallbacks, and rewrites a fresh store:

    dd if=/dev/zero of=/dev/sdX bs=512 count=8 seek=4
