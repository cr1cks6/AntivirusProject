cmake_minimum_required(VERSION 3.15)
project(AntivirusProject VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Настройка компиляции IDL файла через системный MIDL
set(RPC_IDL "${CMAKE_CURRENT_SOURCE_DIR}/AntivirusRpc.idl")
set(RPC_HEADER "${CMAKE_CURRENT_BINARY_DIR}/AntivirusRpc_h.h")
set(RPC_CLIENT "${CMAKE_CURRENT_BINARY_DIR}/AntivirusRpc_c.c")
set(RPC_SERVER "${CMAKE_CURRENT_BINARY_DIR}/AntivirusRpc_s.c")

add_custom_command(
    OUTPUT "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}"
    COMMAND midl /char signed /env x64 /out "${CMAKE_CURRENT_BINARY_DIR}" "${RPC_IDL}"
    DEPENDS "${RPC_IDL}"
    COMMENT "Compiling RPC IDL with MIDL..."
)

# Разрешаем компилятору видеть сгенерированные MIDL-файлы
include_directories(${CMAKE_CURRENT_BINARY_DIR})


# --- ЦЕЛЬ 1: Графическое приложение ---
add_executable(Antivirus WIN32 main.cpp app.manifest "${RPC_CLIENT}" "${RPC_HEADER}")

set_property(TARGET Antivirus PROPERTY VS_PACKAGE_REFERENCES
    "Microsoft.WindowsAppSDK_1.5.240311000"
    "Microsoft.Windows.CppWinRT_2.0.240111.5"
)
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_Platform "x64")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsPackageType "None")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSDKSelfContained "true")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_SelfContained "true")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSDKBootstrapAutoInitialize "false")
set_property(TARGET Antivirus PROPERTY VS_GLOBAL_WindowsAppSdkUndockedRegFreeWinRTInitialize "true")

set_target_properties(Antivirus PROPERTIES
    VS_GLOBAL_NuGetTargetMoniker "native,Version=v0.0"
    VS_GLOBAL_TargetFrameworkMoniker "native,Version=v0.0"
    VS_GLOBAL_RuntimeIdentifiers "win;win-x86;win-x64;win-arm;win-arm64"
)

if(MSVC)
    target_compile_options(Antivirus PRIVATE "/utf-8")
endif()

target_link_libraries(Antivirus PRIVATE shell32.lib user32.lib rpcrt4.lib wtsapi32.lib advapi32.lib)


# --- ЦЕЛЬ 2: Системная служба ---
add_executable(AntivirusService service.cpp "${RPC_SERVER}" "${RPC_HEADER}")

# КРИТИЧНО: Применяем те же свойства нативной платформы, чтобы убрать ошибку NuGet "Sequence contains no elements"
set_target_properties(AntivirusService PROPERTIES
    VS_GLOBAL_NuGetTargetMoniker "native,Version=v0.0"
    VS_GLOBAL_TargetFrameworkMoniker "native,Version=v0.0"
    VS_GLOBAL_RuntimeIdentifiers "win;win-x86;win-x64;win-arm;win-arm64"
)

if(MSVC)
    target_compile_options(AntivirusService PRIVATE "/utf-8")
endif()

target_link_libraries(AntivirusService PRIVATE rpcrt4.lib wtsapi32.lib advapi32.lib user32.lib)