/** @file
  Implementation for PlatformBootManagerLib library class interfaces.

  Copyright (C) 2015-2016, Red Hat, Inc.
  Copyright (c) 2014 - 2023, Arm Ltd. All rights reserved.<BR>
  Copyright (c) 2004 - 2018, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2016, Linaro Ltd. All rights reserved.<BR>
  Copyright (c) 2021, Semihalf All rights reserved.<BR>
  Copyright (c) 2026, Gus Bourg. All rights reserved.<BR>

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <IndustryStandard/Pci22.h>
#include <Library/BootLogoLib.h>
#include <Library/CapsuleLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootManagerLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/BootManagerPolicy.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EsrtManagement.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/NonDiscoverableDevice.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Protocol/Usb2HostController.h>
#include <Protocol/UsbIo.h>
#include <Protocol/PlatformBootManager.h>
#include <Guid/BootDiscoveryPolicy.h>
#include <Guid/EventGroup.h>
#include <Guid/NonDiscoverableDevice.h>
#include <Guid/TtyTerm.h>
#include <Guid/SerialPortLibVendor.h>


#include "PlatformBm.h"

#define DP_NODE_LEN(Type)  { (UINT8)sizeof (Type), (UINT8)(sizeof (Type) >> 8) }

//
// Retry parameters for the USB enumeration recovery loop in
// PlatformBootManagerAfterConsole.  The upstream VIM3 worked around a
// noisy DWC3 with 12 attempts of a 3-second settle each; on X96 Max+
// that is 36 s of pure stalling with an empty USB tree.  Bring-up runs
// one 1 s settle and one retry pass at most - the loop remains a
// recovery net, not a wait.
//
#define X96_USB_SETTLE_STALL_US    (1000 * 1000)
#define X96_USB_ENUM_MAX_ATTEMPTS  2

#pragma pack (1)
typedef struct {
  VENDOR_DEVICE_PATH            SerialDxe;
  UART_DEVICE_PATH              Uart;
  VENDOR_DEFINED_DEVICE_PATH    TermType;
  EFI_DEVICE_PATH_PROTOCOL      End;
} PLATFORM_SERIAL_CONSOLE;
#pragma pack ()

STATIC PLATFORM_SERIAL_CONSOLE  mSerialConsole = {
  //
  // VENDOR_DEVICE_PATH SerialDxe
  //
  {
    { HARDWARE_DEVICE_PATH,  HW_VENDOR_DP, DP_NODE_LEN (VENDOR_DEVICE_PATH) },
    EDKII_SERIAL_PORT_LIB_VENDOR_GUID
  },

  //
  // UART_DEVICE_PATH Uart
  //
  {
    { MESSAGING_DEVICE_PATH, MSG_UART_DP,  DP_NODE_LEN (UART_DEVICE_PATH)   },
    0,                                      // Reserved
    FixedPcdGet64 (PcdUartDefaultBaudRate), // BaudRate
    FixedPcdGet8 (PcdUartDefaultDataBits),  // DataBits
    FixedPcdGet8 (PcdUartDefaultParity),    // Parity
    FixedPcdGet8 (PcdUartDefaultStopBits)   // StopBits
  },

  //
  // VENDOR_DEFINED_DEVICE_PATH TermType
  //
  {
    {
      MESSAGING_DEVICE_PATH, MSG_VENDOR_DP,
      DP_NODE_LEN (VENDOR_DEFINED_DEVICE_PATH)
    }
    //
    // Guid to be filled in dynamically
    //
  },

  //
  // EFI_DEVICE_PATH_PROTOCOL End
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

#pragma pack (1)
typedef struct {
  USB_CLASS_DEVICE_PATH       Keyboard;
  EFI_DEVICE_PATH_PROTOCOL    End;
} PLATFORM_USB_KEYBOARD;
#pragma pack ()

STATIC PLATFORM_USB_KEYBOARD  mUsbKeyboard = {
  //
  // USB_CLASS_DEVICE_PATH Keyboard
  //
  {
    {
      MESSAGING_DEVICE_PATH, MSG_USB_CLASS_DP,
      DP_NODE_LEN (USB_CLASS_DEVICE_PATH)
    },
    0xFFFF, // VendorId: any
    0xFFFF, // ProductId: any
    3,      // DeviceClass: HID
    1,      // DeviceSubClass: boot
    1       // DeviceProtocol: keyboard
  },

  //
  // EFI_DEVICE_PATH_PROTOCOL End
  //
  {
    END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE,
    DP_NODE_LEN (EFI_DEVICE_PATH_PROTOCOL)
  }
};

/**
  Check if the handle satisfies a particular condition.

  @param[in] Handle      The handle to check.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.

  @retval TRUE   The condition is satisfied.
  @retval FALSE  Otherwise. This includes the case when the condition could not
                 be fully evaluated due to an error.
**/
typedef
BOOLEAN
(EFIAPI *FILTER_FUNCTION)(
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );

/**
  Process a handle.

  @param[in] Handle      The handle to process.
  @param[in] ReportText  A caller-allocated string passed in for reporting
                         purposes. It must never be NULL.
**/
typedef
VOID
(EFIAPI *CALLBACK_FUNCTION)(
  IN EFI_HANDLE   Handle,
  IN CONST CHAR16 *ReportText
  );

/**
  Locate all handles that carry the specified protocol, filter them with a
  callback function, and pass each handle that passes the filter to another
  callback.

  @param[in] ProtocolGuid  The protocol to look for.

  @param[in] Filter        The filter function to pass each handle to. If this
                           parameter is NULL, then all handles are processed.

  @param[in] Process       The callback function to pass each handle to that
                           clears the filter.
**/
STATIC
VOID
FilterAndProcess (
  IN EFI_GUID           *ProtocolGuid,
  IN FILTER_FUNCTION    Filter         OPTIONAL,
  IN CALLBACK_FUNCTION  Process
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       NoHandles;
  UINTN       Idx;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  ProtocolGuid,
                  NULL /* SearchKey */,
                  &NoHandles,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    //
    // This is not an error, just an informative condition.
    //
    DEBUG ((
      DEBUG_VERBOSE,
      "%a: %g: %r\n",
      __func__,
      ProtocolGuid,
      Status
      ));
    return;
  }

  ASSERT (NoHandles > 0);
  for (Idx = 0; Idx < NoHandles; ++Idx) {
    CHAR16         *DevicePathText;
    STATIC CHAR16  Fallback[] = L"<device path unavailable>";

    //
    // The ConvertDevicePathToText() function handles NULL input transparently.
    //
    DevicePathText = ConvertDevicePathToText (
                       DevicePathFromHandle (Handles[Idx]),
                       FALSE, // DisplayOnly
                       FALSE  // AllowShortcuts
                       );
    if (DevicePathText == NULL) {
      DevicePathText = Fallback;
    }

    if ((Filter == NULL) || Filter (Handles[Idx], DevicePathText)) {
      Process (Handles[Idx], DevicePathText);
    }

    if (DevicePathText != Fallback) {
      FreePool (DevicePathText);
    }
  }

  gBS->FreePool (Handles);
}

/**
  This FILTER_FUNCTION checks if a handle corresponds to a PCI display device.
**/
STATIC
BOOLEAN
EFIAPI
IsPciDisplay (
  IN EFI_HANDLE    Handle,
  IN CONST CHAR16  *ReportText
  )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  PCI_TYPE00           Pci;

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo
                  );
  if (EFI_ERROR (Status)) {
    //
    // This is not an error worth reporting.
    //
    return FALSE;
  }

  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        0 /* Offset */,
                        sizeof Pci / sizeof (UINT32),
                        &Pci
                        );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a: %s: %r\n", __func__, ReportText, Status));
    return FALSE;
  }

  return IS_PCI_DISPLAY (&Pci);
}

/**
  This FILTER_FUNCTION checks if a handle corresponds to a non-discoverable
  USB host controller.
**/
STATIC
BOOLEAN
EFIAPI
IsUsbHost (
  IN EFI_HANDLE    Handle,
  IN CONST CHAR16  *ReportText
  )
{
  NON_DISCOVERABLE_DEVICE  *Device;
  EFI_STATUS               Status;

  Status = gBS->HandleProtocol (
                  Handle,
                  &gEdkiiNonDiscoverableDeviceProtocolGuid,
                  (VOID **)&Device
                  );
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if (CompareGuid (Device->Type, &gEdkiiNonDiscoverableUhciDeviceGuid) ||
      CompareGuid (Device->Type, &gEdkiiNonDiscoverableEhciDeviceGuid) ||
      CompareGuid (Device->Type, &gEdkiiNonDiscoverableXhciDeviceGuid))
  {
    return TRUE;
  }

  return FALSE;
}

/**
  This CALLBACK_FUNCTION attempts to connect a handle non-recursively, asking
  the matching driver to produce all first-level child handles.
**/
STATIC
VOID
EFIAPI
Connect (
  IN EFI_HANDLE    Handle,
  IN CONST CHAR16  *ReportText
  )
{
  EFI_STATUS  Status;

  Status = gBS->ConnectController (
                  Handle, // ControllerHandle
                  NULL,   // DriverImageHandle
                  NULL,   // RemainingDevicePath -- produce all children
                  FALSE   // Recursive
                  );
  DEBUG ((
    EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE,
    "%a: %s: %r\n",
    __func__,
    ReportText,
    Status
    ));
}

/**
  This CALLBACK_FUNCTION retrieves the EFI_DEVICE_PATH_PROTOCOL from the
  handle, and adds it to ConOut and ErrOut.
**/
STATIC
VOID
EFIAPI
AddOutput (
  IN EFI_HANDLE    Handle,
  IN CONST CHAR16  *ReportText
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %s: handle %p: device path not found\n",
      __func__,
      ReportText,
      Handle
      ));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable (ConOut, DevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %s: adding to ConOut: %r\n",
      __func__,
      ReportText,
      Status
      ));
    return;
  }

  Status = EfiBootManagerUpdateConsoleVariable (ErrOut, DevicePath, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "%a: %s: adding to ErrOut: %r\n",
      __func__,
      ReportText,
      Status
      ));
    return;
  }

  DEBUG ((
    DEBUG_VERBOSE,
    "%a: %s: added to ConOut and ErrOut\n",
    __func__,
    ReportText
    ));
}

STATIC
VOID
PlatformRegisterFvBootOption (
  CONST EFI_GUID  *FileGuid,
  CHAR16          *Description,
  UINT32          Attributes,
  EFI_INPUT_KEY   *Key
  )
{
  EFI_STATUS                         Status;
  INTN                               OptionIndex;
  EFI_BOOT_MANAGER_LOAD_OPTION       NewOption;
  EFI_BOOT_MANAGER_LOAD_OPTION       *BootOptions;
  UINTN                              BootOptionCount;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_LOADED_IMAGE_PROTOCOL          *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePath;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  ASSERT_EFI_ERROR (Status);

  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  ASSERT (DevicePath != NULL);
  DevicePath = AppendDevicePathNode (
                 DevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  ASSERT (DevicePath != NULL);

  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             Attributes,
             Description,
             DevicePath,
             NULL,
             0
             );
  ASSERT_EFI_ERROR (Status);
  FreePool (DevicePath);

  BootOptions = EfiBootManagerGetLoadOptions (
                  &BootOptionCount,
                  LoadOptionTypeBoot
                  );

  OptionIndex = EfiBootManagerFindLoadOption (
                  &NewOption,
                  BootOptions,
                  BootOptionCount
                  );

  if (OptionIndex == -1) {
    Status = EfiBootManagerAddLoadOptionVariable (&NewOption, MAX_UINTN);
    ASSERT_EFI_ERROR (Status);
    if (Key != NULL) {
      Status = EfiBootManagerAddKeyOptionVariable (
                 NULL,
                 (UINT16)NewOption.OptionNumber,
                 0,
                 Key,
                 NULL
                 );
      ASSERT (Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
    }
  }

  EfiBootManagerFreeLoadOption (&NewOption);
  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
}

/** Boot a Fv Boot Option.

  This function is useful for booting the UEFI Shell as it is loaded
  as a non active boot option.

  @param[in] FileGuid      The File GUID.
  @param[in] Description   String describing the Boot Option.

**/
STATIC
VOID
PlatformBootFvBootOption (
  IN  CONST EFI_GUID  *FileGuid,
  IN  CHAR16          *Description
  )
{
  EFI_STATUS                         Status;
  EFI_BOOT_MANAGER_LOAD_OPTION       NewOption;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  FileNode;
  EFI_LOADED_IMAGE_PROTOCOL          *LoadedImage;
  EFI_DEVICE_PATH_PROTOCOL           *DevicePath;

  Status = gBS->HandleProtocol (
                  gImageHandle,
                  &gEfiLoadedImageProtocolGuid,
                  (VOID **)&LoadedImage
                  );
  ASSERT_EFI_ERROR (Status);

  //
  // The UEFI Shell was registered in PlatformRegisterFvBootOption ()
  // previously, thus it must still be available in this FV.
  //
  EfiInitializeFwVolDevicepathNode (&FileNode, FileGuid);
  DevicePath = DevicePathFromHandle (LoadedImage->DeviceHandle);
  ASSERT (DevicePath != NULL);
  DevicePath = AppendDevicePathNode (
                 DevicePath,
                 (EFI_DEVICE_PATH_PROTOCOL *)&FileNode
                 );
  ASSERT (DevicePath != NULL);

  Status = EfiBootManagerInitializeLoadOption (
             &NewOption,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             Description,
             DevicePath,
             NULL,
             0
             );
  ASSERT_EFI_ERROR (Status);
  FreePool (DevicePath);

  EfiBootManagerBoot (&NewOption);
}

STATIC
VOID
GetPlatformOptions (
  VOID
  )
{
  EFI_STATUS                      Status;
  EFI_BOOT_MANAGER_LOAD_OPTION    *CurrentBootOptions;
  EFI_BOOT_MANAGER_LOAD_OPTION    *BootOptions;
  EFI_INPUT_KEY                   *BootKeys;
  PLATFORM_BOOT_MANAGER_PROTOCOL  *PlatformBootManager;
  UINTN                           CurrentBootOptionCount;
  UINTN                           Index;
  UINTN                           BootCount;

  Status = gBS->LocateProtocol (
                  &gPlatformBootManagerProtocolGuid,
                  NULL,
                  (VOID **)&PlatformBootManager
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  Status = PlatformBootManager->GetPlatformBootOptionsAndKeys (
                                  &BootCount,
                                  &BootOptions,
                                  &BootKeys
                                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  //
  // Fetch the existent boot options. If there are none, CurrentBootCount
  // will be zeroed.
  //
  CurrentBootOptions = EfiBootManagerGetLoadOptions (
                         &CurrentBootOptionCount,
                         LoadOptionTypeBoot
                         );
  //
  // Process the platform boot options.
  //
  for (Index = 0; Index < BootCount; Index++) {
    INTN   Match;
    UINTN  BootOptionNumber;

    //
    // If there are any preexistent boot options, and the subject platform boot
    // option is already among them, then don't try to add it. Just get its
    // assigned boot option number so we can associate a hotkey with it. Note
    // that EfiBootManagerFindLoadOption() deals fine with (CurrentBootOptions
    // == NULL) if (CurrentBootCount == 0).
    //
    Match = EfiBootManagerFindLoadOption (
              &BootOptions[Index],
              CurrentBootOptions,
              CurrentBootOptionCount
              );
    if (Match >= 0) {
      BootOptionNumber = CurrentBootOptions[Match].OptionNumber;
    } else {
      //
      // Add the platform boot options as a new one, at the end of the boot
      // order. Note that if the platform provided this boot option with an
      // unassigned option number, then the below function call will assign a
      // number.
      //
      Status = EfiBootManagerAddLoadOptionVariable (
                 &BootOptions[Index],
                 MAX_UINTN
                 );
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "%a: failed to register \"%s\": %r\n",
          __func__,
          BootOptions[Index].Description,
          Status
          ));
        continue;
      }

      BootOptionNumber = BootOptions[Index].OptionNumber;
    }

    //
    // Register a hotkey with the boot option, if requested.
    //
    if (BootKeys[Index].UnicodeChar == L'\0') {
      continue;
    }

    Status = EfiBootManagerAddKeyOptionVariable (
               NULL,
               BootOptionNumber,
               0,
               &BootKeys[Index],
               NULL
               );
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: failed to register hotkey for \"%s\": %r\n",
        __func__,
        BootOptions[Index].Description,
        Status
        ));
    }
  }

  EfiBootManagerFreeLoadOptions (CurrentBootOptions, CurrentBootOptionCount);
  EfiBootManagerFreeLoadOptions (BootOptions, BootCount);
  FreePool (BootKeys);
}

STATIC
VOID
PlatformRegisterOptionsAndKeys (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_INPUT_KEY                 Enter;
  EFI_INPUT_KEY                 F2;
  EFI_INPUT_KEY                 Esc;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootOption;

  GetPlatformOptions ();

  //
  // Register ENTER as CONTINUE key
  //
  Enter.ScanCode    = SCAN_NULL;
  Enter.UnicodeChar = CHAR_CARRIAGE_RETURN;
  Status            = EfiBootManagerRegisterContinueKeyOption (0, &Enter, NULL);
  ASSERT_EFI_ERROR (Status);

  //
  // Map F2 and ESC to Boot Manager Menu
  //
  F2.ScanCode     = SCAN_F2;
  F2.UnicodeChar  = CHAR_NULL;
  Esc.ScanCode    = SCAN_ESC;
  Esc.UnicodeChar = CHAR_NULL;
  Status          = EfiBootManagerGetBootManagerMenu (&BootOption);
  ASSERT_EFI_ERROR (Status);
  Status = EfiBootManagerAddKeyOptionVariable (
             NULL,
             (UINT16)BootOption.OptionNumber,
             0,
             &F2,
             NULL
             );
  ASSERT (Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
  Status = EfiBootManagerAddKeyOptionVariable (
             NULL,
             (UINT16)BootOption.OptionNumber,
             0,
             &Esc,
             NULL
             );
  ASSERT (Status == EFI_SUCCESS || Status == EFI_ALREADY_STARTED);
}

//
// BDS Platform Functions
//

/**
  Do the platform init, can be customized by OEM/IBV
  Possible things that can be done in PlatformBootManagerBeforeConsole:
  > Update console variable: 1. include hot-plug devices;
  >                          2. Clear ConIn and add SOL for AMT
  > Register new Driver#### or Boot####
  > Register new Key####: e.g.: F12
  > Signal ReadyToLock event
  > Authentication action: 1. connect Auth devices;
  >                        2. Identify auto logon user.
**/

//
// The Meson SD/eMMC host driver tags each controller's device path with a
// fixed vendor GUID (see MesonSdMmcDxe).  Without help, BDS names their
// auto-created boot options "UEFI Misc Device" / "UEFI Misc Device 2"; this
// handler recognizes the two GUIDs and gives the boot menu real names.
//
STATIC EFI_GUID  mMesonEmmcDevicePathGuid =
  { 0x9ec8ef97, 0x39d8, 0x48a3, { 0x84, 0xaf, 0xa1, 0x94, 0xd9, 0x92, 0xc6, 0x37 } };
  //
  // FV file GUID of the X96SdDiag application (its .inf FILE_GUID).
  //
  STATIC EFI_GUID  gX96SdDiagFileGuid = {
    0x7c848874, 0x9d54, 0x4977, { 0x8d, 0xdc, 0x79, 0x33, 0x14, 0x84, 0x76, 0xa2 }
  };

STATIC EFI_GUID  mMesonSdDevicePathGuid =
  { 0xb072f2f9, 0x1405, 0x4a13, { 0xa2, 0x10, 0x3e, 0x5a, 0x4d, 0xd3, 0xb5, 0x20 } };

STATIC
CHAR16 *
EFIAPI
X96BootDescriptionHandler (
  IN EFI_HANDLE        Handle,
  IN CONST CHAR16      *DefaultDescription
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *Node;
  VENDOR_DEVICE_PATH        *Vendor;
  CONST CHAR16              *Name;

  Node = DevicePathFromHandle (Handle);
  if (Node == NULL) {
    return NULL;
  }

  Name = NULL;
  while (!IsDevicePathEnd (Node)) {
    if ((DevicePathType (Node) == HARDWARE_DEVICE_PATH) &&
        (DevicePathSubType (Node) == HW_VENDOR_DP))
    {
      Vendor = (VENDOR_DEVICE_PATH *)Node;
      if (CompareGuid (&Vendor->Guid, &mMesonEmmcDevicePathGuid)) {
        Name = L"eMMC";
        break;
      }

      if (CompareGuid (&Vendor->Guid, &mMesonSdDevicePathGuid)) {
        Name = L"SD Card";
        break;
      }
    }

    Node = NextDevicePathNode (Node);
  }

  if (Name == NULL) {
    return NULL;
  }

  //
  // The boot manager takes ownership of the returned string and frees it.
  //
  return AllocateCopyPool (StrSize (Name), Name);
}

STATIC
VOID
X96RegisterLinuxLoaderOption (
  VOID
  );

VOID
EFIAPI
PlatformBootManagerBeforeConsole (
  VOID
  )
{
  //
  // Name the eMMC and SD boot options in the menu (registered before BDS
  // enumerates boot options in PlatformBootManagerAfterConsole).
  //
  EfiBootManagerRegisterBootDescriptionHandler (X96BootDescriptionHandler);

  //
  // Signal EndOfDxe PI Event
  //
  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);

  //
  // Dispatch deferred images after EndOfDxe event.
  //
  EfiBootManagerDispatchDeferredImages ();

  //
  // Locate the PCI root bridges and make the PCI bus driver connect each,
  // non-recursively. This will produce a number of child handles with PciIo on
  // them.
  //
  FilterAndProcess (&gEfiPciRootBridgeIoProtocolGuid, NULL, Connect);

  //
  // Find all display class PCI devices (using the handles from the previous
  // step), and connect them non-recursively. This should produce a number of
  // child handles with GOPs on them.
  //
  FilterAndProcess (&gEfiPciIoProtocolGuid, IsPciDisplay, Connect);

  //
  // Now add the device path of all handles with GOP on them to ConOut and
  // ErrOut.
  //
  FilterAndProcess (&gEfiGraphicsOutputProtocolGuid, NULL, AddOutput);

  //
  // The core BDS code connects short-form USB device paths by explicitly
  // looking for handles with PCI I/O installed, and checking the PCI class
  // code whether it matches the one for a USB host controller. This means
  // non-discoverable USB host controllers need to have the non-discoverable
  // PCI driver attached first.
  //
  FilterAndProcess (&gEdkiiNonDiscoverableDeviceProtocolGuid, IsUsbHost, Connect);

  //
  // Add the hardcoded short-form USB keyboard device path to ConIn.
  //
  EfiBootManagerUpdateConsoleVariable (
    ConIn,
    (EFI_DEVICE_PATH_PROTOCOL *)&mUsbKeyboard,
    NULL
    );

  //
  // Add the hardcoded serial console device path to ConIn, ConOut, ErrOut.
  //
  STATIC_ASSERT (
    FixedPcdGet8 (PcdDefaultTerminalType) == 4,
    "PcdDefaultTerminalType must be TTYTERM"
    );
  STATIC_ASSERT (
    FixedPcdGet8 (PcdUartDefaultParity) != 0,
    "PcdUartDefaultParity must be set to an actual value, not 'default'"
    );
  STATIC_ASSERT (
    FixedPcdGet8 (PcdUartDefaultStopBits) != 0,
    "PcdUartDefaultStopBits must be set to an actual value, not 'default'"
    );

  CopyGuid (&mSerialConsole.TermType.Guid, &gEfiTtyTermGuid);

  EfiBootManagerUpdateConsoleVariable (
    ConIn,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole,
    NULL
    );
  EfiBootManagerUpdateConsoleVariable (
    ConOut,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole,
    NULL
    );
  EfiBootManagerUpdateConsoleVariable (
    ErrOut,
    (EFI_DEVICE_PATH_PROTOCOL *)&mSerialConsole,
    NULL
    );

  //
  // Register platform-specific boot options and keyboard shortcuts.
  //
  PlatformRegisterOptionsAndKeys ();
}

STATIC
VOID
HandleCapsules (
  VOID
  )
{
  ESRT_MANAGEMENT_PROTOCOL  *EsrtManagement;
  EFI_PEI_HOB_POINTERS      HobPointer;
  EFI_CAPSULE_HEADER        *CapsuleHeader;
  BOOLEAN                   NeedReset;
  EFI_STATUS                Status;

  DEBUG ((DEBUG_INFO, "%a: processing capsules ...\n", __func__));

  Status = gBS->LocateProtocol (
                  &gEsrtManagementProtocolGuid,
                  NULL,
                  (VOID **)&EsrtManagement
                  );
  if (!EFI_ERROR (Status)) {
    EsrtManagement->SyncEsrtFmp ();
  }

  //
  // Find all capsule images from hob
  //
  HobPointer.Raw = GetHobList ();
  NeedReset      = FALSE;
  while ((HobPointer.Raw = GetNextHob (
                             EFI_HOB_TYPE_UEFI_CAPSULE,
                             HobPointer.Raw
                             )) != NULL)
  {
    CapsuleHeader = (VOID *)(UINTN)HobPointer.Capsule->BaseAddress;

    Status = ProcessCapsuleImage (CapsuleHeader);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: failed to process capsule %p - %r\n",
        __func__,
        CapsuleHeader,
        Status
        ));
      return;
    }

    NeedReset      = TRUE;
    HobPointer.Raw = GET_NEXT_HOB (HobPointer);
  }

  if (NeedReset) {
    DEBUG ((
      DEBUG_WARN,
      "%a: capsule update successful, resetting ...\n",
      __func__
      ));

    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop ();
  }

  //
  // X96 addition: capsule-on-disk, processed in THIS boot.
  //
  // The stock flow above only handles capsules found in HOBs, i.e. ones
  // that survived a reset in RAM.  That path can never work on this SoC:
  // every reset goes through BL2 DDR retraining, so memory does not
  // persist, and there is no CapsulePei to reclaim it anyway.  Instead,
  // capsules are required to be built with Flags = 0 (no
  // PERSIST_ACROSS_RESET); CoDRelocateCapsule() then loads them from
  // \EFI\UpdateCapsule on the active boot option's ESP and hands them to
  // UpdateCapsule(), which processes flagless capsules immediately -
  // same boot, no RAM persistence required.
  //
  // CoDRelocateCapsule() clears the OsIndications flag and deletes the
  // capsule files up front ("always remove file to avoid deadloop"), so a
  // reset here cannot loop: the next boot finds no flag and no files.
  //
  if (CoDCheckCapsuleOnDiskFlag ()) {
    DEBUG ((DEBUG_INFO, "%a: processing capsules on disk ...\n", __func__));
    Status = CoDRelocateCapsule (3);
    if (EFI_ERROR (Status)) {
      DEBUG ((
        DEBUG_ERROR,
        "%a: capsule on disk processing failed - %r\n",
        __func__,
        Status
        ));
      return;
    }

    DEBUG ((
      DEBUG_WARN,
      "%a: capsule on disk dispatched, resetting ...\n",
      __func__
      ));
    gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    CpuDeadLoop ();
  }
}

#define VERSION_STRING_PREFIX  L"Tianocore/EDK2 firmware version "

/**
  This functions checks the value of BootDiscoverPolicy variable and
  connect devices of class specified by that variable. Then it refreshes
  Boot order for newly discovered boot device.

  @retval  EFI_SUCCESS  Devices connected successfully or connection
                        not required.
  @retval  others       Return values from GetVariable(), LocateProtocol()
                        and ConnectDeviceClass().
**/
STATIC
EFI_STATUS
BootDiscoveryPolicyHandler (
  VOID
  )
{
  EFI_STATUS                        Status;
  UINT32                            DiscoveryPolicy;
  UINT32                            DiscoveryPolicyOld;
  UINTN                             Size;
  EFI_BOOT_MANAGER_POLICY_PROTOCOL  *BMPolicy;
  EFI_GUID                          *Class;

  Size   = sizeof (DiscoveryPolicy);
  Status = gRT->GetVariable (
                  BOOT_DISCOVERY_POLICY_VAR,
                  &gBootDiscoveryPolicyMgrFormsetGuid,
                  NULL,
                  &Size,
                  &DiscoveryPolicy
                  );
  if (Status == EFI_NOT_FOUND) {
    DiscoveryPolicy = PcdGet32 (PcdBootDiscoveryPolicy);
    Status          = PcdSet32S (PcdBootDiscoveryPolicy, DiscoveryPolicy);
    if (Status == EFI_NOT_FOUND) {
      return EFI_SUCCESS;
    } else if (EFI_ERROR (Status)) {
      return Status;
    }
  } else if (EFI_ERROR (Status)) {
    return Status;
  }

  if (DiscoveryPolicy == BDP_CONNECT_MINIMAL) {
    return EFI_SUCCESS;
  }

  switch (DiscoveryPolicy) {
    case BDP_CONNECT_NET:
      Class = &gEfiBootManagerPolicyNetworkGuid;
      break;
    case BDP_CONNECT_ALL:
      Class = &gEfiBootManagerPolicyConnectAllGuid;
      break;
    default:
      DEBUG ((
        DEBUG_INFO,
        "%a - Unexpected DiscoveryPolicy (0x%x). Run Minimal Discovery Policy\n",
        __func__,
        DiscoveryPolicy
        ));
      return EFI_SUCCESS;
  }

  Status = gBS->LocateProtocol (
                  &gEfiBootManagerPolicyProtocolGuid,
                  NULL,
                  (VOID **)&BMPolicy
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_INFO,
      "%a - Failed to locate gEfiBootManagerPolicyProtocolGuid."
      "Driver connect will be skipped.\n",
      __func__
      ));
    return Status;
  }

  Status = BMPolicy->ConnectDeviceClass (BMPolicy, Class);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "%a - ConnectDeviceClass returns - %r\n", __func__, Status));
    return Status;
  }

  //
  // Refresh Boot Options if Boot Discovery Policy has been changed
  //
  Size   = sizeof (DiscoveryPolicyOld);
  Status = gRT->GetVariable (
                  BOOT_DISCOVERY_POLICY_OLD_VAR,
                  &gBootDiscoveryPolicyMgrFormsetGuid,
                  NULL,
                  &Size,
                  &DiscoveryPolicyOld
                  );
  if ((Status == EFI_NOT_FOUND) || (DiscoveryPolicyOld != DiscoveryPolicy)) {
    EfiBootManagerRefreshAllBootOption ();

    Status = gRT->SetVariable (
                    BOOT_DISCOVERY_POLICY_OLD_VAR,
                    &gBootDiscoveryPolicyMgrFormsetGuid,
                    EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
                    sizeof (DiscoveryPolicyOld),
                    &DiscoveryPolicy
                    );
  }

  return EFI_SUCCESS;
}

/**
  Count enumerated USB devices (once per physical device, not per interface):
  every device exposes interface 0, so counting UsbIo handles whose interface
  number is 0 gives the device count regardless of composite devices.
**/
STATIC
UINTN
X96CountUsbDevices (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    *Handles;
  UINTN                         Count;
  UINTN                         Index;
  EFI_USB_IO_PROTOCOL           *UsbIo;
  EFI_USB_INTERFACE_DESCRIPTOR  IfDesc;
  UINTN                         Devices;

  Devices = 0;
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return 0;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid, (VOID **)&UsbIo);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
    if (!EFI_ERROR (Status) && (IfDesc.InterfaceNumber == 0)) {
      Devices++;
    }
  }

  FreePool (Handles);
  return Devices;
}

/**
  Count every downstream port that reports a connected device, across all root
  hubs (via EFI_USB2_HC_PROTOCOL) and every enumerated USB hub (via the USB hub
  class port-status request).  Each such connection is exactly one device that
  should be enumerated; comparing this to X96CountUsbDevices() tells us whether
  a device is present but not (yet) enumerated - the completeness signal used to
  decide whether the USB tree is fully up, with no assumption about how many
  devices to expect.
**/
STATIC
UINTN
X96CountConnectedUsbPorts (
  VOID
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                *Handles;
  UINTN                     Count;
  UINTN                     Index;
  UINTN                     Total;
  EFI_USB2_HC_PROTOCOL      *Usb2Hc;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  UINT8                     MaxSpeed;
  UINT8                     PortCount;
  UINT8                     Is64Bit;
  UINT8                     Port;
  EFI_USB_PORT_STATUS       PortStatus;
  EFI_USB_DEVICE_DESCRIPTOR DevDesc;
  EFI_USB_DEVICE_REQUEST    Request;
  UINT8                     HubDesc[8];
  UINT8                     HubStatus[4];
  UINT32                    XferStatus;

  Total = 0;

  //
  // Root-hub ports.
  //
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsb2HcProtocolGuid, NULL, &Count, &Handles);
  if (!EFI_ERROR (Status) && (Handles != NULL)) {
    for (Index = 0; Index < Count; Index++) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiUsb2HcProtocolGuid, (VOID **)&Usb2Hc);
      if (EFI_ERROR (Status) || (Usb2Hc->GetCapability (Usb2Hc, &MaxSpeed, &PortCount, &Is64Bit) != EFI_SUCCESS)) {
        continue;
      }

      for (Port = 0; Port < PortCount; Port++) {
        if (!EFI_ERROR (Usb2Hc->GetRootHubPortStatus (Usb2Hc, Port, &PortStatus)) &&
            ((PortStatus.PortStatus & USB_PORT_STAT_CONNECTION) != 0))
        {
          Total++;
        }
      }
    }

    FreePool (Handles);
  }

  //
  // Downstream ports of every enumerated hub.
  //
  Handles = NULL;
  Count   = 0;
  Status  = gBS->LocateHandleBuffer (ByProtocol, &gEfiUsbIoProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status) || (Handles == NULL)) {
    return Total;
  }

  for (Index = 0; Index < Count; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiUsbIoProtocolGuid, (VOID **)&UsbIo);
    if (EFI_ERROR (Status) || (UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc) != EFI_SUCCESS)) {
      continue;
    }

    //
    // Only hubs (device class 0x09) have downstream ports; each hub device is
    // visited once (its interface-0 UsbIo).
    //
    if (DevDesc.DeviceClass != 0x09) {
      continue;
    }

    //
    // GET_DESCRIPTOR(hub): class, device-to-host; descriptor type 0x29 (USB2
    // hub).  bNbrPorts is at byte offset 2.
    //
    ZeroMem (&Request, sizeof (Request));
    Request.RequestType = 0xA0;
    Request.Request     = USB_REQ_GET_DESCRIPTOR;
    Request.Value       = (UINT16)(0x29 << 8);
    Request.Index       = 0;
    Request.Length      = sizeof (HubDesc);
    Status              = UsbIo->UsbControlTransfer (
                                   UsbIo,
                                   &Request,
                                   EfiUsbDataIn,
                                   1000,
                                   HubDesc,
                                   sizeof (HubDesc),
                                   &XferStatus
                                   );
    if (EFI_ERROR (Status)) {
      continue;
    }

    for (Port = 1; Port <= HubDesc[2]; Port++) {
      //
      // GET_STATUS(port): class, other-recipient; 4-byte port status/change.
      // wPortStatus BIT0 = current connect status.
      //
      ZeroMem (&Request, sizeof (Request));
      Request.RequestType = 0xA3;
      Request.Request     = USB_REQ_GET_STATUS;
      Request.Value       = 0;
      Request.Index       = Port;
      Request.Length      = sizeof (HubStatus);
      Status              = UsbIo->UsbControlTransfer (
                                     UsbIo,
                                     &Request,
                                     EfiUsbDataIn,
                                     1000,
                                     HubStatus,
                                     sizeof (HubStatus),
                                     &XferStatus
                                     );
      if (!EFI_ERROR (Status) && ((HubStatus[0] & USB_PORT_STAT_CONNECTION) != 0)) {
        Total++;
      }
    }
  }

  FreePool (Handles);
  return Total;
}

/**
  Do the platform specific action after the console is ready
  Possible things that can be done in PlatformBootManagerAfterConsole:
  > Console post action:
    > Dynamically switch output mode from 100x31 to 80x25 for certain scenario
    > Signal console ready platform customized event
  > Run diagnostics like memory testing
  > Connect certain devices
  > Dispatch additional option roms
  > Special boot: e.g.: USB boot, enter UI
**/
VOID
EFIAPI
PlatformBootManagerAfterConsole (
  VOID
  )
{
  EFI_STATUS                           DiagStatus;
  STATIC EFI_GUID  X96SdDiagGuid = {
    0x25b7b0e4, 0x8c11, 0x435e, { 0x9d, 0x7b, 0x62, 0xef, 0xf9, 0x77, 0xd1, 0x72 }
  };
  UINT8                                Diag[96];
  UINTN                                DiagSize;
  UINT32                               Attempts;
  UINT32                               LastStatus;
  UINT32                               Csd[4];
  UINT32                               ClockHz;
  UINT32                               Echo;

  //
  // The SD host exported its identification diagnostics as a boot-services
  // variable; print them on the local console (serial and HDMI) - the only
  // observable path on serial-less units.
  //
  DiagSize = sizeof (Diag);
  DiagStatus = gRT->GetVariable (L"X96SdDiag", &X96SdDiagGuid, NULL, &DiagSize, Diag);
  if (!EFI_ERROR (DiagStatus)) {
    ClockHz = Diag[4] | (Diag[5] << 8) | (Diag[6] << 16) | ((UINT32)Diag[7] << 24);
    Echo    = Diag[8] | (Diag[9] << 8) | (Diag[10] << 16) | ((UINT32)Diag[11] << 24);
    Attempts   = Diag[36] | (Diag[37] << 8) | (Diag[38] << 16) | ((UINT32)Diag[39] << 24);
    LastStatus = Diag[40] | (Diag[41] << 8) | (Diag[42] << 16) | ((UINT32)Diag[43] << 24);
    Print (
      L"X96 SD diag: tuned=%u co=%u tx=%u clk=%uHz attempts=%u last-status=0x%x echo=%08x\n",
      Diag[0],
      Diag[1],
      Diag[2],
      ClockHz,
      Attempts,
      LastStatus,
      Echo
      );
    CopyMem (Csd, &Diag[12], sizeof (Csd));
    Print (
      L"X96 SD CSD: %016x %016x\n",
      ((UINT64)Csd[1] << 32) | Csd[0],
      ((UINT64)Csd[3] << 32) | Csd[2]
      );
  }

  EFI_STATUS                    Status;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *GraphicsOutput;
  UINTN                         FirmwareVerLength;
  UINTN                         PosX;
  UINTN                         PosY;
  EFI_INPUT_KEY                 Key;

  FirmwareVerLength = StrLen (PcdGetPtr (PcdFirmwareVersionString));

  //
  // Show the splash screen.
  //
  Status = BootLogoEnableLogo ();
  if (EFI_ERROR (Status)) {
    if (FirmwareVerLength > 0) {
      Print (
        VERSION_STRING_PREFIX L"%s\n",
        PcdGetPtr (PcdFirmwareVersionString)
        );
    }

    Print (L"Press ESCAPE for boot options ");
  } else if (FirmwareVerLength > 0) {
    Status = gBS->HandleProtocol (
                    gST->ConsoleOutHandle,
                    &gEfiGraphicsOutputProtocolGuid,
                    (VOID **)&GraphicsOutput
                    );
    if (!EFI_ERROR (Status)) {
      PosX = (GraphicsOutput->Mode->Info->HorizontalResolution -
              (StrLen (VERSION_STRING_PREFIX) + FirmwareVerLength) *
              EFI_GLYPH_WIDTH) / 2;
      PosY = 0;

      PrintXY (
        PosX,
        PosY,
        NULL,
        NULL,
        VERSION_STRING_PREFIX L"%s",
        PcdGetPtr (PcdFirmwareVersionString)
        );
    }
  }

  //
  // Connect device specified by BootDiscoverPolicy variable and
  // refresh Boot order for newly discovered boot devices
  //
  BootDiscoveryPolicyHandler ();

  //
  // On ARM, there is currently no reason to use the phased capsule
  // update approach where some capsules are dispatched before EndOfDxe
  // and some are dispatched after. So just handle all capsules here,
  // when the console is up and we can actually give the user some
  // feedback about what is going on.
  //
  HandleCapsules ();

  //
  // Register UEFI Shell
  //
  Key.ScanCode    = SCAN_NULL;
  Key.UnicodeChar = L's';
  PlatformRegisterFvBootOption (&gUefiShellFileGuid, L"UEFI Shell", 0, &Key);

  //
  // X96 SD diagnostics is an FV-resident app bound to the 'd' hotkey: with
  // no serial and possibly no filesystem, pressing 'd' during the countdown
  // prints the SD identification diagnostics on the local console.
  //
  {
    EFI_INPUT_KEY  KeyD;

    KeyD.ScanCode    = SCAN_NULL;
    KeyD.UnicodeChar = L'd';
    PlatformRegisterFvBootOption (&gX96SdDiagFileGuid, L"X96 SD Diagnostic", 0, &KeyD);
  }

  //
  // X96 additions: a plainly-named entry for the setup UI (the same UiApp
  // image PcdBootManagerMenuFile names), and the MaskROM USB-recovery
  // reboot helper embedded in the firmware volume.
  //
  // LOAD_OPTION_ACTIVE is what makes an entry appear in the boot menu:
  // BootManagerMenuApp skips every option without it, so the stock zero
  // attributes used for the UEFI Shell above leave an entry reachable only
  // by hotkey or BootNext.  LOAD_OPTION_CATEGORY_APP keeps BDS from ever
  // booting them automatically while walking BootOrder (BdsEntry only
  // auto-boots LOAD_OPTION_CATEGORY_BOOT), which matters most for the
  // MaskROM entry: a fall-through to it would drop the board into USB
  // recovery instead of trying the next OS.
  //
  PlatformRegisterFvBootOption (
    (CONST EFI_GUID *)PcdGetPtr (PcdBootManagerMenuFile),
    L"Firmware Setup",
    LOAD_OPTION_ACTIVE | LOAD_OPTION_CATEGORY_APP,
    NULL
    );

  //
  // Reliable USB enumeration.
  //
  // On this DWC3 controller EDK2's one-shot xHCI bring-up is timing-marginal:
  // under RELEASE speed enumeration intermittently fails - sometimes the whole
  // tree comes up empty, sometimes the deepest device (storage several hubs
  // down) is the only one missing.  Verbose DEBUG masks it by spacing the boot
  // out.  Recover by retrying the full controller init until the tree is
  // COMPLETE, judged generically: every port that reports a connected device
  // (root hub + every enumerated hub) has an enumerated device behind it
  // (X96CountConnectedUsbPorts() vs X96CountUsbDevices()).  No assumption is
  // made about how many devices to expect.
  //
  // Each pass settles (async hub bring-up runs during the stall) and reconnects
  // so drivers bind.  If the tree is complete, stop.  If it is still growing,
  // settle again WITHOUT re-initing (never tear down a partially-good tree).
  // If it is stuck incomplete (or empty), disconnect/reconnect the XHCI host
  // controller - which tears down XhciDxe AND the non-discoverable PCI shim, so
  // the reconnect re-runs the complete DWC3/PHY bring-up (the mUsbInitialized
  // guard was removed for this) plus a fresh xHCI reset - i.e. an independent
  // fresh attempt.  Only the USB host controllers are touched (never
  // ConnectAll), so the onboard NIC is left alone.
  //
  {
    EFI_HANDLE  *HcHandles;
    UINTN       HcCount;
    UINTN       HcIndex;
    UINTN       Attempt;
    UINTN       Devices;
    UINTN       ConnectedPorts;
    UINTN       PrevDevices;
    UINTN       MaxPorts;

    //
    // MaxPorts tracks the most of the tree ever made visible this boot.  A
    // re-init can regress to a TRUNCATED tree - a hub near the root fails to
    // enumerate, hiding its whole subtree, so the few visible ports can equal
    // the few enumerated devices and look "complete".  Requiring the current
    // view to be at least MaxPorts rejects such a regressed/truncated view.
    //
    PrevDevices = 0;
    MaxPorts    = 0;

    for (Attempt = 0; Attempt < X96_USB_ENUM_MAX_ATTEMPTS; Attempt++) {
      gBS->Stall (X96_USB_SETTLE_STALL_US);

      HcHandles = NULL;
      HcCount   = 0;
      gBS->LocateHandleBuffer (ByProtocol, &gEfiUsb2HcProtocolGuid, NULL, &HcCount, &HcHandles);
      for (HcIndex = 0; (HcHandles != NULL) && (HcIndex < HcCount); HcIndex++) {
        gBS->ConnectController (HcHandles[HcIndex], NULL, NULL, TRUE);
      }

      Devices        = X96CountUsbDevices ();
      ConnectedPorts = X96CountConnectedUsbPorts ();
      if (ConnectedPorts > MaxPorts) {
        MaxPorts = ConnectedPorts;
      }

      DEBUG ((
        DEBUG_INFO,
        "X96 USB: enum attempt %d -> %d devices / %d connected ports (max %d)\n",
        Attempt,
        Devices,
        ConnectedPorts,
        MaxPorts
        ));

      //
      // Complete: every connected port has an enumerated device behind it, AND
      // we are seeing the whole tree (not a regressed/truncated view).
      //
      if ((Devices > 0) && (Devices >= ConnectedPorts) && (ConnectedPorts >= MaxPorts)) {
        if (HcHandles != NULL) {
          FreePool (HcHandles);
        }

        break;
      }

      //
      // Making progress (more devices than last pass): the asynchronous bring-up
      // is still running, let it continue - do NOT re-init.
      //
      if (Devices > PrevDevices) {
        if (HcHandles != NULL) {
          FreePool (HcHandles);
        }

        PrevDevices = Devices;
        continue;
      }

      //
      // Empty, stuck incomplete, or regressed: force a full controller re-init.
      // Reset PrevDevices so the fresh tree's growth counts as progress again.
      //
      if (HcHandles != NULL) {
        for (HcIndex = 0; HcIndex < HcCount; HcIndex++) {
          gBS->DisconnectController (HcHandles[HcIndex], NULL, NULL);
          gBS->ConnectController (HcHandles[HcIndex], NULL, NULL, TRUE);
        }

        FreePool (HcHandles);
      }

      PrevDevices = 0;
    }

    //
    // The reconnects recreated the USB keyboard handle; re-establish the
    // consoles so it drives the boot menu / HII, then refresh boot options so
    // the now-enumerated devices get entries.
    //
    EfiBootManagerConnectAllDefaultConsoles ();
    EfiBootManagerRefreshAllBootOption ();
  }

  //
  // Zero-touch Linux boot: register \X96LinuxBoot.efi (if present on an
  // ESP) at the front of the boot order.
  //
  X96RegisterLinuxLoaderOption ();
}

//
// Register the booti-style Linux loader living on the ESP so that a
// cold boot turns into Linux without a keyboard: it lands first in the
// boot order and BDS auto-boots it within the timeout.  If the loader
// is missing (or returns without handoff), BDS falls back to the rest
// of the boot order (GRUB, eMMC chain...).  ESC still reaches the menu.
//
STATIC
VOID
X96RegisterLinuxLoaderOption (
  VOID
  )
{
  STATIC CONST CHAR16           *LoaderPath = L"\\X96LinuxBoot.efi";
  EFI_STATUS                     Status;
  EFI_HANDLE                    *SfsHandles;
  EFI_HANDLE                    *SfsHandlesCopy;
  UINTN                          SfsCount;
  UINTN                          Index;
  EFI_DEVICE_PATH_PROTOCOL      *DevicePath;
  EFI_BOOT_MANAGER_LOAD_OPTION   Option;
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                          BootOptionCount;
  EFI_INPUT_KEY                  Key;

  SfsHandles = NULL;
  DevicePath = NULL;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &SfsCount,
                  &SfsHandles
                  );
  if (EFI_ERROR (Status)) {
    return;
  }

  SfsHandlesCopy = SfsHandles;
  for (Index = 0; Index < SfsCount; Index++) {
    EFI_FILE_PROTOCOL  *Root;
    EFI_FILE_PROTOCOL  *File;

    Root = NULL;
    File = NULL;

    Status = gBS->HandleProtocol (
                    SfsHandlesCopy[Index],
                    &gEfiSimpleFileSystemProtocolGuid,
                    (VOID **)&Root
                    );
    if (EFI_ERROR (Status) || (Root == NULL)) {
      continue;
    }

    File = NULL;
    Status = Root->Open (
                     Root,
                     &File,
                     (CHAR16 *)LoaderPath,
                     EFI_FILE_MODE_READ,
                     0
                     );
    if ((Status == EFI_SUCCESS) && (File != NULL)) {
      File->Close (File);
      DevicePath = FileDevicePath (SfsHandlesCopy[Index], (CHAR16 *)LoaderPath);
      break;
    }
  }

  FreePool (SfsHandles);

  if (DevicePath == NULL) {
    DEBUG ((DEBUG_INFO, "%a: %s not present on any ESP yet\n", __func__, LoaderPath));
    return;
  }

  Status = EfiBootManagerInitializeLoadOption (
             &Option,
             LoadOptionNumberUnassigned,
             LoadOptionTypeBoot,
             LOAD_OPTION_ACTIVE,
             L"Linux (X96Loader)",
             DevicePath,
             NULL,
             0
             );
  FreePool (DevicePath);

  //
  // Only add once; on repeat boots keep the stored position untouched so
  // user-chosen ordering via the boot menu is respected.
  //
  BootOptions = EfiBootManagerGetLoadOptions (&BootOptionCount, LoadOptionTypeBoot);
  if (EfiBootManagerFindLoadOption (&Option, BootOptions, BootOptionCount) >= 0) {
    EfiBootManagerFreeLoadOption (&Option);
    EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);
    return;
  }

  EfiBootManagerFreeLoadOptions (BootOptions, BootOptionCount);

  Key.ScanCode    = SCAN_NULL;
  Key.UnicodeChar = L'l';

  {
    //
    // EfiBootManagerAddKeyOptionVariable needs the final number, so add the
    // option first, then bind the hotkey.
    //
    EFI_BOOT_MANAGER_LOAD_OPTION  AddOption = Option;

    Status = EfiBootManagerAddLoadOptionVariable (&AddOption, 0);
    if (!EFI_ERROR (Status)) {
      EfiBootManagerAddKeyOptionVariable (NULL, AddOption.OptionNumber, 0, &Key, NULL);
    }

    EfiBootManagerFreeLoadOption (&Option);
    return;
  }
}

/**
  This function is called each second during the boot manager waits the
  timeout.

  @param TimeoutRemain  The remaining timeout.
**/
VOID
EFIAPI
PlatformBootManagerWaitCallback (
  UINT16  TimeoutRemain
  )
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION  Black;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL_UNION  White;
  UINT16                               Timeout;
  EFI_STATUS                           Status;

  Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  Black.Raw = 0x00000000;
  White.Raw = 0x00FFFFFF;

  Status = BootLogoUpdateProgress (
             White.Pixel,
             Black.Pixel,
             L"Press ESCAPE for boot options",
             White.Pixel,
             (Timeout - TimeoutRemain) * 100 / Timeout,
             0
             );
  if (EFI_ERROR (Status)) {
    Print (L".");
  }
}

/**
  The function is called when no boot option could be launched,
  including platform recovery options and options pointing to applications
  built into firmware volumes.

  If this function returns, BDS attempts to enter an infinite loop.
**/
VOID
EFIAPI
PlatformBootManagerUnableToBoot (
  VOID
  )
{
  EFI_STATUS                    Status;
  EFI_BOOT_MANAGER_LOAD_OPTION  BootManagerMenu;
  EFI_BOOT_MANAGER_LOAD_OPTION  *BootOptions;
  UINTN                         OldBootOptionCount;
  UINTN                         NewBootOptionCount;

  //
  // Record the total number of boot configured boot options
  //
  BootOptions = EfiBootManagerGetLoadOptions (
                  &OldBootOptionCount,
                  LoadOptionTypeBoot
                  );
  EfiBootManagerFreeLoadOptions (BootOptions, OldBootOptionCount);

  //
  // Connect all devices, and regenerate all boot options
  //
  EfiBootManagerConnectAll ();
  EfiBootManagerRefreshAllBootOption ();

  //
  // Boot the 'UEFI Shell'. If the Pcd is not set, the UEFI Shell is not
  // an active boot option and must be manually selected through UiApp
  // (at least during the fist boot).
  //
  if (FixedPcdGetBool (PcdUefiShellDefaultBootEnable)) {
    PlatformBootFvBootOption (
      &gUefiShellFileGuid,
      L"UEFI Shell (default)"
      );
  }

  //
  // Record the updated number of boot configured boot options
  //
  BootOptions = EfiBootManagerGetLoadOptions (
                  &NewBootOptionCount,
                  LoadOptionTypeBoot
                  );
  EfiBootManagerFreeLoadOptions (BootOptions, NewBootOptionCount);

  //
  // If the number of configured boot options has changed, reboot
  // the system so the new boot options will be taken into account
  // while executing the ordinary BDS bootflow sequence.
  // *Unless* persistent varstore is being emulated, since we would
  // then end up in an endless reboot loop.
  //
  if (!PcdGetBool (PcdEmuVariableNvModeEnable)) {
    if (NewBootOptionCount != OldBootOptionCount) {
      DEBUG ((
        DEBUG_WARN,
        "%a: rebooting after refreshing all boot options\n",
        __func__
        ));
      gRT->ResetSystem (EfiResetCold, EFI_SUCCESS, 0, NULL);
    }
  }

  Status = EfiBootManagerGetBootManagerMenu (&BootManagerMenu);
  if (EFI_ERROR (Status)) {
    return;
  }

  for ( ; ;) {
    EfiBootManagerBoot (&BootManagerMenu);
  }
}
