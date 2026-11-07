/** @file
  PrePi memory HOB and MMU initialization for Meson SM1.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiPei.h>
#include <Library/ArmMmuLib.h>
#include <Library/ArmVirtMemInfoLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>

#include <Library/IoLib.h>

#include <MesonSm1.h>

#define X96M_PROBE(ch) \
  do { MmioWrite32 (0xFF803000, ch); } while (0)

// Drain: the UART FIFO fills far faster than 115200 baud drains it, and new
// writes into a full FIFO are silently dropped.  Every probe waits out one
// FIFO flush so nothing downstream is masked by queue overflow.
STATIC VOID EFIAPI X96M_Drain (VOID)
{
  volatile UINTN  Loop;
  for (Loop = 0; Loop < 2000000; Loop++) {
  }
}

VOID
BuildMemoryTypeInformationHob (
  VOID
  );

EFI_STATUS
EFIAPI
MemoryPeim (
  IN EFI_PHYSICAL_ADDRESS  UefiMemoryBase,
  IN UINT64                UefiMemorySize
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *MemoryTable;
  VOID                          *TranslationTableBase;
  UINTN                         TranslationTableSize;
  RETURN_STATUS                 Status;
  EFI_RESOURCE_ATTRIBUTE_TYPE   Attributes;

  X96M_PROBE (0x70);
  X96M_Drain ();                                // 'p'  -- MemoryPeim entered

  Attributes = EFI_RESOURCE_ATTRIBUTE_PRESENT |
               EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
               EFI_RESOURCE_ATTRIBUTE_WRITE_COMBINEABLE |
               EFI_RESOURCE_ATTRIBUTE_WRITE_THROUGH_CACHEABLE |
               EFI_RESOURCE_ATTRIBUTE_WRITE_BACK_CACHEABLE |
               EFI_RESOURCE_ATTRIBUTE_TESTED;

  X96M_PROBE (0x50);
  X96M_Drain ();                                // 'P'  -- before first HOB
  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    Attributes,
    MESON_SM1_DRAM_LOW_BASE,
    MESON_SM1_DRAM_LOW_SIZE
    );
  X96M_PROBE (0x51);
  X96M_Drain ();                                // 'Q'  -- DRAM-low HOB ok
  BuildResourceDescriptorHob (
    EFI_RESOURCE_MEMORY_RESERVED,
    EFI_RESOURCE_ATTRIBUTE_PRESENT | EFI_RESOURCE_ATTRIBUTE_INITIALIZED,
    MESON_SM1_SECURE_BASE,
    MESON_SM1_SECURE_SIZE
    );
  X96M_PROBE (0x52);
  X96M_Drain ();                                // 'R'  -- secure HOB ok
  BuildResourceDescriptorHob (
    EFI_RESOURCE_SYSTEM_MEMORY,
    Attributes,
    MESON_SM1_DRAM_HIGH_BASE,
    MESON_SM1_DRAM_HIGH_SIZE
    );
  X96M_PROBE (0x53);
  X96M_Drain ();                                // 'S'  -- DRAM-high HOB ok

  X96M_PROBE (0x71);
  X96M_Drain ();                                // 'q'  -- resource HOBs built

  ArmVirtGetMemoryMap (&MemoryTable);
  X96M_PROBE (0x72);
  X96M_Drain ();                                // 'r' -- memory map fetched
  Status = ArmConfigureMmu (
             MemoryTable,
             &TranslationTableBase,
             &TranslationTableSize
             );
  ASSERT_RETURN_ERROR (Status);
  X96M_PROBE (0x73);
  X96M_Drain ();                                // 's' -- MMU configured

  if (FeaturePcdGet (PcdPrePiProduceMemoryTypeInformationHob)) {
    BuildMemoryTypeInformationHob ();
  }

  return RETURN_ERROR (Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
