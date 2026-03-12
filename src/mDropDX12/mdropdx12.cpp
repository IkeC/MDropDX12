#include "mdropdx12.h"
#include <locale>
#include <codecvt>
#include <algorithm>
#include <cctype>

MDropDX12::MDropDX12() {}

void MDropDX12::Init(wchar_t* exePath) {
  winrt::init_apartment(); // Initialize the WinRT runtime
  start_time = std::chrono::steady_clock::now();

  // Get the executable's directory
  std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

  // Construct the "resources/sprites/" directory path relative to the executable
  std::filesystem::path spritesDir = exeDir / "resources/sprites";
  std::filesystem::create_directories(spritesDir);

  // Construct the file path
  coverSpriteFilePath = spritesDir / "cover.png";
}

// --- SMTC session enumeration + smart selection ---

/*static*/ std::wstring MDropDX12::GetFriendlyName(const std::wstring& appId) {
  // Known AUMIDs → friendly names
  static const struct { const wchar_t* sub; const wchar_t* name; } known[] = {
    { L"Spotify",         L"Spotify" },
    { L"foobar2000",      L"foobar2000" },
    { L"AIMP",            L"AIMP" },
    { L"Winamp",          L"Winamp" },
    { L"MusicBee",        L"MusicBee" },
    { L"TIDAL",           L"TIDAL" },
    { L"iTunes",          L"iTunes" },
    { L"MediaMonkey",     L"MediaMonkey" },
    { L"Plexamp",         L"Plexamp" },
    { L"ZuneMusic",       L"Groove Music" },
    { L"VLC",             L"VLC" },
    { L"mpv",             L"mpv" },
    { L"chrome",          L"Chrome" },
    { L"msedge",          L"Edge" },
    { L"firefox",         L"Firefox" },
    { L"opera",           L"Opera" },
    { L"brave",           L"Brave" },
  };
  for (auto& k : known) {
    // Case-insensitive substring search
    std::wstring lower = appId;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    std::wstring sub = k.sub;
    std::transform(sub.begin(), sub.end(), sub.begin(), ::towlower);
    if (lower.find(sub) != std::wstring::npos) return k.name;
  }
  // UWP: extract portion before first '_'
  auto upos = appId.find(L'_');
  if (upos != std::wstring::npos) {
    // Take last segment after '.' before '_' (e.g., "Microsoft.ZuneMusic" → "ZuneMusic")
    auto dotpos = appId.rfind(L'.', upos);
    if (dotpos != std::wstring::npos && dotpos + 1 < upos)
      return appId.substr(dotpos + 1, upos - dotpos - 1);
    return appId.substr(0, upos);
  }
  // Exe path: extract stem
  auto bslash = appId.rfind(L'\\');
  auto fslash = appId.rfind(L'/');
  size_t start = 0;
  if (bslash != std::wstring::npos) start = bslash + 1;
  if (fslash != std::wstring::npos && fslash + 1 > start) start = fslash + 1;
  std::wstring stem = appId.substr(start);
  // Remove .exe extension
  if (stem.size() > 4) {
    std::wstring ext = stem.substr(stem.size() - 4);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext == L".exe") stem = stem.substr(0, stem.size() - 4);
  }
  if (stem.empty() || stem.size() > 40) {
    // Fallback: truncated raw AUMID
    if (appId.size() > 40) return appId.substr(0, 40) + L"...";
    return appId;
  }
  return stem;
}

static SMTCSessionInfo BuildSessionInfo(const GlobalSystemMediaTransportControlsSession& session) {
  SMTCSessionInfo info;
  try { info.appId = session.SourceAppUserModelId().c_str(); } catch (...) { info.appId = L"(unknown)"; }
  info.displayName = MDropDX12::GetFriendlyName(info.appId);
  try {
    auto pbInfo = session.GetPlaybackInfo();
    auto pbStatus = pbInfo ? pbInfo.PlaybackStatus() : GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed;
    info.playbackStatus = (int)(int32_t)pbStatus;
  } catch (...) { info.playbackStatus = 0; }
  return info;
}

// Music app detection: case-insensitive match against priority list
static bool IsMusicApp(const std::wstring& appId) {
  static const wchar_t* musicApps[] = {
    L"spotify", L"foobar2000", L"aimp", L"winamp", L"musicbee",
    L"tidal", L"itunes", L"mediamonkey", L"plexamp", L"zunemusic",
    L"groove", L"vlc", L"mpv", L"musicapp",
  };
  std::wstring lower = appId;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
  for (auto& m : musicApps) {
    if (lower.find(m) != std::wstring::npos) return true;
  }
  return false;
}

void MDropDX12::EnumerateSessions(const GlobalSystemMediaTransportControlsSessionManager& manager) {
  std::vector<SMTCSessionInfo> list;
  try {
    auto sessions = manager.GetSessions();
    uint32_t count = sessions.Size();
    if (count == 0) {
      auto current = manager.GetCurrentSession();
      if (current)
        list.push_back(BuildSessionInfo(current));
    } else {
      list.reserve(count);
      for (uint32_t i = 0; i < count; i++)
        list.push_back(BuildSessionInfo(sessions.GetAt(i)));
    }
  } catch (const winrt::hresult_error&) {
    try { auto current = manager.GetCurrentSession(); if (current) list.push_back(BuildSessionInfo(current)); } catch (...) {}
  } catch (const std::exception&) {
    try { auto current = manager.GetCurrentSession(); if (current) list.push_back(BuildSessionInfo(current)); } catch (...) {}
  } catch (...) {
    try { auto current = manager.GetCurrentSession(); if (current) list.push_back(BuildSessionInfo(current)); } catch (...) {}
  }
  std::lock_guard<std::mutex> lock(m_smtcMutex);
  m_smtcSessions.swap(list);
}

GlobalSystemMediaTransportControlsSession MDropDX12::SelectBestSession(const GlobalSystemMediaTransportControlsSessionManager& manager) {
  // Use cached session info for smart selection, return session via GetCurrentSession()
  // (GetSessions() may throw on MTA — avoid calling it again)
  std::wstring chosenAppId;
  {
    std::lock_guard<std::mutex> lock(m_smtcMutex);

    // Manual mode: find selected session by AUMID
    if (m_nSMTCSessionMode == 1 && m_szSMTCSelectedAppId[0] != L'\0') {
      for (auto& s : m_smtcSessions) {
        if (s.appId == m_szSMTCSelectedAppId) {
          chosenAppId = s.appId;
          break;
        }
      }
    }

    // Auto mode
    if (chosenAppId.empty() && !m_smtcSessions.empty()) {
      const SMTCSessionInfo* bestPlayingMusic = nullptr;
      const SMTCSessionInfo* bestPlaying = nullptr;
      const SMTCSessionInfo* bestPausedMusic = nullptr;

      for (auto& s : m_smtcSessions) {
        bool music = IsMusicApp(s.appId);
        if (s.playbackStatus == 4) { // Playing
          if (music && !bestPlayingMusic) bestPlayingMusic = &s;
          if (!bestPlaying) bestPlaying = &s;
        } else if (s.playbackStatus == 5) { // Paused
          if (music && !bestPausedMusic) bestPausedMusic = &s;
        }
      }

      if (bestPlayingMusic) chosenAppId = bestPlayingMusic->appId;
      else if (bestPlaying) chosenAppId = bestPlaying->appId;
      else if (bestPausedMusic) chosenAppId = bestPausedMusic->appId;
      else chosenAppId = m_smtcSessions[0].appId; // first available
    }
  }

  // Always use GetCurrentSession() to get the actual session object (works on MTA).
  // The chosenAppId is informational for the UI.
  auto current = manager.GetCurrentSession();
  if (current) {
    m_szActiveSessionAppId = current.SourceAppUserModelId().c_str();
  } else {
    m_szActiveSessionAppId.clear();
  }
  return current;
}

void MDropDX12::PollMediaInfo() {

  if (!doPoll && !doPollExplicit) return;

  try {
    auto current_time = std::chrono::steady_clock::now();
    auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

    if (elapsed_seconds >= 2 || doPollExplicit) {

      auto smtcManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
      EnumerateSessions(smtcManager);
      auto currentSession = SelectBestSession(smtcManager);
      updated = false;
      if (currentSession) {
        auto properties = currentSession.TryGetMediaPropertiesAsync().get();
        if (properties) {
          if (doPollExplicit || properties.Artist().c_str() != currentArtist || properties.Title().c_str() != currentTitle || properties.AlbumTitle().c_str() != currentAlbum) {
            isSongChange = currentAlbum.length() || currentArtist.length() || currentTitle.length();
            currentArtist = properties.Artist().c_str();
            currentTitle = properties.Title().c_str();
            currentAlbum = properties.AlbumTitle().c_str();

            if ((doPollExplicit || doSaveCover) && properties.Thumbnail()) {
              SaveThumbnailToFile(properties);
            }

            updated = true;
          }
        }
      }
      else {
        if (currentArtist.length() || currentTitle.length() || currentAlbum.length()) {
          currentArtist = L"";
          currentTitle = L"";
          currentAlbum = L"";
          updated = true;
        }
      }

      start_time = current_time;
    }
  } catch (const std::exception& e) {
    LogException(L"PollMediaInfo", e, false);
  }
}

bool MDropDX12::SaveThumbnailToFile(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& properties) {
  try {
    // Retrieve the thumbnail
    auto thumbnailRef = properties.Thumbnail();
    if (!thumbnailRef) {
      std::wcerr << L"No thumbnail available for the current media." << std::endl;
      return false;
    }

    // Open the thumbnail stream
    auto thumbnailStream = thumbnailRef.OpenReadAsync().get();
    auto decoder = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(thumbnailStream).get();

    // Encode the image as a PNG and save it to the file
    auto fileStream = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
    auto encoder = winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
      winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), fileStream).get();

    encoder.SetSoftwareBitmap(decoder.GetSoftwareBitmapAsync().get());
    encoder.FlushAsync().get();

    // Write the encoded image to the file
    std::ofstream outputFile(coverSpriteFilePath, std::ios::binary);
    if (!outputFile.is_open()) {
      std::wcerr << L"Failed to open file for writing: " << coverSpriteFilePath << std::endl;
      return false;
    }

    // Use DataReader to read the stream content
    auto size = fileStream.Size();
    fileStream.Seek(0);
    auto buffer = winrt::Windows::Storage::Streams::Buffer(static_cast<uint32_t>(size));
    fileStream.ReadAsync(buffer, static_cast<uint32_t>(size), winrt::Windows::Storage::Streams::InputStreamOptions::None).get();

    outputFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
    outputFile.close();

    std::wcout << L"Thumbnail saved to: " << coverSpriteFilePath.wstring() << std::endl;
    coverUpdated = true;
    return true;
  } catch (const std::exception& e) {
    LogException(L"SaveThumbnailToFile", e, false);
  }
  return false;
}

void MDropDX12::LogDebug(std::wstring info) {
  if (logLevel < 3) return;
  LogInfo(info.c_str());
}

void MDropDX12::LogDebug(const wchar_t* info) {
  if (logLevel < 3) return;
  LogInfo(info);
}

void MDropDX12::LogInfo(std::wstring info) {
  LogInfo(info.c_str());
}

void MDropDX12::LogInfo(const wchar_t* info) {
  if (logLevel < 2) return;

  // Ensure the "log" directory exists
  const char* logDir = "log";
  if (_mkdir(logDir) != 0 && errno != EEXIST) {
    std::cerr << "Failed to create or access log directory: " << logDir << std::endl;
    return;
  }

  // Get the current timestamp
  std::time_t now = std::time(nullptr);
  std::tm localTime;
  localtime_s(&localTime, &now);

  char datestring[20];
  char timestring[20];

  std::strftime(datestring, sizeof(datestring), "%Y-%m-%d", &localTime);
  std::strftime(timestring, sizeof(timestring), "%H:%M:%S", &localTime);

  // Construct the log file path
  std::ostringstream logFilePath;
  logFilePath << logDir << "\\" << datestring << ".visualizer.info.log";

  // Open the log file in append mode
  std::ofstream logFile(logFilePath.str(), std::ios::app);
  if (logFile.is_open()) {
    // Convert wchar_t* to UTF-8 std::string
    std::wstring ws(info);
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    std::string utf8info = conv.to_bytes(ws);

    logFile << timestring << "> " << utf8info << std::endl;
    logFile.close();
  }
  else {
    std::cerr << "Failed to open log file: " << logFilePath.str() << std::endl;
  }
}

void MDropDX12::LogException(const wchar_t* context, const std::exception& e, bool showMessage) {

  if (logLevel < 1) return;

  std::wstring ws(context);
  std::wstring info = L"caught exception: ";
  info += ws;
  LogInfo(info.c_str());

  std::string exceptionMessage = e.what();

  // Ensure the "log" directory exists  
  const char* logDir = "log";
  if (_mkdir(logDir) != 0 && errno != EEXIST) {
    std::cerr << "Failed to create or access log directory: " << logDir << std::endl;
    return;
  }

  // Get the current timestamp
  std::time_t now = std::time(nullptr);
  std::tm localTime;
  localtime_s(&localTime, &now);

  char timestamp[20];
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &localTime);

  // Construct the log file path
  std::ostringstream logFilePath;
  logFilePath << logDir << "\\" << timestamp << ".visualizer.error.log";

  // Write the exception details to the log file
  std::ofstream logFile(logFilePath.str());
  if (logFile.is_open()) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> conv;
    std::string utf8info = conv.to_bytes(ws);

    logFile << "Exception occurred: " << utf8info << "\n" << exceptionMessage << std::endl;

    // Capture and log the stack trace
    logFile << "\nStack trace:\n";
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char));
    if (symbol == NULL) {
      logFile << "Failed to allocate memory for SYMBOL_INFO." << std::endl;
      SymCleanup(process);
      return;
    }
    symbol->MaxNameLen = 255;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

    for (USHORT i = 0; i < frames; i++) {
      SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
      logFile << frames - i - 1 << ": " << symbol->Name << " - 0x" << std::hex << symbol->Address << std::dec << "\n";
    }

    free(symbol);
    SymCleanup(process);

    logFile.close();
  }
  else {
    std::cerr << "Failed to open log file: " << logFilePath.str() << std::endl;
  }

  if (showMessage) {
    // Show a message box with the error details
    std::wstring message = L"An unexpected error occurred:\n\n";
    message += std::wstring(exceptionMessage.begin(), exceptionMessage.end());
    message += L"\n\nDetails have been written to the log directory. Please open an issue on GitHub if the problem persists.\n\nPress Ctrl+O in the Remote to restart Visualizer.";

    MessageBoxW(NULL, message.c_str(), L"MDropDX12 Error", MB_OK | MB_ICONERROR);
  }
}