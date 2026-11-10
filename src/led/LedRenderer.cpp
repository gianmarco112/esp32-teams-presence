#include "LedRenderer.h"
#include <math.h>
namespace {
constexpr uint16_t FRAME_INTERVAL_MS = 25;
constexpr float PI_F = 3.14159265f;
float wrap01(float v) { return v - floorf(v); }
uint8_t clampByte(float v) { if (v < 0.0f) return 0; if (v > 255.0f) return 255; return (uint8_t)v; }
}
void LedRenderer::begin(CRGB* pixels, uint16_t capacity, uint16_t activeCount) {
  _pixels = pixels; _capacity = capacity; _count = activeCount > capacity ? capacity : activeCount; stop();
}
void LedRenderer::stop() { _running = false; }
void LedRenderer::start(const CRGB& color, LedEffect effect, LedGeometry geometry,
                        uint8_t width, uint8_t height, bool serpentine) {
  _color = color; _effect = effect; _geometry = geometry;
  _width = width ? width : 1; _height = height ? height : 1;
  _serpentine = serpentine; _startedAt = millis(); _lastFrameAt = 0;
  _running = true; renderNow(_startedAt);
}
void LedRenderer::renderNow(uint32_t now) {
  if (!_pixels || !_count) return; render(now); _lastFrameAt = now;
}
bool LedRenderer::update(uint32_t now) {
  if (!_running || !_pixels || !_count || (uint32_t)(now - _lastFrameAt) < FRAME_INTERVAL_MS) return false;
  render(now); _lastFrameAt = now; return true;
}
void LedRenderer::coordinatesFor(uint16_t index, float& x, float& y) const {
  if (_geometry == LedGeometry::Ring) {
    const float a = (2.0f * PI_F * index) / (_count ? _count : 1);
    x = 0.5f + 0.5f * cosf(a); y = 0.5f + 0.5f * sinf(a); return;
  }
  if (_geometry == LedGeometry::Matrix) {
    const uint16_t w = _width ? _width : 1, row = index / w;
    uint16_t col = index % w; if (_serpentine && (row & 1U)) col = w - 1U - col;
    x = w > 1 ? (float)col / (w - 1U) : 0.5f;
    y = _height > 1 ? (float)(row % _height) / (_height - 1U) : 0.5f; return;
  }
  x = _count > 1 ? (float)index / (_count - 1U) : 0.5f; y = 0.5f;
}
void LedRenderer::render(uint32_t now) {
  const float elapsed = (float)(now - _startedAt), seconds = elapsed / 1000.0f;
  if (_effect == LedEffect::Static) {
    fill_solid(_pixels, _count, _color);
    if (_count < _capacity) fill_solid(_pixels + _count, _capacity - _count, CRGB::Black);
    return;
  }
  for (uint16_t i = 0; i < _count; i++) {
    float x = 0.5f, y = 0.5f; coordinatesFor(i, x, y); CRGB p = _color;
    switch (_effect) {
      case LedEffect::Pulse: {
        float v = 0.18f + 0.82f * (0.5f + 0.5f * sinf(seconds * 2.0f * PI_F * 0.65f));
        p.nscale8_video(clampByte(v * 255.0f)); break;
      }
      case LedEffect::Rainbow: {
        uint8_t h = (uint8_t)(i * 256U / (_count ? _count : 1) + (uint32_t)(elapsed * 0.035f));
        hsv2rgb_rainbow(CHSV(h, 255, 255), p); break;
      }
      case LedEffect::Wave: {
        float phase = x * 2.0f * PI_F * 1.5f - seconds * 3.2f;
        float v = 0.12f + 0.88f * (0.5f + 0.5f * sinf(phase));
        p.nscale8_video(clampByte(v * 255.0f)); break;
      }
      case LedEffect::Comet: {
        float head = wrap01(seconds * 0.32f), pos = (float)i / (_count ? _count : 1);
        float d = wrap01(head - pos), tail = d < 0.28f ? 1.0f - d / 0.28f : 0.0f;
        p.nscale8_video(clampByte(18.0f + tail * 237.0f)); break;
      }
      case LedEffect::Rotation: {
        float head = wrap01(seconds * 0.22f);
        float pos = _geometry == LedGeometry::Ring ? wrap01(atan2f(y - 0.5f, x - 0.5f) / (2.0f * PI_F)) : (float)i / (_count ? _count : 1);
        float d = fabsf(pos - head); if (d > 0.5f) d = 1.0f - d;
        p.nscale8_video(clampByte((0.08f + 0.92f * expf(-d * 18.0f)) * 255.0f)); break;
      }
      case LedEffect::Radar: {
        float a = atan2f(y - 0.5f, x - 0.5f), phase = wrap01(seconds * 0.18f) * 2.0f * PI_F;
        float d = fabsf(a - phase); if (d > PI_F) d = 2.0f * PI_F - d;
        p.nscale8_video(clampByte((0.08f + 0.92f * expf(-d * 4.0f)) * 255.0f)); break;
      }
      case LedEffect::Static: default: break;
    }
    _pixels[i] = p;
  }
  if (_count < _capacity) fill_solid(_pixels + _count, _capacity - _count, CRGB::Black);
}
