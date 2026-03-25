#include "display_transform.h"

#include <algorithm>
#include <cmath>

namespace video_server {
namespace {

float clamp01(float value) { return std::max(0.0F, std::min(1.0F, value)); }

uint8_t luminance_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>(0.2126F * static_cast<float>(r) + 0.7152F * static_cast<float>(g) +
                              0.0722F * static_cast<float>(b));
}

void rainbow_map(float x, uint8_t& r, uint8_t& g, uint8_t& b) {
  const float t = clamp01(x);
  const float six = t * 6.0F;
  const int region = static_cast<int>(std::floor(six)) % 6;
  const float f = six - std::floor(six);
  const float q = 1.0F - f;

  float rf = 0.0F, gf = 0.0F, bf = 0.0F;
  switch (region) {
    case 0:
      rf = 1.0F;
      gf = f;
      bf = 0.0F;
      break;
    case 1:
      rf = q;
      gf = 1.0F;
      bf = 0.0F;
      break;
    case 2:
      rf = 0.0F;
      gf = 1.0F;
      bf = f;
      break;
    case 3:
      rf = 0.0F;
      gf = q;
      bf = 1.0F;
      break;
    case 4:
      rf = f;
      gf = 0.0F;
      bf = 1.0F;
      break;
    default:
      rf = 1.0F;
      gf = 0.0F;
      bf = q;
      break;
  }

  r = static_cast<uint8_t>(rf * 255.0F);
  g = static_cast<uint8_t>(gf * 255.0F);
  b = static_cast<uint8_t>(bf * 255.0F);
}

void ironbow_map(float x, uint8_t& r, uint8_t& g, uint8_t& b) {
  const float t = clamp01(x);
  if (t < 0.25F) {
    const float k = t / 0.25F;
    r = static_cast<uint8_t>(30.0F * k);
    g = 0;
    b = static_cast<uint8_t>(80.0F + 60.0F * k);
    return;
  }
  if (t < 0.5F) {
    const float k = (t - 0.25F) / 0.25F;
    r = static_cast<uint8_t>(30.0F + 90.0F * k);
    g = static_cast<uint8_t>(10.0F + 30.0F * k);
    b = static_cast<uint8_t>(140.0F - 80.0F * k);
    return;
  }
  if (t < 0.75F) {
    const float k = (t - 0.5F) / 0.25F;
    r = static_cast<uint8_t>(120.0F + 90.0F * k);
    g = static_cast<uint8_t>(40.0F + 80.0F * k);
    b = static_cast<uint8_t>(60.0F - 40.0F * k);
    return;
  }
  const float k = (t - 0.75F) / 0.25F;
  r = static_cast<uint8_t>(210.0F + 45.0F * k);
  g = static_cast<uint8_t>(120.0F + 110.0F * k);
  b = static_cast<uint8_t>(20.0F + 35.0F * k);
}

void arctic_map(float x, uint8_t& r, uint8_t& g, uint8_t& b) {
  const float t = clamp01(x);
  r = static_cast<uint8_t>(40.0F + 170.0F * t);
  g = static_cast<uint8_t>(70.0F + 140.0F * t);
  b = static_cast<uint8_t>(120.0F + 135.0F * t);
}

void map_output_to_source(uint32_t out_x, uint32_t out_y, uint32_t width, uint32_t height, int rotation,
                          uint32_t& src_x, uint32_t& src_y) {
  switch (rotation) {
    case 90:
      src_x = out_y;
      src_y = (height - 1U) - out_x;
      break;
    case 180:
      src_x = (width - 1U) - out_x;
      src_y = (height - 1U) - out_y;
      break;
    case 270:
      src_x = (width - 1U) - out_y;
      src_y = out_x;
      break;
    default:
      src_x = out_x;
      src_y = out_y;
      break;
  }
}

}  // namespace

bool apply_display_transform(const VideoFrameView& frame, const StreamOutputConfig& config, RgbImage& out) {
  if (frame.data == nullptr || frame.width == 0 || frame.height == 0) {
    return false;
  }

  if (!(frame.pixel_format == VideoPixelFormat::RGB24 || frame.pixel_format == VideoPixelFormat::BGR24 ||
        frame.pixel_format == VideoPixelFormat::GRAY8)) {
    return false;
  }

  if (!(config.rotation_degrees == 0 || config.rotation_degrees == 90 || config.rotation_degrees == 180 ||
        config.rotation_degrees == 270)) {
    return false;
  }

  const uint32_t rotated_width =
      (config.rotation_degrees == 90 || config.rotation_degrees == 270) ? frame.height : frame.width;
  const uint32_t rotated_height =
      (config.rotation_degrees == 90 || config.rotation_degrees == 270) ? frame.width : frame.height;

  RgbImage rotated;
  rotated.width = rotated_width;
  rotated.height = rotated_height;
  rotated.rgb.resize(static_cast<size_t>(rotated.width) * rotated.height * 3);

  const auto* input = static_cast<const uint8_t*>(frame.data);
  for (uint32_t y = 0; y < rotated.height; ++y) {
    for (uint32_t x = 0; x < rotated.width; ++x) {
      const size_t out_idx = (static_cast<size_t>(y) * rotated.width + x) * 3;

      uint32_t src_x = 0;
      uint32_t src_y = 0;
      map_output_to_source(x, y, frame.width, frame.height, config.rotation_degrees, src_x, src_y);
      if (config.mirrored) {
        src_x = (frame.width - 1U) - src_x;
      }

      const size_t src_idx = static_cast<size_t>(src_y) * frame.stride_bytes;
      uint8_t r = 0, g = 0, b = 0;
      uint8_t intensity = 0;
      if (frame.pixel_format == VideoPixelFormat::GRAY8) {
        intensity = input[src_idx + src_x];
        r = g = b = intensity;
      } else {
        const size_t px = src_idx + src_x * 3;
        if (frame.pixel_format == VideoPixelFormat::RGB24) {
          r = input[px + 0];
          g = input[px + 1];
          b = input[px + 2];
        } else {
          b = input[px + 0];
          g = input[px + 1];
          r = input[px + 2];
        }
        intensity = luminance_from_rgb(r, g, b);
      }

      const float n = clamp01((static_cast<float>(intensity) / 255.0F - config.palette_min) /
                              std::max(0.0001F, config.palette_max - config.palette_min));
      switch (config.display_mode) {
        case VideoDisplayMode::Passthrough:
          break;
        case VideoDisplayMode::Grayscale:
          r = g = b = intensity;
          break;
        case VideoDisplayMode::WhiteHot: {
          const uint8_t v = static_cast<uint8_t>(n * 255.0F);
          r = g = b = v;
          break;
        }
        case VideoDisplayMode::BlackHot: {
          const uint8_t v = static_cast<uint8_t>((1.0F - n) * 255.0F);
          r = g = b = v;
          break;
        }
        case VideoDisplayMode::Ironbow:
          ironbow_map(n, r, g, b);
          break;
        case VideoDisplayMode::Rainbow:
          rainbow_map(n, r, g, b);
          break;
        case VideoDisplayMode::Arctic:
          arctic_map(n, r, g, b);
          break;
      }

      rotated.rgb[out_idx + 0] = r;
      rotated.rgb[out_idx + 1] = g;
      rotated.rgb[out_idx + 2] = b;
    }
  }

  const uint32_t target_width = config.output_width > 0 ? config.output_width : rotated.width;
  const uint32_t target_height = config.output_height > 0 ? config.output_height : rotated.height;
  if (target_width == rotated.width && target_height == rotated.height) {
    out = std::move(rotated);
    return true;
  }

  out.width = target_width;
  out.height = target_height;
  out.rgb.resize(static_cast<size_t>(out.width) * out.height * 3);

  for (uint32_t y = 0; y < out.height; ++y) {
    const uint32_t src_y = std::min(rotated.height - 1U, (y * rotated.height) / out.height);
    for (uint32_t x = 0; x < out.width; ++x) {
      const uint32_t src_x = std::min(rotated.width - 1U, (x * rotated.width) / out.width);
      const size_t src_idx = (static_cast<size_t>(src_y) * rotated.width + src_x) * 3;
      const size_t dst_idx = (static_cast<size_t>(y) * out.width + x) * 3;
      out.rgb[dst_idx + 0] = rotated.rgb[src_idx + 0];
      out.rgb[dst_idx + 1] = rotated.rgb[src_idx + 1];
      out.rgb[dst_idx + 2] = rotated.rgb[src_idx + 2];
    }
  }

  return true;
}

}  // namespace video_server
