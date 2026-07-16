#include "MultilineBuffer.h"

bool MultilineBuffer::start(fs::FS& fs, const char* tempPath, const char* targetPath,
                            SaveMode mode) {
  if (active_) {
    return false;
  }
  fs_ = &fs;
  tempPath_ = tempPath;
  targetPath_ = targetPath;
  mode_ = mode;
  bytes_ = 0;

  fs_->remove(tempPath_);
  file_ = fs_->open(tempPath_, FILE_WRITE);
  if (!file_) {
    return false;
  }
  active_ = true;
  return true;
}

void MultilineBuffer::abort() {
  if (file_) {
    file_.close();
  }
  if (fs_ != nullptr) {
    fs_->remove(tempPath_);
  }
  active_ = false;
}

MultilineBuffer::Result MultilineBuffer::finish() {
  file_.flush();
  file_.close();
  active_ = false;

  if (mode_ == SaveMode::Append) {
    // Stream temp onto the end of the target, then drop the temp.
    fs::File src = fs_->open(tempPath_, FILE_READ);
    if (!src) {
      return Result::Error;
    }
    fs::File dst = fs_->open(targetPath_, FILE_APPEND);
    if (!dst) {
      src.close();
      fs_->remove(tempPath_);
      return Result::Error;
    }
    uint8_t chunk[256];
    while (src.available() > 0) {
      const size_t n = src.read(chunk, sizeof(chunk));
      if (n == 0 || dst.write(chunk, n) != n) {
        src.close();
        dst.close();
        fs_->remove(tempPath_);
        return Result::Error;
      }
    }
    src.close();
    dst.close();
    fs_->remove(tempPath_);
    return Result::Saved;
  }

  // Overwrite: back up any existing target, then move the temp into place.
  const String bak = targetPath_ + ".bak";
  const bool hadOld = fs_->exists(targetPath_);
  if (hadOld) {
    fs_->remove(bak);
    if (!fs_->rename(targetPath_, bak)) {
      fs_->remove(tempPath_);
      return Result::Error;
    }
  }
  if (!fs_->rename(tempPath_, targetPath_)) {
    if (hadOld) {
      fs_->rename(bak, targetPath_);
    }
    fs_->remove(tempPath_);
    return Result::Error;
  }
  if (hadOld) {
    fs_->remove(bak);
  }
  return Result::Saved;
}

MultilineBuffer::Result MultilineBuffer::feedLine(const char* line) {
  if (!active_) {
    return Result::Error;
  }

  if (strcmp(line, ".END") == 0) {
    return finish();
  }
  if (strcmp(line, ".CANCEL") == 0) {
    abort();
    return Result::Cancelled;
  }
  if (strcmp(line, ".CLEAR") == 0) {
    file_.close();
    fs_->remove(tempPath_);
    file_ = fs_->open(tempPath_, FILE_WRITE);
    bytes_ = 0;
    if (!file_) {
      active_ = false;
      return Result::Error;
    }
    Serial.println("(cleared)");
    return Result::Collecting;
  }
  if (strcmp(line, ".PREVIEW") == 0) {
    file_.flush();
    fs::File preview = fs_->open(tempPath_, FILE_READ);
    if (preview) {
      Serial.println("--- preview ---");
      size_t shown = 0;
      char chunk[129];
      while (preview.available() > 0 && shown < 4096) {
        const size_t n = preview.readBytes(chunk, sizeof(chunk) - 1);
        if (n == 0) {
          break;
        }
        chunk[n] = '\0';
        Serial.print(chunk);
        shown += n;
      }
      if (preview.available() > 0) {
        Serial.println("\n... (preview truncated)");
      }
      preview.close();
      Serial.println("\n--- end preview ---");
    }
    return Result::Collecting;
  }

  const size_t lineLen = strlen(line);
  if (bytes_ + lineLen + 1 > kMaxBytes) {
    Serial.printf("error: input exceeds %u bytes; discarded\n", (unsigned)kMaxBytes);
    abort();
    return Result::Error;
  }
  if (file_.write(reinterpret_cast<const uint8_t*>(line), lineLen) != lineLen ||
      file_.write('\n') != 1) {
    Serial.println("error: write failed (card removed or full?); input discarded");
    abort();
    return Result::Error;
  }
  bytes_ += lineLen + 1;
  return Result::Collecting;
}
