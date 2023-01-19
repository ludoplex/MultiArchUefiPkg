/** @file

    Copyright (c) 2023, Intel Corporation. All rights reserved.<BR>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

**/

#include <unicorn.h>
#include "X86Emulator.h"

#ifdef WRAPPED_ENTRY_POINTS
STATIC LIST_ENTRY mEventList;

typedef struct {
  LIST_ENTRY       Link;
  EFI_EVENT        Event;
  VOID             *X64NotifyContext;
  EFI_EVENT_NOTIFY X64NotifyFunction;
  UINT64           CallerRip;
} WRAPPED_EVENT_RECORD;

STATIC
VOID
EfiWrappersDumpEvents ()
{
  LIST_ENTRY           *Entry;
  WRAPPED_EVENT_RECORD *Record;
  X86_IMAGE_RECORD     *Image;

  DEBUG ((DEBUG_ERROR, "Wrapped EFI_EVENTs:\n"));
  for (Entry = GetFirstNode (&mEventList);
       !IsNull (&mEventList, Entry);
       Entry = GetNextNode (&mEventList, Entry)) {
    Record = BASE_CR (Entry, WRAPPED_EVENT_RECORD, Link);
    Image = FindImageRecord (Record->CallerRip);

    DEBUG ((DEBUG_ERROR, "\tImageBase 0x%lx Event %p Fn %p Context %p\n",
            Image->ImageBase, Record->Event, Record->X64NotifyFunction,
            Record->X64NotifyContext));
  }
}

STATIC
VOID
EFIAPI
EfiWrappersEventNotify (
  IN  EFI_EVENT Event,
  IN  VOID      *Context
  )
{
  WRAPPED_EVENT_RECORD *Record = Context;
  UINT64 Args[2] = { (UINT64) Event, (UINT64) Record->X64NotifyContext };

  CpuRunFunc ((UINT64) Record->X64NotifyFunction, Args);
}

STATIC
WRAPPED_EVENT_RECORD *
EfiWrappersFindEvent (
  IN  EFI_EVENT Event
  )
{
  LIST_ENTRY           *Entry;
  WRAPPED_EVENT_RECORD *Record;

  for (Entry = GetFirstNode (&mEventList);
       !IsNull (&mEventList, Entry);
       Entry = GetNextNode (&mEventList, Entry)) {
    Record = BASE_CR (Entry, WRAPPED_EVENT_RECORD, Link);

    if (Record->Event == Event) {
      return Record;
    }
  }

  return NULL;
}

EFI_STATUS
EfiWrapperCloseEvent (
  IN  UINT64 OriginalRip,
  IN  UINT64 ReturnAddress,
  IN  UINT64 *Args
  )
{
  EFI_EVENT            Event = (EFI_EVENT) Args[0];
  WRAPPED_EVENT_RECORD *Record;
  EFI_STATUS           Status;
  EFI_TPL              Tpl;

  Record = EfiWrappersFindEvent (Event);
  Status = gBS->CloseEvent (Record->Event);

  if (Record != NULL) {
    Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
    RemoveEntryList (&Record->Link);
    gBS->RestoreTPL (Tpl);
    FreePool (Record);
  }

  return Status;
}

EFI_STATUS
EfiWrapperCreateEventCommon (
  IN  UINT64 OriginalRip,
  IN  UINT64 ReturnAddress,
  IN  UINT64 *Args
  )
{
  UINT32               Type = Args[0];
  EFI_TPL              NotifyTpl = Args[1];
  EFI_EVENT_NOTIFY     NotifyFunction = (VOID *) Args[2];
  VOID                 *NotifyContext = (VOID *) Args[3];
  CONST EFI_GUID       *EventGroup = NULL;
  EFI_EVENT            *Event;
  WRAPPED_EVENT_RECORD *Record;
  EFI_STATUS           Status;
  EFI_TPL              Tpl;

  if (OriginalRip == (UINT64) gBS->CreateEvent) {
    Event = (VOID *) Args[4];
  } else {
    ASSERT (OriginalRip == (UINT64) gBS->CreateEventEx);
    EventGroup = (VOID *) Args[4];
    Event = (VOID *) Args[5];
  }

  Record = AllocatePool (sizeof *Record);
  if (Record == NULL) {
     DEBUG ((DEBUG_ERROR, "failed to allocate event wrapper\n"));
     return EFI_OUT_OF_RESOURCES;
  }

  Record->CallerRip = ReturnAddress;
  Record->X64NotifyContext = NotifyContext;
  Record->X64NotifyFunction = NotifyFunction;
  NotifyFunction = EfiWrappersEventNotify;
  NotifyContext = Record;

  /*
   * Before CreateEvent to avoid races! After CreateEvent succeeds,
   * the event could be signalled (and closed from notification fn).
   */
  Tpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  InsertTailList (&mEventList, &Record->Link);
  gBS->RestoreTPL (Tpl);

  if (OriginalRip == (UINT64) gBS->CreateEvent) {
    Status = gBS->CreateEvent (Type, NotifyTpl, NotifyFunction,
                               NotifyContext, &Record->Event);
  } else {
    Status = gBS->CreateEventEx (Type, NotifyTpl, NotifyFunction,
                                 NotifyContext, EventGroup,
                                 &Record->Event);
  }

  if (EFI_ERROR (Status)) {
    *Event = NULL;
    FreePool (Record);
    return Status;
  }

  *Event = Record->Event;
  return EFI_SUCCESS;
}
#endif /* WRAPPED_ENTRY_POINTS */

UINT64
EfiWrappersOverride (
  IN  UINT64 Rip
  )
{
   /*
    * TODO: catch/filter SetMemoryAttributes to ignore any
    * attempts to change attributes for the emulated image itself?
    */

#ifdef WRAPPED_ENTRY_POINTS
  if (Rip == (UINT64) gBS->CreateEvent) {
    return (UINT64) EfiWrapperCreateEventCommon;
  } else if (Rip == (UINT64) gBS->CreateEventEx) {
    return (UINT64) EfiWrapperCreateEventCommon;
  } else if (Rip == (UINT64) gBS->CloseEvent) {
    return (UINT64) EfiWrapperCloseEvent;
  }
#endif /* WRAPPED_ENTRY_POINTS */
  if (Rip == (UINTN) gBS->ExitBootServices) {
    DEBUG ((DEBUG_ERROR,
            "Unsupported emulated ExitBootServices\n"));
    return (UINT64) &NativeUnsupported;
  } else if (Rip == (UINTN) gCpu->RegisterInterruptHandler) {
    DEBUG ((DEBUG_ERROR,
            "Unsupported emulated RegisterInterruptHandler\n"));
    return (UINT64) &NativeUnsupported;
  }

  return Rip;
}

VOID
EfiWrappersInit (
  VOID
  )
{
#ifdef WRAPPED_ENTRY_POINTS
  InitializeListHead (&mEventList);
#endif /* WRAPPED_ENTRY_POINTS */
}

VOID
EfiWrappersDump (
  VOID
  )
{
#ifdef WRAPPED_ENTRY_POINTS
  EfiWrappersDumpEvents ();
#endif /* WRAPPED_ENTRY_POINTS */
}
