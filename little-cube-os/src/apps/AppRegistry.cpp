#include "AppRegistry.h"

#include "../core/AppRouter.h"
#include "../feature_flags.h"
#include "AssistantApp.h"
#include "AudioApp.h"
#include "CalculatorApp.h"
#include "CalendarApp.h"
#include "ClockApp.h"
#include "ContactsApp.h"
#include "FilesApp.h"
#include "HomeApp.h"
#include "NewsApp.h"
#include "NotesApp.h"
#include "ReaderApp.h"
#include "RecorderApp.h"
#include "SettingsApp.h"
#include "TodayApp.h"
#include "WeatherApp.h"
#if FEATURE_VIDEO
#include "VideoApp.h"
#endif

void registerApps(AppRouter& router, Services& services) {
  static HomeApp home(services);
  static TodayApp today(services);
  static ClockApp clock(services);
  static WeatherApp weather(services);
  static CalendarApp calendar(services);
  static NotesApp notes(services);
  static RecorderApp recorder(services);
  static AssistantApp assistant(services);
  static AudioApp audio(services);
  static FilesApp files(services);
  static ContactsApp contacts(services);
  static CalculatorApp calculator(services);
  static SettingsApp settings(services);
  static ReaderApp reader(services);
  static NewsApp news(services);
#if FEATURE_VIDEO
  static VideoApp video(services);
  router.registerApp(AppId::Video, &video);
#endif

  router.registerApp(AppId::Home, &home);
  router.registerApp(AppId::Today, &today);
  router.registerApp(AppId::Clock, &clock);
  router.registerApp(AppId::Weather, &weather);
  router.registerApp(AppId::Calendar, &calendar);
  router.registerApp(AppId::Notes, &notes);
  router.registerApp(AppId::Recorder, &recorder);
  router.registerApp(AppId::Assistant, &assistant);
  router.registerApp(AppId::Audio, &audio);
  router.registerApp(AppId::Files, &files);
  router.registerApp(AppId::Contacts, &contacts);
  router.registerApp(AppId::Calculator, &calculator);
  router.registerApp(AppId::Settings, &settings);
  router.registerApp(AppId::Reader, &reader);
  router.registerApp(AppId::News, &news);
}
