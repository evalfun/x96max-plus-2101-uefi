/** @file
  X96 chain loader: a tiny EFI application whose only job is to start
  X96LinuxBoot.efi with a fixed set of LoadOptions, so GRUB (or anything
  else that can only chainload) can select a kernel without losing the
  booti handoff path.

  The kernel argument set is selected at build time with CHAIN_DEBIAN /
  CHAIN_ARMBIAN; each variant is its own INF/FILE_GUID pair and is
  copied to the ESP next to the bootloader.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>

#ifdef CHAIN_DEBIAN
//
// Debian 6.12.100 mainline kernel + its real initrd, over the Armbian
// SD rootfs (ext4 is a module for the Debian build, so the initrd is
// mandatory here).
//
STATIC CHAR16  mKernelPath[]  = L"\\EFI\\BOOT\\vmlinuz-6.12.114-ophub";
STATIC CHAR16  mInitrdPath[]  = L"\\EFI\\BOOT\\initrd-6.12.114-ophub";
#else
STATIC CHAR16  mKernelPath[]  = L"\\vmlinuz-6.1.188-ophub";
STATIC CHAR16  mInitrdPath[]  = L"\\initrd.img-6.1.188-ophub";
#endif

//
// Same strings the loader itself would accept after its image name.
//
STATIC CHAR16  mOptionsTail[] = L" root=/dev/mmcblk1p2 rootwait";

EFI_STATUS
EFIAPI
X96ChainEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_LOADED_IMAGE_PROTOCOL     *Loaded;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FileSystem;
  EFI_FILE_PROTOCOL             *Root;
  EFI_FILE_PROTOCOL             *LoaderFile;
  EFI_DEVICE_PATH_PROTOCOL      *LoaderDevicePath;
  EFI_HANDLE                    LoaderHandle;
  EFI_LOADED_IMAGE_PROTOCOL     *LoadedLoader;
  UINTN                         OptionsSize;
  UINTN                         Len1;
  UINTN                         Len2;

  Status = gBS->HandleProtocol (
                  ImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&Loaded
                  );
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: LoadedImage: %r\n", Status);
    return Status;
  }

  Status = gBS->HandleProtocol (
                  Loaded->DeviceHandle,
                  &gEfiSimpleFileSystemProtocolGuid,
                  (VOID **)&FileSystem
                  );
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: not on a filesystem: %r\n", Status);
    return Status;
  }

  Status = FileSystem->OpenVolume (FileSystem, &Root);
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: OpenVolume: %r\n", Status);
    return Status;
  }

  Status = Root->Open (
                   Root,
                   &LoaderFile,
                   L"\\X96LinuxBoot.efi",
                   EFI_FILE_MODE_READ,
                   0
                   );
  Root->Close (Root);
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: \\X96LinuxBoot.efi missing: %r\n", Status);
    return Status;
  }

  LoaderDevicePath = FileDevicePath (Loaded->DeviceHandle, L"\\X96LinuxBoot.efi");

  Status = gBS->LoadImage (
                  FALSE,
                  ImageHandle,
                  LoaderDevicePath,
                  NULL,
                  0,
                  &LoaderHandle
                  );
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: LoadImage(loader): %r\n", Status);
    return Status;
  }

  //
  // Hand the fixed option string over as the loader's LoadOptions.
  //
  Status = gBS->HandleProtocol (
                  LoaderHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedLoader
                  );
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: loader LoadedImage: %r\n", Status);
    return Status;
  }

  Len1        = StrLen (mKernelPath);
  Len2        = StrLen (mInitrdPath) + StrLen (mOptionsTail);
  OptionsSize = (Len1 + Len2 + 2) * sizeof (CHAR16);

  LoadedLoader->LoadOptionsSize = (UINT32)OptionsSize;
  Status = gBS->AllocatePool (
                  EfiBootServicesData,
                  OptionsSize,
                  (VOID **)&LoadedLoader->LoadOptions
                  );
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: options pool: %r\n", Status);
    return Status;
  }

  CopyMem (LoadedLoader->LoadOptions, mKernelPath, Len1 * sizeof (CHAR16));
  CopyMem ((UINT8 *)LoadedLoader->LoadOptions + Len1 * sizeof (CHAR16),
           mInitrdPath, (StrLen (mInitrdPath)) * sizeof (CHAR16));
  CopyMem ((UINT8 *)LoadedLoader->LoadOptions + (Len1 + StrLen (mInitrdPath)) * sizeof (CHAR16),
           mOptionsTail, (StrLen (mOptionsTail) + 1) * sizeof (CHAR16));

  DEBUG ((DEBUG_INFO, "X96Chain: starting loader with %d options\n", OptionsSize / sizeof (CHAR16)));

  Status = gBS->StartImage (LoaderHandle, NULL, NULL);
  if (EFI_ERROR (Status)) {
    Print (L"X96Chain: StartImage(loader) %r\n", Status);
  }

  return Status;
}
