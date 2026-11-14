/** @file
  X96NvSaveCommandDxe: the 'nvsave' UEFI Shell dynamic command.

  Writes the emulated non-volatile variable store to the boot card
  immediately, on demand.  A Shell session that ends with a plain reset
  never reaches ReadyToBoot or ExitBootServices, so NVRAM changes made in
  the Shell (bcfg, dmpstore, setvar) would die with the emulated NVRAM;
  'nvsave' saves them right away.  The periodic safety-net timer covers
  the same blind spot automatically, this command makes it deterministic.

  The work happens in X96NvVarsPersistDxe, reached through the persist
  protocol's Checkpoint member.

  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/ShellDynamicCommand.h>
#include <Uefi/UefiBaseType.h>

#include "../X96NvVarsPersistDxe/X96NvVarsPersist.h"

STATIC CONST CHAR16  mCommandName[] = L"nvsave";

STATIC CONST CHAR16  mHelpText[] =
  L"Save the firmware NVRAM (boot options, settings) to the boot SD card.\n"
  L"\n"
  L"nvsave\n"
  L"\n"
  L"The firmware's variables live in RAM and are normally written to the\n"
  L"card at boot-option launch and at ExitBootServices.  A Shell session\n"
  L"that ends with 'reset' bypasses both, so run this after changing boot\n"
  L"options (e.g. 'bcfg boot add') before resetting.\n";

/**
  The command handler.

  @param[in] This              Dynamic command protocol instance.
  @param[in] SystemTable       Unused.
  @param[in] ShellParameters   Unused.
  @param[in] Shell             Unused.

  @retval SHELL_SUCCESS        Store saved.
  @retval SHELL_NOT_FOUND      Persist protocol not found.
**/
SHELL_STATUS
EFIAPI
X96NvSaveHandler (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN EFI_SYSTEM_TABLE                    *SystemTable,
  IN EFI_SHELL_PARAMETERS_PROTOCOL       *ShellParameters,
  IN EFI_SHELL_PROTOCOL                  *Shell
  )
{
  EFI_STATUS                     Status;
  X96_NV_VARS_PERSIST_PROTOCOL  *Persist;

  Status = gBS->LocateProtocol (
                 &gX96NvVarsPersistProtocolGuid,
                 NULL,
                 (VOID **)&Persist
                 );
  if (EFI_ERROR (Status)) {
    Print (L"nvsave: persistence driver not found\n");
    return SHELL_NOT_FOUND;
  }

  Persist->Checkpoint (Persist);
  Print (L"nvsave: NVRAM saved to boot card (see serial log for details)\n");
  return SHELL_SUCCESS;
}

/**
  Return the help text.  The shell frees the returned string, so hand it
  an allocated copy of the static text.

  @param[in] This       Dynamic command protocol instance.
  @param[in] Language   Ignored (single-language build).

  @return Allocated help string, or NULL.
**/
CHAR16 *
EFIAPI
X96NvSaveGetHelp (
  IN EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL  *This,
  IN CONST CHAR8                         *Language
  )
{
  UINTN  Size;

  Size  = (StrLen (mHelpText) + 1) * sizeof (CHAR16);
  return AllocateCopyPool (Size, mHelpText);
}

/**
  Driver entry point: publish the dynamic command.

  @param[in] ImageHandle  Image handle.
  @param[in] SystemTable  System table.

  @retval EFI_SUCCESS  Command published.
**/
EFI_STATUS
EFIAPI
X96NvSaveCommandDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                           Status;
  EFI_HANDLE                           Handle;
  EFI_SHELL_DYNAMIC_COMMAND_PROTOCOL   Command;

  Command.CommandName = (CHAR16 *)mCommandName;
  Command.Handler     = X96NvSaveHandler;
  Command.GetHelp     = X96NvSaveGetHelp;

  Handle = NULL;
  Status = gBS->InstallProtocolInterface (
                  &Handle,
                  &gEfiShellDynamicCommandProtocolGuid,
                  EFI_NATIVE_INTERFACE,
                  &Command
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "X96 NvSave: command install failed: %r\n", Status));
    return Status;
  }

  DEBUG ((DEBUG_INFO, "X96 NvSave: 'nvsave' command registered\n"));
  return EFI_SUCCESS;
}
