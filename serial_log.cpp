#include "serial_log.h"

SerialLog SerialLogger;

void SerialLog::begin(size_t maxSize) {
    maxLen = maxSize;
    buffer.reserve(maxLen);
}

void SerialLog::add(const String& s) {
    buffer += s;

    // limitare buffer
    if (buffer.length() > maxLen) {
        buffer.remove(0, buffer.length() - maxLen);
    }
}

String SerialLog::get() const {
    return buffer;
}

void SerialLog::clear() {
    buffer = "";
}