/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#include "RecoveryInfo.h"
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Protocol/EFIRecoveryInfo.h>
#include <Library/UefiBootServicesTableLib.h>
#include "PartitionTableUpdate.h"
#include <VerifiedBoot.h>


STATIC INT64 HasRecoveryInfo = -1;

BOOLEAN IsRecoveryInfo ()
{
  EFI_STATUS Status = EFI_SUCCESS ;
  EFI_RECOVERYINFO_PROTOCOL *pRecoveryInfoProtocol = NULL;

  if (HasRecoveryInfo == -1 ) {
    Status = gBS->LocateProtocol (& gEfiRecoveryInfoProtocolGuid, NULL,
                                  (VOID **) & pRecoveryInfoProtocol);
    if (Status == EFI_SUCCESS) {
      HasRecoveryInfo = 1;
    } else {
      HasRecoveryInfo = 0;
    }
  }

  return (HasRecoveryInfo == 1);
}

EFI_STATUS RI_GetActiveSlot (Slot *ActiveSlot)
{
  EFI_STATUS Status = EFI_SUCCESS ;
  EFI_RECOVERYINFO_PROTOCOL *pRecoveryInfoProtocol = NULL;
  BootSetType BootSet;
  Slot Slots[] = {{L"_a"}, {L"_b"}};

  Status = gBS->LocateProtocol (& gEfiRecoveryInfoProtocolGuid, NULL,
                               (VOID **) & pRecoveryInfoProtocol);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

  Status = pRecoveryInfoProtocol -> GetBootSet (pRecoveryInfoProtocol,
                                                &BootSet);
  if (Status != EFI_SUCCESS ||
      BootSet == SET_INVALID) {
    DEBUG ((EFI_D_ERROR, "GetBootSet : Error returned %d", Status ));
    return Status;
  }

  if (BootSet >= (sizeof (Slots) / sizeof (Slot))) {
    DEBUG ((EFI_D_ERROR, "GetBootSet: Invalid BootSet chosen"));
    return EFI_UNSUPPORTED;
  }

  /* SET_A = 0 SET_B = 1 */
   GUARD (StrnCpyS (ActiveSlot->Suffix, ARRAY_SIZE (ActiveSlot->Suffix),
                    Slots[BootSet].Suffix,
                    StrLen (Slots[BootSet].Suffix)));
   return EFI_SUCCESS;

}

EFI_STATUS RI_HandleFailedSlot (Slot ActiveSlot)
{
  EFI_STATUS Status = EFI_SUCCESS ;
  EFI_RECOVERYINFO_PROTOCOL *pRecoveryInfoProtocol = NULL;
  BootSetType BootSet = SET_INVALID;

  Status = gBS->LocateProtocol (& gEfiRecoveryInfoProtocolGuid, NULL,
                                (VOID **) & pRecoveryInfoProtocol);
  if (Status != EFI_SUCCESS) {
    return Status;
  }

  if (!StrCmp (ActiveSlot.Suffix, (CONST CHAR16 *)L"_a")) {
    BootSet = SET_A;
  } else if (!StrCmp (ActiveSlot.Suffix, (CONST CHAR16 *)L"_b")) {
    BootSet = SET_B;
  }

  Status = pRecoveryInfoProtocol->HandleFailedSet (pRecoveryInfoProtocol,
                                                    BootSet);
  return EFI_UNSUPPORTED;
}

