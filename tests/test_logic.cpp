#include "logic.h"
#include <assert.h>
#include <string>
int main() {
    char s[64];
    timeLabel(1791975632, s, sizeof(s));
    assert(std::string(s) == "05-07-22_14-30-32");
    timeLabel(0,s,sizeof(s)); assert(std::string(s)=="UNKNOWN");
    int y,m,d;
    jalali(2024,3,20,y,m,d); assert(y==1403 && m==1 && d==1);
    jalali(2025,3,20,y,m,d); assert(y==1403 && m==12 && d==30);
    jalali(2025,3,21,y,m,d); assert(y==1404 && m==1 && d==1);
    jalali(2026,3,21,y,m,d); assert(y==1405 && m==1 && d==1);
    int64_t e=0;
    assert(gpsEpoch("[SYS]-Time: GPS: 1791975632, NTP:0", e) && e==1791975632);
    assert(!gpsEpoch("[SYS]-Time: GPS: 0, NTP:0", e));
    assert(!gpsEpoch("[SYS]-Time: GPS: 1791975632junk", e));
    assert(!gpsEpoch("time=1791975632", e));
    assert(!newSession(100,120100,120000));
    assert(newSession(100,120101,120000));
    assert(windowStart(300,60299,60000)==300);
    assert(windowStart(300,60300,60000)==60300);
    puts("PASS: Jalali, Tehran, GPS parser, session threshold, window boundary");
}
