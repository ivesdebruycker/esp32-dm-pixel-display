/*
  ESP 32 - Sphero R2D2
*/

// https://github.com/nkolban/esp32-snippets/issues/757
// https://gist.github.com/sam016/4abe921b5a9ee27f67b3686910293026
// neilkolban.com/esp32/docs/cpp_utils/html/class_b_l_e_remote_characteristic.html#aa00bffdf3b2843aee32a27af9b643da5

// https://github.com/nkolban/ESP32_BLE_Arduino/blob/master/examples/BLE_client/BLE_client.ino
// https://github.com/igbopie/spherov2.js/blob/9a9fee843a9da6b26c14c60a93870ba242f03566/lib/src/toys/core.ts
// https://community.sphero.com/t/sphero-mini-not-responding-to-wake-command/1401
// https://medium.com/swlh/reverse-engineering-sphero-r2d2-with-javascript-8d1133269623
// https://cryptii.com/pipes/integer-encoder
// https://qiita.com/poruruba/items/634671131619f8067734
// https://github.com/marcos69/esp32bleSpheroMini
// https://lang-ship.com/reference/ESP32/latest/_b_l_e_remote_descriptor_8cpp_source.html
// http://www.neilkolban.com/esp32/docs/cpp_utils/html/class_b_l_e_remote_characteristic.html

// Advertised Device: Name: D2-52D5, Address: c2:80:9f:74:52:d5, serviceUUID: 00010001-574f-4f20-5370-6865726f2121

// Service: uuid: 00001800-0000-1000-8000-00805f9b34fb, start_handle: 1 0x0001, end_handle: 7 0x0007, Generic Access
// Service: uuid: 00001801-0000-1000-8000-00805f9b34fb, start_handle: 8 0x0008, end_handle: 11 0x000b, Generic Attribute
// Service: uuid: 0000180f-0000-1000-8000-00805f9b34fb, start_handle: 22 0x0016, end_handle: 25 0x0019, Battery Service
// Service: uuid: 00010001-574f-4f20-5370-6865726f2121, start_handle: 26 0x001a, end_handle: 65535 0xffff
// Service: uuid: 00020001-574f-4f20-5370-6865726f2121, start_handle: 12 0x000c, end_handle: 21 0x0015

// Dependencies
// |-- <WiFi> 1.0
// |-- <ESP32 BLE Arduino> 1.0.1
// |-- <WebServer> 1.0

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
const char *displayAddress = "9F:C9:66:73:FF:7C";

int scanTime = 3; // In seconds
BLEScan *pBLEScan;
static BLEUUID serviceUUID("00010001-574f-4f20-5370-6865726f2121");
static BLEAddress *Server_BLE_Address;
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

int seq = 0;
std::string generateCommand(char DID, char CID, char SEQ, char DLEN, ...)
{
  short i = 0;
  char sum = 0, data;
  va_list args;

  // Calculate checksum
  sum = DID + CID + SEQ + DLEN;

  int cnter = 6;
  char yyy[cnter + DLEN];
  yyy[0] = char(0xFF);
  yyy[1] = char(0xFE);
  yyy[2] = char(DID);
  yyy[3] = char(CID);
  yyy[4] = char(SEQ);
  yyy[5] = char(DLEN);

  va_start(args, DLEN);
  for (; i < DLEN - 1; i++)
  {
    data = va_arg(args, int);
    // Serial1.write(data);
    yyy[cnter] = data;
    cnter += 1;
    sum += data;
  }
  va_end(args);

  yyy[cnter] = char(~sum);

  return std::string(yyy);

  // Wait for Simple Response
  // return readSimplePacket();
}

bool sendCommand(std::vector<uint8_t> data)
{
  xSemaphoreTake(bleMutex, portMAX_DELAY);
  bool sent = true;
  unsigned long send_start = millis();
  int mtu_size = 509;
  for (size_t i = 0; i < data.size(); i += mtu_size)
  {
    if (!paired || writeCharacteristic == nullptr)
    {
      sent = false;
      break;
    }

    // create chunk
    std::vector<uint8_t> chunk(data.begin() + i, data.begin() + std::min(i + mtu_size, data.size()));

    // write to GATT characteristic
    writeCharacteristic->writeValue(chunk.data(), chunk.size(), true);
  }
  xSemaphoreGive(bleMutex);

  Serial.printf("Sent %d bytes in %lu ms: %s\n", data.size(), millis() - send_start, sent ? "ok" : "not connected");
  return sent;
}

class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice advertisedDevice)
  {
    Serial.printf("Advertised Device: %s \n", advertisedDevice.toString().c_str());
    Serial.println(advertisedDevice.getAddress().toString().c_str());
    // Server_BLE_Address = new BLEAddress(advertisedDevice.getAddress());
    // Scaned_BLE_Address = Server_BLE_Address->toString().c_str();
  }
};

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

void printVector(const std::vector<uint8_t> &vec)
{
  for (size_t i = 0; i < vec.size(); ++i)
  {
    Serial.print(vec[i]);
    if (i < vec.size() - 1)
    {
      Serial.print(", ");
    }
  }
  Serial.println();
}

void printArray(const uint8_t *arr, size_t size)
{
  for (size_t i = 0; i < size; ++i)
  {
    Serial.print(arr[i]);
    if (i < size - 1)
    {
      Serial.print(", ");
    }
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

// create a matrix of 16x16 int values
int matrix[32][32] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

void printMatrix()
{
  // loop through the matrix and send the pixel values
  for (int i = 0; i < 32; i++)
  {
    for (int j = 0; j < 32; j++)
    {
      sendCommand(commands.setPixel(j, i, matrix[i][j] * 100, matrix[i][j] * 100, matrix[i][j] * 100));
      delay(2);
    }
  }
}

void printRandomMatrix()
{
  // loop through the matrix and send the pixel values
  for (int i = 0; i < 32; i++)
  {
    for (int j = 0; j < 32; j++)
    {
      // Serial.print(i);
      // Serial.print(", ");
      // Serial.println(j);

      // make random colors
      std::vector<uint8_t> a = commands.setPixel(j, i, random(255), random(255), random(255));

      sendCommand(a);
      delay(10);
    }
  }
}

void printCorners()
{
  sendCommand(commands.setPixel(0, 0, random(255), random(255), random(255)));
  delay(20);
  sendCommand(commands.setPixel(0, 31, random(255), random(255), random(255)));
  delay(20);
  sendCommand(commands.setPixel(31, 0, random(255), random(255), random(255)));
  delay(20);
  sendCommand(commands.setPixel(31, 31, random(255), random(255), random(255)));
  delay(20);
}

// // Function to split data into chunks
// std::vector<std::vector<uint8_t>> splitIntoChunks(const std::vector<uint8_t> &data, size_t chunkSize)
// {
//   std::vector<std::vector<uint8_t>> chunks;
//   for (size_t i = 0; i < data.size(); i += chunkSize)
//   {
//     chunks.push_back(std::vector<uint8_t>(data.begin() + i, data.begin() + std::min(data.size(), i + chunkSize)));
//   }
//   return chunks;
// }

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

std::vector<std::vector<uint8_t>> chunkBuffer(const std::vector<uint8_t> &data, size_t chunkSize)
{
  std::vector<std::vector<uint8_t>> chunks;
  size_t cursor = 0;
  size_t remaining = data.size();
  while (remaining > 0)
  {
    size_t wl = std::min(chunkSize, remaining);
    chunks.push_back(std::vector<uint8_t>(data.begin() + cursor, data.begin() + cursor + wl));
    cursor += wl;
    remaining -= wl;
  }
  return chunks;
}

// Function to send an image to the display
bool SendImage(const std::vector<uint8_t> &imageData)
{
  printf("image byte size: %d\n", imageData.size());
  // Split image data into chunks
  std::vector<std::vector<uint8_t>> chunks = chunkBuffer(imageData, 4096);

  // Create a buffer to store the payloads
  std::vector<uint8_t> cib;
  int idk = imageData.size() + chunks.size() * 9;

  for (size_t ci = 0; ci < chunks.size(); ++ci)
  {
    const std::vector<uint8_t> &ch = chunks[ci];

    // Write idk as uint16_t in little-endian
    uint16_t idk_le = idk;
    cib.insert(cib.end(), reinterpret_cast<uint8_t *>(&idk_le), reinterpret_cast<uint8_t *>(&idk_le) + sizeof(idk_le));

    // Write two uint8_t zeros
    cib.push_back(0);
    cib.push_back(0);

    // Write uint8_t 2 if ci > 0, else 0
    cib.push_back(ci > 0 ? 2 : 0);

    // Write length of imageData as int32_t in little-endian
    int32_t imageData_len_le = imageData.size() + chunks.size() * 9;
    cib.insert(cib.end(), reinterpret_cast<uint8_t *>(&imageData_len_le), reinterpret_cast<uint8_t *>(&imageData_len_le) + sizeof(imageData_len_le));

    // Write chunk data
    cib.insert(cib.end(), ch.begin(), ch.end());
  }

  // Simulate writing to the device (replace with actual device write function)
  // return d.Write(cib);
  // std::cout << "Payload size: " << cib.size() << std::endl;
  // sendCommand(commands.clock(3, true, true, 100, 100, 180));
  printf("Uploading...\n");
  printf("cib size: %d\n", cib.size());
  uint8_t *dataArr = cib.data();
  writeCharacteristic->writeValue(dataArr, cib.size(), false);

  printf("Upload done\n");
  // Print payloads for verification
  for (uint8_t byte : cib)
  {
    printf("%02X ", byte);
  }
  printf("\n");

  return true;
}

// create test GIF data
std::vector<uint8_t> gif_data = {
    71, 73, 70, 56, 57, 97, 16, 0, 16, 0, 240, 0, 0, 0, 0, 0, 0, 0, 0, 33, 249, 4, 1, 0, 0, 0, 0, 44, 0, 0, 0, 0, 16, 0, 16, 0, 0, 2, 2, 68, 1, 0, 59};

// 'favicon', 32x32px
std::vector<uint8_t> png_data1 = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x08, 0x06, 0x00, 0x00, 0x00, 0x73, 0x7A, 0x7A,
    0xF4, 0x00, 0x00, 0x00, 0x09, 0x70, 0x48, 0x59, 0x73, 0x00, 0x00, 0x0B, 0x13, 0x00, 0x00, 0x0B,
    0x13, 0x01, 0x00, 0x9A, 0x9C, 0x18, 0x00, 0x00, 0x01, 0x60, 0x49, 0x44, 0x41, 0x54, 0x58, 0xC3,
    0xDD, 0x57, 0x4B, 0x6A, 0xC3, 0x30, 0x10, 0x9D, 0x31, 0xB1, 0xBD, 0xF3, 0x4D, 0x42, 0x02, 0xA1,
    0xC2, 0xD9, 0x76, 0xE3, 0x5D, 0x16, 0x3D, 0x81, 0x4F, 0x18, 0x4A, 0x77, 0x3E, 0x40, 0x89, 0x31,
    0xC4, 0x24, 0xF4, 0x26, 0x5A, 0x5A, 0x8B, 0xC9, 0xC6, 0x09, 0x76, 0x6A, 0xFD, 0x15, 0x15, 0x3A,
    0x20, 0x30, 0x68, 0xA4, 0xF7, 0x66, 0x34, 0x3F, 0x03, 0xFC, 0xB1, 0xA0, 0xA5, 0x3E, 0x85, 0xBE,
    0xD3, 0x46, 0x99, 0x04, 0x67, 0x5A, 0xA5, 0xB4, 0xE8, 0xAC, 0xEE, 0x4D, 0x42, 0x82, 0xBB, 0x48,
    0xE2, 0x73, 0x78, 0xB4, 0x76, 0xF6, 0x3D, 0x12, 0xA5, 0x28, 0x04, 0x62, 0x06, 0xE1, 0xE2, 0x13,
    0x4C, 0x3D, 0xE0, 0x8A, 0x63, 0x4C, 0xA0, 0x6D, 0x32, 0xD8, 0xED, 0xB7, 0x33, 0x70, 0x71, 0x34,
    0x08, 0xCA, 0x8F, 0x2E, 0x08, 0x81, 0x5F, 0x24, 0xD2, 0xA2, 0x83, 0xBE, 0xCE, 0x61, 0x7D, 0xD8,
    0x3C, 0x83, 0x59, 0xE1, 0x78, 0xC7, 0xC0, 0xCF, 0xD7, 0xF5, 0xF1, 0x2D, 0x8E, 0x6C, 0xEA, 0x15,
    0x9C, 0xAC, 0x60, 0x42, 0x6D, 0x93, 0x91, 0xE0, 0x8C, 0x04, 0x67, 0x34, 0x46, 0xBB, 0x6C, 0xBD,
    0x4C, 0x66, 0x24, 0xDA, 0x26, 0x7B, 0x39, 0xA0, 0x2C, 0x2B, 0x82, 0x90, 0x08, 0x52, 0x07, 0x76,
    0xFB, 0x2D, 0xB4, 0x4D, 0x06, 0x2E, 0x24, 0xD0, 0xF1, 0x09, 0x66, 0x29, 0xE9, 0xD3, 0x0B, 0xD0,
    0xC1, 0xF5, 0x41, 0x1B, 0x12, 0x1A, 0xB6, 0x5D, 0xB4, 0x6D, 0x48, 0xA6, 0x24, 0xD0, 0xC4, 0xCA,
    0xE7, 0x92, 0xBB, 0xB4, 0xAF, 0x28, 0xD5, 0x4A, 0x12, 0x2B, 0x53, 0x8B, 0x5C, 0xDA, 0xB1, 0xE0,
    0x0C, 0xD2, 0xA2, 0x23, 0xEB, 0x4A, 0x78, 0x3E, 0x5D, 0xA2, 0xE5, 0xF3, 0x12, 0x01, 0x2C, 0xAB,
    0x21, 0x1A, 0x89, 0xC4, 0xC2, 0x95, 0xD2, 0x60, 0x1B, 0x6B, 0x80, 0x93, 0x27, 0x65, 0x31, 0x80,
    0x65, 0x35, 0x90, 0xE0, 0x7A, 0x12, 0xAA, 0x9A, 0x60, 0x22, 0x2B, 0x1B, 0xE5, 0x25, 0x4B, 0x65,
    0xE0, 0xE7, 0xD3, 0x05, 0xCA, 0x6A, 0xD0, 0x66, 0x81, 0x2E, 0x4F, 0xE9, 0xFB, 0x13, 0x81, 0xBD,
    0xBF, 0x59, 0x59, 0x65, 0x0A, 0x6E, 0x5A, 0xAD, 0x7C, 0x3A, 0x5D, 0xD8, 0x59, 0xA0, 0xAF, 0x73,
    0x59, 0xD7, 0xA3, 0xBE, 0xCE, 0x55, 0xFB, 0xFE, 0xFF, 0x05, 0x7D, 0x9D, 0x3F, 0xA6, 0x1E, 0x19,
    0xC8, 0xFA, 0xB0, 0x51, 0xEE, 0x7B, 0xB7, 0xE3, 0xFB, 0xFC, 0xA7, 0x03, 0xB1, 0x21, 0x61, 0x3C,
    0x15, 0x6B, 0xCE, 0x53, 0x8C, 0x18, 0xF8, 0x9F, 0x72, 0x03, 0x9A, 0x10, 0xB2, 0xAD, 0xF3, 0x11,
    0xA1, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

std::vector<uint8_t> png_data2 = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x0A, 0x08, 0x06, 0x00, 0x00, 0x00, 0x8D, 0x32, 0xCF,
    0xBD, 0x00, 0x00, 0x00, 0x01, 0x73, 0x52, 0x47, 0x42, 0x00, 0xAE, 0xCE, 0x1C, 0xE9, 0x00, 0x00,
    0x00, 0x30, 0x49, 0x44, 0x41, 0x54, 0x28, 0x53, 0x63, 0x64, 0x20, 0x12, 0x30, 0x12, 0xA9, 0x8E,
    0x81, 0x6C, 0x85, 0xFF, 0x91, 0x6C, 0x40, 0x31, 0x04, 0xDD, 0xC4, 0xFF, 0xFF, 0xFF, 0xFF, 0x67,
    0x60, 0x64, 0x04, 0x0B, 0xE3, 0x57, 0x48, 0xAC, 0x89, 0x38, 0xFD, 0x46, 0xB6, 0x67, 0x28, 0x37,
    0x11, 0x00, 0x68, 0x84, 0x06, 0x0B, 0xB9, 0x17, 0x17, 0x2B, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

// 'favicon', 32x32px
std::vector<uint8_t>
    favicon_data = {
        0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x20, 0x00, 0x20, 0x00, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x21, 0xF9, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00,
        0x20, 0x00, 0x20, 0x00, 0x00, 0x02, 0x30, 0x84, 0x8F, 0xA9, 0xCB, 0xED, 0x0F, 0xA3, 0x9C, 0xB4,
        0xDA, 0x8B, 0xB3, 0xDE, 0xBC, 0xA3, 0x00, 0x86, 0xA2, 0x16, 0x30, 0x65, 0x06, 0x02, 0xE5, 0x99,
        0x92, 0xCA, 0xE9, 0x1A, 0x29, 0x8C, 0xBE, 0x9C, 0x38, 0x7A, 0xFA, 0xCE, 0xF7, 0xFE, 0x0F, 0x0C,
        0x0A, 0x87, 0xC4, 0xA2, 0xB1, 0x58, 0x00, 0x00, 0x3B};

// C3 00 00 00 00 C2 00 00 00
// -idk- -0 -0 -0
// 89 50 4E 47 0D 0A 1A 0A 00 00 00 0D 49 48 44 52 00 00 00 20 00 00 00 20 08 06 00 00 00 73 7A 7A F4 00 00 00 01 73 52 47 42 00 AE CE 1C E9 00 00 00 7C 49 44 41 54 58 47 ED 94 51 0A C0 20 0C 43 F5 FE 87 DE 28 A3 52 D8 D0 57 85 F5 27 FE 2E 4D 93 27 B3 B7 E2 D3 8B F7 37 05 10 01 11 10 01 4A E0 82 0F 16 F5
// 1B 76 74 C0 02 AC B4 44 F3 EA B1 32 F5 01 27 60 7A 5F 14 17 C6 EF 10 D6 23 A3 01 4C 3B 6B B8 D5 FE 34 C0 17 95 54 FB D3 00 71 D9 6F 04 66 0D 33 D7 99 FE 0B D2 68 E9 C0 56 6A 6A 4E 74 0A 20 02 22 20 02 22 20 02 E5 04 6E FF EB 0F 21 36 EA F8 F7 00 00 00 00 49 45 4E 44 AE 42 60 82

// Function to create payloads from GIF data
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

// 69 00    01 00 00    69 00 00 00      71 8F D0 4C      05 00 0D

std::string i0 = "92000000009100000089504e470d0a1a0a0000000d4948445200000020000000200806000000737a7af40000005849444154789cedd7310ec0200c43d1efdeffce66ea52b1112542b52fc0032259116000db744612004feba99bc8dd57ff64fc05020820800002b817a0b74fa700b65d8138fa828a2abf7706020820800002a88af8fb76bc000d111438934a728d0000000049454e44ae426082";
std::string i1 = "1010010000b9180000db42cb1405000d47494638396120002000f7ff00fedb96aa7353a97254fea800b269402b2b2b3b3a3a010101ffb68bb26a413c3a3afea800aa5b2f2b2b2b3b3939010101a972543c3a3aa97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9754a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9724a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9725a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9725497254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a97254a9725456555400000021ff0b4e45545343415045322e3003000000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b3997067590f323c7a50efcf9638a952acf952c312ed5a75eacfa64925221c4bd6ec54921a7b622debd627d596719576fd595768cbb55d0720e84bf520d18a76831a303060c06203669f563c4c31f1e2c682971af0299922e09f0610606e0c7971618333af2ec59c00f3e0cda1bfa616bb7a0081c6b70714783c1429c203c0830b1f7e0061cc850e0e183830803966e6ca9573ac785c6172c6cd0730d0e7cb981e99e0f1639709e5d7bf9e6051002580f0001fbf6efddbf9f4fbfbefdfbf1edcb5fbf1f3efdfef805e85f7d00b257a08008ced79f7c03e427e081090e08000021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffaf3075468cda217732e652a14e7478e3e0b4ca59ad569d29a587f3a58dab4ab83a148252234fb33a859921a7bb22d2b5668cbb81f7dd66dabd7edd9966bcd0e18e0d7afce9945fbea1dacd827d48a442b361cc78ef638a81c552a6bcf7b0c8be94196c36ccf32a46d003448fd68b56e2cc830762cb9e4dfb00c2980b1d1cd03de04068df070cece65811b742dd0e06fb56ed7b807007c4311f2cd0bcf760d1d50f144008a03b0004debf8729071fbe3b79f3e5d3abf77e7ebdfaf6f0df8f774f1f7dfdf4f1d9cbd77f7ffd80fee5e5675f80f379070021ff0be45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d39fcf020e1cd8fc58d1a2c29d2e3ffaf4075468cda21773fe6c9af323c7a0014682dd0d401559e2b5962c4ea606bd7af492522c4aa5501d7a05e8392d458136e56b376e1d26440f7a3ddb66fc9ca6db956f0000467f51e245a31af020303061898dc156a45c614ed4e8e7c18ab81a0962916f66a0001e7c8060a4c16ba58ac46bb9c097046bc7a28d29957b19e66c059f564db12671e3c40bcb8f1e30710c65ce8e08081030307386eedc39c78acb1536871c5d366fe9cf0d5c3d177db080f4e89c199c3f50002180f70010c08f3f5ffefcfbefede3dfcf1fbefefef4f9575f7e001668207fff0d58e0000a1e28e0830ede97608404c2070021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c183050528148602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d39fbf02051c38b0f9b1a245853b5d7e140a5428d199462fe6641a7428ce8f1c990a75d054eb509e2b5962f4cab52a57a760252224dbf5acd5831a6b6e35dbd62c4d067197b2a5bbf5ee5aaf0306f0757a068c5b95b03fb74eb206a45c314113b08acf8e756c714ff9ea54cd932618333b332a5cc80b3ddb46235ce0d5cda745fb0080fc89e4dbbf6018431173a38b07bc001d2bf0f18e0cdb1626e85bb27fb1ed0faf780e10e8a673e58c0f972e6d77d174008a03b0004debf8728071fbe3b79f3e5d3abf77e7ebdfaf6f0df8f774f1f7dfdf4f1d9cbd777ffd80fee5b517e07ded010021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa42ab466d18b3997067590f323c7a53e7f4efdd93469cdab5ca5669d3a14a9448458c5a625a9b1675aad63b3d264d056e95ba0fefc85a5da126dda0108f2e6ddaa7366d1b8060c0c182078ebd38a442b8e4dbc18b0de02067c3ea6e8f7a7010495171bc09cb9b0c8b19509544640fa334fab1851576650b9755989330f1ed8cdbbb7ef0308632e770c0c08101c72b1f2f5e9c6345e10a892b46ae9a7672e3069c733e582039f2d9de0f14314008a03c0004e6cfa7479fbe7d79f6eee3cb370f7fbe7afaebdfdbdfcf5f7efdfcfb0d00607ff81548607bff1da89f79000021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c183053280c806021430610190824409140c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b3997067590f323c7a53e7f4efdd93469cdab5ca5669d3a14a9448458a5faf31796a4c69e697fae5dbb952683b74ae31698ebaf6e4bb469020d8cbb6ee41a215c73a306060c000c606b63ead8898e258c68e052f35e0733245c03f0d20c8ec383263aa8759621c9b9940e6c18b45f3b4ba7a296906990b402e2b71e6c103c0830b1f7e0061cc850e0e183830807966e6ca9573ac785c61f2c6cd5de376bedcc0f4cf070b38376f9e99c1f803051002580f0001fbf6efddbf9fbf5e3efdfbf89dbcf0f5f7ffcfaf9edc7df8004fa37e000ff15982080030a68a08203020021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811cac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b3997067590f323c7a53e7f16f0e7ef67d3a435af7a5dca95ebd4a1482522c44ab6ec59841a7bb2d5ca752cd5967195ce653a97a65a8359a70e1830d5aece994503671dacd8e7d38a442b361eccd8ee638a6bb152a66cf8e04cb13f2933d85c18adc4b018030f1e4d3aabe9a1080fc8e4dbbf601b81a1d3a38b07bc001ca047e1f30c09b63c5980b773b18fc9bf5ef01c41d18c77cb0c073dfabb1332f801080770008be8329171f5ebcf7f2e7cdab5fff1d3dfbf5eee3c327ffbe7e7afbeae5b79fbf1f3ffb01fe99e79e80f8b9070021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708f0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa04eacfdfd099452fe65cdad429ce8f1c97fafc59d55f50074f25ae9c99f5e756a6fecc82e53913a1d6a55fd592d4d8f3edcfb816932a0abd42ed0b35bf5ba7d3b6000dec007895604ecb3306307512b2aa678b66961c76a21870c7970abe5cb8ef3266689d1f36506a0f3b2255b7a6961d4a903b34578a0b6eddbb80f208cb9d0c101df030e9c167ec0c06f8e15792bf4eda0b070d8c2071877809c22c202d183bfd6eebc004200e001202c082f9efc78f2e0cfa747cfbe7d78feedbc39f2fdf7cfcfbebf1b3a7ffbe7e7ffdee0d00207af011a81f7c000021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811cec59d3808102fe82dafc58d1a2c29d2e3ffa042ab466d18b399732cdf991a30307057cfeccbaf4ea50a435ad627520556b01af3c6722bcca75ab59af0835d6bc8ad56cdbb35e5bcafd48f7ee5dba34251eec1b740002bc3f011f245a916ed0c203de5e7d5a913145c7900d93cd3a3964c8c16cfd2118409af44f9f0e74b2c4d8b77482d287b57e91896355bd20470936e3b7b28c203c0830b1f7e20ae46870e0e183830807969e6ca9573ac1873617203a4993318c0c0f97203d329223b2ce0bc39e9ede50f144008a03d0004eedfc7871fbf7e7bfaf6f3ebdfaf1fbf7cf7fe01c8df80f505385f7f042638a07ff40d702081062a785f7c000021ff0b4e45545343415045322e30030100000021f94093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d381830205fcf9b3f9b1a245853b5d7ef409d427d199462fe6645ac029ce8f1c99fafc5955ab83a71257ceccfa736b53af69f92f4cad5acd5831a7b3215da962d4d067197fa142af46c59a72d116ee53b6040d7bf5f0f16ad68966e61b73ea356";
std::string i2 = "c908010002b9180000db42cb1405000d5c4c1172e1c788255314acf5f265c43a5962347b9981e7c3696b92fd59d8f4e9ada9251e3c40bbb6eddb0710c65ce8e040ef01074a073f60c037c78abb15f6765038b8ebe0038a3b38bef96001e8c05b676f5e002180ef0010802a0f3f5efcf8efe6d19f5fcf1e7cfaf6ecdfcb8f5f1ebe7df5f7d7cf774f9f7ffef603fc77de7b03e6f71e0021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b3997067590f323c7a53e7f4efdd93469cdab3ffdf9d39a75ea50a41211fa142b962956aa0835f65ccbd62d57a12de52a5d2a96ec5baa2dd5be1d8060eb569d338b967560c0c080018d0d6c7d5a9168c5b28d1f135e6ac027658a827f1a40a0f9b1e4c6700dce04fb533301cd85198fe6691563d9d20c3417887c56e2cc8307820b1f4efc405c8d0e1d1c3070604073cdcd972fe75831e642e58e9dbfcefd9cb901eaa00f163878ee5c3303f2070a2004c01e0082f6eee1bf874f9ffdfcfaf8f3b7bfaf3ffe7ef9f6e9c75f7f0416f81f81030068a082011238e0810b12080021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b39fdfd0cea20e7478e3efd69f5c77469d3a435b13ad8ca95eacfaf4825225caa75aacfb355116aece975a959b33419cc555ad76ddfbc6beb0e4070f7adce9945df06356060c000c606cc3ead48b4a262c68e072f35e07332c5c03f0d20c8ec3832e3b806678afd99394166c2a787a6658951b16302b71d17802c5be2cc8307820b1f4efc805c8d0e1d1c30706040f3cccd972fe75831e642e58d9d0f60b0fd397303d43f1f372cf05cfb76f3ce0b2004c01e0082f6eee1bf874fbfbefdfbf8e5df9fcf9e7ffcfafee527e07f03ea57e07e05fa37df00071ad8a082f0010021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d39f8302051c38b0f9b1a245853b5d7ef4e70fa850a2338d5eccc9d4e9509c1f390a2dc0f467d0ad4f79ae648911ecd6af3fc32a9588d0ac57a169af1ed458132e5aab7187b6a4fbd1aeddbb7069b235e8d7c10004809f1e2c5ad1af010303063c3680566a45c614ed3e8e7c78ab01a19629b6f58c807364ca8fe5169ca9752b67029c113b780c5562cdd63f4d33e05c60726da2080f081f4ebcf8018431173a3860e0c000e79c9d3367ceb1627285cb213f87bd1b7a7303d5451f392c00fd396706e50f144008a03d0004eedfc7871fbf7e7bfaf6f3eb778f7fbf7cfef3ddb75f7ffe15682080050e10e0810b0a582081083258200021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d39fbf02051c38b0f9b1a245853b5d7ef4e700a850a2338d5ecc29d4e9509c1f395615da34e8d6a13c57b2c4f8b52b57af5025de3c58d66ad3a70835d63c7bd6ebdba72de57ea45bd5ae5d9a12d97e1d80c02f579d512bd2f539608081c776a5562caab82ae3c608aa1a102a9922c2b3fe10341e6da0c0e3ab06676aad3a3ac1e8c2a7d3ca5edd74f40006a34d3f963df3e081dfc0830b3f1057a34307070c1c18b07cf4f2e4c939568cb910b981c60708dc66de58b901e99e0f163968cebd3103f2070a2004c01e0082f6eee1bf874f9ffdfcfaf8f3b7bfaf3ffe7ef9f6f527e080f9f107a080031c48e07f0c2e489f810e06d81e0021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3818302057cdafc58d1a2c29d2e3ffa042ab466d18b3997067590f323c7a53e7f4efdd93469cdab5ca5669d3a14a9448458c5a625a9b1675aad63b3d264d05629567ffe98be9d8b7629de0103b66ed539b3e858bcfe00c7f5f9b422d18a7113031ef0b631c5be5c274f0e4bf5e04cb03f2733d03c98a7558c63018f262dd734c203b063cb9e7d0061cc850e0ee41e704074ef03067473ac785b616e07807bafee3d20b883e1970f1660ce5b75f5e4051002d80e0001f7eedfbd7f24df2e9efcf8f3e8d3ab1f5f1e3c77f1ede3af9f4f1f7dfbf403c2d767af7ffefdf7fbd5070021ff0b4e45545343415045322e30030100000021f904093200ff002c00000000200020000708ff0011081c48b0a0c1830503280c80602143061019084c403141c38517150a7498f161c489153b8a4448b264c1901c394694a8f2634494185b4214b912c147933811ceac59d3808102057cdafc58d1a2c29d2e3ffa042ab466d18b3997063590f32347070ea6fed45a00eb50a435af66f5b9956c57075f25de3c8835a83f7f4c977a45a8b126d6ac6fdf72bd4b9341dd8f770be4856b966f4b84810df81b80a0abdcb90689564cec73c080c2589f56944c91b265c60e7e1ac81c3224dbb6543f5b163d5a274b8c813f27f8dc986cdadb62b3aa66f059ebed99070f081f4ebcf801ba1a1d3a3860e0c000e79f9d3367ceb162cc85cb0d583e4060006fe8cd0d543da788b000f4e79f199c3f50002180f70010c08f3f5ffefcfbefede3dfcf1fbefefef4f9575f7e001668207fff0d58e0000a1e28e0830ede97608404c207003b";

std::string a1 = "fe03000000fd03000089504e470d0a1a0a0000000d4948445200000020000000200806000000737a7af4000003c449444154789cc59651485b5718c77fce3247ba425c163161c258e246d13909995a300f65644d61be1465e0505ffb56903ecd62a944ba0711f6e6c3f630cbc698db5e1c5497c1361cd44c771bdcd4a556b0a634c5d425ace35228edd9839ee3bdb937c94dcad807977bce77beeffc7ff79c73bf7beb00c1ff68c76a49f27a8225c772fbb7ff5b00af27c885d3f6001bbb79125542d451c51618c5c7e6ae5bc63fe8ee0120b1bdef18e239a7e24693e24d2fb79aee9f2797ab9ecbf10ac8a7dfd8cd9714932b00ce57a122803c70d18047f972fa1300beff7d45f9de7df3ed837857bd0942e59480290be0f5044dc2c5e252d4ae6f04c9e94f48650bb61015df0229586c4631383a079d3e77d9bc6273b40572d254b6c0ec091f673a7d96b8c55456b5871f66558eccab690b8c104049713b90e1874740e50e63d575209318b7f85ba21316dfec091f6777962acee918e091366b1135c218213289719e9ff84ef51753d99230b685e8cacc9c13a6b2b698caaaab9c5900b49db46de00ba161fefe29a1fac55b21fb99c4b8290e28bb15357d0dc17edf8ba1e481bd4ea4244455004da3d7d89b86b6c91b16c196e8042dd109d6c74ed1347a4d095732cb21d476d2cc2fac71f9fc40c924af27c8fad829";
std::string a2 = "8bbf6df2c6b3ff0fcc2facd117eb8099395b88c1ae6e00ee9ebb64198b7e390c010f5ffc9a740c60fb1a5e9999a32fd6615989787f4cb5edfe072607ceaaf6a5af171c01d8be8697cf0f30bfb046fd0f9f5ac47ffef381f2ddbc738b9b776ea9b671dc085b358084a8246ed7ae0542d85d7bd343e291362bf6a68784b69316835ddd0210d1f6b088f7c784d7131422f5bebaceb4f845bc3f26a2ed610188c1ae6e21ad940620cafe92190bcac5af8e4af1c73f1e9cf4a72bdb34bdf31b4f57b6d17497f2cb78e3169532c77520f4ea1baa7de17490a5e41ae0e6fe47ee8371974ea4bb432dff2bdfc61dcd5bf66314ef8f594e73b43d4c2a5b20e4d22df19aeea2d3e726f1c72a7bd343ca2f0b939d555509a5b851509a044a650b44dbc365456b06287e7253d573f9d59856fe036806707d16062094cba3791bd51d8079e89d0aa0791b697637e3dfda24b2dca19297e27e951b596e3df2f73de035f77bf8b7364db9c5f3eb23ab1c6b483fa6eda57f4c83fac82abd53013676ebb93774120af7d504f46c91be5a0740c3ebc7556efaf00dc8eddfa6b7278006d06ace0de5f2dc6b3d4933904f66d0392c44bf5cdca621fd580501acfff5221c06cafef1d18212972673a5793d41d257ebd047564db932369fcce0dfda34cd213e39175285a1f1c3b74c85a2772a60f27f3312318d3f4b2e20fe05985205fa50ba9f84";

std::string b1 = "91020000009002000089504e470d0a1a0a0000000d4948445200000020000000200806000000737a7af40000025749444154789ced96bf4bdc6018c73f8683c3a662124eee942c874ab8e12c1e5d0a4d97da7fc0163adee2e2dea9937f40f72e2eee55702cb4532a45d02bf45a8e602583e1ee444972d81844b87438f3ead1f12dd821ef9237eff37c9e7cdfe707640248b9c7a5dce7c77301b9805c402e2017900bc805fc17020a0fb61e03d0380b69cde8e20950d12acc1d7568cde863fbbb3e32ec65f300a5e85eff15e0b27940e32ca41ff5e92ed600c60274176b54b40ab22cdc94e0cb9b638aeeb57002f8193ce4b27940b87f22de33df70ff84b9a30eb26cb6d2cdd546cae8df30d5df3e127b207dfa6e7eec7cbb698fd96558209dd86eda69db3fc79c9e04606da7c5c6f31ab6a5e0b843363e77d85c6db0b6d3627d791680b2a151374b00c8b02fb71c26d69767d3cc605ba3a1886243040730a72739f47a633e8e3be434889061df7feba1bc7a62503634baf336516ce0b843c28b98ddb40a40dd2ca14fa9940d4dc05ebf48dd2c21cb02285ebf08c0ebc29e80fc41c2b3df3fc44ddafeb9485b96eeddb4cabf600b99c171136c2bc0718737a9bac21f8cbaf43488380d22ca8646b57205801e7b84922c40c11f2442d987af01654313eaeaa60a806d29a2b68e7b8e6d29686a40db1f4ab1004a96b64ca5";
std::string b2 = "6d29e8532afe2021bc8845ed34351001efd652861525181946e311c5b71dec0f12513bc74d80dbcece960c0ba0686a20d2a6a981509e3555762380eebc0dc0a1d7c31f24c8b20005af5f449f5245830062beef0603e0d8193bf3fab1140ba0542b577c2cd6c41c1f7a3de2fa0b56961644cac28b184d0db02d85ba59626569814fdf7f21cb02fc01227114c7ce6ccc040000000049454e44ae426082";

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

// string to byte array
std::vector<uint8_t>
stringToBytes(std::string str)
{
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < str.size(); ++i)
  {
    bytes.push_back(str[i]);
  }
  return bytes;
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
  if (file && file.size() == 2)
  {
    display_mode = (DisplayMode)file.read();
    next_upload_slot = file.read() % max_files;
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

void handleShowPngDone()
{
  if (upload_status == 200)
  {
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

void handleStatus()
{
  const char *modes[] = {"idle", "slideshow", "clock"};
  String json = "{\"display_connected\":";
  json += paired ? "true" : "false";
  json += ",\"mode\":\"";
  json += modes[display_mode];
  json += "\",\"images\":";
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
        if (display_mode == MODE_CLOCK)
        {
          sendCommand(commands.clock(3, true, true, 100, 100, 180));
        }
      }

      if (ota_active)
      {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      // show a new upload right away, the clock keeps running and only stores it
      if (show_upload)
      {
        show_upload = false;
        if (display_mode != MODE_CLOCK)
        {
          last_display_change = millis();
          showNextImage();
        }
      }
      // change the display every 15 seconds
      else if (display_mode == MODE_SLIDESHOW && (last_display_change == 0 || millis() - last_display_change > 15000))
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

void loopOLD()
{
  // digitalWrite(LED_BUILTIN, HIGH); // turn the LED on (HIGH is the voltage level)
  //  put your main code here, to run repeatedly:
  /*
  BLEScanResults foundDevices = pBLEScan->start(scanTime, false);
  Serial.print("Devices found: ");
  Serial.println(foundDevices.getCount());
  Serial.println("Scan done!");
  pBLEScan->clearResults();   // delete results fromBLEScan buffer to release memory
  delay(2000);
  */

  if (paired == false)
  {
    Serial.println("Connecting to Server as client");
    Server_BLE_Address = new BLEAddress("9F:C9:66:73:FF:7C");
    if (connectToserver(*Server_BLE_Address))
    {
      paired = true;
      Serial.println("Paired");
    }
    else
    {
      Serial.println("Pairing failed");
      delay(5000);
    }
  }
  else
  {
    if (0)
    {
      Serial.println("Graffiti");
      printMatrix();
      delay(15000);
    }

    if (0)
    {
      Serial.println("printRandomMatrix");
      printRandomMatrix();
      delay(15000);
    }

    if (0)
    {
      Serial.println("printCorners");
      printCorners();
      delay(15000);
    }

    if (1)
    {
      Serial.println("Clock");
      sendCommand(commands.clock(3, true, true, 100, 100, 180));
      delay(5000);
    }

    if (0)
    {
      Serial.println("Sending GIF...");
      sendCommand(commands.setDrawMode(1));
      std::vector<std::vector<uint8_t>> payload = createPayloads(favicon_data);
      // loop through the payloads and send them
      for (size_t i = 0; i < payload.size(); ++i)
      {
        sendCommand(payload[i]);
        delay(100);
        // print playload to serial
        Serial.print("chunk: ");
        for (uint8_t byte : payload[i])
        {
          printf("%02X ", byte);
        }
        printf("\n");
      }
      Serial.println("Done!");
      delay(15000);
    }

    // Example PNG data

    // load png file to byte array

    if (0)
    {
      sendCommand(commands.setDrawMode(1));
      // Create payloads
      SendImage(png_data1);

      delay(15000);
    }

    if (0)
    {
      std::vector<uint8_t>
          payloads = createPNGPayloads(png_data1);
      uint8_t *dataArr = payloads.data();
      writeCharacteristic->writeValue(dataArr, payloads.size(), false);
      // sendCommand(payloads);
      printf("Upload done\n");
      // Print payloads for verification
      for (uint8_t byte : payloads)
      {
        printf("%02X ", byte);
      }
      printf("\n");

      delay(15000);
    }

    if (1)
    {
      printf("Test demo\n");

      printf("setDrawMode\n");
      sendCommand(commands.setDrawMode(1));

      std::vector<uint8_t> i1_hex = hexStringToByteArray(i0);
      printf("send data\n");
      sendCommand(i1_hex);
      // delay(100);
      // std::vector<uint8_t> i2_hex = hexStringToByteArray(i2);
      // sendCommand(i2_hex);
      // sendCommand(commands.setDrawMode(0));
      delay(15000);

      printf("send data2\n");
      sendCommand(hexStringToByteArray(b1));
      delay(150);
      sendCommand(hexStringToByteArray(b2));
      delay(15000);

      sendCommand(commands.setDrawMode(0));
    }

    // digitalWrite(LED_BUILTIN, LOW); // turn the LED off by making the voltage LOW
  }
}

/*
  def _createPayloads(self, png_data: bytearray) -> bytearray:
        """Creates payloads from a PNG file.

        Args:
            png_data (bytearray): data of the png file

        Returns:
            bytearray: returns bytearray payload
        """
        png_chunks = self._splitIntoChunks(png_data, 4096)
        idk = len(png_data) + len(png_chunks)
        idk_bytes = struct.pack("h", idk)  # Convert to 16-bit signed int
        png_len_bytes = struct.pack("i", len(png_data))
        payloads = bytearray()
        for i, chunk in enumerate(png_chunks):
            payload = (
                idk_bytes + bytearray([0, 0, 2 if i > 0 else 0]) + png_len_bytes + chunk
            )
            payloads.extend(payload)
        return payloads
*/
