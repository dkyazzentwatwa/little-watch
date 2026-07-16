#pragma once

class AppRouter;
struct Services;

// Constructs the 12 apps (static storage, never destroyed) and registers
// them with the router. Called once from the kernel during boot.
void registerApps(AppRouter& router, Services& services);
