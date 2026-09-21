#pragma once
#include <string>
#include <vector>

namespace fp::Log
{
    // printf-style. Lines are buffered until Claim(); after that they go to
    // <base>.log next to the plugin. A copy of recent lines is always kept in
    // memory either way.
    void Write(const char* level, const char* fmt, ...);

    // `base` names the file and its archives: "FlightProbe" gives
    // FlightProbe.log and FlightProbe.01.log upwards.
    //
    // It is a parameter because two processes load this plugin. Session one
    // put both of them in one file: lines from each landed at the other's file
    // offset, one was cut in half and three vanished, and the result read as
    // three hooks failing when all five had installed. Whoever is not the game
    // gets its own name and never touches the real log.
    void Claim(const wchar_t* base);

    // The same, for a log that must not accumulate: no archives, and the file
    // from last time is replaced. crashpad_handler.exe writes one line saying
    // it is doing nothing, and a history of that is worth nothing.
    void ClaimSingle(const wchar_t* base);

    // Deletes <base>.other-<pid>.log, which is what the non-game process was
    // named up to 1.1.2. Returns how many went. Only the game calls this.
    int RemovePerProcessLogs(const wchar_t* base);

    bool Claimed();
    // Start the background writer. Call it after Claim(): until then a
    // line is written on the calling thread, which is what startup wants
    // and what a frame does not.
    void StartWriter();

    // processExiting: called from DLL_PROCESS_DETACH during process exit, when
    // every other thread is already gone.
    void Shutdown(bool processExiting = false);
    void Snapshot(std::vector<std::string>& out, int maxLines);
}

#define LOG(...)     ::fp::Log::Write("info ", __VA_ARGS__)
#define LOG_OK(...)  ::fp::Log::Write("ok   ", __VA_ARGS__)
#define LOG_ERR(...) ::fp::Log::Write("error", __VA_ARGS__)
