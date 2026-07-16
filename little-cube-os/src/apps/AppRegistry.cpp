#include "AppRegistry.h"

#include "../core/AppRouter.h"
#include "AudioApp.h"
#include "CalculatorApp.h"
#include "CalendarApp.h"
#include "ClockApp.h"
#include "ContactsApp.h"
#include "FilesApp.h"
#include "HomeApp.h"
#include "NotesApp.h"
#include "RecorderApp.h"
#include "SettingsApp.h"
#include "TodayApp.h"
#include "WeatherApp.h"

void registerApps(AppRouter& router, Services& services) {
  static HomeApp home(services);
  static TodayApp today(services);
  static ClockApp clock(services);
  static WeatherApp weather(services);
  static CalendarApp calendar(services);
  static NotesApp notes(services);
  static RecorderApp recorder(services);
  static AudioApp audio(services);
  static FilesApp files(services);
  static ContactsApp contacts(services);
  static CalculatorApp calculator(services);
  static SettingsApp settings(services);

  router.registerApp(AppId::Home, &home);
  router.registerApp(AppId::Today, &today);
  router.registerApp(AppId::Clock, &clock);
  router.registerApp(AppId::Weather, &weather);
  router.registerApp(AppId::Calendar, &calendar);
  router.registerApp(AppId::Notes, &notes);
  router.registerApp(AppId::Recorder, &recorder);
  router.registerApp(AppId::Audio, &audio);
  router.registerApp(AppId::Files, &files);
  router.registerApp(AppId::Contacts, &contacts);
  router.registerApp(AppId::Calculator, &calculator);
  router.registerApp(AppId::Settings, &settings);
}
