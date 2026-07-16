#pragma once

// Service locator handed to every app and serial command family. Wired once
// by the Kernel; everything is a non-owning pointer with Kernel lifetime.
// Forward declarations only — include the specific header where you use one.

class DisplayAdapter;
class InputAdapter;
class SdCardAdapter;
class RtcAdapter;
class BatteryAdapter;
class AudioAdapter;

class SdStorage;

class SettingsService;
class WifiService;
class ProvisioningService;
class TimeService;
class WeatherService;
class NotesService;
class RecorderService;

class SerialCommandService;

class EventBus;
class AppRouter;
struct SystemState;

struct Services {
  DisplayAdapter* display = nullptr;
  InputAdapter* input = nullptr;
  SdCardAdapter* sdCard = nullptr;
  RtcAdapter* rtc = nullptr;
  BatteryAdapter* battery = nullptr;
  AudioAdapter* audio = nullptr;

  SdStorage* storage = nullptr;

  SettingsService* settings = nullptr;
  WifiService* wifi = nullptr;
  ProvisioningService* provisioning = nullptr;
  TimeService* time = nullptr;
  WeatherService* weather = nullptr;
  NotesService* notes = nullptr;
  RecorderService* recorder = nullptr;

  SerialCommandService* serial = nullptr;

  EventBus* events = nullptr;
  AppRouter* router = nullptr;
  SystemState* state = nullptr;
};
