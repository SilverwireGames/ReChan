#pragma once
#include <cstdio>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <vector>

class Log {
public:
    static Log& Get();

    void Init(const char* filename = "rechan.log");
    void Shutdown();

    void LogMessage(const char* fmt, ...);
    void LogMessageV(const char* fmt, va_list args);

    std::vector<std::string> GetMessageStack() const {
        return m_MessageStack;
    }

private:
    Log();
    ~Log();

    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;

    void WriteToConsole(const char* text);

private:
    std::ofstream m_File;
    std::mutex m_Mutex;
    std::vector<std::string> m_MessageStack;
};

// Written in every configuration; rare events only.
#define LOG_ALWAYS(fmt, ...) Log::Get().LogMessage(fmt, ##__VA_ARGS__)

#if defined(NDEBUG)
#define LOG(...) ((void)0)
#else
#define LOG(fmt, ...) Log::Get().LogMessage(fmt, ##__VA_ARGS__)
#endif
