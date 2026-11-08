/** @file
  X96 Linux Boot - u-boot "booti"-style Linux handoff from EDK2.
  Copyright (c) 2026. BSD-2-Clause-Patent.
**/

#include <Uefi.h>
#include <Pi/PiBootMode.h>
#include <Pi/PiHob.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/ArmLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/FdtLib.h>
#include <Library/PcdLib.h>
#include <Library/HobLib.h>

#include <Guid/Fdt.h>
#include <Guid/FdtHob.h>
#include <Guid/HobList.h>
#include <Guid/FileInfo.h>
#include <Protocol/SimpleFileSystem.h>

#define ARM64_IMAGE_MAGIC              SIGNATURE_32 ('A','R','M',0x64)
#define ARM64_IMAGE_MAGIC_OFFSET       0x38
#define ARM64_IMAGE_TEXT_OFFSET_OFF    0x08
#define ARM64_IMAGE_SIZE_OFF           0x10

#define KERNEL_TARGET_BASE  0x01080000ULL
#define INITRD_TARGET_BASE  0x15000000ULL
#define FDT_TARGET_BASE     0x0B000000ULL

#define KERNEL_MAX        (192U * 1024 * 1024)
#define INITRD_MAX        (128U * 1024 * 1024)
#define FDT_MAX           (512U * 1024)
#define FDT_SLACK         (96U * 1024)

//
// The kernel command line handed off to the kernel through
// /chosen/bootargs (u-boot/win32 boot protocol style).
//
#define CMDLINE_MAX  512

CHAR16  mKernelPath[]  = L"\\vmlinuz-6.6.74-happy-new-year";
CHAR16  mInitrdPath[]  = L"\\initrd.img-6.6.74-happy-new-year";
CHAR16  mCmdline[]     = L"console=ttyAML0,115200n8 root=UUID=4e793417-9f2f-4ead-a796-eb92da3db26a rw rootfstype=ext4 rootflags=data=writeback net.ifnames=0";

//
// Handoff helper in X96LinuxBootJump.S.  C ABI: (x0 = entry point,
// x1 = physical FDT pointer).  The helper moves x0->x8, copies x1 into
// the arm64 Linux boot protocol's x0, zeroes x1..x3, then branches.
//
VOID
EFIAPI
X96LinuxBootHandoff (
  IN UINT64  Entry,
  IN UINT64  Fdt
  );

VOID
EFIAPI
X96CpuDehydrate (VOID);

VOID
EFIAPI
X96LinuxFinal (
  IN UINT64  Entry,
  IN UINT64  Fdt
  );

STATIC
EFI_STATUS
ReadFileToTarget (
  IN EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem,
  IN CHAR16                           *Path,
  IN EFI_PHYSICAL_ADDRESS             Target,
  IN UINTN                            MaxSize,
  OUT UINTN                           *LoadedSize,
  IN BOOLEAN                          CheckMagic
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *Root;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *FileInfo;
  UINTN              FileInfoSize;
  UINTN              Bytes;
  UINT8              Header[64];
  UINT64             TextOffset;

  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    Print (L"X96Linux: OpenVolume(): %r\n", Status);
    return Status;
  }

  Status = Root->Open (Root, &File, Path, EFI_FILE_MODE_READ, 0);
  if (EFI_ERROR (Status)) {
    Root->Close (Root);
    Print (L"X96Linux: Open(%s): %r\n", Path, Status);
    return Status;
  }

  FileInfoSize = 0;
  Status       = File->GetInfo (File, &gEfiFileInfoGuid, &FileInfoSize, NULL);
  if (Status == EFI_BUFFER_TOO_SMALL) {
    FileInfo = AllocatePool (FileInfoSize);
    if (FileInfo != NULL) {
      Status = File->GetInfo (File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    }
  }
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if ((UINT64)FileInfo->FileSize > (UINT64)MaxSize) {
    FreePool (FileInfo);
    File->Close (File);
    Print (L"X96Linux: %s is %llu bytes, limit %llu.\n", Path,
           (UINT64)FileInfo->FileSize, (UINT64)MaxSize);
    return EFI_INVALID_PARAMETER;
  }

  Bytes  = sizeof Header;
  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = File->Read (File, &Bytes, Header);
  if (EFI_ERROR (Status) || (Bytes < ARM64_IMAGE_MAGIC_OFFSET + 4)) {
    FreePool (FileInfo);
    File->Close (File);
    Print (L"X96Linux: %s header read failed.\n", Path);
    return EFI_LOAD_ERROR;
  }

  if (CheckMagic &&
      (ReadUnaligned32 ((UINT32 *)(Header + ARM64_IMAGE_MAGIC_OFFSET)) != ARM64_IMAGE_MAGIC))
  {
    Print (L"X96Linux: %s is not an arm64 Image.\n", Path);
    FreePool (FileInfo);
    File->Close (File);
    return EFI_LOAD_ERROR;
  }

  CopyMem (&TextOffset, Header + ARM64_IMAGE_TEXT_OFFSET_OFF, sizeof (TextOffset));

  Status = File->SetPosition (File, 0);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Bytes  = (UINTN)FileInfo->FileSize;
  Status = File->Read (File, &Bytes, (VOID *)(UINTN)Target);
  if (EFI_ERROR (Status)) {
    Print (L"X96Linux: read %s failed: %r\n", Path, Status);
    File->Close (File);
    FreePool (FileInfo);
    return Status;
  }

  File->Close (File);
  FreePool (FileInfo);

  InvalidateDataCacheRange ((VOID *)(UINTN)Target, Bytes);
  Print (L"X96Linux: %s -> 0x%016llx (%llu bytes, text_offset=0x%llx)\n",
         Path, (UINT64)Target, (UINT64)Bytes, TextOffset);

  //
  // The booti semantics require the tail of the image space to be
  // zeroed when image_size exceeds the file size (rel0 fixup).
  //
  *LoadedSize = Bytes;

  return EFI_SUCCESS;
}

STATIC
EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *
FindBootFileSystem (
  VOID
  )
{
  EFI_STATUS                       Status;
  UINTN                            HandleCount;
  EFI_HANDLE                       *Handles;
  UINTN                            Index;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *FileSystem;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    FileSystem = NULL;
    Status = gBS->HandleProtocol (
                    Handles[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&FileSystem
                    );
    if (!EFI_ERROR (Status) && (FileSystem != NULL)) {
      FreePool (Handles);
      return FileSystem;
    }
  }

  FreePool (Handles);
  return NULL;
}

STATIC
EFI_STATUS
PrepareFdt (
  OUT EFI_PHYSICAL_ADDRESS  *FdtTarget
  )
{
  EFI_STATUS            Status;
  VOID                  *Source;
  VOID                  *Copy;
  INT32                 Chosen;
  UINTN                 SizeWithSlack;
  UINTN                 CmdlineBytes;

  *FdtTarget = FDT_TARGET_BASE;

  //
  // Prefer the firmware-published DeviceTree configuration table - the
  // same source the Linux EFI stub picks its DTB from...
  //
  Source = NULL;
  Status = EfiGetSystemConfigurationTable (&gFdtTableGuid, &Source);
  if (EFI_ERROR (Status) || (Source == NULL) || (FdtCheckHeader (Source) != 0)) {
    //
    // DxeCore published the original PrePi HOB list under gEfiHobListGuid;
    // our PlatformPeiLib recorded the normalised FDT in a gFdtHobGuid HOB.
    //
    VOID  *HobList;

    Status = EfiGetSystemConfigurationTable (&gEfiHobListGuid, &HobList);
    if (!EFI_ERROR (Status) && (HobList != NULL)) {
        EFI_HOB_GUID_TYPE  *Hob;

      Hob = GetFirstGuidHob (&gFdtHobGuid);
      if (Hob != NULL) {
        EFI_PHYSICAL_ADDRESS  HobVal;

        CopyMem (&HobVal, GET_GUID_HOB_DATA (Hob), sizeof HobVal);
        Source = (VOID *)(UINTN)HobVal;
      }
    }
  }

  if (EFI_ERROR (Status) || (Source == NULL) || (FdtCheckHeader (Source) != 0)) {
    Source = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  }


  if ((Source == NULL) || (FdtCheckHeader (Source) != 0)) {
    Print (L"X96Linux: no DeviceTree reachable (config table + FV probe failed)\n");
    return EFI_NOT_FOUND;
  }

  SizeWithSlack = FdtTotalSize (Source) + FDT_SLACK;

  Status = gBS->AllocatePages (
                  AllocateAddress,
                  EfiBootServicesData,
                  EFI_SIZE_TO_PAGES (SizeWithSlack),
                  FdtTarget
                  );
  if (EFI_ERROR (Status)) {
    //
    // The fixed target was already taken (or not available); fall back
    // to any memory - the handoff passes whichever address was given.
    //
    Print (L"X96Linux: fixed FDT target busy, falling back\n");
    Status = gBS->AllocatePages (
                    AllocateAnyPages,
                    EfiBootServicesData,
                    EFI_SIZE_TO_PAGES (SizeWithSlack),
                    FdtTarget
                    );
    if (EFI_ERROR (Status)) {
      Print (L"X96Linux: FDT any-page alloc: %r\n", Status);
      return Status;
    }
  }

  Copy = (VOID *)(UINTN)*FdtTarget;
  ZeroMem (Copy, SizeWithSlack);
  if (FdtOpenInto (Source, Copy, SizeWithSlack) != 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  Chosen = FdtPathOffset (Copy, "/chosen");
  if (Chosen < 0) {
    Chosen = FdtAddSubnode (Copy, 0, "chosen");
    if (Chosen < 0) {
      return EFI_DEVICE_ERROR;
    }
  }

  CmdlineBytes = (2 * StrLen (mCmdline) + 2) * sizeof (CHAR16);
  if (FdtSetProp (Copy, Chosen, "bootargs", mCmdline, CmdlineBytes / 2) != 0) {
    return EFI_DEVICE_ERROR;
  }

  FdtDelProp (Copy, Chosen, "linux,initrd-start");
  FdtDelProp (Copy, Chosen, "linux,initrd-end");

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
X96LinuxEntryPoint (
  IN EFI_HANDLE                    ImageHandle,
  IN EFI_SYSTEM_TABLE              *SystemTable
  )
{
  EFI_STATUS                         Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL    *FileSystem;
  EFI_PHYSICAL_ADDRESS               KernelBase;
  EFI_PHYSICAL_ADDRESS               InitrdBase;
  EFI_PHYSICAL_ADDRESS               FdtBase;
  UINTN                              LoadedSize;
  UINTN                              InitrdSize;
  CHAR8                              Header[64];
  UINT64                             TextOffset;
  UINT64                             ImageSize;
  EFI_STATUS                         Status2;
  UINTN                              Pages;

  Print (L"\nX96Linux (booti-style booter) for X96 Max+ build.\n");
  {
    UINT64  CurEl;
    __asm__ volatile ("mrs %0, currentel" : "=r" (CurEl));
    Print (L"X96Linux: CurrentEL raw 0x%lx (EL%a)\n", CurEl,
           ((((CurEl >> 2) & 3) == 1) ? L"1" : (((CurEl >> 2) & 3) == 2) ? L"2" : L"3"));
  }

  FileSystem = FindBootFileSystem ();
  if (FileSystem == NULL) {
    Print (L"X96Linux: no SimpleFileSystem yet.\n");
    return EFI_SUCCESS;
  }

  Status = PrepareFdt (&FdtBase);
  if (EFI_ERROR (Status)) {
    Print (L"X96Linux: FDT prep: %r\n", Status);
    return EFI_SUCCESS;
  }
  Print (L"X96Linux: FDT copy at 0x%llx OK.\n", FdtBase);

  KernelBase = 0;
  Pages      = EFI_SIZE_TO_PAGES (KERNEL_MAX);
  Status     = gBS->AllocatePages (AllocateAnyPages, EfiLoaderData, Pages + 1, &KernelBase);
  if (EFI_ERROR (Status)) {
    Print (L"X96Linux: kernel any-page alloc: %r\n", Status);
    return EFI_SUCCESS;
  }
  Print (L"X96Linux: kernel raw alloc 0x%llx\n", (UINT64)KernelBase);
  // round UP to a 2 MiB boundary and drop the extra leading page
  {
    EFI_PHYSICAL_ADDRESS  Aligned = (KernelBase + 0x1FFFFFULL) & ~0x1FFFFFULL;
    UINTN                 ExtraPages = (UINTN)((Aligned - KernelBase) / EFI_PAGE_SIZE);
    if (ExtraPages > 0) {
      gBS->FreePages (KernelBase, ExtraPages);
    }
    KernelBase = Aligned;
  }

  Status = ReadFileToTarget (FileSystem, mKernelPath, KernelBase, KERNEL_MAX, &LoadedSize, TRUE);
  if (EFI_ERROR (Status)) {
    return EFI_SUCCESS;
  }
  ZeroMem (Header, sizeof (Header));
  CopyMem (Header, (VOID *)(UINTN)KernelBase, sizeof (Header));
  CopyMem ((VOID *)&TextOffset, Header + ARM64_IMAGE_TEXT_OFFSET_OFF, sizeof TextOffset);
  CopyMem ((VOID *)&ImageSize, Header + ARM64_IMAGE_SIZE_OFF, sizeof ImageSize);
  DEBUG ((DEBUG_INFO, "X96Linux: kernel at 0x%016llx: size 0x%lx\n", KernelBase, ImageSize));
  Print (L"X96Linux: kernel at 0x%016llx size 0x%lx\n", KernelBase, ImageSize);
  Print (L"X96Linux: text_offset 0x%lx, entry 0x%016llx (image base)\n", TextOffset, KernelBase);
  //
  // booti semantics: the whole Image lives at a 2MiB-aligned base and the
  // branch goes to the Image's first byte; text_offset only influences
  // where u-boot relocates it.  Our copy IS at those bytes, so branch
  // there.  The FDT pointer goes in x0, x1..x3 zeroed in the asm.
  //

  InitrdBase = INITRD_TARGET_BASE;
  Pages      = EFI_SIZE_TO_PAGES (INITRD_MAX);
  Status2    = gBS->AllocatePages (AllocateAddress, EfiBootServicesData, Pages, &InitrdBase);
  if (EFI_ERROR (Status2)) {
    Print (L"X96Linux: initrd fixed target busy (%r); using any pages\n", Status2);
    InitrdBase = 0;
    Status2    = gBS->AllocatePages (AllocateAnyPages, EfiBootServicesData, Pages, &InitrdBase);
    if (EFI_ERROR (Status2)) {
      Print (L"X96Linux: initrd any-page alloc: %r\n", Status2);
      return EFI_SUCCESS;
    }
  }
  Print (L"X96Linux: initrd target 0x%llx\n", (UINT64)InitrdBase);

  Status2 = ReadFileToTarget (FileSystem, mInitrdPath, InitrdBase, INITRD_MAX, &InitrdSize, FALSE);
  if (EFI_ERROR (Status2)) {
    Print (L"X96Linux: no initrd (%r); booting without it.\n", Status2);
    InitrdSize = 0;
  } else {
    //
    // Add the initrd memory ranges to the FDT's "/chosen" so the kernel
    // sees them (booti boot protocol)
    //
    VOID    *FdtCombo = (VOID *)(UINTN)FdtBase;
    INT32   Chosen;
    UINT64  StartVal = InitrdBase;
    UINT64  EndVal   = InitrdBase + InitrdSize;

    Chosen = FdtPathOffset (FdtCombo, "/chosen");
    if (Chosen >= 0) {
      FdtSetProp (FdtCombo, Chosen, "linux,initrd-start", &StartVal, sizeof StartVal);
      FdtSetProp (FdtCombo, Chosen, "linux,initrd-end", &EndVal, sizeof EndVal);
      Print (L"X96Linux: initrd at 0x%llx - 0x%llx\n", StartVal, EndVal);
    }
  }

  Print (L"X96Linux: cleanup-before-linux (EL2)...\n");
  //
  // Cleanup-before-linux.  Each step is individually announced: if anything
  // raises an exception the last printed step names the offender.
  //
  Print (L"X96Linux: [1/6] write-back invalidate D-cache\n");
  WriteBackInvalidateDataCache ();
  Print (L"X96Linux: [2/6] D-cache invalidate\n");
  InvalidateDataCache ();
  Print (L"X96Linux: [3/6] I-cache invalidate\n");
  InvalidateInstructionCache ();
  Print (L"X96Linux: [4/6..6/6] final asm: dehydrate + branch (never returns)\n");
  //
  // No return after this point.  Kernel boot at EL2, x0=FDT, x1..x3=0.
  //
  X96LinuxFinal ((UINT64)KernelBase, FdtBase);

  return EFI_SUCCESS;  // never reached
}
