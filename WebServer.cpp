#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <WebServer.h>
#include "globals.h"
#include "NVRAM.h"
#include "ETHClass.h"
#include "serial_log.h"
#include <Update.h>

// ═══════════════════════════════════════════════════════════
// 🔒 Include-uri pentru verificare semnătură OTA
// ═══════════════════════════════════════════════════════════
#include "mbedtls/sha256.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// Forward declarations
bool verifyFirmwareSignature(const uint8_t*, size_t, const uint8_t*, size_t);
size_t base64_decode(const char*, uint8_t*, size_t);
// ═══════════════════════════════════════════════════════════
// 🔒 CHEIE PUBLICĂ RSA pentru verificare semnătură OTA
// ═══════════════════════════════════════════════════════════
const char* rsa_public_key_pem = R"(
-----BEGIN PUBLIC KEY-----
MIIBojANBgkqhkiG9w0BAQEFAAOCAY8AMIIBigKCAYEAq21jTfw7zCWF+j3IdbrD
F1BH9Jbwc6rKSeFR3VlDGNKv2jv2V8Xdc8A4Z+9f5UeiN/j3ObG9J65gcpY7KAOm
V1PREvoX7bqIZ4N+ri8m1ngmXXfi/T9HygagWmelgpqua4Y87bbeynVYBsEnbOOO
21+m6kQe1B8G7Wj/VNuoET74JdyuxPMtAW2lUDuWkmpeNcHrzhYIcvHqxGxb8mtI
TrVcqGt0zxrNX9LMPEU8rJ4abN6JYI9UgE+0sFliM1mKAiNx5i/eGfxF0X5d3u2B
osjD+TuGYiSuKA6N9XVpRk699DqSXPm8qER1xjXO6ypCOODBckkc9ASyBmrCwmfx
396n4Z+y86OSaeDVvqE41GsImHmKhVQR7v5tlpOQBhRrywdng4P5vUXodtzFRQXW
1EFgFDV3yQzSvNzs+AUceSO4GCL7sqT00k9i2PnrCRjheZqyvOEFqlEvXmM+UkHf
QBGCqYOsXQzTYIRuuYo0EhjBUx9LQJHEB26RwCXsBOidAgMBAAE=
-----END PUBLIC KEY-----
)";
// ↑ VEI COMPLETA CU CHEIA TA PUBLICĂ

// ═══════════════════════════════════════════════════════════
// 🔒 Decode Base64 pentru semnătură OTA
// ═══════════════════════════════════════════════════════════
#include "mbedtls/base64.h"

size_t base64_decode(const char* input, uint8_t* output, size_t max_len) {
  size_t olen = 0;
  int ret = mbedtls_base64_decode(output, max_len, &olen, 
                                   (const unsigned char*)input, strlen(input));
  if (ret == 0) {
    return olen;
  }
  return 0;
}


static WebServer server(80);
WebSocketsServer webSocket = WebSocketsServer(81);  // WebSocket pe port 81

static bool gpioInitialized = false;
static unsigned long lastGPIOInitAttempt = 0;
static const unsigned long GPIO_INIT_DELAY = 10000; // 10 secunde după pornire

bool websocketConnected = false;
static uint16_t wsClientCount = 0;

static void webLog(const String& msg) {
  SerialLogger.add("[" + String(millis()) + " ms] " + msg + "\n");
}

static String g_rootHtmlCache;
static uint32_t g_rootHtmlBuiltMs = 0;
static bool g_rootHtmlDirty = true;
static const uint32_t ROOT_CACHE_TTL_MS = 5000;

#define MANUAL_TIMEOUT_MS 60000 // 60 secunde

// ================= OTA SECURITY =================
static const char* OTA_USER = "admin";
static const char* OTA_PASS = "orbital123";

static inline bool otaAuthOrFail() {
  if (!server.authenticate(OTA_USER, OTA_PASS)) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

static uint32_t g_otaLastWsMs = 0;

static void wsOtaSend(const char* status, int progress /* -1 daca nu */) {
  StaticJsonDocument<192> doc;
  doc["target"] = "ota";
  if (status) doc["status"] = status;
  if (progress >= 0) doc["progress"] = progress;
  String out;
  serializeJson(doc, out);
  webSocket.broadcastTXT(out);
}

extern ETHClass ETH;  // declarăm instanța globală ETH

static bool initializePin(int pin, bool currentState) {
    try {
        pinMode(pin, OUTPUT);
        digitalWrite(pin, currentState ? HIGH : LOW);
        return true;
    } catch (...) {
        return false;
    }
}

// Trimite periodic date JSON către client
void sendLiveData() {
    StaticJsonDocument<1024> doc;

    const float va = isfinite((float)v_a) ? (float)v_a : 0.0f;
    const float vb = isfinite((float)v_b) ? (float)v_b : 0.0f;
    const float vc = isfinite((float)v_c) ? (float)v_c : 0.0f;

    doc["v_a"] = va;
    doc["v_b"] = vb;
    doc["v_c"] = vc;
    doc["c_a"] = c_a;
    doc["c_b"] = c_b;
    doc["c_c"] = c_c;

    doc["fw_version"]  = fwVersionString;
    doc["pf_use_rete"] = pfUseRete;

    

    // Folosește WattA/B/C din globals.h, nu p_a_ema etc!
doc["p_a"] = WattA / 1000.0f;
doc["p_b"] = WattB / 1000.0f;
doc["p_c"] = WattC / 1000.0f;

    // Puterea totală: suma fazelor, ca pe display!
doc["power"] = Watt / 1000.0f;
    doc["power_rete"] = PowerRete;
  doc["power_rete_source"] = (ethernetConnected && modbusConnected) ? "Modbus" : "ADE7758 (stima)";
    doc["energy"] = KWh;
    doc["freq"] = Frequency;
    doc["pf"]           = Pf;
    doc["cosfi_rete"]   = CosfiRete;
    doc["cosfi_source"] = (ethernetConnected && modbusConnected && pfUseRete) ? "Modbus" : "ADE7758";
    doc["Step1"] = Step1;
    doc["Step2"] = Step2;
    doc["Step3"] = Step3;
    doc["Step4"] = Step4;
    doc["manual"] = manualControlEnabled;
    doc["stepsEnabled"] = stepsEnabled;  // 🔹 Stare alimentare stepuri
   
    doc["connected"] = websocketConnected;
    doc["eth"] = ethernetConnected;
    doc["modbus"] = modbusConnected;
    doc["modbus_ip"] = MODBUS_SLAVE_IP.toString();  // ✅ Invia IP corrente
    doc["export_safety_active"] = g_exportSafetyActive;
    doc["export_excess_w"] = g_exportExcessW;

    String output;
    serializeJson(doc, output);
    webSocket.broadcastTXT(output);
}


static String getCommonStyles() {
    String styles = "<style>";
    styles += "body{font-family:'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;margin:0;padding:0;background-color:#f8f9fa;color:#212529}";
    styles += "h1{color:#0d6efd;text-align:center;font-size:2rem;margin:1rem 0;font-weight:600}";
    styles += ".container{max-width:1200px;margin:0 auto;padding:1rem}";
    styles += ".grid-container{display:grid;grid-template-columns:repeat(auto-fit, minmax(300px, 1fr));gap:1rem;margin-bottom:1.5rem}";
    styles += ".card{border-radius:0.5rem;box-shadow:0 0.125rem 0.25rem rgba(0,0,0,0.075);padding:1.25rem;background:#fff}";
    styles += ".card-header{font-size:1.25rem;font-weight:600;margin-bottom:1rem;color:#0d6efd;border-bottom:1px solid #dee2e6;padding-bottom:0.5rem}";
    styles += "table{width:100%;border-collapse:collapse;margin:1rem 0}";
    styles += "th{padding:0.75rem;text-align:center;border:1px solid #dee2e6;background-color:#f1f8ff;color:#0a58ca;font-size:1rem}";
    styles += "td{padding:0.75rem;text-align:center;border:1px solid #dee2e6;font-size:0.95rem}";
    styles += ".status-badge{display:inline-block;width:12px;height:12px;border-radius:50%;margin-right:0.5rem}";
    styles += ".btn{background-color:#0d6efd;color:white;padding:0.5rem 1rem;border:none;border-radius:0.25rem;cursor:pointer;font-size:0.9rem;transition:all 0.2s;margin-right:0.5rem}";
    styles += ".btn:hover{background-color:#0b5ed7;transform:translateY(-1px)}";
    styles += ".btn-danger{background-color:#dc3545}";
    styles += ".btn-success{background-color:#198754}";
    styles += ".btn-warning{background-color:#ffc107;color:#000}";
    styles += ".form-control{padding:0.5rem;border:1px solid #ced4da;border-radius:0.25rem;width:100%;margin-bottom:0.75rem}";
    styles += ".form-label{display:block;margin-bottom:0.5rem;font-weight:500}";
    styles += ".value-display{font-weight:600;color:#0a58ca}";
    styles += ".warning{color:#dc3545;font-weight:500}";
    styles += ".status-container{display:flex;align-items:center;margin-bottom:0.5rem}";
    styles += "#manual-indicator{background:#ffc107;color:#000;text-align:center;padding:0.5rem;font-weight:600;border-radius:0.25rem;margin-bottom:1rem}";
    styles += ".measurement-grid{display:grid;grid-template-columns:repeat(3, 1fr);gap:0.5rem}";
    styles += ".measurement-item{background:#f8f9fa;padding:0.75rem;border-radius:0.25rem;text-align:center}";
    styles += ".measurement-value{font-size:1.1rem;font-weight:600}";
    styles += ".measurement-label{font-size:0.8rem;color:#6c757d}";
    styles += ".alert-info{background:#cfe2ff;border-left:4px solid #0d6efd;padding:0.75rem;border-radius:0.25rem;margin-bottom:1rem}";
    styles += ".alert-warning{background:#fff3cd;border-left:4px solid #ffc107;padding:0.75rem;border-radius:0.25rem;margin-bottom:1rem}";
    styles += ".logs-box{display:none;white-space:pre-wrap;background:#111;color:#f1f1f1;padding:0.75rem;border-radius:0.25rem;max-height:260px;overflow:auto;font-family:Consolas,monospace;font-size:0.85rem}";
    styles += "@media (max-width: 768px){.grid-container{grid-template-columns:1fr}.measurement-grid{grid-template-columns:1fr}}";
    styles += "</style>";
    return styles;
}

// JavaScript injectabil în HTML prin getJavaScript()

String getJavaScript() {
  String js = "<script>";
  js += "let ws;";
  js += "let step1InputFocused = false;";
  js += "let step1InputLockUntil = 0;";
  js += "let modbusIpInputFocused = false;";
  js += "let energyInputFocused = false;";  // 🔹 nou
  js += "let logsVisible = false;";
  js += "let reconnectAttempts = 0;";
  js += "let maxReconnectAttempts = 0;";
  js += "const isNum = (v) => (typeof v === 'number' && isFinite(v));";
  js += "let lastWsMessageMs = Date.now();";
  js += "let wsConnectStartMs = 0;";

  js += "function refreshLogs() {";
  js += "  fetch('/logs', {cache:'no-store'})";
  js += "    .then(r => r.text())";
  js += "    .then(t => {";
  js += "      const box = document.getElementById('logs-panel');";
  js += "      if (!box) return;";
  js += "      box.textContent = t || 'Nessun log disponibile.';";
  js += "      box.scrollTop = box.scrollHeight;";
  js += "    })";
  js += "    .catch(() => {";
  js += "      const box = document.getElementById('logs-panel');";
  js += "      if (box) box.textContent = 'Errore lettura log.';";
  js += "    });";
  js += "}";

  js += "function toggleLogsPanel() {";
  js += "  const box = document.getElementById('logs-panel');";
  js += "  const btn = document.getElementById('logsToggleBtn');";
  js += "  if (!box || !btn) return;";
  js += "  logsVisible = !logsVisible;";
  js += "  box.style.display = logsVisible ? 'block' : 'none';";
  js += "  btn.textContent = logsVisible ? 'Ascunde Loguri' : 'Arata Loguri';";
  js += "  if (logsVisible) refreshLogs();";
  js += "}";

  js += "function clearLogs() {";
  js += "  fetch('/logs/clear', {method:'POST'})";
  js += "    .then(() => refreshLogs())";
  js += "    .catch(() => alert('Eroare clear log'));";
  js += "}";

  js += "function connectWebSocket() {";
  js += "  if (maxReconnectAttempts > 0 && reconnectAttempts >= maxReconnectAttempts) {";
  js += "    console.log('❌ Troppi tentativi WebSocket, interruzione...');";
  js += "    document.getElementById('status-dot').style.backgroundColor = 'red';";
  js += "    document.getElementById('status-msg').textContent = 'Connessione fallita';";
  js += "    return;";
  js += "  }";
  
  js += "  ws = new WebSocket('ws://' + location.hostname + ':81');";
  js += "  wsConnectStartMs = Date.now();";

  js += "  ws.onopen = () => {";
  js += "    console.log('✅ WebSocket connesso!');";
  js += "    document.getElementById('status-dot').style.backgroundColor = 'green';";
  js += "    const el = document.getElementById('ws-connecting');";
  js += "    if (el) el.style.display = 'none';";
  js += "    document.getElementById('status-msg').textContent = 'Connesso';";
  js += "    reconnectAttempts = 0;";
  js += "    lastWsMessageMs = Date.now();";
  js += "  };";

  js += "  ws.onerror = (error) => {";
  js += "    console.log('❌ Errore WebSocket:', error);";
  js += "  };";

  js += "  ws.onclose = (event) => {";
  js += "    console.log('🔄 WebSocket chiuso, codice:', event.code);";
  js += "    document.getElementById('status-dot').style.backgroundColor = 'red';";
  js += "    const el = document.getElementById('ws-connecting');";
  js += "    if (el) el.style.display = 'inline-block';";
  js += "    document.getElementById('status-msg').textContent = 'Riconnessione...';";
  js += "    reconnectAttempts++;";
  js += "    wsConnectStartMs = 0;";
  js += "    let delay = Math.min(100 + (reconnectAttempts * 50), 2000);";
  js += "    setTimeout(() => {";
  js += "      console.log('🔄 Nuovo tentativo WebSocket... tentativo', reconnectAttempts);";
  js += "      connectWebSocket();";
  js += "    }, delay);";
  js += "  };";

  js += "  ws.onmessage = (event) => {";
  js += "    try {";
  js += "    lastWsMessageMs = Date.now();";
  js += "    let data;";
js += "    try { data = JSON.parse(event.data); }";
js += "    catch(e){ console.log('WS bad JSON:', event.data); return; }";


  js += "    if (data.status && data.target === 'modbus') {";
  js += "      const el = document.getElementById('modbus-msg');";
  js += "      if (el) { el.textContent = data.status; el.style.color = '#28a745';";
  js += "        setTimeout(() => { el.textContent = ''; }, 3000); }";
  js += "    }";

  js += "    document.getElementById('eth-status').style.backgroundColor = data.eth ? 'green' : 'red';";
  js += "    document.getElementById('modbus-status').style.backgroundColor = data.modbus ? 'green' : 'red';";
  js += "    if (data.fw_version !== undefined) {";
  js += "      const fw = document.getElementById('fw-ver');";
  js += "      if (fw) fw.textContent = data.fw_version;";
  js += "    }";

  js += "    if (data.status && !data.target) {";  // status generic
  js += "      const el = document.getElementById('status-msg');";
  js += "      if (el) { el.textContent = data.status; el.style.color = '#28a745';";
  js += "        setTimeout(() => { el.textContent = 'Pronto'; el.style.color = '#555'; }, 3000); }";
  js += "    }";

  js += "    if (isNum(data.v_a)) document.getElementById('v_a').textContent = data.v_a.toFixed(2) + ' V';";
  js += "    if (isNum(data.v_b)) document.getElementById('v_b').textContent = data.v_b.toFixed(2) + ' V';";
  js += "    if (isNum(data.v_c)) document.getElementById('v_c').textContent = data.v_c.toFixed(2) + ' V';";
  js += "    if (isNum(data.c_a)) document.getElementById('c_a').textContent = data.c_a.toFixed(2) + ' A';";
  js += "    if (isNum(data.c_b)) document.getElementById('c_b').textContent = data.c_b.toFixed(2) + ' A';";
  js += "    if (isNum(data.c_c)) document.getElementById('c_c').textContent = data.c_c.toFixed(2) + ' A';";

  js += "    if (isNum(data.power))";
  js += "      document.getElementById('power').textContent = data.power.toFixed(2) + ' kW';";
  js += "      if (isNum(data.power_rete)) { const el = document.getElementById('power-rete'); if (el) el.textContent = data.power_rete.toFixed(2) + ' kW'; }";
  js += "      if (data.power_rete_source !== undefined) { const src = document.getElementById('power-rete-src'); if (src) src.textContent = data.power_rete_source; }";

  js += "    if (isNum(data.energy)) {";
  js += "      document.getElementById('energy').textContent = data.energy.toFixed(2) + ' kWh';";
  js += "      if (!energyInputFocused) {";
  js += "        const energyInput = document.getElementById('energySet');";
  js += "        if (energyInput) energyInput.value = data.energy.toFixed(2);";
  js += "      }";
  js += "    }";

  js += "    if (isNum(data.freq))";
  js += "      document.getElementById('freq').textContent = data.freq.toFixed(1) + ' Hz';";

  js += "    if (isNum(data.pf) && isNum(data.cosfi_rete)) {";
  js += "      const modbusOk = data.modbus === true;";
  js += "      const useRete = modbusOk && data.pf_use_rete;";
  js += "      const pfVal   = useRete ? data.cosfi_rete : data.pf;";
  js += "      const srcLabel = useRete";
  js += "        ? '<span style=\"color:#27ae60;font-size:0.8em\"> 📡 Rete</span>'";
  js += "        : '<span style=\"color:#e67e22;font-size:0.8em\"> ⚡ ADE7758</span>';";
  js += "      document.getElementById('pf').innerHTML = pfVal.toFixed(3) + srcLabel;";
  js += "      const btn = document.getElementById('pf-src-btn');";
  js += "      if (btn) btn.textContent = useRete ? '📡 Rete' : '⚡ ADE7758';";
  js += "    }";

  // Adaugă funcția toggle după bloc:
  js += "  function togglePfSource() {";
  js += "    fetch('/toggle_pf_source').then(r=>r.text()).then(t=>console.log('PF src:',t));";
  js += "  }";

  js += "    if (isNum(data.p_a)) document.getElementById('p_a').textContent = data.p_a.toFixed(2) + ' kW';";
  js += "    if (isNum(data.p_b)) document.getElementById('p_b').textContent = data.p_b.toFixed(2) + ' kW';";
  js += "    if (isNum(data.p_c)) document.getElementById('p_c').textContent = data.p_c.toFixed(2) + ' kW';";

  js += "    if (data.manual !== undefined) {";
  js += "      const manualDiv = document.getElementById('manual-indicator');";
  js += "      if (data.manual) { manualDiv.style.display = 'block'; manualDiv.textContent = '⚠️ Modalità Manuale Attiva'; }";
  js += "      else { manualDiv.style.display = 'none'; }";
  js += "    }";

js += "    if (data.Step1 !== undefined) {";
js += "      const input = document.getElementById('step1-value');";
js += "      const lockActive = Date.now() < step1InputLockUntil;";
js += "      const manualOn = data.manual === true;";
js += "      if (input && !step1InputFocused && !lockActive && !manualOn) {";
js += "        const pct = Math.round((data.Step1 * 100) / 4095);";
js += "        input.value = pct;";
js += "      }";
js += "    }";

  js += "    ['Step2', 'Step3', 'Step4'].forEach(step => {";
  js += "      const btn = document.getElementById(step);";
  js += "      if (btn && data[step] !== undefined) {";
  js += "        btn.className = data[step] ? 'btn btn-success' : 'btn btn-danger';";
  js += "        btn.textContent = step + ': ' + (data[step] ? 'ON' : 'OFF');";
  js += "      }";
  js += "    });";
  
  js += "    if (data.stepsEnabled !== undefined) {";
  js += "      const btn = document.getElementById('stepsEnableBtn');";
  js += "      if (btn) {";
  js += "        btn.className = data.stepsEnabled ? 'btn btn-success' : 'btn btn-danger';";
  js += "        btn.textContent = data.stepsEnabled ? '⚡ En Output: ON' : '⚡ En Output: OFF';";
  js += "      }";
  js += "    }";
  
  js += "    if (!modbusIpInputFocused && data.modbus_ip !== undefined) {";
  js += "      const ipInput = document.getElementById('modbus-ip');";
  js += "      if (ipInput) ipInput.value = data.modbus_ip;";
  js += "    }";
  js += "    } catch(e) { console.log('WS handler error:', e); }";
  
  js += "  };";
  js += "}";

  js += "function togglePfSource() {";
  js += "  fetch('/toggle_pf_source', {cache:'no-store'}).then(r=>r.text()).then(t=>console.log('PF:',t));";
  js += "}";

  js += "function toggleManualMode() {";
  js += "  if (!ws || ws.readyState !== WebSocket.OPEN) return;";
  js += "  const manualDiv = document.getElementById('manual-indicator');";
  js += "  const currentlyOn = manualDiv && manualDiv.style.display === 'block';";
  js += "  ws.send(JSON.stringify({ cmd: 'setManualMode', enabled: !currentlyOn }));";
  js += "}";

  js += "function toggleStep(step) {";
  js += "  ws.send(JSON.stringify({ cmd: 'toggle', step: step }));";
  js += "}";

  js += "function toggleStepsEnable() {";
  js += "  ws.send(JSON.stringify({ cmd: 'toggleStepsEnable' }));";
  js += "}";

js += "function updateStep1() {";
js += "  if (!ws || ws.readyState !== WebSocket.OPEN) {";
js += "    alert('WebSocket non connesso. Riprova.');";
js += "    return;";
js += "  }";
js += "  const input = document.getElementById('step1-value');";
js += "  if (!input) return;";
js += "  let percent = parseInt(input.value);";
js += "  if (isNaN(percent) || percent < 0 || percent > 100) {";
js += "    alert('Il valore deve essere tra 0 e 100 %');";
js += "    return;";
js += "  }";
js += "  const pwmRaw = Math.round(percent * 4095 / 100);";
// Blochează update-ul live suficient cât să ajungă confirmarea din backend
js += "  step1InputFocused = true;";
js += "  step1InputLockUntil = Date.now() + 5000;";
js += "  ws.send(JSON.stringify({ cmd: 'setStep1', value: pwmRaw }));";
js += "  setTimeout(() => { step1InputFocused = false; }, 5000);";
js += "}";

  js += "document.addEventListener('DOMContentLoaded', () => {";
  js += "  const input = document.getElementById('step1-value');";
  js += "  if (input) {";
  js += "    input.addEventListener('focus', () => { step1InputFocused = true; });";
  js += "    input.addEventListener('blur', () => { step1InputFocused = false; });";
  js += "  }";

  js += "  const ipInput = document.getElementById('modbus-ip');";
  js += "  if (ipInput) {";
  js += "    ipInput.addEventListener('focus', () => { modbusIpInputFocused = true; });";
  js += "    ipInput.addEventListener('blur', () => { modbusIpInputFocused = false; });";
  js += "  }";

  js += "  const energyInput = document.getElementById('energySet');";
  js += "  if (energyInput) {";
  js += "    energyInput.addEventListener('focus', () => { energyInputFocused = true; });";
  js += "    energyInput.addEventListener('blur', () => { energyInputFocused = false; });";
  js += "  }";

  js += "  connectWebSocket();";
  js += "  setInterval(() => {";
  js += "    const staleMs = Date.now() - lastWsMessageMs;";
  js += "    if (!ws) return;";
  js += "    if (ws.readyState === WebSocket.CONNECTING && wsConnectStartMs > 0 && (Date.now() - wsConnectStartMs) > 2500) {";
  js += "      console.log('⚠️ WS connect timeout, forcing reconnect...');";
  js += "      try { ws.close(); } catch(_) {}";
  js += "      return;";
  js += "    }";
  js += "    if (ws.readyState === WebSocket.OPEN && staleMs > 3000) {";
  js += "      console.log('⚠️ WS stale stream, forcing reconnect...');";
  js += "      try { ws.close(); } catch(_) {}";
  js += "    }";
  js += "  }, 1000);";
  js += "  setInterval(() => { if (logsVisible) refreshLogs(); }, 1500);";
  js += "});";

  // ✅ Auto-Zero offset
  js += "function autoZeroCalibration() {";
  js += "  if (!confirm('⚠️ IMPORTANTE:\\n\\n1. Scollegare TUTTI gli ingressi di tensione e corrente\\n2. Attendere 5 secondi\\n3. Premere OK per salvare automaticamente gli offset')) return;";
  js += "  const v_a_text = document.getElementById('v_a').textContent.replace(' V', '');";
  js += "  const v_b_text = document.getElementById('v_b').textContent.replace(' V', '');";
  js += "  const v_c_text = document.getElementById('v_c').textContent.replace(' V', '');";
  js += "  const c_a_text = document.getElementById('c_a').textContent.replace(' A', '');";
  js += "  const c_b_text = document.getElementById('c_b').textContent.replace(' A', '');";
  js += "  const c_c_text = document.getElementById('c_c').textContent.replace(' A', '');";
  js += "  const data = {";
  js += "    cmd: 'calibrate',";
  js += "    voltageOffsetA: parseFloat(v_a_text),";
  js += "    voltageOffsetB: parseFloat(v_b_text),";
  js += "    voltageOffsetC: parseFloat(v_c_text),";
  js += "    currentOffsetA: parseFloat(c_a_text),";
  js += "    currentOffsetB: parseFloat(c_b_text),";
  js += "    currentOffsetC: parseFloat(c_c_text)";
  js += "  };";
  js += "  console.log('Auto-Zero data:', data);";
  js += "  ws.send(JSON.stringify(data));";
  js += "  document.getElementById('voltageOffsetA').value = data.voltageOffsetA.toFixed(2);";
  js += "  document.getElementById('voltageOffsetB').value = data.voltageOffsetB.toFixed(2);";
  js += "  document.getElementById('voltageOffsetC').value = data.voltageOffsetC.toFixed(2);";
  js += "  document.getElementById('currentOffsetA').value = data.currentOffsetA.toFixed(2);";
  js += "  document.getElementById('currentOffsetB').value = data.currentOffsetB.toFixed(2);";
  js += "  document.getElementById('currentOffsetC').value = data.currentOffsetC.toFixed(2);";
  js += "  alert('✅ Auto-Zero completato! Gli offset sono stati salvati.');";
  js += "}";

  // ✅ Salvează calibrarea generală + offset-uri
  js += "function saveCalibration() {";
  js += "  const data = {";
  js += "    cmd: 'calibrate',";
  js += "    setpoint: parseFloat(document.getElementById('setpoint').value),";
  js += "    currentFactor: parseFloat(document.getElementById('currentFactor').value),";
  js += "    CT_Primary: parseFloat(document.getElementById('CT_Primary').value),";
  js += "    CT_Secondary: parseFloat(document.getElementById('CT_Secondary').value),";
  js += "    voltageFactor: parseFloat(document.getElementById('voltageFactor').value),";
  js += "    prez1: parseInt(document.getElementById('prez1').value),";
  js += "    prez2: parseInt(document.getElementById('prez2').value),";
  js += "    prez3: parseInt(document.getElementById('prez3').value),";
  js += "    prez4: parseInt(document.getElementById('prez4').value),";
  js += "    voltageOffsetA: parseFloat(document.getElementById('voltageOffsetA').value),";
  js += "    voltageOffsetB: parseFloat(document.getElementById('voltageOffsetB').value),";
  js += "    voltageOffsetC: parseFloat(document.getElementById('voltageOffsetC').value),";
  js += "    currentOffsetA: parseFloat(document.getElementById('currentOffsetA').value),";
  js += "    currentOffsetB: parseFloat(document.getElementById('currentOffsetB').value),";
  js += "    currentOffsetC: parseFloat(document.getElementById('currentOffsetC').value)";
  js += "  };";
  js += "  ws.send(JSON.stringify(data));";
  js += "  alert('Calibrazione salvata!');";
  js += "}";

  // ✅ Energie: reset + setare manuală
  js += "function resetEnergy() {";
  js += "  if (!confirm('Azzerare l\\'energia accumulata?')) return;";
  js += "  const input = document.getElementById('energySet');";
  js += "  if (input) input.value = '0.00';";
  js += "  ws.send(JSON.stringify({ cmd: 'setEnergy', energy: 0 }));";
  js += "}";

  js += "function setEnergyValue() {";
  js += "  const input = document.getElementById('energySet');";
  js += "  if (!input) return;";
  js += "  const val = parseFloat(input.value);";
  js += "  if (isNaN(val)) {";
  js += "    alert('Valore energia non valido');";
  js += "    return;";
  js += "  }";
  js += "  ws.send(JSON.stringify({ cmd: 'setEnergy', energy: val }));";
  js += "}";

  // ✅ IP Modbus
  js += "function setModbusIP() {";
  js += "  const ip = document.getElementById('modbus-ip').value;";
  js += "  ws.send(JSON.stringify({ cmd: 'setModbusIP', ip: ip }));";
  js += "}";

// ================= OTA JS (cu prompt user/pass o singura data) =================
js += "let otaAuth = '';"; // 'Basic base64(user:pass)'

js += "function ensureOtaAuth(){";
js += "  if(otaAuth) return true;";
js += "  const u = prompt('OTA user:', 'admin');";
js += "  if(u === null) return false;";
js += "  const p = prompt('OTA password:');";
js += "  if(p === null) return false;";
js += "  otaAuth = 'Basic ' + btoa(u + ':' + p);";
js += "  return true;";
js += "}";

js += "async function startOtaUpdate(){";
js += "  const f = document.getElementById('ota-file').files[0];";
js += "  if(!f){ alert('Seleziona un file .bin'); return; }";
js += "  if(!confirm('⚠️ Confermi aggiornamento firmware?\\n\\n- Non spegnere il dispositivo\\n- Attendi il reboot')) return;";
js += "  if(!confirm('Conferma finale: vuoi davvero procedere?')) return;";

js += "  if(!ensureOtaAuth()) return;";

js += "  document.getElementById('ota-msg').textContent='Autenticazione...';";

js += "  try {";
js += "    const r = await fetch('/ota_status', { cache:'no-store', headers:{ 'Authorization': otaAuth } });";
js += "    if(!r.ok){";
js += "      document.getElementById('ota-msg').textContent='Auth fallita (401)';";
js += "      otaAuth='';";
js += "      return;";
js += "    }";
js += "  } catch(e){";
js += "    document.getElementById('ota-msg').textContent='Errore rete (auth)';";
js += "    return;";
js += "  }";

js += "  const form = new FormData();";
js += "  form.append('update', f);";

js += "  document.getElementById('ota-msg').textContent='Upload in corso...';";
js += "  document.getElementById('ota-bar').style.width='0%';";
js += "  document.getElementById('ota-pct').textContent='0%';";

js += "  const xhr = new XMLHttpRequest();";
js += "  xhr.open('POST', '/update', true);";
js += "  xhr.setRequestHeader('Authorization', otaAuth);";
js += "  xhr.setRequestHeader('X-OTA-CONFIRM','YES');";

// 🔒 DEV MODE: semnătura nu e trimisă (comentat)
// Pentru securitate maximă, decomentează după testare:
// js += "  xhr.setRequestHeader('X-OTA-Signature', signatureBase64);";

js += "  xhr.upload.onprogress = (e)=>{";
js += "    if(e.lengthComputable){";
js += "      const pct = Math.round((e.loaded/e.total)*100);";
js += "      document.getElementById('ota-bar').style.width = pct+'%';";
js += "      document.getElementById('ota-pct').textContent = pct+'%';";
js += "    }";
js += "  };";

js += "  xhr.onload = ()=>{";
js += "    const t = xhr.responseText || '';";
js += "    if(xhr.status === 200){";
js += "      document.getElementById('ota-msg').textContent = t || 'OK. Rebooting...';";
js += "      setTimeout(()=>location.reload(), 8000);";
js += "    } else if(xhr.status === 401){";
js += "      document.getElementById('ota-msg').textContent = 'Auth fallita (401)';";
js += "      otaAuth='';";
js += "    } else if(xhr.status === 403){";
js += "      document.getElementById('ota-msg').textContent = 'Conferma mancante o firma invalida (403)';";
js += "    } else {";
js += "      document.getElementById('ota-msg').textContent = 'OTA failed: ' + t;";
js += "    }";
js += "  };";

js += "  xhr.onerror = ()=>{";
js += "    const pct = document.getElementById('ota-pct').textContent;";
js += "    if(pct === '100%'){";
js += "      document.getElementById('ota-msg').textContent = 'Upload completato. Dispositivo in riavvio...';";
js += "      setTimeout(()=>location.reload(), 8000);";
js += "    } else {";
js += "      document.getElementById('ota-msg').textContent = 'Errore rete OTA';";
js += "    }";
js += "  };";

js += "  xhr.send(form);";
js += "}";

    // ═══════════════════════════════════════════════════════════
    // 🔄 RESTART ESP32 cu confirmare în italiană
    // ═══════════════════════════════════════════════════════════
    js += "function restartESP() {";
    js += "  if (!confirm('⚠️ Sei sicuro di voler riavviare l\\'ESP32?\\n\\n' +";
    js += "               'Il sistema si riavvierà e perderai la connessione per circa 10 secondi.')) {";
    js += "    return;";
    js += "  }";
    js += "  fetch('/restart', { method: 'POST' })";
    js += "    .then(response => {";
    js += "      if (response.ok) {";
    js += "        alert('✅ Riavvio in corso...\\n\\nAttendi 10 secondi e ricarica la pagina.');";
    js += "        setTimeout(() => location.reload(), 10000);";
    js += "      } else {";
    js += "        alert('❌ Errore durante il riavvio!');";
    js += "      }";
    js += "    })";
    js += "    .catch(err => {";
    js += "      console.error('Restart error:', err);";
    js += "      alert('❌ Errore di comunicazione!');";
    js += "    });";
    js += "}";
    
    js += "</script>";
  return js;
}

static void handleOTAStatus() {
  if (!otaAuthOrFail()) return;
  server.send(200, "application/json", "{\"ok\":true}");
}

// ═══════════════════════════════════════════════════════════
// 🔒 Handler OTA cu verificare semnătură RSA
// ═══════════════════════════════════════════════════════════
static void handleOTAUpload() {
  if (!otaAuthOrFail()) return;

  HTTPUpload& upload = server.upload();
  static size_t otaTotalLen = 0;
  
  // 🔒 Buffer pentru firmware (verificare semnătură)
  static uint8_t* firmware_buffer = nullptr;
  static size_t firmware_size = 0;
  static bool update_started = false;

  if (upload.status == UPLOAD_FILE_START) {

    String hdr = server.header("X-OTA-CONFIRM");
    if (hdr != "YES") {
      if (Update.isRunning()) Update.abort();
      wsOtaSend("OTA blocat: confirmare lipsa", -1);
      return;
    }

    wsOtaSend("OTA: start upload", 0);
    g_otaLastWsMs = 0;

    otaTotalLen = (size_t)server.header("Content-Length").toInt();
    Serial.printf("🔒 OTA Content-Length: %u bytes\n", (unsigned)otaTotalLen);
    
    // 🔒 Alocă buffer pentru firmware (max 4MB)
    firmware_size = 0;
    firmware_buffer = (uint8_t*)malloc(4 * 1024 * 1024);
    
    if (!firmware_buffer) {
      Serial.println("❌ Eroare alocare memorie pentru firmware!");
      wsOtaSend("OTA: memorie insuficienta", -1);
      return;
    }
    
    Serial.println("✅ Buffer firmware alocat");

    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      wsOtaSend("OTA: Update.begin() FAILED", -1);
      free(firmware_buffer);
      firmware_buffer = nullptr;
      return;
    }
    
    update_started = true;
    Serial.println("✅ OTA Update început");
  }
  else if (upload.status == UPLOAD_FILE_WRITE && update_started) {
    
    // 🔒 Salvează în buffer pentru verificare
    if (firmware_buffer && (firmware_size + upload.currentSize) < 4*1024*1024) {
      memcpy(firmware_buffer + firmware_size, upload.buf, upload.currentSize);
      firmware_size += upload.currentSize;
    }

    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      wsOtaSend("OTA: write FAILED", -1);
      return;
    }

    int pct = -1;
    if (otaTotalLen > 0) {
      pct = (int)((100ULL * upload.totalSize) / otaTotalLen);
      if (pct < 0) pct = 0;
      if (pct > 100) pct = 100;
    }

    uint32_t now = millis();
    if (pct >= 0 && (now - g_otaLastWsMs) > 100) {
      g_otaLastWsMs = now;
      wsOtaSend("OTA: uploading", pct);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    
    Serial.printf("📦 Upload terminat: %u bytes\n", firmware_size);
    wsOtaSend("OTA: verificare semnatura...", 99);
    
// 🔒 VERIFICĂ SEMNĂTURA
String sigHeader = server.header("X-OTA-Signature");

bool signature_valid = false;

if (sigHeader.length() > 0 && firmware_buffer) {
  Serial.println("🔒 Semnătură primită în header X-OTA-Signature");
  Serial.printf("   Lungime Base64: %d caractere\n", sigHeader.length());
  
  // Alocă buffer pentru semnătură decodată (384 bytes pentru RSA 3072-bit)
  uint8_t* sig_buffer = (uint8_t*)malloc(512);
  
  if (sig_buffer) {
    // Decode Base64
    size_t decoded_len = base64_decode(sigHeader.c_str(), sig_buffer, 512);
    
    if (decoded_len > 0) {
      Serial.printf("✅ Semnătură decodată: %d bytes\n", decoded_len);
      
      // VERIFICĂ SEMNĂTURA!
      signature_valid = verifyFirmwareSignature(firmware_buffer, firmware_size,
                                                sig_buffer, decoded_len);
      
      if (!signature_valid) {
        Serial.println("❌❌❌ SEMNĂTURĂ INVALIDĂ - UPDATE RESPINS! ❌❌❌");
        Update.abort();
        free(sig_buffer);
        free(firmware_buffer);
        firmware_buffer = nullptr;
        
        wsOtaSend("OTA: Semnatura INVALIDA!", -1);
        server.send(403, "text/plain", "Invalid signature - update rejected!");
        return;
      }
      
      Serial.println("✅✅✅ SEMNĂTURĂ VALIDĂ - UPDATE AUTORIZAT! ✅✅✅");
      
    } else {
      Serial.println("❌ Eroare decode Base64!");
    }
    
    free(sig_buffer);
  }
} else {
  Serial.println("⚠️ Fără semnătură în header");
  
  // 🔒 OPȚIONAL: Respinge update-uri fără semnătură
  // Decomentează pentru securitate MAXIMĂ:
  /*
  Serial.println("❌ UPDATE RESPINS - lipsește semnătura!");
  Update.abort();
  free(firmware_buffer);
  firmware_buffer = nullptr;
  wsOtaSend("OTA: Semnatura lipsa!", -1);
  server.send(403, "text/plain", "Signature required!");
  return;
  */
  
  // Pentru DEV MODE - permite update fără semnătură
  Serial.println("⚠️ DEV MODE - continuu fără verificare semnătură");
}
    
    // Eliberează buffer
    if (firmware_buffer) {
      free(firmware_buffer);
      firmware_buffer = nullptr;
    }
    firmware_size = 0;

    if (Update.end(true)) {
      wsOtaSend("OTA: OK, reboot...", 100);
      server.send(200, "text/plain", "OK. Rebooting...");
      delay(400);
      ESP.restart();
    } else {
      Update.printError(Serial);
      wsOtaSend("OTA: FAILED at end()", -1);
      server.send(500, "text/plain", "OTA failed.");
    }
    
    update_started = false;
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    wsOtaSend("OTA: aborted", -1);
    
    if (firmware_buffer) {
      free(firmware_buffer);
      firmware_buffer = nullptr;
    }
    firmware_size = 0;
    update_started = false;
  }
}


static void handleRoot() {
  const uint32_t now = millis();
  if (!g_rootHtmlDirty && g_rootHtmlCache.length() > 0 && (now - g_rootHtmlBuiltMs) < ROOT_CACHE_TTL_MS) {
    server.send(200, "text/html", g_rootHtmlCache);
    return;
  }

    String html = "<!DOCTYPE html><html lang='it'><head><meta charset='UTF-8'>";
  html.reserve(24576);
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<title>EBS Orbital Monitor</title>";
    html += getCommonStyles();
    html += "</head><body>";
    html += "<div class='container'>";
    html += "<h1>⚡ ELC Orbital Italia Power Monitor</h1>";

    html += "<div id='manual-indicator' style='display:none;'>⚠️ Modalità Manuale Attiva</div>";

    html += "<div class='status-container'>";
    html += "<span class='status-badge' id='status-dot' style='background-color:red'></span>";
    html += "<span id='status-msg'>Connessione in corso...</span>";
    html += "<span id='ws-connecting' style='margin-left:10px;color:#888'>Connessione WebSocket...</span>";
    html += "<span style='margin-left:auto;color:#aaa;font-size:0.85em'>FW v<span id='fw-ver'>" + String(fwVersionString) + "</span></span>";
    html += "</div>";;

    // =================== MĂSURĂTORI ÎN TIMP REAL ===================
    html += "<div class='grid-container'>";
    
    html += "<div class='card'>";
    html += "<div class='card-header'>Misure in Tempo Reale</div>";
    html += "<table><tr><th>Parametro</th><th>Fase A</th><th>Fase B</th><th>Fase C</th></tr>";
    html += "<tr><td>Tensione</td><td><span id='v_a'>0 V</span></td><td><span id='v_b'>0 V</span></td><td><span id='v_c'>0 V</span></td></tr>";
    html += "<tr><td>Corrente</td><td><span id='c_a'>0 A</span></td><td><span id='c_b'>0 A</span></td><td><span id='c_c'>0 A</span></td></tr>";
    html += "<tr><td>Potenza attiva</td><td><span id='p_a'>0 kW</span></td><td><span id='p_b'>0 kW</span></td><td><span id='p_c'>0 kW</span></td></tr>";
    html += "</table>";
    html += "<div style='margin-top:1rem'>";
    html += "<strong>Potenza totale:</strong> <span class='value-display' id='power'>0 kW</span><br>";
    html += "<strong>Potenza rete:</strong> <span class='value-display' id='power-rete'>0 kW</span> <span id='power-rete-src' style='font-size:0.8em;color:#666'>(n/a)</span><br>";
    html += "<strong>Energia:</strong> <span class='value-display' id='energy'>0 kWh</span><br>";
    html += "<strong>Frequenza:</strong> <span id='freq'>50.0 Hz</span><br>";
    html += "<strong>Fattore di potenza:</strong> <span id='pf'>1.000</span> ";
    html += "<button id='pf-src-btn' onclick='togglePfSource()' style='font-size:0.8em;padding:2px 8px;margin-left:6px'>📡 Rete</button>";
    html += "</div></div>";

    // =================== CONTROL MANUAL ===================
    html += "<div class='card'>";
    html += "<div class='card-header'>Controllo Manuale</div>";
    
    // 🔹 Buton ENABLE/DISABLE alimentare stepuri + RESTART ESP32
    html += "<div style='margin-bottom:1rem'>";
    html += "<button class='btn btn-success' id='stepsEnableBtn' onclick='toggleStepsEnable()'>⚡ Alimentare: ON</button>";
    html += " ";  // Spațiu între butoane
    html += "<button class='btn btn-danger' onclick='restartESP()' style='margin-left:10px'>🔄 Riavvia ESP</button>";
    html += "<p style='margin-top:0.5rem;color:#666;font-size:0.9rem'>Controllo alimentazione generale Step 1-4 (mosfet P) | Riavvio sistema ESP32</p>";
    html += "</div>";
    html += "<hr style='border:none;border-top:1px solid #ddd;margin:1rem 0'>";
    
    html += "<button class='btn btn-warning' onclick='toggleManualMode()'>Attiva/Disattiva Modalità Manuale</button>";
    html += "<div style='margin-top:1rem'>";
    html += "<label class='form-label'>Step1 (0-100%):</label>";
    html += "<input type='number' id='step1-value' class='form-control' min='0' max='100' step='1' value='0'>";
    html += "<button class='btn' onclick='updateStep1()'>Imposta Step1</button>";
    html += "</div>";
    html += "<div style='margin-top:1rem'>";
    html += "<button class='btn btn-danger' id='Step2' onclick='toggleStep(\"Step2\")'>Step2: OFF</button>";
    html += "<button class='btn btn-danger' id='Step3' onclick='toggleStep(\"Step3\")'>Step3: OFF</button>";
    html += "<button class='btn btn-danger' id='Step4' onclick='toggleStep(\"Step4\")'>Step4: OFF</button>";
    html += "</div></div>";

    // =================== STATUS REȚEA ===================
    html += "<div class='card'>";
    html += "<div class='card-header'>Stato della Rete</div>";
    html += "<div class='status-container'><span class='status-badge' id='eth-status'></span>Ethernet</div>";
    html += "<div class='status-container'><span class='status-badge' id='modbus-status'></span>Modbus TCP</div>";
    html += "<div id='modbus-msg' style='color:#28a745;font-weight:500;margin-top:0.5rem'></div>";
    html += "<div style='margin-top:1rem'>";
    html += "<label class='form-label'>IP Slave Modbus:</label>";
    html += "<input type='text' id='modbus-ip' class='form-control' value='" + MODBUS_SLAVE_IP.toString() + "'>";
    html += "<button class='btn' onclick='setModbusIP()'>Salva IP</button>";
    html += "</div></div>";

    html += "<div class='card'>";
    html += "<div class='card-header'>Loguri Sistem</div>";
    html += "<div style='margin-bottom:0.75rem'>";
    html += "<button class='btn' id='logsToggleBtn' onclick='toggleLogsPanel()'>Arata Loguri</button>";
    html += "<button class='btn' onclick='refreshLogs()'>Refresh</button>";
    html += "<button class='btn btn-danger' onclick='clearLogs()'>Clear</button>";
    html += "</div>";
    html += "<pre id='logs-panel' class='logs-box'>Nessun log disponibile.</pre>";
    html += "</div>";
    
    html += "</div>"; // închide grid-container
	
	// =================== OTA UPDATE ===================
html += "<div class='card'>";
html += "<div class='card-header'>⬆️ Firmware Update (OTA)</div>";

html += "<div class='alert-info'>";
html += "<strong>Nota:</strong> Aggiornamento protetto da password + conferma. ";
html += "Durante l'upload vedrai il progresso in tempo reale.";
html += "</div>";

html += "<label class='form-label'>Seleziona firmware (.bin):</label>";
html += "<input type='file' id='ota-file' class='form-control' accept='.bin'>";

html += "<div style='margin-top:0.75rem'>";
html += "<button class='btn btn-warning' onclick='startOtaUpdate()' style='font-weight:600'>Avvia OTA</button>";
html += "<span id='ota-msg' style='margin-left:10px;color:#555'>Pronto</span>";
html += "</div>";

// progress bar simplu
html += "<div style='margin-top:1rem;background:#e9ecef;border-radius:6px;overflow:hidden;height:18px'>";
html += "<div id='ota-bar' style='height:18px;width:0%;background:#0d6efd'></div>";
html += "</div>";
html += "<div style='margin-top:0.5rem'><strong>Progresso:</strong> <span id='ota-pct'>0%</span></div>";

html += "</div>";


    // =================== CALIBRARE GENERALĂ ===================
    html += "<div class='card' style='margin-top:1rem'>";
    html += "<div class='card-header'>⚙️ Calibrazione Generale</div>";
    
    html += "<div class='grid-container' style='grid-template-columns:repeat(2, 1fr)'>";
    
    html += "<div>";
    html += "<label class='form-label'>Setpoint (W):</label>";
    html += "<input type='number' class='form-control' id='setpoint' value='" + String(setpoint) + "'>";
    html += "<label class='form-label'>Fattore Corrente:</label>";
    html += "<input type='number' step='0.001' class='form-control' id='currentFactor' value='" + String(currentFactor) + "'>";
    html += "<label class='form-label'>TA Primario (A):</label>";
    html += "<input type='number' step='1' class='form-control' id='CT_Primary' value='" + String(CT_Primary, 0) + "'>";
    html += "<label class='form-label'>TA Secondario (A):</label>";
    html += "<input type='number' step='0.1' class='form-control' id='CT_Secondary' value='" + String(CT_Secondary, 1) + "'>";
    html += "<label class='form-label'>Fattore Tensione:</label>";
    html += "<input type='number' step='0.001' class='form-control' id='voltageFactor' value='" + String(voltageFactor) + "'>";
    html += "</div>";
    
    html += "<div>";
    html += "<label class='form-label'>Prez1 (W):</label>";
    html += "<input type='number' class='form-control' id='prez1' value='" + String(prez1) + "'>";
    html += "<label class='form-label'>Prez2 (W):</label>";
    html += "<input type='number' class='form-control' id='prez2' value='" + String(prez2) + "'>";
    html += "<label class='form-label'>Prez3 (W):</label>";
    html += "<input type='number' class='form-control' id='prez3' value='" + String(prez3) + "'>";
    html += "<label class='form-label'>Prez4 (W):</label>";
    html += "<input type='number' class='form-control' id='prez4' value='" + String(prez4) + "'>";
    html += "</div>";
    
    html += "</div>";
    
    // Butonul de calibrare generală rămâne
    html += "<button class='btn btn-success' onclick='saveCalibration()' style='margin-top:1rem'>Salva Calibrazione</button>";
    
    // 🔹 Secțiune nouă: setare manuală energie + reset
    html += "<div style='margin-top:1rem'>";
    html += "<label class='form-label'>Energia (kWh):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='energySet' value='" + String(KWh, 2) + "'>";
    html += "<button class='btn' style='margin-top:0.5rem;margin-right:0.5rem' onclick='setEnergyValue()'>Salva Energia</button>";
    html += "<button class='btn btn-danger' style='margin-top:0.5rem' onclick='resetEnergy()'>Azzera Energia</button>";
    html += "</div>";

    html += "</div>";


    // =================== CALIBRARE OFFSET (MUTATĂ LA SFÂRȘIT) ===================
    html += "<div class='card' style='margin-top:1rem'>";
    html += "<div class='card-header'>🔧 Calibrazione Offset (Correzione Zero)</div>";
    
    html += "<div class='alert-warning'>";
    html += "<strong>⚠️ Procedura di calibrazione offset:</strong><br>";
    html += "1. Scollegare TUTTI gli ingressi di tensione e corrente<br>";
    html += "2. Attendere 5 secondi per la stabilizzazione<br>";
    html += "3. Premere il pulsante <strong>Auto-Zero</strong> per la calibrazione automatica<br>";
    html += "4. OPPURE inserire manualmente i valori visualizzati e premere <strong>Salva Calibrazione</strong>";
    html += "</div>";

    html += "<button class='btn btn-warning' onclick='autoZeroCalibration()' style='margin-bottom:1rem;font-weight:600'>⚡ Auto-Zero (Calibrazione Automatica)</button>";

    html += "<div class='grid-container' style='grid-template-columns:repeat(2, 1fr)'>";
    
    // Coloana 1 - Offset-uri tensiune
    html += "<div>";
    html += "<h4 style='color:#0d6efd;margin-bottom:0.5rem'>Offset Tensione</h4>";
    html += "<label class='form-label'>Offset Tensione A (V):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='voltageOffsetA' value='" + String(voltageOffsetA, 2) + "'>";
    html += "<label class='form-label'>Offset Tensione B (V):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='voltageOffsetB' value='" + String(voltageOffsetB, 2) + "'>";
    html += "<label class='form-label'>Offset Tensione C (V):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='voltageOffsetC' value='" + String(voltageOffsetC, 2) + "'>";
    html += "</div>";
    
    // Coloana 2 - Offset-uri curent
    html += "<div>";
    html += "<h4 style='color:#0d6efd;margin-bottom:0.5rem'>Offset Corrente</h4>";
    html += "<label class='form-label'>Offset Corrente A (A):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='currentOffsetA' value='" + String(currentOffsetA, 2) + "'>";
    html += "<label class='form-label'>Offset Corrente B (A):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='currentOffsetB' value='" + String(currentOffsetB, 2) + "'>";
    html += "<label class='form-label'>Offset Corrente C (A):</label>";
    html += "<input type='number' step='0.01' class='form-control' id='currentOffsetC' value='" + String(currentOffsetC, 2) + "'>";
    html += "</div>";
    
    html += "</div>"; // închide grid pentru offset-uri
    html += "</div>"; // închide card offset

    html += "</div>"; // închide container
    html += getJavaScript();
    html += "</body></html>";

    g_rootHtmlCache = html;
    g_rootHtmlBuiltMs = now;
    g_rootHtmlDirty = false;

    server.send(200, "text/html", g_rootHtmlCache);
}


static void handleSettings() {
    server.sendHeader("Location", "/");
    server.send(303);
}

static void handleNotFound() {
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET) ? "GET" : "POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";
    for (uint8_t i = 0; i < server.args(); i++) {
        message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
}



void handleToggleManualControl() {
    manualControlEnabled = !manualControlEnabled;
    manualControlTimeout = millis();
    server.send(200, "application/json", "{\"success\":true}");
}

// Primește comenzi de la clientul WebSocket
void handleWebSocketMessage(uint8_t num, uint8_t *payload, size_t length) {
    String msg = String((char*)payload);
    StaticJsonDocument<512> doc;
    DeserializationError err = deserializeJson(doc, msg);
    if (err) return;
    

    if (!doc.containsKey("cmd")) return;
    String cmd = doc["cmd"].as<String>();
    webLog("WS cmd from client " + String(num) + ": " + cmd);

    // 🔹 Toggle Step2/3/4 în modul manual
    if (cmd == "toggle") {
        if (!manualControlEnabled) {
            webSocket.sendTXT(num, "{\"status\":\"È necessario attivare la modalità manuale\"}");
            return;
        }
        String step = doc["step"].as<String>();
        if (step == "Step2") Step2 = !Step2;
        if (step == "Step3") Step3 = !Step3;
        if (step == "Step4") Step4 = !Step4;

        updateStepGPIOs();
        g_rootHtmlDirty = true;
        manualControlTimeout = millis();
        webSocket.sendTXT(num, "{\"status\":\"" + step + " commutato\"}");

    // 🔹 Setare Step1 (PWM) în modul manual
    } else if (cmd == "setStep1") {
        if (!manualControlEnabled) {
            webSocket.sendTXT(num, "{\"status\":\"È necessario attivare la modalità manuale\"}");
            return;
        }
        
        int newValue = doc["value"].as<int>();
        if (newValue < 0 || newValue > 4095) {
            webSocket.sendTXT(num, "{\"status\":\"Il valore deve essere 0-4095\"}");
            return;
        }
        
        Step1 = newValue;
        pwm1 = Step1;
        ledcWrite(0, pwm1);

        g_rootHtmlDirty = true;
        manualControlTimeout = millis();
        webSocket.sendTXT(num, "{\"status\":\"Step1 aggiornato a " + String(Step1) + "\"}");

    // 🔹 Calibrare generală + offset-uri, salvată în NVRAM
        } else if (cmd == "calibrate") {
            // ✅ Calibrare generală
            if (doc.containsKey("setpoint"))       setpoint       = doc["setpoint"].as<float>();
            if (doc.containsKey("currentFactor"))  currentFactor  = doc["currentFactor"].as<float>();
            if (doc.containsKey("voltageFactor"))  voltageFactor  = doc["voltageFactor"].as<float>();
            if (doc.containsKey("CT_Primary"))   CT_Primary   = doc["CT_Primary"].as<float>();
            if (doc.containsKey("CT_Secondary")) CT_Secondary = doc["CT_Secondary"].as<float>();

            // --- Praguri prez cu validare (nu acceptăm valori <= 0) ---
            if (doc.containsKey("prez1")) {
                int tmp = doc["prez1"].as<int>();
                if (tmp >= 0) prez1 = tmp;
            }
            if (doc.containsKey("prez2")) {
                int tmp = doc["prez2"].as<int>();
                if (tmp >= 0) prez2 = tmp;
            }
            if (doc.containsKey("prez3")) {
                int tmp = doc["prez3"].as<int>();
                if (tmp >= 0) prez3 = tmp;
            }
            if (doc.containsKey("prez4")) {
                int tmp = doc["prez4"].as<int>();
                if (tmp >= 0) prez4 = tmp;
            }

            DBG_PRINTf("🌐 Calibrazione ricevuta: prez1=%d prez2=%d prez3=%d prez4=%d\n",
                       prez1, prez2, prez3, prez4);

            // ✅ OFFSET-URI (NOUĂ FUNCȚIONALITATE)
float CT_Ratio = 1.0f;
if (CT_Secondary > 0.0f)
    CT_Ratio = CT_Primary / CT_Secondary;

// K = conversie counts -> A sau V
float KI_A = IRMS_SCALE_A * CT_Ratio * currentFactor;
float KI_B = IRMS_SCALE_B * CT_Ratio * currentFactor;
float KI_C = IRMS_SCALE_C * CT_Ratio * currentFactor;

float KV_A = VRMS_SCALE_A * voltageFactor;
float KV_B = VRMS_SCALE_B * voltageFactor;
float KV_C = VRMS_SCALE_C * voltageFactor;

// OFFSETURILE vin din UI în A/V → le convertim în COUNTS înainte de salvare

if (doc.containsKey("currentOffsetA")) {
    float offA_A = doc["currentOffsetA"].as<float>();
    if (KI_A > 0.0f) currentOffsetA = offA_A / KI_A;
}

if (doc.containsKey("currentOffsetB")) {
    float offB_A = doc["currentOffsetB"].as<float>();
    if (KI_B > 0.0f) currentOffsetB = offB_A / KI_B;
}

if (doc.containsKey("currentOffsetC")) {
    float offC_A = doc["currentOffsetC"].as<float>();
    if (KI_C > 0.0f) currentOffsetC = offC_A / KI_C;
}

if (doc.containsKey("voltageOffsetA")) {
    float offA_V = doc["voltageOffsetA"].as<float>();
    if (KV_A > 0.0f) voltageOffsetA = offA_V / KV_A;
}

if (doc.containsKey("voltageOffsetB")) {
    float offB_V = doc["voltageOffsetB"].as<float>();
    if (KV_B > 0.0f) voltageOffsetB = offB_V / KV_B;
}

if (doc.containsKey("voltageOffsetC")) {
    float offC_V = doc["voltageOffsetC"].as<float>();
    if (KV_C > 0.0f) voltageOffsetC = offC_V / KV_C;
}


            // 🔎 Debug înainte de scriere
            DBG_PRINTf("💾 Prima di saveSettingsToNVRAM: prez1=%d prez2=%d prez3=%d prez4=%d\n",
                       prez1, prez2, prez3, prez4);

            saveSettingsToNVRAM();  // ⬅️ salvează toate valorile în NVRAM
            g_rootHtmlDirty = true;

            webSocket.sendTXT(num, "{\"status\":\"Calibrazione salvata con offset\",\"target\":\"calibration\"}");

} else if (cmd == "autozero") {
    const int N = 20;
    uint32_t accIA=0, accIB=0, accIC=0;
    uint32_t accVA=0, accVB=0, accVC=0;

    for (int i=0; i<N; i++) {
        accIA += meter.read24bits(AIRMS);
        accIB += meter.read24bits(BIRMS);
        accIC += meter.read24bits(CIRMS);

        accVA += meter.read24bits(AVRMS);
        accVB += meter.read24bits(BVRMS);
        accVC += meter.read24bits(CVRMS);

        delay(5);
    }

    // ✅ offseturi în COUNTS (compatibil cu loop-ul tău)
    currentOffsetA = (float)(accIA / N);
    currentOffsetB = (float)(accIB / N);
    currentOffsetC = (float)(accIC / N);

    voltageOffsetA = (float)(accVA / N);
    voltageOffsetB = (float)(accVB / N);
    voltageOffsetC = (float)(accVC / N);

    saveSettingsToNVRAM();
    g_rootHtmlDirty = true;

    DBG_PRINTf("✅ AutoZero saved COUNTS: Ioff A/B/C=%.0f/%.0f/%.0f  Voff A/B/C=%.0f/%.0f/%.0f\n",
               currentOffsetA, currentOffsetB, currentOffsetC,
               voltageOffsetA, voltageOffsetB, voltageOffsetC);

    webSocket.sendTXT(num, "{\"status\":\"AutoZero OK (COUNTS)\",\"target\":\"calibration\"}");

    // 🔹 Setare energie (kWh) manual + NVRAM

    } else if (cmd == "setEnergy") {
        if (doc.containsKey("energy")) {
            KWh = doc["energy"].as<float>();
            saveSettingsToNVRAM();
          g_rootHtmlDirty = true;
            webLog("Energy set to " + String(KWh, 3) + " kWh");
            webSocket.sendTXT(num, "{\"status\":\"Energia impostata\",\"target\":\"energy\"}");
        }

    // 🔹 Activare / dezactivare modul manual
    } else if (cmd == "setManualMode") {
        manualControlEnabled = doc["enabled"].as<bool>();
        manualControlTimeout = millis();
      webLog(String("Manual mode ") + (manualControlEnabled ? "ENABLED" : "DISABLED"));
        
        if (!manualControlEnabled) {
            Step1 = 0;
            Step2 = false;
            Step3 = false;
            Step4 = false;
            pwm1 = 0;
            ledcWrite(0, 0);
            updateStepGPIOs();
        }

          g_rootHtmlDirty = true;
        String s = manualControlEnabled ? "Modalità manuale ATTIVA" : "Modalità manuale DISATTIVATA";
        webSocket.sendTXT(num, "{\"status\":\"" + s + "\",\"target\":\"manual\"}");

    // 🔹 Toggle alimentare stepuri (mosfet P)
    } else if (cmd == "toggleStepsEnable") {
        stepsEnabled = !stepsEnabled;
        updateStepGPIOs();  // Aplică imediat schimbarea
        saveSettingsToNVRAM();  // Salvează în NVRAM pentru persistență
        g_rootHtmlDirty = true;
      webLog(String("Steps power ") + (stepsEnabled ? "ENABLED" : "DISABLED"));
        
        String s = stepsEnabled ? "Alimentare stepuri ATTIVATA" : "Alimentare stepuri DISATTIVATA";
        webSocket.sendTXT(num, "{\"status\":\"" + s + "\",\"target\":\"stepsEnable\"}");

    // 🔹 Setare IP Modbus + salvare în NVRAM
    } else if (cmd == "setModbusIP") {
        if (doc.containsKey("ip")) {
            String ipStr = doc["ip"].as<String>();
            if (MODBUS_SLAVE_IP.fromString(ipStr)) {
                saveModbusIPToNVRAM();  // ✅ scrie în EEPROM
              webLog("Modbus IP updated to " + ipStr);

                if (client.connected()) {
                    client.stop();
                }
                modbusConnected = false;
                g_rootHtmlDirty = true;

                webSocket.sendTXT(num, "{\"status\":\"IP Modbus salvato, riconnessione.\",\"target\":\"modbus\"}");
            } else {
                webSocket.sendTXT(num, "{\"status\":\"IP Modbus non valido\",\"target\":\"modbus\"}");
            }
        }
    }
}


void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
      wsClientCount++;
      websocketConnected = (wsClientCount > 0);
            webLog("WS client connected: " + String(num));
            Serial.printf("[WebSocket] Client %u connesso\n", num);
            break;
        case WStype_DISCONNECTED:
      if (wsClientCount > 0) wsClientCount--;
      websocketConnected = (wsClientCount > 0);
            webLog("WS client disconnected: " + String(num));
            Serial.printf("[WebSocket] Client %u disconnesso\n", num);
            break;
        case WStype_TEXT:
            handleWebSocketMessage(num, payload, length);
            break;
        default:
            break;
    }
}

void handleWebClient() {
    server.handleClient();
    webSocket.loop();

    static unsigned long lastSend = 0;
    if (millis() - lastSend >= 200) {
      if (websocketConnected) sendLiveData();
        lastSend = millis();
    }

    // ✅ FIX TIMEOUT - RESET ESPLICITO Step1 quando scade timeout
    if (manualControlEnabled && millis() - manualControlTimeout > MANUAL_TIMEOUT_MS) {
        manualControlEnabled = false;
        
        // ✅ RESET ESPLICITO TUTTI I STEP
        Step1 = 0;
        Step2 = false;
        Step3 = false;
        Step4 = false;
        pwm1 = 0;
        ledcWrite(0, 0);  // Reset hardware PWM
        updateStepGPIOs();  // Reset GPIO fisici
        g_rootHtmlDirty = true;
        
        Serial.println("⏱️ Timeout modo manuale - tutti i step resettati");
        
        StaticJsonDocument<64> msg;
        msg["status"] = "Modalità manuale disattivata automaticamente";
        msg["manual"] = false;
        String out;
        serializeJson(msg, out);
        webSocket.broadcastTXT(out);
    }
}

void loopWebSocket() {
  webSocket.loop();  // asigură procesarea conexiunilor WebSocket
}

void initWebServerAndSocket() {
  SerialLogger.begin(8192);
  webLog("Web server init");

    const char* hdrKeys[] = { "X-OTA-CONFIRM" };
    server.collectHeaders(hdrKeys, 1);

    server.on("/", HTTP_GET, handleRoot);
    server.onNotFound(handleNotFound);
    server.on("/ota_status", HTTP_GET, handleOTAStatus);
  server.on("/logs", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/plain; charset=utf-8", SerialLogger.get());
  });
  server.on("/logs/clear", HTTP_POST, []() {
    SerialLogger.clear();
    webLog("Logs cleared from WebUI");
    server.send(200, "text/plain", "OK");
  });

    server.on("/update", HTTP_POST,
        []() { if (!otaAuthOrFail()) return; },
        handleOTAUpload
    );

    server.on("/restart", HTTP_POST, []() {
        Serial.println("⚠️ Restart ESP32 solicitat din WebUI...");
        server.send(200, "text/plain", "OK - Restarting...");
        delay(500);
        Serial.println("🔄 Restarting NOW!");
        Serial.flush();
        ESP.restart();
    });

    // ── Toggle sursă Power Factor ──────────────────────────────
    server.on("/toggle_pf_source", HTTP_GET, []() {
        pfUseRete = !pfUseRete;
      g_rootHtmlDirty = true;
        server.send(200, "text/plain", pfUseRete ? "rete" : "ade");
        Serial.printf("🔄 PF source: %s\n", pfUseRete ? "Rete (Modbus)" : "ADE7758");
    });

    server.begin();
    Serial.println("HTTP server avviato (ETH)");

    static bool socketStarted = false;
    if (!socketStarted) {
        webSocket.begin();
        webSocket.onEvent(onWebSocketEvent);
        socketStarted = true;
        Serial.println("Server WebSocket avviato");
    }
}
// ═══════════════════════════════════════════════════════════
// 🔒 Verificare semnătură firmware OTA
// ═══════════════════════════════════════════════════════════
bool verifyFirmwareSignature(const uint8_t* firmware_data, size_t firmware_size,
                             const uint8_t* signature_data, size_t signature_size) {
  
  Serial.println("🔒 Verificare semnătură firmware...");
  Serial.printf("   Firmware: %u bytes\n", firmware_size);
  Serial.printf("   Semnătură: %u bytes\n", signature_size);
  
  // Verifică dimensiunea semnăturii (RSA 3072-bit = 384 bytes)
  if (signature_size != 384) {
    Serial.printf("❌ Dimensiune semnătură invalidă: %u (așteptat 384)\n", signature_size);
    return false;
  }
  
  // Calculează hash SHA-256 al firmware-ului
  unsigned char hash[32];
  mbedtls_sha256_ret(firmware_data, firmware_size, hash, 0);
  
  Serial.print("✅ Hash firmware: ");
  for (int i = 0; i < 16; i++) {
    Serial.printf("%02x", hash[i]);
  }
  Serial.println("...");
  
  // Parse cheie publică
  mbedtls_pk_context pk_ctx;
  mbedtls_pk_init(&pk_ctx);
  
  int ret = mbedtls_pk_parse_public_key(&pk_ctx, 
                                        (const unsigned char*)rsa_public_key_pem,
                                        strlen(rsa_public_key_pem) + 1);
  if (ret != 0) {
    Serial.printf("❌ Eroare parse cheie publică: -0x%04X\n", -ret);
    mbedtls_pk_free(&pk_ctx);
    return false;
  }
  
  Serial.println("✅ Cheie publică încărcată");
  
  // Verifică semnătura RSA cu PKCS#1 v1.5 padding
  ret = mbedtls_pk_verify(&pk_ctx, MBEDTLS_MD_SHA256,
                          hash, sizeof(hash),
                          signature_data, signature_size);
  
  mbedtls_pk_free(&pk_ctx);
  
  if (ret == 0) {
    Serial.println("✅✅✅ SEMNĂTURĂ VALIDĂ! ✅✅✅");
    return true;
  } else {
    Serial.printf("❌❌❌ SEMNĂTURĂ INVALIDĂ: -0x%04X ❌❌❌\n", -ret);
    return false;
  }
}


void startWebServer() {
  WiFi.softAP("EBS-Orbital-Config", "12345678");
delay(100);  // ✅ Lasă WiFi să pornească

  IPAddress local_IP(192, 168, 1, 177);
  IPAddress gateway(192, 168, 1, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(local_IP, gateway, subnet);
  delay(500);  // ✅ Lasă configurația să se aplice
  Serial.println("Access Point WiFi avviato a " + WiFi.softAPIP().toString());
  initWebServerAndSocket();
}