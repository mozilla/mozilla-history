/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: sw=2 ts=2 et :
 */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Plugin App.
 *
 * The Initial Developer of the Original Code is
 *   Ben Turner <bent.mozilla@gmail.com>
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "WindowsMessageLoop.h"
#include "SyncChannel.h"

#include "nsAutoPtr.h"
#include "nsServiceManagerUtils.h"
#include "nsStringGlue.h"
#include "nsIXULAppInfo.h"

#include "mozilla/Mutex.h"

using mozilla::ipc::SyncChannel;
using mozilla::MutexAutoUnlock;

using namespace mozilla::ipc::windows;

/**
 * The Windows-only code below exists to solve a general problem with deadlocks
 * that we experience when sending synchronous IPC messages to processes that
 * contain native windows (i.e. HWNDs). Windows (the OS) sends synchronous
 * messages between parent and child HWNDs in multiple circumstances (e.g.
 * WM_PARENTNOTIFY, WM_NCACTIVATE, etc.), even when those HWNDs are controlled
 * by different threads or different processes. Thus we can very easily end up
 * in a deadlock by a call stack like the following:
 *
 * Process A:
 *   - CreateWindow(...) creates a "parent" HWND.
 *   - SendCreateChildWidget(HWND) is a sync IPC message that sends the "parent"
 *         HWND over to Process B. Process A blocks until a response is received
 *         from Process B.
 *
 * Process B:
 *   - RecvCreateWidget(HWND) gets the "parent" HWND from Process A.
 *   - CreateWindow(..., HWND) creates a "child" HWND with the parent from
 *         process A.
 *   - Windows (the OS) generates a WM_PARENTNOTIFY message that is sent
 *         synchronously to Process A. Process B blocks until a response is
 *         received from Process A. Process A, however, is blocked and cannot
 *         process the message. Both processes are deadlocked.
 *
 * The example above has a few different workarounds (e.g. setting the
 * WS_EX_NOPARENTNOTIFY style on the child window) but the general problem is
 * persists. Once two HWNDs are parented we must not block their owning
 * threads when manipulating either HWND.
 *
 * Windows requires any application that hosts native HWNDs to always process
 * messages or risk deadlock. Given our architecture the only way to meet
 * Windows' requirement and allow for synchronous IPC messages is to pump a
 * miniature message loop during a sync IPC call. We avoid processing any
 * queued messages during the loop, but "nonqueued" messages (see
 * http://msdn.microsoft.com/en-us/library/ms644927(VS.85).aspx under the
 * section "Nonqueued messages") cannot be avoided. Those messages are trapped
 * in a special window procedure where we can either ignore the message or
 * process it in some fashion.
 */

namespace {

UINT gEventLoopMessage =
    RegisterWindowMessage(L"SyncChannel Windows Message Loop Message");

const wchar_t kOldWndProcProp[] = L"MozillaIPCOldWndProc";

// This isn't defined before Windows XP.
enum { WM_XP_THEMECHANGED = 0x031A };

PRUnichar gAppMessageWindowName[256] = { 0 };
PRInt32 gAppMessageWindowNameLength = 0;

nsTArray<HWND>* gNeuteredWindows = nsnull;

typedef nsTArray<nsAutoPtr<DeferredMessage> > DeferredMessageArray;
DeferredMessageArray* gDeferredMessages = nsnull;

HHOOK gDeferredGetMsgHook = NULL;
HHOOK gDeferredCallWndProcHook = NULL;

DWORD gUIThreadId = 0;
int gEventLoopDepth = 0;

LRESULT CALLBACK
DeferredMessageHook(int nCode,
                    WPARAM wParam,
                    LPARAM lParam)
{
  // XXX This function is called for *both* the WH_CALLWNDPROC hook and the
  //     WH_GETMESSAGE hook, but they have different parameters. We don't
  //     use any of them except nCode which has the same meaning.

  // Only run deferred messages if all of these conditions are met:
  //   1. The |nCode| indicates that this hook should do something.
  //   2. We have deferred messages to run.
  //   3. We're not being called from the PeekMessage within the WaitForNotify
  //      function (indicated with SyncChannel::IsPumpingMessages). We really
  //      only want to run after returning to the main event loop.
  if (nCode >= 0 && gDeferredMessages && !SyncChannel::IsPumpingMessages()) {
    NS_ASSERTION(gDeferredGetMsgHook && gDeferredCallWndProcHook,
                 "These hooks must be set if we're being called!");
    NS_ASSERTION(gDeferredMessages->Length(), "No deferred messages?!");

    // Unset hooks first, in case we reenter below.
    UnhookWindowsHookEx(gDeferredGetMsgHook);
    UnhookWindowsHookEx(gDeferredCallWndProcHook);
    gDeferredGetMsgHook = 0;
    gDeferredCallWndProcHook = 0;

    // Unset the global and make sure we delete it when we're done here.
    nsAutoPtr<DeferredMessageArray> messages(gDeferredMessages);
    gDeferredMessages = nsnull;

    // Run all the deferred messages in order.
    PRUint32 count = messages->Length();
    for (PRUint32 index = 0; index < count; index++) {
      messages->ElementAt(index)->Run();
    }
  }

  // Always call the next hook.
  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

LRESULT
ProcessOrDeferMessage(HWND hwnd,
                      UINT uMsg,
                      WPARAM wParam,
                      LPARAM lParam)
{
  DeferredMessage* deferred = nsnull;

  // Most messages ask for 0 to be returned if the message is processed.
  LRESULT res = 0;

  switch (uMsg) {
    // Messages that can be deferred as-is. These must not contain pointers in
    // their wParam or lParam arguments!
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
    case WM_CHILDACTIVATE:
    case WM_DESTROY:
    case WM_IME_NOTIFY:
    case WM_IME_SETCONTEXT:
    case WM_KILLFOCUS:
    case WM_NCDESTROY:
    case WM_PARENTNOTIFY:
    case WM_SETFOCUS:
    case WM_SHOWWINDOW: // Intentional fall-through.
    case WM_XP_THEMECHANGED: {
      deferred = new DeferredSendMessage(hwnd, uMsg, wParam, lParam);
      break;
    }

    case WM_SETCURSOR: {
      // Friggin unconventional return value...
      res = TRUE;
      deferred = new DeferredSendMessage(hwnd, uMsg, wParam, lParam);
      break;
    }

    case WM_NCACTIVATE: {
      res = TRUE;
      deferred = new DeferredNCActivateMessage(hwnd, uMsg, wParam, lParam);
      break;
    }

    // These messages need to use the RedrawWindow function to generate the
    // right kind of message. We can't simply fake them as the MSDN docs say
    // explicitly that paint messages should not be sent by an application.
    case WM_ERASEBKGND: {
      UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_NOINTERNALPAINT |
                   RDW_NOFRAME | RDW_NOCHILDREN | RDW_ERASENOW;
      deferred = new DeferredRedrawMessage(hwnd, flags);
      break;
    }
    case WM_NCPAINT: {
      UINT flags = RDW_INVALIDATE | RDW_FRAME | RDW_NOINTERNALPAINT |
                   RDW_NOERASE | RDW_NOCHILDREN | RDW_ERASENOW;
      deferred = new DeferredRedrawMessage(hwnd, flags);
      break;
    }

    // This message will generate a WM_PAINT message if there are invalid
    // areas.
    case WM_PAINT: {
      deferred = new DeferredUpdateMessage(hwnd);
      break;
    }

    // This message holds a string in its lParam that we must copy.
    case WM_SETTINGCHANGE: {
      deferred = new DeferredSettingChangeMessage(hwnd, uMsg, wParam, lParam);
      break;
    }

    // These messages are faked via a call to SetWindowPos.
    case WM_WINDOWPOSCHANGED: // Intentional fall-through.
    case WM_WINDOWPOSCHANGING: {
      UINT flags = SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE |
                   SWP_NOOWNERZORDER | SWP_NOZORDER | SWP_DEFERERASE;
      deferred = new DeferredWindowPosMessage(hwnd, flags);
      break;
    }
    case WM_NCCALCSIZE: {
      UINT flags = SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE |
                   SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_NOZORDER |
                   SWP_DEFERERASE | SWP_NOSENDCHANGING;
      deferred = new DeferredWindowPosMessage(hwnd, flags);
      break;
    }

    // Messages that are safe to pass to DefWindowProc go here.
    case WM_GETICON:
    case WM_GETMINMAXINFO:
    case WM_GETTEXT:
    case WM_NCHITTEST:
    case WM_SETICON: // Intentional fall-through.
    case WM_SYNCPAINT: {
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    // Unknown messages only.
    default: {
#ifdef DEBUG
      nsCAutoString log("Received \"nonqueued\" message ");
      log.AppendInt(uMsg);
      log.AppendLiteral(" during a synchronous IPC message for window ");
      log.AppendInt((PRInt64)hwnd);

      wchar_t className[256] = { 0 };
      if (GetClassNameW(hwnd, className, sizeof(className) - 1) > 0) {
        log.AppendLiteral(" (\"");
        log.Append(NS_ConvertUTF16toUTF8((PRUnichar*)className));
        log.AppendLiteral("\")");
      }

      log.AppendLiteral(", sending it to DefWindowProc instead of the normal "
                        "window procedure.");
      NS_ERROR(log.get());
#endif
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  NS_ASSERTION(deferred, "Must have a message here!");

  // Create the deferred message array if it doesn't exist already.
  if (!gDeferredMessages) {
    gDeferredMessages = new nsTArray<nsAutoPtr<DeferredMessage> >(20);
    NS_ASSERTION(gDeferredMessages, "Out of memory!");
  }

  // Save for later. The array takes ownership of |deferred|.
  gDeferredMessages->AppendElement(deferred);
  return res;
}

LRESULT CALLBACK
NeuteredWindowProc(HWND hwnd,
                   UINT uMsg,
                   WPARAM wParam,
                   LPARAM lParam)
{
  WNDPROC oldWndProc = (WNDPROC)GetProp(hwnd, kOldWndProcProp);
  if (!oldWndProc) {
    // We should really never ever get here.
    NS_ERROR("No old wndproc!");
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
  }

  // See if we care about this message. We may either ignore it, send it to
  // DefWindowProc, or defer it for later.
  return ProcessOrDeferMessage(hwnd, uMsg, wParam, lParam);
}

static bool
WindowIsMozillaWindow(HWND hWnd)
{
  if (!IsWindow(hWnd)) {
    NS_WARNING("Window has died!");
    return false;
  }

  PRUnichar buffer[256] = { 0 };
  int length = GetClassNameW(hWnd, (wchar_t*)buffer, sizeof(buffer) - 1);
  if (length <= 0) {
    NS_WARNING("Failed to get class name!");
    return false;
  }

  nsDependentString className(buffer, length);
  if (StringBeginsWith(className, NS_LITERAL_STRING("Mozilla")) ||
      StringBeginsWith(className, NS_LITERAL_STRING("Gecko")) ||
      className.EqualsLiteral("nsToolkitClass") ||
      className.EqualsLiteral("nsAppShell:EventWindowClass")) {
    return true;
  }

  // nsNativeAppSupport makes a window like "FirefoxMessageWindow" based on the
  // toolkit app's name. It's pretty expensive to calculate this so we only try
  // once.
  if (gAppMessageWindowNameLength == 0) {
    nsCOMPtr<nsIXULAppInfo> appInfo =
      do_GetService("@mozilla.org/xre/app-info;1");
    if (appInfo) {
      nsCAutoString appName;
      if (NS_SUCCEEDED(appInfo->GetName(appName))) {
        appName.Append("MessageWindow");
        nsDependentString windowName(gAppMessageWindowName);
        CopyUTF8toUTF16(appName, windowName);
        gAppMessageWindowNameLength = windowName.Length();
      }
    }

    // Don't try again if that failed.
    if (gAppMessageWindowNameLength == 0) {
      gAppMessageWindowNameLength = -1;
    }
  }

  if (gAppMessageWindowNameLength != -1 &&
      className.Equals(nsDependentString(gAppMessageWindowName,
                                         gAppMessageWindowNameLength))) {
    return true;
  }

  return false;
}

bool
NeuterWindowProcedure(HWND hWnd)
{
  if (!WindowIsMozillaWindow(hWnd)) {
    // Some other kind of window, skip.
    return false;
  }

  NS_ASSERTION(!GetProp(hWnd, kOldWndProcProp), "This should always be null!");

  // It's possible to get NULL out of SetWindowLongPtr, and the only way to know
  // if that's a valid old value is to use GetLastError. Clear the error here so
  // we can tell.
  SetLastError(ERROR_SUCCESS);

  LONG_PTR currentWndProc =
    SetWindowLongPtr(hWnd, GWLP_WNDPROC, (LONG_PTR)NeuteredWindowProc);
  if (!currentWndProc) {
    if (ERROR_SUCCESS == GetLastError()) {
      // No error, so we set something and must therefore reset it.
      SetWindowLongPtr(hWnd, GWLP_WNDPROC, currentWndProc);
    }
    return false;
  }

  NS_ASSERTION(currentWndProc != (LONG_PTR)NeuteredWindowProc,
               "This shouldn't be possible!");

  if (!SetProp(hWnd, kOldWndProcProp, (HANDLE)currentWndProc)) {
    // Cleanup
    NS_WARNING("SetProp failed!");
    SetWindowLongPtr(hWnd, GWLP_WNDPROC, currentWndProc);
    RemoveProp(hWnd, kOldWndProcProp);
    return false;
  }

  return true;
}

void
RestoreWindowProcedure(HWND hWnd)
{
  NS_ASSERTION(WindowIsMozillaWindow(hWnd),
               "Not a mozilla window, this shouldn't be in our list!");

  LONG_PTR oldWndProc = (LONG_PTR)RemoveProp(hWnd, kOldWndProcProp);
  if (oldWndProc) {
    NS_ASSERTION(oldWndProc != (LONG_PTR)NeuteredWindowProc,
                 "This shouldn't be possible!");

    LONG_PTR currentWndProc =
      SetWindowLongPtr(hWnd, GWLP_WNDPROC, oldWndProc);
    NS_ASSERTION(currentWndProc == (LONG_PTR)NeuteredWindowProc,
                 "This should never be switched out from under us!");
  }
}

LRESULT CALLBACK
CallWindowProcedureHook(int nCode,
                        WPARAM wParam,
                        LPARAM lParam)
{
  if (nCode >= 0) {
    NS_ASSERTION(gNeuteredWindows, "This should never be null!");

    HWND hWnd = reinterpret_cast<CWPSTRUCT*>(lParam)->hwnd;

    if (!gNeuteredWindows->Contains(hWnd) && NeuterWindowProcedure(hWnd)) {
      if (!gNeuteredWindows->AppendElement(hWnd)) {
        NS_ERROR("Out of memory!");
        RestoreWindowProcedure(hWnd);
      }
    }
  }
  return CallNextHookEx(NULL, nCode, wParam, lParam);
}

inline void
AssertWindowIsNotNeutered(HWND hWnd)
{
#ifdef DEBUG
  // Make sure our neutered window hook isn't still in place.
  LONG_PTR wndproc = GetWindowLongPtr(hWnd, GWLP_WNDPROC);
  NS_ASSERTION(wndproc != (LONG_PTR)NeuteredWindowProc, "Window is neutered!");
#endif
}

} // anonymous namespace

void
SyncChannel::WaitForNotify()
{
  mMutex.AssertCurrentThreadOwns();

  NS_ASSERTION(gEventLoopDepth >= 0, "Event loop depth mismatch!");

  HHOOK windowHook = NULL;

  nsAutoTArray<HWND, 20> neuteredWindows;

  if (++gEventLoopDepth == 1) {
    NS_ASSERTION(!SyncChannel::IsPumpingMessages(),
                 "Shouldn't be pumping already!");
    SyncChannel::SetIsPumpingMessages(true);

    if (!gUIThreadId) {
      gUIThreadId = GetCurrentThreadId();
    }
    NS_ASSERTION(gUIThreadId, "ThreadId should not be 0!");
    NS_ASSERTION(gUIThreadId == GetCurrentThreadId(),
                 "Running on different threads!");

    NS_ASSERTION(!gNeuteredWindows, "Should only set this once!");
    gNeuteredWindows = &neuteredWindows;

    windowHook = SetWindowsHookEx(WH_CALLWNDPROC, CallWindowProcedureHook,
                                  NULL, gUIThreadId);
    NS_ASSERTION(windowHook, "Failed to set hook!");
  }

  {
    MutexAutoUnlock unlock(mMutex);

    while (1) {
      // Wait until we have a message in the queue. MSDN docs are a bit unclear
      // but it seems that windows from two different threads (and it should be
      // noted that a thread in another process counts as a "different thread")
      // will implicitly have their message queues attached if they are parented
      // to one another. This wait call, then, will return for a message
      // delivered to *either* thread.
      DWORD result = MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE,
                                               QS_ALLINPUT);
      if (result != WAIT_OBJECT_0) {
        NS_ERROR("Wait failed!");
        break;
      }

      // The only way to know on which thread the message was delivered is to
      // use some logic on the return values of GetQueueStatus and PeekMessage.
      // PeekMessage will return false if there are no "queued" messages, but it
      // will run all "nonqueued" messages before returning. So if PeekMessage
      // returns false and there are no "nonqueued" messages that were run then
      // we know that the message we woke for was intended for a window on
      // another thread.
      bool haveSentMessagesPending =
        (HIWORD(GetQueueStatus(QS_SENDMESSAGE)) & QS_SENDMESSAGE) != 0;

      // This PeekMessage call will actually process all "nonqueued" messages
      // that are pending before returning. If we have "nonqueued" messages
      // pending then we should have switched out all the window procedures
      // above. In that case this PeekMessage call won't actually cause any
      // mozilla code (or plugin code) to run.

      // We check first to see if we should break out of the loop by looking for
      // the special message from the IO thread. We pull it out of the queue.
      MSG msg = { 0 };
      if (PeekMessageW(&msg, (HWND)-1, gEventLoopMessage, gEventLoopMessage,
                       PM_REMOVE)) {
        break;
      }

      // If the following PeekMessage call fails to return a message for us (and
      // returns false) and we didn't run any "nonqueued" messages then we must
      // have woken up for a message designated for a window in another thread.
      // If we loop immediately then we could enter a tight loop, so we'll give
      // up our time slice here to let the child process its message.
      if (!PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE) &&
          !haveSentMessagesPending) {
        // Message was for child, we should wait a bit.
        SwitchToThread();
      }
    }
  }

  NS_ASSERTION(gEventLoopDepth > 0, "Event loop depth mismatch!");

  if (--gEventLoopDepth == 0) {
    if (windowHook) {
      UnhookWindowsHookEx(windowHook);
    }

    NS_ASSERTION(gNeuteredWindows == &neuteredWindows, "Bad pointer!");
    gNeuteredWindows = nsnull;

    PRUint32 count = neuteredWindows.Length();
    for (PRUint32 index = 0; index < count; index++) {
      RestoreWindowProcedure(neuteredWindows[index]);
    }

    SyncChannel::SetIsPumpingMessages(false);

    // Before returning we need to set a hook to run any deferred messages that
    // we received during the IPC call. The hook will unset itself as soon as
    // someone else calls GetMessage, PeekMessage, or runs code that generates a
    // "nonqueued" message.
    if (gDeferredMessages &&
        !(gDeferredGetMsgHook && gDeferredCallWndProcHook)) {
      NS_ASSERTION(gDeferredMessages->Length(), "No deferred messages?!");

      gDeferredGetMsgHook = SetWindowsHookEx(WH_GETMESSAGE, DeferredMessageHook,
                                             NULL, gUIThreadId);
      gDeferredCallWndProcHook = SetWindowsHookEx(WH_CALLWNDPROC,
                                                  DeferredMessageHook, NULL,
                                                  gUIThreadId);
      NS_ASSERTION(gDeferredGetMsgHook && gDeferredCallWndProcHook,
                   "Failed to set hooks!");
    }
  }
}

void
SyncChannel::NotifyWorkerThread()
{
  mMutex.AssertCurrentThreadOwns();
  NS_ASSERTION(gUIThreadId, "This should have been set already!");
  if (!PostThreadMessage(gUIThreadId, gEventLoopMessage, 0, 0)) {
    NS_WARNING("Failed to post thread message!");
  }
}

void
DeferredSendMessage::Run()
{
  AssertWindowIsNotNeutered(hWnd);
  if (!IsWindow(hWnd)) {
    NS_ERROR("Invalid window!");
    return;
  }

  WNDPROC wndproc =
    reinterpret_cast<WNDPROC>(GetWindowLongPtr(hWnd, GWLP_WNDPROC));
  if (!wndproc) {
    NS_ERROR("Invalid window procedure!");
    return;
  }

  CallWindowProc(wndproc, hWnd, message, wParam, lParam);
}

void
DeferredRedrawMessage::Run()
{
  AssertWindowIsNotNeutered(hWnd);
  if (!IsWindow(hWnd)) {
    NS_ERROR("Invalid window!");
    return;
  }

#ifdef DEBUG
  BOOL ret =
#endif
  RedrawWindow(hWnd, NULL, NULL, flags);
  NS_ASSERTION(ret, "RedrawWindow failed!");
}

void
DeferredUpdateMessage::Run()
{
  AssertWindowIsNotNeutered(hWnd);
  if (!IsWindow(hWnd)) {
    NS_ERROR("Invalid window!");
    return;
  }

#ifdef DEBUG
  BOOL ret =
#endif
  UpdateWindow(hWnd);
  NS_ASSERTION(ret, "UpdateWindow failed!");
}

DeferredSettingChangeMessage::DeferredSettingChangeMessage(HWND aHWnd,
                                                           UINT aMessage,
                                                           WPARAM aWParam,
                                                           LPARAM aLParam)
: DeferredSendMessage(aHWnd, aMessage, aWParam, aLParam)
{
  NS_ASSERTION(aMessage == WM_SETTINGCHANGE, "Wrong message type!");
  if (aLParam) {
    lParamString = _wcsdup(reinterpret_cast<const wchar_t*>(aLParam));
    lParam = reinterpret_cast<LPARAM>(lParamString);
  }
  else {
    lParam = NULL;
  }
}

DeferredSettingChangeMessage::~DeferredSettingChangeMessage()
{
  if (lParamString) {
    free(lParamString);
  }
}

void
DeferredWindowPosMessage::Run()
{
  AssertWindowIsNotNeutered(hWnd);
  if (!IsWindow(hWnd)) {
    NS_ERROR("Invalid window!");
    return;
  }

#ifdef DEBUG
  BOOL ret = 
#endif
  SetWindowPos(hWnd, 0, 0, 0, 0, 0, flags);
  NS_ASSERTION(ret, "SetWindowPos failed!");
}

DeferredNCActivateMessage::DeferredNCActivateMessage(HWND aHWnd,
                                                     UINT aMessage,
                                                     WPARAM aWParam,
                                                     LPARAM aLParam)
: DeferredSendMessage(aHWnd, aMessage, aWParam, aLParam),
  region(NULL)
{
  NS_ASSERTION(aMessage == WM_NCACTIVATE, "Wrong message!");
  if (aLParam) {
    // This is a window that doesn't have a visual style and so lParam is a
    // handle to an update region. We need to duplicate it.
    HRGN source = reinterpret_cast<HRGN>(aLParam);

    DWORD dataSize = GetRegionData(source, 0, NULL);
    if (!dataSize) {
      NS_ERROR("GetRegionData failed!");
      return;
    }

    nsAutoArrayPtr<char> buffer = new char[dataSize];
    NS_ASSERTION(buffer, "Out of memory!");

    RGNDATA* data = reinterpret_cast<RGNDATA*>(buffer.get());

    dataSize = GetRegionData(source, dataSize, data);
    if (!dataSize) {
      NS_ERROR("GetRegionData failed!");
      return;
    }

    HRGN tempRegion = ExtCreateRegion(NULL, dataSize, data);
    if (!tempRegion) {
      NS_ERROR("ExtCreateRegion failed!");
      return;
    }

    region = tempRegion;
    lParam = reinterpret_cast<LPARAM>(region);
  }
}

DeferredNCActivateMessage::~DeferredNCActivateMessage()
{
  if (region) {
    DeleteObject(region);
  }
}