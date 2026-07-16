// Little Cube OS - a calm, Wi-Fi-enabled personal information device for
// the Waveshare ESP32-S3-Touch-AMOLED-1.8 cube.
//
// This .ino is intentionally code-free. The Arduino sketch preprocessor
// (ctags prototype generation) on some setups mangles prototypes for
// functions defined in .ino files, producing firmware that recurses in
// setup(). setup() and loop() are therefore ordinary C++ definitions in
// src/core/Kernel.cpp, which the Arduino core links and calls as usual.
//
// Build: scripts/build.sh   Flash: scripts/upload.sh   Docs: docs/
