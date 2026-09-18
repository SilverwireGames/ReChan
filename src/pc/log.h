#pragma once
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#if !defined(RC_PLATFORM_PSP)
#include <fstream>
#include <mutex>
#endif

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
#if !defined(RC_PLATFORM_PSP)
    std::ofstream m_File;
    std::mutex m_Mutex;
#endif
    std::vector<std::string> m_MessageStack;
};

#if defined(NDEBUG)
#define LOG(...) ((void)0)
#else
#define LOG(fmt, ...) Log::Get().LogMessage(fmt, ##__VA_ARGS__)
#endif
