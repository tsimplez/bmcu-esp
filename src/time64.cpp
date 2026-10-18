#include "time64.h"
// millis() 32 位回绕次数累计值，每次回绕增加 0x100000000
uint64_t _time64_time_H=0;
// 上一次读取到的 32 位 millis() 值，用于检测回绕
uint32_t _time64_time_L=0;
/**
 * 获取 64 位毫秒时间戳
 * @return 单调递增的 64 位毫秒时间
 * 把 32 位 millis() 与溢出高位拼接，避免约 49 天后的时间回绕。
 */
uint64_t get_time64()
{
    const uint32_t T=millis();
    if(T<_time64_time_L)
    {
        _time64_time_H+=0x100000000;
    }
    _time64_time_L=T;
    return _time64_time_H|_time64_time_L;
}
