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
// X96UartNibbles: print 16 hex nibbles of x16 via raw UART MMIO for the
// "MMU off"-phase diagnostics.  No C libs, no stack writes.
//
ASM_FUNC(X96UartNibbles)
  mov   x17, #60        // loop count 16 nibbles * ~4 ops
  mov   x20, x16
  mov   x19, #15
.LNibLoop:
  lsr   x18, x20, x19
  and   x18, x18, #0xf
  cmp   x18, #10
  blo   .LNibIsDigit
  add   x18, x18, #0x57
  b     .LNibStore
.LNibIsDigit:
  add   x18, x18, #0x30
.LNibStore:
  ldr   x15, =0xFF803000
  str   w18, [x15]
  sub   x19, x19, #4
  cmp   x19, #0
  b.ge  .LNibLoop
  ret

//
// X96LinuxFinal: the last-mile.  Prints the kernel entry and FDT pointers
// through raw MMIO, dehydrates the CPU INLINE (never a C return after the
// MMU goes off), prints "M", and branches with the arm64 boot protocol.
// NEVER returns to C past the first instruction.
//
// x0 = entry point
// x1 = FDT pointer
//
ASM_FUNC(X96LinuxFinal)
  stp   x0, x1, [sp, #-32]!

  // print "K" marker
  mov   w17, #0x4B
  ldr   x15, =0xFF803000
  str   w17, [x15]
  mov   w17, #0x0d
  str   w17, [x15]
  mov   w17, #0x0a
  str   w17, [x15]

  // kernel entry pointer hex
  mov   x16, x0
  bl    ASM_PFX(X96UartNibbles)
  // "-" between values
  mov   w18, #0x2d
  ldr   x15, =0xFF803000
  str   w18, [x15]
  // FDT pointer hex
  mov   x16, x1
  bl    X96UartNibbles
  mov   w18, #0x0d
  ldr   x15, =0xFF803000
  str   w18, [x15]
  mov   w18, #0x0a
  str   w18, [x15]

  // "P" marker: entered the dehydrate phase
  mov   w18, #0x50
  ldr   x15, =0xFF803000
  str   w18, [x15]
  mov   w18, #0x0d
  str   w18, [x15]
  mov   w18, #0x0a
  str   w18, [x15]

  ldp   x0, x1, [sp], #32

  // dehydrate INLINE, per-EL; no bl/ret, no C from here on
  mrs   x16, currentel
  cmp   x16, #0x8
  b.eq  2f
  b.lt  1f
  b     3f                 // EL3: hand off untouched (never expected)
1: // EL1
  mrs   x17, sctlr_el1
  bic   x17, x17, #1         // M
  bic   x17, x17, #4         // C (D-cache)
  bic   x17, x17, #(1 << 12) // I
  msr   sctlr_el1, x17
  dsb   sy
  isb
  tlbi  vmalle1
  dsb   sy
  isb
  b     3f
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
3:
  // print "M" marker (MMU off)
  mov   w19, #0x4D
  ldr   x15, =0xFF803000
  str   w19, [x15]
  mov   w19, #0x0d
  str   w19, [x15]
  mov   w19, #0x0a
  str   w19, [x15]

  // handoff: x8 = entry, x0 = FDT, x1..3=0
  mov   x8, x0
  mov   x0, x1
  mov   x1, xzr
  mov   x2, xzr
  mov   x3, xzr
  msr   daifset, #0xF
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
