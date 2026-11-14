/** @file
  Shared declarations for the X96 NvVars variable-store persistence.

  The board has no SPI NOR, so VariableRuntimeDxe runs in emulated NV mode
  (PcdEmuVariableNvModeEnable): the store lives in RAM and dies at reset.
  X96NvVarsPersistDxe checkpoints that store into a flat file on the boot
  card's FAT partition and replays it back early in the next boot, before
  BDS reads BootOrder.

  The X96BootManagerLib consults the protocol below to decide whether the
  hard-coded fallback boot options (UEFI Shell, X96 SD Diagnostic) should be
  registered: on a restored store NVRAM is authoritative -- a boot entry the
  user deleted stays deleted -- while a virgin store gets the fallbacks so
  the box always boots something.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef X96_NV_VARS_PERSIST_H_
#define X96_NV_VARS_PERSIST_H_

//
// Store file name, at the root of the boot card's FAT volume (the volume
// that carries the uefi.bin gate file).  From Linux the card is mounted at
// /boot, so the store lives at /boot/NvVars.
//
#define X96_NV_VARS_FILE_NAME  L"NvVars"

//
// Installed at driver entry, before BDS runs.  PersistLoaded is FALSE until
// the store image was read from the card AND replayed into the variable
// services; HasBootableOption tells the boot manager whether the restored
// NVRAM can boot anything on its own (it is meaningless while PersistLoaded
// is FALSE).
//
// The replay itself is invoked by the boot manager through Replay(), NOT
// from a notify event: a SetVariable issued from a TPL_CALLBACK context
// wedges the variable services on this board, so the pass must run in the
// boot manager's own TPL_APPLICATION context.  (An event notified at
// TPL_APPLICATION is not a legal replacement either -- the UEFI spec rejects
// notify TPLs at or below TPL_APPLICATION, which is how the previous
// attempt died in an ASSERT.)
//
typedef struct X96_NV_VARS_PERSIST_PROTOCOL_  X96_NV_VARS_PERSIST_PROTOCOL;

typedef
EFI_STATUS
(*X96_NV_VARS_REPLAY_FUNC) (
  IN OUT X96_NV_VARS_PERSIST_PROTOCOL  *This
  );

//
// Manual checkpoint entry point, exposed to the 'nvsave' shell command:
// saves the current NVRAM to the card on demand (a Shell session that ends
// with a plain reset never reaches ReadyToBoot or ExitBootServices).
//
typedef
EFI_STATUS
(*X96_NV_VARS_CHECKPOINT_FUNC) (
  IN OUT X96_NV_VARS_PERSIST_PROTOCOL  *This
  );

struct X96_NV_VARS_PERSIST_PROTOCOL_ {
  X96_NV_VARS_REPLAY_FUNC      Replay;
  X96_NV_VARS_CHECKPOINT_FUNC  Checkpoint;
  BOOLEAN                      PersistLoaded;
  BOOLEAN                      HasBootableOption;
};

extern EFI_GUID  gX96NvVarsPersistProtocolGuid;

//
// Store location and format (little-endian), shared with the Linux-side
// sync helper scripts/x96-nvvars.py:
//
//   The image lives RAW on the boot card at LBA 4 (byte offset 2048), the
//   unformatted gap between the MBR and the first partition, and may span
//   up to 128 sectors (64 KiB).  Nothing is formatted there, so neither
//   fsck nor the filesystem can touch it.
//
//   offset  size  field
//   0       8     magic  "X96NVRAM"
//   8       4     version (1)
//   12      4     record count
//   16      4     data length in bytes (header excluded, includes trailing
//                 zero padding so the length is a multiple of 4)
//   20      4     CRC32 (EFI CRC-32, computed over the data region)
//   24      8     reserved (zero)
//   32      ..    records, back to back:
//
//   record: EFI_GUID  VariableGuid          (16 bytes)
//           UINT32       NameLength         (bytes, incl. L'\0')
//           CHAR16       Name[NameLength/2] (UTF-16LE, NUL-terminated)
//           UINT32       Attributes
//           UINT32       DataLength
//           UINT8        Data[DataLength]
//
// Variable attributes are filtered at checkpoint time: only
// EFI_VARIABLE_NON_VOLATILE entries are written.
//

#endif // X96_NV_VARS_PERSIST_H_
