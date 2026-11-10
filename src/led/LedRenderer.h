#pragma once
#include <Arduino.h>
#include <FastLED.h>

// Generates pixel frames independently of the physical output driver.
enum class LedGeometry : uint8_t { Strip = 0, Ring = 1, Matrix = 2 };
enum class LedEffect : uint8_t {
  Static = 0, Pulse = 1, Rainbow = 2, Wave = 3, Comet = 4, Rotation = 5, Radar = 6
};
class LedRenderer {
 public:
  void begin(CRGB* pixels, uint16_t capacity, uint16_t activeCount);
  void stop();
  void start(const CRGB& color, LedEffect effect, LedGeometry geometry,
             uint8_t width, uint8_t height, bool serpentine);
  void renderNow(uint32_t now);
  bool update(uint32_t now);
 private:
  CRGB* _pixels = nullptr;
  uint16_t _capacity = 0, _count = 0;
  CRGB _color = CRGB::Black;
  LedEffect _effect = LedEffect::Static;
  LedGeometry _geometry = LedGeometry::Strip;
  uint8_t _width = 8, _height = 8;
  bool _serpentine = true, _running = false;
  uint32_t _startedAt = 0, _lastFrameAt = 0;
  void render(uint32_t now);
  void coordinatesFor(uint16_t index, float& x, float& y) const;
};
