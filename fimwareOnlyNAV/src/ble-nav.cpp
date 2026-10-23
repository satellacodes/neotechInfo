/*
 * ble_nav.cpp — BLE NimBLE + parsing perintah JSON.
 * Perintah yang didukung: nav, speed, ping. (mode/idle/contrast/reset_time dibuang)
 * Callback BLE hanya enqueue ke cmdQueue; diproses di loop() (bebas race).
 */
#include "app.h"

// ============ BLE CALLBACKS ============
class SrvCb : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    // 100–200 ms (unit 1.25 ms): hemat daya vs 30–60 ms lama; latensi cukup utk nav
    s->updateConnParams(info.getConnHandle(), 80, 160, 0, 400);
    displayDirty = true;
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    displayDirty = true;
  }
};

class RxCb : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.length();
    if (n == 0 || n >= MSG_MAX) return;
    Msg m;
    memcpy(m.data, v.data(), n);
    m.data[n] = 0;
    m.len = n;
    if (xQueueSend(cmdQueue, &m, 0) != pdTRUE) {
      Msg tmp; xQueueReceive(cmdQueue, &tmp, 0);  // buang terlama
      xQueueSend(cmdQueue, &m, 0);
    }
  }
};

// ============ COMMAND HANDLER (dari loop) ============
static void handleCommand(const char* js) {
  JsonDocument doc;
  if (deserializeJson(doc, js) != DeserializationError::Ok) return;
  const char* cmd = doc["cmd"] | "";

  if (!strcmp(cmd, "nav")) {
    copyAscii(nav.arrow,    sizeof(nav.arrow),    doc["arrow"]    | "");
    copyAscii(nav.distance, sizeof(nav.distance), doc["distance"] | "");
    copyAscii(nav.street,   sizeof(nav.street),   doc["street"]   | "");
    copyAscii(nav.eta,      sizeof(nav.eta),      doc["eta"]      | "");
    nav.autoSrc    = (int)(doc["auto"] | 0) == 1;
    nav.lastUpdate = millis();
    navStale       = false;
    displayDirty   = true;
  }
  else if (!strcmp(cmd, "speed")) {
    int k = (int)(doc["kmh"] | -1);
    if (k < 0) {
      speedActive = false;
      speedStale  = false;
    } else {
      speedKmh    = constrain(k, 0, 299);
      speedAt     = millis();
      speedActive = true;
      speedStale  = false;
    }
    displayDirty = true;
  }
  else if (!strcmp(cmd, "ping")) {
    if (pTxChar && bleConnected) {
      pTxChar->setValue((const uint8_t*)"pong", 4);
      pTxChar->notify();
    }
  }
}

void processQueue() {
  Msg m;
  int guard = 4;  // batasi per putaran loop
  while (guard-- > 0 && xQueueReceive(cmdQueue, &m, 0) == pdTRUE) {
    handleCommand(m.data);
  }
}

// ============ SETUP BLE ============
void setupBLE() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);   // hemat vs P9; cukup utk 0.5 m

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new SrvCb());
  pServer->advertiseOnDisconnect(true);

  NimBLEService* svc = pServer->createService(SVC_UUID);

  NimBLECharacteristic* pRx = svc->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  pRx->setCallbacks(new RxCb());

  pTxChar = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);

  svc->start();

  // Advertising: nama di scan response, UUID di paket utama
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  advData.addServiceUUID(SVC_UUID);
  NimBLEAdvertisementData scanData;
  scanData.setName(DEVICE_NAME);
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(scanData);
  adv->start();
}
