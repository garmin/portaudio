
#include "pa_util.h"
#include "pa_debugprint.h"

#include "pa_win_wdmks_utils.h"

#include <windows.h>
#include <dbt.h>
#include <process.h>
#include <assert.h>
#include <ks.h>
#include <ksmedia.h>

#include <stdio.h>

/* Implemented in pa_front.c */
extern void PaUtil_DevicesChanged();

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define PA_THREAD_FUNC static unsigned WINAPI
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define PA_THREAD_FUNC static DWORD WINAPI
#endif

typedef struct PaHotPlugDeviceEventHandlerInfo
{
    HANDLE  hWnd;
    HANDLE  hMsgThread;
    HANDLE  hNotify;
} PaHotPlugDeviceEventHandlerInfo;

static BOOL IsDeviceAudio(const TCHAR* deviceName)
{
#if 0
    /* This code below will make sure that only AUDIO devices insertion is notified (MIDI devices
       "unfortunately" have the KSCATEGORY_AUDIO also...)

       There is a problem with removal though, since a removed device cannot be queried
       for its channel count... */
    int channelCnt = 0;

#ifdef UNICODE
    const wchar_t* name = deviceName;
#else
    wchar_t name[MAX_PATH];
    mbstowcs(name, deviceName, MAX_PATH-1);
#endif
    channelCnt |= PaWin_WDMKS_QueryFilterMaximumChannelCount(name, 1);
    channelCnt |= PaWin_WDMKS_QueryFilterMaximumChannelCount(name, 0);

    return (channelCnt != 0);
#else
    return TRUE;
#endif
}

static LRESULT CALLBACK sWinProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg)
    {
    case WM_QUERYENDSESSION:
    case WM_POWERBROADCAST:
        return TRUE;

    case WM_CLOSE:
        PostMessage(hWnd, WM_QUIT, 0, 0);
        break;

    case WM_DEVICECHANGE:
        switch(wParam)
        {
        case DBT_DEVICEARRIVAL:
            {
                PDEV_BROADCAST_DEVICEINTERFACE ptr = (PDEV_BROADCAST_DEVICEINTERFACE)lParam;
                if (ptr->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                    break;

                if (!IsEqualGUID(&ptr->dbcc_classguid, &KSCATEGORY_AUDIO))
                    break;

                if (IsDeviceAudio(ptr->dbcc_name))
                {
                    PA_DEBUG(("Device inserted: %s\n", ptr->dbcc_name));
                    PaUtil_DevicesChanged();
                }
            }
            break;
        case DBT_DEVICEREMOVECOMPLETE:
            {
                PDEV_BROADCAST_DEVICEINTERFACE ptr = (PDEV_BROADCAST_DEVICEINTERFACE)lParam;
                if (ptr->dbcc_devicetype != DBT_DEVTYP_DEVICEINTERFACE)
                    break;

                if (!IsEqualGUID(&ptr->dbcc_classguid, &KSCATEGORY_AUDIO))
                    break;

                if (IsDeviceAudio(ptr->dbcc_name))
                {
                    PA_DEBUG(("Device removed : %s\n", ptr->dbcc_name));
                    PaUtil_DevicesChanged();
                }
            }
            break;
        default:
            break;
        }
        break;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

PA_THREAD_FUNC PaRunMessageLoop(void* ptr)
{
    PaHotPlugDeviceEventHandlerInfo* pInfo = (PaHotPlugDeviceEventHandlerInfo*)ptr;
    WNDCLASSA wnd = { 0 };
    HMODULE hInstance = GetModuleHandleA(NULL);

    wnd.lpfnWndProc = sWinProc;
    wnd.hInstance = hInstance;
    wnd.lpszClassName = "{1E0D4F5A-B31F-4dcc-AE3C-4F30A47BD521}";   /* Using a GUID as class name */
    /* Need a top level invisible window in order to receive OS broadcast messages */
    pInfo->hWnd = CreateWindowA(MAKEINTATOM(RegisterClassA(&wnd)), "window", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (pInfo->hWnd)
    {
        // Media GUID = {4d36e96c-e325-11ce-bfc1-08002be10318}
        const GUID WceusbshGUID = { 0x04d36e96c, 0xe325, 0x11ce, 0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18 };
        DEV_BROADCAST_DEVICEINTERFACE_A NotificationFilter = { sizeof(DEV_BROADCAST_DEVICEINTERFACE) };
        NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        NotificationFilter.dbcc_classguid = WceusbshGUID;

        pInfo->hNotify = RegisterDeviceNotificationA( 
            pInfo->hWnd,                
            &NotificationFilter,        
            DEVICE_NOTIFY_WINDOW_HANDLE|DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
            );

        assert(pInfo->hNotify);

        if (pInfo->hNotify)
        {
            MSG msg; 
            BOOL result;
            while((result = GetMessageA(&msg, pInfo->hWnd, 0, 0)) != 0) 
            { 
                if (result == -1)
                {
                    break;
                }
                TranslateMessage(&msg); 
                DispatchMessageA(&msg); 
            } 
            UnregisterDeviceNotification(pInfo->hNotify);
            pInfo->hNotify = 0;
        }
        DestroyWindow(pInfo->hWnd);
        pInfo->hWnd = 0;
    }
    return 0;
}

static PaHotPlugDeviceEventHandlerInfo* s_handler = 0;

void PaUtil_InitializeHotPlug()
{
    if (s_handler == 0)
    {
        s_handler = (PaHotPlugDeviceEventHandlerInfo*)PaUtil_AllocateMemory(sizeof(PaHotPlugDeviceEventHandlerInfo));
        if (s_handler)
        {
            /* Start message thread */
            s_handler->hMsgThread = CREATE_THREAD_FUNCTION(NULL, 0, PaRunMessageLoop, s_handler, 0, NULL);
            assert(s_handler->hMsgThread != 0);
        }
    }
}

void PaUtil_TerminateHotPlug()
{
    if (s_handler != 0)
    {
        if (s_handler->hWnd)
        {
            PostMessage(s_handler->hWnd, WM_QUIT, 0, 0);
            if (WaitForSingleObject(s_handler->hMsgThread, 1000) == WAIT_TIMEOUT)
            {
                TerminateThread(s_handler->hMsgThread, -1);
            }
        }
        PaUtil_FreeMemory( s_handler );
        s_handler = 0;
    }
}
