/** @file
  MMU memory map for Meson SM1.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/ArmLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>

#include <MesonSm1.h>

#define DESCRIPTOR_COUNT  3

VOID
ArmVirtGetMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *Map;

  ASSERT (VirtualMemoryMap != NULL);
  Map = AllocateZeroPool (sizeof (*Map) * DESCRIPTOR_COUNT);
  ASSERT (Map != NULL);
  if (Map == NULL) {
    return;
  }

  //
  // Simplified two-region map for the X96 Max+ bring-up.  The DRAM split
  // (low / reserved-secure / high) exists in the VIM3 map so the secure
  // region is described as device memory; here the same physical range is
  // simply folded into the DRAM region.  The OS is handed the DTB's
  // reserved-memory nodes (secmon@5000000 / secmon@5300000) and never
  // touches it; folding avoids the misaligned-boundary walk that hangs
  // UpdateRegionMappingRecursive on this SoC bring-up.
  //
  Map[0].PhysicalBase = 0x00000000ULL;
  Map[0].VirtualBase  = 0x00000000ULL;
  Map[0].Length       = MESON_SM1_DRAM_TOP;
  Map[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  Map[1].PhysicalBase = MESON_SM1_DRAM_TOP;
  Map[1].VirtualBase  = MESON_SM1_DRAM_TOP;
  Map[1].Length       = SIZE_4GB - MESON_SM1_DRAM_TOP;
  Map[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  *VirtualMemoryMap = Map;
}
