/* Copyright (c) 2010
 * The Regents of the University of Michigan
 * All Rights Reserved
 *
 * Permission is granted to use, copy and redistribute this software
 * for noncommercial education and research purposes, so long as no
 * fee is charged, and so long as the name of the University of Michigan
 * is not used in any advertising or publicity pertaining to the use
 * or distribution of this software without specific, written prior
 * authorization.  Permission to modify or otherwise create derivative
 * works of this software is not granted.
 *
 * This software is provided as is, without representation or warranty
 * of any kind either express or implied, including without limitation
 * the implied warranties of merchantability, fitness for a particular
 * purpose, or noninfringement.  The Regents of the University of
 * Michigan shall not be liable for any damages, including special,
 * indirect, incidental, or consequential damages, with respect to any
 * claim arising out of or in connection with the use of the software,
 * even if it has been or is hereafter advised of the possibility of
 * such damages.
 */

#ifndef NFS41_WMLKM_H
#define NFS41_WMLKM_H 1

#ifdef __cplusplus
extern "C" {
#endif

NTSTATUS
WmlTinySystemControl(
    __inout PVOID WmiLibInfo,
    __in PVOID DeviceObject,
    __in PVOID Irp
    );

ULONG
WmlTrace(
    __in ULONG Type,
    __in LPVOID TraceGuid,
    __in ULONG64 LoggerHandle,
    ... // Pairs: Address, Length
    );

#ifdef __cplusplus
};
#endif

#endif // NFS41_WMLKM_H

