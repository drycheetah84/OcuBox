#include "common/log.h"

namespace hw {

LogLevel Log::level = LogLevel::Info;
Log::Sink Log::sink = nullptr;

static const char* level_tag(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
    }
    return "?????";
}

void Log::write(LogLevel l, std::string_view module, std::string_view msg) {
    if (l < level) return;
    if (sink) sink(l, module, msg);
    std::FILE* out = (l >= LogLevel::Warn) ? stderr : stdout;
    std::fprintf(out, "  [%s] %-10.*s | %.*s\n", level_tag(l),
                 (int)module.size(), module.data(),
                 (int)msg.size(), msg.data());
    std::fflush(out);
}

} // namespace hw
