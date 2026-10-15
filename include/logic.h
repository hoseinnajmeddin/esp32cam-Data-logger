#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Gregorian -> Solar Hijri. Integer 33-year-cycle conversion, used for 2020..2099.
inline void jalali(int gy, int gm, int gd, int &jy, int &jm, int &jd) {
    const int offsets[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    int gy2 = gm > 2 ? gy + 1 : gy;
    int days = 355666 + 365 * gy + (gy2 + 3) / 4 - (gy2 + 99) / 100
             + (gy2 + 399) / 400 + gd + offsets[gm - 1];
    jy = -1595 + 33 * (days / 12053);
    days %= 12053;
    jy += 4 * (days / 1461);
    days %= 1461;
    if (days > 365) { jy += (days - 1) / 365; days = (days - 1) % 365; }
    if (days < 186) { jm = 1 + days / 31; jd = 1 + days % 31; }
    else { jm = 7 + (days - 186) / 30; jd = 1 + (days - 186) % 30; }
}
inline bool validEpoch(int64_t e) { return e >= 1577836800LL && e < 4102444800LL; }
inline bool gpsEpoch(const char *line, int64_t &epoch) {
    const char *p = strstr(line, "[SYS]-Time: GPS: ");
    if (!p) return false;
    p += strlen("[SYS]-Time: GPS: ");
    if (*p < '0' || *p > '9') return false;
    char *end = nullptr;
    int64_t v = strtoll(p, &end, 10);
    if (*end != ',' && *end != '\r' && *end != '\0' && *end != ' ') return false;
    if (!validEpoch(v)) return false;
    epoch = v;
    return true;
}
inline void timeLabel(int64_t utc, char *out, size_t n) {
    if (!validEpoch(utc)) { snprintf(out, n, "UNKNOWN"); return; }
    // Tehran fixed UTC+03:30, without historical DST interpretation.
    int64_t shifted = utc + 12600;
    int days = static_cast<int>(shifted / 86400);
    int seconds = static_cast<int>(shifted % 86400);
    int gy = 1970;
    auto leap = [](int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
    while (days >= (leap(gy) ? 366 : 365)) days -= leap(gy++) ? 366 : 365;
    int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (leap(gy)) lengths[1] = 29;
    int gm = 0;
    while (days >= lengths[gm]) { days -= lengths[gm]; ++gm; }
    int y, m, d;
    jalali(gy, gm + 1, days + 1, y, m, d);
    snprintf(out, n, "%02d-%02d-%02d_%02d-%02d-%02d",
             y % 100, m, d, seconds / 3600, (seconds / 60) % 60, seconds % 60);
}
inline bool newSession(uint64_t previous, uint64_t now, uint64_t gap) {
    return now > previous && now - previous > gap;
}
inline uint64_t windowStart(uint64_t anchor, uint64_t now, uint64_t duration) {
    return anchor + ((now - anchor) / duration) * duration;
}
