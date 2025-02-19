/*
 * Copyright (c) 2009, Google Inc.
 * All rights reserved.
 *
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of The Linux Foundation nor
 *       the names of its contributors may be used to endorse or promote
 *       products derived from this software without specific prior written
 *       permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 *  Changes from Qualcomm Innovation Center are provided under the following license:
 *
 *  Copyright (c) 2022 - 2023 Qualcomm Innovation Center, Inc. All rights
 *  reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted (subject to the limitations in the
 *  disclaimer below) provided that the following conditions are met:
 *
 *      * Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      * Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials provided
 *        with the distribution.
 *
 *      * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
 *        contributors may be used to endorse or promote products derived
 *        from this software without specific prior written permission.
 *
 *  NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
 *  GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
 *  HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
 *  ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 *  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 *  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 *  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 *  OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 *  IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "AutoGen.h"
#include "BootLinux.h"
#include "BootStats.h"
#include "KeyPad.h"
#include "LinuxLoaderLib.h"
#include <FastbootLib/FastbootMain.h>
#include <Library/DeviceInfo.h>
#include <Library/DrawUI.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PartitionTableUpdate.h>
#include <Library/ShutdownServices.h>
#include <Library/StackCanary.h>
#include <Library/ThreadStack.h>
#include <Library/HypervisorMvCalls.h>
#include <Library/UpdateCmdLine.h>
#include <Protocol/EFICardInfo.h>
#include <Protocol/EFIClock.h>
#include <Protocol/EFIPmicSdam.h>
#include "RecoveryInfo.h"

#define MAX_APP_STR_LEN 64
#define MAX_NUM_FS 10
#define DEFAULT_STACK_CHK_GUARD 0xc0c0c0c0

#if HIBERNATION_SUPPORT_NO_AES
VOID BootIntoHibernationImage (BootInfo *Info, BOOLEAN *SetRotAndBootState);
#endif

STATIC BOOLEAN BootReasonAlarm = FALSE;
STATIC BOOLEAN BootIntoFastboot = FALSE;
STATIC BOOLEAN BootIntoRecovery = FALSE;
UINT64 FlashlessBootImageAddr = 0;
UINT64 NetworkBootImageAddr = 0;
STATIC UINT32 BootDeviceType = EFI_MAX_FLASH_TYPE;

// Deactivates MDTP by entering recovery UI
STATIC EFI_STATUS MdtpDisable (VOID) {
  BOOLEAN MdtpActive = FALSE;
  EFI_STATUS Status = EFI_SUCCESS;
  QCOM_MDTP_PROTOCOL *MdtpProtocol;

  if (FixedPcdGetBool (EnableMdtpSupport)) {
    Status = IsMdtpActive (&MdtpActive);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    if (MdtpActive) {
      Status = gBS->LocateProtocol (&gQcomMdtpProtocolGuid, NULL, (VOID **)&MdtpProtocol);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "Failed to locate MDTP protocol, Status=%r\n", Status));
        return Status;
      }
      Status = MdtpProtocol->MdtpDeactivate (MdtpProtocol, FALSE);
    }
  }

  return Status;
}

// Retrieves the reboot reason
STATIC UINT8 GetRebootReason (UINT32 *ResetReason) {
  EFI_RESETREASON_PROTOCOL *RstReasonIf;
  EFI_STATUS Status;

  Status = gBS->LocateProtocol (&gEfiResetReasonProtocolGuid, NULL, (VOID **)&RstReasonIf);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error locating the reset reason protocol\n"));
    return Status;
  }

  RstReasonIf->GetResetReason (RstReasonIf, ResetReason, NULL, NULL);
  if (RstReasonIf->Revision >= EFI_RESETREASON_PROTOCOL_REVISION && ClearResetReason ()) {
    RstReasonIf->ClearResetReason (RstReasonIf);
  }
  return Status;
}

// Sets the default audio framework
STATIC VOID SetDefaultAudioFw () {
  CHAR8 AudioFW[MAX_AUDIO_FW_LENGTH];
  STATIC CHAR8* Src;
  STATIC CHAR8* AUDIOFRAMEWORK;
  STATIC UINT32 Length;
  EFI_STATUS Status;

  AUDIOFRAMEWORK = GetAudioFw ();
  if (AUDIOFRAMEWORK == NULL) {
    DEBUG ((EFI_D_ERROR, "AUDIOFRAMEWORK is NULL\n"));
    return;
  }

  if (AsciiStrLen (AUDIOFRAMEWORK) > 0) {
    Status = ReadAudioFrameWork (&Src, &Length);
    if (Status == EFI_SUCCESS) {
      if ((AsciiStrLen (Src) == 0)) {
        AsciiStrnCpyS (AudioFW, MAX_AUDIO_FW_LENGTH, AUDIOFRAMEWORK, AsciiStrLen (AUDIOFRAMEWORK));
        StoreAudioFrameWork (AudioFW, AsciiStrLen (AUDIOFRAMEWORK));
      }
    }
  }
}

// Prints the CPU frequency
STATIC VOID PrintCpuFrequency (VOID) {
  EFI_CLOCK_PROTOCOL  *ClockProtocol = NULL;
  EFI_KERNEL_PROTOCOL *KernIntf = NULL;
  EFI_STATUS  Status = EFI_SUCCESS;
  UINT32  numOfCore = 0;
  UINT32  pnPerfLevel;
  UINT32  pnFrequencyHz;
  UINT32  pnRequiredVoltage;
  UINT32  i = 0;

  Status = gBS->LocateProtocol (&gEfiKernelProtocolGuid, NULL, (VOID **)&KernIntf);
  if (EFI_SUCCESS != Status) {
    return;
  }

  numOfCore = KernIntf->MpCpu->MpcoreGetAvailCpuCount ();
  if (!numOfCore) {
    return;
  }

  Status = gBS->LocateProtocol (&gEfiClockProtocolGuid, NULL, (VOID **)&ClockProtocol);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to locate CLOCK protocol\r\n"));
    return;
  }

  if (ClockProtocol) {
    for (i = 0; i < numOfCore; i++) {
      Status = ClockProtocol->GetCpuPerfLevel (ClockProtocol, i, &pnPerfLevel);
      if (Status != EFI_SUCCESS) {
        continue;
      }
      Status = ClockProtocol->GetCpuPerfLevelFrequency (ClockProtocol, i, pnPerfLevel, &pnFrequencyHz, &pnRequiredVoltage);
    }
  }
}

// Checks if A/B retry count update is required
BOOLEAN IsABRetryCountUpdateRequired (VOID) {
  BOOLEAN BatteryStatus;

  TargetPauseForBatteryCharge (&BatteryStatus);

  if ((BatteryStatus && IsChargingScreenEnable ()) || BootIntoFastboot || BootIntoRecovery) {
    return FALSE;
  }
  return TRUE;
}

// Determines the boot device type
UINT32 GetBootDeviceType () {
  UINTN  DataSize = sizeof (BootDeviceType);
  EFI_STATUS Status = EFI_SUCCESS;

  if (BootDeviceType == EFI_MAX_FLASH_TYPE) {
    Status = gRT->GetVariable (L"SharedImemBootCfgVal", &gQcomTokenSpaceGuid, NULL, &DataSize, &BootDeviceType);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "Failed to get boot device type, %r\n", Status));
    }
  }

  return BootDeviceType;
}

// Linux Loader Application EntryPoint
EFI_STATUS EFIAPI __attribute__ ((no_sanitize ("safe-stack"))) LinuxLoaderEntry (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
  EFI_STATUS Status;
  UINT32 Val = 0;
  UINT32 BootReason = NORMAL_MODE;
  UINT32 KeyPressed = SCAN_NULL;
  UINT32 PowerKeyPressTime = 0;
  CHAR8 SilentBootMode = NON_SILENT_MODE;
  BOOLEAN MultiSlotBoot = FALSE;
  BOOLEAN FlashlessBoot = FALSE;
  BOOLEAN NetworkBoot = FALSE;
  BOOLEAN SetRotAndBootState = FALSE;

  DEBUG ((EFI_D_INFO, "Loader Build Info: %a %a\n", __DATE__, __TIME__));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoader Load Address to debug ABL: 0x%llx\n", (UINTN)LinuxLoaderEntry & (~ (0xFFF))));
  DEBUG ((EFI_D_VERBOSE, "LinuxLoaderEntry Address: 0x%llx\n", (UINTN)LinuxLoaderEntry));

  BootStatsSetInitTimeStamp ();

  Status = InitThreadUnsafeStack ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Unable to Allocate memory for Unsafe Stack: %r\n", Status));
    goto stack_guard_update_default;
  }

  StackGuardChkSetup ();

  BootStatsSetTimeStamp (BS_BL_START);

  if (IsMultiBoot ()) {
    Val = GetBootDeviceType ();
    if (Val == EFI_EMMC_NETWORK_FLASH_TYPE) {
      NetworkBootImageAddr = BASE_ADDRESS;
      NetworkBoot = TRUE;
      goto flashless_boot;
    } else if (Val == EFI_PCIE_FLASH_TYPE) {
      FlashlessBootImageAddr = BASE_ADDRESS;
      FlashlessBoot = TRUE;
      goto flashless_boot;
    } else if (Val == 0) {
      DEBUG ((EFI_D_ERROR, "Failed to get boot device type\n"));
      goto stack_guard_update_default;
    }
  }

  Status = DeviceInfoInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Initialize the device info failed: %r\n", Status));
    goto stack_guard_update_default;
  }

  Status = EnumeratePartitions ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "LinuxLoader: Could not enumerate partitions: %r\n", Status));
    goto stack_guard_update_default;
  }

  UpdatePartitionEntries ();

  MultiSlotBoot = PartitionHasMultiSlot ((CONST CHAR16 *)L"boot");
  if (MultiSlotBoot) {
    DEBUG ((EFI_D_VERBOSE, "Multi Slot boot is supported\n"));
    FindPtnActiveSlot ();
  }

  if (IsPowerKeyMultiplex ()) {
    Status = GetPowerKeyPressInfo (&PowerKeyPressTime);
    if (Status == EFI_SUCCESS) {
      if (PowerKeyPressTime > 3800 && PowerKeyPressTime < 4200) {
        BootIntoFastboot = TRUE;
      }
    }
  }

  Status = GetKeyPress (&KeyPressed);
  if (Status == EFI_SUCCESS) {
    if (KeyPressed == SCAN_DOWN) BootIntoFastboot = TRUE;
    if (KeyPressed == SCAN_UP) BootIntoRecovery = TRUE;
    if (KeyPressed == SCAN_ESC) RebootDevice (EMERGENCY_DLOAD);
  } else if (Status == EFI_DEVICE_ERROR) {
    DEBUG ((EFI_D_ERROR, "Error reading key status: %r\n", Status));
    goto stack_guard_update_default;
  }

  SetDefaultAudioFw ();
  PrintCpuFrequency ();

  Status = GetRebootReason (&BootReason);
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Failed to get Reboot reason: %r\n", Status));
    goto stack_guard_update_default;
  }

  switch (BootReason) {
    case FASTBOOT_MODE:
      BootIntoFastboot = TRUE;
      break;
    case RECOVERY_MODE:
      BootIntoRecovery = TRUE;
      break;
    case ALARM_BOOT:
      BootReasonAlarm = TRUE;
      break;
    case DM_VERITY_ENFORCING:
      Status = EnableEnforcingMode (TRUE);
      if (Status != EFI_SUCCESS) goto stack_guard_update_default;
      break;
    case DM_VERITY_LOGGING:
      Status = MdtpDisable ();
      if (EFI_ERROR (Status) && Status != EFI_NOT_FOUND) {
        DEBUG ((EFI_D_ERROR, "MdtpDisable Returned error: %r\n", Status));
        goto stack_guard_update_default;
      }
      Status = EnableEnforcingMode (FALSE);
      if (Status != EFI_SUCCESS) goto stack_guard_update_default;
      break;
    case DM_VERITY_KEYSCLEAR:
      Status = ResetDeviceState ();
      if (Status != EFI_SUCCESS) {
        DEBUG ((EFI_D_ERROR, "VB Reset Device State error: %r\n", Status));
        goto stack_guard_update_default;
      }
      break;
    case SILENT_MODE:
      SilentBootMode = SILENT_MODE;
      break;
    case NON_SILENT_MODE:
      SilentBootMode = NON_SILENT_MODE;
      break;
    case FORCED_SILENT:
      SilentBootMode = FORCED_SILENT;
      break;
    case FORCED_NON_SILENT:
      SilentBootMode = FORCED_NON_SILENT;
      break;
    default:
      if (BootReason != NORMAL_MODE) {
        DEBUG ((EFI_D_ERROR, "Boot reason: 0x%x not handled, defaulting to Normal Boot\n", BootReason));
      }
      break;
  }

  Status = RecoveryInit (&BootIntoRecovery);
  if (Status != EFI_SUCCESS) DEBUG ((EFI_D_VERBOSE, "RecoveryInit failed ignore: %r\n", Status));

flashless_boot:
  Status = BoardInit ();
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Error finding board information: %r\n", Status));
    return Status;
  }

  DEBUG ((EFI_D_INFO, "KeyPress:%u, BootReason:%u\n", KeyPressed, BootReason));
  DEBUG ((EFI_D_INFO, "Fastboot=%d, Recovery:%d\n", BootIntoFastboot, BootIntoRecovery));
  DEBUG ((EFI_D_INFO, "SilentBoot Mode:%u\n", SilentBootMode));

  if (!GetVmData ()) {
    DEBUG ((EFI_D_ERROR, "VM Hyp calls not present\n"));
  }

  if (BootIntoFastboot) {
    goto fastboot;
  } else {
    BootInfo Info = {0};
    Info.MultiSlotBoot = MultiSlotBoot;
    Info.BootIntoRecovery = BootIntoRecovery;
    Info.BootReasonAlarm = BootReasonAlarm;
    Info.FlashlessBoot = FlashlessBoot;
    Info.NetworkBoot = NetworkBoot;
    Info.SilentBootMode = SilentBootMode;

    #if HIBERNATION_SUPPORT_NO_AES
    BootIntoHibernationImage (&Info, &SetRotAndBootState);
    #endif

    Status = LoadImageAndAuth (&Info, FALSE, SetRotAndBootState);
    if (Status != EFI_SUCCESS) {
      DEBUG ((EFI_D_ERROR, "LoadImageAndAuth failed: %r\n", Status));
      if (IsRecoveryInfo ()) {
        Slot CurrentSlot = GetCurrentSlotSuffix ();
        RI_HandleFailedSlot (CurrentSlot);
      }
      goto fastboot;
    }

    BootLinux (&Info);
  }

fastboot:
  if (FlashlessBoot || NetworkBoot) {
    DEBUG ((EFI_D_ERROR, "No fastboot support for flashless chipsets, Infinite loop\n"));
    while (1);
  }
  DEBUG ((EFI_D_INFO, "Launching fastboot\n"));
  Status = FastbootInitialize ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "Failed to Launch Fastboot App: %d\n", Status));
    goto stack_guard_update_default;
  }

stack_guard_update_default:
  __stack_chk_guard = DEFAULT_STACK_CHK_GUARD;
  DeInitThreadUnsafeStack ();
  return Status;
}
