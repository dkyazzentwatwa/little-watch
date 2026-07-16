#include "MultilineBuffer.h"

// TODO(task-10): bounded buffer + SD temp-file streaming + atomic rename;
// .PREVIEW/.CLEAR handling; terminator excluded from content.

void MultilineBuffer::start(const char* targetPath) {
  targetPath_ = targetPath;
  buffer_ = "";
  active_ = true;
}

MultilineBuffer::Result MultilineBuffer::feedLine(const char* line) {
  (void)line;
  active_ = false;
  return Result::Cancelled;
}
