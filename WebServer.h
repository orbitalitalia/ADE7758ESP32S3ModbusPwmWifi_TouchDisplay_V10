#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <WebServer.h>
#include <WebSocketsServer.h>

// ═══════════════════════════════════════════════════════════
// Declarații variabile globale (extern = doar declarație)
// ═══════════════════════════════════════════════════════════
extern WebSocketsServer webSocket;
extern bool websocketConnected;
extern bool resetEnergyRequested;

// ═══════════════════════════════════════════════════════════
// DOAR DECLARAȚII de funcții (fără implementare!)
// ═══════════════════════════════════════════════════════════

// WebSocket și comunicare
void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length);
void handleWebSocketMessage(uint8_t num, uint8_t *payload, size_t length);
void sendLiveData();
void webUiLogMessage(const char* msg);
void loopWebSocket();

// WebServer și rutare
void startWebServer();
void initWebServerAndSocket();
void handleWebClient();
void handleRoot();
void handleData();
void handleSetEnergy();
void handleCalibrate();
void handleNotFound();

// Handlers pentru control manual și stepuri
void handleToggleManualControl();
void handleToggleStep2();
void handleToggleStep3();
void handleToggleStep4();
void handleToggleStep();
void handleSetStepF();

// GPIO și inițializare
bool isGPIO0Safe();
bool safeGPIOInit();

#endif