#define _WIN32_WINNT 0x0A00  // КРИТИЧНО: Явно объявляем поддержку Windows 10/11 в самом верху
#define WINVER 0x0A00
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>          // КРИТИЧНО: Чтобы компилятор точно знал базовый класс IUnknown!
#undef GetCurrentTime // КРИТИЧНО: чтобы избежать конфликтов времени Win32 и WinRT!
#include <shellapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <string>
#include <vector>

// WinRT и WinUI 3.0 заголовки
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Text.h>               // Для работы со шрифтами (Bold)
#include <winrt/Windows.UI.h>                    // Для работы с цветами
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>       // КРИТИЧНО: Для работы с цветами и кистями

// Подключаем сгенерированный MIDL заголовок RPC
#include "AntivirusRpc_h.h"

// КРИТИЧНО: Объявляем COM-интерфейс IWindowNative вручную.
struct __declspec(uuid("E352E75C-1FC2-418F-A1AD-E162464790E5")) IWindowNative : ::IUnknown
{
    virtual HRESULT __stdcall get_WindowHandle(HWND* hWnd) = 0;
};

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_APP_ICON 1001
#define ID_TRAY_OPEN 1002
#define ID_TRAY_EXIT 1003

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::XamlTypeInfo;
using namespace Microsoft::UI::Xaml::Markup;
using namespace Windows::UI::Xaml::Interop;
using namespace Microsoft::UI::Xaml::Media;

// Глобальные переменные
HWND g_hwndHidden = NULL;
NOTIFYICONDATAW g_nid = {};
UINT g_taskbarRestartMsg = 0;
winrt::Microsoft::UI::Windowing::AppWindow g_appWindow{ nullptr };
winrt::Microsoft::UI::Xaml::Window g_xamlWindow{ nullptr };
const wchar_t* SERVICE_NAME = L"MyAntivirusService";

// Обязательные функции аллокации памяти для RPC
void* __RPC_USER midl_user_allocate(size_t size) { return malloc(size); }
void __RPC_USER midl_user_free(void* ptr) { free(ptr); }

// --- Вспомогательные системные проверки ---

// Получаем PID родительского процесса
DWORD GetParentPid() {
    DWORD currentPid = GetCurrentProcessId();
    DWORD parentPid = 0;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (pe.th32ProcessID == currentPid) {
                    parentPid = pe.th32ParentProcessID;
                    break;
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
    return parentPid;
}

// Получаем PID нашей службы
DWORD GetServicePid() {
    DWORD pid = 0;
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS);
        if (hService) {
            DWORD bytesNeeded = 0;
            QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, NULL, 0, &bytesNeeded);
            if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                std::vector<BYTE> buffer(bytesNeeded);
                LPSERVICE_STATUS_PROCESS pStatus = (LPSERVICE_STATUS_PROCESS)buffer.data();
                if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)pStatus, bytesNeeded, &bytesNeeded)) {
                    pid = pStatus->dwProcessId;
                }
            }
            CloseHandle(hService);
        }
        CloseHandle(hSCM);
    }
    return pid;
}

// Проверка: запущена ли служба прямо сейчас
bool IsServiceRunning() {
    bool running = false;
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_QUERY_STATUS);
        if (hService) {
            SERVICE_STATUS_PROCESS status = {};
            DWORD bytesNeeded = 0;
            if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
                running = (status.dwCurrentState == SERVICE_RUNNING);
            }
            CloseHandle(hService);
        }
        CloseHandle(hSCM);
    }
    return running;
}

// Требование 1 GUI: Запуск службы и ожидание состояния RUNNING
bool StartAntivirusService() {
    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ALL_ACCESS);
    if (!hSCM) return false;

    SC_HANDLE hService = OpenServiceW(hSCM, SERVICE_NAME, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseHandle(hSCM);
        return false;
    }

    BOOL success = StartServiceW(hService, 0, NULL);
    if (success || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) {
        // Ожидаем статус RUNNING
        SERVICE_STATUS_PROCESS status = {};
        DWORD bytesNeeded = 0;
        for (int i = 0; i < 30; ++i) { // Ждем максимум 15 секунд
            if (QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&status, sizeof(status), &bytesNeeded)) {
                if (status.dwCurrentState == SERVICE_RUNNING) {
                    CloseHandle(hService);
                    CloseHandle(hSCM);
                    return true;
                }
            }
            Sleep(500);
        }
    }

    CloseHandle(hService);
    CloseHandle(hSCM);
    return false;
}

// Вызов RPC сервера для плавной остановки службы (с явным связыванием!)
void RequestServiceStop() {
    RPC_WSTR pszStringBinding = NULL;
    // Требование 4 и 5: Подключаемся к RPC ALPC (ncalrpc)
    if (RpcStringBindingComposeW(NULL, (RPC_WSTR)L"ncalrpc", NULL, (RPC_WSTR)L"AntivirusRpcEndpoint", NULL, &pszStringBinding) == RPC_S_OK) {
        RPC_BINDING_HANDLE hBinding = NULL;
        if (RpcBindingFromStringBindingW(pszStringBinding, &hBinding) == RPC_S_OK) {
            
            // Вызываем удаленную функцию, передавая дескриптор связи
            RpcTryExcept {
                StopAntivirusService(hBinding); // Передаем хэндл явно
            }
            RpcExcept(1) {
                // Если служба уже мертва или RPC недоступен
            }
            RpcEndExcept

            RpcBindingFree(&hBinding);
        }
        RpcStringFreeW(&pszStringBinding);
    }
}

// Опережающие объявления функций UI
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowMainWindow();
void ExitApp();
void ShowContextMenu(HWND hwnd);
LRESULT CALLBACK HiddenWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

struct App : public ApplicationT<App, IXamlMetadataProvider>
{
    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        Resources().MergedDictionaries().Append(XamlControlsResources());

        g_xamlWindow = Window();
        g_xamlWindow.Title(L"Антивирус (Главное окно)");

        // Меню "Файл -> Выход"
        MenuBar menuBar;
        MenuBarItem fileMenu;
        fileMenu.Title(L"Файл");
        MenuFlyoutItem exitItem;
        exitItem.Text(L"Выход");
        exitItem.Click([&](auto&&, auto&&) { 
            RequestServiceStop(); // Требование 3 GUI: Клик по меню останавливает службу
        });
        fileMenu.Items().Append(exitItem);
        menuBar.Items().Append(fileMenu);

        Border card;
        card.Background(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 32, 32, 32)));
        card.CornerRadius(CornerRadius{12});
        card.Padding({40, 40, 40, 40});
        card.Width(480);
        card.HorizontalAlignment(HorizontalAlignment::Center);
        card.VerticalAlignment(VerticalAlignment::Center);

        StackPanel cardContent;
        cardContent.Spacing(18);

        FontIcon shieldIcon;
        shieldIcon.Glyph(L"\uF13C");
        shieldIcon.FontSize(80);
        shieldIcon.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 16, 124, 65)));

        TextBlock titleText;
        titleText.Text(L"Компьютер защищен");
        titleText.FontSize(24);
        titleText.FontWeight(Windows::UI::Text::FontWeights::Bold());
        titleText.HorizontalAlignment(HorizontalAlignment::Center);

        TextBlock subText;
        subText.Text(L"Защита работает под управлением системной службы.");
        subText.FontSize(13);
        subText.Foreground(SolidColorBrush(Windows::UI::ColorHelper::FromArgb(255, 180, 180, 180)));
        subText.HorizontalAlignment(HorizontalAlignment::Center);
        subText.TextAlignment(TextAlignment::Center);
        subText.TextWrapping(TextWrapping::Wrap);

        cardContent.Children().Append(shieldIcon);
        cardContent.Children().Append(titleText);
        cardContent.Children().Append(subText);
        card.Child(cardContent);

        Grid rootLayout;
        RowDefinition r1, r2;
        r1.Height(GridLength{0, GridUnitType::Auto});
        r2.Height(GridLength{1, GridUnitType::Star});
        rootLayout.RowDefinitions().Append(r1);
        rootLayout.RowDefinitions().Append(r2);

        rootLayout.Children().Append(menuBar);
        Grid::SetRow(menuBar, 0);

        rootLayout.Children().Append(card);
        Grid::SetRow(card, 1);

        g_xamlWindow.Content(rootLayout);

        // ИСПРАВЛЕНО: Безопасный вызов QueryInterface через com_ptr для классического COM
        winrt::com_ptr<::IWindowNative> windowNative;
        g_xamlWindow.as(windowNative);
        
        HWND hwnd{0};
        windowNative->get_WindowHandle(&hwnd);
        auto windowId = Microsoft::UI::GetWindowIdFromWindow(hwnd);
        g_appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);

        g_appWindow.Closing([&](auto&& sender, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            args.Cancel(true);
            sender.Hide();
        });
    }

    IXamlType GetXamlType(TypeName const& type) { return m_provider.GetXamlType(type); }
    IXamlType GetXamlType(hstring const& fullname) { return m_provider.GetXamlType(fullname); }
    com_array<XmlnsDefinition> GetXmlnsDefinitions() { return m_provider.GetXmlnsDefinitions(); }

private:
    XamlControlsXamlMetaDataProvider m_provider;
};

void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_APP_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_SHIELD);
    lstrcpyW(g_nid.szTip, L"Мой Антивирус");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() { Shell_NotifyIconW(NIM_DELETE, &g_nid); }

void ShowMainWindow() {
    if (g_xamlWindow && g_appWindow) {
        g_xamlWindow.Activate();
        g_appWindow.Show();
    }
}

void ExitApp() {
    RemoveTrayIcon();
    if (g_xamlWindow) {
        Application::Current().Exit();
    } else {
        PostQuitMessage(0);
    }
}

void ShowContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_OPEN, L"Открыть");
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK HiddenWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == g_taskbarRestartMsg) {
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (uMsg) {
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                ShowMainWindow();
            } else if (lParam == WM_RBUTTONUP) {
                ShowContextMenu(hwnd);
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_OPEN) {
                ShowMainWindow();
            } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
                RequestServiceStop(); // Требование 4 GUI: Выход из трея останавливает службу
            }
            return 0;
        case WM_DESTROY:
            ExitApp();
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // БОНУС 1: Обработчик запроса на Secure Desktop
    if (wcsstr(pCmdLine, L"--secure-prompt")) {
        int res = MessageBoxW(NULL, 
            L"Вы действительно хотите остановить службу Антивируса?\nЭто сделает компьютер уязвимым.", 
            L"Запрос безопасности антивируса", 
            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        return res; // Вернет IDYES (6) или IDNO (7)
    }

    // Требование 1 GUI: Проверка состояния службы при старте
    if (!IsServiceRunning()) {
        StartAntivirusService(); // Запускаем, ждем и завершаем работу
        return 0; 
    }

    // Требование 2 GUI: Проверка родительского процесса
    // Мы ДОЛЖНЫ быть запущены исключительно нашей системной службой
    DWORD parentPid = GetParentPid();
    DWORD servicePid = GetServicePid();
    if (parentPid == 0 || servicePid == 0 || parentPid != servicePid) {
        // Если родитель не служба - молча выходим
        return 0;
    }

    // Защита от повторного запуска (Мьютекс)
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\MyAntivirusSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    g_taskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    const wchar_t CLASS_NAME[] = L"HiddenTrayClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = HiddenWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    g_hwndHidden = CreateWindowExW(0, CLASS_NAME, L"Tray Window", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    AddTrayIcon(g_hwndHidden);

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto&&) {
        ::winrt::make<App>();
    });

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}