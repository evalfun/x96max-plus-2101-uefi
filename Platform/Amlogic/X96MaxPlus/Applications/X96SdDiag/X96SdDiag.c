/** @file
  Shell application: dump the SD identification diagnostics that the
  MesonSdMmc host exported during boot (the X96SdDiag boot-services
  variable).  Decodes the tuning outcome and the raw CSD so a serial-less
  box can be diagnosed over HDMI + USB keyboard.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Uefi/UefiSpec.h>

#include <MesonSdDiag.h>

#define mDiagGuid  mMesonSdDiagGuid

STATIC
VOID
PrintStatusName (
  IN UINT32  Status
  )
{
  if (Status == 0) {
    Print (L"SUCCESS");
    return;
  }

  switch (Status & 0x0F) {
    case 0x7:
      Print (L"DEVICE_ERROR");
      break;
    case 0xE:
      Print (L"TIMEOUT");
      break;
    case 0x2:
      Print (L"INVALID_PARAMETER");
      break;
    case 0x9:
      Print (L"NOT_FOUND");
      break;
    default:
      Print (L"code-0x%x", Status & 0x0F);
      break;
  }
}

STATIC
UINT32
Rd (
  IN CONST MESON_SD_DIAG_VAR  *D,
  IN UINTN              Off
  )
{
  UINTN Idx;

  Idx = Off;
  return
    ((UINT32)((UINT8 *)&D->Csd[Idx])[0]) |
    ((UINT32)((UINT8 *)&D->Csd[Idx + 1])[0] << 8) |
    ((UINT32)((UINT8 *)&D->Csd[Idx + 2])[0] << 16) |
    ((UINT32)((UINT8 *)&D->Csd[Idx + 3])[0] << 24);
}

EFI_STATUS
EFIAPI
X96SdDiagEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  MESON_SD_DIAG_VAR  D;
  UINTN        Size;
  EFI_STATUS   Status;
  UINT32       W0;
  UINT32       W1;
  UINT32       W2;
  UINT32       W3;
  UINT32       Struct;
  UINT32       CSize;
  UINTN        Idx;

  Size = sizeof (D);
  ZeroMem (&D, sizeof (D));

  Status = gRT->GetVariable (L"X96SdDiag", &mDiagGuid, NULL, &Size, &D);
  if (EFI_ERROR (Status)) {
    Print (L"X96SdDiag: variable not present (%r).\n", Status);
    Print (L"Boot with the SD card inserted, then run this again.\n");
    return Status;
  }

  Print (L"== X96 SD identification diagnostics ==\n");
  Print (L"magic=0x%x  (0x21583936 = fresh this boot)\n", D.Magic);
  Print (L"phase search: tuned=%u co=%u tx=%u clock=%uHz\n", D.Tuned, D.Co, D.Tx, D.ClockHz);
  Print (L"tuner: attempts=%u last-status=%x (", D.Attempts, D.LastStatus);
  PrintStatusName (D.LastStatus);
  Print (L")   [7=device-error(CRC/采样错), e=timeout(线路不通)]\n");
  Print (L"CMD8 echo word: %08x   [bit8..15 should be 0xAA]\n", D.EchoR0);

  W0 = Rd ((&D), 0);
  W1 = Rd ((&D), 4);
  W2 = Rd ((&D), 8);
  W3 = Rd ((&D), 12);

  Print (L"CSD words (R2 as sampled): w0=%08x w1=%08x w2=%08x w3=%08x\n", W0, W1, W2, W3);
  Struct = (W0 >> 30) & 0x3;
  Print (L"CSD structure=%u (%u == v2/SDHC)\n", Struct, 1);
  if (Struct == 1) {
    CSize   = ((W1 & 0x3F) << 16) | ((W2 >> 16) & 0xFFFF);
    Print (L"v2 C_SIZE=%u -> capacity ~%u MiB (%u blocks of 512KiB)\n", CSize, (CSize + 1) / 2, (CSize + 1));
  }

  Print (
    L"OCR word: %08x  [bit30=CCS(SDHC), bit31=pow-down]\n",
    D.Ocr
    );
  Print (
    L"MmcDxe state: notifies=%u last-state=%u statemask=%08x polls=%u\n",
    D.NotifyCount,
    D.LastState,
    D.StateMask,
    D.CardPolls
    );
  Print (L"cmd trace=0x%016lx  last-cmd=%u\n", D.CmdTrace, D.LastCmd);
  Print (L"raw regs   :");
  for (Idx = 0; Idx < 16; Idx++) {
    Print (L" %08x", D.RawRegs[Idx]);
  }

  Print (L"\n");
  Print (L"\nhealthy SD expected: structure=1, v2 C_SIZE~=~%u MiB, echo bit8..15=AA,\n", 65504 / 2);
  Print (L"sampled w3 third word == echo of OCR block (0xC0FF8000-family).\n");

  return EFI_SUCCESS;
}
