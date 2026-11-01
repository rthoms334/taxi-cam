#pragma once

namespace taxi_camera::standalone {
// The English wording is the key, so a missing or outdated entry shows English.
// {0}..{9} are values the companion fills in; a translation may reorder them.
struct Translation {
  const wchar_t* english;
  const wchar_t* text;
};
}  // namespace taxi_camera::standalone
