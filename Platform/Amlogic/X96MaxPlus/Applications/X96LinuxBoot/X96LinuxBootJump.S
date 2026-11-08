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
