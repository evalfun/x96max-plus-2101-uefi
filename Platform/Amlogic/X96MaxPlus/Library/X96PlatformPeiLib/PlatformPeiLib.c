/** @file
  Amlogic X96 Max+ (SM1) PrePi platform initialization and FDT normalization.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Guid/FdtHob.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/FdtLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

#include <MesonSm1.h>

STATIC
EFI_STATUS
SetRegRange (
  IN VOID         *Fdt,
  IN CONST CHAR8  *Path,
  IN UINT64       Base,
  IN UINT64       Size
  )
{
  INT32   Node;
  UINT64  Reg[2];

  Node = FdtPathOffset (Fdt, Path);
  if (Node < 0) {
    DEBUG ((DEBUG_ERROR, "X96M: missing FDT node %a\n", Path));
    return EFI_NOT_FOUND;
  }

  Reg[0] = SwapBytes64 (Base);
  Reg[1] = SwapBytes64 (Size);
  if (FdtSetProp (Fdt, Node, "reg", Reg, sizeof (Reg)) != 0) {
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
NormalizeDeviceTree (
  IN VOID  *Fdt
  )
{
  INT32       Chosen;
  EFI_STATUS  Status;

  Status = SetRegRange (
             Fdt,
             "/memory",
             MESON_SM1_DRAM_LOW_BASE,
             MESON_SM1_DRAM_TOP
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SetRegRange (
             Fdt,
             "/reserved-memory/secmon@5000000",
             MESON_SM1_SECURE_BASE,
             0x00300000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = SetRegRange (
             Fdt,
             "/reserved-memory/secmon@5300000",
             MESON_SM1_SECURE_BASE + 0x00300000,
             0x02000000
             );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Chosen = FdtPathOffset (Fdt, "/chosen");
  if (Chosen >= 0) {
    FdtDelProp (Fdt, Chosen, "bootargs");
    FdtDelProp (Fdt, Chosen, "linux,initrd-start");
    FdtDelProp (Fdt, Chosen, "linux,initrd-end");
    FdtDelProp (Fdt, Chosen, "kaslr-seed");
    FdtDelProp (Fdt, Chosen, "u-boot,version");
  }

  //
  // This box has no SPI NOR: the firmware variable store lives in emulated
  // (RAM) NV, so there is no VARIABLE_FLASH_INFO geometry to describe.
  //
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
PlatformPeim (
  VOID
  )
{
  VOID        *Source;
  VOID        *Destination;
  UINTN       Pages;
  UINTN       Size;
  UINT64      *FdtHobData;
  EFI_STATUS  Status;

  Source = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  if ((Source == NULL) || (FdtCheckHeader (Source) != 0)) {
    DEBUG ((DEBUG_ERROR, "X96M: invalid embedded FDT at %p\n", Source));
    return EFI_VOLUME_CORRUPTED;
  }

  Size  = FdtTotalSize (Source) + PcdGet32 (PcdDeviceTreeAllocationPadding);
  Pages = EFI_SIZE_TO_PAGES (Size);
  Destination = AllocatePages (Pages);
  if (Destination == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (FdtOpenInto (Source, Destination, EFI_PAGES_TO_SIZE (Pages)) != 0) {
    return EFI_VOLUME_CORRUPTED;
  }

  Status = NormalizeDeviceTree (Destination);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "X96M: FDT normalization failed: %r\n", Status));
    return Status;
  }

  FdtHobData = BuildGuidHob (&gFdtHobGuid, sizeof (*FdtHobData));
  if (FdtHobData == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  *FdtHobData = (UINTN)Destination;

  BuildFvHob (PcdGet64 (PcdFvBaseAddress), PcdGet32 (PcdFvSize));
  return EFI_SUCCESS;
}
