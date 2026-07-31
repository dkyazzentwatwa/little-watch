#include "LcvReader.h"

#if FEATURE_VIDEO

#include <SD_MMC.h>
#include <string.h>

#include "../board_config.h"

namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void reason(char* out, size_t len, const char* msg) {
  if (out != nullptr && len > 0) {
    strncpy(out, msg, len - 1);
    out[len - 1] = '\0';
  }
}

}  // namespace

bool LcvReader::parse(const uint8_t* raw, LcvHeader& out, uint32_t fileSize, char* reasonOut,
                      size_t reasonLen) {
  if (memcmp(raw, "LCV1", 4) != 0) {
    reason(reasonOut, reasonLen, "not an .lcv file (bad magic)");
    return false;
  }
  if (rd16(raw + 4) != 1 || rd16(raw + 6) != 64) {
    reason(reasonOut, reasonLen, "unsupported .lcv version");
    return false;
  }
  out.width = rd16(raw + 8);
  out.height = rd16(raw + 10);
  out.fps = rd16(raw + 12);
  out.audioChannels = rd16(raw + 14);
  out.audioRateHz = rd32(raw + 16);
  out.frameCount = rd32(raw + 20);
  out.durationMs = rd32(raw + 24);
  out.indexOffset = rd32(raw + 28);
  out.dataOffset = rd32(raw + 32);
  out.maxFrameBytes = rd32(raw + 36);
  if (out.width == 0 || out.width > DISPLAY_WIDTH || out.height == 0 ||
      out.height > DISPLAY_HEIGHT) {
    reason(reasonOut, reasonLen, "frame size exceeds panel");
    return false;
  }
  if (out.fps == 0 || out.fps > 30 || out.audioChannels != 1 || out.audioRateHz < 8000 ||
      out.audioRateHz > 48000 || out.audioRateHz % out.fps != 0) {
    reason(reasonOut, reasonLen, "bad fps/audio parameters");
    return false;
  }
  if (out.frameCount == 0 || out.durationMs == 0 || out.dataOffset < 64 ||
      out.indexOffset <= out.dataOffset ||
      static_cast<uint64_t>(out.indexOffset) + 4ull * out.frameCount > fileSize) {
    reason(reasonOut, reasonLen, "corrupt .lcv layout");
    return false;
  }
  if (out.maxFrameBytes == 0 || out.maxFrameBytes > kMaxFrameBytes) {
    reason(reasonOut, reasonLen, "frames too large for playback");
    return false;
  }
  // Reserved bytes (40..63) are deliberately NOT checked against zero: minor
  // additive format extensions may use them without a version bump, and a v1
  // reader stays forward-compatible by ignoring them.
  return true;
}

bool LcvReader::readHeader(const char* path, LcvHeader& out, char* reasonOut, size_t reasonLen) {
  fs::File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    reason(reasonOut, reasonLen, "cannot open file");
    return false;
  }
  uint8_t raw[64];
  const bool ok = f.read(raw, sizeof(raw)) == sizeof(raw);
  const uint32_t size = f.size();
  f.close();
  if (!ok) {
    reason(reasonOut, reasonLen, "file shorter than header");
    return false;
  }
  return parse(raw, out, size, reasonOut, reasonLen);
}

bool LcvReader::open(const char* path, char* reasonOut, size_t reasonLen) {
  close();
  file_ = SD_MMC.open(path, FILE_READ);
  if (!file_) {
    reason(reasonOut, reasonLen, "cannot open file");
    return false;
  }
  uint8_t raw[64];
  if (file_.read(raw, sizeof(raw)) != sizeof(raw) ||
      !parse(raw, header_, file_.size(), reasonOut, reasonLen)) {
    close();
    return false;
  }
  eof_ = false;
  if (!file_.seek(header_.dataOffset)) {
    close();  // an I/O fault here (card pulled mid-open) must not leak the handle
    return false;
  }
  return true;
}

void LcvReader::close() {
  if (file_) {
    file_.close();
  }
  eof_ = false;
}

bool LcvReader::seekToFrame(uint32_t frame) {
  if (!file_ || frame >= header_.frameCount) {
    return false;
  }
  uint8_t raw[4];
  if (!file_.seek(header_.indexOffset + 4 * frame) || file_.read(raw, 4) != 4) {
    return false;
  }
  const uint32_t off = rd32(raw);
  if (off < header_.dataOffset || off >= header_.indexOffset) {
    return false;  // corrupted index entry — never seek outside the data region
  }
  eof_ = false;
  return file_.seek(off);
}

bool LcvReader::nextChunk(ChunkType& typeOut, uint32_t& sizeOut) {
  if (!file_ || eof_) {
    return false;
  }
  if (file_.position() >= header_.indexOffset) {
    eof_ = true;  // ran cleanly into the index: natural end of data
    return false;
  }
  uint8_t raw[4];
  if (file_.read(raw, 4) != 4) {
    return false;
  }
  const uint8_t type = raw[0];
  if (type != static_cast<uint8_t>(ChunkType::Video) &&
      type != static_cast<uint8_t>(ChunkType::Audio)) {
    return false;  // desynced — treated as a read failure by the caller
  }
  typeOut = static_cast<ChunkType>(type);
  sizeOut = static_cast<uint32_t>(raw[1]) | (static_cast<uint32_t>(raw[2]) << 8) |
            (static_cast<uint32_t>(raw[3]) << 16);
  // Bound the chunk before the caller sizes a read from it: a video payload
  // may never exceed the header's maxFrameBytes, and no chunk of any type may
  // extend past the start of the index. (maxFrameBytes describes video
  // payloads only — audio chunks are bounded by the layout check alone.)
  if (typeOut == ChunkType::Video && sizeOut > header_.maxFrameBytes) {
    return false;
  }
  const uint32_t pad = (4 - (sizeOut % 4)) % 4;
  if (static_cast<uint64_t>(file_.position()) + sizeOut + pad > header_.indexOffset) {
    return false;
  }
  return true;
}

bool LcvReader::readChunk(uint8_t* buf, uint32_t sizeBytes) {
  if (!file_ || file_.read(buf, sizeBytes) != sizeBytes) {
    return false;
  }
  const uint32_t pad = (4 - (sizeBytes % 4)) % 4;
  return pad == 0 || file_.seek(file_.position() + pad);
}

bool LcvReader::skipChunk(uint32_t sizeBytes) {
  if (!file_) {
    return false;
  }
  const uint32_t pad = (4 - (sizeBytes % 4)) % 4;
  return file_.seek(file_.position() + sizeBytes + pad);
}

#endif  // FEATURE_VIDEO
