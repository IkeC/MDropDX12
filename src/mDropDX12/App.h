#pragma once

namespace mdrop { class Engine; }
void PrecompilePresetShaders(mdrop::Engine& engine, const std::wstring& baseDir,
                             std::wstring& wLine, std::ofstream& compiledList, int& compiledShaders);

// Named pipe IPC — extern state for settings UI
#include "pipe_server.h"

extern PipeServer g_pipeServer;
extern WCHAR g_szLastIPCMessage[2048];
extern WCHAR g_szLastIPCTime[16];
extern std::atomic<int> g_lastIPCMessageSeq;
