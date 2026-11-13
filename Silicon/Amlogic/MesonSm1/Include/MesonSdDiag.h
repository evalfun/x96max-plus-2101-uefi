/** @file
  Shared layout of the X96SdDiag boot-services variable: written by the
  Meson SD host driver during identification, read by BDS (console print)
  and by the X96SdDiag FV application -- byte-offset-safe from either side.

  All offsets are relative to the variable data start; identical struct
  with pragma pack on both readers/writers.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef MESON_SD_DIAG_H_
#define MESON_SD_DIAG_H_

#define MESON_SD_DIAG_MAGIC        0x21583936        // 'X96!'
#define MESON_SD_DIAG_VARIABLE     L"X96SdDiag"

STATIC EFI_GUID mMesonSdDiagGuid = {
  0x25b7b0e4, 0x8c11, 0x435e, { 0x9d, 0x7b, 0x62, 0xef, 0xf9, 0x77, 0xd1, 0x72 }
};

#pragma pack (1)
typedef struct {
  UINT32  Magic;        // [0]  MESON_SD_DIAG_MAGIC
  UINT8   Tuned;        // [4]  phase search locked (1/0)
  UINT8   Co;           // [5]  locked co-phase
  UINT8   Tx;           // [6]  locked tx-phase
  UINT8   Pad;
  UINT32  ClockHz;      // [8]  identification clock in use
  UINT32  EchoR0;       // [12] CMD8 echo word of the (attempted) phase
  UINT8   Csd[16];      // [16] raw CSD words as captured from CMD9 (R2)
  UINT32  Ocr;          // [32] last ACMD41 OCR word
  UINT32  Attempts;     // [36] CMD8 probes issued by the tuner
  UINT32  LastStatus;   // [40] EFI_STATUS of the last CMD8 probe
  UINT32  Cid[4];       // [44] CMD2 (R2) words of the (attempted) phase
  UINT64  CmdTrace;     // [60] bitmask of every CMD index the SD host saw
  UINT32  LastCmd;      // [68] last command index issued on the SD host
  UINT32  NotifyCount;  // [72] MmcNotifyState transition count (SD host only)
  UINT32  LastState;    // [76] last MMC_STATE reached via NotifyState
  UINT32  StateMask;    // [80] OR of every MMC_STATE bit the SD host saw
  UINT32  CardPolls;    // [84] IsCardPresent calls (MmcDxe timer + binding liveness)
  UINT32  RawRegs[16];  // [88] controller 0x40..0x7C as the vendor chain left it
  UINT32  EmmcNotifyCount; // [152] same counters for the eMMC host
  UINT32  EmmcLastState;   // [156]
  UINT32  EmmcStateMask;   // [160]
  UINT32  EmmcLastCmd;     // [164]
  UINT64  EmmcCmdTrace;    // [168] bitmask of CMD indices the eMMC host saw
  UINT32  Cmd3Arg;         // [176] argument of the last CMD3 on the SD host
  UINT32  Cmd3R0;          // [180] CMD_RSP latched for that CMD3
  UINT32  Cmd3R1;          // [184] CMD_RSP1 latched for that CMD3
  UINT32  Cmd3Retries;     // [188] times the RCA sentinel retry re-issued CMD3
} MESON_SD_DIAG_VAR;
#pragma pack ()

#endif // MESON_SD_DIAG_H_
