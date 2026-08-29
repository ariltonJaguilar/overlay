#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <ole2.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#include <uiautomation.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
struct ProcessInfo { DWORD pid{}; DWORD parent{}; std::wstring name; std::wstring path; };

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

std::vector<ProcessInfo> processes() {
    std::vector<ProcessInfo> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) do {
        ProcessInfo item{entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile, {}};
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item.pid);
        if (process) {
            wchar_t path[32768]{}; DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size)) item.path.assign(path, size);
            CloseHandle(process);
        }
        result.push_back(std::move(item));
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    return result;
}

bool wandRelated(const ProcessInfo& item) {
    if (item.pid == GetCurrentProcessId()) return false;
    const auto text = lower(item.name + L" " + item.path);
    return text.find(L"wand") != std::wstring::npos || text.find(L"wemod") != std::wstring::npos;
}

const wchar_t* tcpState(DWORD state) {
    switch (state) {
    case MIB_TCP_STATE_CLOSED: return L"CLOSED";
    case MIB_TCP_STATE_LISTEN: return L"LISTEN";
    case MIB_TCP_STATE_SYN_SENT: return L"SYN_SENT";
    case MIB_TCP_STATE_SYN_RCVD: return L"SYN_RECEIVED";
    case MIB_TCP_STATE_ESTAB: return L"ESTABLISHED";
    case MIB_TCP_STATE_FIN_WAIT1: return L"FIN_WAIT_1";
    case MIB_TCP_STATE_FIN_WAIT2: return L"FIN_WAIT_2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"CLOSE_WAIT";
    case MIB_TCP_STATE_CLOSING: return L"CLOSING";
    case MIB_TCP_STATE_LAST_ACK: return L"LAST_ACK";
    case MIB_TCP_STATE_TIME_WAIT: return L"TIME_WAIT";
    case MIB_TCP_STATE_DELETE_TCB: return L"DELETE_TCB";
    default: return L"UNKNOWN";
    }
}

std::wstring address(DWORD raw) {
    IN_ADDR addr{}; addr.S_un.S_addr = raw;
    wchar_t buffer[64]{};
    InetNtopW(AF_INET, &addr, buffer, static_cast<DWORD>(std::size(buffer)));
    return buffer;
}

void printTcp(const std::set<DWORD>& pids) {
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<BYTE> data(size);
    if (GetExtendedTcpTable(data.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(data.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (!pids.contains(row.dwOwningPid)) continue;
        std::wcout << L"  TCP pid=" << row.dwOwningPid << L" " << address(row.dwLocalAddr)
                   << L":" << ntohs(static_cast<u_short>(row.dwLocalPort)) << L" -> "
                   << address(row.dwRemoteAddr) << L":" << ntohs(static_cast<u_short>(row.dwRemotePort))
                   << L" state=" << tcpState(row.dwState) << L"\n";
    }
}

void printUdp(const std::set<DWORD>& pids) {
    ULONG size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    std::vector<BYTE> data(size);
    if (GetExtendedUdpTable(data.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) != NO_ERROR) return;
    const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(data.data());
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& row = table->table[index];
        if (!pids.contains(row.dwOwningPid)) continue;
        std::wcout << L"  UDP pid=" << row.dwOwningPid << L" " << address(row.dwLocalAddr)
                   << L":" << ntohs(static_cast<u_short>(row.dwLocalPort)) << L"\n";
    }
}

struct WindowContext { const std::set<DWORD>* pids; std::vector<HWND>* windows; };
BOOL CALLBACK collectWindow(HWND window, LPARAM parameter) {
    auto& context = *reinterpret_cast<WindowContext*>(parameter);
    DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
    if (context.pids->contains(pid)) context.windows->push_back(window);
    return TRUE;
}

BOOL CALLBACK printVisibleWindow(HWND window, LPARAM) {
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[1024]{}, className[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (!title[0]) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    wchar_t processPath[32768]{};
    DWORD pathSize = static_cast<DWORD>(std::size(processPath));
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        QueryFullProcessImageNameW(process, 0, processPath, &pathSize);
        CloseHandle(process);
    }
    std::wcout << L"  HWND=0x" << std::hex << reinterpret_cast<ULONG_PTR>(window) << std::dec
               << L" pid=" << pid << L" class=\"" << className << L"\" title=\"" << title
               << L"\" process=\"" << processPath << L"\"\n";
    return TRUE;
}

std::wstring bstr(BSTR value) {
    std::wstring result = value ? std::wstring(value, SysStringLen(value)) : L"";
    SysFreeString(value); return result;
}

bool booleanProperty(IUIAutomationElement* element, PROPERTYID property) {
    VARIANT value{};
    VariantInit(&value);
    const bool result = SUCCEEDED(element->GetCurrentPropertyValue(property, &value)) &&
                        value.vt == VT_BOOL && value.boolVal == VARIANT_TRUE;
    VariantClear(&value);
    return result;
}

void printAutomationTree(IUIAutomationTreeWalker* walker, IUIAutomationElement* element,
                         int depth, int maximumDepth) {
    if (!element || depth > maximumDepth) return;
    BSTR nameRaw = nullptr, typeRaw = nullptr, idRaw = nullptr, classRaw = nullptr;
    CONTROLTYPEID typeId{};
    element->get_CurrentName(&nameRaw);
    element->get_CurrentLocalizedControlType(&typeRaw);
    element->get_CurrentAutomationId(&idRaw);
    element->get_CurrentClassName(&classRaw);
    element->get_CurrentControlType(&typeId);
    RECT bounds{};
    element->get_CurrentBoundingRectangle(&bounds);
    std::wcout << std::wstring(static_cast<size_t>(depth) * 2, L' ') << L"- type="
               << bstr(typeRaw) << L"(" << typeId << L") name=\"" << bstr(nameRaw)
               << L"\" automationId=\"" << bstr(idRaw) << L"\" class=\""
               << bstr(classRaw) << L"\" bounds=" << bounds.left << L"," << bounds.top
               << L"-" << bounds.right << L"," << bounds.bottom << L" patterns=";
    bool anyPattern = false;
    const struct { PROPERTYID property; const wchar_t* name; } patterns[] = {
        {UIA_IsInvokePatternAvailablePropertyId, L"Invoke"},
        {UIA_IsTogglePatternAvailablePropertyId, L"Toggle"},
        {UIA_IsValuePatternAvailablePropertyId, L"Value"},
        {UIA_IsRangeValuePatternAvailablePropertyId, L"RangeValue"},
        {UIA_IsSelectionItemPatternAvailablePropertyId, L"SelectionItem"},
        {UIA_IsLegacyIAccessiblePatternAvailablePropertyId, L"LegacyIAccessible"},
        {UIA_IsScrollItemPatternAvailablePropertyId, L"ScrollItem"},
    };
    for (const auto& pattern : patterns) {
        if (!booleanProperty(element, pattern.property)) continue;
        std::wcout << (anyPattern ? L"," : L"") << pattern.name;
        anyPattern = true;
    }
    if (!anyPattern) std::wcout << L"none";
    std::wcout << L"\n";
    if (depth == maximumDepth) return;
    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(element, &child))) return;
    while (child) {
        printAutomationTree(walker, child, depth + 1, maximumDepth);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release(); child = next;
    }
}

// SystemExtendedHandleInformation, deliberately read-only. Pipe ownership can only
// be inferred from handles the current user is allowed to duplicate.
struct HandleEntry { void* object; ULONG_PTR pid; ULONG_PTR handle; ULONG access;
    USHORT trace; USHORT type; ULONG attributes; ULONG reserved; };
struct HandleTable { ULONG_PTR count; ULONG_PTR reserved; HandleEntry entries[1]; };
using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, void*, ULONG, ULONG*);

void printPipes(const std::set<DWORD>& pids) {
    auto ntQuery = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!ntQuery) return;
    ULONG size = 1 << 20;
    std::vector<BYTE> data(size);
    NTSTATUS status{};
    while ((status = ntQuery(64, data.data(), size, &size)) == static_cast<NTSTATUS>(0xC0000004L))
        data.resize(size + (1 << 20));
    if (status < 0) { std::wcout << L"  [indisponivel: NTSTATUS " << std::hex << status << std::dec << L"]\n"; return; }
    const auto* table = reinterpret_cast<const HandleTable*>(data.data());
    std::map<DWORD, HANDLE> processHandles;
    for (DWORD pid : pids) processHandles[pid] = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
    std::set<std::wstring> found;
    for (ULONG_PTR index = 0; index < table->count; ++index) {
        const auto& entry = table->entries[index];
        const DWORD pid = static_cast<DWORD>(entry.pid);
        auto process = processHandles.find(pid);
        if (process == processHandles.end() || !process->second) continue;
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process->second, reinterpret_cast<HANDLE>(entry.handle),
                             GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        if (GetFileType(duplicate) == FILE_TYPE_PIPE) {
            wchar_t path[4096]{};
            const DWORD length = GetFinalPathNameByHandleW(duplicate, path,
                static_cast<DWORD>(std::size(path)), FILE_NAME_NORMALIZED);
            if (length && length < std::size(path)) found.insert(std::to_wstring(pid) + L" " + path);
        }
        CloseHandle(duplicate);
    }
    for (const auto& pipe : found) std::wcout << L"  pid=" << pipe << L"\n";
    if (found.empty()) std::wcout << L"  [nenhum pipe identificavel; tente executar como administrador]\n";
    for (auto [pid, handle] : processHandles) if (handle) CloseHandle(handle);
}

void printUriProtocols(HKEY hive, const wchar_t* hiveName) {
    HKEY classes = nullptr;
    if (RegOpenKeyExW(hive, L"Software\\Classes", 0, KEY_READ, &classes) != ERROR_SUCCESS) return;
    for (DWORD index = 0;; ++index) {
        wchar_t keyName[512]{};
        DWORD keyLength = static_cast<DWORD>(std::size(keyName));
        const LSTATUS status = RegEnumKeyExW(classes, index, keyName, &keyLength, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS) continue;
        HKEY protocol = nullptr;
        if (RegOpenKeyExW(classes, keyName, 0, KEY_READ, &protocol) != ERROR_SUCCESS) continue;
        wchar_t marker[8]{};
        DWORD markerSize = sizeof(marker);
        const bool isProtocol = RegQueryValueExW(protocol, L"URL Protocol", nullptr, nullptr,
                                                  reinterpret_cast<BYTE*>(marker), &markerSize) == ERROR_SUCCESS;
        const auto lowered = lower(keyName);
        if (isProtocol && (lowered.find(L"wand") != std::wstring::npos ||
                           lowered.find(L"wemod") != std::wstring::npos)) {
            wchar_t command[32768]{};
            DWORD commandSize = sizeof(command);
            HKEY openCommand = nullptr;
            if (RegOpenKeyExW(protocol, L"shell\\open\\command", 0, KEY_READ, &openCommand) == ERROR_SUCCESS) {
                RegQueryValueExW(openCommand, nullptr, nullptr, nullptr,
                                 reinterpret_cast<BYTE*>(command), &commandSize);
                RegCloseKey(openCommand);
            }
            std::wcout << L"  " << hiveName << L" protocol=" << keyName
                       << L" command=\"" << command << L"\"\n";
        }
        RegCloseKey(protocol);
    }
    RegCloseKey(classes);
}
} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    const auto all = processes();
    std::set<DWORD> roots;
    for (const auto& item : all) if (wandRelated(item)) roots.insert(item.pid);
    // Include descendants even when helper executable names do not contain Wand.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& item : all)
            if (roots.contains(item.parent) && !roots.contains(item.pid)) { roots.insert(item.pid); changed = true; }
    }
    std::wcout << L"=== Wand Diagnostic (somente leitura) ===\n\n[Processos]\n";
    for (const auto& item : all) if (roots.contains(item.pid))
        std::wcout << L"  pid=" << item.pid << L" ppid=" << item.parent << L" name=" << item.name
                   << L"\n    path=" << (item.path.empty() ? L"<acesso negado>" : item.path) << L"\n";
    if (roots.empty()) { std::wcout << L"  Wand/WeMod nao encontrado.\n"; return 2; }

    std::wcout << L"\n[Portas IPv4]\n"; printTcp(roots); printUdp(roots);
    std::wcout << L"\n[Named Pipes por handle]\n"; printPipes(roots);
    std::wcout << L"\n[Protocolos URI registrados para Wand/WeMod]\n";
    printUriProtocols(HKEY_CURRENT_USER, L"HKCU");
    printUriProtocols(HKEY_LOCAL_MACHINE, L"HKLM");

    std::vector<HWND> windows; WindowContext context{&roots, &windows};
    EnumWindows(collectWindow, reinterpret_cast<LPARAM>(&context));
    std::wcout << L"\n[Janelas e UI Automation]\n";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation));
    IUIAutomationTreeWalker* walker = nullptr;
    if (automation) automation->get_ControlViewWalker(&walker);
    for (HWND window : windows) {
        wchar_t title[1024]{}, className[256]{}; DWORD pid = 0;
        GetWindowTextW(window, title, static_cast<int>(std::size(title)));
        GetClassNameW(window, className, static_cast<int>(std::size(className)));
        GetWindowThreadProcessId(window, &pid);
        RECT rect{}; GetWindowRect(window, &rect);
        std::wcout << L"\n  HWND=0x" << std::hex << reinterpret_cast<ULONG_PTR>(window) << std::dec
                   << L" pid=" << pid << L" visible=" << IsWindowVisible(window)
                   << L" class=\"" << className << L"\" title=\"" << title << L"\" rect="
                   << rect.left << L"," << rect.top << L"-" << rect.right << L"," << rect.bottom << L"\n";
        if (automation && walker) {
            IUIAutomationElement* root = nullptr;
            if (SUCCEEDED(automation->ElementFromHandle(window, &root)) && root) {
                printAutomationTree(walker, root, 2, 14); root->Release();
            }
        }
    }
    if (windows.empty()) {
        std::wcout << L"  [nenhuma janela superior pertence aos PIDs detectados]\n"
                      L"\n[Janelas visiveis sem filtro, para diagnosticar hosts externos]\n";
        EnumWindows(printVisibleWindow, 0);
    }
    if (walker) walker->Release(); if (automation) automation->Release();
    CoUninitialize();
    return 0;
}
