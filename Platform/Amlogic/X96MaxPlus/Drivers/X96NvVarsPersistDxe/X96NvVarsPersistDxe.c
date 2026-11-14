/** @file
  X96NvVarsPersistDxe: persist the emulated non-volatile variable store to a
  raw region of the boot SD card.

  This platform has no SPI NOR.  MdeModulePkg's VariableRuntimeDxe therefore
  runs in emulated NV mode (PcdEmuVariableNvModeEnable): every variable lives
  in RAM and dies at reset.  This driver checkpoints that store into a
  reserved, unformatted LBA range of the boot card (sectors 4..131, the gap
  between the MBR and the first partition) and replays it back into the
  variable services before BDS reads BootOrder, so boot entries authored in
  the firmware survive power cycles.

  The store goes through EFI_BLOCK_IO only.  A FAT-file-backed store was
  tried first and abandoned: on this board the firmware-side "open existing
  file and rewrite" path loses writes silently, and the FAT driver's presence
  on the boot-critical path raced the boot manager's controller connects.
  BlockIo is the one I/O path the platform exercises heavily and reliably
  (GRUB reads megabytes through it every boot).

  Only the SD host's device path (MesonSdMmcDxe's vendor GUID) is accepted;
  the eMMC is never touched.  The persistence window is bounded by boot
  services: block I/O is gone after ExitBootServices, so checkpoints run at
  ReadyToBoot and just before ExitBootServices.  Variables set after the OS
  took over (e.g. efibootmgr from Linux) are NOT checkpointed -- author boot
  entries from the firmware boot manager or the Shell, or sync them from
  Linux with scripts/x96-nvvars.py, which reads and writes the same LBA
  range using the format documented in X96NvVarsPersist.h.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Guid/EventGroup.h>
#include <Guid/GlobalVariable.h>
#include <Guid/VarErrorFlag.h>
#include <Guid/ZeroGuid.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#include "X96NvVarsPersist.h"

//
// Store image upper bound.  This box's variables (Boot#### entries,
// BootOrder, key options, console paths, PlatformLang) total a few KiB.
//
#define X96_STORE_MAX       0x20000
#define X96_NAME_MAX_CHARS  0x100

//
// Raw store location: sectors 4..131 of the boot card, the unformatted gap
// between the MBR (sector 0) and the first partition (sector 8192 on the
// standard card layout).  64 KiB is far more than the store ever needs; the
// image length in its header governs how much of it is meaningful.
//
#define X96_STORE_LBA       4
#define X96_STORE_SECTORS   128
#define X96_STORE_BYTES     (X96_STORE_SECTORS * 512)

//
// Device-path vendor GUID of the MesonSdMmcDxe SD host controller (Port B).
// Only BlockIo handles whose path carries this node are touched; the eMMC
// (9ec8ef97-...) must never see a store write.
//
STATIC CONST EFI_GUID  mSdHostDevicePathGuid =
  { 0xb072f2f9, 0x1405, 0x4a13, { 0xa2, 0x10, 0x3e, 0x5a, 0x4d, 0xd3, 0xb5, 0x20 } };

//
// Header: magic(8) version(4) count(4) length(4) crc32(4) reserved(8).
//
#define X96_HEADER_SIZE  32

STATIC CONST CHAR8  mMagic[8] = { 'X', '9', '6', 'N', 'V', 'R', 'A', 'M' };

//
// Persisted-store state.
//
STATIC X96_NV_VARS_PERSIST_PROTOCOL  mPersist;
STATIC EFI_HANDLE                    mPersistHandle;
STATIC BOOLEAN                       mLoadDone;    // load attempt concluded (read/validate phase)
STATIC BOOLEAN                       mReplayed;    // replay pass completed into the variable services
STATIC BOOLEAN                       mStoreReady;  // store read + validated, awaiting replay
STATIC CHAR16                        mNameBuf[X96_NAME_MAX_CHARS];
STATIC UINTN                         mImageLength;   // validated store image length

//
// Checkpoint state (allocated lazily, kept for reuse).
//
STATIC UINT8   *mWorkBuf;      // serialized store image
STATIC UINT8   *mScratchBuf;   // per-variable data scratch
STATIC UINT32   mLastCrc;
STATIC BOOLEAN  mLastValid;
STATIC BOOLEAN  mCheckpointBusy;  // reentrancy guard (periodic timer vs. events)
STATIC EFI_EVENT mTimerEvent;     // periodic safety-net checkpoint

STATIC
BOOLEAN
X96StoreHasBootableOption (
  VOID
  );

//
// Variable-driver internal state / platform wedges that must never be
// persisted or replayed:
//
//   VarErrorFlag - the FTW error flag is the variable driver's own state;
//   replaying it drives SetVariable into the fault-handling path, which
//   hangs the box.
//
//   Lang - the legacy ISO639-2 language tag.  SetVariable('Lang') has
//   wedged the variable services on this board at every replay attempt,
//   at any TPL, and always on this variable (see X96ReplayStore).  The
//   system re-derives Lang from PlatformLang on every boot through
//   BdsDxe's InitializeLanguage, and replaying PlatformLang triggers the
//   same derivation inside the variable driver, so nothing is lost.
//
STATIC CONST EFI_GUID  mVarErrorFlagGuid = EDKII_VAR_ERROR_FLAG_GUID;

STATIC
BOOLEAN
X96IsInternalVariable (
  IN CONST EFI_GUID  *Guid,
  IN CONST CHAR16    *Name
  )
{
  return (BOOLEAN)((CompareGuid (Guid, &mVarErrorFlagGuid) &&
                    (StrCmp (Name, VAR_ERROR_FLAG_NAME) == 0)) ||
                   ((CompareGuid (Guid, &gEfiGlobalVariableGuid) &&
                     (StrCmp (Name, L"Lang") == 0))));
}

/**
  Find the boot card: the BlockIo whose device path carries the MesonSdMmcDxe
  SD host controller's vendor GUID.  The eMMC never matches.

  @param[out] BlkIo  BlockIo of the boot card, or NULL.

  @retval TRUE   The boot card was found and BlkIo is valid.
  @retval FALSE  No SD BlockIo.
**/
STATIC
BOOLEAN
X96FindSdBlockIo (
  OUT EFI_BLOCK_IO_PROTOCOL  **BlkIo
  )
{
  EFI_STATUS                 Status;
  EFI_HANDLE                 *Handles;
  UINTN                      Count;
  UINTN                      Index;
  EFI_DEVICE_PATH_PROTOCOL   *DevPath;
  EFI_DEVICE_PATH_PROTOCOL   *Node;
  VENDOR_DEVICE_PATH         *Vendor;

  *BlkIo   = NULL;
  Handles  = NULL;
  Count    = 0;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiBlockIoProtocolGuid,
                  NULL,
                  &Count,
                  &Handles
                  );
  if (EFI_ERROR (Status) || (Count == 0)) {
    if (Handles != NULL) {
      FreePool (Handles);
    }

    return FALSE;
  }

  for (Index = 0; Index < Count; Index++) {
    DevPath = DevicePathFromHandle (Handles[Index]);
    if (DevPath == NULL) {
      continue;
    }

    for (Node = DevPath; !IsDevicePathEnd (Node); Node = NextDevicePathNode (Node)) {
      if ((DevicePathType (Node) != HARDWARE_DEVICE_PATH) ||
          (DevicePathSubType (Node) != HW_VENDOR_DP))
      {
        continue;
      }

      Vendor = (VENDOR_DEVICE_PATH *)Node;
      if (CompareGuid (&Vendor->Guid, &mSdHostDevicePathGuid)) {
        Status = gBS->HandleProtocol (
                        Handles[Index],
                        &gEfiBlockIoProtocolGuid,
                        (VOID **)BlkIo
                        );
        if (!EFI_ERROR (Status) && (*BlkIo != NULL)) {
          FreePool (Handles);
          return TRUE;
        }

        *BlkIo = NULL;
      }
    }
  }

  FreePool (Handles);
  return FALSE;
}

/**
  Append one variable record to the serialized image.

  @param[in,out] Offset  Current image length, updated on success.

  @retval EFI_SUCCESS        Record appended.
  @retval EFI_BUFFER_TOO_SMALL  The image buffer would overflow; the caller
                                 aborts the whole checkpoint.
**/
STATIC
EFI_STATUS
X96AppendRecord (
  IN OUT UINTN    *Offset,
  IN     EFI_GUID *Guid,
  IN     CHAR16   *Name,
  IN     UINT32    Attributes,
  IN     UINT8    *Data,
  IN     UINTN     DataSize
  )
{
  UINTN   NameBytes;
  UINTN   Need;
  UINT32  NameBytes32;
  UINT32  DataSize32;

  NameBytes   = (StrLen (Name) + 1) * sizeof (CHAR16);
  NameBytes32 = (UINT32)NameBytes;
  DataSize32  = (UINT32)DataSize;
  Need        = sizeof (EFI_GUID) + sizeof (UINT32) + NameBytes +
                sizeof (UINT32) + sizeof (UINT32) + DataSize;

  if ((*Offset + Need) > X96_STORE_MAX) {
    return EFI_BUFFER_TOO_SMALL;
  }

  CopyMem (mWorkBuf + *Offset, Guid, sizeof (EFI_GUID));
  *Offset += sizeof (EFI_GUID);

  CopyMem (mWorkBuf + *Offset, &NameBytes32, sizeof NameBytes32);
  *Offset += sizeof NameBytes32;

  CopyMem (mWorkBuf + *Offset, Name, NameBytes);
  *Offset += NameBytes;

  CopyMem (mWorkBuf + *Offset, &Attributes, sizeof Attributes);
  *Offset += sizeof Attributes;

  CopyMem (mWorkBuf + *Offset, &DataSize32, sizeof DataSize32);
  *Offset += sizeof DataSize32;

  CopyMem (mWorkBuf + *Offset, Data, DataSize);
  *Offset += DataSize;

  return EFI_SUCCESS;
}

/**
  Checkpoint the non-volatile variable store into the store file.

  @param[in] ForceWrite  Write even when the image is unchanged since the
                         last checkpoint (used before ExitBootServices).
**/
STATIC
VOID
X96CheckpointBody (
  IN BOOLEAN  ForceWrite
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlkIo;
  EFI_GUID               Guid;
  UINTN                  NameSize;
  UINTN                  DataSize;
  UINT32                 Attributes;
  UINTN                  Offset;
  UINTN                  Records;
  UINTN                  DataLength;
  UINTN                  WriteBytes;
  UINT32                 Crc;

  if (!X96FindSdBlockIo (&BlkIo)) {
    DEBUG ((DEBUG_INFO, "X96 NvVars: no SD BlockIo, checkpoint skipped\n"));
    return;
  }

  if (mWorkBuf == NULL) {
    mWorkBuf    = AllocatePool (X96_STORE_MAX);
    mScratchBuf = AllocatePool (X96_STORE_MAX);
    if ((mWorkBuf == NULL) || (mScratchBuf == NULL)) {
      return;
    }
  }

  Offset  = X96_HEADER_SIZE;
  Records = 0;
  Guid    = gZeroGuid;
  //
  // GetNextVariableName treats a non-empty caller buffer as "continue after
  // this name"; the replay pass leaves the last replayed name in this
  // shared buffer, so it must start empty here.
  //
  ZeroMem (mNameBuf, sizeof mNameBuf);
  NameSize = X96_NAME_MAX_CHARS * sizeof (CHAR16);
  //
  // The variable services require the caller's name buffer to hold a NUL
  // terminator even on the first call (VariableServiceGetNextVariableName
  // runs StrnLenS over it); a zeroed buffer terminates immediately and
  // passes that check.  Static storage is zero-init, and returned names
  // keep it terminated afterwards.
  //
  Status  = gRT->GetNextVariableName (&NameSize, mNameBuf, &Guid);
  DEBUG ((DEBUG_INFO, "X96 NvVars: enum first call -> %r\n", Status));

  while (!EFI_ERROR (Status)) {
    DataSize   = X96_STORE_MAX;
    Attributes = 0;
    Status = gRT->GetVariable (mNameBuf, &Guid, &Attributes, &DataSize, mScratchBuf);
    DEBUG ((
      DEBUG_INFO,
      "X96 NvVars: enum '%s' get -> %r attr=%x size=%x\n",
      mNameBuf,
      Status,
      Attributes,
      DataSize
      ));
    if (!EFI_ERROR (Status) && ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0)) {
      if (X96IsInternalVariable (&Guid, mNameBuf)) {
        DEBUG ((DEBUG_INFO, "X96 NvVars: skip internal '%s'\n", mNameBuf));
      } else {
        Status = X96AppendRecord (
                   &Offset,
                   &Guid,
                   mNameBuf,
                   Attributes,
                   mScratchBuf,
                   DataSize
                   );
        if (Status == EFI_BUFFER_TOO_SMALL) {
          DEBUG ((
            DEBUG_ERROR,
            "X96 NvVars: store image overflowed at '%s', checkpoint aborted\n",
            mNameBuf
            ));
          return;
        }

        Records++;
      }
    }

    NameSize = X96_NAME_MAX_CHARS * sizeof (CHAR16);
    Status   = gRT->GetNextVariableName (&NameSize, mNameBuf, &Guid);
  }

  DEBUG ((
    DEBUG_INFO,
    "X96 NvVars: enum done, %d records, last status %r\n",
    Records,
    Status
    ));

  //
  // An empty enumeration must never clobber a good store image.  The
  // ExitBootServices callback has been observed to enumerate zero records
  // in a context where the ReadyToBoot pass, minutes earlier in the same
  // boot, enumerated 22; trusting that empty pass would regress the store
  // to a count=0 header every boot and break persistence entirely.
  //
  if ((Records == 0) && mLastValid) {
    DEBUG ((
      DEBUG_ERROR,
      "X96 NvVars: checkpoint enumerated 0 records with a valid store present, keeping the existing image\n"
      ));
    return;
  }

  //
  // CalculateCrc32 wants a multiple of 4 bytes; pad the image.
  //
  DataLength = Offset - X96_HEADER_SIZE;
  while ((DataLength & 3) != 0) {
    mWorkBuf[X96_HEADER_SIZE + DataLength] = 0;
    DataLength++;
  }

  Crc = 0;
  gBS->CalculateCrc32 (
         mWorkBuf + X96_HEADER_SIZE,
         DataLength,
         &Crc
         );

  if (!ForceWrite && mLastValid && (Crc == mLastCrc)) {
    return;
  }

  CopyMem (mWorkBuf + 0, mMagic, sizeof mMagic);
  {
    UINT32  Version = 1;
    UINT32  Count   = (UINT32)Records;
    UINT32  Length  = DataLength;
    UINT32  Zero    = 0;

    CopyMem (mWorkBuf + 8, &Version, sizeof Version);
    CopyMem (mWorkBuf + 12, &Count, sizeof Count);
    CopyMem (mWorkBuf + 16, &Length, sizeof Length);
    CopyMem (mWorkBuf + 20, &Crc, sizeof Crc);
    CopyMem (mWorkBuf + 24, &Zero, sizeof Zero);
    CopyMem (mWorkBuf + 28, &Zero, sizeof Zero);
  }

  //
  // Write the image raw, one sector per single-block command, and verify
  // each sector by reading it back immediately.  Only the sectors the image
  // actually occupies are written; a shorter image over a longer one is
  // harmless because the header's length and CRC govern what a reader
  // trusts.  Single-block commands only: see the note in X96TryLoad.
  //
  if ((BlkIo->Media == NULL) || (BlkIo->Media->BlockSize != 512) ||
      (BlkIo->Media->ReadOnly))
  {
    DEBUG ((DEBUG_ERROR, "X96 NvVars: SD media unusable, checkpoint skipped\n"));
    return;
  }

  WriteBytes = ((Offset + 511) & ~(UINTN)511);
  if (WriteBytes > X96_STORE_BYTES) {
    DEBUG ((DEBUG_ERROR, "X96 NvVars: image overflows the store area\n"));
    return;
  }

  //
  // Pad the transfer buffer so a stale tail beyond the image reads back as
  // zeros, not as old garbage (the verify compares whole sectors).
  //
  ZeroMem (mWorkBuf + Offset, WriteBytes - Offset);
  WriteBytes = 0;
  {
    UINTN  Sector;
    UINTN  SectorCount;

    SectorCount = ((Offset + 511) & ~(UINTN)511) / 512;
    for (Sector = 0; Sector < SectorCount; Sector++) {
      Status = BlkIo->WriteBlocks (
                        BlkIo,
                        BlkIo->Media->MediaId,
                        X96_STORE_LBA + Sector,
                        512,
                        mWorkBuf + Sector * 512
                        );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "X96 NvVars: checkpoint write sector %d failed: %r\n",
          Sector,
          Status
          ));
        return;
      }

      Status = BlkIo->ReadBlocks (
                        BlkIo,
                        BlkIo->Media->MediaId,
                        X96_STORE_LBA + Sector,
                        512,
                        mScratchBuf
                        );
      if (!EFI_ERROR (Status) &&
          (CompareMem (mScratchBuf, mWorkBuf + Sector * 512, 512) == 0))
      {
        WriteBytes += 512;
      } else {
        DEBUG ((
          DEBUG_ERROR,
          "X96 NvVars: checkpoint verify sector %d FAILED (%r)\n",
          Sector,
          Status
          ));
        return;
      }
    }
  }

  mLastValid = TRUE;
  mLastCrc   = Crc;
  DEBUG ((
    DEBUG_INFO,
    "X96 NvVars: checkpointed %d variables (%d bytes, verified per sector)\n",
    Records,
    Offset
    ));
}

/**
  Reentrancy wrapper: the periodic timer can fire in the middle of another
  checkpoint's card I/O, and interleaved writes would corrupt the store.
  One pass at a time; overlapping requests are simply dropped (the next
  tick or boot-phase event re-checks).

  @param[in] ForceWrite  Write even when the image is unchanged since the
                         last checkpoint (used before ExitBootServices).
**/
STATIC
VOID
X96Checkpoint (
  IN BOOLEAN  ForceWrite
  )
{
  if (mCheckpointBusy) {
    return;
  }

  mCheckpointBusy = TRUE;
  X96CheckpointBody (ForceWrite);
  mCheckpointBusy = FALSE;
}

/**
  Periodic checkpoint: catches variable changes made in the UEFI Shell
  (bcfg, dmpstore) and committed with a plain reset, which bypasses both
  the ReadyToBoot and the ExitBootServices checkpoints.  The CRC-skip in
  X96Checkpoint keeps an unchanged NVRAM at pure-enumeration cost, so the
  timer touches the card only when something actually changed.
**/
STATIC
VOID
EFIAPI
X96TimerCheckpoint (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  X96Checkpoint (FALSE);
}

/**
  Event callback: checkpoint at ReadyToBoot and before ExitBootServices.
**/
STATIC
VOID
EFIAPI
X96CheckpointEvent (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  DEBUG ((DEBUG_INFO, "X96 NvVars: checkpoint event fired\n"));
  X96Checkpoint (FALSE);
}

/**
  Replay a serialized store image into the variable services.
**/
STATIC
EFI_STATUS
X96ReplayStore (
  IN UINT8   *Image,
  IN UINTN    ImageSize
  )
{
  UINT32    Count;
  UINT32    Length;
  UINT32    Crc;
  UINT32    CalcCrc;
  UINTN     Pos;
  UINTN     Records;
  UINTN     Parsed;
  EFI_GUID  Guid;
  UINT32    NameBytes;
  UINT32    Attributes;
  UINT32    DataSize;
  CHAR16    *Name;
  EFI_STATUS Status;

  if ((ImageSize < X96_HEADER_SIZE) ||
      (CompareMem (Image, mMagic, sizeof mMagic) != 0))
  {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (&Count, Image + 12, sizeof Count);
  CopyMem (&Length, Image + 16, sizeof Length);
  CopyMem (&Crc, Image + 20, sizeof Crc);

  if (((UINTN)Length != (ImageSize - X96_HEADER_SIZE)) || (Length == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  CalcCrc = 0;
  gBS->CalculateCrc32 (Image + X96_HEADER_SIZE, Length, &CalcCrc);
  if (CalcCrc != Crc) {
    DEBUG ((
      DEBUG_ERROR,
      "X96 NvVars: store file CRC bad (file %08x, calc %08x), ignored\n",
      Crc,
      CalcCrc
      ));
    return EFI_CRC_ERROR;
  }

  DEBUG ((DEBUG_INFO, "X96 NvVars: replaying %d variables...\n", Count));

  Pos     = X96_HEADER_SIZE;
  Records = 0;
  Parsed  = 0;

  //
  // Parsed counts every record consumed from the image (so a skipped
  // record cannot stall the walk); Records counts successful replays.
  //
  while (Parsed < Count) {
    if ((Pos + sizeof (EFI_GUID) + sizeof (UINT32)) > (UINTN)(X96_HEADER_SIZE + Length)) {
      return EFI_INVALID_PARAMETER;
    }

    CopyMem (&Guid, Image + Pos, sizeof Guid);
    Pos += sizeof Guid;

    CopyMem (&NameBytes, Image + Pos, sizeof NameBytes);
    Pos += sizeof NameBytes;

    if ((NameBytes == 0) ||
        ((NameBytes & 1) != 0) ||
        (NameBytes > (X96_NAME_MAX_CHARS * sizeof (CHAR16))) ||
        ((Pos + NameBytes) > (UINTN)(X96_HEADER_SIZE + Length)))
    {
      return EFI_INVALID_PARAMETER;
    }

    //
    // Copy the name into the aligned static buffer before anything reads it
    // as CHAR16: records after an odd-sized data field start at odd image
    // offsets, and the image is only byte-aligned.  StrCmp and the variable
    // services assert on unaligned CHAR16 pointers (String.c alignment
    // check), which dead-looped every replay of such a record.
    //
    CopyMem (mNameBuf, Image + Pos, NameBytes);
    if (NameBytes < (X96_NAME_MAX_CHARS * sizeof (CHAR16))) {
      mNameBuf[NameBytes / 2] = 0;
    }

    Name = mNameBuf;
    Pos                    += NameBytes;

    if ((Pos + sizeof (UINT32) + sizeof (UINT32)) > (UINTN)(X96_HEADER_SIZE + Length)) {
      return EFI_INVALID_PARAMETER;
    }

    CopyMem (&Attributes, Image + Pos, sizeof Attributes);
    Pos += sizeof Attributes;

    CopyMem (&DataSize, Image + Pos, sizeof DataSize);
    Pos += sizeof DataSize;

    if ((Pos + DataSize) > (UINTN)(X96_HEADER_SIZE + Length)) {
      return EFI_INVALID_PARAMETER;
    }

    //
    // Skip records the variable services cannot accept, and variable-driver
    // internal state (see X96IsInternalVariable): replaying the FTW error
    // flag hangs the variable services outright.
    //
    if (((Attributes & (EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS |
                        EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS)) != 0) ||
        (((Attributes & EFI_VARIABLE_RUNTIME_ACCESS) != 0) &&
         ((Attributes & EFI_VARIABLE_BOOTSERVICE_ACCESS) == 0)) ||
        X96IsInternalVariable (&Guid, Name))
    {
      DEBUG ((DEBUG_INFO, "X96 NvVars: skip unsupported '%s'\n", Name));
    } else {
      //
      // A non-volatile variable must be boot-service accessible or
      // SetVariable rejects it; hand-crafted store images (scripts/
      // x96-nvvars.py) may carry a bare NV attribute, so repair it here.
      //
      if ((Attributes & EFI_VARIABLE_NON_VOLATILE) != 0) {
        Attributes |= EFI_VARIABLE_BOOTSERVICE_ACCESS;
      }

      DEBUG ((DEBUG_INFO, "X96 NvVars: replay %d/%d '%s'...\n", Parsed + 1, Count, Name));
      Status = gRT->SetVariable (Name, &Guid, Attributes, DataSize, Image + Pos);
      DEBUG ((DEBUG_INFO, "X96 NvVars: replay '%s' -> %r\n", Name, Status));
      if (EFI_ERROR (Status)) {
        DEBUG ((DEBUG_INFO, "X96 NvVars: replay '%s' failed: %r\n", Name, Status));
      } else {
        Records++;
      }
    }

    Pos += DataSize;
    Parsed++;
  }

  DEBUG ((DEBUG_INFO, "X96 NvVars: restored %d variables from card\n", Records));
  return EFI_SUCCESS;
}

/**
  Replay the validated store image into the variable services.  Invoked by
  the boot manager from its own TPL_APPLICATION context (see the protocol
  declaration in X96NvVarsPersist.h for why this must not run from a notify
  event).

  @param[in,out] This  The persist protocol instance.

  @retval EFI_SUCCESS    Replayed (or already replayed).
  @retval EFI_NOT_READY  No validated store image (not yet read, or none).
  @retval others         First SetVariable failure from the replay pass.
**/
EFI_STATUS
X96Replay (
  IN OUT X96_NV_VARS_PERSIST_PROTOCOL  *This
  )
{
  EFI_STATUS  ReplayStatus;

  if (mReplayed) {
    return EFI_SUCCESS;
  }

  if (!mStoreReady) {
    return EFI_NOT_READY;
  }

  ReplayStatus = X96ReplayStore (mWorkBuf, mImageLength);
  mReplayed    = TRUE;
  if (!EFI_ERROR (ReplayStatus)) {
    This->PersistLoaded        = TRUE;
    This->HasBootableOption    = X96StoreHasBootableOption ();
    mPersist.PersistLoaded     = TRUE;
    mPersist.HasBootableOption = This->HasBootableOption;

    DEBUG ((
      DEBUG_INFO,
      "X96 NvVars: store restored, bootable option %a\n",
      This->HasBootableOption ? "present" : "ABSENT"
      ));
  }

  return ReplayStatus;
}

/**
  One load attempt: read the raw store image from the boot card and replay
  it.  Concludes the load phase on the first attempt (the boot manager
  signals this once, after the card is connected).

  Every sector is read with a single-block command.  This board's SD host
  wedges silently on larger multi-block transfers at BDS time (the same
  marginality that shows in the vendor chain's own SD timeouts), while
  single-block reads -- what the identification and FAT-metdata paths have
  always used -- have never failed.
**/
STATIC
VOID
X96TryLoad (
  VOID
  )
{
  EFI_STATUS             Status;
  EFI_BLOCK_IO_PROTOCOL  *BlkIo;
  UINT32                 HeaderLength;
  UINTN                  Sectors;
  UINTN                  Index;

  if (!X96FindSdBlockIo (&BlkIo)) {
    DEBUG ((DEBUG_INFO, "X96 NvVars: load - no SD BlockIo\n"));
    mLoadDone = TRUE;
    return;
  }

  if ((BlkIo->Media == NULL) || (BlkIo->Media->BlockSize != 512)) {
    DEBUG ((DEBUG_ERROR, "X96 NvVars: load - SD media unusable\n"));
    mLoadDone = TRUE;
    return;
  }

  //
  // Header sector first: it says how much of the store is real.
  //
  Status = BlkIo->ReadBlocks (
                    BlkIo,
                    BlkIo->Media->MediaId,
                    X96_STORE_LBA,
                    512,
                    mWorkBuf
                    );
  DEBUG ((DEBUG_INFO, "X96 NvVars: load header read -> %r\n", Status));
  if (EFI_ERROR (Status)) {
    mLoadDone = TRUE;
    return;
  }

  if (CompareMem (mWorkBuf, mMagic, sizeof mMagic) != 0) {
    //
    // Virgin store: nothing to restore.  The boot manager sees no persisted
    // protocol and registers its fallback boot options, and the first
    // checkpoint writes the store.
    //
    mLoadDone = TRUE;
    DEBUG ((DEBUG_INFO, "X96 NvVars: no store image on card, starting fresh\n"));
    return;
  }

  CopyMem (&HeaderLength, mWorkBuf + 16, sizeof HeaderLength);
  if ((HeaderLength == 0) ||
      ((UINTN)HeaderLength > (X96_STORE_BYTES - X96_HEADER_SIZE)))
  {
    DEBUG ((
      DEBUG_ERROR,
      "X96 NvVars: store header length %x invalid, starting fresh\n",
      HeaderLength
      ));
    mLoadDone = TRUE;
    return;
  }

  Sectors = ((UINTN)HeaderLength + X96_HEADER_SIZE + 511) / 512;
  for (Index = 1; Index < Sectors; Index++) {
    Status = BlkIo->ReadBlocks (
                      BlkIo,
                      BlkIo->Media->MediaId,
                      X96_STORE_LBA + Index,
                      512,
                      mWorkBuf + Index * 512
                      );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "X96 NvVars: load sector %d read failed: %r\n", Index, Status));
      mLoadDone = TRUE;
      return;
    }
  }

  mImageLength = X96_HEADER_SIZE + HeaderLength;
  mStoreReady  = TRUE;
  mLoadDone    = TRUE;
  //
  // The image is validated and waiting.  The boot manager, still in its own
  // TPL_APPLICATION context, calls the persist protocol's Replay() right
  // after it signalled this load event: SetVariable from a TPL_CALLBACK
  // context wedges the variable services on this platform, so the replay
  // pass must run where the boot manager's own SetVariable calls run.
  //
  DEBUG ((DEBUG_INFO, "X96 NvVars: store image validated, awaiting replay call\n"));
}

/**
  ASCII hex-digit test for Boot#### name filtering.
**/
STATIC
BOOLEAN
IsHexDigit (
  IN CHAR16  Char
  )
{
  return (((Char >= L'0') && (Char <= L'9')) ||
          ((Char >= L'A') && (Char <= L'F')) ||
          ((Char >= L'a') && (Char <= L'f')));
}

/**
  Decide whether the restored NVRAM can boot anything on its own: any
  Boot#### entry that is ACTIVE and is not a CATEGORY_APP entry (UiApp /
  Firmware Setup are stored as boot options too, but BDS never auto-boots
  them).  Per the UEFI spec, category 0 (LOAD_OPTION_CATEGORY_BOOT) is the
  default for plain boot options, so only the APP category disqualifies.

  @retval TRUE   At least one active boot option exists.
  @retval FALSE  The store cannot boot without the fallbacks.
**/
STATIC
BOOLEAN
X96StoreHasBootableOption (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_GUID    Guid;
  UINTN       NameSize;
  UINTN       DataSize;
  UINT32      Attributes;
  UINT32      LoadOptionAttributes;
  BOOLEAN     Bootable;

  Guid    = gZeroGuid;
  //
  // Same contract as in X96Checkpoint: the buffer must start empty.
  //
  ZeroMem (mNameBuf, sizeof mNameBuf);
  NameSize = X96_NAME_MAX_CHARS * sizeof (CHAR16);
  Status   = gRT->GetNextVariableName (&NameSize, mNameBuf, &Guid);

  while (!EFI_ERROR (Status)) {
    Bootable = FALSE;

    //
    // Boot option variables are named "Boot" + 4 hex digits; this rejects
    // BootOrder, BootCurrent, BootNext and friends.
    //
    if ((StrLen (mNameBuf) == 8) && (StrnCmp (mNameBuf, L"Boot", 4) == 0) &&
        IsHexDigit (mNameBuf[4]) && IsHexDigit (mNameBuf[5]) &&
        IsHexDigit (mNameBuf[6]) && IsHexDigit (mNameBuf[7]))
    {
      //
      // The option data is variable-length (attributes + description +
      // device path); a minimal 4-byte buffer makes GetVariable fail with
      // EFI_BUFFER_TOO_SMALL, which silently disqualified every active
      // entry.  Read the whole option into the big scratch buffer and take
      // the attributes from its head.
      //
      DataSize = X96_STORE_MAX;
      Status   = gRT->GetVariable (
                       mNameBuf,
                       &gEfiGlobalVariableGuid,
                       &Attributes,
                       &DataSize,
                       mScratchBuf
                       );
      if (!EFI_ERROR (Status) && (DataSize >= sizeof (UINT32))) {
        CopyMem (&LoadOptionAttributes, mScratchBuf, sizeof (LoadOptionAttributes));
        Bootable = ((LoadOptionAttributes & LOAD_OPTION_ACTIVE) != 0) &&
                   ((LoadOptionAttributes & LOAD_OPTION_CATEGORY) != LOAD_OPTION_CATEGORY_APP);
      }
    }

    if (Bootable) {
      return TRUE;
    }

    NameSize = X96_NAME_MAX_CHARS * sizeof (CHAR16);
    Status   = gRT->GetNextVariableName (&NameSize, mNameBuf, &Guid);
  }

  return FALSE;
}


/**
  Load-trigger notification: the boot manager signals this once the boot
  card's filesystem is connected and the controller churn has settled.
  Loading here is serialized with the BDS flow instead of racing the
  controller connects that used to be in flight when a SimpleFileSystem
  protocol notification fired.
**/
STATIC
VOID
EFIAPI
X96LoadEventNotify (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  if (mLoadDone) {
    return;
  }

  DEBUG ((DEBUG_INFO, "X96 NvVars: load event signalled\n"));
  X96TryLoad ();
}

/**
  Manual checkpoint, exposed through the persist protocol to the 'nvsave'
  shell command: saves the current NVRAM to the card on demand.

  @param[in,out] This  The persist protocol instance.

  @retval EFI_SUCCESS  The store image on the card now matches the NVRAM.
**/
EFI_STATUS
X96SaveNow (
  IN OUT X96_NV_VARS_PERSIST_PROTOCOL  *This
  )
{
  X96Checkpoint (FALSE);
  return EFI_SUCCESS;
}

/**
  Driver entry point.
**/
EFI_STATUS
EFIAPI
X96NvVarsPersistDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS  Status;
  EFI_EVENT   Event;

  mWorkBuf    = AllocatePool (X96_STORE_MAX);
  mScratchBuf = AllocatePool (X96_STORE_MAX);
  if ((mWorkBuf == NULL) || (mScratchBuf == NULL)) {
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INFO, "X96 NvVars: driver entry\n"));

  //
  // Publish the persist protocol right away: the boot manager drives the
  // replay through it (see X96Replay), and its presence with
  // PersistLoaded=FALSE means "nothing restored yet" to every consumer.
  //
  mPersist.Replay            = X96Replay;
  mPersist.Checkpoint        = X96SaveNow;
  mPersist.PersistLoaded     = FALSE;
  mPersist.HasBootableOption = FALSE;
  mPersistHandle             = NULL;
  Status = gBS->InstallProtocolInterface (
                  &mPersistHandle,
                  &gX96NvVarsPersistProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &mPersist
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "X96 NvVars: protocol install failed: %r\n", Status));
    return Status;
  }

  //
  // Wait for the boot manager to signal the load event; the store is
  // validated here, replayed by the boot manager itself right after.
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  X96LoadEventNotify,
                  NULL,
                  &gX96NvVarsLoadEventGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Checkpoint at ReadyToBoot (covers firmware-authored changes, e.g. boot
  // entries registered by the boot manager) and before ExitBootServices
  // (covers changes made afterwards, e.g. from the Shell).
  //
  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  X96CheckpointEvent,
                  NULL,
                  &gEfiEventReadyToBootGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  X96CheckpointEvent,
                  NULL,
                  &gEfiEventBeforeExitBootServicesGuid,
                  &Event
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // Periodic safety net: a Shell session that ends with a plain reset
  // never reaches ReadyToBoot or ExitBootServices, so bcfg-created boot
  // options would die with the emulated NVRAM.  Every 10 s the checkpoint
  // re-enumerates the store and rewrites the card only when the CRC
  // changed.
  //
  mTimerEvent = NULL;
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  TPL_CALLBACK,
                  X96TimerCheckpoint,
                  NULL,
                  &mTimerEvent
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (mTimerEvent, TimerPeriodic, EFI_TIMER_PERIOD_SECONDS (10));
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "X96 NvVars: checkpoint timer failed: %r\n", Status));
    }
  }

  return EFI_SUCCESS;
}
