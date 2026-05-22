#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsvc.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <accctrl.h>
#include <aclapi.h>
#include <vector>
#include <string>
#include <mutex>
#include <tlhelp32.h>

// Подключаем сгенерированный MIDL заголовок RPC
#include "AntivirusRpc_h.h"

#pragma comment(lib, "userenv.lib")

// Глобальные состояния службы
const wchar_t* SERVICE_NAME = L"MyAntivirusService";
SERVICE_STATUS_HANDLE g_StatusHandle = NULL;
HANDLE g_ServiceStopEvent = INVALID_HANDLE_VALUE;
std::mutex g_ProcessesMutex;
std::vector<HANDLE> g_ActiveGuiHandles;

// Функции защиты процессов (DACL)
void ProtectProcessFromTermination(HANDLE hProcess) {
    PSID pAdminSid = NULL;
    PSID pUserSid = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    
    AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_REGS_ADMINS, 0, 0, 0, 0, 0, 0, &pAdminSid);
    AllocateAndInitializeSid(&NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_REGS_USERS, 0, 0, 0, 0, 0, 0, &pUserSid);

    EXPLICIT_ACCESSW ea[2] = {};
    
    // БОНУС 4: Запрещаем администраторам завершать процесс
    ea[0].grfAccessPermissions = PROCESS_TERMINATE;
    ea[0].grfAccessMode = DENY_ACCESS;
    ea[0].grfInheritance = NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[0].Trustee.ptstrName = (LPWSTR)pAdminSid;

    // БОНУС 2 и 3: Запрещаем обычным пользователям завершать процесс
    ea[1].grfAccessPermissions = PROCESS_TERMINATE;
    ea[1].grfAccessMode = DENY_ACCESS;
    ea[1].grfInheritance = NO_INHERITANCE;
    ea[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[1].Trustee.ptstrName = (LPWSTR)pUserSid;

    PACL pNewDacl = NULL;
    SetEntriesInAclW(2, ea, NULL, &pNewDacl);

    // Применяем DACL к дескриптору безопасности процесса
    SetSecurityInfo(hProcess, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDacl, NULL);

    if (pNewDacl) LocalFree(pNewDacl);
    if (pAdminSid) FreeSid(pAdminSid);
    if (pUserSid) FreeSid(pUserSid);
}

// Запуск GUI в сессии пользователя
void LaunchGuiInSession(DWORD sessionId) {
    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) return;

    HANDLE hDuplicatedToken = NULL;
    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDuplicatedToken);
    CloseHandle(hToken);

    if (!hDuplicatedToken) return;

    // Получаем путь к нашему GUI
    wchar_t servicePath[MAX_PATH];
    GetModuleFileNameW(NULL, servicePath, MAX_PATH);
    std::wstring guiPath(servicePath);
    size_t pos = guiPath.find(L"AntivirusService.exe");
    if (pos != std::wstring::npos) {
        guiPath.replace(pos, 20, L"Antivirus.exe");
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // Требование 1 и 2: Окно при запуске должно быть скрыто

    PROCESS_INFORMATION pi = {};
    
    void* pEnv = NULL;
    CreateEnvironmentBlock(&pEnv, hDuplicatedToken, FALSE);

    BOOL success = CreateProcessAsUserW(
        hDuplicatedToken,
        guiPath.c_str(),
        NULL, NULL, NULL, FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        pEnv, NULL, &si, &pi
    );

    if (success) {
        // БОНУС 3: Защищаем запущенный GUI от принудительного закрытия пользователями
        ProtectProcessFromTermination(pi.hProcess);

        std::lock_guard<std::mutex> lock(g_ProcessesMutex);
        g_ActiveGuiHandles.push_back(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    if (pEnv) DestroyEnvironmentBlock(pEnv);
    CloseHandle(hDuplicatedToken);
}

// Запуск GUI во всех активных сессиях (кроме сессии 0)
void LaunchGuiInAllSessions() {
    WTS_SESSION_INFOW* pSessionInfo = NULL;
    DWORD sessionCount = 0;
    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &pSessionInfo, &sessionCount)) {
        for (DWORD i = 0; i < sessionCount; ++i) {
            if (pSessionInfo[i].SessionId != 0 && 
                (pSessionInfo[i].State == WTSActive || pSessionInfo[i].State == WTSDisconnected)) {
                LaunchGuiInSession(pSessionInfo[i].SessionId);
            }
        }
        WTSFreeMemory(pSessionInfo);
    }
}

// БОНУС 1: Запрос подтверждения на Secure Desktop (экране входа/блокировки)
bool AskConfirmationOnSecureDesktop(DWORD sessionId) {
    HANDLE hToken = NULL;
    if (!WTSQueryUserToken(sessionId, &hToken)) return true; // Если токен не взять, разрешаем по умолчанию

    HANDLE hDuplicatedToken = NULL;
    DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityIdentification, TokenPrimary, &hDuplicatedToken);
    CloseHandle(hToken);

    if (!hDuplicatedToken) return true;

    wchar_t servicePath[MAX_PATH];
    GetModuleFileNameW(NULL, servicePath, MAX_PATH);
    std::wstring guiPath(servicePath);
    size_t pos = guiPath.find(L"AntivirusService.exe");
    if (pos != std::wstring::npos) {
        guiPath.replace(pos, 20, L"Antivirus.exe");
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.lpDesktop = (LPWSTR)L"Winsta0\\Winlogon"; // Запуск строго на Secure Desktop!

    PROCESS_INFORMATION pi = {};
    std::wstring cmd = guiPath + L" --secure-prompt";

    BOOL success = CreateProcessAsUserW(
        hDuplicatedToken,
        NULL,
        (LPWSTR)cmd.c_str(),
        NULL, NULL, FALSE,
        0, NULL, NULL, &si, &pi
    );

    bool confirmed = false;
    if (success) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        confirmed = (exitCode == IDYES); // IDYES = 6
    } else {
        confirmed = true; // Если не смогли запустить, разрешаем выход
    }

    CloseHandle(hDuplicatedToken);
    return confirmed;
}

// Завершение всех запущенных GUI процессов при остановке службы
void TerminateAllGuiProcesses() {
    std::lock_guard<std::mutex> lock(g_ProcessesMutex);
    for (HANDLE hProcess : g_ActiveGuiHandles) {
        // Чтобы остановить процесс, мы временно сбросим DACL, который сами же установили
        SetSecurityInfo(hProcess, SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL);
        TerminateProcess(hProcess, 0); // Требование 6: Завершить все GUI
        CloseHandle(hProcess);
    }
    g_ActiveGuiHandles.clear();
}

// Реализация RPC интерфейса остановки службы
long StopAntivirusService() {
    DWORD activeSessionId = WTSGetActiveConsoleSessionId();
    
    // БОНУС 1: Спрашиваем подтверждение на Secure Desktop активного пользователя
    if (AskConfirmationOnSecureDesktop(activeSessionId)) {
        RpcMgmtStopServerListening(NULL); // Останавливаем сервер RPC
        return 1; // Успешно остановлено
    }
    return 0; // Отклонено пользователем
}

// Обязательные функции аллокации памяти для RPC
void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
void __RPC_USER midl_user_free(void* ptr) { free(ptr); }

// Обработчик сигналов службы
DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD dwEventType, LPVOID lpEventData, LPVOID lpContext) {
    switch (dwControl) {
        // Требование 2: Отслеживание входа новых пользователей
        case SERVICE_CONTROL_SESSION_CHANGE:
            if (dwEventType == WTS_SESSION_LOGON) {
                WTSSESSION_NOTIFICATION* pNotification = (WTSSESSION_NOTIFICATION*)lpEventData;
                if (pNotification->dwSessionId != 0) {
                    LaunchGuiInSession(pNotification->dwSessionId);
                }
            }
            break;
        default:
            break;
    }
    return NO_ERROR;
}

// Главный поток службы
void WINAPI ServiceMain(DWORD dwArgc, LPTSTR* lpszArgv) {
    // Требование 3: Отключаем обработку Stop и Shutdown на уровне SCM
    // Мы регистрируем обработчик, но НЕ возвращаем флаги SERVICE_ACCEPT_STOP и SERVICE_ACCEPT_SHUTDOWN
    g_StatusHandle = RegisterServiceCtrlHandlerExW(SERVICE_NAME, ServiceCtrlHandler, NULL);
    if (!g_StatusHandle) return;

    SERVICE_STATUS status = {};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwControlsAccepted = SERVICE_ACCEPT_SESSION_CHANGE; // Принимаем только смену сессий!
    SetServiceStatus(g_StatusHandle, &status);

    g_ServiceStopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    // БОНУС 4: Защищаем саму службу от принудительного закрытия кем угодно
    ProtectProcessFromTermination(GetCurrentProcess());

    status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_StatusHandle, &status);

    // Требование 1: Запуск GUI во всех имеющихся терминальных сессиях
    LaunchGuiInAllSessions();

    // Требование 4: Настройка сервера Windows RPC с транспортом ALPC (ncalrpc)
    RpcServerUseProtseqEpW((RPC_WSTR)L"ncalrpc", RPC_C_PROTSEQ_MAX_REQS_DEFAULT, (RPC_WSTR)L"AntivirusRpcEndpoint", NULL);
    RpcServerRegisterIf(AntivirusRpc_v1_0_s_ifspec, NULL, NULL); // Регистрация интерфейса
    
    // Служба висит здесь и обрабатывает RPC-запросы до тех пор, пока сервер не остановят
    RpcServerListen(1, RPC_C_LISTEN_MAX_CALLS_DEFAULT, FALSE);

    // Началась остановка службы
    status.dwCurrentState = SERVICE_STOP_PENDING;
    SetServiceStatus(g_StatusHandle, &status);

    // Требование 6: Завершаем все запущенные нами графические приложения
    TerminateAllGuiProcesses();

    CloseHandle(g_ServiceStopEvent);

    status.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(g_StatusHandle, &status);
}

int wmain(int argc, wchar_t* argv[]) {
    SERVICE_TABLE_ENTRYW ServiceTable[] = {
        { (LPWSTR)SERVICE_NAME, ServiceMain },
        { NULL, NULL }
    };
    StartServiceCtrlDispatcherW(ServiceTable);
    return 0;
}