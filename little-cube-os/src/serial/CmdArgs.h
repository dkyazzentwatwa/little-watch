#pragma once

#include <Arduino.h>

// In-place tokenizer for serial command lines. Splits on spaces; a token
// may be double-quoted to contain spaces ("Home Network"). Modifies the
// buffer (inserts NULs) and advances the cursor.
namespace cmdargs {

inline char* nextToken(char*& cursor) {
  if (cursor == nullptr) {
    return nullptr;
  }
  while (*cursor == ' ') {
    cursor++;
  }
  if (*cursor == '\0') {
    return nullptr;
  }

  char* start;
  if (*cursor == '"') {
    cursor++;
    start = cursor;
    while (*cursor != '\0' && *cursor != '"') {
      cursor++;
    }
  } else {
    start = cursor;
    while (*cursor != '\0' && *cursor != ' ') {
      cursor++;
    }
  }
  if (*cursor != '\0') {
    *cursor = '\0';
    cursor++;
  }
  return start;
}

// Remainder of the line, trimmed; quotes around the whole rest are removed.
inline char* rest(char*& cursor) {
  if (cursor == nullptr) {
    return nullptr;
  }
  while (*cursor == ' ') {
    cursor++;
  }
  if (*cursor == '\0') {
    return nullptr;
  }
  char* start = cursor;
  size_t len = strlen(start);
  while (len > 0 && start[len - 1] == ' ') {
    start[--len] = '\0';
  }
  if (len >= 2 && start[0] == '"' && start[len - 1] == '"') {
    start[len - 1] = '\0';
    start++;
  }
  cursor = nullptr;
  return start;
}

}  // namespace cmdargs
