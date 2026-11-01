#include <Arduino.h>
#include <vector> // Include the vector header
#include "Commands.h"

Commands::Commands()
{
}

std::vector<uint8_t> Commands::on()
{
  uint8_t onMsg[] = {5, 0, 7, 1, 1};
  return std::vector<uint8_t>(onMsg, onMsg + 5);
}

std::vector<uint8_t> Commands::off()
{
  uint8_t offMsg[] = {5, 0, 7, 1, 0};
  return std::vector<uint8_t>(offMsg, offMsg + 5);
}

std::vector<uint8_t> Commands::setPixel(int x, int y, int r, int g, int b)
{
  uint8_t setPixelMsg[] = {
      10, 0, 5, 1, 0,
      r % 256,
      g % 256,
      b % 256,
      x % 32,
      y % 32};

  return std::vector<uint8_t>(setPixelMsg, setPixelMsg + 10);
}

std::vector<uint8_t> Commands::clock(int style, bool visibleDate, bool hour24, int r, int g, int b)
{
  uint8_t clockMsg[] = {
      8,
      0,
      6,
      1,
      ((style % 7) | (visibleDate ? 128 : 0)) | (hour24 ? 64 : 0),
      r % 256,
      g % 256,
      b % 256,
  };
  return std::vector<uint8_t>(clockMsg, clockMsg + 8);
}

// Set screen brightness. Range 5-100 (%)
std::vector<uint8_t> Commands::setBrightness(int brightnessPct)
{
  uint8_t brightnessMsg[] = {
      5,
      0,
      4,
      128,
      constrain(brightnessPct, 5, 100),
  };
  return std::vector<uint8_t>(brightnessMsg, brightnessMsg + 5);
}

// Set draw mode
std::vector<uint8_t> Commands::setDrawMode(int drawMode)
{
  uint8_t msg[] = {
      5,
      0,
      4,
      1,
      drawMode % 256,
  };
  return std::vector<uint8_t>(msg, msg + 5);
}