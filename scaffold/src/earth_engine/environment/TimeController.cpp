#include "TimeController.h"
#include <chrono>
#include <cmath>

namespace earth_engine {

// Unix epoch (1970-01-01 00:00:00 UTC) as Julian Date
constexpr double kJdUnixEpoch = 2440587.5;
constexpr double kSecPerDay = 86400.0;

double unixToJulian(double unixTimestamp) {
    return kJdUnixEpoch + unixTimestamp / kSecPerDay;
}

double julianToUnix(double jd) {
    return (jd - kJdUnixEpoch) * kSecPerDay;
}

double currentJulianDate() {
    auto now = std::chrono::system_clock::now();
    auto dur = now.time_since_epoch();
    double sec = std::chrono::duration<double>(dur).count();
    return unixToJulian(sec);
}

// ============================================================
// TimeController
// ============================================================

TimeController::TimeController()
    : jd_(currentJulianDate()) {}

void TimeController::setJulianDate(double jd) {
    jd_ = jd;
}

void TimeController::setUnixTimestamp(double ts) {
    jd_ = unixToJulian(ts);
}

double TimeController::unixTimestamp() const {
    return julianToUnix(jd_);
}

void TimeController::advanceSeconds(double seconds) {
    jd_ += seconds / kSecPerDay;
}

} // namespace earth_engine
