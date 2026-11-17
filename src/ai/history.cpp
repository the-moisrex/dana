#include "history.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace dana::ai {

    namespace {

        bool parse_digits(std::string_view text, std::size_t& pos, int count, int& out) {
            if (pos + static_cast<std::size_t>(count) > text.size()) {
                return false;
            }
            int value = 0;
            for (int i = 0; i < count; ++i) {
                char const digit = text[pos + static_cast<std::size_t>(i)];
                if (digit < '0' || digit > '9') {
                    return false;
                }
                value = (value * 10) + (digit - '0');
            }
            pos += static_cast<std::size_t>(count);
            out  = value;
            return true;
        }

    } // namespace

    std::string iso_from_epoch(double value) {
        if (value <= 0) {
            return {};
        }
        double const seconds = value > 1e12 ? value / 1000.0 : value;
        auto const   point =
          std::chrono::system_clock::time_point(std::chrono::milliseconds(static_cast<std::int64_t>(seconds * 1000.0)));
        std::time_t const raw = std::chrono::system_clock::to_time_t(point);
        std::tm           broken{};
        ::gmtime_r(&raw, &broken);
        std::ostringstream formatted;
        formatted << std::put_time(&broken, "%Y-%m-%dT%H:%M:%SZ");
        return formatted.str();
    }

    double epoch_of(std::string_view value) {
        if (value.empty()) {
            return 0;
        }
        std::size_t pos    = 0;
        int         year   = 0;
        int         month  = 0;
        int         day    = 0;
        int         hour   = 0;
        int         minute = 0;
        int         second = 0;
        if (!parse_digits(value, pos, 4, year)) {
            return 0;
        }
        if (pos >= value.size() || value[pos] != '-') {
            return 0;
        }
        ++pos;
        if (!parse_digits(value, pos, 2, month)) {
            return 0;
        }
        if (pos >= value.size() || value[pos] != '-') {
            return 0;
        }
        ++pos;
        if (!parse_digits(value, pos, 2, day)) {
            return 0;
        }
        if (pos >= value.size() || (value[pos] != 'T' && value[pos] != ' ')) {
            return 0;
        }
        ++pos;
        if (!parse_digits(value, pos, 2, hour)) {
            return 0;
        }
        if (pos >= value.size() || value[pos] != ':') {
            return 0;
        }
        ++pos;
        if (!parse_digits(value, pos, 2, minute)) {
            return 0;
        }
        if (pos >= value.size() || value[pos] != ':') {
            return 0;
        }
        ++pos;
        if (!parse_digits(value, pos, 2, second)) {
            return 0;
        }

        std::tm broken{};
        broken.tm_year = year - 1900;
        broken.tm_mon  = month - 1;
        broken.tm_mday = day;
        broken.tm_hour = hour;
        broken.tm_min  = minute;
        broken.tm_sec  = second;
        return static_cast<double>(::timegm(&broken));
    }

} // namespace dana::ai
