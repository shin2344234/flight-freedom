#include "core/log.h"

#include <Windows.h>
#include <cstdarg>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "core/paths.h"

namespace fp::Log
{
    static std::mutex               g_mu;
    static std::deque<std::string>  g_recent;   // for the Status tab
    static std::deque<std::string>  g_pending;  // not yet on disk
    static FILE*                    g_file    = nullptr;
    static bool                     g_claimed = false;
    static constexpr size_t         kKeep     = 400;

    // The file I/O runs on its own thread. It used to run on whichever thread
    // called LOG, with an fflush per line, and the game's thread is one of
    // them: a probe run writing 2,760 lines a second was making 2,760 blocking
    // flushes a second inside the game's frame, which was enough to stop a
    // held d-pad direction registering as held. Nothing is dropped or capped;
    // the writer just does the waiting instead of the game.
    //
    // It flushes whenever it empties the queue, so an idle session is on disk
    // within milliseconds and a crash loses at most what was written in the
    // time one batch takes. Under load it batches, which is the point.
    static std::deque<std::string>  g_queue;
    static std::condition_variable  g_wake;
    static HANDLE                   g_writer  = nullptr;
    static bool                     g_stopping = false;

    static DWORD WINAPI WriterMain(LPVOID)
    {
        std::deque<std::string> batch;
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lk(g_mu);
                g_wake.wait(lk, [] { return g_stopping || !g_queue.empty(); });
                if (g_queue.empty() && g_stopping) return 0;
                batch.swap(g_queue);
            }
            if (g_file)
            {
                for (const std::string& l : batch)
                {
                    fputs(l.c_str(), g_file);
                    fputc('\n', g_file);
                }
                fflush(g_file);
            }
            batch.clear();
        }
    }

    static std::string Stamp()
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        char b[32];
        snprintf(b, sizeof b, "%02d:%02d:%02d.%03d", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        return b;
    }

    void Write(const char* level, const char* fmt, ...)
    {
        char msg[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof msg, fmt, ap);
        va_end(ap);

        std::string line = "[" + Stamp() + "] [" + level + "] " + msg;

        std::lock_guard<std::mutex> lk(g_mu);
        g_recent.push_back(line);
        if (g_recent.size() > kKeep) g_recent.pop_front();
        if (g_file)
        {
            fputs(line.c_str(), g_file);
            fputc('\n', g_file);
            fflush(g_file);
        }
        else
        {
            g_pending.push_back(line);
            if (g_pending.size() > kKeep) g_pending.pop_front();
        }
    }

    // Keep the last dozen sessions instead of one. The whole point of this
    // plugin is the log it leaves, and a probe run is usually worth comparing
    // against the one before it: the town flight and the mountain flight are
    // two sessions, not one. Lifted from Master Looter, where a launch that
    // destroyed the previous log cost a capture that had answered something.
    //
    // Plain text, not compressed. These get pasted into a message, and an
    // archive is a barrier to that.
    static constexpr int kArchives = 24;   // plus the live one. Sessions are cheap; losing one is not.

    // A rename loses to a sharing violation while another process still has
    // the file open, and the game can be restarted faster than the last one
    // lets go. Windows reports that as ERROR_SHARING_VIOLATION or
    // ERROR_ACCESS_DENIED; a missing source is not a failure, it is the first
    // run.
    static bool MoveOver(const wchar_t* from, const wchar_t* to)
    {
        for (int i = 0; i < 10; ++i)
        {
            if (MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING)) return true;
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
            Sleep(100);
        }
        return false;
    }

    // True when the live log is out of the way and a fresh one may be opened.
    // False means the previous session's log is still sitting at the live
    // path, and the caller must append to it rather than truncate it.
    static bool Rotate(const wchar_t* base)
    {
        wchar_t from[96], to[96], live[96];
        _snwprintf_s(live, _countof(live), _TRUNCATE, L"%s.log", base);
        // Oldest out first, then each one shuffles up a place, so the numbers
        // read as age: 01 is the session before this one, 11 the furthest back.
        _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.%02d.log", base, kArchives);
        DeleteFileW(Paths::File(to).c_str());
        for (int i = kArchives - 1; i >= 1; --i)
        {
            _snwprintf_s(from, _countof(from), _TRUNCATE, L"%s.%02d.log", base, i);
            _snwprintf_s(to,   _countof(to),   _TRUNCATE, L"%s.%02d.log", base, i + 1);
            MoveOver(Paths::File(from).c_str(), Paths::File(to).c_str());
        }
        _snwprintf_s(to, _countof(to), _TRUNCATE, L"%s.01.log", base);
        return MoveOver(Paths::File(live).c_str(), Paths::File(to).c_str());
    }

    // Caller holds g_mu. `fresh` false appends, because the live file still
    // holds the previous session and opening it "w" would empty it. That is
    // not hypothetical: two restarts three minutes apart on 20 September 2026
    // cost a 1.9 GB log in the plugin next door, and this one had lost its
    // own .02 the same way.
    static void Open(const wchar_t* base, bool fresh = true)
    {
        wchar_t live[96];
        _snwprintf_s(live, _countof(live), _TRUNCATE, L"%s.log", base);
        g_file = _wfopen(Paths::File(live).c_str(), fresh ? L"w" : L"a");
        if (!g_file) return;
        if (!fresh)
            fputs("[log  ] the previous session's log could not be renamed, most likely because that "
                  "process still had it open. This session is appended to it rather than writing over it.\n",
                  g_file);
        for (const auto& l : g_pending)
        {
            fputs(l.c_str(), g_file);
            fputc('\n', g_file);
        }
        g_pending.clear();
        fflush(g_file);
    }

    void Claim(const wchar_t* base)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_claimed) return;
        g_claimed = true;
        const bool fresh = Rotate(base);
        Open(base, fresh);
    }

    void ClaimSingle(const wchar_t* base)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_claimed) return;
        g_claimed = true;
        Open(base);
    }

    // Up to 1.1.2 the non-game process named its log after its own process id,
    // so every crashpad_handler.exe that ever started left a file behind and
    // nothing removed them. One bin64 had 78 of them, one line each. The name
    // is fixed now, and this clears out what the old builds left.
    //
    // A wildcard can match a file through its 8.3 short name, so the name that
    // comes back is checked against the pattern again before anything goes.
    // Nothing outside <base>.other-*.log is ever deleted.
    int RemovePerProcessLogs(const wchar_t* base)
    {
        wchar_t prefix[96], pattern[96];
        _snwprintf_s(prefix,  _countof(prefix),  _TRUNCATE, L"%s.other-", base);
        _snwprintf_s(pattern, _countof(pattern), _TRUNCATE, L"%s*.log", prefix);

        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(Paths::File(pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;

        const size_t plen = wcslen(prefix);
        int removed = 0;
        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const size_t n = wcslen(fd.cFileName);
            if (n <= plen + 4) continue;
            if (_wcsnicmp(fd.cFileName, prefix, plen) != 0) continue;
            if (_wcsicmp(fd.cFileName + n - 4, L".log") != 0) continue;
            if (DeleteFileW(Paths::File(fd.cFileName).c_str())) ++removed;
        } while (FindNextFileW(h, &fd));

        FindClose(h);
        return removed;
    }

    bool Claimed()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        return g_claimed;
    }

    void Shutdown(bool processExiting)
    {
        if (processExiting)
        {
            // Windows has already ended every other thread, the writer included,
            // and one that died holding g_mu or the file's own lock would hang
            // the game's exit on either. No thread is left to race with, so the
            // queue goes out without taking any lock, and the file is flushed
            // but not closed: fclose takes the file's lock as well.
            if (g_file)
            {
                for (const std::string& l : g_queue) { _fwrite_nolock(l.data(), 1, l.size(), g_file); _fputc_nolock('\n', g_file); }
                _fflush_nolock(g_file);
            }
            return;
        }
        HANDLE writer = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_stopping = true;
            writer = g_writer;
        }
        g_wake.notify_all();
        if (writer)
        {
            // The queue is drained by the writer before it returns, so the
            // last lines of a session reach the file.
            WaitForSingleObject(writer, 2000);
            CloseHandle(writer);
        }
        std::lock_guard<std::mutex> lk(g_mu);
        g_writer = nullptr;
        if (g_file)
        {
            for (const std::string& l : g_queue) { fputs(l.c_str(), g_file); fputc('\n', g_file); }
            g_queue.clear();
            fflush(g_file);
            fclose(g_file);
            g_file = nullptr;
        }
    }

    void StartWriter()
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_writer || !g_file) return;
        g_writer = CreateThread(nullptr, 0, WriterMain, nullptr, 0, nullptr);
    }

    void Snapshot(std::vector<std::string>& out, int maxLines)
    {
        std::lock_guard<std::mutex> lk(g_mu);
        out.clear();
        const size_t n = g_recent.size();
        const size_t start = (maxLines > 0 && n > static_cast<size_t>(maxLines)) ? n - maxLines : 0;
        for (size_t i = start; i < n; ++i) out.push_back(g_recent[i]);
    }
}
