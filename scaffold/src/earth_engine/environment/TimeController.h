#pragma once

#include <cstdint>

namespace earth_engine {

/// 统一时间源。
///
/// 环境系统（太阳、星空）使用此时间控制器，不放各自读取系统时间。
/// 内部存储 Julian Date（TT），提供 Unix 时间戳互转。
///
/// 使用方式：
///   1. setJulianDate() 或 setUnixTimestamp() 设置时间
///   2. advanceSeconds() 每帧步进
///   3. julianDate() / unixTimestamp() 读取
class TimeController {
public:
    TimeController();

    /// 当前 Julian Date（TT）
    double julianDate() const { return jd_; }
    void setJulianDate(double jd);

    /// Unix 时间戳（秒，UTC）
    double unixTimestamp() const;
    void setUnixTimestamp(double ts);

    /// 时间步进
    /// @param seconds 秒数（正=前进，负=倒退）
    void advanceSeconds(double seconds);

    /// 日长（秒）
    static constexpr double secondsPerDay = 86400.0;

private:
    double jd_;  // Julian Date (Terrestrial Time)
};

/// Unix → Julian Date
double unixToJulian(double unixTimestamp);

/// Julian Date → Unix
double julianToUnix(double jd);

/// 获取当前系统时间的 Julian Date
double currentJulianDate();

} // namespace earth_engine
