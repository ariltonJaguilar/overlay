#include <windows.h>
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iostream>
#include <string>
#include <vector>

namespace {
struct Element {
    IUIAutomationElement* value{};
    std::wstring name;
    std::wstring className;
    CONTROLTYPEID type{};
    RECT bounds{};

    Element() = default;
    Element(const Element&) = delete;
    Element& operator=(const Element&) = delete;
    Element(Element&& other) noexcept
        : value(other.value), name(std::move(other.name)), className(std::move(other.className)),
          type(other.type), bounds(other.bounds) { other.value = nullptr; }
    Element& operator=(Element&& other) noexcept {
        if (this == &other) return *this;
        if (value) value->Release();
        value = other.value; other.value = nullptr;
        name = std::move(other.name); className = std::move(other.className);
        type = other.type; bounds = other.bounds;
        return *this;
    }
    ~Element() { if (value) value->Release(); }
};

std::wstring takeBstr(BSTR raw) {
    std::wstring result = raw ? std::wstring(raw, SysStringLen(raw)) : L"";
    SysFreeString(raw);
    return result;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

BOOL CALLBACK findWandWindow(HWND window, LPARAM parameter) {
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[256]{}, className[256]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    if (lower(title) == L"wand" && std::wstring(className) == L"Chrome_WidgetWin_1") {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

void flatten(IUIAutomationTreeWalker* walker, IUIAutomationElement* current,
             std::vector<Element>& output, int depth = 0) {
    if (!current || depth > 20) return;
    Element item;
    current->AddRef();
    item.value = current;
    BSTR raw = nullptr;
    current->get_CurrentName(&raw); item.name = takeBstr(raw);
    raw = nullptr; current->get_CurrentClassName(&raw); item.className = takeBstr(raw);
    current->get_CurrentControlType(&item.type);
    current->get_CurrentBoundingRectangle(&item.bounds);
    output.push_back(std::move(item));

    IUIAutomationElement* child = nullptr;
    if (FAILED(walker->GetFirstChildElement(current, &child))) return;
    while (child) {
        flatten(walker, child, output, depth + 1);
        IUIAutomationElement* next = nullptr;
        walker->GetNextSiblingElement(child, &next);
        child->Release();
        child = next;
    }
}

int centerY(const RECT& rect) { return rect.top + (rect.bottom - rect.top) / 2; }

bool validRect(const RECT& rect) {
    return rect.right > rect.left && rect.bottom > rect.top;
}

bool contains(const RECT& outer, const RECT& inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool looksLikeSwitch(const Element& candidate, const std::vector<Element>& elements) {
    bool off = false, on = false;
    for (const auto& child : elements) {
        if (child.type != UIA_TextControlTypeId || !contains(candidate.bounds, child.bounds)) continue;
        const auto name = lower(child.name);
        off = off || name == L"off";
        on = on || name == L"on";
    }
    return off && on;
}

Element* switchFor(const Element& label, std::vector<Element>& elements) {
    Element* best = nullptr;
    int bestScore = INT_MAX;
    for (auto& candidate : elements) {
        const int width = candidate.bounds.right - candidate.bounds.left;
        const int height = candidate.bounds.bottom - candidate.bounds.top;
        if (candidate.type != UIA_GroupControlTypeId || !validRect(candidate.bounds)) continue;
        if (candidate.bounds.left <= label.bounds.right || width < 50 || width > 160 || height < 18 || height > 50) continue;
        if (candidate.className.find(L"au-target") == std::wstring::npos) continue;
        if (!looksLikeSwitch(candidate, elements)) continue;
        const int dy = std::abs(centerY(candidate.bounds) - centerY(label.bounds));
        if (dy > 12) continue;
        const int score = dy * 1000 + candidate.bounds.left - label.bounds.right;
        if (score < bestScore) { best = &candidate; bestScore = score; }
    }
    return best;
}

Element* valueFor(const Element& label, std::vector<Element>& elements) {
    Element* best = nullptr;
    int bestScore = INT_MAX;
    for (auto& candidate : elements) {
        if (candidate.type != UIA_SpinnerControlTypeId && candidate.type != UIA_SliderControlTypeId) continue;
        if (!validRect(candidate.bounds) || candidate.bounds.left <= label.bounds.right) continue;
        const int dy = std::abs(centerY(candidate.bounds) - centerY(label.bounds));
        if (dy > 14) continue;
        const int score = dy * 1000 + candidate.bounds.left - label.bounds.right;
        if (score < bestScore) { best = &candidate; bestScore = score; }
    }
    return best;
}

Element* findLabel(const std::wstring& requested, std::vector<Element>& elements) {
    const auto wanted = lower(requested);
    Element* partial = nullptr;
    for (auto& element : elements) {
        if (element.type != UIA_TextControlTypeId || element.name.empty()) continue;
        const auto candidate = lower(element.name);
        if (candidate == wanted) return &element;
        if (!partial && candidate.find(wanted) != std::wstring::npos) partial = &element;
    }
    return partial;
}

bool isChecked(const Element& element) {
    return lower(element.className).find(L"checked") != std::wstring::npos;
}

HRESULT invokeLegacy(Element& element) {
    IUIAutomationLegacyIAccessiblePattern* legacy = nullptr;
    const HRESULT query = element.value->GetCurrentPatternAs(
        UIA_LegacyIAccessiblePatternId, IID_PPV_ARGS(&legacy));
    if (FAILED(query) || !legacy) return query;
    const HRESULT result = legacy->DoDefaultAction();
    legacy->Release();
    return result;
}

bool clickInBackground(HWND wandWindow, const Element& element) {
    if (!validRect(element.bounds)) return false;
    POINT target{
        element.bounds.left + (element.bounds.right - element.bounds.left) / 2,
        element.bounds.top + (element.bounds.bottom - element.bounds.top) / 2
    };
    if (!ScreenToClient(wandWindow, &target)) return false;
    DWORD_PTR ignored = 0;
    const LPARAM position = MAKELPARAM(target.x, target.y);
    SendMessageTimeoutW(wandWindow, WM_MOUSEMOVE, 0, position,
                        SMTO_ABORTIFHUNG, 250, &ignored);
    if (!SendMessageTimeoutW(wandWindow, WM_LBUTTONDOWN, MK_LBUTTON, position,
                             SMTO_ABORTIFHUNG, 250, &ignored)) return false;
    return SendMessageTimeoutW(wandWindow, WM_LBUTTONUP, 0, position,
                               SMTO_ABORTIFHUNG, 250, &ignored) != 0;
}

bool clickInForeground(HWND wandWindow, const Element& element) {
    if (!validRect(element.bounds)) return false;
    const POINT target{
        element.bounds.left + (element.bounds.right - element.bounds.left) / 2,
        element.bounds.top + (element.bounds.bottom - element.bounds.top) / 2
    };
    POINT previousCursor{};
    GetCursorPos(&previousCursor);
    const HWND previousForeground = GetForegroundWindow();
    ShowWindow(wandWindow, SW_SHOWNOACTIVATE);
    SetForegroundWindow(wandWindow);
    SetCursorPos(target.x, target.y);
    INPUT input[2]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[1].type = INPUT_MOUSE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const bool sent = SendInput(2, input, sizeof(INPUT)) == 2;
    Sleep(60);
    SetCursorPos(previousCursor.x, previousCursor.y);
    if (previousForeground && previousForeground != wandWindow) SetForegroundWindow(previousForeground);
    return sent;
}

bool refresh(IUIAutomation* automation, IUIAutomationTreeWalker* walker, HWND window,
             std::vector<Element>& elements) {
    elements.clear();
    IUIAutomationElement* root = nullptr;
    if (FAILED(automation->ElementFromHandle(window, &root)) || !root) return false;
    flatten(walker, root, elements);
    root->Release();
    return true;
}

bool plausibleModLabel(const Element& label, const RECT& wandBounds) {
    const auto name = lower(label.name);
    if (name == L"info" || name == L"off" || name == L"on" || name.size() < 4) return false;
    // The game content begins to the right of Wand's navigation sidebar.
    return label.bounds.left >= wandBounds.left + 240;
}

void printList(std::vector<Element>& elements, const RECT& wandBounds) {
    bool found = false;
    for (auto& label : elements) {
        if (label.type != UIA_TextControlTypeId || label.name.empty() || !validRect(label.bounds) ||
            !plausibleModLabel(label, wandBounds)) continue;
        if (Element* toggle = switchFor(label, elements)) {
            std::wcout << L"  " << label.name << L" [" << (isChecked(*toggle) ? L"ON" : L"OFF") << L"]\n";
            found = true;
        } else if (Element* value = valueFor(label, elements)) {
            IUIAutomationRangeValuePattern* range = nullptr;
            if (SUCCEEDED(value->value->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) && range) {
                double current = 0, minimum = 0, maximum = 0;
                range->get_CurrentValue(&current); range->get_CurrentMinimum(&minimum); range->get_CurrentMaximum(&maximum);
                std::wcout << L"  " << label.name << L" [valor=" << current << L", min=" << minimum
                           << L", max=" << maximum << L"]\n";
                range->Release(); found = true;
            }
        }
    }
    if (!found) std::wcout << L"  Nenhum mod visivel. Abra a pagina do trainer no Wand.\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::wcerr << L"Uso:\n  WandControlProbe list\n  WandControlProbe toggle \"Nome do mod\"\n"
                      L"  WandControlProbe set \"Nome do mod\" valor\n";
        return 1;
    }
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) { std::wcerr << L"Falha ao inicializar COM.\n"; return 2; }
    IUIAutomation* automation = nullptr;
    IUIAutomationTreeWalker* walker = nullptr;
    HWND window = nullptr;
    EnumWindows(findWandWindow, reinterpret_cast<LPARAM>(&window));
    if (!window || FAILED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(&automation))) || !automation ||
        FAILED(automation->get_ControlViewWalker(&walker)) || !walker) {
        std::wcerr << L"Janela visivel do Wand nao encontrada ou UI Automation indisponivel.\n";
        if (walker) walker->Release(); if (automation) automation->Release(); CoUninitialize(); return 3;
    }
    std::vector<Element> elements;
    refresh(automation, walker, window, elements);
    RECT wandBounds{};
    GetWindowRect(window, &wandBounds);
    const std::wstring command = lower(argv[1]);
    int exitCode = 0;
    if (command == L"list") {
        std::wcout << L"Mods visiveis no Wand:\n";
        printList(elements, wandBounds);
    } else if (command == L"toggle" && argc >= 3) {
        Element* label = findLabel(argv[2], elements);
        Element* toggle = label ? switchFor(*label, elements) : nullptr;
        if (!label || !toggle) {
            std::wcerr << L"Mod/switch nao encontrado: " << argv[2] << L"\n"; exitCode = 4;
        } else {
            const bool before = isChecked(*toggle);
            HRESULT action = invokeLegacy(*toggle);
            std::wstring mechanism = L"LegacyIAccessible";
            if (FAILED(action)) {
                if (clickInBackground(window, *toggle)) {
                    action = S_OK;
                    mechanism = L"UIA+mensagem em background";
                }
            }
            Sleep(500);
            refresh(automation, walker, window, elements);
            label = findLabel(argv[2], elements); toggle = label ? switchFor(*label, elements) : nullptr;
            if (SUCCEEDED(action) && toggle && isChecked(*toggle) == before &&
                mechanism == L"UIA+mensagem em background") {
                if (clickInForeground(window, *toggle)) {
                    mechanism = L"UIA+click dinamico em foreground";
                    Sleep(500);
                    refresh(automation, walker, window, elements);
                    label = findLabel(argv[2], elements);
                    toggle = label ? switchFor(*label, elements) : nullptr;
                }
            }
            if (FAILED(action) || !toggle || isChecked(*toggle) == before) {
                std::wcerr << L"A acao nao foi confirmada pelo estado do Wand. HRESULT=0x"
                           << std::hex << action << std::dec << L"\n"; exitCode = 5;
            } else {
                std::wcout << label->name << L": " << (isChecked(*toggle) ? L"ON" : L"OFF")
                           << L" (mecanismo=" << mechanism << L")\n";
            }
        }
    } else if (command == L"set" && argc >= 4) {
        Element* label = findLabel(argv[2], elements);
        Element* value = label ? valueFor(*label, elements) : nullptr;
        wchar_t* end = nullptr;
        const double requested = std::wcstod(argv[3], &end);
        IUIAutomationRangeValuePattern* range = nullptr;
        if (!label || !value || end == argv[3] || *end != L'\0' ||
            FAILED(value->value->GetCurrentPatternAs(UIA_RangeValuePatternId, IID_PPV_ARGS(&range))) || !range) {
            std::wcerr << L"Mod numerico/valor invalido: " << argv[2] << L"\n"; exitCode = 6;
        } else {
            double minimum = 0, maximum = 0, current = 0;
            range->get_CurrentMinimum(&minimum); range->get_CurrentMaximum(&maximum);
            if (requested < minimum || requested > maximum || FAILED(range->SetValue(requested))) {
                std::wcerr << L"Valor fora do intervalo ou recusado (" << minimum << L".." << maximum << L").\n"; exitCode = 7;
            } else {
                Sleep(150); range->get_CurrentValue(&current);
                std::wcout << label->name << L": " << current << L"\n";
            }
            range->Release();
        }
    } else {
        std::wcerr << L"Comando invalido. Use list, toggle ou set.\n"; exitCode = 1;
    }
    elements.clear(); walker->Release(); automation->Release(); CoUninitialize();
    return exitCode;
}
