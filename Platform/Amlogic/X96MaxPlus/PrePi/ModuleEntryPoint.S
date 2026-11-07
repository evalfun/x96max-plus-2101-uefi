//
//  Copyright (c) 2011-2013, ARM Limited. All rights reserved.
//  Copyright (c) 2015-2016, Linaro Limited. All rights reserved.
//  Copyright (c) 2026, Gus Bourg. All rights reserved.
//
//  SPDX-License-Identifier: BSD-2-Clause-Patent
//
//

#include <AsmMacroLib.h>

//
// Debug probe: the Amlogic UART_AO_A data register.  The first-stage
// bootloader has already programmed the console, so a plain MMIO byte
// store is enough to leave a trace while the MMU/cache state is unproven.
//
//
// Bare exception catcher.  16 EL2 vector entries, all pointing at one
// handler that leaves an exclamation mark on the UART then tris.  If this
// fires, we know the hang is an exception, not a spin.
//
  .align  11
  ASM_PFX(x96_exception_vectors):
    .rept  16
    b      ASM_PFX(x96_exception_handler)
    .endr
    .rept  2
    nop
    .endr

ASM_FUNC(x96_exception_handler)
  ldr   x2, =0xFF803000
  mov   w3, #0x21
  str   w3, [x2]
  mov   w3, #0x0d
  str   w3, [x2]
  mov   w3, #0x0a
  str   w3, [x2]
  mrs   x4, ESR_EL2
  str   w4, [x2]
  mrs   x4, FAR_EL2
  str   w4, [x2]
3:  b     3b

  .macro  UARTPROBE ch
    ldr   x2, =0xFF803000
    mov   w3, #\ch
    str   w3, [x2]
    mov   w3, #0x0d
    str   w3, [x2]
    mov   w3, #0x0a
    str   w3, [x2]
  .endm

ASM_FUNC(_ModuleEntryPoint)
  UARTPROBE  0x40          // '@' -- entered the flat image entry
  UARTPROBE  0x3f          // '?' -- build fingerprint

  //
  // Which core got here?  A secondary core walking through this code would
  // corrupt shared state mid-run, which is exactly the shape of the hang
  // after the H probe.
  //
  ldr   x2, =0xFF803000
  mrs   x4, MPIDR_EL1
  and   x4, x4, #0xff
  add   x4, x4, #0x30
  str   w4, [x2]           // core id as a digit
  mov   w3, #0x0d
  str   w3, [x2]
  mov   w3, #0x0a
  str   w3, [x2]

  //
  // Report the exception level the bootloader handed us.  Amlogic's
  // legacy 2015 U-Boot runs at EL3 - unlike the ATF handoff the PrePi was
  // written for - and the MMU setup later behaves differently there.
  //
  mrs   x4, CurrentEL
  and   x4, x4, #0xc
  cmp   x4, #0xc
  b.ne  1f
  UARTPROBE  0x33          // '3' => running at EL3
1:
  //
  // Normalise the processor state the way U-Boot's cleanup_before_linux()
  // does before handing off, so this path also works when the legacy
  // Amlogic U-Boot simply jumps with its own caches and page tables live.
  // (At EL3 skip: SCTLR_EL2 is not the controlling register there and the
  // PrePi's own MMU code handles EL3.)
  //
  mrs   x4, CurrentEL
  and   x4, x4, #0xc
  cmp   x4, #0xc
  b.eq  2f
  mrs   x4, SCTLR_EL2
  bic   x4, x4, #1         // M (MMU)
  bic   x4, x4, #4         // C (data cache)
  bic   x4, x4, #(1 << 12) // I (instruction cache)
  msr   SCTLR_EL2, x4
  dsb   sy
  isb
  tlbi  alle2is
  dsb   sy
  isb
2:
  //
  // ARMv8 vector table: catch every synchronous/IRQ/FIQ/SError so the hang
  // announces itself instead of dying silently.
  //
  adr   x5, ASM_PFX(x96_exception_vectors)
  msr   VBAR_EL2, x5
  isb

  //
  // The vendor BL31 argument registers are not a stable public contract.
  // The flat-image trampoline supplies the runtime image base in x1, so use
  // the DTB embedded 0x1000 bytes into that image.
  //
  add   x0, x1, #0x1000

  mov   x28, x0             // preserve DTB pointer
  mov   x27, x1             // preserve base of image pointer
  UARTPROBE  0x42          // 'B' -- image base captured

  // Enable Floating Point. This needs to be done before entering C code, which
  // may use FP/SIMD registers.
  bl    ArmEnableVFP
  UARTPROBE  0x43          // 'C' -- VFP enabled

  bl    ASM_PFX(DiscoverDramFromDt)
  UARTPROBE  0x44          // 'D' -- DRAM discovered

  // Get ID of this CPU in Multicore system
  bl    ASM_PFX(ArmReadMpidr)
  // Keep a copy of the MpId register value
  mov   x20, x0
  UARTPROBE  0x45          // 'E' -- just before stack setup

// Check if we can install the stack at the top of the System Memory or if we need
// to install the stacks at the bottom of the Firmware Device (case the FD is located
// at the top of the DRAM)
_SetupStackPosition:
  // Compute Top of System Memory
  LDR_LIT(x1, PcdGet64 (PcdSystemMemoryBase))
  LDR_LIT(x2, PcdGet64 (PcdSystemMemorySize))
  sub   x2, x2, #1
  add   x1, x1, x2      // x1 = SystemMemoryTop = PcdSystemMemoryBase + PcdSystemMemorySize

  // Calculate Top of the Firmware Device
  LDR_LIT(x2, PcdGet64 (PcdFdBaseAddress))
  MOV32 (w3, FixedPcdGet32 (PcdFdSize) - 1)
  add   x3, x3, x2      // x3 = FdTop = PcdFdBaseAddress + PcdFdSize

  // UEFI Memory Size (stacks are allocated in this region)
  MOV32 (x4, FixedPcdGet32(PcdSystemMemoryUefiRegionSize))

  //
  // Reserve the memory for the UEFI region (contain stacks on its top)
  //

  // Calculate how much space there is between the top of the Firmware and the Top of the System Memory
  subs  x0, x1, x3   // x0 = SystemMemoryTop - FdTop
  b.mi  _SetupStack  // Jump if negative (FdTop > SystemMemoryTop). Case when the PrePi is in XIP memory outside of the DRAM
  cmp   x0, x4
  b.ge  _SetupStack

  // Case the top of stacks is the FdBaseAddress
  mov   x1, x2

_SetupStack:
  // x1 contains the top of the stack (and the UEFI Memory)

  // Because the 'push' instruction is equivalent to 'stmdb' (decrement before), we need to increment
  // one to the top of the stack. We check if incrementing one does not overflow (case of DRAM at the
  // top of the memory space)
  adds  x21, x1, #1
  b.cs  _SetupOverflowStack

_SetupAlignedStack:
  mov   x1, x21
  b     _GetBaseUefiMemory

_SetupOverflowStack:
  // Case memory at the top of the address space. Ensure the top of the stack is EFI_PAGE_SIZE
  // aligned (4KB)
  and   x1, x1, ~EFI_PAGE_MASK

_GetBaseUefiMemory:
  // Calculate the Base of the UEFI Memory
  sub   x21, x1, x4

_GetStackBase:
  // r1 = The top of the Mpcore Stacks
  mov   sp, x1

  // Stack for the primary core = PrimaryCoreStack
  MOV32 (x2, FixedPcdGet32(PcdCPUCorePrimaryStackSize))
  sub   x22, x1, x2

  mov   x0, x20
  mov   x1, x21
  mov   x2, x22

  // Set the frame pointer to NULL so any backtraces terminate here
  mov   x29, xzr
  UARTPROBE  0x46          // 'F' -- stack ready, entering C

  // Jump to PrePiCore C code
  //    x0 = MpId
  //    x1 = UefiMemoryBase
  //    x2 = StacksBase
  bl    ASM_PFX(CEntryPoint)

_NeverReturn:
  b _NeverReturn

// VOID
// DiscoverDramFromDt (
//   VOID   *DeviceTreeBaseAddress,   // passed by loader in x0, preserved in x28
//   VOID   *ImageBase                // passed by FDF trampoline in x1, preserved in x27
//   );
ASM_PFX(DiscoverDramFromDt):
  mov   x29, x30            // preserve LR

  //
  // The base of the runtime image has been preserved in x27. Check whether
  // the expected magic number can be found in the header.
  //
  LDR_LIT_TMP(w8, .LArm64LinuxMagic, x8)
  ldr   w9, [x27, #0x38]
  cmp   w8, w9
  bne   .Lout

  //
  //
  // OK, so far so good. We have confirmed that we likely have a DTB and are
  // booting via the arm64 Linux boot protocol. Update the base-of-image PCD
  // to the actual relocated value, and add the shift of PcdFdBaseAddress to
  // PcdFvBaseAddress as well
  //
  adr   x8, PcdGet64 (PcdFdBaseAddress)
  adr   x9, PcdGet64 (PcdFvBaseAddress)
  ldr   x6, [x8]
  ldr   x7, [x9]
  sub   x7, x7, x6
  add   x7, x7, x27
  str   x27, [x8]
  str   x7, [x9]

  //
  // The runtime address may be different from the link time address so fix
  // up the PE/COFF relocations. Since we are calling a C function, use the
  // window at the beginning of the FD image as a temp stack.
  //
  mov   x0, x7
  adr   x1, PeCoffLoaderImageReadFromMemory
  mov   sp, x7
  bl    RelocatePeCoffImage

  //
  // Discover the memory size and offset from the DTB, and record in the
  // respective PCDs. This will also return false if a corrupt DTB is
  // encountered.
  //
  mov   x0, x28
  adr   x1, PcdGet64 (PcdSystemMemoryBase)
  adr   x2, PcdGet64 (PcdSystemMemorySize)
  bl    FindMemnode
  cbz   x0, .Lout

  //
  // Copy the DTB to the slack space right after the 64 byte arm64/Linux style
  // image header at the base of this image (defined in the FDF), and record the
  // pointer in PcdDeviceTreeInitialBaseAddress.
  //
  adr   x8, PcdGet64 (PcdDeviceTreeInitialBaseAddress)
  add   x27, x27, #0x40
  str   x27, [x8]

  mov   x0, x27
  mov   x1, x28
  bl    CopyFdt

.Lout:
  ret    x29

.LArm64LinuxMagic:
  .byte   0x41, 0x52, 0x4d, 0x64
