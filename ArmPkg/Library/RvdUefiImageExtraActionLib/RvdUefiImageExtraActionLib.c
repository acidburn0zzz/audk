/**@file

Copyright (c) 2006 - 2009, Intel Corporation. All rights reserved.<BR>
Portions copyright (c) 2008 - 2010, Apple Inc. All rights reserved.<BR>
Portions copyright (c) 2011 - 2012, ARM Ltd. All rights reserved.<BR>

SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <PiDxe.h>
#include <Library/UefiImageLib.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiImageExtraActionLib.h>
#include <Library/SemihostLib.h>
#include <Library/PrintLib.h>

/**
  Append string to debugger script file, create file if needed.

  This library can show up in multiple places so we need to append the file every time we write to it.
  For example Sec can use this to load the DXE core, and the DXE core would use this to load all the
  other modules. So we have two instances of the library in the system.

  @param  Buffer  Buffer to write to file.
  @param  Length  Length of Buffer in bytes.
**/
VOID
WriteStringToFile (
  IN  VOID    *Buffer,
  IN  UINT32  Length
  )
{
  // Working around and issue with the code that is commented out. For now send it to the console.
  // You can copy the console into a file and source the file as a script and you get symbols.
  // This gets you all the symbols except for SEC. To get SEC symbols you need to copy the
  // debug print in the SEC into the debugger manually
  SemihostWriteString (Buffer);

  /*
    I'm currently having issues with this code crashing the debugger. Seems like it should work.

    UINT32        SemihostHandle;
    UINT32        SemihostMode = SEMIHOST_FILE_MODE_WRITE | SEMIHOST_FILE_MODE_BINARY | SEMIHOST_FILE_MODE_UPDATE;

    SemihostFileOpen ("c:\rvi_symbols.inc", SemihostMode, &SemihostHandle);
    SemihostFileWrite (SemihostHandle, &Length, Buffer);
    SemihostFileClose (SemihostHandle);
   */
}

/**
  If the build is done on cygwin the paths are cygpaths.
  /cygdrive/c/tmp.txt vs c:\tmp.txt so we need to convert
  them to work with RVD commands

  @param  Name  Path to convert if needed

**/
CHAR8 *
DeCygwinPathIfNeeded (
  IN  CHAR8  *Name
  )
{
  CHAR8  *Ptr;
  UINTN  Index;
  UINTN  Len;

  Ptr = AsciiStrStr (Name, "/cygdrive/");
  if (Ptr == NULL) {
    return Name;
  }

  Len = AsciiStrLen (Ptr);

  // convert "/cygdrive" to spaces
  for (Index = 0; Index < 9; Index++) {
    Ptr[Index] = ' ';
  }

  // convert /c to c:
  Ptr[9]  = Ptr[10];
  Ptr[10] = ':';

  // switch path separators
  for (Index = 11; Index < Len; Index++) {
    if (Ptr[Index] == '/') {
      Ptr[Index] = '\\';
    }
  }

  return Name;
}

/**
  Performs additional actions after a PE/COFF image has been loaded and relocated.

  If ImageContext is NULL, then ASSERT().

  @param  ImageContext  Pointer to the image context structure that describes the
                        PE/COFF image that has already been loaded and relocated.

**/
VOID
EFIAPI
UefiImageLoaderRelocateImageExtraAction (
  IN CONST UEFI_IMAGE_LOADER_IMAGE_CONTEXT  *ImageContext
  )
{
  RETURN_STATUS Status;
  CONST CHAR8   *PdbPath;
  UINT32        PdbPathSize;
  CHAR8         Buffer[256];

  Status = UefiImageGetSymbolsPath (ImageContext, &PdbPath, &PdbPathSize);
  if (RETURN_ERROR (Status)) {
    return;
  }

#if (__ARMCC_VERSION < 500000)
  AsciiSPrint (Buffer, sizeof (Buffer), "load /a /ni /np \"%a\" &0x%08x\n", PdbPath, UefiImageLoaderGetImageAddress (ImageContext));
 #else
  AsciiSPrint (Buffer, sizeof (Buffer), "add-symbol-file %a -o 0x%08x\n", PdbPath, UefiImageLoaderGetImageAddress (ImageContext));
 #endif
  DeCygwinPathIfNeeded (&Buffer[16]);

  WriteStringToFile (Buffer, AsciiStrSize (Buffer));
}

/**
  Performs additional actions just before a PE/COFF image is unloaded.  Any resources
  that were allocated by UefiImageLoaderRelocateImageExtraAction() must be freed.

  If ImageContext is NULL, then ASSERT().

  @param  ImageContext  Pointer to the image context structure that describes the
                        PE/COFF image that is being unloaded.

**/
VOID
EFIAPI
UefiImageLoaderUnloadImageExtraAction (
  IN OUT UEFI_IMAGE_LOADER_IMAGE_CONTEXT  *ImageContext
  )
{
  RETURN_STATUS Status;
  CONST CHAR8   *PdbPath;
  UINT32        PdbPathSize;
  CHAR8         Buffer[256];

  Status = UefiImageGetSymbolsPath (ImageContext, &PdbPath, &PdbPathSize);
  if (RETURN_ERROR (Status)) {
    return;
  }

  AsciiSPrint (Buffer, sizeof (Buffer), "unload symbols_only \"%a\"\n", PdbPath);
  DeCygwinPathIfNeeded (Buffer);

  WriteStringToFile (Buffer, AsciiStrSize (Buffer));
}
