#include "AtomicFile.h"

#include <FS.h>

// Protocol proven on the CrowPanel writer deck (same .tmp/.bak siblings, so
// swapped cards look familiar to both devices):
//   1. write <path>.tmp fully, flush, close
//   2. re-open and verify the byte count
//   3. rename existing target to <path>.bak
//   4. rename .tmp -> target
//   5. delete .bak (roll back from it if step 4 fails)

namespace AtomicFile {

namespace {
String sibling(const char* path, const char* suffix) {
  String s(path);
  s += suffix;
  return s;
}
}  // namespace

bool writeAll(fs::FS& fs, const char* path, const uint8_t* data, size_t length) {
  if (path == nullptr || (data == nullptr && length > 0)) {
    return false;
  }
  const String tmp = sibling(path, ".tmp");
  const String bak = sibling(path, ".bak");

  fs.remove(tmp);
  fs::File out = fs.open(tmp, FILE_WRITE);
  if (!out) {
    return false;
  }
  const size_t written = length > 0 ? out.write(data, length) : 0;
  out.flush();
  out.close();
  if (written != length) {
    fs.remove(tmp);
    return false;
  }

  fs::File verify = fs.open(tmp, FILE_READ);
  const bool sizeOk = verify && verify.size() == length;
  if (verify) {
    verify.close();
  }
  if (!sizeOk) {
    fs.remove(tmp);
    return false;
  }

  const bool hadOld = fs.exists(path);
  if (hadOld) {
    fs.remove(bak);
    if (!fs.rename(path, bak)) {
      fs.remove(tmp);
      return false;
    }
  }
  if (!fs.rename(tmp, path)) {
    if (hadOld) {
      fs.rename(bak, path);  // roll back
    }
    fs.remove(tmp);
    return false;
  }
  if (hadOld) {
    fs.remove(bak);
  }
  return true;
}

bool writeAll(fs::FS& fs, const char* path, const String& text) {
  return writeAll(fs, path, reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
}

void cleanupSiblings(fs::FS& fs, const char* path) {
  const String tmp = sibling(path, ".tmp");
  const String bak = sibling(path, ".bak");
  fs.remove(tmp);
  if (fs.exists(bak)) {
    if (!fs.exists(path)) {
      // Interrupted between steps 3 and 4: the .bak is the real content.
      fs.rename(bak, path);
    } else {
      fs.remove(bak);
    }
  }
}

String partialPath(const char* path) {
  return sibling(path, ".partial");
}

bool finalizePartial(fs::FS& fs, const char* path) {
  const String partial = partialPath(path);
  if (!fs.exists(partial)) {
    return false;
  }
  fs.remove(path);
  return fs.rename(partial, path);
}

}  // namespace AtomicFile
