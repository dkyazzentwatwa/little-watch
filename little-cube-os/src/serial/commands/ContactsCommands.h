#pragma once

// Contacts serial command family — registered with SerialCommandService.
// TODO(task-10/18): implementation.

struct Services;
class SerialCommandService;

void registerContactsCommands(SerialCommandService& serial, Services& services);
