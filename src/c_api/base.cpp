#include "c_api/internal.hpp"

#include "secs/core/metrics.hpp"

// ----------------------------- 内存/错误/版本 -----------------------------

void *secs_malloc(size_t n) { return std::malloc(n); }

void secs_free(void *p) { std::free(p); }

char *secs_error_message(secs_error_t err) {
    try {
        if (err.value == 0) {
            return dup_string("ok");
        }

        if (err.category && std::strcmp(err.category, kCApiCategory) == 0) {
            return dup_string(c_api_message_for(err.value));
        }

        const auto *cat = category_from_name(err.category);
        if (!cat) {
            std::string msg = "unknown error category";
            if (err.category) {
                msg += ": ";
                msg += err.category;
            }
            msg += " (";
            msg += std::to_string(err.value);
            msg += ")";
            return dup_string(msg);
        }

        return dup_string(std::error_code{err.value, *cat}.message());
    } catch (...) {
        return nullptr;
    }
}

const char *secs_version_string(void) {
#ifdef SECS_VERSION_STRING
    return SECS_VERSION_STRING;
#else
    return "0.1.0";
#endif
}

secs_error_t secs_log_set_level(secs_log_level_t level) {
    return guard_error([&]() -> secs_error_t {
        using secs::core::LogLevel;
        switch (level) {
        case SECS_LOG_TRACE:
            secs::core::set_log_level(LogLevel::trace);
            return ok();
        case SECS_LOG_DEBUG:
            secs::core::set_log_level(LogLevel::debug);
            return ok();
        case SECS_LOG_INFO:
            secs::core::set_log_level(LogLevel::info);
            return ok();
        case SECS_LOG_WARN:
            secs::core::set_log_level(LogLevel::warn);
            return ok();
        case SECS_LOG_ERROR:
            secs::core::set_log_level(LogLevel::error);
            return ok();
        case SECS_LOG_CRITICAL:
            secs::core::set_log_level(LogLevel::critical);
            return ok();
        case SECS_LOG_OFF:
            secs::core::set_log_level(LogLevel::off);
            return ok();
        }
        return c_api_err(SECS_C_API_INVALID_ARGUMENT);
    });
}

secs_error_t secs_metrics_set_hook(const secs_metrics_hook_t *hook) {
    return guard_error([&]() -> secs_error_t {
        secs::core::MetricsHooks out{};
        if (hook) {
            out.counter = hook->counter;
            out.gauge = hook->gauge;
            out.histogram = hook->histogram;
            out.user_data = hook->user_data;
        }
        secs::core::set_metrics_hooks(out);
        return ok();
    });
}
