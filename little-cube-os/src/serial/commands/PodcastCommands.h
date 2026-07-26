#pragma once

struct Services;

// `podcast ...` family (spec §24). Subscribe to RSS feeds and download the
// latest episode onto the SD card so it plays through Audio -> Podcasts.
//   podcast feeds            list subscribed feeds (ids below)
//   podcast add <url>        subscribe to an RSS feed
//   podcast remove <id>      unsubscribe
//   podcast fetch [id|url]   download the latest episode (default feed 0)
//   podcast status           current download status
//   podcast list             downloaded episodes on the card
bool handlePodcastCommand(Services& services, const char* verb, char* args);
void printPodcastHelp();
