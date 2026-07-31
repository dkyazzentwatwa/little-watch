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

class AssistantService;
class SettingsService;
class WifiService;
class ProvisioningService;
class TimeService;
class WeatherService;
class NotesService;
class CalendarService;
class ContactsService;
class RecorderService;
class BookService;
class MusicService;
class NewsService;
class PodcastService;
class RadioService;
class VideoService;
class VideoPlayer;

class SerialCommandService;

class AmoledProtection;

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
  NewsService* news = nullptr;
  NotesService* notes = nullptr;
  CalendarService* calendar = nullptr;
  ContactsService* contacts = nullptr;
  RecorderService* recorder = nullptr;
  AssistantService* assistant = nullptr;
  BookService* books = nullptr;
  MusicService* music = nullptr;
  PodcastService* podcasts = nullptr;
  RadioService* radio = nullptr;
  VideoService* video = nullptr;
  VideoPlayer* videoPlayer = nullptr;

  SerialCommandService* serial = nullptr;

  // Apps read shiftX()/shiftY() from this on every render of persistent
  // chrome — burn-in defense is a hard requirement (spec §37).
  AmoledProtection* amoled = nullptr;

  EventBus* events = nullptr;
  AppRouter* router = nullptr;
  SystemState* state = nullptr;
};
