
#include "pa_util.h"
#include "pa_debugprint.h"
#include "pa_allocation.h"

#include "pa_win_wdmks_utils.h"

#include <windows.h>
#include <dbt.h>
#include <process.h>
#include <assert.h>
#include <ks.h>
#include <ksmedia.h>

#include <setupapi.h>

#include <stdio.h>

/* Implemented in pa_front.c 
  @param first  0 = unknown, 1 = insertion, 2 = removal
  @param second Host specific device change info (in windows it is the device path)
*/
extern void PaUtil_DevicesChanged(unsigned, void*);

/* use CreateThread for CYGWIN/Windows Mobile, _beginthreadex for all others */
#if !defined(__CYGWIN__) && !defined(_WIN32_WCE)
#define CREATE_THREAD_FUNCTION (HANDLE)_beginthreadex
#define PA_THREAD_FUNC static unsigned WINAPI
#else
#define CREATE_THREAD_FUNCTION CreateThread
#define PA_THREAD_FUNC static DWORD WINAPI
#endif

typedef struct PaHotPlugDeviceInfo
{
    TCHAR   name[MAX_PATH];
    struct PaHotPlugDeviceInfo* next;
} PaHotPlugDeviceInfo;

typedef struct PaHotPlugDeviceEventHandlerInfo
{
    HANDLE  hWnd;
    HANDLE  hMsgThread;
    HANDLE  hNotify;
    CRITICAL_SECTION lock;
    PaUtilAllocationGroup* cacheAllocGroup;
    PaHotPlugDeviceInfo* cache;

} PaHotPlugDeviceEventHandlerInfo;

static BOOL RemoveDeviceFromCache(PaHotPlugDeviceEventHandlerInfo* pInfo, const TCHAR* name)
{
    if (pInfo->cache != NULL)
    {
        PaHotPlugDeviceInfo* lastEntry = 0;
        PaHotPlugDeviceInfo* entry = pInfo->cache;
        while (entry != NULL)
        {
            if (_stricmp(entry->name, name) == 0)
            {
                if (lastEntry)
                {
                    lastEntry->next = entry->next;
                }
                else
                {
                    pInfo->cache = NULL;
                }
                PaUtil_GroupFreeMemory(pInfo->cacheAllocGroup, entry);
                return TRUE;
            }

            lastEntry = entry;
            entry = entry->next;
        }
    }
    return FALSE;
}

static void InsertDeviceIntoCache(PaHotPlugDeviceEventHandlerInfo* pInfo, const TCHAR* name)
{
    PaHotPlugDeviceInfo** ppEntry = NULL;

    /* Remove it first (if possible) so we don't accidentally get duplicates */
    RemoveDeviceFromCache(pInfo, name);

    if (pInfo->cache == NULL)
    {
        ppEntry = &pInfo->cache;
    }
    else
    {
        PaHotPlugDeviceInfo* entry = pInfo->cache;
        while (entry->next != NULL)
        {
            entry = entry->next;
        }
        ppEntry = &entry->next;
    }

    *ppEntry = (PaHotPlugDeviceInfo*)PaUtil_GroupAllocateMemory(pInfo->cacheAllocGroup, sizeof(PaHotPlugDeviceInfo));
#ifdef UNICODE
    wcsncpy((*ppEntry)->name, name, MAX_PATH-1);
#else
    strncpy((*ppEntry)->name, name, MAX_PATH-1);
#endif
    (*ppEntry)->next = NULL;
}

static BOOL IsDeviceAudio(const TCHAR* deviceName)
{
    int channelCnt = 0;

#ifdef UNICODE
    const wchar_t* name = deviceName;
#else
    wchar_t name[MAX_PATH];
    mbstowcs(name, deviceName, MAX_PATH-1);
#endif
    channelCnt += PaWin_WDMKS_QueryFilterMaximumChannelCount(name, 1);
    channelCnt += PaWin_WDMKS_QueryFilterMaximumChannelCount(name, 0);

    return (channelCnt > 0);
}

static void PopulateCacheWithAvailableAudioDevices(PaHotPlugDeviceEventHandlerInfo* pInfo)
{
    HDEVINFO handle = NULL;
    const int sizeInterface = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA) + (MAX_PATH * sizeof(WCHAR));
    SP_DEVICE_INTERFACE_DETAIL_DATA* devInterfaceDetails = (SP_DEVICE_INTERFACE_DETAIL_DATA*)PaUtil_AllocateMemory(sizeInterface);

    if (devInterfaceDetails)
    {
        GUID* category_audio = (GUID*)&KSCATEGORY_AUDIO;
        devInterfaceDetails->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        /* Open a handle to search for devices (filters) */
        handle = SetupDiGetClassDevs(category_audio,NULL,NULL,DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if( handle != NULL )
        {
            int device;
            SP_DEVICE_INTERFACE_DATA interfaceData;
            SP_DEVICE_INTERFACE_DATA aliasData;
            SP_DEVINFO_DATA devInfoData;
            int noError;

            /* Iterate through the devices */
            for( device = 0;;device++ )
            {
                interfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
                interfaceData.Reserved = 0;
                aliasData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
                aliasData.Reserved = 0;
                devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);
                devInfoData.Reserved = 0;

                noError = SetupDiEnumDeviceInterfaces(handle,NULL,category_audio,device,&interfaceData);
                if( !noError )
                    break; /* No more devices */

                noError = SetupDiGetDeviceInterfaceDetail(handle,&interfaceData,devInterfaceDetails,sizeInterface,NULL,&devInfoData);
                if( noError )
                {
                    if (IsDeviceAudio(devInterfaceDetails->DevicePath))
                    {
                        PA_DEBUG(("Hotplug cache populated with: '%s'\n", devInterfaceDetails->DevicePath));
                        InsertDeviceIntoCache(pInfo, devInterfaceDetails->DevicePath);
                    }
                }
            }
            SetupDiDestroyDeviceInfoList(handle);
        }

        PaUtil_FreeMemory(devInterfaceDetails);
    }

}


static LRESULT CALLBACK PaMsgWinProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    PaHotPlugDeviceEventHandlerInfo* pInfo = (PaHotPlugDeviceEventHandlerInfo*)( GetWindowLongPtr(hWnd, GWLP_USERDATA) );
    switch(msg)
    {
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
                    InsertDeviceIntoCache(pInfo, ptr->dbcc_name);
                    PaUtil_DevicesChanged(1, ptr->dbcc_name);
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

                if (RemoveDeviceFromCache(pInfo, ptr->dbcc_name))
                {
                    PA_DEBUG(("Device removed : %s\n", ptr->dbcc_name));
                    PaUtil_DevicesChanged(2, ptr->dbcc_name);
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

    wnd.lpfnWndProc = PaMsgWinProc;
    wnd.hInstance = hInstance;
    wnd.lpszClassName = "{1E0D4F5A-B31F-4dcc-AE3C-4F30A47BD521}";   /* Using a GUID as class name */
    pInfo->hWnd = CreateWindowA(MAKEINTATOM(RegisterClassA(&wnd)), NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);
    if (pInfo->hWnd)
    {
        DEV_BROADCAST_DEVICEINTERFACE_A NotificationFilter = { sizeof(DEV_BROADCAST_DEVICEINTERFACE) };
        NotificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;

#ifndef DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
#define DEVICE_NOTIFY_ALL_INTERFACE_CLASSES  0x00000004
#endif

        pInfo->hNotify = RegisterDeviceNotificationA( 
            pInfo->hWnd,                
            &NotificationFilter,        
            DEVICE_NOTIFY_WINDOW_HANDLE|DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
            );

        assert(pInfo->hNotify);

        SetWindowLongPtr(pInfo->hWnd, GWLP_USERDATA, (LONG_PTR)pInfo);

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
            s_handler->cacheAllocGroup = PaUtil_CreateAllocationGroup();
            InitializeCriticalSection(&s_handler->lock);
            PopulateCacheWithAvailableAudioDevices(s_handler);
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
        DeleteCriticalSection(&s_handler->lock);
        PaUtil_FreeAllAllocations( s_handler->cacheAllocGroup );
        PaUtil_DestroyAllocationGroup( s_handler->cacheAllocGroup );
        PaUtil_FreeMemory( s_handler );
        s_handler = 0;
    }
}

void PaUtil_LockHotPlug()
{
    EnterCriticalSection(&s_handler->lock);
}

void PaUtil_UnlockHotPlug()
{
    LeaveCriticalSection(&s_handler->lock);
}
