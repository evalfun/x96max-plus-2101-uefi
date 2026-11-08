// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// X96 Linux Boot arm64 handoff: move the C-ABI (x0 = entry, x1 = FDT)
// into the arm64 Linux boot protocol (x0 = FDT, x1..x3 = 0) and branch
// forever.  No frame pointer is meaningful once we switch ELs/loads;
// the return in this function never happens.
//

#include <AsmMacroLib.h>

ASM_FUNC(X96LinuxBootHandoff)
  mov   x8, x0          // entry point
  mov   x0, x1          // kernel boot protocol x0 = FDT pointer
  mov   x1, xzr         // kernel boot protocol requires x1..x3 = 0
  mov   x2, xzr
  mov   x3, xzr
  dsb   sy
  isb
  br    x8

//
// X96CpuDehydrate: unconditional, self-contained cleanup.  Reads the
// CurrentEL, disables the D-cache / I-cache / MMU through the
// corresponding EL's SCTLR, invalidates the TLB, and returns.
// x16/x17 used as scratch.  No library reliance.
//
ASM_FUNC(X96CpuDehydrate)
  mrs   x16, currentel
  cmp   x16, #0x8
  b.gt  3f          // EL3+: stop looping (should not happen)
  b.eq  2f
  cbnz  x16, 1f
  b     3f
1: // EL1
  mrs   x17, sctlr_el1
  bic   x17, x17, #1        // M
  bic   x17, x17, #4        // C (D-cache)
  bic   x17, x17, #(1 << 12) // I
  msr   sctlr_el1, x17
  dsb   sy
  isb
  tlbi  vmalle1
  dsb   sy
  isb
  ret
2: // EL2
  mrs   x17, sctlr_el2
  bic   x17, x17, #1
  bic   x17, x17, #4
  bic   x17, x17, #(1 << 12)
  msr   sctlr_el2, x17
  dsb   sy
  isb
  tlbi  alle2
  dsb   sy
  isb
  ret
3:
  ret
