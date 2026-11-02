// Connects to an iDotMatrix display over BLE and controls it over HTTP on Wi-Fi

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ErriezCRC32.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Update.h>
#include "Commands.h"
#include "secrets.h"

static Commands commands;

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
const char *hostname = "pixel-display";
const char *displayAddress = DISPLAY_ADDRESS;

int scanTime = 3; // In seconds
static BLERemoteCharacteristic *writeCharacteristic;

WebServer server(80);

static BLEUUID writeServiceUUID("000000fa-0000-1000-8000-00805f9b34fb");
static BLEUUID readServiceUUID("0000fa03-0000-1000-8000-00805f9b34fb");
static BLEUUID writeCharUUID("0000fa02-0000-1000-8000-00805f9b34fb");

// Images are kept in LittleFS as /img<slot>.png, the oldest slot gets replaced first
const int max_files = 10;
// Largest size seen working, a 4196 byte PNG sent as two chunks isn't shown
const size_t max_png_size = 3663;
int next_upload_slot = 0;
int current_display_file = 0;

enum DisplayMode
{
  MODE_IDLE = 0,
  MODE_SLIDESHOW = 1,
  MODE_CLOCK = 2
};
static volatile DisplayMode display_mode = MODE_SLIDESHOW;
// 0 until it's set for the first time, the display's own value isn't known
static volatile int brightness_pct = 0;
static volatile bool screen_on = true;

static volatile unsigned long last_display_change = 0;

// Set from the BLE task, read from the main loop
static volatile bool paired = false;
static volatile bool restore_display = false;
// Entering draw mode blanks the screen, so only do it when the display isn't in it yet
static volatile bool draw_mode_active = false;
static volatile bool show_upload = false;
// BLE traffic slows down Wi-Fi a lot, so the slideshow waits during a firmware update
static volatile bool ota_active = false;
static SemaphoreHandle_t bleMutex;
static BLEClient *pClient = nullptr;

class DisplayClientCallbacks : public BLEClientCallbacks
{
  void onConnect(BLEClient *client) {}
  void onDisconnect(BLEClient *client)
  {
    paired = false;
    Serial.println("Display disconnected");
  }
};

bool sendCommand(std::vector<uint8_t> data)
{
  xSemaphoreTake(bleMutex, portMAX_DELAY);
  bool sent = true;
  unsigned long send_start = millis();
  // a write without response has to fit in one packet
  size_t mtu_size = pClient != nullptr ? std::min<size_t>(509, pClient->getMTU() - 3) : 20;
  for (size_t i = 0; i < data.size(); i += mtu_size)
  {
    if (!paired || writeCharacteristic == nullptr)
    {
      sent = false;
      break;
    }

    // create chunk
    std::vector<uint8_t> chunk(data.begin() + i, data.begin() + std::min(i + mtu_size, data.size()));

    // Without response: the write confirmation sometimes never arrives, and waiting for it
    // blocked the web server for minutes. The display confirms images with a notification.
    // The pause keeps it from dropping packets, 10 ms or less wasn't reliable in testing.
    writeCharacteristic->writeValue(chunk.data(), chunk.size(), false);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  xSemaphoreGive(bleMutex);

  Serial.printf("Sent %d bytes in %lu ms: %s\n", data.size(), millis() - send_start, sent ? "ok" : "not connected");
  return sent;
}

static void notifyCallback(
    BLERemoteCharacteristic *pBLERemoteCharacteristic,
    uint8_t *pData,
    size_t length,
    bool isNotify)
{
  Serial.printf("Notify at %lu ms:", millis());
  for (size_t i = 0; i < length; i++)
  {
    Serial.printf(" %02X", pData[i]);
  }
  Serial.println();
}

bool connectToserver(BLEAddress pAddress)
{
  if (pClient == nullptr)
  {
    pClient = BLEDevice::createClient();
    pClient->setClientCallbacks(new DisplayClientCallbacks());
    Serial.println(" - Created client");
  }

  // Connect to the BLE Server.
  if (!pClient->connect(pAddress))
  {
    Serial.println("Connect failed");
    return false;
  }
  // the default of 23 bytes makes a 3 KB image take almost 6 seconds
  pClient->setMTU(517);
  Serial.printf(" - MTU: %d\n", pClient->getMTU());
  Serial.print(" - Connected to BT device: ");
  Serial.println(pClient->getPeerAddress().toString().c_str());

  Serial.print("rssi = ");
  Serial.println(pClient->getRssi());

  if (!pClient->isConnected())
  {
    Serial.println("Not connected");
    return false;
  }

  Serial.println(" - Getting services");
  std::map<std::string, BLERemoteService *> *foundServices = pClient->getServices();
  for (auto myPair = foundServices->begin(); myPair != foundServices->end(); myPair++)
  {
    /*std::string str = myPair->second->canRead()?"R":"";
      str += myPair->second->canWrite()?"\\W":"";
      str += myPair->second->canBroadcast()?"\\B":"";
      str += myPair->second->canIndicate()?"\\I":"";
      str += myPair->second->canNotify()?"\\N]":"]";*/
    // Serial.println(myPair->first.c_str());
    Serial.println(myPair->second->toString().c_str());
  }

  BLERemoteService *writeService = pClient->getService(writeServiceUUID);
  if (writeService != nullptr)
  {
    std::map<std::string, BLERemoteCharacteristic *> *foundServices = writeService->getCharacteristics();
    for (auto myPair = foundServices->begin(); myPair != foundServices->end(); myPair++)
    {
      Serial.println(myPair->second->toString().c_str());
    }
    writeCharacteristic = writeService->getCharacteristic(writeCharUUID);
    if (writeCharacteristic == nullptr)
    {
      Serial.println("Failed to find writeCharacteristic");
      pClient->disconnect();
      return false;
    }
    Serial.println(" - Found writeCharacteristic");
    if (writeCharacteristic->canWrite())
    {
      Serial.println(" -- Writeable");
    }
    BLERemoteCharacteristic *notifyCharacteristic = writeService->getCharacteristic(readServiceUUID);
    if (notifyCharacteristic != nullptr && notifyCharacteristic->canNotify())
    {
      notifyCharacteristic->registerForNotify(notifyCallback);
    }
    return true;
  }
  else
  {
    Serial.println("Failed to find writeService");
    pClient->disconnect();
    return false;
  }

  // set max brightness
  // sendCommand(commands.setBrightness(100)); <- not working

  // Obtain a reference to the characteristic in the service of the remote BLE server.
  /*pCommandCharacteristic = pCommandService->getCharacteristic(charUUID);
    if (pCommandCharacteristic != nullptr)
      Serial.println(" - Found our characteristic");
      return true;*/
}

// function to split data into chunks
std::vector<std::vector<uint8_t>> splitIntoChunks(const std::vector<uint8_t> &data, size_t chunkSize)
{
  std::vector<std::vector<uint8_t>> chunks;
  for (size_t i = 0; i < data.size(); i += chunkSize)
  {
    chunks.push_back(std::vector<uint8_t>(data.begin() + i, data.begin() + std::min(data.size(), i + chunkSize)));
  }
  return chunks;
}

// Function to create payloads from GIF data, not used yet
std::vector<std::vector<uint8_t>>
createPayloads(const std::vector<uint8_t> &gif_data)
{ // Calculate CRC32
  // CRC32 crc;
  // crc.update(gif_data.data(), gif_data.size());
  // uint32_t crcValue = crc.finalize();
  uint32_t crcValue = crc32Buffer(gif_data.data(), gif_data.size());

  Serial.print("size data: ");
  Serial.println(gif_data.size());

  Serial.print("crcValue: ");
  Serial.println(crcValue);

  // Create header
  std::vector<uint8_t>
      header = {
          255, 255, 1, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 5, 0, 13};

  // Set length and CRC in header
  uint32_t totalLength = gif_data.size() + header.size();
  Serial.print("data + header: ");
  Serial.println(totalLength);
  printf("(%02X) \n", totalLength);

  memcpy(&header[5], &totalLength, 4);
  memcpy(&header[9], &crcValue, 4);

  // Split GIF data into chunks
  std::vector<std::vector<uint8_t>> gif_chunks = splitIntoChunks(gif_data, 4096);

  // Create payloads
  std::vector<std::vector<uint8_t>> payloads;
  for (size_t i = 0; i < gif_chunks.size(); ++i)
  {
    header[4] = (i > 0) ? 2 : 0;
    uint16_t chunk_len = gif_chunks[i].size() + header.size();
    memcpy(&header[0], &chunk_len, 2);
    Serial.print("this chunck size: ");
    Serial.println(chunk_len);
    printf("(%02X) \n", chunk_len);
    std::vector<uint8_t> payload = header;
    payload.insert(payload.end(), gif_chunks[i].begin(), gif_chunks[i].end());
    payloads.push_back(payload);
  }

  return payloads;
}

std::vector<uint8_t>
hexStringToByteArray(const std::string &hex)
{
  std::vector<uint8_t> byteArray;
  for (size_t i = 0; i < hex.length(); i += 2)
  {
    std::string byteString = hex.substr(i, 2);
    uint8_t byte = static_cast<uint8_t>(strtol(byteString.c_str(), nullptr, 16));
    byteArray.push_back(byte);
  }
  return byteArray;
}

// Function to create payloads from PNG data
std::vector<uint8_t> createPNGPayloads(const std::vector<uint8_t> &png_data)
{
  // Split PNG data into chunks
  std::vector<std::vector<uint8_t>> png_chunks = splitIntoChunks(png_data, 4096);
  Serial.print("png_chunks: ");
  Serial.println(png_chunks.size());

  // Calculate idk and convert to bytes
  int16_t idk = png_data.size() + png_chunks.size();
  uint8_t idk_bytes[2];
  memcpy(idk_bytes, &idk, sizeof(idk));
  Serial.print("idk: ");
  Serial.println(idk);

  // Convert PNG length to bytes
  int32_t png_len = png_data.size();
  uint8_t png_len_bytes[4];
  memcpy(png_len_bytes, &png_len, sizeof(png_len));
  Serial.print("png_len: ");
  Serial.println(png_len);

  // Create payloads
  std::vector<uint8_t> payloads;
  for (size_t i = 0; i < png_chunks.size(); ++i)
  {
    std::vector<uint8_t> payload;
    payload.insert(payload.end(), idk_bytes, idk_bytes + sizeof(idk_bytes));
    payload.push_back(0);
    payload.push_back(0);
    payload.push_back(i > 0 ? 2 : 0);
    payload.insert(payload.end(), png_len_bytes, png_len_bytes + sizeof(png_len_bytes));
    payload.insert(payload.end(), png_chunks[i].begin(), png_chunks[i].end());
    payloads.insert(payloads.end(), payload.begin(), payload.end());
  }

  return payloads;
}

void handlePost()
{
  if (server.hasArg("plain") == false)
  {
    server.send(400, "text/plain", "Body not received");
    return;
  }

  String body = server.arg("plain");
  if (body.length() == 0 || body.length() % 2 != 0)
  {
    server.send(400, "text/plain", "Body must be a hex string");
    return;
  }

  std::vector<uint8_t> data = hexStringToByteArray(body.c_str());

  // a raw command can change the display's mode
  draw_mode_active = false;
  if (sendCommand(data))
  {
    server.send(200, "text/plain", "Data sent over Bluetooth");
  }
  else
  {
    server.send(503, "text/plain", "Display not connected");
  }
}

String imagePath(int slot)
{
  return "/img" + String(slot) + ".png";
}

int countImages()
{
  int count = 0;
  for (int slot = 0; slot < max_files; slot++)
  {
    if (LittleFS.exists(imagePath(slot)))
    {
      count++;
    }
  }
  return count;
}

void saveSettings()
{
  File file = LittleFS.open("/settings", "w");
  if (file)
  {
    file.write((uint8_t)display_mode);
    file.write((uint8_t)next_upload_slot);
    file.write((uint8_t)brightness_pct);
    file.write((uint8_t)(screen_on ? 1 : 0));
    file.close();
  }
}

void loadSettings()
{
  if (!LittleFS.exists("/settings"))
  {
    return;
  }

  File file = LittleFS.open("/settings", "r");
  if (file && file.size() >= 2)
  {
    display_mode = (DisplayMode)file.read();
    next_upload_slot = file.read() % max_files;
  }
  // older firmware didn't store these
  if (file && file.size() >= 4)
  {
    brightness_pct = file.read();
    screen_on = file.read() != 0;
  }
  if (file)
  {
    file.close();
  }
}

void setDisplayMode(DisplayMode mode)
{
  display_mode = mode;
  saveSettings();
}

std::vector<uint8_t> png_data;
int upload_status = 0;
String upload_message;

// The web server also calls upload handlers for other POST requests, and
// server.upload() then returns an invalid reference that crashes the board
bool isMultipartUpload()
{
  return server.header("Content-Type").startsWith("multipart/");
}

// Collects and checks an uploaded PNG, returns true once it's complete and valid
bool receivePng()
{
  if (!isMultipartUpload())
  {
    return false;
  }

  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    Serial.printf("Upload file start: %s\n", upload.filename.c_str());
    png_data.clear();
    upload_status = upload.filename.endsWith(".png") ? 0 : 400;
    upload_message = "File must be a PNG";
  }
  else if (upload.status == UPLOAD_FILE_WRITE && upload_status == 0)
  {
    if (png_data.size() + upload.currentSize > max_png_size)
    {
      png_data.clear();
      upload_status = 413;
      upload_message = "File too large";
      return false;
    }
    png_data.insert(png_data.end(), upload.buf, upload.buf + upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END && upload_status == 0)
  {
    Serial.printf("Upload file end: %d bytes\n", png_data.size());

    const uint8_t png_signature[] = {0x89, 'P', 'N', 'G'};
    if (png_data.size() < 4 || memcmp(png_data.data(), png_signature, 4) != 0)
    {
      upload_status = 400;
      upload_message = "File must be a PNG";
      return false;
    }

    upload_status = 200;
    return true;
  }
  return false;
}

// handle uploaded file, the response is sent by handlePngDone
void handlePngUpload()
{
  if (!receivePng())
  {
    return;
  }

  String path = imagePath(next_upload_slot);
  File file = LittleFS.open(path, "w");
  size_t written = file ? file.write(png_data.data(), png_data.size()) : 0;
  if (file)
  {
    file.close();
  }
  if (written != png_data.size())
  {
    LittleFS.remove(path);
    upload_status = 507;
    upload_message = "Not enough storage";
    return;
  }

  Serial.printf("Stored in slot %d\n", next_upload_slot);
  current_display_file = next_upload_slot;
  next_upload_slot = (next_upload_slot + 1) % max_files;
  saveSettings();
  show_upload = true;
  upload_message = "File stored";
}

void handlePngDone()
{
  server.send(upload_status == 0 ? 400 : upload_status, "text/plain", upload_status == 0 ? "No file received" : upload_message);
  upload_status = 0;
  png_data.clear();
  png_data.shrink_to_fit();
}

// Shows an uploaded image without storing it
void handleShowPngUpload()
{
  receivePng();
}

// Showing something on purpose turns the screen back on, so it's never shown to a dark screen
void wakeScreen()
{
  if (!screen_on && sendCommand(commands.on()))
  {
    screen_on = true;
    saveSettings();
  }
}

void handleShowPngDone()
{
  if (upload_status == 200)
  {
    wakeScreen();
    // the clock won't be on screen anymore
    if (display_mode == MODE_CLOCK)
    {
      setDisplayMode(MODE_IDLE);
    }
    if (!draw_mode_active)
    {
      draw_mode_active = sendCommand(commands.setDrawMode(1));
    }
    last_display_change = millis(); // a running slideshow gives it a full turn
    bool sent = sendCommand(createPNGPayloads(png_data));
    upload_status = sent ? 200 : 503;
    upload_message = sent ? "Image shown" : "Display not connected";
  }
  handlePngDone();
}

void handleStartClock()
{
  setDisplayMode(MODE_CLOCK);
  draw_mode_active = false;
  wakeScreen();
  if (sendCommand(commands.clock(3, true, true, 100, 100, 180)))
  {
    server.send(200, "text/plain", "Clock started");
  }
  else
  {
    server.send(202, "text/plain", "Display not connected, clock starts on reconnect");
  }
}

void handleStartSlideshow()
{
  setDisplayMode(MODE_SLIDESHOW);
  last_display_change = 0;
  wakeScreen();
  server.send(200, "text/plain", "Slideshow started");
}

void handleStopSlideshow()
{
  setDisplayMode(MODE_IDLE);
  server.send(200, "text/plain", "Slideshow stopped");
}

void handleClearImages()
{
  for (int slot = 0; slot < max_files; slot++)
  {
    LittleFS.remove(imagePath(slot));
  }
  next_upload_slot = 0;
  current_display_file = 0;
  saveSettings();
  server.send(200, "text/plain", "Images removed");
}

void handleBrightness()
{
  int value = server.arg("value").toInt();
  if (value < 5 || value > 100)
  {
    server.send(400, "text/plain", "Brightness must be 5 to 100");
    return;
  }
  if (!sendCommand(commands.setBrightness(value)))
  {
    server.send(503, "text/plain", "Display not connected");
    return;
  }
  brightness_pct = value;
  saveSettings();
  server.send(200, "text/plain", "Brightness set");
}

void handleScreen()
{
  String on = server.arg("on");
  if (on != "0" && on != "1")
  {
    server.send(400, "text/plain", "on must be 0 or 1");
    return;
  }
  bool turn_on = on == "1";
  if (!sendCommand(turn_on ? commands.on() : commands.off()))
  {
    server.send(503, "text/plain", "Display not connected");
    return;
  }
  screen_on = turn_on;
  saveSettings();
  server.send(200, "text/plain", turn_on ? "Screen on" : "Screen off");
}

void handleStatus()
{
  const char *modes[] = {"idle", "slideshow", "clock"};
  String json = "{\"display_connected\":";
  json += paired ? "true" : "false";
  json += ",\"mode\":\"";
  json += modes[display_mode];
  json += "\",\"brightness\":";
  json += brightness_pct > 0 ? String(brightness_pct) : "null";
  json += ",\"screen_on\":";
  json += screen_on ? "true" : "false";
  json += ",\"images\":";
  json += countImages();
  json += ",\"max_images\":";
  json += max_files;
  json += ",\"fs_used\":";
  json += LittleFS.usedBytes();
  json += ",\"fs_total\":";
  json += LittleFS.totalBytes();
  json += ",\"free_heap\":";
  json += ESP.getFreeHeap();
  json += ",\"wifi_rssi\":";
  json += WiFi.RSSI();
  json += ",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"uptime_s\":";
  json += millis() / 1000;
  json += "}";
  server.send(200, "application/json", json);
}

bool ota_authorized = false;

void handleUpdateUpload()
{
  if (!isMultipartUpload())
  {
    return;
  }

  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START)
  {
    ota_authorized = server.authenticate("admin", OTA_PASSWORD);
    if (ota_authorized)
    {
      Serial.printf("OTA start: %s\n", upload.filename.c_str());
      ota_active = true;
      Update.begin(UPDATE_SIZE_UNKNOWN);
    }
  }
  else if (!ota_authorized)
  {
    return;
  }
  else if (upload.status == UPLOAD_FILE_WRITE)
  {
    Update.write(upload.buf, upload.currentSize);
  }
  else if (upload.status == UPLOAD_FILE_END)
  {
    Update.end(true);
    ota_active = false;
    Serial.printf("OTA end: %d bytes, %s\n", upload.totalSize, Update.errorString());
  }
  else if (upload.status == UPLOAD_FILE_ABORTED)
  {
    ota_active = false;
    Update.abort();
  }
}

void handleUpdateDone()
{
  if (!ota_authorized)
  {
    return server.requestAuthentication();
  }

  ota_authorized = false;
  if (Update.hasError() || !Update.isFinished())
  {
    server.send(500, "text/plain", Update.errorString());
    return;
  }

  server.send(200, "text/plain", "Update done, restarting");
  delay(500);
  ESP.restart();
}

void setupWebServer()
{
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info)
               { Serial.printf("Wi-Fi disconnected, reason %d\n", info.wifi_sta_disconnected.reason); },
               ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.setHostname(hostname);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  // by default it joins the first access point found, not the strongest
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.begin(ssid, password); // Connect to the network
  Serial.print("Connecting to ");
  Serial.print(ssid);
  Serial.println(" ...");

  const char *collected_headers[] = {"Content-Type"};
  server.collectHeaders(collected_headers, 1);

  server.on("/send-raw", HTTP_POST, handlePost);
  server.on("/send-png", HTTP_POST, handlePngDone, handlePngUpload);
  server.on("/show-png", HTTP_POST, handleShowPngDone, handleShowPngUpload);
  server.on("/start-clock", HTTP_POST, handleStartClock);
  server.on("/slideshow/start", HTTP_POST, handleStartSlideshow);
  server.on("/slideshow/stop", HTTP_POST, handleStopSlideshow);
  server.on("/images/clear", HTTP_POST, handleClearImages);
  server.on("/brightness", HTTP_POST, handleBrightness);
  server.on("/screen", HTTP_POST, handleScreen);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.begin();
}

void showNextImage();

// Keeps the display connected and updated, separate from loop() so HTTP stays up in the meantime
void bleTask(void *parameter)
{
  BLEAddress address(displayAddress);
  BLEScan *scan = BLEDevice::getScan();
  scan->setActiveScan(false);
  scan->setInterval(100);
  scan->setWindow(50); // leave airtime for Wi-Fi
  int failed_connects = 0;

  // both share one radio, so give Wi-Fi the chance to connect first
  for (int i = 0; i < 300 && WiFi.status() != WL_CONNECTED; i++)
  {
    vTaskDelay(pdMS_TO_TICKS(100));
  }

  for (;;)
  {
    if (paired)
    {
      // put the display back in the right state after it was away
      if (restore_display)
      {
        restore_display = false;
        draw_mode_active = false;
        last_display_change = 0;
        if (brightness_pct > 0)
        {
          sendCommand(commands.setBrightness(brightness_pct));
        }
        if (display_mode == MODE_CLOCK)
        {
          sendCommand(commands.clock(3, true, true, 100, 100, 180));
        }
        // last, in case showing the clock turns the screen on
        if (!screen_on)
        {
          sendCommand(commands.off());
        }
      }

      if (ota_active)
      {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      // show a new upload right away, the clock keeps running and only stores it.
      // An image turns the screen back on, so nothing is shown while it's off
      if (show_upload)
      {
        show_upload = false;
        if (display_mode != MODE_CLOCK && screen_on)
        {
          last_display_change = millis();
          showNextImage();
        }
      }
      // change the display every 15 seconds
      else if (display_mode == MODE_SLIDESHOW && screen_on && (last_display_change == 0 || millis() - last_display_change > 15000))
      {
        last_display_change = millis();
        showNextImage();
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // connecting replaces the remote services, so drop our reference first
    xSemaphoreTake(bleMutex, portMAX_DELAY);
    writeCharacteristic = nullptr;
    xSemaphoreGive(bleMutex);

    // only try to connect when the display is advertising
    bool found = false;
    BLEScanResults results = scan->start(scanTime, false);
    for (int i = 0; i < results.getCount(); i++)
    {
      if (results.getDevice(i).getAddress().equals(address))
      {
        found = true;
      }
    }
    scan->clearResults();

    if (!found)
    {
      Serial.println("Display not found");
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }

    Serial.println("Connecting to Server as client");
    if (connectToserver(address))
    {
      failed_connects = 0;
      restore_display = true;
      paired = true;
      Serial.println("Paired");
    }
    else
    {
      Serial.println("Pairing failed");
      // the BLE stack only has a few client registrations, a restart gets them back
      if (++failed_connects >= 3)
      {
        ESP.restart();
      }
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

void showNextImage()
{
  for (int i = 0; i < max_files; i++)
  {
    int slot = (current_display_file + i) % max_files;
    if (!LittleFS.exists(imagePath(slot)))
    {
      continue;
    }

    File file = LittleFS.open(imagePath(slot), "r");
    if (!file)
    {
      continue;
    }

    std::vector<uint8_t> image(file.size());
    file.read(image.data(), image.size());
    file.close();

    Serial.print("current_display_file: ");
    Serial.println(slot);
    if (!draw_mode_active)
    {
      draw_mode_active = sendCommand(commands.setDrawMode(1));
    }
    sendCommand(createPNGPayloads(image));
    current_display_file = (slot + 1) % max_files;
    return;
  }
}

void setup()
{
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxTimeoutMs(0); // don't slow down logging when no USB host is attached
#endif
  delay(1000); // give me time to bring up serial monitor
  Serial.println("ESP32 pixel display");

  if (!LittleFS.begin(true))
  {
    Serial.println("LittleFS mount failed");
  }
  loadSettings();

  bleMutex = xSemaphoreCreateMutex();
  BLEDevice::init("");

  setupWebServer();

  xTaskCreate(bleTask, "ble", 8192, nullptr, 1, nullptr);
}

bool mdns_started = false;
bool wifi_connected = false;

void loop()
{
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED && !wifi_connected)
  {
    wifi_connected = true;
    Serial.print("IP address:\t");
    Serial.println(WiFi.localIP());
    if (!mdns_started)
    {
      mdns_started = MDNS.begin(hostname);
      MDNS.addService("http", "tcp", 80);
    }
  }
  else if (WiFi.status() != WL_CONNECTED)
  {
    wifi_connected = false;
  }

  delay(2);
}
