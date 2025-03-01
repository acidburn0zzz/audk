/** @file
  CPU exception handler library implementation for SMM modules.

  Copyright (c) 2013 - 2022, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiSmm.h>
#include "CpuExceptionCommon.h"
#include <Library/SmmServicesTableLib.h>
#include <Guid/DebugImageInfoTable.h>

CONST UINTN  mDoFarReturnFlag = 1;

RESERVED_VECTORS_DATA      mReservedVectorsData[CPU_EXCEPTION_NUM];
EFI_CPU_INTERRUPT_HANDLER  mExternalInterruptHandlerTable[CPU_EXCEPTION_NUM];
EXCEPTION_HANDLER_DATA     mExceptionHandlerData = {
  CPU_EXCEPTION_NUM,
  0,                     // To be fixed
  mReservedVectorsData,
  mExternalInterruptHandlerTable
};

STATIC CONST EFI_DEBUG_IMAGE_INFO_TABLE_HEADER *mDebugImageInfoTableHeader = NULL;
/**
  Common exception handler.

  @param ExceptionType  Exception type.
  @param SystemContext  Pointer to EFI_SYSTEM_CONTEXT.
**/
VOID
EFIAPI
CommonExceptionHandler (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  CommonExceptionHandlerWorker (ExceptionType, SystemContext, &mExceptionHandlerData);
}

/**
  Initializes all CPU exceptions entries and provides the default exception handlers.

  Caller should try to get an array of interrupt and/or exception vectors that are in use and need to
  persist by EFI_VECTOR_HANDOFF_INFO defined in PI 1.3 specification.
  If caller cannot get reserved vector list or it does not exists, set VectorInfo to NULL.
  If VectorInfo is not NULL, the exception vectors will be initialized per vector attribute accordingly.

  @param[in]  VectorInfo    Pointer to reserved vector list.

  @retval EFI_SUCCESS           CPU Exception Entries have been successfully initialized
                                with default exception handlers.
  @retval EFI_INVALID_PARAMETER VectorInfo includes the invalid content if VectorInfo is not NULL.
  @retval EFI_UNSUPPORTED       This function is not supported.

**/
EFI_STATUS
EFIAPI
InitializeCpuExceptionHandlers (
  IN EFI_VECTOR_HANDOFF_INFO  *VectorInfo OPTIONAL
  )
{
  InitializeSpinLock (&mExceptionHandlerData.DisplayMessageSpinLock);
  return InitializeCpuExceptionHandlersWorker (VectorInfo, &mExceptionHandlerData);
}

/**
  Registers a function to be called from the processor interrupt handler.

  This function registers and enables the handler specified by InterruptHandler for a processor
  interrupt or exception type specified by InterruptType. If InterruptHandler is NULL, then the
  handler for the processor interrupt or exception type specified by InterruptType is uninstalled.
  The installed handler is called once for each processor interrupt or exception.
  NOTE: This function should be invoked after InitializeCpuExceptionHandlers() is invoked,
  otherwise EFI_UNSUPPORTED returned.

  @param[in]  InterruptType     Defines which interrupt or exception to hook.
  @param[in]  InterruptHandler  A pointer to a function of type EFI_CPU_INTERRUPT_HANDLER that is called
                                when a processor interrupt occurs. If this parameter is NULL, then the handler
                                will be uninstalled.

  @retval EFI_SUCCESS           The handler for the processor interrupt was successfully installed or uninstalled.
  @retval EFI_ALREADY_STARTED   InterruptHandler is not NULL, and a handler for InterruptType was
                                previously installed.
  @retval EFI_INVALID_PARAMETER InterruptHandler is NULL, and a handler for InterruptType was not
                                previously installed.
  @retval EFI_UNSUPPORTED       The interrupt specified by InterruptType is not supported,
                                or this function is not supported.
**/
EFI_STATUS
EFIAPI
RegisterCpuInterruptHandler (
  IN EFI_EXCEPTION_TYPE         InterruptType,
  IN EFI_CPU_INTERRUPT_HANDLER  InterruptHandler
  )
{
  return RegisterCpuInterruptHandlerWorker (InterruptType, InterruptHandler, &mExceptionHandlerData);
}

/**
  Setup separate stacks for certain exception handlers.
  If the input Buffer and BufferSize are both NULL, use global variable if possible.

  @param[in]       Buffer        Point to buffer used to separate exception stack.
  @param[in, out]  BufferSize    On input, it indicates the byte size of Buffer.
                                 If the size is not enough, the return status will
                                 be EFI_BUFFER_TOO_SMALL, and output BufferSize
                                 will be the size it needs.

  @retval EFI_SUCCESS             The stacks are assigned successfully.
  @retval EFI_UNSUPPORTED         This function is not supported.
  @retval EFI_BUFFER_TOO_SMALL    This BufferSize is too small.
**/
EFI_STATUS
EFIAPI
InitializeSeparateExceptionStacks (
  IN     VOID   *Buffer,
  IN OUT UINTN  *BufferSize
  )
{
  return EFI_UNSUPPORTED;
}

// FIXME: Lib?
STATIC
EFI_STATUS
EFIAPI
InternalGetSystemConfigurationTable (
  IN  EFI_GUID  *TableGuid,
  OUT VOID      **Table
  )
{
  UINTN             Index;

  ASSERT (TableGuid != NULL);
  ASSERT (Table != NULL);

  *Table = NULL;
  for (Index = 0; Index < gSmst->NumberOfTableEntries; Index++) {
    if (CompareGuid (TableGuid, &(gSmst->SmmConfigurationTable[Index].VendorGuid))) {
      *Table = gSmst->SmmConfigurationTable[Index].VendorTable;
      return EFI_SUCCESS;
    }
  }

  return EFI_NOT_FOUND;
}

BOOLEAN
GetImageInfoByIp (
  OUT UINTN        *ImageBase,
  OUT CONST CHAR8  **SymbolsPath,
  IN  UINTN        CurrentEip
  )
{
  EFI_STATUS                        Status;
  UINT32                            Index;
  CONST EFI_DEBUG_IMAGE_INFO_NORMAL *NormalImage;

  if (mDebugImageInfoTableHeader == NULL) {
    Status = InternalGetSystemConfigurationTable (
               &gEfiDebugImageInfoTableGuid,
               (VOID **) &mDebugImageInfoTableHeader
               );
    if (EFI_ERROR (Status)) {
      mDebugImageInfoTableHeader = NULL;
      return FALSE;
    }
  }

  ASSERT (mDebugImageInfoTableHeader != NULL);

  for (Index = 0; Index < mDebugImageInfoTableHeader->TableSize; ++Index) {
    if (mDebugImageInfoTableHeader->EfiDebugImageInfoTable[Index].ImageInfoType == NULL) {
      continue;
    }

    if (*mDebugImageInfoTableHeader->EfiDebugImageInfoTable[Index].ImageInfoType != EFI_DEBUG_IMAGE_INFO_TYPE_NORMAL) {
      continue;
    }

    NormalImage = mDebugImageInfoTableHeader->EfiDebugImageInfoTable[Index].NormalImage;

    ASSERT (NormalImage->LoadedImageProtocolInstance != NULL);

    if (CurrentEip >= (UINTN) NormalImage->LoadedImageProtocolInstance->ImageBase &&
        CurrentEip < (UINTN) NormalImage->LoadedImageProtocolInstance->ImageBase + NormalImage->LoadedImageProtocolInstance->ImageSize) {
      *ImageBase   = (UINTN) NormalImage->LoadedImageProtocolInstance->ImageBase;
      *SymbolsPath = NormalImage->PdbPath;
      return TRUE;
    }
  }

  return FALSE;
}
