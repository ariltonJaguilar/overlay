#include "WandIntegration.h"

#include <windows.h>
#include <ole2.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <memory>
#include <thread>

namespace {
struct ComReleaser { template<class T> void operator()(T* value) const { if (value) value->Release(); } };
template<class T> using ComPtr = std::unique_ptr<T, ComReleaser>;
struct Node { ComPtr<IUIAutomationElement> element; std::wstring name, className; CONTROLTYPEID type{}; RECT rect{}; };

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}
std::wstring normalizedGameName(const std::wstring& value) {
    const int required = NormalizeString(NormalizationD, value.c_str(),
                                         static_cast<int>(value.size()), nullptr, 0);
    std::wstring decomposed(required > 0 ? static_cast<size_t>(required) : value.size(), L'\0');
    if (required > 0) NormalizeString(NormalizationD, value.c_str(), static_cast<int>(value.size()),
                                      decomposed.data(), required);
    else decomposed = value;
    std::wstring result;
    bool pendingSpace = false;
    for (wchar_t raw : decomposed) {
        const wchar_t ch = static_cast<wchar_t>(std::towlower(raw));
        if (ch >= 0x0300 && ch <= 0x036f) continue;
        if (std::iswalnum(ch)) {
            if (pendingSpace && !result.empty()) result.push_back(L' ');
            result.push_back(ch); pendingSpace = false;
        } else {
            pendingSpace = true;
        }
    }
    return result;
}

size_t editDistance(const std::wstring& left, const std::wstring& right) {
    std::vector<size_t> row(right.size() + 1);
    for (size_t index = 0; index <= right.size(); ++index) row[index] = index;
    for (size_t l = 1; l <= left.size(); ++l) {
        size_t diagonal = row[0]; row[0] = l;
        for (size_t r = 1; r <= right.size(); ++r) {
            const size_t old = row[r];
            row[r] = (std::min)({row[r] + 1, row[r - 1] + 1,
                                 diagonal + (left[l - 1] == right[r - 1] ? 0u : 1u)});
            diagonal = old;
        }
    }
    return row.back();
}
std::wstring take(BSTR value) {
    std::wstring result = value ? std::wstring(value, SysStringLen(value)) : L"";
    SysFreeString(value); return result;
}
bool valid(const RECT& r) { return r.right > r.left && r.bottom > r.top; }
int centerY(const RECT& r) { return r.top + (r.bottom - r.top) / 2; }
bool contains(const RECT& a, const RECT& b) {
    return b.left >= a.left && b.top >= a.top && b.right <= a.right && b.bottom <= a.bottom;
}

BOOL CALLBACK findWindow(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[128]{}, cls[128]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    GetClassNameW(window, cls, static_cast<int>(std::size(cls)));
    if (lower(title) == L"wand" && std::wstring(cls) == L"Chrome_WidgetWin_1") {
        *reinterpret_cast<HWND*>(parameter) = window; return FALSE;
    }
    return TRUE;
}

void flatten(IUIAutomationTreeWalker* walker, IUIAutomationElement* current,
             std::vector<Node>& nodes, int depth = 0) {
    if (!current || depth > 20) return;
    current->AddRef(); Node node; node.element.reset(current);
    BSTR raw = nullptr; current->get_CurrentName(&raw); node.name = take(raw);
    raw = nullptr; current->get_CurrentClassName(&raw); node.className = take(raw);
    current->get_CurrentControlType(&node.type); current->get_CurrentBoundingRectangle(&node.rect);
    nodes.push_back(std::move(node));
    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(current, &child))) return;
    while (child) {
        flatten(walker, child, nodes, depth + 1);
        IUIAutomationElement* next = nullptr; walker->GetNextSiblingElement(child, &next);
        child->Release(); child = next;
    }
}

struct Snapshot {
    HRESULT com = E_FAIL; HWND window{}; ComPtr<IUIAutomation> automation;
    ComPtr<IUIAutomationTreeWalker> walker; std::vector<Node> nodes;
    Snapshot() { com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~Snapshot() { nodes.clear(); walker.reset(); automation.reset(); if (SUCCEEDED(com)) CoUninitialize(); }
    bool load(bool allowHidden = false) {
        EnumWindows(findWindow, reinterpret_cast<LPARAM>(&window));
        if (!window && allowHidden) return false;
        IUIAutomation* rawAutomation = nullptr;
        if (FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&rawAutomation)))) return false;
        automation.reset(rawAutomation);
        IUIAutomationTreeWalker* rawWalker = nullptr;
        if (FAILED(automation->get_ControlViewWalker(&rawWalker))) return false;
        walker.reset(rawWalker);
        IUIAutomationElement* root = nullptr;
        if (FAILED(automation->ElementFromHandle(window, &root)) || !root) return false;
        flatten(walker.get(), root, nodes); root->Release(); return true;
    }
};

bool looksLikeSwitch(const Node& candidate, const std::vector<Node>& nodes) {
    bool off = false, on = false;
    for (const auto& node : nodes) if (node.type == UIA_TextControlTypeId && contains(candidate.rect, node.rect)) {
        const auto name = lower(node.name); off = off || name == L"off"; on = on || name == L"on";
    }
    return off && on;
}
Node* switchFor(const Node& label, std::vector<Node>& nodes) {
    Node* best = nullptr; int score = INT_MAX;
    for (auto& node : nodes) {
        const int w = node.rect.right - node.rect.left, h = node.rect.bottom - node.rect.top;
        if (node.type != UIA_GroupControlTypeId || node.rect.left <= label.rect.right ||
            w < 50 || w > 160 || h < 18 || h > 50 || !looksLikeSwitch(node, nodes)) continue;
        const int dy = std::abs(centerY(node.rect) - centerY(label.rect));
        if (dy <= 12 && dy * 1000 + node.rect.left - label.rect.right < score) {
            best = &node; score = dy * 1000 + node.rect.left - label.rect.right;
        }
    }
    return best;
}
Node* valueFor(const Node& label, std::vector<Node>& nodes) {
    Node* best = nullptr; int score = INT_MAX;
    for (auto& node : nodes) {
        if ((node.type != UIA_SpinnerControlTypeId && node.type != UIA_SliderControlTypeId) ||
            !valid(node.rect) || node.rect.left <= label.rect.right) continue;
        const int dy = std::abs(centerY(node.rect) - centerY(label.rect));
        const int candidateScore = dy * 1000 + node.rect.left - label.rect.right;
        if (dy <= 14 && candidateScore < score) { best = &node; score = candidateScore; }
    }
    return best;
}
Node* findText(const std::wstring& name, std::vector<Node>& nodes) {
    const auto wanted = lower(name);
    for (auto& node : nodes) if ((node.type == UIA_TextControlTypeId || node.type == UIA_ButtonControlTypeId) &&
                                  lower(node.name) == wanted) return &node;
    return nullptr;
}
bool backgroundClick(HWND window, const Node& node) {
    POINT point{node.rect.left + (node.rect.right-node.rect.left)/2,
                node.rect.top + (node.rect.bottom-node.rect.top)/2};
    if (!ScreenToClient(window, &point)) return false;
    const LPARAM position = MAKELPARAM(point.x, point.y); DWORD_PTR ignored = 0;
    SendMessageTimeoutW(window, WM_MOUSEMOVE, 0, position, SMTO_ABORTIFHUNG, 250, &ignored);
    if (!SendMessageTimeoutW(window, WM_LBUTTONDOWN, MK_LBUTTON, position, SMTO_ABORTIFHUNG, 250, &ignored)) return false;
    return SendMessageTimeoutW(window, WM_LBUTTONUP, 0, position, SMTO_ABORTIFHUNG, 250, &ignored) != 0;
}

void scrollIntoView(Node& node) {
    IUIAutomationScrollItemPattern* pattern = nullptr;
    if (SUCCEEDED(node.element->GetCurrentPatternAs(UIA_ScrollItemPatternId,
                                                     IID_PPV_ARGS(&pattern))) && pattern) {
        pattern->ScrollIntoView();
        pattern->Release();
        Sleep(80);
        node.element->get_CurrentBoundingRectangle(&node.rect);
    }
}

bool invokeWithoutForeground(HWND wandWindow, Node& node) {
    IUIAutomationInvokePattern* pattern = nullptr;
    if (FAILED(node.element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))) || !pattern)
        return false;
    const HWND previousForeground = GetForegroundWindow();
    const bool wasVisible = IsWindowVisible(wandWindow) != FALSE;
    const bool wasMinimized = IsIconic(wandWindow) != FALSE;
    ShowWindow(wandWindow, SW_HIDE);
    const HRESULT action = pattern->Invoke();
    pattern->Release();
    if (wasVisible) {
        ShowWindow(wandWindow, wasMinimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        SetWindowPos(wandWindow, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (previousForeground && previousForeground != wandWindow)
        SetForegroundWindow(previousForeground);
    return SUCCEEDED(action);
}

HRESULT setValueWithoutForeground(HWND wandWindow, IUIAutomationRangeValuePattern* pattern,
                                  double requested) {
    const HWND previousForeground = GetForegroundWindow();
    const bool wasVisible = IsWindowVisible(wandWindow) != FALSE;
    const bool wasMinimized = IsIconic(wandWindow) != FALSE;
    ShowWindow(wandWindow, SW_HIDE);
    const HRESULT result = pattern->SetValue(requested);
    if (wasVisible) {
        ShowWindow(wandWindow, wasMinimized ? SW_SHOWMINNOACTIVE : SW_SHOWNOACTIVATE);
        SetWindowPos(wandWindow, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
    if (previousForeground && previousForeground != wandWindow)
        SetForegroundWindow(previousForeground);
    return result;
}

bool processNamedWand() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry); bool found = false;
    if (Process32FirstW(snapshot, &entry)) do {
        if (lower(entry.szExeFile) == L"wand.exe") { found = true; break; }
    } while (Process32NextW(snapshot, &entry));
    CloseHandle(snapshot); return found;
}
} // namespace

bool WandIntegration::isRunning() { return processNamedWand(); }

bool WandIntegration::isTrainerActive(DWORD gamePid) {
    if (!gamePid) return false;
    std::vector<DWORD> wandPids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) do {
            if (lower(entry.szExeFile) == L"wand.exe" || lower(entry.szExeFile) == L"wandauxiliaryservice.exe")
                wandPids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    if (wandPids.empty()) return false;
    ULONG size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<BYTE> storage(size);
    if (GetExtendedTcpTable(storage.data(), &size, FALSE, AF_INET,
                            TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) return false;
    const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(storage.data());
    // MIB tables expose the IPv4 address in network byte order; on supported
    // little-endian Windows targets 127.0.0.1 is represented as 0x0100007f.
    constexpr DWORD loopback = 0x0100007f;
    for (DWORD index = 0; index < table->dwNumEntries; ++index) {
        const auto& game = table->table[index];
        if (game.dwOwningPid != gamePid || game.dwState != MIB_TCP_STATE_ESTAB ||
            game.dwLocalAddr != loopback || game.dwRemoteAddr != loopback) continue;
        for (DWORD otherIndex = 0; otherIndex < table->dwNumEntries; ++otherIndex) {
            const auto& wand = table->table[otherIndex];
            if (wand.dwState == MIB_TCP_STATE_ESTAB && wand.dwLocalAddr == loopback &&
                wand.dwRemoteAddr == loopback && wand.dwLocalPort == game.dwRemotePort &&
                wand.dwRemotePort == game.dwLocalPort &&
                std::find(wandPids.begin(), wandPids.end(), wand.dwOwningPid) != wandPids.end())
                return true;
        }
    }
    return false;
}

bool WandIntegration::startHidden(std::wstring& error) {
    wchar_t command[32768]{}; DWORD size = sizeof(command); HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\wand\\shell\\open\\command",
                      0, KEY_READ, &key) != ERROR_SUCCESS ||
        RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<BYTE*>(command), &size) != ERROR_SUCCESS) {
        if (key) RegCloseKey(key); error = L"Wand não está instalado ou o protocolo wand: não existe."; return false;
    }
    RegCloseKey(key);
    std::wstring text = command, executable;
    if (!text.empty() && text.front() == L'\"') {
        const auto end = text.find(L'\"', 1); if (end != std::wstring::npos) executable = text.substr(1, end - 1);
    }
    if (executable.empty()) { error = L"Não foi possível localizar o executável do Wand."; return false; }
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION process{}; std::vector<wchar_t> mutableCommand(executable.begin(), executable.end()); mutableCommand.push_back(0);
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        error = L"Falha ao iniciar o Wand (" + std::to_wstring(GetLastError()) + L")."; return false;
    }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    const HWND previousForeground = GetForegroundWindow();
    std::thread([previousForeground] {
        const ULONGLONG deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            HWND wandWindow = nullptr;
            EnumWindows(findWindow, reinterpret_cast<LPARAM>(&wandWindow));
            if (wandWindow) {
                ShowWindow(wandWindow, SW_MINIMIZE);
                SetWindowPos(wandWindow, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                if (previousForeground && previousForeground != wandWindow)
                    SetForegroundWindow(previousForeground);
                break;
            }
            Sleep(25);
        }
    }).detach();
    return true;
}

WandLoadResult WandIntegration::selectGameAndLoad(const std::wstring& gameName, DWORD gamePid) {
    WandLoadResult result; Snapshot snapshot;
    if (!snapshot.load()) { result.error = L"Abra a janela do Wand ao menos uma vez para habilitar a integração."; return result; }
    if (!gameName.empty()) {
        Node* game = nullptr;
        const std::wstring requested = normalizedGameName(gameName);
        std::vector<std::pair<size_t, Node*>> candidates;
        for (auto& node : snapshot.nodes) {
            if (node.type != UIA_ButtonControlTypeId ||
                node.className.find(L"sidebar-game-row") == std::wstring::npos) continue;
            const std::wstring candidate = normalizedGameName(node.name);
            if (candidate == requested) { game = &node; break; }
            candidates.emplace_back(editDistance(requested, candidate), &node);
        }
        if (!game && !candidates.empty()) {
            std::sort(candidates.begin(), candidates.end(),
                [](const auto& left, const auto& right) { return left.first < right.first; });
            const size_t maximumLength = (std::max)(requested.size(),
                normalizedGameName(candidates.front().second->name).size());
            const double similarity = maximumLength ?
                1.0 - static_cast<double>(candidates.front().first) / maximumLength : 0.0;
            const bool clearlyUnique = candidates.size() == 1 ||
                candidates[1].first >= candidates.front().first + 2;
            if (similarity >= 0.86 && clearlyUnique) game = candidates.front().second;
        }
        if (!game) { result.error = L"O jogo \"" + gameName + L"\" não foi encontrado na biblioteca visível do Wand."; return result; }
        const std::wstring matchedGameName = game->name;
        if (game->className.find(L"sidebar-game-row--current") == std::wstring::npos) {
            if (!invokeWithoutForeground(snapshot.window, *game)) {
                result.error = L"Não foi possível selecionar \"" + gameName + L"\" no Wand."; return result;
            }
        }
        bool selected = false;
        for (int attempt = 0; attempt < 12 && !selected; ++attempt) {
            if (attempt) {
                Sleep(250);
                Snapshot refreshed;
                if (refreshed.load()) snapshot.nodes = std::move(refreshed.nodes);
            }
            for (const auto& node : snapshot.nodes)
                if (node.type == UIA_ButtonControlTypeId &&
                    normalizedGameName(node.name) == normalizedGameName(matchedGameName) &&
                    node.className.find(L"sidebar-game-row--current") != std::wstring::npos) {
                    selected = true; break;
                }
        }
        if (!selected) { result.error = L"O Wand não confirmou a seleção de \"" + gameName + L"\"."; return result; }
    }

    // With the game page selected, attach the trainer to the already running game.
    Node* play = nullptr;
    if (!isTrainerActive(gamePid)) {
        for (int attempt = 0; attempt < 24 && !play; ++attempt) {
            for (auto& node : snapshot.nodes)
                if (node.type == UIA_ButtonControlTypeId &&
                    node.className.find(L"play-button__main-button") != std::wstring::npos) {
                    play = &node; break;
                }
            if (!play) {
                Sleep(250);
                Snapshot refreshed;
                if (refreshed.load()) snapshot.nodes = std::move(refreshed.nodes);
            }
        }
        if (!play) {
            result.error = L"A página do jogo abriu, mas o botão Jogar não ficou disponível.";
            return result;
        }
        if (!invokeWithoutForeground(snapshot.window, *play)) {
            result.error = L"Não foi possível ativar o trainer no Wand."; return result;
        }
        bool active = false;
        for (int attempt = 0; attempt < 40 && !(active = isTrainerActive(gamePid)); ++attempt)
            Sleep(250);
        if (gamePid && !active) {
            result.error = L"O Wand recebeu o Play, mas não confirmou a conexão com o jogo.";
            return result;
        }
    }

    // Page navigation and trainer attachment are asynchronous in Electron. Keep
    // refreshing until actual mod rows are present instead of trusting the first frame.
    int stableSnapshots = 0;
    for (int attempt = 0; attempt < 20 && stableSnapshots < 3; ++attempt) {
        if (attempt || play) {
            if (attempt) Sleep(250);
            Snapshot refreshed;
            if (refreshed.load()) snapshot.nodes = std::move(refreshed.nodes);
        }
        std::vector<WandMod> currentMods;
        RECT windowRect{}; GetWindowRect(snapshot.window, &windowRect);
        for (auto& label : snapshot.nodes) {
            const auto lowered = lower(label.name);
            if (label.type != UIA_TextControlTypeId || !valid(label.rect) || label.rect.left < windowRect.left + 240 ||
                lowered == L"info" || lowered == L"off" || lowered == L"on" || lowered.size() < 4) continue;
            if (Node* toggle = switchFor(label, snapshot.nodes)) {
                WandMod mod; mod.kind = WandMod::Kind::Switch; mod.name = label.name;
                mod.enabled = lower(toggle->className).find(L"checked") != std::wstring::npos;
                currentMods.push_back(std::move(mod));
            } else if (Node* value = valueFor(label, snapshot.nodes)) {
                IUIAutomationRangeValuePattern* range = nullptr;
                if (SUCCEEDED(value->element->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                                                    IID_PPV_ARGS(&range))) && range) {
                    WandMod mod; mod.name = label.name; mod.numeric = true;
                    range->get_CurrentValue(&mod.value); range->get_CurrentMinimum(&mod.minimum);
                    range->get_CurrentMaximum(&mod.maximum); range->get_CurrentSmallChange(&mod.step);
                    mod.kind = (std::floor(mod.step) == mod.step && std::floor(mod.value) == mod.value)
                        ? WandMod::Kind::Integer : WandMod::Kind::Slider;
                    if (mod.step <= 0) mod.step = 1;
                    range->Release(); currentMods.push_back(std::move(mod));
                }
            }
        }
        if (!currentMods.empty() && currentMods.size() == result.mods.size()) ++stableSnapshots;
        else stableSnapshots = 0;
        if (!currentMods.empty()) result.mods = std::move(currentMods);
    }
    if (result.mods.empty()) { result.error = L"Nenhum mod encontrado para " + gameName + L"."; return result; }
    result.success = true; return result;
}

bool WandIntegration::toggle(const std::wstring& modName, bool& enabled, std::wstring& error) {
    Snapshot snapshot; if (!snapshot.load()) { error = L"Janela do Wand indisponível."; return false; }
    Node* label = findText(modName, snapshot.nodes); Node* toggle = label ? switchFor(*label, snapshot.nodes) : nullptr;
    if (!toggle) { error = L"Mod não encontrado: " + modName; return false; }
    const bool before = lower(toggle->className).find(L"checked") != std::wstring::npos;
    scrollIntoView(*toggle);
    if (!backgroundClick(snapshot.window, *toggle)) { error = L"O Wand recusou o comando em segundo plano."; return false; }
    Sleep(350); Snapshot after;
    if (!after.load()) { error = L"Não foi possível confirmar o novo estado."; return false; }
    label = findText(modName, after.nodes); toggle = label ? switchFor(*label, after.nodes) : nullptr;
    enabled = toggle && lower(toggle->className).find(L"checked") != std::wstring::npos;
    if (!toggle || enabled == before) { error = L"O estado do mod não mudou."; return false; }
    return true;
}

bool WandIntegration::setValue(const std::wstring& modName, double requested, double& actual,
                               std::wstring& error) {
    Snapshot snapshot; if (!snapshot.load()) { error = L"Janela do Wand indisponível."; return false; }
    Node* label = findText(modName, snapshot.nodes); Node* value = label ? valueFor(*label, snapshot.nodes) : nullptr;
    IUIAutomationRangeValuePattern* range = nullptr;
    if (!value || FAILED(value->element->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                                               IID_PPV_ARGS(&range))) || !range) {
        error = L"Controle numérico não encontrado: " + modName; return false;
    }
    double minimum = 0, maximum = 0;
    range->get_CurrentMinimum(&minimum); range->get_CurrentMaximum(&maximum);
    requested = std::clamp(requested, minimum, maximum);
    const HRESULT changed = setValueWithoutForeground(snapshot.window, range, requested);
    range->Release();
    if (FAILED(changed)) { error = L"O Wand recusou o novo valor."; return false; }
    Sleep(150); Snapshot after;
    if (!after.load()) { error = L"Não foi possível confirmar o valor."; return false; }
    label = findText(modName, after.nodes); value = label ? valueFor(*label, after.nodes) : nullptr;
    if (!value || FAILED(value->element->GetCurrentPatternAs(UIA_RangeValuePatternId,
                                                               IID_PPV_ARGS(&range))) || !range) {
        error = L"O controle desapareceu antes da confirmação."; return false;
    }
    range->get_CurrentValue(&actual); range->Release();
    return std::abs(actual - requested) < 0.0001;
}
