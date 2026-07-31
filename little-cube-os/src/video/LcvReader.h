#pragma once

#include <Arduino.h>

#include "../feature_flags.h"

#if FEATURE_VIDEO

#include <FS.h>

// .lcv container reader (docs/lcv-format.md). Pure parsing: header
// validation, sequential chunk iteration, index lookups. No decoding, no
// display, no tasks — VideoPlayer owns all of that. Instance methods are
// called from the reader task; the static readHeader() is safe anywhere.
struct LcvHeader {
  uint16_t width = 0;   // rotated, as stored on disk: 252
  uint16_t height = 0;  // rotated, as stored on disk: 448
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
  bool isOpen() { return static_cast<bool>(file_); }
  const LcvHeader& header() const { return header_; }

  // Position the cursor at frame group N via the on-disk index: a single
  // 4-byte read at indexOffset + 4*N. The index is never loaded into RAM.
  bool seekToFrame(uint32_t frame);

  // Read the next chunk header. False at end of data (endOfData()) or on a
  // short/failed read.
  bool nextChunk(ChunkType& typeOut, uint32_t& sizeOut);
  // Read the current chunk payload (sizeBytes from nextChunk) into buf and
  // skip the alignment padding. buf must hold sizeBytes.
  bool readChunk(uint8_t* buf, uint32_t sizeBytes);
  bool skipChunk(uint32_t sizeBytes);
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
