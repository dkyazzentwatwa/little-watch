#pragma once

// Calendar serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerCalendarCommands(SerialCommandService& serial, Services& services);
