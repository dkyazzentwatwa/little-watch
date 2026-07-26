#include "BookService.h"

#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD_MMC.h>

#include "../hardware/SdCardAdapter.h"
#include "../storage/AtomicFile.h"
#include "../storage/SdStorage.h"
#include "../storage/StoragePaths.h"

namespace {

constexpr const char* kPosPath = "/reader_pos.json";

// widgets::textBlock draws into a line[96] buffer, so it never lays more than
// 95 characters on one line regardless of width. The pagination replay must
// honor the same cap or it would count byte boundaries the renderer never draws.
constexpr int kWrapLineMax = 95;

bool isBookFile(const char* name) {
  if (name == nullptr || name[0] == '.') {
    return false;  // skip dotfiles / macOS AppleDouble (._foo) junk
  }
  const String n(name);
  if (n.endsWith(".tmp") || n.endsWith(".bak") || n.endsWith(".partial")) {
    return false;
  }
  return n.endsWith(".txt") || n.endsWith(".md");
}

const char* baseName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash != nullptr ? slash + 1 : path;
}

// Fixed scratch for readPage(): a raw byte window and, parallel to the cleaned
// display text, a map from each display char to the RAW source byte (relative
// to the window start) that produced it. readPage() is called ONLY from the
// kernel/UI thread (ReaderApp), never reentrantly and never from a task, so
// these file-scope buffers are safe and keep ~9 KB off the call stack and out
// of the heap (fixed size, no growth). kScratchCap must be >= the caller's page
// buffer (ReaderApp::kPageBufCap == 3072).
constexpr size_t kScratchCap = 3072;
constexpr size_t kMaxTagLen = 256;    // cap an HTML-tag scan so a stray '<' cannot eat a page
constexpr int kMaxSkipChunks = 32;    // a page turn skips at most this many all-markup windows
uint8_t sRawBuf[kScratchCap];
uint16_t sSrcMap[kScratchCap];

bool isMarkdownPath(const char* path) {
  const String n(path);
  return n.endsWith(".md");
}

inline bool isAsciiLetter(uint8_t c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// A '<' begins an HTML tag only when followed by a name char, '/', '!' or '?'.
// Otherwise it is a literal '<' (e.g. "a < b") and must NOT swallow text.
inline bool isTagStartByte(uint8_t c) {
  return isAsciiLetter(c) || c == '/' || c == '!' || c == '?';
}

inline uint8_t lowerByte(uint8_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// True when raw[open..close] (open=='<', close=='>') is a line-break tag, in any
// of the forms <br>, <br/>, <br />, </br>.
bool isBrTag(const uint8_t* raw, size_t open, size_t close) {
  size_t i = open + 1;                       // char after '<'
  if (i < close && raw[i] == '/') i++;       // allow </br>
  if (i >= close || lowerByte(raw[i]) != 'b') return false;
  i++;
  if (i >= close || lowerByte(raw[i]) != 'r') return false;
  i++;
  return i == close || raw[i] == ' ' || raw[i] == '\t' || raw[i] == '/';
}

// Turns a raw byte window into the ASCII the 6x8 font can draw, and records, for
// every display char emitted, the source byte offset (relative to the window
// start) that produced it. That map is the whole trick behind keeping nextOffset
// a valid RAW byte offset even though the wrap now runs over TRANSFORMED chars:
// after the wrap decides a page ends at display char N, readPage() reads back
// srcMap[N] to get the raw byte where the next page begins. Every rule below only
// DROPS or COLLAPSES bytes (an N-byte run yields <= N display chars), so the
// source cursor is monotonic and the map is well defined. The one many-to-one
// case (ellipsis -> "...") maps its trailing dots to the END of the sequence, so
// a page break landing inside "..." still advances and never re-emits shown dots.
//
// `markdown` gets the full cleaner; a .txt gets only the UTF-8 decode and the
// blank-line collapse (a .txt may legitimately contain '#', '*', '<', ...).
// srcMap needs maxDisplay+1 slots: one per emitted char, plus a sentinel at
// srcMap[displayLen] holding the final source cursor (one past the last byte
// consumed), used when a page consumes the whole window.
void cleanTransform(const uint8_t* raw, size_t rawLen, bool rawIsEof, bool markdown,
                    char* out, size_t maxDisplay, uint16_t* srcMap, size_t* displayLenOut) {
  size_t s = 0;          // source cursor (bytes from window start)
  size_t d = 0;          // display cursor (index into out)
  int nlSinceText = 0;   // consecutive '\n' emitted since the last visible char
  size_t colChars = 0;   // display chars on the current line (0 == line start)

  auto emit = [&](char ch, size_t srcBegin) -> bool {
    if (d >= maxDisplay) return false;
    srcMap[d] = static_cast<uint16_t>(srcBegin);
    out[d] = ch;
    d++;
    return true;
  };

  while (s < rawLen) {
    // ---- markdown line markers (only at the very start of a line) ----
    if (markdown && colChars == 0) {
      // If this line is cut off by the window edge (and more file follows),
      // defer the WHOLE line to the next page so a marker (#, ---, -, >) is
      // always decided on the complete line — never half-detected, never leaked
      // as literal text at the edge. The d>0 guard keeps the page making
      // progress (an over-long first line falls through and is emitted instead).
      if (!rawIsEof && d > 0) {
        size_t q = s;
        while (q < rawLen && raw[q] != '\n') q++;
        if (q >= rawLen) break;  // no newline before the edge -> defer the line
      }
      size_t t = s;
      while (t < rawLen && (raw[t] == ' ' || raw[t] == '\t')) t++;  // peek past indent
      if (t < rawLen) {
        const uint8_t c = raw[t];

        // Heading: #{1..6} + space -> drop the marker, keep the text as prose.
        if (c == '#') {
          size_t h = t;
          int hashes = 0;
          while (h < rawLen && raw[h] == '#' && hashes < 6) { h++; hashes++; }
          if (h < rawLen && raw[h] == ' ') {
            while (h < rawLen && (raw[h] == ' ' || raw[h] == '\t')) h++;
            s = h;  // rest of the line emits as normal text
            continue;
          }
        }

        // Horizontal rule: a line of only -, * or _ (>=3, spaces allowed) -> drop
        // the markers and keep the terminating newline as a clean break.
        if (c == '-' || c == '*' || c == '_') {
          size_t e = t;
          int cnt = 0;
          bool only = true;
          bool sawNl = false;
          while (e < rawLen) {
            const uint8_t x = raw[e];
            if (x == '\n') { sawNl = true; break; }
            if (x == c) {
              cnt++;
            } else if (x != ' ' && x != '\t') {
              only = false;
              break;
            }
            e++;
          }
          const bool lineComplete = sawNl || (e >= rawLen && rawIsEof);
          if (only && cnt >= 3 && lineComplete) {
            s = e;  // stop before the '\n' (or at EOF); newline handled next pass
            continue;
          }
        }

        // List item: -, * or + then a space -> normalize to "- ".
        if ((c == '-' || c == '*' || c == '+') && t + 1 < rawLen && raw[t + 1] == ' ') {
          if (d + 2 > maxDisplay) break;  // emit the pair atomically -> srcMap stays monotonic
          emit('-', t);
          emit(' ', t + 1);
          colChars += 2;
          s = t + 2;
          continue;
        }

        // Blockquote: '>' (+ optional space) -> drop the marker.
        if (c == '>') {
          size_t e = t + 1;
          if (e < rawLen && raw[e] == ' ') e++;
          s = e;
          continue;
        }
        // no marker: fall through and emit from s (any indent is preserved)
      }
    }

    const uint8_t b = raw[s];

    // ---- newlines & whitespace ----
    if (b == '\n') {
      if (nlSinceText >= 2) { s++; continue; }  // collapse 3+ blank lines -> 1
      if (!emit('\n', s)) break;
      nlSinceText++;
      colChars = 0;
      s++;
      continue;
    }
    if (b == '\r') { s++; continue; }  // drop CR (CRLF -> LF)
    if (b == '\t') {                   // tab -> single space
      if (!emit(' ', s)) break;
      colChars++;
      s++;
      continue;
    }
    if (b < 0x20 || b == 0x7F) { s++; continue; }  // drop other control bytes

    // ---- printable ASCII ----
    if (b < 0x80) {
      if (markdown) {
        if (b == '<') {
          if (s + 1 < rawLen && isTagStartByte(raw[s + 1])) {
            size_t e = s + 1;
            const size_t limit = (s + 1 + kMaxTagLen < rawLen) ? s + 1 + kMaxTagLen : rawLen;
            while (e < limit && raw[e] != '>') e++;
            if (e < limit && raw[e] == '>') {  // complete tag
              if (isBrTag(raw, s, e) && nlSinceText < 2) {
                if (!emit('\n', s)) break;
                nlSinceText++;
                colChars = 0;
              }
              s = e + 1;
              continue;
            }
            if (e >= rawLen && !rawIsEof) {  // tag runs past the window edge
              if (s == 0 && d == 0) {        // no progress yet -> take '<' literally
                if (!emit('<', s)) break;
                nlSinceText = 0;
                colChars++;
                s++;
                continue;
              }
              break;  // defer the tag to the next page (nextOffset lands on '<')
            }
            if (rawIsEof && e >= rawLen) { s = rawLen; continue; }  // drop trailing junk at EOF
            if (!emit('<', s)) break;  // over-long "tag" -> treat '<' as literal
            nlSinceText = 0;
            colChars++;
            s++;
            continue;
          }
          if (s + 1 >= rawLen && !rawIsEof) {  // lone '<' at the window edge
            if (s == 0 && d == 0) {
              if (!emit('<', s)) break;
              nlSinceText = 0;
              colChars++;
              s++;
              continue;
            }
            break;  // defer
          }
          if (!emit('<', s)) break;  // stray '<' (not tag-like) -> literal
          nlSinceText = 0;
          colChars++;
          s++;
          continue;
        }
        if (b == '*' || b == '_' || b == '`') { s++; continue; }  // drop emphasis markers
      }
      if (!emit(static_cast<char>(b), s)) break;
      if (b != ' ') nlSinceText = 0;
      colChars++;
      s++;
      continue;
    }

    // ---- UTF-8 multibyte -> one ASCII char (or "..." for the ellipsis) ----
    size_t seqLen;
    if ((b & 0xE0) == 0xC0) {
      seqLen = 2;
    } else if ((b & 0xF0) == 0xE0) {
      seqLen = 3;
    } else if ((b & 0xF8) == 0xF0) {
      seqLen = 4;
    } else {
      seqLen = 1;  // invalid lead byte
    }
    if (seqLen == 1) {
      if (!emit('?', s)) break;
      nlSinceText = 0;
      colChars++;
      s++;
      continue;
    }
    if (s + seqLen > rawLen) {  // sequence split by the window edge
      if (!rawIsEof) {
        if (s == 0 && d == 0) {   // window too small to ever hold it -> resync by 1
          if (!emit('?', s)) break;
          nlSinceText = 0;
          colChars++;
          s++;
          continue;
        }
        break;  // defer the whole sequence to the next page
      }
      if (!emit('?', s)) break;  // truncated at real EOF -> a single '?'
      nlSinceText = 0;
      colChars++;
      s = rawLen;
      continue;
    }
    uint32_t cp = b & (seqLen == 2 ? 0x1F : seqLen == 3 ? 0x0F : 0x07);
    bool bad = false;
    for (size_t k = 1; k < seqLen; k++) {
      const uint8_t bk = raw[s + k];
      if ((bk & 0xC0) != 0x80) { bad = true; break; }
      cp = (cp << 6) | (bk & 0x3F);
    }
    if (bad) {                   // malformed -> one '?', resync by a single byte
      if (!emit('?', s)) break;
      nlSinceText = 0;
      colChars++;
      s++;
      continue;
    }
    const size_t sEnd = s + seqLen;
    if (cp == 0x2026) {             // horizontal ellipsis -> "..."
      if (d + 3 > maxDisplay) break;  // defer the whole group so the map stays sane
      emit('.', s);                 // first dot maps to the sequence start ...
      emit('.', sEnd);              // ... trailing dots map to the sequence END so a
      emit('.', sEnd);              // page break inside "..." still moves forward
      nlSinceText = 0;
      colChars += 3;
      s = sEnd;
      continue;
    }
    char ascii;
    switch (cp) {
      case 0x2014:  // em dash
      case 0x2013:  // en dash
        ascii = '-';
        break;
      case 0x2018:  // left single quote
      case 0x2019:  // right single quote / apostrophe
        ascii = '\'';
        break;
      case 0x201C:  // left double quote
      case 0x201D:  // right double quote
        ascii = '"';
        break;
      case 0x00A0:  // non-breaking space
        ascii = ' ';
        break;
      case 0x2022:  // bullet
        ascii = '-';
        break;
      default:  // any other code point -> a single '?'
        ascii = '?';
        break;
    }
    if (!emit(ascii, s)) break;
    if (ascii != ' ') nlSinceText = 0;
    colChars++;
    s = sEnd;
  }

  srcMap[d] = static_cast<uint16_t>(s);  // sentinel: source cursor past the last consumed byte
  *displayLenOut = d;
}

// A faithful replay of widgets::textBlock's greedy word wrap that COUNTS
// characters instead of drawing, stopping after `maxLines` lines. Returns how
// many chars of `text` fill those lines — the DISPLAY index at which the next
// page begins. `text` is the CLEANED page text (post-transform), so this count
// is not a file offset; readPage() maps it back to a raw byte offset through the
// source map. Kept byte-for-byte in step with ui/widgets/Widgets.cpp; if that
// wrap rule changes, this must change with it.
size_t wrapConsume(const char* text, uint16_t charsPerLine, uint16_t maxLines) {
  int cpl = charsPerLine;
  if (cpl <= 0) {
    return 0;
  }
  if (cpl > kWrapLineMax) {
    cpl = kWrapLineMax;
  }
  const char* p = text;
  uint16_t lines = 0;
  while (*p != '\0' && lines < maxLines) {
    int take = 0;
    int lastSpace = -1;
    while (p[take] != '\0' && p[take] != '\n' && take < cpl) {
      if (p[take] == ' ') {
        lastSpace = take;
      }
      take++;
    }
    int lineLen = take;
    if (p[take] != '\0' && p[take] != '\n' && lastSpace > 0) {
      lineLen = lastSpace;  // break at the last space that fits
    }
    lines++;
    p += lineLen;
    while (*p == ' ') {
      p++;
    }
    if (*p == '\n') {
      p++;
    }
  }
  return static_cast<size_t>(p - text);
}

}  // namespace

void BookService::begin(SdStorage* storage) {
  storage_ = storage;
  // Repair the index if a write was interrupted, before reading it.
  AtomicFile::cleanupSiblings(LittleFS, kPosPath);
  loadPositions();
}

void BookService::reload() {
  loadPositions();
}

void BookService::titleFromPath(const char* path, char* out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return;
  }
  String t(baseName(path));
  const int dot = t.lastIndexOf('.');
  if (dot > 0) {
    t.remove(dot);
  }
  t.replace('_', ' ');
  t.replace('-', ' ');
  t.trim();
  if (t.length() > 0) {
    t.setCharAt(0, toupper(t.charAt(0)));
  }
  strncpy(out, t.c_str(), outSize - 1);
  out[outSize - 1] = '\0';
}

int BookService::findPosition(const char* path) const {
  for (size_t i = 0; i < posCount_; i++) {
    if (posPaths_[i].equals(path)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

uint32_t BookService::position(const char* path) const {
  const int at = findPosition(path);
  return at >= 0 ? posOffsets_[at] : 0;
}

void BookService::loadPositions() {
  posCount_ = 0;
  fs::File f = LittleFS.open(kPosPath, FILE_READ);
  if (!f) {
    return;
  }
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.printf(
        "[reader] %s is corrupt (%s) — resume positions reset; the next page "
        "turn overwrites it\n",
        kPosPath, err.c_str());
    return;
  }
  for (JsonPair kv : doc.as<JsonObject>()) {
    if (posCount_ >= kMaxRemembered) {
      break;
    }
    posPaths_[posCount_] = kv.key().c_str();
    posOffsets_[posCount_] = kv.value().as<uint32_t>();
    posCount_++;
  }
}

bool BookService::savePositions() {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  for (size_t i = 0; i < posCount_; i++) {
    obj[posPaths_[i].c_str()] = posOffsets_[i];
  }
  String body;
  serializeJson(doc, body);
  return AtomicFile::writeAll(LittleFS, kPosPath, body);
}

void BookService::setPosition(const char* path, uint32_t offset) {
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  const int at = findPosition(path);
  if (at >= 0) {
    if (posOffsets_[at] == offset) {
      return;  // unchanged — spare the flash a needless rewrite
    }
    posOffsets_[at] = offset;
    savePositions();
    return;
  }
  if (posCount_ >= kMaxRemembered) {
    // FIFO-evict the oldest so a freshly opened book is always remembered.
    // Refusing (as NotesService does for favourites) would silently drop the
    // reader's current place, which is worse than forgetting an old book.
    for (size_t i = 0; i + 1 < posCount_; i++) {
      posPaths_[i] = posPaths_[i + 1];
      posOffsets_[i] = posOffsets_[i + 1];
    }
    posCount_ = kMaxRemembered - 1;
  }
  posPaths_[posCount_] = path;
  posOffsets_[posCount_] = offset;
  posCount_++;
  savePositions();
}

void BookService::clearPosition(const char* path) {
  const int at = findPosition(path);
  if (at < 0) {
    return;
  }
  for (size_t i = static_cast<size_t>(at); i + 1 < posCount_; i++) {
    posPaths_[i] = posPaths_[i + 1];
    posOffsets_[i] = posOffsets_[i + 1];
  }
  posCount_--;
  savePositions();
}

size_t BookService::list(BookInfo* out, size_t maxBooks, size_t* totalOut) {
  if (totalOut != nullptr) {
    *totalOut = 0;
  }
  if (out == nullptr || maxBooks == 0 || storage_ == nullptr ||
      storage_->card() == nullptr || !storage_->card()->mounted()) {
    return 0;
  }
  fs::File dir = SD_MMC.open(paths::kDocuments);
  if (!dir || !dir.isDirectory()) {
    if (dir) {
      dir.close();
    }
    return 0;  // directory missing (unwritable card at mount) — honest empty
  }

  size_t count = 0;
  size_t total = 0;
  for (fs::File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory() && isBookFile(entry.name())) {
      // Counted even past the window: the walk is cheap next to a read, and
      // without the true total the UI cannot say how much it is hiding.
      total++;
      if (count < maxBooks) {
        BookInfo& b = out[count];
        // entry.name() is a basename on this core (3.3.8) — same assumption as
        // NotesService::list — so rebuild the full path from the known dir.
        const String full = String(paths::kDocuments) + "/" + entry.name();
        strncpy(b.path, full.c_str(), sizeof(b.path) - 1);
        b.path[sizeof(b.path) - 1] = '\0';
        titleFromPath(b.path, b.title, sizeof(b.title));
        b.sizeBytes = static_cast<uint32_t>(entry.size());
        b.resumeOffset = position(b.path);
        count++;
      }
    }
    entry.close();
  }
  dir.close();

  if (totalOut != nullptr) {
    *totalOut = total;
  }
  return count;
}

uint32_t BookService::fileSize(const char* path) {
  String safe;
  if (storage_ == nullptr || storage_->card() == nullptr || !storage_->card()->mounted() ||
      !storage_->sanitizePath(path, safe)) {
    return 0;
  }
  fs::File f = SD_MMC.open(safe, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) {
      f.close();
    }
    return 0;
  }
  const uint32_t size = static_cast<uint32_t>(f.size());
  f.close();
  return size;
}

size_t BookService::readPage(const char* path, uint32_t startOffset, uint16_t charsPerLine,
                             uint16_t maxLines, char* out, size_t outCap, uint32_t& nextOffset,
                             bool& atEnd) {
  nextOffset = startOffset;
  atEnd = false;
  if (out == nullptr || outCap == 0) {
    return 0;
  }
  out[0] = '\0';
  if (charsPerLine == 0 || maxLines == 0) {
    return 0;
  }

  String safe;
  if (storage_ == nullptr || storage_->card() == nullptr || !storage_->card()->mounted() ||
      !storage_->sanitizePath(path, safe)) {
    return 0;  // read failure (atEnd stays false)
  }
  fs::File f = SD_MMC.open(safe, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) {
      f.close();
    }
    return 0;  // read failure
  }
  const uint32_t size = static_cast<uint32_t>(f.size());
  if (startOffset >= size) {
    f.close();
    atEnd = true;
    return 0;  // clean end of file (empty page)
  }

  const bool markdown = isMarkdownPath(path);
  size_t cap = outCap - 1;  // most display chars we may emit into the page buffer
  if (cap > kScratchCap - 1) {
    cap = kScratchCap - 1;
  }

  // A page turn skips any run of windows that clean to nothing visible (e.g. a
  // large embedded tag) so it always lands on real text or a real EOF, never on
  // a false "couldn't read" from an all-markup window. Bounded by kMaxSkipChunks.
  uint32_t pageStart = startOffset;
  for (int iter = 0; iter < kMaxSkipChunks; iter++) {
    if (pageStart >= size) {
      f.close();
      atEnd = true;
      nextOffset = size;
      return 0;  // clean end of file after skipping trailing markup
    }
    if (!f.seek(pageStart)) {
      f.close();
      nextOffset = startOffset;
      return 0;  // read failure
    }
    const uint32_t remain = size - pageStart;
    const size_t want = remain < static_cast<uint32_t>(kScratchCap - 1)
                            ? static_cast<size_t>(remain)
                            : (kScratchCap - 1);
    const size_t nRead = f.readBytes(reinterpret_cast<char*>(sRawBuf), want);
    if (nRead == 0) {
      f.close();
      nextOffset = startOffset;
      return 0;  // read failure
    }
    const bool rawIsEof = (pageStart + static_cast<uint32_t>(nRead)) >= size;

    // Clean the window into `out`, tracking each display char's source byte.
    size_t displayLen = 0;
    cleanTransform(sRawBuf, nRead, rawIsEof, markdown, out, cap, sSrcMap, &displayLen);
    out[displayLen] = '\0';

    // Wrap the CLEANED text to find where this page ends (a display index)...
    const size_t consumed = wrapConsume(out, charsPerLine, maxLines);
    if (consumed == 0) {
      // Nothing visible in this window — advance past it and try the next one.
      const uint32_t adv = sSrcMap[displayLen];  // final source cursor (relative)
      if (adv == 0) {
        f.close();  // consumed no source at all — bail rather than spin
        nextOffset = pageStart;
        atEnd = pageStart >= size;
        return 0;
      }
      pageStart += adv;
      continue;
    }

    // ...then map that display index back to a RAW byte offset for the next page.
    out[consumed] = '\0';  // hand back exactly this page's cleaned text
    f.close();
    nextOffset = pageStart + sSrcMap[consumed];
    atEnd = nextOffset >= size;
    return consumed;
  }

  // Pathological: too many all-markup windows in a row. Advance so we never spin.
  f.close();
  nextOffset = pageStart;
  atEnd = pageStart >= size;
  return 0;
}
