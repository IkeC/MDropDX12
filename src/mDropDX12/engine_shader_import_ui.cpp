// engine_shader_import_ui.cpp — Shader Import ToolWindow implementation.
//
// Provides paste GLSL → auto-convert to HLSL → live preview → save as .milk preset.
// GLSL→HLSL conversion ported from Milkwave Remote ShaderHelper.cs.

#include "tool_window.h"
#include "engine.h"
#include "state.h"
#include "utility.h"
#include "md_defines.h"
#include <CommCtrl.h>
#include <commdlg.h>
#include <sstream>
#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace mdrop {

// ─── Open / Close (Engine methods) ──────────────────────────────────────

void Engine::OpenShaderImportWindow() {
    if (!m_shaderImportWindow)
        m_shaderImportWindow = std::make_unique<ShaderImportWindow>(this);
    m_shaderImportWindow->Open();
}

void Engine::CloseShaderImportWindow() {
    if (m_shaderImportWindow)
        m_shaderImportWindow->Close();
}

// ─── Constructor ────────────────────────────────────────────────────────

ShaderImportWindow::ShaderImportWindow(Engine* pEngine)
    : ToolWindow(pEngine, 700, 800)
{
}

// ─── Build Controls ─────────────────────────────────────────────────────

void ShaderImportWindow::DoBuildControls() {
    HWND hw = m_hWnd;
    auto L = BuildBaseControls();
    HFONT hFont = GetFont();
    m_nTopY = L.y;

    RECT rc;
    GetClientRect(hw, &rc);
    int clientH = rc.bottom - rc.top;

    int x = L.x, rw = L.rw, lineH = L.lineH, gap = L.gap;
    int y = m_nTopY;
    int btnW = MulDiv(80, lineH, 26);
    int btnH = lineH + 4;
    int pad = 8;

    // Calculate proportional edit heights from available space
    int fixedH = (lineH + gap) * 3  // 3 label rows
               + (btnH + gap)       // Convert button row
               + (btnH + gap)       // bottom buttons row
               + pad;
    int editH = clientH - y - fixedH;
    if (editH < 120) editH = 120;
    int glslH = editH * 38 / 100;
    int hlslH = editH * 38 / 100;
    int errH  = editH - glslH - hlslH;
    if (errH < 40) errH = 40;

    // "GLSL Input:" label + Paste / Clear buttons
    TrackControl(CreateWindowExW(0, L"STATIC", L"GLSL Input:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Paste", IDC_MW_SHIMPORT_PASTE,
        x + rw - btnW * 2 - 8, y - 2, btnW, btnH, hFont));
    TrackControl(CreateBtn(hw, L"Clear", IDC_MW_SHIMPORT_CLEAR,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // GLSL multiline edit
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, glslH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_GLSL_EDIT, NULL, NULL));
    y += glslH + gap;

    // Convert button
    TrackControl(CreateBtn(hw, L"Convert >>", IDC_MW_SHIMPORT_CONVERT,
        x, y, btnW + 20, btnH, hFont));
    y += btnH + gap;

    // "HLSL Output:" label + Copy button
    TrackControl(CreateWindowExW(0, L"STATIC", L"HLSL Output:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    TrackControl(CreateBtn(hw, L"Copy", IDC_MW_SHIMPORT_COPY,
        x + rw - btnW, y - 2, btnW, btnH, hFont));
    y += lineH + gap;

    // HLSL multiline edit
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
        WS_VSCROLL | WS_HSCROLL | ES_NOHIDESEL,
        x, y, rw, hlslH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_HLSL_EDIT, NULL, NULL));
    y += hlslH + gap;

    // "Errors:" label
    TrackControl(CreateWindowExW(0, L"STATIC", L"Errors:", WS_CHILD | WS_VISIBLE,
        x, y, 100, lineH, hw, NULL, NULL, NULL));
    y += lineH + gap;

    // Error multiline edit (read-only)
    TrackControl(CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
        WS_VSCROLL,
        x, y, rw, errH, hw, (HMENU)(INT_PTR)IDC_MW_SHIMPORT_ERROR_EDIT, NULL, NULL));
    y += errH + gap;

    // Bottom buttons: Apply, Save
    TrackControl(CreateBtn(hw, L"Apply", IDC_MW_SHIMPORT_APPLY,
        x, y, btnW, btnH, hFont));
    int saveW = MulDiv(140, lineH, 26);
    TrackControl(CreateBtn(hw, L"Save as .milk...", IDC_MW_SHIMPORT_SAVE,
        x + rw - saveW, y, saveW, btnH, hFont));

    // Set monospace font on code edits
    HFONT hMono = CreateFontW(-MulDiv(10, GetDpiForWindow(hw), 72), 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (hMono) {
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_GLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
        SendDlgItemMessageW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, WM_SETFONT, (WPARAM)hMono, TRUE);
    }
}

// ─── Layout ─────────────────────────────────────────────────────────────

void ShaderImportWindow::OnResize() {
    RebuildFonts();
}

void ShaderImportWindow::LayoutControls() {
    // All layout is handled by DoBuildControls; OnResize calls RebuildFonts.
}

// ─── Command Handler ────────────────────────────────────────────────────

LRESULT ShaderImportWindow::DoCommand(HWND hWnd, int id, int code, LPARAM lParam) {
    Engine* p = m_pEngine;

    if (code == BN_CLICKED) {
        switch (id) {
        case IDC_MW_SHIMPORT_PASTE: {
            // Paste from clipboard into GLSL edit, then auto-convert
            if (OpenClipboard(hWnd)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pText = (wchar_t*)GlobalLock(hData);
                    if (pText) {
                        SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_GLSL_EDIT, pText);
                        GlobalUnlock(hData);
                    }
                }
                CloseClipboard();
            }
            ConvertGLSLtoHLSL();
            return 0;
        }
        case IDC_MW_SHIMPORT_CLEAR:
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_GLSL_EDIT, L"");
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT, L"");
            SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_ERROR_EDIT, L"");
            return 0;

        case IDC_MW_SHIMPORT_CONVERT:
            ConvertGLSLtoHLSL();
            return 0;

        case IDC_MW_SHIMPORT_COPY: {
            // Copy HLSL to clipboard
            int len = GetWindowTextLengthW(GetDlgItem(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT));
            if (len > 0) {
                std::wstring text(len + 1, L'\0');
                GetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_HLSL_EDIT, text.data(), len + 1);
                if (OpenClipboard(hWnd)) {
                    EmptyClipboard();
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
                    if (hMem) {
                        memcpy(GlobalLock(hMem), text.c_str(), (len + 1) * sizeof(wchar_t));
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    CloseClipboard();
                }
            }
            return 0;
        }
        case IDC_MW_SHIMPORT_APPLY:
            ApplyShader();
            return 0;

        case IDC_MW_SHIMPORT_SAVE:
            SaveAsPreset();
            return 0;
        }
    }
    return -1;
}

LRESULT ShaderImportWindow::DoMessage(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TIMER && wParam == 1) {
        // Check compilation result after Apply
        KillTimer(hWnd, 1);
        Engine* p = m_pEngine;

        std::wstring errText;
        if (p->m_shaders.comp.ptr != NULL) {
            errText = L"Shader compiled successfully.";
        } else {
            errText = L"Compilation failed.\r\n";
            // Try to read diagnostic file
            wchar_t diagPath[MAX_PATH];
            swprintf(diagPath, MAX_PATH, L"%lsdiag_comp_shader.txt", p->m_szBaseDir);
            FILE* f = _wfopen(diagPath, L"r");
            if (f) {
                char buf[4096] = {};
                size_t n = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                buf[n] = '\0';
                // Convert to wide string
                int wlen = MultiByteToWideChar(CP_ACP, 0, buf, -1, NULL, 0);
                if (wlen > 0) {
                    std::wstring wbuf(wlen, L'\0');
                    MultiByteToWideChar(CP_ACP, 0, buf, -1, wbuf.data(), wlen);
                    errText += wbuf;
                }
            }
        }
        SetDlgItemTextW(hWnd, IDC_MW_SHIMPORT_ERROR_EDIT, errText.c_str());
        return 0;
    }
    return -1;
}

// ─── Apply (live preview) ───────────────────────────────────────────────

void ShaderImportWindow::ApplyShader() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    // Get HLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_HLSL_EDIT));
    if (len <= 0) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No HLSL text to apply.");
        return;
    }
    std::wstring hlslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, hlslW.data(), len + 1);

    // Convert wide → narrow, replacing \r\n with LINEFEED_CONTROL_CHAR
    std::string hlsl;
    hlsl.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = hlslW[i];
        if (ch == L'\r') {
            if (i + 1 < len && hlslW[i + 1] == L'\n') i++; // skip \n after \r
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch == L'\n') {
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch < 128) {
            hlsl += (char)ch;
        } else {
            hlsl += '?';
        }
    }

    // Write into state
    strncpy_s(p->m_pState->m_szCompShadersText, MAX_BIGSTRING_LEN, hlsl.c_str(), _TRUNCATE);
    p->m_pState->m_nCompPSVersion = 3; // ps_3_0
    p->m_pState->m_nMaxPSVersion = max(p->m_pState->m_nWarpPSVersion, 3);

    // Enqueue recompile
    p->EnqueueRenderCmd(RenderCmd::RecompileCompShader);

    // Check result after a short delay
    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Compiling...");
    SetTimer(hw, 1, 500, NULL);
}

// ─── Save as .milk ──────────────────────────────────────────────────────

void ShaderImportWindow::SaveAsPreset() {
    HWND hw = m_hWnd;
    Engine* p = m_pEngine;

    // Get HLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_HLSL_EDIT));
    if (len <= 0) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"No HLSL text to save.");
        return;
    }
    std::wstring hlslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, hlslW.data(), len + 1);

    // File save dialog
    wchar_t filePath[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hw;
    ofn.lpstrFilter = L"MilkDrop Preset (*.milk)\0*.milk\0All Files\0*.*\0";
    ofn.lpstrFile = filePath;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = p->GetPresetDir();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"milk";

    if (!GetSaveFileNameW(&ofn))
        return;

    // Convert wide → narrow with LINEFEED_CONTROL_CHAR
    std::string hlsl;
    hlsl.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = hlslW[i];
        if (ch == L'\r') {
            if (i + 1 < len && hlslW[i + 1] == L'\n') i++;
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch == L'\n') {
            hlsl += (char)LINEFEED_CONTROL_CHAR;
        } else if (ch < 128) {
            hlsl += (char)ch;
        } else {
            hlsl += '?';
        }
    }

    // Heap-allocate CState (too large for stack — ~200KB+ of char arrays)
    auto tempState = std::make_unique<CState>();
    tempState->Default(0xFFFFFFFF);
    tempState->m_nCompPSVersion = 3;
    tempState->m_nWarpPSVersion = 0;
    tempState->m_nMaxPSVersion = 3;
    tempState->m_nMinPSVersion = 0;
    strncpy_s(tempState->m_szCompShadersText, MAX_BIGSTRING_LEN, hlsl.c_str(), _TRUNCATE);

    if (tempState->Export(filePath)) {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Preset saved successfully.");
    } else {
        SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, L"Failed to save preset.");
    }
}

// ─── GLSL→HLSL Conversion ──────────────────────────────────────────────
// Ported from Milkwave Remote ShaderHelper.cs

// Helper: replace variable name patterns in various contexts
std::string ShaderImportWindow::ReplaceVarName(const std::string& oldName, const std::string& newName, const std::string& inp) {
    std::string res = inp;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(res, " " + oldName + " ", " " + newName + " ");
    replaceAll(res, oldName + ".", newName + ".");
    replaceAll(res, "(" + oldName + "-", "(" + newName + "-");
    replaceAll(res, "(" + oldName + ",", "(" + newName + ",");
    replaceAll(res, "," + oldName + ")", "," + newName + ")");
    replaceAll(res, ", " + oldName + ")", ", " + newName + ")");
    replaceAll(res, "(" + oldName + ")", "(" + newName + ")");
    replaceAll(res, oldName + "=", newName + "=");
    replaceAll(res, oldName + "*", newName + "*");
    replaceAll(res, "*" + oldName, "*" + newName);
    replaceAll(res, oldName + " =", newName + " =");
    replaceAll(res, oldName + "+", newName + "+");
    replaceAll(res, oldName + " +", newName + " +");
    replaceAll(res, oldName + ";", newName + ";");
    replaceAll(res, "float2 " + oldName + ",", "float2 " + newName + ", ");
    replaceAll(res, "float2 " + oldName + ";", "float2 " + newName + "; ");
    replaceAll(res, "float2 " + oldName + " ", "float2 " + newName + " ");
    replaceAll(res, "float2 " + oldName + ")", "float2 " + newName + ")");
    return res;
}

// Find closing bracket matching open/close at given nesting level
int ShaderImportWindow::FindClosingBracket(const std::string& input, char open, char close, int startLevel) {
    int level = startLevel;
    for (int i = 0; i < (int)input.size(); i++) {
        if (input[i] == open) level++;
        else if (input[i] == close) {
            level--;
            if (level == 0) return i;
        }
    }
    return -1;
}

// Fix matrix multiplication: *=mat2(...) → = mul(x, transpose(float2x2(...)))
std::string ShaderImportWindow::FixMatrixMultiplication(const std::string& inputLine) {
    std::string result = inputLine;
    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    try {
        replaceAll(result, "*= mat", "*=mat");
        replaceAll(result, "* mat", "*mat");
        replaceAll(result, " *mat", "*mat");

        std::string token = "*=mat";
        size_t index = result.find(token);
        if (index != std::string::npos) {
            // e.g. "uv *= mat2(cos(a), -sin(a), sin(a), cos(a));"
            char matSizeChar = result[index + token.size()];
            if (matSizeChar >= '2' && matSizeChar <= '4') {
                int matSize = matSizeChar - '0';
                std::string fac1 = result.substr(0, index);
                // Trim right
                while (!fac1.empty() && fac1.back() == ' ') fac1.pop_back();
                std::string indent_str;
                size_t fac1Start = fac1.find_first_not_of(" \t");
                if (fac1Start != std::string::npos)
                    indent_str = fac1.substr(0, fac1Start);
                fac1 = fac1.substr(fac1Start != std::string::npos ? fac1Start : 0);

                std::string rest = result.substr(index + token.size() + 2); // skip "matN("
                int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                if (closingIdx > 0) {
                    std::string args = rest.substr(0, closingIdx);
                    result = indent_str + fac1 + " = mul(" + fac1 + ", transpose(float"
                           + std::to_string(matSize) + "x" + std::to_string(matSize)
                           + "(" + args + ")));";
                }
            }
        } else {
            token = "*mat";
            index = result.find(token);
            if (index != std::string::npos) {
                char matSizeChar = result[index + token.size()];
                if (matSizeChar >= '2' && matSizeChar <= '4') {
                    int matSize = matSizeChar - '0';
                    std::string prefix = result.substr(0, index);
                    // Trim to get fac1 (last word before *mat)
                    while (!prefix.empty() && prefix.back() == ' ') prefix.pop_back();
                    size_t lastSpace = prefix.rfind(' ');
                    std::string fac1 = (lastSpace != std::string::npos) ? prefix.substr(lastSpace + 1) : prefix;
                    std::string left = (lastSpace != std::string::npos) ? prefix.substr(0, lastSpace + 1) : "";

                    std::string rest = result.substr(index + token.size() + 2);
                    int closingIdx = FindClosingBracket(rest, '(', ')', 1);
                    if (closingIdx > 0) {
                        std::string args = rest.substr(0, closingIdx);
                        result = left + "mul(" + fac1 + ", transpose(float"
                               + std::to_string(matSize) + "x" + std::to_string(matSize)
                               + "(" + args + ")));";
                    }
                }
            }
        }

        // Replace remaining mat types
        replaceAll(result, "mat2(", "float2x2(");
        replaceAll(result, "mat3(", "float3x3(");
        replaceAll(result, "mat4(", "float4x4(");
        replaceAll(result, "mat2 ", "float2x2 ");
        replaceAll(result, "mat3 ", "float3x3 ");
        replaceAll(result, "mat4 ", "float4x4 ");
    } catch (...) {
        return inputLine;
    }
    return result;
}

// Fix float2/3/4 single-argument expansion: float3(1) → float3(1,1,1)
std::string ShaderImportWindow::FixFloatNumberOfArguments(const std::string& inputLine, const std::string& fullContext) {
    std::string result = inputLine;
    for (int numArgs = 2; numArgs <= 4; numArgs++) {
        std::string prefix = "float" + std::to_string(numArgs) + "(";
        size_t index = result.find(prefix);
        if (index != std::string::npos) {
            std::string rest = result.substr(index + prefix.size());
            int closingIdx = FindClosingBracket(rest, '(', ')', 1);
            if (closingIdx > 0) {
                std::string argsLine = rest.substr(0, closingIdx);
                // Check if only one argument (no commas at top level)
                if (argsLine.find(',') == std::string::npos) {
                    // Check if it's a number, function call, or known float variable
                    bool shouldExpand = false;
                    // Is it a numeric literal?
                    try {
                        size_t pos;
                        (void)std::stof(argsLine, &pos);
                        if (pos == argsLine.size()) shouldExpand = true;
                    } catch (...) {}
                    // Is it a function call (contains parens)?
                    if (!shouldExpand && argsLine.find('(') != std::string::npos && argsLine.find(')') != std::string::npos)
                        shouldExpand = true;
                    // Is it a known float variable in context?
                    if (!shouldExpand && (fullContext.find("float " + argsLine + ",") != std::string::npos ||
                                          fullContext.find("float " + argsLine + ";") != std::string::npos))
                        shouldExpand = true;

                    if (shouldExpand) {
                        std::string expanded = argsLine;
                        for (int i = 1; i < numArgs; i++)
                            expanded += ", " + argsLine;
                        result = result.substr(0, index + prefix.size())
                               + expanded
                               + result.substr(index + prefix.size() + closingIdx);
                    }
                }
            }
        }
    }
    return result;
}

// Fix two-argument atan(y,x) → atan2(y,x)
std::string ShaderImportWindow::FixAtan(const std::string& line) {
    std::string result = line;
    size_t index = result.find("atan(");
    if (index != std::string::npos) {
        std::string rest = result.substr(index + 5);
        int closingIdx = FindClosingBracket(rest, '(', ')', 1);
        if (closingIdx > 0) {
            std::string args = rest.substr(0, closingIdx);
            if (args.find(',') != std::string::npos) {
                // Two arguments → atan2
                auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
                    size_t pos = 0;
                    while ((pos = s.find(from, pos)) != std::string::npos) {
                        s.replace(pos, from.size(), to);
                        pos += to.size();
                    }
                };
                replaceAll(result, "atan(", "atan2(");
            }
        }
    }
    return result;
}

// Basic code formatter: consistent indentation, blank line cleanup
std::string ShaderImportWindow::BasicFormatShaderCode(const std::string& code) {
    std::string src = code;
    // Normalize else placement
    {
        auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
            size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replaceAll(src, "}else", "}\nelse");
        replaceAll(src, "} else", "}\nelse");
    }

    int indentSize = 2;
    std::istringstream iss(src);
    std::string rawLine;
    std::ostringstream out;
    int indentLevel = 0;
    bool lastLineWasEmpty = false;

    std::vector<std::string> lines;
    while (std::getline(iss, rawLine)) {
        // Remove trailing \r
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();
        lines.push_back(rawLine);
    }

    for (size_t i = 0; i < lines.size(); i++) {
        std::string line = lines[i];
        // Trim
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) {
            if (!lastLineWasEmpty) {
                out << "\n";
                lastLineWasEmpty = true;
            }
            continue;
        }
        line = line.substr(start);
        lastLineWasEmpty = false;

        // Decrease indent for closing brace
        if (line == "}")
            indentLevel = max(indentLevel - 1, 0);

        std::string indent_str(indentLevel * indentSize, ' ');

        // Handle inline comments
        size_t commentIdx = line.find("//");
        if (commentIdx != std::string::npos && commentIdx > 0) {
            std::string codePart = line.substr(0, commentIdx);
            while (!codePart.empty() && codePart.back() == ' ') codePart.pop_back();
            std::string commentPart = line.substr(commentIdx + 2);
            while (!commentPart.empty() && commentPart.front() == ' ') commentPart.erase(commentPart.begin());
            commentPart = "// " + commentPart;

            if (!codePart.empty())
                out << indent_str << codePart << "\n";
            out << indent_str << commentPart << "\n";
        } else {
            out << indent_str << line << "\n";
        }

        // Add blank line after lone '}'
        if (line == "}" && i + 1 < lines.size()) {
            std::string next = lines[i + 1];
            size_t ns = next.find_first_not_of(" \t");
            if (ns != std::string::npos)
                out << "\n";
        }

        // Increase indent after lines ending with '{'
        if (!line.empty() && line.back() == '{')
            indentLevel++;
    }

    // Trim leading/trailing blank lines
    std::string result = out.str();
    while (!result.empty() && result.front() == '\n') result.erase(result.begin());
    while (result.size() > 1 && result[result.size() - 1] == '\n' && result[result.size() - 2] == '\n')
        result.erase(result.size() - 1);

    return result;
}

// ─── Main Conversion ────────────────────────────────────────────────────

void ShaderImportWindow::ConvertGLSLtoHLSL() {
    HWND hw = m_hWnd;

    // Get GLSL text
    int len = GetWindowTextLengthW(GetDlgItem(hw, IDC_MW_SHIMPORT_GLSL_EDIT));
    if (len <= 0) return;

    std::wstring glslW(len + 1, L'\0');
    GetDlgItemTextW(hw, IDC_MW_SHIMPORT_GLSL_EDIT, glslW.data(), len + 1);

    // Convert wide → narrow for processing
    std::string inp;
    inp.reserve(len);
    for (int i = 0; i < len; i++) {
        wchar_t ch = glslW[i];
        if (ch < 128) inp += (char)ch;
        else inp += '?';
    }

    std::string errors;
    std::string result;

    auto replaceAll = [](std::string& s, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };

    try {
        // Phase 1: Global replacements
        replaceAll(inp, "vec2", "float2");
        replaceAll(inp, "vec3", "float3");
        replaceAll(inp, "vec4", "float4");
        replaceAll(inp, "fract (", "fract(");
        replaceAll(inp, "mod (", "mod(");
        replaceAll(inp, "mix (", "mix(");
        replaceAll(inp, "fract(", "frac(");
        replaceAll(inp, "mod(", "mod_conv(");
        replaceAll(inp, "mix(", "lerp(");
        inp = ReplaceVarName("time", "time_conv", inp);
        replaceAll(inp, "refrac(", "refract("); // undo damage to refract
        replaceAll(inp, "iTimeDelta", "xTimeDelta"); // protect from iTime replace
        replaceAll(inp, "iTime", "time");
        replaceAll(inp, "iResolution", "uv");
        replaceAll(inp, "iFrame", "frame");
        replaceAll(inp, "iMouse", "mouse");
        replaceAll(inp, "texture(", "tex2D(");
        replaceAll(inp, "highp ", "");
        replaceAll(inp, "void mainImage(", "mainImage(");
        replaceAll(inp, "atan (", "atan(");

        // Phase 2: Extract mainImage
        size_t indexMainImage = inp.find("mainImage(");

        std::string inpHeader;
        std::string inpMain;
        std::string retVarName = "fragColor";

        if (indexMainImage == std::string::npos) {
            // No mainImage — wrap everything
            inpMain = inp + "\n}";
        } else {
            std::string afterMain = inp.substr(indexMainImage);
            int closingIdx = FindClosingBracket(afterMain, '{', '}', 0);

            inpHeader = inp.substr(0, indexMainImage);

            if (closingIdx > 0) {
                inpMain = inp.substr(indexMainImage, closingIdx + 1);
            } else {
                inpMain = inp.substr(indexMainImage);
            }

            // Footer (anything after mainImage closing brace)
            std::string inpFooter;
            size_t footerStart = indexMainImage + (closingIdx > 0 ? closingIdx + 1 : afterMain.size());
            if (footerStart < inp.size())
                inpFooter = inp.substr(footerStart);
            inpHeader += inpFooter;

            // Strip comments and blank lines from header
            {
                std::istringstream hss(inpHeader);
                std::string line;
                std::ostringstream clean;
                bool inComment = false;
                std::string prevLine;
                while (std::getline(hss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    std::string trimmed = line;
                    size_t s = trimmed.find_first_not_of(" \t");
                    if (s != std::string::npos) trimmed = trimmed.substr(s);
                    else trimmed = "";

                    if (trimmed.substr(0, 2) == "//") continue;
                    if (trimmed.substr(0, 2) == "/*") {
                        inComment = (trimmed.find("*/") == std::string::npos);
                        continue;
                    }
                    if (trimmed.find("*/") != std::string::npos) {
                        inComment = false;
                        continue;
                    }
                    if (inComment) continue;
                    if (!trimmed.empty() || !prevLine.empty()) {
                        clean << trimmed << "\n";
                    }
                    prevLine = trimmed;
                }
                inpHeader = clean.str();
            }
        }

        // Rename builtins in header to avoid conflicts
        inpHeader = ReplaceVarName("uv", "uv_conv", inpHeader);
        inpHeader = ReplaceVarName("ang", "ang_conv", inpHeader);

        // Add mod_conv helpers if needed
        if (inp.find("mod_conv(") != std::string::npos) {
            std::string modHelpers =
                "// CONV: adding helper functions\n"
                "float mod_conv(float x, float y) { return x - y * floor(x / y); }\n"
                "float2 mod_conv(float2 x, float2 y) { return x - y * floor(x / y); }\n"
                "float3 mod_conv(float3 x, float3 y) { return x - y * floor(x / y); }\n"
                "float4 mod_conv(float4 x, float4 y) { return x - y * floor(x / y); }\n\n";
            inpHeader = modHelpers + inpHeader;
        }

        // Add lessThan helper if needed
        if (inp.find("lessThan") != std::string::npos || inp.find("lessthan") != std::string::npos) {
            std::string ltHelper =
                "float4 lessThan(float4 a, float4 b) { return float4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0); }\n\n";
            inpHeader = ltHelper + inpHeader;
        }

        // Add #defines for iChannel samplers
        {
            std::string defines;
            bool hasChannel = false;
            if (inp.find("iChannel0") != std::string::npos) { defines += "#define iChannel0 sampler_noise_lq\n"; hasChannel = true; }
            if (inp.find("iChannel1") != std::string::npos) { defines += "#define iChannel1 sampler_noise_lq\n"; hasChannel = true; }
            if (inp.find("iChannel2") != std::string::npos) { defines += "#define iChannel2 sampler_noise_lq\n"; hasChannel = true; }
            if (inp.find("iChannel3") != std::string::npos) { defines += "#define iChannel3 sampler_noise_lq\n"; hasChannel = true; }
            if (hasChannel) defines = "// CONV: setting iChannel samplers to default noise texture\n" + defines + "\n";
            if (inp.find(" tx") == std::string::npos) {
                defines = "#define tx (sin(time)*0.5+1)\n\n" + defines;
            }
            inpHeader = defines + inpHeader;
        }

        // Build shader_body wrapper
        std::ostringstream sbHeader;
        sbHeader << "\nshader_body {\n";

        if (indexMainImage != std::string::npos) {
            // Extract and convert mainImage arguments
            size_t argsOpen = inpMain.find('(');
            if (argsOpen != std::string::npos) {
                std::string argsRest = inpMain.substr(argsOpen + 1);
                int argsClose = FindClosingBracket(argsRest, '(', ')', 1);
                if (argsClose > 0) {
                    std::string argsStr = argsRest.substr(0, argsClose);
                    // Split by comma
                    std::vector<std::string> args;
                    {
                        std::istringstream ass(argsStr);
                        std::string tok;
                        while (std::getline(ass, tok, ',')) {
                            // Trim
                            size_t s = tok.find_first_not_of(" \t");
                            size_t e = tok.find_last_not_of(" \t");
                            if (s != std::string::npos) tok = tok.substr(s, e - s + 1);
                            args.push_back(tok);
                        }
                    }
                    for (auto& arg : args) {
                        if (arg.find("out ") != std::string::npos) {
                            // Output parameter — extract var name
                            size_t f4pos = arg.find("float4 ");
                            if (f4pos != std::string::npos)
                                retVarName = arg.substr(f4pos + 7);
                            replaceAll(arg, "out ", "");
                            sbHeader << arg << " = 0;\n";
                        } else {
                            replaceAll(arg, "in ", "");
                            sbHeader << arg << " = uv;\n";
                        }
                    }
                }
            }
        }

        sbHeader << "// CONV: Center on screen, then try some aspect correction\n";
        sbHeader << "uv = (uv*2) - 1;\n";
        sbHeader << "uv *= aspect.xy;\n";
        sbHeader << "// CONV: Adjust this to flip the output (+-uv.x, +-uv.y)\n";
        sbHeader << "uv = float2(uv.x, -uv.y);\n";
        sbHeader << "// CONV: Adjust viewpoint (x,y individually or both)\n";
        sbHeader << "uv += float2(0,0) + 0;\n";
        sbHeader << "uv *= float2(1,1) * 1;\n";

        // Find the opening brace of mainImage body and replace everything before it
        size_t braceIdx = inpMain.find('{');
        if (braceIdx != std::string::npos)
            inpMain = sbHeader.str() + inpMain.substr(braceIdx + 1);
        else
            inpMain = sbHeader.str() + inpMain;

        inp = inpHeader + inpMain;

        // Phase 3: Per-line processing
        std::string breakReplacement;
        std::ostringstream sb;
        {
            std::istringstream lss(inp);
            std::string line;
            while (std::getline(lss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::string currentLine = line;

                // Save for-loop condition for break replacement
                if (line.find("for(") != std::string::npos || line.find("for (") != std::string::npos) {
                    // Extract condition from second semicolon-delimited section
                    size_t forIdx = line.find("for");
                    size_t parenIdx = line.find('(', forIdx);
                    if (parenIdx != std::string::npos) {
                        std::string forBody = line.substr(parenIdx);
                        size_t semi1 = forBody.find(';');
                        if (semi1 != std::string::npos) {
                            std::string cond = forBody.substr(semi1 + 1);
                            size_t semi2 = cond.find(';');
                            if (semi2 != std::string::npos)
                                cond = cond.substr(0, semi2);
                            // Replace comparison operators with =
                            replaceAll(cond, "<=", "=");
                            replaceAll(cond, ">=", "=");
                            replaceAll(cond, "<", "=");
                            replaceAll(cond, ">", "=");
                            // Trim
                            size_t s = cond.find_first_not_of(" \t");
                            if (s != std::string::npos) cond = cond.substr(s);
                            breakReplacement = cond;
                        }
                    }
                }

                if (line.find("float2 uv =") != std::string::npos) {
                    currentLine = "// " + line;
                } else if (line.find("iDate") != std::string::npos) {
                    errors += "iDate unsupported\r\n";
                    sb << "// CONV: iDate unsupported\n";
                    currentLine = "// " + line;
                } else if (line.find("xTimeDelta") != std::string::npos) {
                    errors += "iTimeDelta unsupported\r\n";
                    sb << "// CONV: iTimeDelta unsupported\n";
                    replaceAll(currentLine, "xTimeDelta", "iTimeDelta");
                    currentLine = "// " + currentLine;
                } else if (line.find("break") != std::string::npos) {
                    if (breakReplacement.empty()) {
                        sb << "// CONV: no saved break condition, see manual\n";
                    } else {
                        sb << "// CONV: replaced break with breaking condition\n";
                        replaceAll(currentLine, "break", breakReplacement);
                    }
                }

                currentLine = FixMatrixMultiplication(currentLine);
                currentLine = FixFloatNumberOfArguments(currentLine, inp);
                currentLine = FixAtan(currentLine);

                sb << currentLine << "\n";
            }
        }
        result = sb.str();

        // Remove trailing backslash line continuations
        {
            std::istringstream bss(result);
            std::string line;
            std::ostringstream clean;
            std::vector<std::string> allLines;
            while (std::getline(bss, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                allLines.push_back(line);
            }
            for (size_t i = 0; i < allLines.size(); i++) {
                if (!allLines[i].empty() && allLines[i].back() == '\\') {
                    clean << allLines[i].substr(0, allLines[i].size() - 1);
                    if (i + 1 < allLines.size()) {
                        std::string next = allLines[i + 1];
                        size_t s = next.find_first_not_of(" \t");
                        if (s != std::string::npos) next = next.substr(s);
                        clean << next << "\n";
                        i++; // skip next line
                    }
                } else {
                    clean << allLines[i] << "\n";
                }
            }
            result = clean.str();
        }

        // Add return value before closing brace of shader_body
        {
            size_t sbIdx = result.find("shader_body");
            if (sbIdx != std::string::npos) {
                std::string after = result.substr(sbIdx);
                int closeIdx = FindClosingBracket(after, '{', '}', 0);
                if (closeIdx > 0) {
                    size_t insertPos = sbIdx + closeIdx;
                    result = result.substr(0, insertPos)
                           + "ret = " + retVarName + ";\n"
                           + result.substr(insertPos);
                }
            }
        }

        // Fix double-renamed variables
        replaceAll(result, "_conv_conv", "_conv");

        // Format
        result = BasicFormatShaderCode(result);

    } catch (...) {
        errors += "Conversion error (exception)\r\n";
    }

    // Convert result to wide and set in HLSL edit
    std::wstring resultW;
    resultW.reserve(result.size());
    for (char c : result) resultW += (wchar_t)(unsigned char)c;

    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_HLSL_EDIT, resultW.c_str());

    // Show errors
    std::wstring errW;
    if (errors.empty()) errW = L"Conversion complete.";
    else for (char c : errors) errW += (wchar_t)(unsigned char)c;
    SetDlgItemTextW(hw, IDC_MW_SHIMPORT_ERROR_EDIT, errW.c_str());
}

} // namespace mdrop
