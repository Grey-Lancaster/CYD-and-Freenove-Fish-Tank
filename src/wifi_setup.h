#pragma once
#include <Arduino.h>
#include <functional>
void wifiSetupBegin(const char *hostname, std::function<void(const String &ip)> onConnected);
void wifiSetupLoop();
void wifiForgetAll();        // forget every saved network (the picker shows again after a restart)
bool wifiLangIsSpanish();        // language chosen on the WiFi picker (or the web page)
void wifiSetLang(bool spanish);
