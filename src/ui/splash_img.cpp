// =====================================================================
// splash_img.cpp - decoding the splash picture with the mask ROM
//
// Why the ROM: the S3 screen has no JPEG peripheral (esp_driver_jpeg is gated on
// CONFIG_SOC_JPEG_CODEC_SUPPORTED, which the S3 does not set) and linking a
// decoder would cost tens of KB of a 1.875 MB OTA slot that is already 90 % full.
// The mask ROM, however, ships TJpgDec - esp32s3.rom.ld exports jd_prepare at
// 0x40000858 and jd_decomp at 0x40000864 - so the algorithm is free and this file
// is only the two callbacks it drives.
//
// What the ROM's build of TJpgDec is set up for is fixed and is written down in
// esp32s3/rom/tjpgd.h: JD_FORMAT 0, i.e. RGB888 output (3 bytes/pixel), a 512 B
// stream buffer, and the descaling feature enabled. RGB888 output is not a problem
// because tjpgd hands the picture over one MCU band at a time: each band is
// converted to the RGB565 the panel wants as it arrives, so a full-size RGB888
// image (1.15 MB) is never materialised.
//
// A JPEG of this size is around a fifth of a second of CPU on the S3, which is
// paid once, while the panel would otherwise be blank.
// =====================================================================
#include "ui/splash_img.h"

#include <Arduino.h>

#include <esp_heap_caps.h>

#include "esp32s3/rom/tjpgd.h"

// board_build.embed_files in platformio.ini puts the JPEG in flash and defines
// these, named after its path. The linker drops the data again unless something
// refers to it, which is why the aliases live here and nowhere else.
extern const uint8_t splash_jpg_start[] asm("_binary_docs_mockups_candash_splash_base_92_jpg_start");
extern const uint8_t splash_jpg_end[] asm("_binary_docs_mockups_candash_splash_base_92_jpg_end");

namespace splashimg {
namespace {

constexpr int kWidth = 800;
constexpr int kHeight = 480;
constexpr size_t kImageBytes = (size_t)kWidth * (size_t)kHeight * 2;  // RGB565

// TJpgDec's pool: its Huffman and quantisation tables plus the IDCT / colour
// conversion work buffer. A baseline image's tables are ~2 KB, so this is
// comfortable. It is internal RAM because the IDCT hits it hard, and it is freed
// the moment the decode returns, so it never overlaps the dashboard.
constexpr size_t kWorkPoolBytes = 32 * 1024;

uint16_t *g_pixels = nullptr;
lv_image_dsc_t g_dsc = {};

// Where the input callback has got to. The file is memory-mapped flash, so there
// is nothing to buffer between calls - it is a cursor and an end.
struct Reader {
  const uint8_t *p;
  const uint8_t *end;
};

UINT readFromFlash(JDEC *jd, BYTE *buff, UINT nbytes) {
  Reader *r = (Reader *)jd->device;
  const size_t left = (size_t)(r->end - r->p);
  const UINT take = ((size_t)nbytes < left) ? nbytes : (UINT)left;
  if (buff != nullptr && take > 0) memcpy(buff, r->p, take);
  r->p += take;
  return take;  // fewer than asked for means end of file, which tjpgd handles
}

UINT writeBand(JDEC *, void *band, JRECT *rect) {
  const uint8_t *src = (const uint8_t *)band;  // RGB888, one band
  for (int y = (int)rect->top; y <= (int)rect->bottom; y++) {
    uint16_t *dst = g_pixels + (size_t)y * (size_t)kWidth + (size_t)rect->left;
    for (int x = (int)rect->left; x <= (int)rect->right; x++) {
      const uint8_t r = src[0];
      const uint8_t g = src[1];
      const uint8_t b = src[2];
      src += 3;
      *dst++ = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
    }
  }
  return 1;  // non-zero: keep going
}

}  // namespace

bool decode() {
  release();

  g_pixels = (uint16_t *)heap_caps_malloc(kImageBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (g_pixels == nullptr) {
    Serial.printf("[splash] no PSRAM for the picture (%u B) - flat field instead\n",
                  (unsigned)kImageBytes);
    return false;
  }

  void *pool = heap_caps_malloc(kWorkPoolBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (pool == nullptr) {
    Serial.println("[splash] no internal RAM for the decoder pool - flat field instead");
    release();
    return false;
  }

  Reader reader = {splash_jpg_start, splash_jpg_end};
  JDEC jd = {};
  const uint32_t started = millis();

  JRESULT r = jd_prepare(&jd, readFromFlash, pool, (UINT)kWorkPoolBytes, &reader);
  if (r == JDR_OK && (jd.width != (UINT)kWidth || jd.height != (UINT)kHeight)) {
    Serial.printf("[splash] picture is %ux%u, not %dx%d - flat field instead\n",
                  (unsigned)jd.width, (unsigned)jd.height, kWidth, kHeight);
    r = JDR_PAR;
  }
  if (r == JDR_OK) r = jd_decomp(&jd, writeBand, 0);  // 0 = full size, no descale

  heap_caps_free(pool);
  const uint32_t ms = millis() - started;

  if (r != JDR_OK) {
    Serial.printf("[splash] JPEG decode failed (%d) after %lu ms - flat field instead\n", (int)r,
                  (unsigned long)ms);
    release();
    return false;
  }

  g_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  g_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  g_dsc.header.w = kWidth;
  g_dsc.header.h = kHeight;
  g_dsc.header.stride = kWidth * 2;  // 1600 B, already 4-byte aligned
  g_dsc.data_size = (uint32_t)kImageBytes;
  g_dsc.data = (const uint8_t *)g_pixels;

  Serial.printf("[splash] picture %dx%d in %lu ms (%u B flash, %u B PSRAM)\n", kWidth, kHeight,
                (unsigned long)ms, (unsigned)(splash_jpg_end - splash_jpg_start),
                (unsigned)kImageBytes);
  return true;
}

const lv_image_dsc_t *image() { return (g_pixels != nullptr) ? &g_dsc : nullptr; }

void release() {
  if (g_pixels != nullptr) {
    heap_caps_free(g_pixels);
    g_pixels = nullptr;
  }
  g_dsc = {};
}

}  // namespace splashimg
