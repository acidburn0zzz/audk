/** @file
  Implement image verification services for secure boot service

  Caution: This file requires additional review when modified.
  This library will have external input - PE/COFF image.
  This external input must be validated carefully to avoid security issue like
  buffer overflow, integer overflow.

  DxeImageVerificationLibImageRead() function will make sure the PE/COFF image content
  read is within the image buffer.

  DxeImageVerificationHandler(), HashPeImageByType(), HashPeImage() function will accept
  untrusted PE/COFF image and validate its data structure within this image buffer before use.

Copyright (c) 2009 - 2018, Intel Corporation. All rights reserved.<BR>
(C) Copyright 2016 Hewlett Packard Enterprise Development LP<BR>
SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include "DxeImageVerificationLib.h"
#include "Library/BaseCryptLib.h"
#include "Library/UefiImageLib.h"

//
// Notify string for authorization UI.
//
CHAR16  mNotifyString1[MAX_NOTIFY_STRING_LEN] = L"Image verification pass but not found in authorized database!";
CHAR16  mNotifyString2[MAX_NOTIFY_STRING_LEN] = L"Launch this image anyway? (Yes/Defer/No)";
//
// Public Exponent of RSA Key.
//
CONST UINT8  mRsaE[] = { 0x01, 0x00, 0x01 };

//
// OID ASN.1 Value for Hash Algorithms
//
UINT8  mHashOidValue[] = {
  0x2B, 0x0E, 0x03, 0x02, 0x1A,                         // OBJ_sha1
  0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x04, // OBJ_sha224
  0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, // OBJ_sha256
  0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, // OBJ_sha384
  0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, // OBJ_sha512
};

//
//  Support hash types
//
#define HASHALG_SHA256                         0x00000000
#define HASHALG_SHA384                         0x00000001
#define HASHALG_SHA512                         0x00000002
#define HASHALG_SHA1                           0x00000003
#define HASHALG_MAX                            0x00000004

HASH_TABLE mHash[] = {
  { L"SHA256", 32, &mHashOidValue[14], 9, &gEfiCertSha256Guid, Sha256GetContextSize, Sha256Init, Sha256Update, Sha256Final },
  { L"SHA384", 48, &mHashOidValue[23], 9, &gEfiCertSha384Guid, Sha384GetContextSize, Sha384Init, Sha384Update, Sha384Final },
  { L"SHA512", 64, &mHashOidValue[32], 9, &gEfiCertSha512Guid, Sha512GetContextSize, Sha512Init, Sha512Update, Sha512Final },
#ifndef DISABLE_SHA1_DEPRECATED_INTERFACES
  { L"SHA1",   20, &mHashOidValue[0],  5, &gEfiCertSha1Guid, Sha1GetContextSize,   Sha1Init,   Sha1Update,   Sha1Final  },
#endif
};

/**
  SecureBoot Hook for processing image verification.

  @param[in] VariableName                 Name of Variable to be found.
  @param[in] VendorGuid                   Variable vendor GUID.
  @param[in] DataSize                     Size of Data found. If size is less than the
                                          data, this value contains the required size.
  @param[in] Data                         Data pointer.

**/
VOID
EFIAPI
SecureBootHook (
  IN CHAR16    *VariableName,
  IN EFI_GUID  *VendorGuid,
  IN UINTN     DataSize,
  IN VOID      *Data
  );

/**
  Get the image type.

  @param[in]    File       This is a pointer to the device path of the file that is
                           being dispatched.

  @return UINT32           Image Type

**/
UINT32
GetImageType (
  IN  CONST EFI_DEVICE_PATH_PROTOCOL  *File
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                DeviceHandle;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;

  if (File == NULL) {
    return IMAGE_UNKNOWN;
  }

  //
  // First check to see if File is from a Firmware Volume
  //
  DeviceHandle   = NULL;
  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)File;
  Status         = gBS->LocateDevicePath (
                          &gEfiFirmwareVolume2ProtocolGuid,
                          &TempDevicePath,
                          &DeviceHandle
                          );
  if (!EFI_ERROR (Status)) {
    Status = gBS->OpenProtocol (
                    DeviceHandle,
                    &gEfiFirmwareVolume2ProtocolGuid,
                    NULL,
                    NULL,
                    NULL,
                    EFI_OPEN_PROTOCOL_TEST_PROTOCOL
                    );
    if (!EFI_ERROR (Status)) {
      return IMAGE_FROM_FV;
    }
  }

  //
  // Next check to see if File is from a Block I/O device
  //
  DeviceHandle   = NULL;
  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)File;
  Status         = gBS->LocateDevicePath (
                          &gEfiBlockIoProtocolGuid,
                          &TempDevicePath,
                          &DeviceHandle
                          );
  if (!EFI_ERROR (Status)) {
    BlockIo = NULL;
    Status  = gBS->OpenProtocol (
                     DeviceHandle,
                     &gEfiBlockIoProtocolGuid,
                     (VOID **)&BlockIo,
                     NULL,
                     NULL,
                     EFI_OPEN_PROTOCOL_GET_PROTOCOL
                     );
    if (!EFI_ERROR (Status) && (BlockIo != NULL)) {
      if (BlockIo->Media != NULL) {
        if (BlockIo->Media->RemovableMedia) {
          //
          // Block I/O is present and specifies the media is removable
          //
          return IMAGE_FROM_REMOVABLE_MEDIA;
        } else {
          //
          // Block I/O is present and specifies the media is not removable
          //
          return IMAGE_FROM_FIXED_MEDIA;
        }
      }
    }
  }

  //
  // File is not in a Firmware Volume or on a Block I/O device, so check to see if
  // the device path supports the Simple File System Protocol.
  //
  DeviceHandle   = NULL;
  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)File;
  Status         = gBS->LocateDevicePath (
                          &gEfiSimpleFileSystemProtocolGuid,
                          &TempDevicePath,
                          &DeviceHandle
                          );
  if (!EFI_ERROR (Status)) {
    //
    // Simple File System is present without Block I/O, so assume media is fixed.
    //
    return IMAGE_FROM_FIXED_MEDIA;
  }

  //
  // File is not from an FV, Block I/O or Simple File System, so the only options
  // left are a PCI Option ROM and a Load File Protocol such as a PXE Boot from a NIC.
  //
  TempDevicePath = (EFI_DEVICE_PATH_PROTOCOL *)File;
  while (!IsDevicePathEndType (TempDevicePath)) {
    switch (DevicePathType (TempDevicePath)) {
      case MEDIA_DEVICE_PATH:
        if (DevicePathSubType (TempDevicePath) == MEDIA_RELATIVE_OFFSET_RANGE_DP) {
          return IMAGE_FROM_OPTION_ROM;
        }

        break;

      case MESSAGING_DEVICE_PATH:
        if (DevicePathSubType (TempDevicePath) == MSG_MAC_ADDR_DP) {
          return IMAGE_FROM_REMOVABLE_MEDIA;
        }

        break;

      default:
        break;
    }

    TempDevicePath = NextDevicePathNode (TempDevicePath);
  }

  return IMAGE_UNKNOWN;
}

/**
  Calculate hash of Pe/Coff image based on the authenticode image hashing in
  PE/COFF Specification 8.0 Appendix A

  Caution: This function may receive untrusted input.
  PE/COFF image is external input, so this function will validate its data structure
  within this image buffer before use.

  Notes: PE/COFF image has been checked by UefiImageLibLib UefiImageInitializeContext() in
  its caller function DxeImageVerificationHandler().

  @param[in]    HashAlg   Hash algorithm type.

  @retval TRUE            Successfully hash image.
  @retval FALSE           Fail in hash image.

**/
BOOLEAN
HashPeImage (
  UEFI_IMAGE_LOADER_IMAGE_CONTEXT  *ImageContext,
  IN  HASH_TABLE                   *HashAlg,
  UINT8                         ImageDigest[MAX_DIGEST_SIZE],
  UINTN                         *ImageDigestSize
  )
{
  BOOLEAN                   Status;
  VOID                      *HashCtx;
  UINTN                     CtxSize;

  HashCtx       = NULL;
  Status        = FALSE;

  //
  // Initialize context of hash.
  //
  ZeroMem (ImageDigest, MAX_DIGEST_SIZE);

  CtxSize      = HashAlg->GetContextSize ();

  HashCtx = AllocatePool (CtxSize);
  if (HashCtx == NULL) {
    return FALSE;
  }

  Status = HashAlg->HashInit (HashCtx);

  if (!Status) {
    goto Done;
  }

  Status = UefiImageHashImageDefault (ImageContext, HashCtx, HashAlg->HashUpdate);

  if (!Status) {
    DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Failed to hash this image using %s.\n", HashAlg->Name));
    goto Done;
  }

  ASSERT (HashAlg->DigestLength <= MAX_DIGEST_SIZE);
  Status  = HashAlg->HashFinal(HashCtx, ImageDigest);

  *ImageDigestSize = HashAlg->DigestLength;

Done:
  if (HashCtx != NULL) {
    FreePool (HashCtx);
  }

  return Status;
}

/**
  Recognize the Hash algorithm in PE/COFF Authenticode and calculate hash of
  Pe/Coff image based on the authenticode image hashing in PE/COFF Specification
  8.0 Appendix A

  Caution: This function may receive untrusted input.
  PE/COFF image is external input, so this function will validate its data structure
  within this image buffer before use.

  @param[in]  AuthData            Pointer to the Authenticode Signature retrieved from signed image.
  @param[in]  AuthDataSize        Size of the Authenticode Signature in bytes.

  @retval EFI_UNSUPPORTED             Hash algorithm is not supported.
  @retval EFI_SUCCESS                 Hash successfully.

**/
EFI_STATUS
HashPeImageByType (
  UEFI_IMAGE_LOADER_IMAGE_CONTEXT  *ImageContext,
  IN CONST UINT8                *AuthData,
  IN UINTN                      AuthDataSize,
  UINT8                         ImageDigest[MAX_DIGEST_SIZE],
  UINTN                         *ImageDigestSize,
  CONST EFI_GUID                **CertType
  )
{
  UINT8  Index;

  //
  // Check the Hash algorithm in PE/COFF Authenticode.
  //    According to PKCS#7 Definition:
  //        SignedData ::= SEQUENCE {
  //            version Version,
  //            digestAlgorithms DigestAlgorithmIdentifiers,
  //            contentInfo ContentInfo,
  //            .... }
  //    The DigestAlgorithmIdentifiers can be used to determine the hash algorithm in PE/COFF hashing
  //    This field has the fixed offset (+32) in final Authenticode ASN.1 data.
  //    Fixed offset (+32) is calculated based on two bytes of length encoding.
  //
  if ((*(AuthData + 1) & TWO_BYTE_ENCODE) != TWO_BYTE_ENCODE) {
    //
    // Only support two bytes of Long Form of Length Encoding.
    //
    return EFI_UNSUPPORTED;
  }

  for (Index = 0; Index < HASHALG_MAX; Index++) {
    if (AuthDataSize < 32 + mHash[Index].OidLength) {
      continue;
    }

    if (CompareMem (AuthData + 32, mHash[Index].OidValue, mHash[Index].OidLength) == 0) {
      break;
    }
  }

  if (Index == HASHALG_MAX) {
    return EFI_UNSUPPORTED;
  }

  //
  // HASH PE Image based on Hash algorithm in PE/COFF Authenticode.
  //
  if (!HashPeImage (ImageContext, &mHash[Index], ImageDigest, ImageDigestSize)) {
    return EFI_UNSUPPORTED;
  }

  *CertType = mHash[Index].CertType;

  return EFI_SUCCESS;
}

/**
  Returns the size of a given image execution info table in bytes.

  This function returns the size, in bytes, of the image execution info table specified by
  ImageExeInfoTable. If ImageExeInfoTable is NULL, then 0 is returned.

  @param  ImageExeInfoTable          A pointer to a image execution info table structure.

  @retval 0       If ImageExeInfoTable is NULL.
  @retval Others  The size of a image execution info table in bytes.

**/
UINTN
GetImageExeInfoTableSize (
  EFI_IMAGE_EXECUTION_INFO_TABLE  *ImageExeInfoTable
  )
{
  UINTN                     Index;
  EFI_IMAGE_EXECUTION_INFO  *ImageExeInfoItem;
  UINTN                     TotalSize;

  if (ImageExeInfoTable == NULL) {
    return 0;
  }

  ImageExeInfoItem = (EFI_IMAGE_EXECUTION_INFO *)((UINT8 *)ImageExeInfoTable + sizeof (EFI_IMAGE_EXECUTION_INFO_TABLE));
  TotalSize        = sizeof (EFI_IMAGE_EXECUTION_INFO_TABLE);
  for (Index = 0; Index < ImageExeInfoTable->NumberOfImages; Index++) {
    TotalSize       += ReadUnaligned32 ((UINT32 *)&ImageExeInfoItem->InfoSize);
    ImageExeInfoItem = (EFI_IMAGE_EXECUTION_INFO *)((UINT8 *)ImageExeInfoItem + ReadUnaligned32 ((UINT32 *)&ImageExeInfoItem->InfoSize));
  }

  return TotalSize;
}

/**
  Create an Image Execution Information Table entry and add it to system configuration table.

  @param[in]  Action          Describes the action taken by the firmware regarding this image.
  @param[in]  Name            Input a null-terminated, user-friendly name.
  @param[in]  DevicePath      Input device path pointer.
  @param[in]  Signature       Input signature info in EFI_SIGNATURE_LIST data structure.
  @param[in]  SignatureSize   Size of signature. Must be zero if Signature is NULL.

**/
VOID
AddImageExeInfo (
  IN       EFI_IMAGE_EXECUTION_ACTION  Action,
  IN       CHAR16                      *Name OPTIONAL,
  IN CONST EFI_DEVICE_PATH_PROTOCOL    *DevicePath,
  IN       EFI_SIGNATURE_LIST          *Signature OPTIONAL,
  IN       UINTN                       SignatureSize
  )
{
  EFI_IMAGE_EXECUTION_INFO_TABLE  *ImageExeInfoTable;
  EFI_IMAGE_EXECUTION_INFO_TABLE  *NewImageExeInfoTable;
  EFI_IMAGE_EXECUTION_INFO        *ImageExeInfoEntry;
  UINTN                           ImageExeInfoTableSize;
  UINTN                           NewImageExeInfoEntrySize;
  UINTN                           NameStringLen;
  UINTN                           DevicePathSize;
  CHAR16                          *NameStr;

  ImageExeInfoTable    = NULL;
  NewImageExeInfoTable = NULL;
  ImageExeInfoEntry    = NULL;
  NameStringLen        = 0;
  NameStr              = NULL;

  if (DevicePath == NULL) {
    return;
  }

  if (Name != NULL) {
    NameStringLen = StrSize (Name);
  } else {
    NameStringLen = sizeof (CHAR16);
  }

  EfiGetSystemConfigurationTable (&gEfiImageSecurityDatabaseGuid, (VOID **)&ImageExeInfoTable);
  if (ImageExeInfoTable != NULL) {
    //
    // The table has been found!
    // We must enlarge the table to accommodate the new exe info entry.
    //
    ImageExeInfoTableSize = GetImageExeInfoTableSize (ImageExeInfoTable);
  } else {
    //
    // Not Found!
    // We should create a new table to append to the configuration table.
    //
    ImageExeInfoTableSize = sizeof (EFI_IMAGE_EXECUTION_INFO_TABLE);
  }

  DevicePathSize = GetDevicePathSize (DevicePath);

  //
  // Signature size can be odd. Pad after signature to ensure next EXECUTION_INFO entry align
  //
  ASSERT (Signature != NULL || SignatureSize == 0);
  NewImageExeInfoEntrySize = sizeof (EFI_IMAGE_EXECUTION_INFO) + NameStringLen + DevicePathSize + SignatureSize;

  NewImageExeInfoTable = (EFI_IMAGE_EXECUTION_INFO_TABLE *)AllocateRuntimePool (ImageExeInfoTableSize + NewImageExeInfoEntrySize);
  if (NewImageExeInfoTable == NULL) {
    return;
  }

  if (ImageExeInfoTable != NULL) {
    CopyMem (NewImageExeInfoTable, ImageExeInfoTable, ImageExeInfoTableSize);
  } else {
    NewImageExeInfoTable->NumberOfImages = 0;
  }

  NewImageExeInfoTable->NumberOfImages++;
  ImageExeInfoEntry = (EFI_IMAGE_EXECUTION_INFO *)((UINT8 *)NewImageExeInfoTable + ImageExeInfoTableSize);
  //
  // Update new item's information.
  //
  WriteUnaligned32 ((UINT32 *)ImageExeInfoEntry, Action);
  WriteUnaligned32 ((UINT32 *)((UINT8 *)ImageExeInfoEntry + sizeof (EFI_IMAGE_EXECUTION_ACTION)), (UINT32)NewImageExeInfoEntrySize);

  NameStr = (CHAR16 *)(ImageExeInfoEntry + 1);
  if (Name != NULL) {
    CopyMem ((UINT8 *)NameStr, Name, NameStringLen);
  } else {
    ZeroMem ((UINT8 *)NameStr, sizeof (CHAR16));
  }

  CopyMem (
    (UINT8 *)NameStr + NameStringLen,
    DevicePath,
    DevicePathSize
    );
  if (Signature != NULL) {
    CopyMem (
      (UINT8 *)NameStr + NameStringLen + DevicePathSize,
      Signature,
      SignatureSize
      );
  }

  //
  // Update/replace the image execution table.
  //
  gBS->InstallConfigurationTable (&gEfiImageSecurityDatabaseGuid, (VOID *)NewImageExeInfoTable);

  //
  // Free Old table data!
  //
  if (ImageExeInfoTable != NULL) {
    FreePool (ImageExeInfoTable);
  }
}

/**
  Check whether the hash of an given X.509 certificate is in forbidden database (DBX).

  @param[in]  Certificate       Pointer to X.509 Certificate that is searched for.
  @param[in]  CertSize          Size of X.509 Certificate.
  @param[in]  SignatureList     Pointer to the Signature List in forbidden database.
  @param[in]  SignatureListSize Size of Signature List.
  @param[out] RevocationTime    Return the time that the certificate was revoked.
  @param[out] IsFound           Search result. Only valid if EFI_SUCCESS returned.

  @retval EFI_SUCCESS           Finished the search without any error.
  @retval Others                Error occurred in the search of database.

**/
EFI_STATUS
IsCertHashFoundInDbx (
  IN  UINT8               *Certificate,
  IN  UINTN               CertSize,
  IN  EFI_SIGNATURE_LIST  *SignatureList,
  IN  UINTN               SignatureListSize,
  OUT EFI_TIME            *RevocationTime,
  OUT BOOLEAN             *IsFound
  )
{
  EFI_STATUS          Status;
  EFI_SIGNATURE_LIST  *DbxList;
  UINTN               DbxSize;
  EFI_SIGNATURE_DATA  *CertHash;
  UINTN               CertHashCount;
  UINTN               Index;
  UINT32              HashAlg;
  VOID                *HashCtx;
  UINT8               CertDigest[MAX_DIGEST_SIZE];
  UINT8               *DbxCertHash;
  UINTN               SiglistHeaderSize;
  UINT8               *TBSCert;
  UINTN               TBSCertSize;

  Status   = EFI_ABORTED;
  *IsFound = FALSE;
  DbxList  = SignatureList;
  DbxSize  = SignatureListSize;
  HashCtx  = NULL;
  HashAlg  = HASHALG_MAX;

  if ((RevocationTime == NULL) || (DbxList == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Retrieve the TBSCertificate from the X.509 Certificate.
  //
  if (!X509GetTBSCert (Certificate, CertSize, &TBSCert, &TBSCertSize)) {
    return Status;
  }

  while ((DbxSize > 0) && (SignatureListSize >= DbxList->SignatureListSize)) {
    //
    // Determine Hash Algorithm of Certificate in the forbidden database.
    //
    if (CompareGuid (&DbxList->SignatureType, &gEfiCertX509Sha256Guid)) {
      HashAlg = HASHALG_SHA256;
    } else if (CompareGuid (&DbxList->SignatureType, &gEfiCertX509Sha384Guid)) {
      HashAlg = HASHALG_SHA384;
    } else if (CompareGuid (&DbxList->SignatureType, &gEfiCertX509Sha512Guid)) {
      HashAlg = HASHALG_SHA512;
    } else {
      DbxSize -= DbxList->SignatureListSize;
      DbxList  = (EFI_SIGNATURE_LIST *)((UINT8 *)DbxList + DbxList->SignatureListSize);
      continue;
    }

    //
    // Calculate the hash value of current TBSCertificate for comparision.
    //
    if (mHash[HashAlg].GetContextSize == NULL) {
      goto Done;
    }

    ZeroMem (CertDigest, MAX_DIGEST_SIZE);
    HashCtx = AllocatePool (mHash[HashAlg].GetContextSize ());
    if (HashCtx == NULL) {
      goto Done;
    }

    if (!mHash[HashAlg].HashInit (HashCtx)) {
      goto Done;
    }

    if (!mHash[HashAlg].HashUpdate (HashCtx, TBSCert, TBSCertSize)) {
      goto Done;
    }

    if (!mHash[HashAlg].HashFinal (HashCtx, CertDigest)) {
      goto Done;
    }

    FreePool (HashCtx);
    HashCtx = NULL;

    SiglistHeaderSize = sizeof (EFI_SIGNATURE_LIST) + DbxList->SignatureHeaderSize;
    CertHash          = (EFI_SIGNATURE_DATA *)((UINT8 *)DbxList + SiglistHeaderSize);
    CertHashCount     = (DbxList->SignatureListSize - SiglistHeaderSize) / DbxList->SignatureSize;
    for (Index = 0; Index < CertHashCount; Index++) {
      //
      // Iterate each Signature Data Node within this CertList for verify.
      //
      DbxCertHash = CertHash->SignatureData;
      if (CompareMem (DbxCertHash, CertDigest, mHash[HashAlg].DigestLength) == 0) {
        //
        // Hash of Certificate is found in forbidden database.
        //
        Status   = EFI_SUCCESS;
        *IsFound = TRUE;

        //
        // Return the revocation time.
        //
        CopyMem (RevocationTime, (EFI_TIME *)(DbxCertHash + mHash[HashAlg].DigestLength), sizeof (EFI_TIME));
        goto Done;
      }

      CertHash = (EFI_SIGNATURE_DATA *)((UINT8 *)CertHash + DbxList->SignatureSize);
    }

    DbxSize -= DbxList->SignatureListSize;
    DbxList  = (EFI_SIGNATURE_LIST *)((UINT8 *)DbxList + DbxList->SignatureListSize);
  }

  Status = EFI_SUCCESS;

Done:
  if (HashCtx != NULL) {
    FreePool (HashCtx);
  }

  return Status;
}

/**
  Check whether signature is in specified database.

  @param[in]  VariableName        Name of database variable that is searched in.
  @param[in]  Signature           Pointer to signature that is searched for.
  @param[in]  CertType            Pointer to hash algorithm.
  @param[in]  SignatureSize       Size of Signature.
  @param[out] IsFound             Search result. Only valid if EFI_SUCCESS returned

  @retval EFI_SUCCESS             Finished the search without any error.
  @retval Others                  Error occurred in the search of database.

**/
EFI_STATUS
IsSignatureFoundInDatabase (
  IN  CHAR16    *VariableName,
  IN  UINT8     *Signature,
  IN  CONST EFI_GUID    *CertType,
  IN  UINTN     SignatureSize,
  OUT BOOLEAN   *IsFound
  )
{
  EFI_STATUS          Status;
  EFI_SIGNATURE_LIST  *CertList;
  EFI_SIGNATURE_DATA  *Cert;
  UINTN               DataSize;
  UINT8               *Data;
  UINTN               Index;
  UINTN               CertCount;

  //
  // Read signature database variable.
  //
  *IsFound = FALSE;
  Data     = NULL;
  DataSize = 0;
  Status   = gRT->GetVariable (VariableName, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    if (Status == EFI_NOT_FOUND) {
      //
      // No database, no need to search.
      //
      Status = EFI_SUCCESS;
    }

    return Status;
  }

  Data = (UINT8 *)AllocateZeroPool (DataSize);
  if (Data == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Status = gRT->GetVariable (VariableName, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, Data);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  //
  // Enumerate all signature data in SigDB to check if signature exists for executable.
  //
  CertList = (EFI_SIGNATURE_LIST *)Data;
  while ((DataSize > 0) && (DataSize >= CertList->SignatureListSize)) {
    CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;
    Cert      = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList + sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
    if ((CertList->SignatureSize == sizeof (EFI_SIGNATURE_DATA) - 1 + SignatureSize) && (CompareGuid (&CertList->SignatureType, CertType))) {
      for (Index = 0; Index < CertCount; Index++) {
        if (CompareMem (Cert->SignatureData, Signature, SignatureSize) == 0) {
          //
          // Find the signature in database.
          //
          *IsFound = TRUE;
          //
          // Entries in UEFI_IMAGE_SECURITY_DATABASE that are used to validate image should be measured
          //
          if (StrCmp (VariableName, EFI_IMAGE_SECURITY_DATABASE) == 0) {
            SecureBootHook (VariableName, &gEfiImageSecurityDatabaseGuid, CertList->SignatureSize, Cert);
          }

          break;
        }

        Cert = (EFI_SIGNATURE_DATA *)((UINT8 *)Cert + CertList->SignatureSize);
      }

      if (*IsFound) {
        break;
      }
    }

    DataSize -= CertList->SignatureListSize;
    CertList  = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

Done:
  if (Data != NULL) {
    FreePool (Data);
  }

  return Status;
}

/**
  Check whether the timestamp is valid by comparing the signing time and the revocation time.

  @param SigningTime         A pointer to the signing time.
  @param RevocationTime      A pointer to the revocation time.

  @retval  TRUE              The SigningTime is not later than the RevocationTime.
  @retval  FALSE             The SigningTime is later than the RevocationTime.

**/
BOOLEAN
IsValidSignatureByTimestamp (
  IN EFI_TIME  *SigningTime,
  IN EFI_TIME  *RevocationTime
  )
{
  if (SigningTime->Year != RevocationTime->Year) {
    return (BOOLEAN)(SigningTime->Year < RevocationTime->Year);
  } else if (SigningTime->Month != RevocationTime->Month) {
    return (BOOLEAN)(SigningTime->Month < RevocationTime->Month);
  } else if (SigningTime->Day != RevocationTime->Day) {
    return (BOOLEAN)(SigningTime->Day < RevocationTime->Day);
  } else if (SigningTime->Hour != RevocationTime->Hour) {
    return (BOOLEAN)(SigningTime->Hour < RevocationTime->Hour);
  } else if (SigningTime->Minute != RevocationTime->Minute) {
    return (BOOLEAN)(SigningTime->Minute < RevocationTime->Minute);
  }

  return (BOOLEAN)(SigningTime->Second <= RevocationTime->Second);
}

/**
  Check if the given time value is zero.

  @param[in]  Time      Pointer of a time value.

  @retval     TRUE      The Time is Zero.
  @retval     FALSE     The Time is not Zero.

**/
BOOLEAN
IsTimeZero (
  IN EFI_TIME  *Time
  )
{
  if ((Time->Year == 0) && (Time->Month == 0) &&  (Time->Day == 0) &&
      (Time->Hour == 0) && (Time->Minute == 0) && (Time->Second == 0))
  {
    return TRUE;
  }

  return FALSE;
}

/**
  Check whether the timestamp signature is valid and the signing time is also earlier than
  the revocation time.

  @param[in]  AuthData        Pointer to the Authenticode signature retrieved from signed image.
  @param[in]  AuthDataSize    Size of the Authenticode signature in bytes.
  @param[in]  RevocationTime  The time that the certificate was revoked.

  @retval TRUE      Timestamp signature is valid and signing time is no later than the
                    revocation time.
  @retval FALSE     Timestamp signature is not valid or the signing time is later than the
                    revocation time.

**/
BOOLEAN
PassTimestampCheck (
  IN CONST UINT8            *AuthData,
  IN UINTN     AuthDataSize,
  IN EFI_TIME  *RevocationTime
  )
{
  EFI_STATUS          Status;
  BOOLEAN             VerifyStatus;
  EFI_SIGNATURE_LIST  *CertList;
  EFI_SIGNATURE_DATA  *Cert;
  UINT8               *DbtData;
  UINTN               DbtDataSize;
  UINT8               *RootCert;
  UINTN               RootCertSize;
  UINTN               Index;
  UINTN               CertCount;
  EFI_TIME            SigningTime;

  //
  // Variable Initialization
  //
  VerifyStatus = FALSE;
  DbtData      = NULL;
  CertList     = NULL;
  Cert         = NULL;
  RootCert     = NULL;
  RootCertSize = 0;

  //
  // If RevocationTime is zero, the certificate shall be considered to always be revoked.
  //
  if (IsTimeZero (RevocationTime)) {
    return FALSE;
  }

  //
  // RevocationTime is non-zero, the certificate should be considered to be revoked from that time and onwards.
  // Using the dbt to get the trusted TSA certificates.
  //
  DbtDataSize = 0;
  Status      = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE2, &gEfiImageSecurityDatabaseGuid, NULL, &DbtDataSize, NULL);
  if (Status != EFI_BUFFER_TOO_SMALL) {
    goto Done;
  }

  DbtData = (UINT8 *)AllocateZeroPool (DbtDataSize);
  if (DbtData == NULL) {
    goto Done;
  }

  Status = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE2, &gEfiImageSecurityDatabaseGuid, NULL, &DbtDataSize, (VOID *)DbtData);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  CertList = (EFI_SIGNATURE_LIST *)DbtData;
  while ((DbtDataSize > 0) && (DbtDataSize >= CertList->SignatureListSize)) {
    if (CompareGuid (&CertList->SignatureType, &gEfiCertX509Guid)) {
      Cert      = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList + sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
      CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;
      for (Index = 0; Index < CertCount; Index++) {
        //
        // Iterate each Signature Data Node within this CertList for verify.
        //
        RootCert     = Cert->SignatureData;
        RootCertSize = CertList->SignatureSize - sizeof (EFI_GUID);
        //
        // Get the signing time if the timestamp signature is valid.
        //
        if (ImageTimestampVerify (AuthData, AuthDataSize, RootCert, RootCertSize, &SigningTime)) {
          //
          // The signer signature is valid only when the signing time is earlier than revocation time.
          //
          if (IsValidSignatureByTimestamp (&SigningTime, RevocationTime)) {
            VerifyStatus = TRUE;
            goto Done;
          }
        }

        Cert = (EFI_SIGNATURE_DATA *)((UINT8 *)Cert + CertList->SignatureSize);
      }
    }

    DbtDataSize -= CertList->SignatureListSize;
    CertList     = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

Done:
  if (DbtData != NULL) {
    FreePool (DbtData);
  }

  return VerifyStatus;
}

/**
  Check whether the image signature is forbidden by the forbidden database (dbx).
  The image is forbidden to load if any certificates for signing are revoked before signing time.

  @param[in]  AuthData      Pointer to the Authenticode signature retrieved from the signed image.
  @param[in]  AuthDataSize  Size of the Authenticode signature in bytes.

  @retval TRUE              Image is forbidden by dbx.
  @retval FALSE             Image is not forbidden by dbx.

**/
BOOLEAN
IsForbiddenByDbx (
  IN CONST UINT8            *AuthData,
  IN UINTN                  AuthDataSize,
  UINT8                         ImageDigest[MAX_DIGEST_SIZE],
  UINTN                         ImageDigestSize
  )
{
  EFI_STATUS          Status;
  BOOLEAN             IsForbidden;
  BOOLEAN             IsFound;
  UINT8               *Data;
  UINTN               DataSize;
  EFI_SIGNATURE_LIST  *CertList;
  UINTN               CertListSize;
  EFI_SIGNATURE_DATA  *CertData;
  UINT8               *RootCert;
  UINTN               RootCertSize;
  UINTN               CertCount;
  UINTN               Index;
  UINT8               *CertBuffer;
  UINTN               BufferLength;
  UINT8               *TrustedCert;
  UINTN               TrustedCertLength;
  UINT8               CertNumber;
  UINT8               *CertPtr;
  UINT8               *Cert;
  UINTN               CertSize;
  EFI_TIME            RevocationTime;

  //
  // Variable Initialization
  //
  IsForbidden       = TRUE;
  Data              = NULL;
  CertList          = NULL;
  CertData          = NULL;
  RootCert          = NULL;
  RootCertSize      = 0;
  Cert              = NULL;
  CertBuffer        = NULL;
  BufferLength      = 0;
  TrustedCert       = NULL;
  TrustedCertLength = 0;

  //
  // The image will not be forbidden if dbx can't be got.
  //
  DataSize = 0;
  Status   = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE1, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, NULL);
  ASSERT (EFI_ERROR (Status));
  if (Status != EFI_BUFFER_TOO_SMALL) {
    if (Status == EFI_NOT_FOUND) {
      //
      // Evidently not in dbx if the database doesn't exist.
      //
      IsForbidden = FALSE;
    }

    return IsForbidden;
  }

  Data = (UINT8 *)AllocateZeroPool (DataSize);
  if (Data == NULL) {
    return IsForbidden;
  }

  Status = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE1, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, (VOID *)Data);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  //
  // Verify image signature with RAW X509 certificates in DBX database.
  // If passed, the image will be forbidden.
  //
  CertList     = (EFI_SIGNATURE_LIST *)Data;
  CertListSize = DataSize;
  while ((CertListSize > 0) && (CertListSize >= CertList->SignatureListSize)) {
    if (CompareGuid (&CertList->SignatureType, &gEfiCertX509Guid)) {
      CertData  = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList + sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
      CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;

      for (Index = 0; Index < CertCount; Index++) {
        //
        // Iterate each Signature Data Node within this CertList for verify.
        //
        RootCert     = CertData->SignatureData;
        RootCertSize = CertList->SignatureSize - sizeof (EFI_GUID);

        //
        // Call AuthenticodeVerify library to Verify Authenticode struct.
        //
        IsForbidden = AuthenticodeVerify (
                        AuthData,
                        AuthDataSize,
                        RootCert,
                        RootCertSize,
                        ImageDigest,
                        ImageDigestSize
                        );
        if (IsForbidden) {
          DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is signed but signature is forbidden by DBX.\n"));
          goto Done;
        }

        CertData = (EFI_SIGNATURE_DATA *)((UINT8 *)CertData + CertList->SignatureSize);
      }
    }

    CertListSize -= CertList->SignatureListSize;
    CertList      = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

  //
  // Check X.509 Certificate Hash & Possible Timestamp.
  //

  //
  // Retrieve the certificate stack from AuthData
  // The output CertStack format will be:
  //       UINT8  CertNumber;
  //       UINT32 Cert1Length;
  //       UINT8  Cert1[];
  //       UINT32 Cert2Length;
  //       UINT8  Cert2[];
  //       ...
  //       UINT32 CertnLength;
  //       UINT8  Certn[];
  //
  Pkcs7GetSigners (AuthData, AuthDataSize, &CertBuffer, &BufferLength, &TrustedCert, &TrustedCertLength);
  if ((BufferLength == 0) || (CertBuffer == NULL) || ((*CertBuffer) == 0)) {
    IsForbidden = TRUE;
    goto Done;
  }

  //
  // Check if any hash of certificates embedded in AuthData is in the forbidden database.
  //
  CertNumber = (UINT8)(*CertBuffer);
  CertPtr    = CertBuffer + 1;
  for (Index = 0; Index < CertNumber; Index++) {
    CertSize = (UINTN)ReadUnaligned32 ((UINT32 *)CertPtr);
    Cert     = (UINT8 *)CertPtr + sizeof (UINT32);
    //
    // Advance CertPtr to the next cert in image signer's cert list
    //
    CertPtr = CertPtr + sizeof (UINT32) + CertSize;

    Status = IsCertHashFoundInDbx (Cert, CertSize, (EFI_SIGNATURE_LIST *)Data, DataSize, &RevocationTime, &IsFound);
    if (EFI_ERROR (Status)) {
      //
      // Error in searching dbx. Consider it as 'found'. RevocationTime might
      // not be valid in such situation.
      //
      IsForbidden = TRUE;
    } else if (IsFound) {
      //
      // Found Cert in dbx successfully. Check the timestamp signature and
      // signing time to determine if the image can be trusted.
      //
      if (PassTimestampCheck (AuthData, AuthDataSize, &RevocationTime)) {
        IsForbidden = FALSE;
        //
        // Pass DBT check. Continue to check other certs in image signer's cert list against DBX, DBT
        //
        continue;
      } else {
        IsForbidden = TRUE;
        DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is signed but signature failed the timestamp check.\n"));
        goto Done;
      }
    }
  }

  IsForbidden = FALSE;

Done:
  if (Data != NULL) {
    FreePool (Data);
  }

  Pkcs7FreeSigners (CertBuffer);
  Pkcs7FreeSigners (TrustedCert);

  return IsForbidden;
}

/**
  Check whether the image signature can be verified by the trusted certificates in DB database.

  @param[in]  AuthData      Pointer to the Authenticode signature retrieved from signed image.
  @param[in]  AuthDataSize  Size of the Authenticode signature in bytes.

  @retval TRUE         Image passed verification using certificate in db.
  @retval FALSE        Image didn't pass verification using certificate in db.

**/
BOOLEAN
IsAllowedByDb (
  IN CONST UINT8        *AuthData,
  IN UINTN              AuthDataSize,
  UINT8                 ImageDigest[MAX_DIGEST_SIZE],
  UINTN                 ImageDigestSize
  )
{
  EFI_STATUS          Status;
  BOOLEAN             VerifyStatus;
  BOOLEAN             IsFound;
  EFI_SIGNATURE_LIST  *CertList;
  EFI_SIGNATURE_DATA  *CertData;
  UINTN               DataSize;
  UINT8               *Data;
  UINT8               *RootCert;
  UINTN               RootCertSize;
  UINTN               Index;
  UINTN               CertCount;
  UINTN               DbxDataSize;
  UINT8               *DbxData;
  EFI_TIME            RevocationTime;

  Data         = NULL;
  CertList     = NULL;
  CertData     = NULL;
  RootCert     = NULL;
  DbxData      = NULL;
  RootCertSize = 0;
  VerifyStatus = FALSE;

  //
  // Fetch 'db' content. If 'db' doesn't exist or encounters problem to get the
  // data, return not-allowed-by-db (FALSE).
  //
  DataSize = 0;
  Status   = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, NULL);
  ASSERT (EFI_ERROR (Status));
  if (Status != EFI_BUFFER_TOO_SMALL) {
    return VerifyStatus;
  }

  Data = (UINT8 *)AllocateZeroPool (DataSize);
  if (Data == NULL) {
    return VerifyStatus;
  }

  Status = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE, &gEfiImageSecurityDatabaseGuid, NULL, &DataSize, (VOID *)Data);
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  //
  // Fetch 'dbx' content. If 'dbx' doesn't exist, continue to check 'db'.
  // If any other errors occurred, no need to check 'db' but just return
  // not-allowed-by-db (FALSE) to avoid bypass.
  //
  DbxDataSize = 0;
  Status      = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE1, &gEfiImageSecurityDatabaseGuid, NULL, &DbxDataSize, NULL);
  ASSERT (EFI_ERROR (Status));
  if (Status != EFI_BUFFER_TOO_SMALL) {
    if (Status != EFI_NOT_FOUND) {
      goto Done;
    }

    //
    // 'dbx' does not exist. Continue to check 'db'.
    //
  } else {
    //
    // 'dbx' exists. Get its content.
    //
    DbxData = (UINT8 *)AllocateZeroPool (DbxDataSize);
    if (DbxData == NULL) {
      goto Done;
    }

    Status = gRT->GetVariable (EFI_IMAGE_SECURITY_DATABASE1, &gEfiImageSecurityDatabaseGuid, NULL, &DbxDataSize, (VOID *)DbxData);
    if (EFI_ERROR (Status)) {
      goto Done;
    }
  }

  //
  // Find X509 certificate in Signature List to verify the signature in pkcs7 signed data.
  //
  CertList = (EFI_SIGNATURE_LIST *)Data;
  while ((DataSize > 0) && (DataSize >= CertList->SignatureListSize)) {
    if (CompareGuid (&CertList->SignatureType, &gEfiCertX509Guid)) {
      CertData  = (EFI_SIGNATURE_DATA *)((UINT8 *)CertList + sizeof (EFI_SIGNATURE_LIST) + CertList->SignatureHeaderSize);
      CertCount = (CertList->SignatureListSize - sizeof (EFI_SIGNATURE_LIST) - CertList->SignatureHeaderSize) / CertList->SignatureSize;

      for (Index = 0; Index < CertCount; Index++) {
        //
        // Iterate each Signature Data Node within this CertList for verify.
        //
        RootCert     = CertData->SignatureData;
        RootCertSize = CertList->SignatureSize - sizeof (EFI_GUID);

        //
        // Call AuthenticodeVerify library to Verify Authenticode struct.
        //
        VerifyStatus = AuthenticodeVerify (
                         AuthData,
                         AuthDataSize,
                         RootCert,
                         RootCertSize,
                         ImageDigest,
                         ImageDigestSize
                         );
        if (VerifyStatus) {
          //
          // The image is signed and its signature is found in 'db'.
          //
          if (DbxData != NULL) {
            //
            // Here We still need to check if this RootCert's Hash is revoked
            //
            Status = IsCertHashFoundInDbx (RootCert, RootCertSize, (EFI_SIGNATURE_LIST *)DbxData, DbxDataSize, &RevocationTime, &IsFound);
            if (EFI_ERROR (Status)) {
              //
              // Error in searching dbx. Consider it as 'found'. RevocationTime might
              // not be valid in such situation.
              //
              VerifyStatus = FALSE;
            } else if (IsFound) {
              //
              // Check the timestamp signature and signing time to determine if the RootCert can be trusted.
              //
              VerifyStatus = PassTimestampCheck (AuthData, AuthDataSize, &RevocationTime);
              if (!VerifyStatus) {
                DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is signed and signature is accepted by DB, but its root cert failed the timestamp check.\n"));
              }
            }
          }

          //
          // There's no 'dbx' to check revocation time against (must-be pass),
          // or, there's revocation time found in 'dbx' and checked againt 'dbt'
          // (maybe pass or fail, depending on timestamp compare result). Either
          // way the verification job has been completed at this point.
          //
          goto Done;
        }

        CertData = (EFI_SIGNATURE_DATA *)((UINT8 *)CertData + CertList->SignatureSize);
      }
    }

    DataSize -= CertList->SignatureListSize;
    CertList  = (EFI_SIGNATURE_LIST *)((UINT8 *)CertList + CertList->SignatureListSize);
  }

Done:

  if (VerifyStatus) {
    SecureBootHook (EFI_IMAGE_SECURITY_DATABASE, &gEfiImageSecurityDatabaseGuid, CertList->SignatureSize, CertData);
  }

  if (Data != NULL) {
    FreePool (Data);
  }

  if (DbxData != NULL) {
    FreePool (DbxData);
  }

  return VerifyStatus;
}

/**
  Provide verification service for signed images, which include both signature validation
  and platform policy control. For signature types, both UEFI WIN_CERTIFICATE_UEFI_GUID and
  MSFT Authenticode type signatures are supported.

  In this implementation, only verify external executables when in USER MODE.
  Executables from FV is bypass, so pass in AuthenticationStatus is ignored.

  The image verification policy is:
    If the image is signed,
      At least one valid signature or at least one hash value of the image must match a record
      in the security database "db", and no valid signature nor any hash value of the image may
      be reflected in the security database "dbx".
    Otherwise, the image is not signed,
      The SHA256 hash value of the image must match a record in the security database "db", and
      not be reflected in the security data base "dbx".

  Caution: This function may receive untrusted input.
  PE/COFF image is external input, so this function will validate its data structure
  within this image buffer before use.

  @param[in]    AuthenticationStatus
                           This is the authentication status returned from the security
                           measurement services for the input file.
  @param[in]    File       This is a pointer to the device path of the file that is
                           being dispatched. This will optionally be used for logging.
  @param[in]    FileBuffer File buffer matches the input file device path.
  @param[in]    FileSize   Size of File buffer matches the input file device path.
  @param[in]    BootPolicy A boot policy that was used to call LoadImage() UEFI service.

  @retval EFI_SUCCESS            The file specified by DevicePath and non-NULL
                                 FileBuffer did authenticate, and the platform policy dictates
                                 that the DXE Foundation may use the file.
  @retval EFI_SUCCESS            The device path specified by NULL device path DevicePath
                                 and non-NULL FileBuffer did authenticate, and the platform
                                 policy dictates that the DXE Foundation may execute the image in
                                 FileBuffer.
  @retval EFI_SECURITY_VIOLATION The file specified by File did not authenticate, and
                                 the platform policy dictates that File should be placed
                                 in the untrusted state. The image has been added to the file
                                 execution table.
  @retval EFI_ACCESS_DENIED      The file specified by File and FileBuffer did not
                                 authenticate, and the platform policy dictates that the DXE
                                 Foundation may not use File. The image has
                                 been added to the file execution table.

**/
EFI_STATUS
EFIAPI
DxeImageVerificationHandler (
  IN  UINT32                          AuthenticationStatus,
  IN  CONST EFI_DEVICE_PATH_PROTOCOL  *File  OPTIONAL,
  IN  VOID                            *FileBuffer,
  IN  UINTN                           FileSize,
  IN  BOOLEAN                         BootPolicy
  )
{
  BOOLEAN                       IsVerified;
  EFI_SIGNATURE_LIST            *SignatureList;
  UINTN                         SignatureListSize;
  EFI_SIGNATURE_DATA            *Signature;
  EFI_IMAGE_EXECUTION_ACTION    Action;
  CONST WIN_CERTIFICATE                *WinCertificate;
  UINT32                        Policy;
  UINT8                         *SecureBoot;
  CONST WIN_CERTIFICATE_EFI_PKCS       *PkcsCertData;
  CONST WIN_CERTIFICATE_UEFI_GUID      *WinCertUefiGuid;
  CONST UINT8                          *AuthData;
  UINTN                         AuthDataSize;
  CHAR16                        *NameStr;
  EFI_STATUS                    HashStatus;
  EFI_STATUS                    DbStatus;
  BOOLEAN                       IsFound;
  UEFI_IMAGE_LOADER_IMAGE_CONTEXT      *ImageContext;

  UINT8                               ImageDigest[MAX_DIGEST_SIZE];
  UINTN                               ImageDigestSize;
  CONST EFI_GUID *CertType;

  SignatureList     = NULL;
  SignatureListSize = 0;
  WinCertificate    = NULL;
  PkcsCertData      = NULL;
  Action            = EFI_IMAGE_EXECUTION_AUTH_UNTESTED;
  IsVerified        = FALSE;
  IsFound           = FALSE;

  // FIXME:
  ASSERT (FileSize == sizeof (UEFI_IMAGE_LOADER_IMAGE_CONTEXT));
  ImageContext = FileBuffer;

  //
  // Check the image type and get policy setting.
  //
  switch (GetImageType (File)) {
    case IMAGE_FROM_FV:
      Policy = ALWAYS_EXECUTE;
      break;

    case IMAGE_FROM_OPTION_ROM:
      Policy = PcdGet32 (PcdOptionRomImageVerificationPolicy);
      break;

    case IMAGE_FROM_REMOVABLE_MEDIA:
      Policy = PcdGet32 (PcdRemovableMediaImageVerificationPolicy);
      break;

    case IMAGE_FROM_FIXED_MEDIA:
      Policy = PcdGet32 (PcdFixedMediaImageVerificationPolicy);
      break;

    default:
      Policy = DENY_EXECUTE_ON_SECURITY_VIOLATION;
      break;
  }

  //
  // If policy is always/never execute, return directly.
  //
  if (Policy == ALWAYS_EXECUTE) {
    return EFI_SUCCESS;
  }

  if (Policy == NEVER_EXECUTE) {
    return EFI_ACCESS_DENIED;
  }

  //
  // The policy QUERY_USER_ON_SECURITY_VIOLATION and ALLOW_EXECUTE_ON_SECURITY_VIOLATION
  // violates the UEFI spec and has been removed.
  //
  ASSERT (Policy != QUERY_USER_ON_SECURITY_VIOLATION && Policy != ALLOW_EXECUTE_ON_SECURITY_VIOLATION);
  if ((Policy == QUERY_USER_ON_SECURITY_VIOLATION) || (Policy == ALLOW_EXECUTE_ON_SECURITY_VIOLATION)) {
    CpuDeadLoop ();
  }

  GetEfiGlobalVariable2 (EFI_SECURE_BOOT_MODE_NAME, (VOID **)&SecureBoot, NULL);
  //
  // Skip verification if SecureBoot variable doesn't exist.
  //
  if (SecureBoot == NULL) {
    return EFI_SUCCESS;
  }

  //
  // Skip verification if SecureBoot is disabled but not AuditMode
  //
  if (*SecureBoot == SECURE_BOOT_MODE_DISABLE) {
    FreePool (SecureBoot);
    return EFI_SUCCESS;
  }

  FreePool (SecureBoot);

  //
  // Read the Dos header.
  //
  if (FileBuffer == NULL) {
    return EFI_ACCESS_DENIED;
  }

  HashStatus = UefiImageGetFirstCertificate (ImageContext, &WinCertificate);

  //
  // Start Image Validation.
  //
  if (HashStatus == RETURN_NOT_FOUND) {
    //
    // This image is not signed. The SHA256 hash value of the image must match a record in the security database "db",
    // and not be reflected in the security data base "dbx".
    //
    if (!HashPeImage (ImageContext, &mHash[HASHALG_SHA256], ImageDigest, &ImageDigestSize)) {
      DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Failed to hash this image.\n"));
      goto Failed;
    }

    CertType = &gEfiCertSha256Guid;

    DbStatus = IsSignatureFoundInDatabase (
                 EFI_IMAGE_SECURITY_DATABASE1,
                 ImageDigest,
                 CertType,
                 ImageDigestSize,
                 &IsFound
                 );
    if (EFI_ERROR (DbStatus) || IsFound) {
      //
      // Image Hash is in forbidden database (DBX).
      //
      DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is not signed and hash of image is forbidden by DBX.\n"));
      goto Failed;
    }

    DbStatus = IsSignatureFoundInDatabase (
                 EFI_IMAGE_SECURITY_DATABASE,
                 ImageDigest,
                 CertType,
                 ImageDigestSize,
                 &IsFound
                 );
    if (!EFI_ERROR (DbStatus) && IsFound) {
      //
      // Image Hash is in allowed database (DB).
      //
      return EFI_SUCCESS;
    }

    //
    // Image Hash is not found in both forbidden and allowed database.
    //
    DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is not signed and hash of image is not found in DB/DBX.\n"));
    goto Failed;
  }

  //
  // Verify the signature of the image, multiple signatures are allowed as per PE/COFF Section 4.7
  // "Attribute Certificate Table".
  // The first certificate starts at offset (SecDataDir->VirtualAddress) from the start of the file.
  //
  for (
    ;
    !RETURN_ERROR (HashStatus);
    HashStatus = UefiImageGetNextCertificate (ImageContext, &WinCertificate)
    )
  {
    //
    // Verify the image's Authenticode signature, only DER-encoded PKCS#7 signed data is supported.
    //
    if (WinCertificate->wCertificateType == WIN_CERT_TYPE_PKCS_SIGNED_DATA) {
      //
      // The certificate is formatted as WIN_CERTIFICATE_EFI_PKCS which is described in the
      // Authenticode specification.
      //
      PkcsCertData = (CONST WIN_CERTIFICATE_EFI_PKCS *)WinCertificate;
      if (PkcsCertData->Hdr.dwLength <= sizeof (PkcsCertData->Hdr)) {
        break;
      }

      AuthData     = PkcsCertData->CertData;
      AuthDataSize = PkcsCertData->Hdr.dwLength - sizeof (PkcsCertData->Hdr);
    } else if (WinCertificate->wCertificateType == WIN_CERT_TYPE_EFI_GUID) {
      //
      // The certificate is formatted as WIN_CERTIFICATE_UEFI_GUID which is described in UEFI Spec.
      //
      WinCertUefiGuid = (CONST WIN_CERTIFICATE_UEFI_GUID *)WinCertificate;

      if (!CompareGuid (&WinCertUefiGuid->CertType, &gEfiCertPkcs7Guid)) {
        continue;
      }

      if (WinCertUefiGuid->Hdr.dwLength <= OFFSET_OF(WIN_CERTIFICATE_UEFI_GUID, CertData)) {
        break;
      }
      AuthData     = WinCertUefiGuid->CertData;
      AuthDataSize = WinCertUefiGuid->Hdr.dwLength - OFFSET_OF (WIN_CERTIFICATE_UEFI_GUID, CertData);
    } else {

      continue;
    }

    HashStatus = HashPeImageByType (ImageContext, AuthData, AuthDataSize, ImageDigest, &ImageDigestSize, &CertType);
    if (EFI_ERROR (HashStatus)) {
      continue;
    }

    //
    // Check the digital signature against the revoked certificate in forbidden database (dbx).
    //
    if (IsForbiddenByDbx (AuthData, AuthDataSize, ImageDigest, ImageDigestSize)) {
      Action     = EFI_IMAGE_EXECUTION_AUTH_SIG_FAILED;
      IsVerified = FALSE;
    }

    //
    // Check the digital signature against the valid certificate in allowed database (db).
    //
    if (!IsVerified) {
      if (IsAllowedByDb (AuthData, AuthDataSize, ImageDigest, ImageDigestSize)) {
        IsVerified = TRUE;
      }
    }

    //
    // Check the image's hash value.
    //
    DbStatus = IsSignatureFoundInDatabase (
                 EFI_IMAGE_SECURITY_DATABASE1,
                 ImageDigest,
                 CertType,
                 ImageDigestSize,
                 &IsFound
                 );
    if (EFI_ERROR (DbStatus) || IsFound) {
      Action = EFI_IMAGE_EXECUTION_AUTH_SIG_FOUND;
      DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is signed but hash of image is found in DBX.\n"));
      IsVerified = FALSE;
      break;
    }

    if (!IsVerified) {
      DbStatus = IsSignatureFoundInDatabase (
                   EFI_IMAGE_SECURITY_DATABASE,
                   ImageDigest,
                   CertType,
                   ImageDigestSize,
                   &IsFound
                   );
      if (!EFI_ERROR (DbStatus) && IsFound) {
        IsVerified = TRUE;
      } else {
        DEBUG ((DEBUG_INFO, "DxeImageVerificationLib: Image is signed but signature is not allowed by DB and hash of image is not found in DB/DBX.\n"));
      }
    }
  }

  if (IsVerified) {
    return EFI_SUCCESS;
  }
  if ((Action == EFI_IMAGE_EXECUTION_AUTH_SIG_FAILED) || (Action == EFI_IMAGE_EXECUTION_AUTH_SIG_FOUND)) {
    //
    // Get image hash value as signature of executable.
    //
    SignatureListSize = sizeof (EFI_SIGNATURE_LIST) + sizeof (EFI_SIGNATURE_DATA) - 1 + ImageDigestSize;
    SignatureList     = (EFI_SIGNATURE_LIST *)AllocateZeroPool (SignatureListSize);
    if (SignatureList == NULL) {
      SignatureListSize = 0;
      goto Failed;
    }

    SignatureList->SignatureHeaderSize = 0;
    SignatureList->SignatureListSize   = (UINT32)SignatureListSize;
    SignatureList->SignatureSize       = (UINT32)(sizeof (EFI_SIGNATURE_DATA) - 1 + ImageDigestSize);
    CopyMem (&SignatureList->SignatureType, CertType, sizeof (EFI_GUID));
    Signature = (EFI_SIGNATURE_DATA *)((UINT8 *)SignatureList + sizeof (EFI_SIGNATURE_LIST));
    CopyMem (Signature->SignatureData, ImageDigest, ImageDigestSize);
  }

Failed:
  //
  // Policy decides to defer or reject the image; add its information in image
  // executable information table in either case.
  //
  NameStr = ConvertDevicePathToText (File, FALSE, TRUE);
  AddImageExeInfo (Action, NameStr, File, SignatureList, SignatureListSize);
  if (NameStr != NULL) {
    DEBUG ((DEBUG_INFO, "The image doesn't pass verification: %s\n", NameStr));
    FreePool (NameStr);
  }

  if (SignatureList != NULL) {
    FreePool (SignatureList);
  }

  if (Policy == DEFER_EXECUTE_ON_SECURITY_VIOLATION) {
    return EFI_SECURITY_VIOLATION;
  }

  return EFI_ACCESS_DENIED;
}

/**
  On Ready To Boot Services Event notification handler.

  Add the image execution information table if it is not in system configuration table.

  @param[in]  Event     Event whose notification function is being invoked
  @param[in]  Context   Pointer to the notification function's context

**/
VOID
EFIAPI
OnReadyToBoot (
  IN      EFI_EVENT  Event,
  IN      VOID       *Context
  )
{
  EFI_IMAGE_EXECUTION_INFO_TABLE  *ImageExeInfoTable;
  UINTN                           ImageExeInfoTableSize;

  EfiGetSystemConfigurationTable (&gEfiImageSecurityDatabaseGuid, (VOID **)&ImageExeInfoTable);
  if (ImageExeInfoTable != NULL) {
    return;
  }

  ImageExeInfoTableSize = sizeof (EFI_IMAGE_EXECUTION_INFO_TABLE);
  ImageExeInfoTable     = (EFI_IMAGE_EXECUTION_INFO_TABLE *)AllocateRuntimePool (ImageExeInfoTableSize);
  if (ImageExeInfoTable == NULL) {
    return;
  }

  ImageExeInfoTable->NumberOfImages = 0;
  gBS->InstallConfigurationTable (&gEfiImageSecurityDatabaseGuid, (VOID *)ImageExeInfoTable);
}

/**
  Register security measurement handler.

  @param  ImageHandle   ImageHandle of the loaded driver.
  @param  SystemTable   Pointer to the EFI System Table.

  @retval EFI_SUCCESS   The handlers were registered successfully.
**/
EFI_STATUS
EFIAPI
DxeImageVerificationLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_EVENT  Event;

  //
  // Register the event to publish the image execution table.
  //
  EfiCreateEventReadyToBootEx (
    TPL_CALLBACK,
    OnReadyToBoot,
    NULL,
    &Event
    );

  return RegisterSecurity2Handler (
           DxeImageVerificationHandler,
           EFI_AUTH_OPERATION_VERIFY_IMAGE | EFI_AUTH_OPERATION_IMAGE_REQUIRED
           );
}
