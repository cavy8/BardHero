// src/QpcClock.h
#pragma once

#include <Windows.h>

namespace SH {
    // One QPC domain for the whole plugin (clock + anchor publisher must
    // agree, spec 6).
    inline double QpcSec() {
        static const double freq = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return static_cast<double>(f.QuadPart);
        }();
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<double>(c.QuadPart) / freq;
    }
}
