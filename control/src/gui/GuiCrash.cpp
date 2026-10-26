#include "gui/GuiCrash.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwchar>

#include <windows.h>

#ifdef _WIN32

#include <imgui.h>
#include <imgui_internal.h>

namespace mediamax::gui
{
namespace
{

const wchar_t* kCrashLogName = L"mediamax_control_crash.log";

// Builds the crash/diagnostic log path (next to the executable) into buf.
// Returns false if the path does not fit. Allocation-free: the crash filter
// may run in a process whose heap is corrupted, so it must not allocate.
bool crashLogPathW(wchar_t* buf, size_t capacity)
{
    const wchar_t* dir = L".";
    size_t dirLen = 1;
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        const wchar_t* slash = wcsrchr(exePath, L'\\');
        if (slash != nullptr)
        {
            dir = exePath;
            dirLen = static_cast<size_t>(slash - exePath);
        }
    }
    if (dirLen + 1 + wcslen(kCrashLogName) + 1 > capacity)
    {
        return false;
    }
    memcpy(buf, dir, dirLen * sizeof(wchar_t));
    buf[dirLen] = L'\\';
    (void)wcscpy_s(buf + dirLen + 1, capacity - dirLen - 1, kCrashLogName);
    return true;
}

// Fixed-capacity line writer. The exception handler runs in a process whose
// state is unknown, so it allocates nothing: stack buffers + direct kernel
// file calls only.
struct ReportWriter
{
    HANDLE hFile = INVALID_HANDLE_VALUE;
    unsigned char buffer[16384];
    std::size_t pos = 0;

    bool open(const wchar_t* path)
    {
        // Append, not overwrite: the per-frame dock invariant checks may
        // have recorded tree drift in this same file before the crash,
        // and that context is exactly what a post-mortem needs.
        hFile = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER toEnd = {};
        ::SetFilePointerEx(hFile, toEnd, nullptr, FILE_END);
        return true;
    }

    void close()
    {
        flush();
        if (hFile != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;
        }
    }

    void flush()
    {
        if (hFile != INVALID_HANDLE_VALUE && pos > 0)
        {
            DWORD written = 0;
            ::WriteFile(hFile, buffer, static_cast<DWORD>(pos), &written, nullptr);
            ::FlushFileBuffers(hFile);
            pos = 0;
        }
    }

    void write(const char* text)
    {
        std::size_t len = 0;
        while (text[len] != '\0')
        {
            ++len;
        }
        if (pos + len >= sizeof(buffer))
        {
            flush();
        }
        if (len >= sizeof(buffer))
        {
            return; // oversized line: drop it rather than corrupt the file
        }
        for (std::size_t i = 0; i < len; ++i)
        {
            buffer[pos + i] = static_cast<unsigned char>(text[i]);
        }
        pos += len;
    }

    void printf(const char* fmt, ...)
    {
        char tmp[2048];
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(tmp, sizeof(tmp) - 1, fmt, args);
        va_end(args);
        if (n > 0)
        {
            write(tmp);
        }
    }
};

// "0x%08X" (or "-") of a dock node id, from a pointer.
void writeNodeId(ReportWriter& w, const ImGuiDockNode* node)
{
    if (node == nullptr)
    {
        w.write("-");
        return;
    }
    w.printf("0x%08X", node->ID);
}

// Dumps one window's identity + docking + draw-list state (one line each).
void dumpWindow(ReportWriter& w, const ImGuiWindow* win)
{
    if (win == nullptr)
    {
        w.write("    (null)\n");
        return;
    }
    const ImGuiWindowFlags f = win->Flags;
    w.printf("    window '%s' id=0x%08X flags=0x%08X%s active=%d wasActive=%d hidden=%d "
             "collapsed=%d skipItems=%d beginCount=%d\n",
             win->Name != nullptr ? win->Name : "(null)", win->ID,
             static_cast<unsigned int>(f),
             (f & ImGuiWindowFlags_DockNodeHost) != 0 ? " [DockNodeHost]" : "", win->Active ? 1 : 0,
             win->WasActive ? 1 : 0, win->Hidden ? 1 : 0, win->Collapsed ? 1 : 0,
             win->SkipItems ? 1 : 0, win->BeginCount);
    w.printf("      dockId=0x%08X dockNode=", win->DockId);
    writeNodeId(w, win->DockNode);
    w.write(" dockNodeAsHost=");
    writeNodeId(w, win->DockNodeAsHost);
    w.printf(" lastFrameActive=%d lastTimeActive=%.3f memoryCompacted=%d\n", win->LastFrameActive,
             win->LastTimeActive, win->MemoryCompacted ? 1 : 0);
    if (win->DrawList != nullptr)
    {
        const ImDrawList& dl = *win->DrawList;
        w.printf("      drawlist cmds=%d vtx=%d splitter{current=%d count=%d "
                 "channels{size=%d capacity=%d data=%p}}\n",
                 dl.CmdBuffer.Size, dl.VtxBuffer.Size, dl._Splitter._Current, dl._Splitter._Count,
                 dl._Splitter._Channels.Size, dl._Splitter._Channels.Capacity,
                 dl._Splitter._Channels.Data);
    }
    else
    {
        w.write("      drawlist=(null)\n");
    }
}

// Dumps one dock node: identity, tree links, host window, windows, state.
void dumpNode(ReportWriter& w, const ImGuiDockNode* node)
{
    if (node == nullptr)
    {
        return;
    }
    w.write("  node id=");
    writeNodeId(w, node);
    w.write(" parent=");
    writeNodeId(w, node->ParentNode);
    w.write(" child0=");
    writeNodeId(w, node->ChildNodes[0]);
    w.write(" child1=");
    writeNodeId(w, node->ChildNodes[1]);
    w.printf(" state=%d flags{shared=0x%08X local=0x%08X merged=0x%08X}\n",
             static_cast<int>(node->State), static_cast<unsigned int>(node->SharedFlags),
             static_cast<unsigned int>(node->LocalFlags),
             static_cast<unsigned int>(node->MergedFlags));
    w.write("    hostWindow=");
    if (node->HostWindow != nullptr)
    {
        w.printf("'%s' id=0x%08X flags=0x%08X\n",
                 node->HostWindow->Name != nullptr ? node->HostWindow->Name : "(null)",
                 node->HostWindow->ID,
                 static_cast<unsigned int>(node->HostWindow->Flags));
    }
    else
    {
        w.write("null\n");
    }
    w.printf("    visibleWindow='%s' windows=%d lastFrameAlive=%d lastFrameActive=%d isVisible=%d\n",
             node->VisibleWindow != nullptr && node->VisibleWindow->Name != nullptr
                 ? node->VisibleWindow->Name
                 : "(none)",
             node->Windows.Size, node->LastFrameAlive, node->LastFrameActive,
             node->IsVisible ? 1 : 0);
    const int shown = node->Windows.Size < 8 ? node->Windows.Size : 8;
    for (int wn = 0; wn < shown; ++wn)
    {
        const ImGuiWindow* win = node->Windows[wn];
        w.printf("      win[%d]='%s' id=0x%08X\n", wn,
                 win != nullptr && win->Name != nullptr ? win->Name : "(null)",
                 win != nullptr ? win->ID : 0);
    }
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
    wchar_t path[MAX_PATH + 64] = {};
    if (!crashLogPathW(path, IM_COUNTOF(path)))
    {
        (void)wcscpy_s(path, IM_COUNTOF(path), L"mediamax_control_crash.log");
    }
    ReportWriter w;
    if (w.open(path))
    {
        SYSTEMTIME st = {};
        ::GetLocalTime(&st);
        w.printf("=== mediamax_control crash report ===\n");
        w.printf("time: %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
        w.printf("pid: %lu  tid: %lu\n", static_cast<unsigned long>(::GetCurrentProcessId()),
                 static_cast<unsigned long>(::GetCurrentThreadId()));
        w.printf("exe base: 0x%I64X\n",
                 static_cast<DWORD64>(reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr))));
        w.printf("exception code: 0x%08lX  at 0x%p\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode),
                 ep->ExceptionRecord->ExceptionAddress);
        const CONTEXT* c = ep->ContextRecord;
        if (c != nullptr)
        {
            w.printf("RIP=0x%I64X RSP=0x%I64X RBP=0x%I64X\n", c->Rip, c->Rsp, c->Rbp);
            w.printf("RAX=0x%I64X RCX=0x%I64X RDX=0x%I64X RBX=0x%I64X\n", c->Rax, c->Rcx, c->Rdx,
                     c->Rbx);
            w.printf("RSI=0x%I64X RDI=0x%I64X R8=0x%I64X  R9=0x%I64X\n", c->Rsi, c->Rdi, c->R8, c->R9);
            w.printf("R10=0x%I64X R11=0x%I64X R12=0x%I64X R13=0x%I64X\n", c->R10, c->R11, c->R12,
                     c->R13);
            w.printf("R14=0x%I64X R15=0x%I64X\n", c->R14, c->R15);
        }

        // Full Dear ImGui state: this is what a write-to-NULL deep in the
        // docking code needs (which node held which host window, and whether
        // that window's draw list was ever split into docking channels).
        ImGuiContext* ictx = ImGui::GetCurrentContext();
        if (ictx == nullptr)
        {
            w.write("imgui: no context\n");
        }
        else
        {
            const ImGuiDockContext& dc = ictx->DockContext;
            w.printf("imgui: frame=%d time=%.3f configFlags=0x%08X gcCompactAll=%d "
                     "wantFullRebuild=%d dockRequests=%d\n",
                     ictx->FrameCount, ictx->Time, static_cast<unsigned int>(ictx->IO.ConfigFlags),
                     ictx->GcCompactAll ? 1 : 0, dc.WantFullRebuild ? 1 : 0, dc.Requests.Size);
            w.printf("imgui: windows=%d\n", ictx->Windows.Size);
            const int windowShown = ictx->Windows.Size < 64 ? ictx->Windows.Size : 64;
            for (int wn = 0; wn < windowShown; ++wn)
            {
                dumpWindow(w, ictx->Windows[wn]);
            }
            w.printf("imgui: dock nodes=%d\n", dc.Nodes.Data.Size);
            for (int n = 0; n < dc.Nodes.Data.Size; ++n)
            {
                dumpNode(w, static_cast<ImGuiDockNode*>(dc.Nodes.Data[n].val_p));
            }
            if (ictx->CurrentWindow != nullptr)
            {
                w.printf("imgui: currentWindow='%s' id=0x%08X\n",
                         ictx->CurrentWindow->Name != nullptr ? ictx->CurrentWindow->Name : "(null)",
                         ictx->CurrentWindow->ID);
            }
        }
        w.close();
    }

    // Never die silently: tell the user where the report went.
    ::MessageBoxW(nullptr,
                  L"MediaMax Control hit an unhandled exception and is closing.\n"
                  L"A crash report was written to 'mediamax_control_crash.log' next to the "
                  L"executable.",
                  L"MediaMax Control", MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace

void installCrashDiagnostics()
{
    ::SetUnhandledExceptionFilter(crashFilter);
}

void appendDiagnostic(const std::string& line)
{
    wchar_t path[MAX_PATH + 64] = {};
    if (!crashLogPathW(path, IM_COUNTOF(path)))
    {
        return;
    }
    HANDLE hFile = ::CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        return;
    }
    LARGE_INTEGER toEnd = {};
    if (::SetFilePointerEx(hFile, toEnd, nullptr, FILE_END) == FALSE)
    {
        ::CloseHandle(hFile);
        return;
    }
    SYSTEMTIME st = {};
    ::GetLocalTime(&st);
    char header[64];
    std::snprintf(header, sizeof(header), "[%04d-%02d-%02d %02d:%02d:%02d] ", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string payload = header + line + "\n";
    DWORD written = 0;
    ::WriteFile(hFile, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    ::FlushFileBuffers(hFile);
    ::CloseHandle(hFile);
}

} // namespace mediamax::gui

#else

namespace mediamax::gui
{
void installCrashDiagnostics() {}
void appendDiagnostic(const std::string&) {}
} // namespace mediamax::gui

#endif // _WIN32
