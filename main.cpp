#include <windows.h>
#include <shellapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Microsoft.UI.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <microsoft.ui.xaml.window.h>
#include <MddBootstrap.h> // Bootstrapper для WinUI 3

// Идентификаторы для трея
#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_APP_ICON 1001
#define ID_TRAY_OPEN 1002
#define ID_TRAY_EXIT 1003

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

// Глобальные переменные (состояния)
HWND g_hwndHidden = NULL;
NOTIFYICONDATAW g_nid = {};
UINT g_taskbarRestartMsg = 0;
winrt::Microsoft::UI::Windowing::AppWindow g_appWindow{ nullptr };
winrt::Microsoft::UI::Xaml::Window g_xamlWindow{ nullptr };

// Объявления функций
void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon();
void ShowMainWindow();
void ExitApp();
void ShowContextMenu(HWND hwnd);

// --- КЛАСС ПРИЛОЖЕНИЯ WinUI 3 ---
struct App : ApplicationT<App>
{
    void OnLaunched(LaunchActivatedEventArgs const&)
    {
        g_xamlWindow = Window();
        g_xamlWindow.Title(L"Антивирус (Главное окно)");

        // Требование 9: Меню главного окна "Файл -> Выход"
        MenuBar menuBar;
        MenuBarItem fileMenu;
        fileMenu.Title(L"Файл");
        MenuFlyoutItem exitItem;
        exitItem.Text(L"Выход");
        exitItem.Click([&](auto&&, auto&&) { ExitApp(); });
        fileMenu.Items().Append(exitItem);
        menuBar.Items().Append(fileMenu);

        StackPanel panel;
        panel.Children().Append(menuBar);
        
        TextBlock text;
        text.Text(L"Антивирус работает в фоне. Это интерфейс WinUI 3.0!");
        text.Margin({20, 20, 20, 20});
        panel.Children().Append(text);

        g_xamlWindow.Content(panel);

        // Получаем доступ к системному управлению окном WinUI
        auto windowNative = g_xamlWindow.as<IWindowNative>();
        HWND hwnd{0};
        windowNative->get_WindowHandle(&hwnd);
        auto windowId = Microsoft::UI::GetWindowIdFromWindow(hwnd);
        g_appWindow = Microsoft::UI::Windowing::AppWindow::GetFromWindowId(windowId);

        // Требование 8: При закрытии прячем окно, но продолжаем работу
        g_appWindow.Closing([&](auto&& sender, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
            args.Cancel(true); // Отменяем полное закрытие
            sender.Hide();     // Прячем окно
        });

        // Требование 7: Мы НЕ вызываем g_appWindow.Show() здесь, 
        // поэтому при запуске окно не показывается, программа стартует скрытно в трее.
    }
};

// --- ФУНКЦИИ WIN32 ДЛЯ ТРЕЯ ---
void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_APP_ICON;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_SHIELD);
    lstrcpyW(g_nid.szTip, L"Мой Антивирус");
    Shell_NotifyIconW(NIM_ADD, &g_nid); // Требование 1
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

void ShowMainWindow() {
    if (g_xamlWindow && g_appWindow) {
        g_xamlWindow.Activate();
        g_appWindow.Show();
    }
}

void ExitApp() {
    RemoveTrayIcon();
    if (g_xamlWindow) {
        Application::Current().Exit(); // Завершаем цикл WinUI 3
    } else {
        PostQuitMessage(0);
    }
}

void ShowContextMenu(HWND hwnd) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, ID_TRAY_OPEN, L"Открыть"); // Требование 4
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, ID_TRAY_EXIT, L"Выход");   // Требование 5

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(hMenu);
}

// Обработчик скрытого окна, принимающий сообщения от трея
LRESULT CALLBACK HiddenWindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    // Требование 6: Пересоздание иконки при падении/перезапуске Проводника
    if (uMsg == g_taskbarRestartMsg) {
        AddTrayIcon(hwnd);
        return 0;
    }
    switch (uMsg) {
        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                ShowMainWindow(); // Требование 2
            } else if (lParam == WM_RBUTTONUP) {
                ShowContextMenu(hwnd); // Требование 3
            }
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_OPEN) {
                ShowMainWindow();
            } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
                ExitApp();
            }
            return 0;
        case WM_DESTROY:
            ExitApp();
            return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

// --- ТОЧКА ВХОДА ---
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    // Требование 10: Защита от повторного запуска (Мьютекс)
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Local\\MyAntivirusSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Антивирус уже запущен!", L"Ошибка", MB_ICONWARNING | MB_OK);
        return 0;
    }

    // Инициализация среды Windows App SDK (нужно для работы WinUI 3 из .exe)
    HRESULT hr = MddBootstrapInitialize2(0x00010005, L"", MIN_VERSION{0}, MddBootstrapInitializeOptions_OnNoMatch_ShowUI);
    if (FAILED(hr)) {
        ReleaseMutex(hMutex);
        CloseHandle(hMutex);
        return 0;
    }

    g_taskbarRestartMsg = RegisterWindowMessageW(L"TaskbarCreated");

    // Создаем невидимое окно Win32. Оно нужно ТОЛЬКО чтобы ловить клики по трею.
    const wchar_t CLASS_NAME[] = L"HiddenTrayClass";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = HiddenWindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    // Окно создается, но НЕ показывается (мы не вызываем ShowWindow)
    g_hwndHidden = CreateWindowExW(0, CLASS_NAME, L"Tray Window", WS_OVERLAPPEDWINDOW, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    AddTrayIcon(g_hwndHidden);

    // Запускаем современное WinUI 3 приложение
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Application::Start([](auto&&) {
        ::winrt::make<App>(); // Цикл заблокируется здесь, пока мы не нажмем Выход
    });

    // Очистка ресурсов после закрытия программы
    MddBootstrapShutdown();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}