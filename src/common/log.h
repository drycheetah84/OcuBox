// Minimal leveled logger for hollywood_emu.
#pragma once
#include <string>
#include <string_view>
#include <cstdio>
#include <format>

namespace hw {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error };

class Log {
public:
    static LogLevel level;
    static void set_level(LogLevel l) { level = l; }

    // Optional sink: if set, every emitted line is also delivered here (used by the
    // GUI to display live boot output). Must be thread-safe on the callee side.
    using Sink = void(*)(LogLevel, std::string_view module, std::string_view msg);
    static Sink sink;
    static void set_sink(Sink s) { sink = s; }

    static void write(LogLevel l, std::string_view module, std::string_view msg);

    template <class... Args>
    static void log(LogLevel l, std::string_view module,
                    std::format_string<Args...> fmt, Args&&... args) {
        if (l < level) return;
        write(l, module, std::format(fmt, std::forward<Args>(args)...));
    }
};

#define HW_TRACE(mod, ...) ::hw::Log::log(::hw::LogLevel::Trace, mod, __VA_ARGS__)
#define HW_DEBUG(mod, ...) ::hw::Log::log(::hw::LogLevel::Debug, mod, __VA_ARGS__)
#define HW_INFO(mod, ...)  ::hw::Log::log(::hw::LogLevel::Info,  mod, __VA_ARGS__)
#define HW_WARN(mod, ...)  ::hw::Log::log(::hw::LogLevel::Warn,  mod, __VA_ARGS__)
#define HW_ERROR(mod, ...) ::hw::Log::log(::hw::LogLevel::Error, mod, __VA_ARGS__)

} // namespace hw
