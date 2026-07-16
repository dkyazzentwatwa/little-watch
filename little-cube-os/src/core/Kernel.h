#pragma once

// Boot and main loop. The .ino delegates here; everything the firmware owns
// is constructed and wired in Kernel.cpp.
void kernelSetup();
void kernelLoop();
