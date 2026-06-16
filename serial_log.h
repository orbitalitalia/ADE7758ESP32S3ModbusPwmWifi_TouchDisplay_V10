#pragma once
#include <Arduino.h>

class SerialLog {
public:
    void begin(size_t maxSize = 4096);
    void add(const String& s);
    String get() const;
    void clear();

private:
    String buffer;
    size_t maxLen = 4096;
};

extern SerialLog SerialLogger;