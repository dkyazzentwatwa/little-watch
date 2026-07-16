#pragma once

// System event bus vocabulary (spec §38). Services publish these; UI and
// other services subscribe instead of poking each other directly.
enum class SystemEvent {
  WifiScanStarted,
  WifiScanCompleted,
  WifiConnecting,
  WifiConnected,
  WifiDisconnected,
  InternetAvailable,
  InternetUnavailable,

  SdInserted,
  SdMounted,
  SdRemoved,
  SdFull,
  SdError,

  RecordingStarted,
  RecordingPaused,
  RecordingStopped,

  AudioStarted,
  AudioPaused,
  AudioStopped,

  AlarmTriggered,
  TimerCompleted,

  SerialCommandReceived,
  BackupStarted,
  BackupCompleted,
  BackupFailed,

  WeatherUpdated,
  TimeSynced,
  SettingsChanged,
};
