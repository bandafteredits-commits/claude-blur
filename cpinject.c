/*
 * cpinject.exe - small helper started by CheesyProxxy when the motion blur is switched on.
 *  1. creates C:\Users\Public\CheesyProxxy and lets Microsoft Store apps (Minecraft Bedrock) read it
 *  2. copies motionblur.dll (from the folder next to this exe) into that folder
 *  3. loads the DLL into Minecraft.Windows.exe (LoadLibraryW in the game process)
 * Exit code: 0 = injected / already loaded, 1 = Minecraft is not running, 2 = failed.
 *
 * Build (MinGW-w64):  x86_64-w64-mingw32-gcc -O2 -mwindows -o cpinject.exe cpinject.c -ladvapi32
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <aclapi.h>
#include <sddl.h>

#define DIR_PATH L"C:\\Users\\Public\\CheesyProxxy"
#define DLL_PATH L"C:\\Users\\Public\\CheesyProxxy\\motionblur.dll"
#define CFG_PATH L"C:\\Users\\Public\\CheesyProxxy\\motionblur.cfg"

static BOOL allow_store_apps(const WCHAR* path) {
    PSID sid = NULL;
    if (!ConvertStringSidToSidW(L"S-1-15-2-1", &sid)) return FALSE; /* ALL APPLICATION PACKAGES */
    EXPLICIT_ACCESSW ea = {0};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    PACL oldAcl = NULL, newAcl = NULL; PSECURITY_DESCRIPTOR sd = NULL;
    BOOL ok = FALSE;
    if (GetNamedSecurityInfoW(path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &oldAcl, NULL, &sd) == ERROR_SUCCESS &&
        SetEntriesInAclW(1, &ea, oldAcl, &newAcl) == ERROR_SUCCESS &&
        SetNamedSecurityInfoW((LPWSTR)path, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, newAcl, NULL) == ERROR_SUCCESS)
        ok = TRUE;
    if (newAcl) LocalFree(newAcl);
    if (sd) LocalFree(sd);
    LocalFree(sid);
    return ok;
}

static DWORD find_pid(const WCHAR* name) {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e; e.dwSize = sizeof e;
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
        if (lstrcmpiW(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(s);
    return pid;
}

static BOOL already_loaded(DWORD pid) {
    BOOL found = FALSE;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return FALSE;
    MODULEENTRY32W m; m.dwSize = sizeof m;
    for (BOOL ok = Module32FirstW(s, &m); ok; ok = Module32NextW(s, &m))
        if (lstrcmpiW(m.szModule, L"motionblur.dll") == 0) { found = TRUE; break; }
    CloseHandle(s);
    return found;
}

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cmd, int show) {
    (void)h; (void)p; (void)cmd; (void)show;
    CreateDirectoryW(DIR_PATH, NULL);
    allow_store_apps(DIR_PATH);

    /* settings file: create with defaults if missing (inherits the folder's permissions) */
    if (GetFileAttributesW(CFG_PATH) == INVALID_FILE_ATTRIBUTES) {
        DWORD def[5] = { 0x4243504D, 0, 3, 60, 1 }, n;
        HANDLE f = CreateFileW(CFG_PATH, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, def, sizeof def, &n, NULL); CloseHandle(f); }
    }

    /* copy motionblur.dll that sits next to this exe */
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    for (int i = lstrlenW(self); i > 0; i--) if (self[i - 1] == L'\\') { self[i] = 0; break; }
    WCHAR src[MAX_PATH + 32];
    lstrcpyW(src, self); lstrcatW(src, L"motionblur.dll");
    if (!CopyFileW(src, DLL_PATH, FALSE) && GetFileAttributesW(DLL_PATH) == INVALID_FILE_ATTRIBUTES) return 2;
    allow_store_apps(DLL_PATH);

    DWORD pid = find_pid(L"Minecraft.Windows.exe");
    if (!pid) return 1;
    if (already_loaded(pid)) return 0;

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                              PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return 2;
    SIZE_T bytes = (lstrlenW(DLL_PATH) + 1) * sizeof(WCHAR);
    void* remote = VirtualAllocEx(proc, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    int rc = 2;
    if (remote && WriteProcessMemory(proc, remote, DLL_PATH, bytes, NULL)) {
        FARPROC ll = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE t = CreateRemoteThread(proc, NULL, 0, (LPTHREAD_START_ROUTINE)ll, remote, 0, NULL);
        if (t) {
            WaitForSingleObject(t, 10000);
            DWORD code = 0; GetExitCodeThread(t, &code);
            rc = code ? 0 : 2;
            CloseHandle(t);
        }
    }
    if (remote) VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);
    return rc;
}
