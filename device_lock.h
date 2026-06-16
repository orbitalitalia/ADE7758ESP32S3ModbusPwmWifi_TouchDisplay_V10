#pragma once
#include <Arduino.h>

bool deviceLock_init();   // apelat la boot - false = clonă detectată
bool deviceLock_check();  // verificare periodică opțională
void deviceLock_erase();  // pentru re-inregistrare (service mode)