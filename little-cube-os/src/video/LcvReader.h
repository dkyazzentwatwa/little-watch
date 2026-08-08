#pragma once

#include <Arduino.h>

#include "../board_config.h"
#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <FS.h>

// .lcv container reader (docs/lcv-format.md). Pure parsing: header
// validation, sequential chunk iteration, index lookups. No decoding, no
// display, no tasks — VideoPlayer owns all of that. Instance methods are
// called from the reader task; the static readHeader() is safe anywhere.
struct LcvHeader {
  // As stored on disk, in whatever orientation `orientation` names — Rotated
  // is 252x448 for 16:9, Upright 368x206. Files packed before the orientation
  // byte existed hold zero there, which reads as Rotated: correct for every
  // one of them.
  uint16_t width = 0;
  uint16_t height = 0;
  VideoOrientation orientation = VideoOrientation::Rotated;
  uint16_t fps = 0;
  uint16_t audioChannels = 0;
  uint32_t audioRateHz = 0;
  uint32_t frameCount = 0;
  uint32_t durationMs = 0;
  uint32_t indexOffset = 0;
  uint32_t dataOffset = 0;
  uint32_t maxFrameBytes = 0;
};

class LcvReader {
 public:
  enum class ChunkType : uint8_t { Video = 1, Audio = 2 };

  static constexpr uint32_t kMaxFrameBytes = 96 * 1024;

  // Reads and validates the 64-byte header of `path` (absolute, already
  // sanitized). On failure writes a short human-readable reason.
  static bool readHeader(const char* path, LcvHeader& out, char* reasonOut, size_t reasonLen);

  bool open(const char* path, char* reasonOut, size_t reasonLen);
  void close();
  bool isOpen() const { return static_cast<bool>(file_); }
  const LcvHeader& header() const { return header_; }

  // Position the cursor at frame group N via the on-disk index: a single
  // 4-byte read at indexOffset + 4*N. The index is never loaded into RAM.
  bool seekToFrame(uint32_t frame);

  // Read the next chunk header. False at end of data (endOfData()) or on a
  // short/failed read.
  bool nextChunk(ChunkType& typeOut, uint32_t& sizeOut);
  // Read the current chunk payload into buf and skip the alignment padding.
  // sizeBytes must come from nextChunk(), which has already bounded it.
  bool readChunk(uint8_t* buf, uint32_t sizeBytes);
  // True when the cursor reached indexOffset cleanly — the natural end.
  bool endOfData() const { return eof_; }

 private:
  static bool parse(const uint8_t* raw, LcvHeader& out, uint32_t fileSize, char* reasonOut,
                    size_t reasonLen);

  fs::File file_;
  LcvHeader header_;
  bool eof_ = false;
};

#endif  // FEATURE_VIDEO
