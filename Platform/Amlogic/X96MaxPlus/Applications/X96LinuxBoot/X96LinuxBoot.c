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
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
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

CHAR16  mKernelPath[]  = L"\\vmlinuz-6.12.114-ophub";
CHAR16  mInitrdPath[]  = L"\\initrd.img-6.12.114-ophub";
CHAR8   mCmdline[]     = "console=ttyAML0,115200n8 root=/dev/mmcblk1p2 rootwait rootfstype=ext4 rw loglevel=7";

//
// Optional command-line override, parsed from the UEFI Shell's
// LoadedImage->LoadOptions when the app was started with arguments:
//
//   X96LinuxBoot.efi \vmlinuz-X \initrd.img-X root=/dev/sda1
//
// The first path (or the one containing "vmlinuz") selects the kernel
// image, the second (containing "initrd"/"initramfs") the initrd, and
// every other token is appended to the kernel command line.
//
STATIC CHAR16  mArgKernelPath[256];
STATIC CHAR16  mArgInitrdPath[256];
STATIC CHAR8   mArgCmdline[768];
STATIC CHAR8   mBootCmdlineBuf[1024];
STATIC CHAR8  *mBootCmdline = mCmdline;

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

  CmdlineBytes = AsciiStrLen (mBootCmdline) + 1;   // FDT wants NUL-terminated ASCII bytes
  if (FdtSetProp (Copy, Chosen, "bootargs", mBootCmdline, CmdlineBytes) != 0) {
    return EFI_DEVICE_ERROR;
  }
  Print (L"X96Linux: bootargs set (%d bytes ascii): %a\n", CmdlineBytes, mBootCmdline);

  FdtDelProp (Copy, Chosen, "linux,initrd-start");
  FdtDelProp (Copy, Chosen, "linux,initrd-end");

  return EFI_SUCCESS;
}

//
// Case-insensitive substring search over a bounded CHAR16 token.
//
STATIC
UINTN
X96Str16Sub (
  IN CONST CHAR16  *Sub,
  IN CONST CHAR16  *Tok,
  IN UINTN          TokLen
  )
{
  UINTN  SubLen = StrLen (Sub);

  if (TokLen < SubLen) {
    return (UINTN)-1;
  }

  for (UINTN  s = 0; s + SubLen <= TokLen; s++) {
    UINTN  k;

    for (k = 0; k < SubLen; k++) {
      CHAR16  a = Tok[s + k];
      CHAR16  b = Sub[k];
      CHAR16  aa = (a >= L'A') && (a <= L'Z') ? a + 32 : a;
      CHAR16  bb = (b >= L'A') && (b <= L'Z') ? b + 32 : b;

      if (aa != bb) {
        break;
      }
    }
    if (k == SubLen) {
      return s;
    }
  }

  return (UINTN)-1;
}

STATIC
VOID
X96Copy16To (
  OUT CHAR16    *Dst,
  IN  UINTN      DstSize,
  IN  CHAR16    *Tok,
  IN  UINTN      TokLen
  )
{
  UINTN  k;

  if (TokLen >= DstSize) {
    TokLen = DstSize - 1;
  }
  for (k = 0; k < TokLen; k++) {
    Dst[k] = Tok[k];
  }
  Dst[k] = 0;
}

//
// Convert a host (little-endian) UINT64 into FDT-property byte order
// (big-endian per the device-tree specification).
//
STATIC
UINT64
FdtHostToBe64 (
  UINT64  v
  )
{
  UINT64  r = 0;
  UINTN   i;

  for (i = 0; i < 8; i++) {
    r = (r << 8) | (v & 0xff);
    v >>= 8;
  }

  return r;
}

// Classify one CHAR16 token from the command line:
//  - contains "vmlinuz"      -> kernel image path
//  - contains "initrd"/"initramfs" or ends ".img" -> initrd path
//  - starts with '\\' and uncategorised -> first-unknown = kernel, next = initrd
//  - contains '='            -> kernel command line fragment
//
STATIC
VOID
X96TokenInto (
  IN  CHAR16    *Tok,
  IN  UINTN      TokLen,
  IN  BOOLEAN    KernelTaken,
  IN  BOOLEAN    InitrdTaken
  )
{
  UINTN   j;
  BOOLEAN HasEq  = FALSE;
  BOOLEAN IsPath = (TokLen > 0) && (Tok[0] == L'\\');

  for (j = 0; j < TokLen; j++) {
    if ((Tok[j] == L'=') || (Tok[j] == L' ')) {
      HasEq = TRUE;
      break;
    }
  }

  if (HasEq) {
    // kernel command line fragment (ASCII only)
    UINTN  k = AsciiStrLen (mArgCmdline);
    UINTN  n;

    if (k > 0) {
      mArgCmdline[k++] = ' ';
    }
    for (n = 0; (n < TokLen) && (k < sizeof (mArgCmdline) - 1); n++) {
      mArgCmdline[k++] = (CHAR8)Tok[n];
    }
    mArgCmdline[k] = 0;
    return;
  }

  if ((j = X96Str16Sub (L"vmlinuz", Tok, TokLen)) != (UINTN)-1) {
    X96Copy16To (mArgKernelPath, sizeof (mArgKernelPath), Tok, TokLen);
    return;
  }
  if ((X96Str16Sub (L"initrd", Tok, TokLen) != (UINTN)-1) ||
      (X96Str16Sub (L"initramfs", Tok, TokLen) != (UINTN)-1) ||
      ((TokLen > 4) && (Tok[TokLen - 4] == L'.') &&
       (Tok[TokLen - 3] == L'i') && (Tok[TokLen - 2] == L'm') &&
       (Tok[TokLen - 1] == L'g'))) {
    X96Copy16To (mArgInitrdPath, sizeof (mArgInitrdPath), Tok, TokLen);
    return;
  }

  if (IsPath) {
    if (!KernelTaken && (mArgKernelPath[0] == 0)) {
      X96Copy16To (mArgKernelPath, sizeof (mArgKernelPath), Tok, TokLen);
    } else if (!InitrdTaken && (mArgInitrdPath[0] == 0)) {
      X96Copy16To (mArgInitrdPath, sizeof (mArgInitrdPath), Tok, TokLen);
    }
  }
}

//
// Parse the UEFI Shell's LoadOptions (CHAR16, NUL-terminated) into
// mArgKernelPath / mArgInitrdPath / mArgCmdline.  Also handles the
// app name itself being present as the first option.
//
STATIC
VOID
X96ParseLoadOptions (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_STATUS                  Status;
  EFI_LOADED_IMAGE_PROTOCOL   *Li;
  CHAR16                      *Opts;
  UINTN                        Len;
  UINTN                        i;

  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Li);
  if (EFI_ERROR (Status) || (Li == NULL) || (Li->LoadOptionsSize == 0) || (Li->LoadOptions == NULL)) {
    return;
  }

  Opts = (CHAR16 *)Li->LoadOptions;
  Len  = Li->LoadOptionsSize / sizeof (CHAR16);

  mArgKernelPath[0] = 0;
  mArgInitrdPath[0] = 0;
  mArgCmdline[0]    = 0;

  i = 0;
  //
  // Skip the app name, if present (an "efi"-suffixed, non-path token).
  //
  while ((i < Len) && (Opts[i] != 0) && (Opts[i] == L' ')) {
    i++;
  }
  if ((i < Len) && (Opts[i] != 0)) {
    UINTN  Start = i;

    while ((i < Len) && (Opts[i] != 0) && (Opts[i] != L' ')) {
      i++;
    }
    if ((i > Start) && (i - Start > 4) && (Opts[i - 4] == L'.') &&
        (i - 3 < Len) && ((Opts[i - 3] | 32) == L'e') &&
        ((Opts[i - 2] | 32) == L'f') && ((Opts[i - 1] | 32) == L'i') && (Opts[Start] != L'\\')) {
      // skip it
    } else {
      X96TokenInto (Opts + Start, i - Start, FALSE, FALSE);
    }
  }

  while ((i < Len) && (Opts[i] != 0)) {
    while ((i < Len) && (Opts[i] == L' ')) {
      i++;
    }
    if ((i >= Len) || (Opts[i] == 0)) {
      break;
    }
    {
      UINTN  Start = i;

      while ((i < Len) && (Opts[i] != 0) && (Opts[i] != L' ')) {
        i++;
      }
      X96TokenInto (Opts + Start, i - Start, FALSE, FALSE);
    }
  }

  //
  // Compose the effective bootargs: console if the user did not supply
  // one, user fragments, then the default root= if omitted.
  //
  if (mArgCmdline[0] != 0) {
    if (AsciiStrStr (mArgCmdline, "console=") == NULL) {
      AsciiStrCpyS (mBootCmdlineBuf, sizeof (mBootCmdlineBuf), "console=ttyAML0,115200n8 ");
    }
    else {
      mBootCmdlineBuf[0] = 0;
    }
    AsciiStrCatS (mBootCmdlineBuf, sizeof (mBootCmdlineBuf), mArgCmdline);
    if (AsciiStrStr (mArgCmdline, "root=") == NULL) {
      AsciiStrCatS (mBootCmdlineBuf, sizeof (mBootCmdlineBuf), " root=/dev/mmcblk1p2 rootwait");
    }
    mBootCmdline = mBootCmdlineBuf;
  }

  Print (L"X96Linux: shell ctx: %ls\n", (mArgKernelPath[0] != 0) ? mArgKernelPath : mKernelPath);
}

//
// Locate the disk that contains the boot data (Searching EfiSimpleFileSystem).
//
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

  X96ParseLoadOptions (ImageHandle);

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

  Status = ReadFileToTarget (FileSystem, (mArgKernelPath[0] != 0) ? mArgKernelPath : mKernelPath, KernelBase, KERNEL_MAX, &LoadedSize, TRUE);
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

  Status2 = ReadFileToTarget (FileSystem, (mArgInitrdPath[0] != 0) ? mArgInitrdPath : mInitrdPath, InitrdBase, INITRD_MAX, &InitrdSize, FALSE);
  if (EFI_ERROR (Status2)) {
    Print (L"X96Linux: no initrd (%r); booting without it.\n", Status2);
    InitrdSize = 0;
  } else {
    //
    // Add the initrd memory ranges to the FDT's "/chosen" so the kernel
    // sees them (booti boot protocol).  FDT property data is big-endian
    // per the device-tree spec: writing the host (LE) bytes verbatim
    // makes the kernel read 0x1500000000 instead of 0x15000000.
    //
    VOID    *FdtCombo = (VOID *)(UINTN)FdtBase;
    INT32   Chosen;
    UINT64  StartVal = FdtHostToBe64 (InitrdBase);
    UINT64  EndVal   = FdtHostToBe64 (InitrdBase + InitrdSize);

    Chosen = FdtPathOffset (FdtCombo, "/chosen");
    if (Chosen >= 0) {
      FdtSetProp (FdtCombo, Chosen, "linux,initrd-start", &StartVal, sizeof StartVal);
      FdtSetProp (FdtCombo, Chosen, "linux,initrd-end", &EndVal, sizeof EndVal);
      Print (L"X96Linux: initrd at 0x%llx - 0x%llx (BE props)\n",
             InitrdBase, InitrdBase + InitrdSize);
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
