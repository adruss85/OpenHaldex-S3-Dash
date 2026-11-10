// =====================================================================
// splash_img.h - the boot splash's background picture
//
// The splash's gradient field, wordmark and the empty track of its progress
// rule are one pre-rendered 800x480 baseline JPEG. They are drawn as a picture
// because LVGL's software renderer paints those gradients in visible bands and
// slowly enough that the progress fill outruns the paint - the 0.6.1 splash
// dropped them for exactly that reason and was a flat black field instead.
//
// Nothing is linked to decode it: the ESP32-S3's mask ROM contains TJpgDec
// (jd_prepare / jd_decomp, exported by esp32s3.rom.ld), so this module is a
// wrapper around the ROM and costs about 1 KB of flash rather than a library.
//
// The decoded picture is RGB565 in PSRAM - 768 000 B - and is held only for the
// three seconds the splash is up. See src/ui/dash_ui.cpp for the rule drawn on
// top of it and the version label in the corner.
// =====================================================================
#pragma once

#include <lvgl.h>

namespace splashimg {

/// Decode the embedded splash JPEG into an RGB565 buffer in PSRAM.
/// Returns false, having logged why, if the image is not a baseline 800x480
/// file or the buffers cannot be had; the splash then falls back to a flat
/// field with the same rule and labels on it.
bool decode();

/// The decoded picture, for lv_image_set_src(). nullptr until a decode()
/// has succeeded, and again after release().
const lv_image_dsc_t *image();

/// Free the picture. Called when the splash comes down, so the dashboard
/// gets its PSRAM back.
void release();

}  // namespace splashimg
