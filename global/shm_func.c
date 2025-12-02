// shm_func.c — Windows(Local\ namespace) / Linux(SysV) shared memory
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _WIN32
    #include <sys/types.h>
    #include <sys/ipc.h>
    #include <sys/shm.h>
#else
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#endif

#include "global.h"
#include "shm_type.h"

/* ===== Global pointers (project original) ===== */
SHM_PROC_DATA       *proc_shm_ptr   = NULL;
SHM_SYSTEM_SET      *system_set_ptr = NULL;
VMS_COMMAND_DATA    *vms_command_ptr = NULL;
CONNECTION_STATUS   *connection_status_ptr = NULL;
MESSAGEDATA	        *message_data_ptr = NULL;
LISTINFO	        *list_info_ptr = NULL;

/* Linux shm ids (Windows uses handles instead) */
int shmid_proc       = -1;
int shmid_system_set = -1;
int shmid_command_set  = -1;
int shmid_connection_set = -1;
int shmid_message_data = -1;
int shmid_list_info    = -1;

#ifdef _WIN32
static HANDLE hMapProc   = NULL;
static HANDLE hMapSystem = NULL;
static HANDLE hMapDevstatus = NULL;
static HANDLE hMapDrnstatus = NULL;
static HANDLE hMapLbcnstatus = NULL;

/* Helper: last error print */
static void win_perror(const char* msg, DWORD err){
    fprintf(stderr, "%s (%lu)\n", msg, (unsigned long)err);
}

/* Try open/create mapping with fallbacks:
    1) Open Global\NAME   (may require admin; will likely fail with ERROR_ACCESS_DENIED)
    2) Open Local\NAME
    3) Open NAME
    4) Create Local\NAME
    5) Create NAME
*/
static HANDLE open_or_create_map(const wchar_t* baseName, DWORD sizeBytes){
    HANDLE h = NULL;
    DWORD err;

    wchar_t gname[64], lname[64];
    _snwprintf(gname, 64, L"Global\\%s", baseName);
    _snwprintf(lname, 64, L"Local\\%s",  baseName);

    /* try open existing first */
    h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, gname);
    if (h) return h;
    h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, lname);
    if (h) return h;
    h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, baseName);
    if (h) return h;

    /* create Local\ first (no admin) */
    h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                            0, sizeBytes, lname);
    if (h) return h;

    err = GetLastError();
    if (err == ERROR_ACCESS_DENIED){
        /* fallback: try unprefixed */
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                0, sizeBytes, baseName);
        if (h) return h;
        err = GetLastError();
    }
    /* last resort: try Global\ (may still fail without privilege) */
    if (!h){
        h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                0, sizeBytes, gname);
        if (h) return h;
        win_perror("CreateFileMapping failed", GetLastError());
    }
    return NULL;
}
#endif

/* ---------------- SysV-like wrapper ---------------- */

int shm_create(key_t shm_key, long int shm_size)
{
#ifndef _WIN32
    int shmid = shmget((key_t)shm_key, shm_size, 0666 | IPC_CREAT);
    if (shmid == -1) {
        perror("shmget failed");
        return -1;
    }
    return shmid;
#else
    (void)shm_key; (void)shm_size;
    return 1; /* Windows path doesn't use this id */
#endif
}

BOOL shm_all_create(void)
{
#ifndef _WIN32
    shmid_proc = shm_create(SHM_KEY_PROCESS, sizeof(SHM_PROC_DATA));
    if (shmid_proc == -1) return false;

    shmid_system_set = shm_create(SHM_KEY_SYSTEM_SET, sizeof(SHM_SYSTEM_SET));
    if (shmid_system_set == -1) return false;

    shmid_command_set = shm_create(SHM_KEY_SYSTEM_SET, sizeof(VMS_COMMAND_DATA));
    if (shmid_command_set == -1) return false;

    shmid_connection_set = shm_create(SHM_KEY_SYSTEM_SET, sizeof(CONNECTION_STATUS));
    if (shmid_connection_set == -1) return false;

    shmid_message_data = shm_create(SHM_KEY_MESSAGEDATA, sizeof(MESSAGEDATA));
    if (shmid_message_data == -1) return false;

    shmid_list_info = shm_create(SHM_KEY_LISTINFO, sizeof(LISTINFO));
    if (shmid_list_info == -1) return false;

    return true;
#else
    if (!hMapProc){
        hMapProc = open_or_create_map(L"SHM_PROCESS", sizeof(SHM_PROC_DATA));
        if (!hMapProc){ win_perror("shm_all_create(): CreateFileMapping PROCESS failed", GetLastError()); return false; }
    }
    if (!hMapSystem){
        hMapSystem = open_or_create_map(L"SHM_SYSTEM_SET", sizeof(SHM_SYSTEM_SET));
        if (!hMapSystem){ win_perror("shm_all_create(): CreateFileMapping SYSTEM failed", GetLastError()); return false; }
    }
    if (!hMapDevstatus){
        hMapDevstatus = open_or_create_map(L"SHM_DEV_STATUS", sizeof(SHM_DEV_STATUS));
        if (!hMapDevstatus){ win_perror("shm_all_create(): CreateFileMapping DevStatus failed", GetLastError()); return false; }
    }
    if (!hMapDrnstatus){
        hMapDrnstatus = open_or_create_map(L"SHM_DRN_STATUS", sizeof(SHM_DRN_STATUS));
        if (!hMapDrnstatus){ win_perror("shm_all_create(): CreateFileMapping DrnStatus failed", GetLastError()); return false; }
    }
    if (!hMapLbcnstatus){
        hMapLbcnstatus = open_or_create_map(L"SHM_LBCN_STATUS", sizeof(SHM_LBCN_STATUS));
        if (!hMapLbcnstatus){ win_perror("shm_all_create(): CreateFileMapping LbcnStatus failed", GetLastError()); return false; }
    }
    shmid_proc = 1; shmid_system_set = 2;
    return true;
#endif
}

BOOL shm_open(int shmid, int type)
{
    (void)shmid;
#ifndef _WIN32
    void *shared_memory = shmat(shmid, NULL, 0);
    if (shared_memory == (void *)-1) {
        perror("shmat failed");
        return false;
    }
    if (type == SHMID_PROCESS_DATA)      proc_shm_ptr   = (SHM_PROC_DATA*)shared_memory;
    else if (type == SHMID_SYSTEM_SET)   system_set_ptr = (SHM_SYSTEM_SET*)shared_memory;
    else if (type == SHMID_COMMAND_SET)   vms_command_ptr = (VMS_COMMAND_DATA*)shared_memory;
    else if (type == SHMID_CONNECTION_SET)   connection_status_ptr = (CONNECTION_STATUS*)shared_memory;
    else if (type == SHMID_MESSAGEDATA)  message_data_ptr = (MESSAGEDATA*)shared_memory;
    else if (type == SHMID_LISTINFO)     list_info_ptr = (LISTINFO*)shared_memory;
    else return false;
    return true;
#else
    void* p = NULL;
    if (type == SHMID_PROCESS_DATA){
        if (!hMapProc){
            /* open or create on demand */
            hMapProc = open_or_create_map(L"SHM_PROCESS", sizeof(SHM_PROC_DATA));
            if (!hMapProc){ win_perror("shm_open(PROCESS): Create/Open failed", GetLastError()); return false; }
        }
        p = MapViewOfFile(hMapProc, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_PROC_DATA));
        if (!p){ win_perror("MapViewOfFile(PROCESS) failed", GetLastError()); return false; }
        proc_shm_ptr = (SHM_PROC_DATA*)p;
        return true;
    } else if (type == SHMID_SYSTEM_SET){
        if (!hMapSystem){
            hMapSystem = open_or_create_map(L"SHM_SYSTEM_SET", sizeof(SHM_SYSTEM_SET));
            if (!hMapSystem){ win_perror("shm_open(SYSTEM): Create/Open failed", GetLastError()); return false; }
        }
        p = MapViewOfFile(hMapSystem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_SYSTEM_SET));
        if (!p){ win_perror("MapViewOfFile(SYSTEM) failed", GetLastError()); return false; }
        system_set_ptr = (SHM_SYSTEM_SET*)p;
        return true;
    } else if (type == SHMID_DEVSTATUS_SET){
        if (!hMapDevstatus){
            hMapDevstatus = open_or_create_map(L"SHM_DEV_STATUS", sizeof(SHM_DEV_STATUS));
            if (!hMapDevstatus){ win_perror("shm_open(SYSTEM): Create/Open failed", GetLastError()); return false; }
        }
        p = MapViewOfFile(hMapDevstatus, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_DEV_STATUS));
        if (!p){ win_perror("MapViewOfFile(SYSTEM) failed", GetLastError()); return false; }
        dev_status_ptr = (SHM_DEV_STATUS*)p;
        return true;
    } else if (type == SHMID_DRNSTATUS_SET){
        if (!hMapDrnstatus){
            hMapDrnstatus = open_or_create_map(L"SHM_DRN_STATUS", sizeof(SHM_DRN_STATUS));
            if (!hMapDrnstatus){ win_perror("shm_open(SYSTEM): Create/Open failed", GetLastError()); return false; }
        }
        p = MapViewOfFile(hMapDrnstatus, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_DRN_STATUS));
        if (!p){ win_perror("MapViewOfFile(SYSTEM) failed", GetLastError()); return false; }
        drn_status_ptr = (SHM_DRN_STATUS*)p;
        return true;
    } else if (type == SHMID_LBCNSTATUS_SET){
        if (!hMapLbcnstatus){
            hMapLbcnstatus = open_or_create_map(L"SHM_LBCN_STATUS", sizeof(SHM_LBCN_STATUS));
            if (!hMapLbcnstatus){ win_perror("shm_open(SYSTEM): Create/Open failed", GetLastError()); return false; }
        }
        p = MapViewOfFile(hMapLbcnstatus, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SHM_LBCN_STATUS));
        if (!p){ win_perror("MapViewOfFile(SYSTEM) failed", GetLastError()); return false; }
        lbcn_status_ptr = (SHM_LBCN_STATUS*)p;
        return true;
    }
    return false;
#endif
}

BOOL shm_all_open(void)
{
    if (shmid_proc == -1) 
        shmid_proc = shmget(SHM_KEY_PROCESS, sizeof(SHM_PROC_DATA), 0666);
    if (shmid_system_set == -1) 
        shmid_system_set = shmget(SHM_KEY_SYSTEM_SET, sizeof(SHM_SYSTEM_SET), 0666);
    if (shmid_command_set == -1) 
        shmid_command_set = shmget(SHM_KEY_SYSTEM_SET, sizeof(VMS_COMMAND_DATA), 0666);
    if (shmid_connection_set == -1) 
        shmid_connection_set = shmget(SHM_KEY_SYSTEM_SET, sizeof(CONNECTION_STATUS), 0666);
    if (shmid_message_data == -1)
        shmid_message_data = shmget(SHM_KEY_MESSAGEDATA, sizeof(MESSAGEDATA), 0666);
    if (shmid_list_info == -1)
        shmid_list_info = shmget(SHM_KEY_LISTINFO, sizeof(LISTINFO), 0666);

    // ID를 찾았는지 확인 (여전히 -1이면 실패)
    if (shmid_proc == -1 || shmid_system_set == -1 || 
        shmid_message_data == -1 || shmid_list_info == -1) {
        printf("shm_all_open] Failed to get shm IDs via shmget.\n");
        return FALSE;
    }

    BOOL ok1 = shm_open(shmid_proc,       SHMID_PROCESS_DATA);
    BOOL ok2 = shm_open(shmid_system_set, SHMID_SYSTEM_SET);
    BOOL ok3 = shm_open(shmid_message_data, SHMID_MESSAGEDATA);
    BOOL ok4 = shm_open(shmid_list_info, SHMID_LISTINFO);
    return (ok1 && ok2 && ok3 && ok4);
}

int shm_close(void)
{
    int nReturn = -SHM_MAX_COUNT;
#ifndef _WIN32
    if (proc_shm_ptr   && shmdt(proc_shm_ptr)   != -1){ proc_shm_ptr   = NULL; nReturn++; }
    if (system_set_ptr && shmdt(system_set_ptr) != -1){ system_set_ptr = NULL; nReturn++; }
    if (message_data_ptr && shmdt(message_data_ptr) != -1){ message_data_ptr = NULL; nReturn++; }
    if (list_info_ptr && shmdt(list_info_ptr) != -1){ list_info_ptr = NULL; nReturn++; }
    return nReturn;
#else
    if (proc_shm_ptr)   { UnmapViewOfFile(proc_shm_ptr);   proc_shm_ptr   = NULL; nReturn++; }
    if (system_set_ptr) { UnmapViewOfFile(system_set_ptr); system_set_ptr = NULL; nReturn++; }
    if (dev_status_ptr) { UnmapViewOfFile(dev_status_ptr); dev_status_ptr = NULL; nReturn++; }
    if (drn_status_ptr) { UnmapViewOfFile(drn_status_ptr); drn_status_ptr = NULL; nReturn++; }
    if (lbcn_status_ptr) { UnmapViewOfFile(lbcn_status_ptr); lbcn_status_ptr = NULL; nReturn++; }
    return nReturn;
#endif
}

int shm_delete(void)
{
    int nReturn = -SHM_MAX_COUNT;
#ifndef _WIN32
    if (shmid_proc       != -1 && shmctl(shmid_proc,       IPC_RMID, NULL) != -1) nReturn++;
    if (shmid_system_set != -1 && shmctl(shmid_system_set, IPC_RMID, NULL) != -1) nReturn++;
    if (shmid_message_data != -1 && shmctl(shmid_message_data, IPC_RMID, NULL) != -1) nReturn++;
    if (shmid_list_info != -1 && shmctl(shmid_list_info, IPC_RMID, NULL) != -1) nReturn++;
    return nReturn;
#else
    if (proc_shm_ptr)   { UnmapViewOfFile(proc_shm_ptr);   proc_shm_ptr   = NULL; nReturn++; }
    if (system_set_ptr) { UnmapViewOfFile(system_set_ptr); system_set_ptr = NULL; nReturn++; }
    if (dev_status_ptr) { UnmapViewOfFile(dev_status_ptr); dev_status_ptr = NULL; nReturn++; }
    if (drn_status_ptr) { UnmapViewOfFile(drn_status_ptr); drn_status_ptr = NULL; nReturn++; }
    if (lbcn_status_ptr) { UnmapViewOfFile(lbcn_status_ptr); lbcn_status_ptr = NULL; nReturn++; }
    if (hMapProc)   { CloseHandle(hMapProc);   hMapProc   = NULL; }
    if (hMapSystem) { CloseHandle(hMapSystem); hMapSystem = NULL; }
    if (hMapDevstatus) { CloseHandle(hMapDevstatus); hMapDevstatus = NULL; }
    if (hMapDrnstatus) { CloseHandle(hMapDrnstatus); hMapDrnstatus = NULL; }
    if (hMapLbcnstatus) { CloseHandle(hMapLbcnstatus); hMapLbcnstatus = NULL; }
    return nReturn;
#endif
}

/* Optional no-op (Linux impl project-specific) */
void shm_erase(int shmid){ (void)shmid; }
