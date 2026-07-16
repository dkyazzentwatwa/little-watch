#pragma once

// SD card directory contract (spec §30) plus the CrowPanel writer-deck
// interop tree. All user content lives under kRoot; the deck tree is the
// only other allowed root (SD-swap note sharing).
namespace paths {

constexpr const char* kRoot = "/littlecube";
constexpr const char* kNotesText = "/littlecube/notes/text";
constexpr const char* kNotesAudio = "/littlecube/notes/audio";
constexpr const char* kRecordings = "/littlecube/recordings";
constexpr const char* kMusic = "/littlecube/music";
constexpr const char* kPodcastFeeds = "/littlecube/podcasts/feeds";
constexpr const char* kPodcastDownloads = "/littlecube/podcasts/downloads";
constexpr const char* kRadio = "/littlecube/radio";
constexpr const char* kCalendar = "/littlecube/calendar";
constexpr const char* kContacts = "/littlecube/contacts";
constexpr const char* kDocuments = "/littlecube/documents";
constexpr const char* kExports = "/littlecube/exports";
constexpr const char* kBackups = "/littlecube/backups";
constexpr const char* kCache = "/littlecube/cache";
constexpr const char* kSystemIndexes = "/littlecube/system/indexes";
constexpr const char* kSystemRecovery = "/littlecube/system/recovery";

// CrowPanel writer deck (cypher-desk) interop: clean .md/.txt bodies, no
// frontmatter; tolerate stray .tmp/.bak siblings; notebooks one level deep.
constexpr const char* kDeckRoot = "/cypher-puter/desk";
constexpr const char* kDeckNotes = "/cypher-puter/desk/notes";

}  // namespace paths
