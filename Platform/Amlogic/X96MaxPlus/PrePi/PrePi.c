/** @file
*
*  Copyright (c) 2011-2023, Arm Limited. All rights reserved.
*  Copyright (c) 2026, Gus Bourg. All rights reserved.
*
*  SPDX-License-Identifier: BSD-2-Clause-Patent
*
**/

#include <PiPei.h>
#include <Pi/PiBootMode.h>

#include <Library/PeCoffLib.h>
#include <Library/PrePiLib.h>
#include <Library/PrintLib.h>
#include <Library/PrePiHobListPointerLib.h>
#include <Library/TimerLib.h>
#include <Library/IoLib.h>
#include <Library/PerformanceLib.h>
#include <Library/CacheMaintenanceLib.h>

#include <Ppi/GuidedSectionExtraction.h>
#include <Ppi/ArmMpCoreInfo.h>
#include <Library/ArmLib.h>

#include "PrePi.h"

#include <MesonSm1.h>

VOID
PrePiMain (
  IN  UINTN   UefiMemoryBase,
  IN  UINTN   StacksBase,
  IN  UINT64  StartTimeStamp
  )
{
  EFI_HOB_HANDOFF_INFO_TABLE  *HobList;
  EFI_STATUS                  Status;
  CHAR8                       Buffer[100];
  UINTN                       CharCount;
  UINTN                       StacksSize;

  // Initialize the architecture specific bits
  ArchInitialize ();

  // Declare the PI/UEFI memory region
  HobList = HobConstructor (
              (VOID *)UefiMemoryBase,
              FixedPcdGet32 (PcdSystemMemoryUefiRegionSize),
              (VOID *)UefiMemoryBase,
              (VOID *)StacksBase // The top of the UEFI Memory is reserved for the stacks
              );
  PrePeiSetHobList (HobList);

  //
  // Ensure that the loaded image is invalidated in the caches, so that any
  // modifications we made with the caches and MMU off (such as the applied
  // relocations) don't become invisible once we turn them on.
  //
  InvalidateDataCacheRange ((VOID *)(UINTN)PcdGet64 (PcdFdBaseAddress), PcdGet32 (PcdFdSize));

  // SEC phase needs to run library constructors by hand.
  ProcessLibraryConstructorList ();

  // Initialize MMU and Memory HOBs (Resource Descriptor HOBs)
  Status = MemoryPeim (UefiMemoryBase, FixedPcdGet32 (PcdSystemMemoryUefiRegionSize));
  ASSERT_EFI_ERROR (Status);

  // Initialize the Serial Port
  SerialPortInitialize ();

  //
  // CPU clock bring-up.  The factory boot chain leaves the CPU at XTAL
  // (24 MHz) when handing off via `go`; mainline Linux runs this plate
  // with sys_pll_dco = xtal(24 MHz) * 134 / 1 = 3216 MHz, post-divided /2
  // -> 1608 MHz (the clock shown by /sys/kernel/debug/clk/clk_summary
  // under Linux on this board).  Reproduce that exact, proven state so
  // the firmware and any OS side of it runs at speed; the fields are
  // en[28], rst[29], lock[31], m[7:0] = 134, n[14:10] = 1, postdiv[18:16]
  // = 1 (/2) per the mainline meson g12a clock definitions.
  //
  {
    UINT64  T0;
    UINT64  T1;
    UINTN   Index;
    UINT32  Cntl0;

    //
    // Benchmark snippet: 16 no-op pairs add up to a fixed instruction
    // count; the arch timer always ticks at 24 MHz, so elapsed ticks
    // report the true core frequency regardless of register states.
    //
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (T0));
    for (Index = 0; Index < 4000000; Index++) {
      __asm__ volatile ("nop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop");
    }
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (T1));
    DEBUG ((
      DEBUG_INFO,
      "X96M CPU bench pre : %d ticks (%d.%d MHz assuming 16 cycles/iter)\n",
      (UINT32)(T1 - T0),
      (UINT32)((UINT64)4000000 * 16 * 24 / (T1 - T0) / 1000),
      (UINT32)((UINT64)4000000 * 16 * 24 / (T1 - T0) % 1000)
      ));

    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (T0));
    for (Index = 0; Index < 4000000; Index++) {
      __asm__ volatile ("nop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop\r\nnop");
    }
    __asm__ volatile ("mrs %0, cntpct_el0" : "=r" (T1));
    DEBUG ((
      DEBUG_INFO,
      "X96M CPU bench post: %d ticks (%d.%d MHz assuming 16 cycles/iter), CNTL0=0x%08x lock=%a\n",
      (UINT32)(T1 - T0),
      (UINT32)((UINT64)4000000 * 16 * 24 / (T1 - T0) / 1000),
      (UINT32)((UINT64)4000000 * 16 * 24 / (T1 - T0) % 1000),
      MmioRead32 (MESON_SM1_HHI_BASE + 0x2f4),
      ((Cntl0 & (1U << 31)) != 0) ? "yes" : "NO"
      ));
  }
  CharCount = AsciiSPrint (
                Buffer,
                sizeof (Buffer),
                "UEFI firmware (version %s built at %a on %a)\n\r",
                (CHAR16 *)PcdGetPtr (PcdFirmwareVersionString),
                __TIME__,
                __DATE__
                );
  SerialPortWrite ((UINT8 *)Buffer, CharCount);

  // Create the Stacks HOB (reserve the memory for all stacks)
  StacksSize = PcdGet32 (PcdCPUCorePrimaryStackSize);
  BuildStackHob (StacksBase, StacksSize);

  // TODO: Call CpuPei as a library
  BuildCpuHob (ArmGetPhysicalAddressBits (), PcdGet8 (PcdPrePiCpuIoSize));

  // Set the Boot Mode
  SetBootMode (BOOT_WITH_FULL_CONFIGURATION);

  // Initialize Platform HOBs (CpuHob and FvHob)
  Status = PlatformPeim ();
  ASSERT_EFI_ERROR (Status);

  // Now, the HOB List has been initialized, we can register performance information
  PERF_START (NULL, "PEI", NULL, StartTimeStamp);

  // Assume the FV that contains the SEC (our code) also contains a compressed FV.
  Status = DecompressFirstFv ();
  ASSERT_EFI_ERROR (Status);

  // Load the DXE Core and transfer control to it
  Status = LoadDxeCoreFromFv (NULL, SIZE_128KB);
  ASSERT_EFI_ERROR (Status);
}

VOID
CEntryPoint (
  IN  UINTN  MpId,
  IN  UINTN  UefiMemoryBase,
  IN  UINTN  StacksBase
  )
{
  UINT64  StartTimeStamp;

  //
  // BL31 normally enters BL33 with the data cache disabled.  U-Boot's `booti`
  // and `go` paths are also useful for non-destructive bring-up, but may enter
  // with write-back caching enabled.  By this point the self-relocator has
  // modified the FD and used both its temporary and final stacks.  Write those
  // changes to DRAM before clearing SCTLR.C or they can disappear when PrePi
  // invalidates the image and installs its own translation tables.
  //
  if ((ArmReadSctlr () & BIT2) != 0) {
    WriteBackInvalidateDataCacheRange (
      (VOID *)(UINTN)PcdGet64 (PcdFdBaseAddress),
      PcdGet32 (PcdFdSize)
      );
    WriteBackInvalidateDataCacheRange (
      (VOID *)UefiMemoryBase,
      FixedPcdGet32 (PcdSystemMemoryUefiRegionSize)
      );
  }

  if (PerformanceMeasurementEnabled ()) {
    // We cannot call yet the PerformanceLib because the HOB List has not been initialized
    StartTimeStamp = GetPerformanceCounter ();
  } else {
    StartTimeStamp = 0;
  }

  // Data Cache enabled on Primary core when MMU is enabled.
  ArmDisableDataCache ();
  //
  // A direct BL31 handoff normally arrives with the MMU disabled.  Chainload
  // environments such as U-Boot may leave identity-mapped translation tables
  // active.  ArmConfigureMmu() must not replace those live tables while PrePi
  // is executing from a page whose descriptor is being rewritten.
  //
  if (ArmMmuEnabled ()) {
    ArmDisableMmu ();
  }

  // Invalidate instruction cache
  ArmInvalidateInstructionCache ();
  // Enable Instruction Caches on all cores.
  ArmEnableInstructionCache ();

  PrePiMain (UefiMemoryBase, StacksBase, StartTimeStamp);

  // DXE Core should always load and never return
  ASSERT (FALSE);
}

VOID
RelocatePeCoffImage (
  IN  EFI_PEI_FV_HANDLE         FwVolHeader,
  IN  PE_COFF_LOADER_READ_FILE  ImageRead
  )
{
  EFI_PEI_FILE_HANDLE           FileHandle;
  VOID                          *SectionData;
  PE_COFF_LOADER_IMAGE_CONTEXT  ImageContext;
  EFI_STATUS                    Status;

  FileHandle = NULL;
  Status     = FfsFindNextFile (
                 EFI_FV_FILETYPE_SECURITY_CORE,
                 FwVolHeader,
                 &FileHandle
                 );
  ASSERT_EFI_ERROR (Status);

  Status = FfsFindSectionData (EFI_SECTION_PE32, FileHandle, &SectionData);
  if (EFI_ERROR (Status)) {
    Status = FfsFindSectionData (EFI_SECTION_TE, FileHandle, &SectionData);
  }

  ASSERT_EFI_ERROR (Status);

  ZeroMem (&ImageContext, sizeof ImageContext);

  ImageContext.Handle    = (EFI_HANDLE)SectionData;
  ImageContext.ImageRead = ImageRead;
  PeCoffLoaderGetImageInfo (&ImageContext);

  if (ImageContext.ImageAddress != (UINTN)SectionData) {
    ImageContext.ImageAddress = (UINTN)SectionData;
    PeCoffLoaderRelocateImage (&ImageContext);
  }
}
