#pragma once

#include <stddef.h>

enum class AnnounceResponseStatus {
  Parsed,
  InvalidJson,
  MissingAdded
};

AnnounceResponseStatus parseAnnounceAdded(
  const char* payload,
  size_t payloadLength,
  bool& added
);
