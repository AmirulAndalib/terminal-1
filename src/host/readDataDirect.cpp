// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "readDataDirect.hpp"
#include "dbcs.h"
#include "misc.h"

#include "../interactivity/inc/ServiceLocator.hpp"

// Routine Description:
// - Constructs direct read data class to hold context across sessions
// generally when there's not enough data to return.
// Arguments:
// - pInputBuffer - Buffer that data will be read from.
// - pInputReadHandleData - Context stored across calls from the same
// input handle to return partial data appropriately.
// the user's buffer (pOutRecords)
// - eventReadCount - the number of events to read
// - partialEvents - any partial events already read
// Return Value:
// - THROW: Throws E_INVALIDARG for invalid pointers.
DirectReadData::DirectReadData(_In_ InputBuffer* const pInputBuffer,
                               _In_ INPUT_READ_HANDLE_DATA* const pInputReadHandleData,
                               const size_t eventReadCount,
                               _In_ std::deque<std::unique_ptr<IInputEvent>> partialEvents) :
    ReadData(pInputBuffer, pInputReadHandleData),
    _eventReadCount{ eventReadCount },
    _partialEvents{ std::move(partialEvents) }
{
}

// Routine Description:
// - This routine is called to complete a direct read that blocked in
//   ReadInputBuffer. The context of the read was saved in the DirectReadData
//   structure. This routine is called when events have been written to
//   the input buffer. It is called in the context of the writing thread.
// Arguments:
// - TerminationReason - if this routine is called because a ctrl-c or
// ctrl-break was seen, this argument contains CtrlC or CtrlBreak. If
// the owning thread is exiting, it will have ThreadDying. Otherwise 0.
// - fIsUnicode - Should we return UCS-2 unicode data, or should we
// run the final data through the current Input Codepage before
// returning?
// - pReplyStatus - The status code to return to the client
// application that originally called the API (before it was queued to
// wait)
// - pNumBytes - not used
// - pControlKeyState - For certain types of reads, this specifies
// which modifier keys were held.
// - pOutputData - a pointer to a
// std::deque<std::unique_ptr<IInputEvent>> that is used to the read
// input events back to the server
// Return Value:
// - true if the wait is done and result buffer/status code can be sent back to the client.
// - false if we need to continue to wait until more data is available.
bool DirectReadData::Notify(const WaitTerminationReason TerminationReason,
                            const bool fIsUnicode,
                            _Out_ NTSTATUS* const pReplyStatus,
                            _Out_ size_t* const pNumBytes,
                            _Out_ DWORD* const pControlKeyState,
                            _Out_ void* const pOutputData)
try
{
    FAIL_FAST_IF_NULL(pOutputData);
    FAIL_FAST_IF(_pInputReadHandleData->GetReadCount() == 0);

    const auto& gci = Microsoft::Console::Interactivity::ServiceLocator::LocateGlobals().getConsoleInformation();
    assert(gci.IsConsoleLocked());

    *pReplyStatus = STATUS_SUCCESS;
    *pControlKeyState = 0;
    *pNumBytes = 0;

    std::deque<std::unique_ptr<IInputEvent>> readEvents;

    // If ctrl-c or ctrl-break was seen, ignore it.
    if (WI_IsAnyFlagSet(TerminationReason, (WaitTerminationReason::CtrlC | WaitTerminationReason::CtrlBreak)))
    {
        return false;
    }

    // See if called by CsrDestroyProcess or CsrDestroyThread
    // via ConsoleNotifyWaitBlock. If so, just decrement the ReadCount and return.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::ThreadDying))
    {
        *pReplyStatus = STATUS_THREAD_IS_TERMINATING;
        return true;
    }

    // We must see if we were woken up because the handle is being
    // closed. If so, we decrement the read count. If it goes to
    // zero, we wake up the close thread. Otherwise, we wake up any
    // other thread waiting for data.
    if (WI_IsFlagSet(TerminationReason, WaitTerminationReason::HandleClosing))
    {
        *pReplyStatus = STATUS_ALERTED;
        return true;
    }

    // if we get to here, this routine was called either by the input
    // thread or a write routine.  both of these callers grab the
    // current console lock.

    // calculate how many events we need to read
    size_t amountAlreadyRead;
    if (FAILED(SizeTAdd(_partialEvents.size(), _outEvents.size(), &amountAlreadyRead)))
    {
        *pReplyStatus = STATUS_INTEGER_OVERFLOW;
        return true;
    }
    size_t amountToRead;
    if (FAILED(SizeTSub(_eventReadCount, amountAlreadyRead, &amountToRead)))
    {
        *pReplyStatus = STATUS_INTEGER_OVERFLOW;
        return true;
    }

    // check if partial bytes are already stored that we should read
    if (!fIsUnicode && amountToRead > 0)
    {
        std::string buffer(amountToRead, '\0');
        gsl::span writer{ buffer };
        _pInputBuffer->ConsumeCachedA(writer);

        const auto read = buffer.size() - writer.size();
        std::string_view str{ buffer.data(), read };
        StringToInputEvents(str, _partialEvents);

        amountToRead -= read;
    }

    *pReplyStatus = _pInputBuffer->Read(readEvents,
                                        amountToRead,
                                        false,
                                        false,
                                        fIsUnicode,
                                        false);

    if (*pReplyStatus == CONSOLE_STATUS_WAIT)
    {
        return false;
    }

    // split key events to oem chars if necessary
    if (!fIsUnicode && *pReplyStatus == STATUS_SUCCESS)
    {
        SplitToOem(readEvents);
    }

    // combine partial and whole events
    while (!_partialEvents.empty())
    {
        readEvents.push_front(std::move(_partialEvents.back()));
        _partialEvents.pop_back();
    }

    // move read events to out storage
    for (size_t i = 0; i < _eventReadCount; ++i)
    {
        if (readEvents.empty())
        {
            break;
        }
        _outEvents.push_back(std::move(readEvents.front()));
        readEvents.pop_front();
    }

    if (!readEvents.empty())
    {
        std::string buffer;
        InputEventsToString(readEvents, buffer);
        _pInputBuffer->CacheA(buffer);
    }

    // move events to pOutputData
    const auto pOutputDeque = static_cast<std::deque<std::unique_ptr<IInputEvent>>* const>(pOutputData);
    *pNumBytes = _outEvents.size() * sizeof(INPUT_RECORD);
    *pOutputDeque = std::move(_outEvents);

    return true;
}
catch (...)
{
    // There doesn't seem to be a way to just get the NT exception code with WIL.
    *pReplyStatus = NTSTATUS_FROM_HRESULT(wil::ResultFromCaughtException());
    return true;
}

void DirectReadData::MigrateUserBuffersOnTransitionToBackgroundWait(const void* /*oldBuffer*/, void* /*newBuffer*/)
{
    // Direct read doesn't hold API message buffers
}
