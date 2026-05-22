cmake_minimum_required(VERSION 3.15)
# Называем проект (Антивирус)
project(AntivirusProject VERSION 1.0 LANGUAGES CXX)

# WinUI 3 требует современный стандарт C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Собираем exe файл (без черного окна консоли) + подключаем манифест
add_executable(Antivirus WIN32 main.cpp app.manifest)

# Автоматическая загрузка WinUI 3 и C++/WinRT через NuGet
set_property(TARGET Antivirus PROPERTY VS_PACKAGE_REFERENCES
    "Microsoft.WindowsAppSDK_1.5.240311000"
    "Microsoft.Windows.CppWinRT_2.0.240111.5"
)

# WinUI 3 требует явного указания архитектуры процессора (x64)
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_Platform "x64")
# Указываем, что наше приложение "Распакованное" (Unpackaged)
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsPackageType "None")

# --- СУПЕР-ФИКС: ДЕЛАЕМ ПРИЛОЖЕНИЕ ПОЛНОСТЬЮ АВТОНОМНЫМ (Self-Contained) ---
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSDKSelfContained "true")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_SelfContained "true")

# --- ОТКЛЮЧАЕМ СЛОМАННЫЙ ЗАГРУЗЧИК И ВКЛЮЧАЕМ ЛОКАЛЬНЫЙ АКТИВАТОР ---
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSDKBootstrapAutoInitialize "false")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSdkUndockedRegFreeWinRTInitialize "true")

# --- ИСПРАВЛЕНИЕ БАГА VS2022 "Sequence contains no elements" ---
set_target_properties(Antivirus PROPERTIES
    VS_GLOBAL_NuGetTargetMoniker "native,Version=v0.0"
    VS_GLOBAL_TargetFrameworkMoniker "native,Version=v0.0"
    VS_GLOBAL_RuntimeIdentifiers "win;win-x86;win-x64;win-arm;win-arm64"
)

# --- ИСПРАВЛЕНИЕ КОДИРОВКИ (КРАКОЗЯБР) ДЛЯ КОНКРЕТНОЙ ЦЕЛИ ---
if(MSVC)
    target_compile_options(Antivirus PRIVATE "/utf-8")
endif()

# Подключаем системные библиотеки Windows
target_link_libraries(Antivirus PRIVATE shell32.lib user32.lib)