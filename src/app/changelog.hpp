#pragma once
#include <windows.h>
#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "../shared/ui_text.hpp"

namespace taxi_camera::standalone {
// "What's new" notes live in changelog.json at the repository root. The
// companion fetches that file from main when What's new opens and shows the
// entries at or below the installed version, newest first. A feature's picture
// is a file under docs/whats-new/ in the same repository, fetched the same way.
inline constexpr std::size_t kChangelogMaxBytes = 64 * 1024, kChangelogMaxReleases = 64, kChangelogMaxChanges = 32, kChangelogMaxText = 400,
                             kChangelogMaxFeatures = 8, kChangelogMaxTitle = 80, kChangelogMaxImagePath = 120;
struct ChangelogVersion {
  std::uint32_t major{}, minor{}, patch{};
  friend bool operator==(const ChangelogVersion&, const ChangelogVersion&) = default;
  friend auto operator<=>(const ChangelogVersion&, const ChangelogVersion&) = default;
};
// A new feature: a short title, a sentence or two, and optionally a picture.
struct ChangelogFeature {
  std::string title, text;
  std::string image;  // Repository path under docs/whats-new/, or empty.
};
struct ChangelogRelease {
  ChangelogVersion version;
  std::string date;  // Optional YYYY-MM-DD.
  std::vector<ChangelogFeature> features;
  std::vector<std::string> fixes;
  std::vector<std::string> changes;  // Other notes (every release before 1.2.0).
};
// A feature picture's repository path: docs/whats-new/..., a .png or .jpg
// file name of letters, digits, '.', '-', '_' and '/' separators.
inline bool valid_whats_new_image(std::string_view path) {
  constexpr std::string_view prefix = "docs/whats-new/";
  if (path.size() > kChangelogMaxImagePath || path.size() <= prefix.size() || path.substr(0, prefix.size()) != prefix ||
      path.find("..") != std::string_view::npos || path.find("//") != std::string_view::npos)
    return false;
  for (const char c : path)
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/'))
      return false;
  const auto ends = [&](std::string_view suffix) {
    return path.size() > suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
  };
  return ends(".png") || ends(".jpg") || ends(".jpeg");
}

// Strict major.minor.patch with 16-bit components, matching Windows version resources.
inline bool parse_changelog_version(std::string_view text, ChangelogVersion& value) {
  std::array<std::uint32_t, 3> parts{};
  std::size_t at = 0;
  for (unsigned i = 0; i < 3; ++i) {
    if (i && (at >= text.size() || text[at++] != '.'))
      return false;
    const auto start = at;
    std::uint32_t number = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9') {
      number = number * 10 + static_cast<std::uint32_t>(text[at++] - '0');
      if (number > 65535)
        return false;
    }
    if (at == start || (at - start > 1 && text[start] == '0'))
      return false;
    parts[i] = number;
  }
  if (at != text.size())
    return false;
  value = {parts[0], parts[1], parts[2]};
  return true;
}
inline std::wstring changelog_version_text(const ChangelogVersion& v) {
  return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." + std::to_wstring(v.patch);
}

namespace changelog_detail {
struct Reader {
  std::string_view text;
  std::size_t at{};
  const char* error{};

  bool fail(const char* message) {
    if (!error)
      error = message;
    return false;
  }
  void space() {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n'))
      ++at;
  }
  bool take(char c) {
    space();
    if (at < text.size() && text[at] == c) {
      ++at;
      return true;
    }
    return false;
  }
  bool expect(char c, const char* message) { return take(c) || fail(message); }
  bool hex4(std::uint32_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (at >= text.size())
        return false;
      const char c = text[at++];
      value <<= 4;
      if (c >= '0' && c <= '9')
        value |= static_cast<std::uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        value |= static_cast<std::uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        value |= static_cast<std::uint32_t>(c - 'A' + 10);
      else
        return false;
    }
    return true;
  }
  static void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
      out += static_cast<char>(0x80 | (cp & 0x3f));
    }
  }
  bool string(std::string& out, std::size_t limit) {
    space();
    if (at >= text.size() || text[at] != '"')
      return fail("Expected a string.");
    ++at;
    out.clear();
    while (at < text.size()) {
      const auto c = static_cast<unsigned char>(text[at++]);
      if (c == '"') {
        // Reject invalid UTF-8 here so display conversion cannot drop text.
        if (!out.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, out.data(), static_cast<int>(out.size()), nullptr, 0))
          return fail("Text is not valid UTF-8.");
        return true;
      }
      if (c < 0x20)
        return fail("Control character in text.");
      if (c != '\\') {
        out += static_cast<char>(c);
      } else {
        if (at >= text.size())
          break;
        const char e = text[at++];
        std::uint32_t cp{};
        switch (e) {
          case '"':
          case '\\':
          case '/':
            out += e;
            break;
          case 'b':
            out += '\b';
            break;
          case 'f':
            out += '\f';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
            if (!hex4(cp))
              return fail("Invalid \\u escape.");
            if (cp >= 0xdc00 && cp <= 0xdfff)
              return fail("Unpaired surrogate escape.");
            if (cp >= 0xd800 && cp <= 0xdbff) {
              std::uint32_t low{};
              if (at + 1 >= text.size() || text[at] != '\\' || text[at + 1] != 'u')
                return fail("Unpaired surrogate escape.");
              at += 2;
              if (!hex4(low) || low < 0xdc00 || low > 0xdfff)
                return fail("Unpaired surrogate escape.");
              cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            }
            append_utf8(out, cp);
            break;
          default:
            return fail("Invalid escape in text.");
        }
      }
      if (out.size() > limit)
        return fail("Text is too long.");
    }
    return fail("Unterminated string.");
  }
  // Calls member(key) for each member; member consumes the value.
  template <class Member>
  bool object(Member&& member) {
    if (!expect('{', "Expected an object."))
      return false;
    if (take('}'))
      return true;
    do {
      std::string key;
      if (!string(key, 32) || !expect(':', "Expected ':'.") || !member(key))
        return false;
    } while (take(','));
    return expect('}', "Expected ',' or '}'.");
  }
  template <class Item>
  bool array(Item&& item) {
    if (!expect('[', "Expected an array."))
      return false;
    if (take(']'))
      return true;
    do {
      if (!item())
        return false;
    } while (take(','));
    return expect(']', "Expected ',' or ']'.");
  }
  // One value of any kind, nested a few levels at most: a field a newer
  // changelog added, skipped when parsing leniently.
  bool skip(unsigned depth = 0) {
    space();
    if (depth > 8 || at >= text.size())
      return fail("Expected a value.");
    const char c = text[at];
    if (c == '"') {
      std::string ignored;
      return string(ignored, kChangelogMaxBytes);
    }
    if (c == '{')
      return object([&](const std::string&) { return skip(depth + 1); });
    if (c == '[')
      return array([&] { return skip(depth + 1); });
    // true, false, null or a number.
    const auto start = at;
    while (at < text.size() && ((text[at] >= 'a' && text[at] <= 'z') || (text[at] >= '0' && text[at] <= '9') || text[at] == '-' ||
                                text[at] == '+' || text[at] == '.' || text[at] == 'E'))
      ++at;
    return at > start || fail("Expected a value.");
  }
};
inline bool valid_date(std::string_view d) {
  if (d.size() != 10 || d[4] != '-' || d[7] != '-')
    return false;
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u})
    if (d[i] < '0' || d[i] > '9')
      return false;
  const int month = (d[5] - '0') * 10 + (d[6] - '0'), day = (d[8] - '0') * 10 + (d[9] - '0');
  return month >= 1 && month <= 12 && day >= 1 && day <= 31;
}
}  // namespace changelog_detail

// Schema: {"releases": [{"version": "1.2.0", "date": "2026-11-01",
//   "features": [{"title": "...", "text": "...", "image": "docs/whats-new/1.2.0/x.png"}],
//   "fixes": ["..."], "changes": ["..."]}]}
// Releases are newest first with strictly descending versions and at least one
// feature, fix or change. Strict parsing (the repository's own check) refuses
// unknown keys; the companion parses leniently and skips them, so a field a
// later changelog adds cannot hide the notes from an installed version.
enum class ChangelogParse { strict, lenient };
inline bool parse_changelog(std::string_view text,
                            std::vector<ChangelogRelease>& releases,
                            const char** error = nullptr,
                            ChangelogParse mode = ChangelogParse::strict) {
  changelog_detail::Reader r{text};
  std::vector<ChangelogRelease> parsed;
  const auto finish = [&](bool ok) {
    if (error)
      *error = ok ? nullptr : (r.error ? r.error : "Invalid changelog.");
    if (ok)
      releases = std::move(parsed);
    return ok;
  };
  const auto unknown = [&](const char* message) { return mode == ChangelogParse::lenient ? r.skip() : r.fail(message); };
  if (text.size() > kChangelogMaxBytes)
    return finish(r.fail("Changelog is too large."));
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
    r.at = 3;
  // A list of 1-32 non-empty notes of at most 400 characters.
  const auto notes = [&](std::vector<std::string>& out) {
    return r.array([&] {
      std::string value;
      if (out.size() >= kChangelogMaxChanges)
        return r.fail("Too many changes in one release.");
      if (!r.string(value, kChangelogMaxText))
        return false;
      if (value.find_first_not_of(" \t\r\n") == std::string::npos)
        return r.fail("Change text is empty.");
      out.push_back(std::move(value));
      return true;
    });
  };
  const auto feature = [&](ChangelogRelease& entry) {
    if (entry.features.size() >= kChangelogMaxFeatures)
      return r.fail("Too many features in one release.");
    ChangelogFeature item;
    bool have_title = false, have_text = false, have_image = false;
    const bool ok = r.object([&](const std::string& key) {
      if (key == "title" && !have_title) {
        have_title = true;
        return r.string(item.title, kChangelogMaxTitle);
      }
      if (key == "text" && !have_text) {
        have_text = true;
        return r.string(item.text, kChangelogMaxText);
      }
      if (key == "image" && !have_image) {
        have_image = true;
        return (r.string(item.image, kChangelogMaxImagePath) && valid_whats_new_image(item.image)) ||
               r.fail("A feature image is a .png or .jpg path under docs/whats-new/.");
      }
      return unknown("Unknown or repeated feature field.");
    });
    if (!ok)
      return false;
    if (item.title.find_first_not_of(" \t\r\n") == std::string::npos || item.text.find_first_not_of(" \t\r\n") == std::string::npos)
      return r.fail("Each feature needs a title and text.");
    entry.features.push_back(std::move(item));
    return true;
  };
  bool have_releases = false;
  const auto release = [&] {
    if (parsed.size() >= kChangelogMaxReleases)
      return r.fail("Too many releases.");
    ChangelogRelease entry;
    bool have_version = false, have_date = false, have_features = false, have_fixes = false, have_changes = false;
    const bool ok = r.object([&](const std::string& key) {
      std::string value;
      if (key == "version" && !have_version) {
        have_version = true;
        return (r.string(value, 32) && parse_changelog_version(value, entry.version)) || r.fail("Version must be major.minor.patch.");
      }
      if (key == "date" && !have_date) {
        have_date = true;
        return (r.string(entry.date, 10) && changelog_detail::valid_date(entry.date)) || r.fail("Date must be YYYY-MM-DD.");
      }
      if (key == "features" && !have_features) {
        have_features = true;
        return r.array([&] { return feature(entry); });
      }
      if (key == "fixes" && !have_fixes) {
        have_fixes = true;
        return notes(entry.fixes);
      }
      if (key == "changes" && !have_changes) {
        have_changes = true;
        return notes(entry.changes);
      }
      return unknown("Unknown or repeated release field.");
    });
    if (!ok)
      return false;
    if (!have_version || (entry.features.empty() && entry.fixes.empty() && entry.changes.empty()))
      return r.fail("Each release needs a version and at least one feature, fix or change.");
    if (!parsed.empty() && !(entry.version < parsed.back().version))
      return r.fail("Releases must be newest first with unique versions.");
    parsed.push_back(std::move(entry));
    return true;
  };
  const bool ok = r.object([&](const std::string& key) {
    if (key != "releases" || have_releases)
      return have_releases && key == "releases" ? r.fail("Unknown or repeated top-level field.")
                                                : unknown("Unknown or repeated top-level field.");
    have_releases = true;
    return r.array(release);
  });
  if (!ok)
    return finish(false);
  r.space();
  if (r.at != text.size())
    return finish(r.fail("Unexpected text after the changelog."));
  if (parsed.empty())
    return finish(r.fail("Changelog has no releases."));
  return finish(true);
}

// main can carry notes for merged builds that are not published yet.
inline std::vector<ChangelogRelease> releases_up_to(const std::vector<ChangelogRelease>& releases, const ChangelogVersion& installed) {
  std::vector<ChangelogRelease> shown;
  for (const auto& release : releases)
    if (release.version <= installed)
      shown.push_back(release);
  return shown;
}

inline std::wstring changelog_widen(const std::string& text) {
  if (text.empty())
    return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
  if (n > 0)
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
  return out;
}
// "2026-11-01" as "1 November 2026".
inline std::wstring changelog_date_text(const std::string& date) {
  static constexpr const wchar_t* months[]{L"January", L"February", L"March",     L"April",   L"May",      L"June",
                                           L"July",    L"August",   L"September", L"October", L"November", L"December"};
  if (!changelog_detail::valid_date(date))
    return changelog_widen(date);
  const int month = (date[5] - '0') * 10 + (date[6] - '0'), day = (date[8] - '0') * 10 + (date[9] - '0');
  return std::to_wstring(day) + L" " + months[month - 1] + L" " + changelog_widen(date.substr(0, 4));
}
// What's new top to bottom: the newest release shown in full (its features
// with their pictures, then fixes and other changes), then earlier releases
// as compact lists.
struct WhatsNewBlock {
  enum class Kind {
    heading,  // text: "Taxi Cam 1.2.0", detail: its date.
    section,  // text: "New features", "Fixes", "Other changes" or "Earlier versions".
    feature,  // text: title, detail: text, image: repository path or empty.
    bullet,   // text: one fix or change.
    release,  // An earlier release: text "Taxi Cam 1.1.3", detail: its date.
    note,     // text: a message instead of notes.
  } kind;
  std::wstring text, detail;
  std::string image;
  // Interface wording around the notes, translated where it is shown; text is
  // this English wording with {0} filled from value. Null for the notes.
  const wchar_t* wording{};
  std::wstring value{};
};
inline std::vector<WhatsNewBlock> whats_new_blocks(const std::vector<ChangelogRelease>& shown, const ChangelogVersion& installed) {
  using Kind = WhatsNewBlock::Kind;
  std::vector<WhatsNewBlock> blocks;
  const auto chrome = [](Kind kind, const wchar_t* wording, std::wstring value = {}) {
    std::wstring text = wording;
    if (const auto at = text.find(L"{0}"); at != std::wstring::npos)
      text.replace(at, 3, value);
    return WhatsNewBlock{kind, text, {}, {}, wording, std::move(value)};
  };
  if (shown.empty()) {
    blocks.push_back(chrome(Kind::note, ui_text(L"No notes are listed for Taxi Cam {0} yet."), changelog_version_text(installed)));
    return blocks;
  }
  for (std::size_t i = 0; i < shown.size(); ++i) {
    const auto& release = shown[i];
    const auto version = L"Taxi Cam " + changelog_version_text(release.version);
    const auto date = release.date.empty() ? std::wstring{} : changelog_date_text(release.date);
    if (i == 0) {
      blocks.push_back({Kind::heading, version, date, {}});
      if (!release.features.empty())
        blocks.push_back(chrome(Kind::section, ui_text(L"New features")));
      for (const auto& feature : release.features)
        blocks.push_back({Kind::feature, changelog_widen(feature.title), changelog_widen(feature.text), feature.image});
      if (!release.fixes.empty())
        blocks.push_back(chrome(Kind::section, ui_text(L"Fixes")));
      for (const auto& fix : release.fixes)
        blocks.push_back({Kind::bullet, changelog_widen(fix), {}, {}});
      if (!release.changes.empty())
        blocks.push_back(
            chrome(Kind::section, release.features.empty() && release.fixes.empty() ? ui_text(L"Changes") : ui_text(L"Other changes")));
      for (const auto& change : release.changes)
        blocks.push_back({Kind::bullet, changelog_widen(change), {}, {}});
      continue;
    }
    if (i == 1)
      blocks.push_back(chrome(Kind::section, ui_text(L"Earlier versions")));
    blocks.push_back({Kind::release, version, date, {}});
    for (const auto& feature : release.features)
      blocks.push_back({Kind::bullet, changelog_widen(feature.title) + L": " + changelog_widen(feature.text), {}, {}});
    for (const auto& fix : release.fixes)
      blocks.push_back(chrome(Kind::bullet, ui_text(L"Fixed: {0}"), changelog_widen(fix)));
    for (const auto& change : release.changes)
      blocks.push_back({Kind::bullet, changelog_widen(change), {}, {}});
  }
  return blocks;
}

// The last installed version whose notes were read, in the global settings.ini
// so a settings reset shows the link again.
inline bool whats_new_pending(const std::wstring& directory, const ChangelogVersion& installed) {
  if (directory.empty())
    return true;
  wchar_t seen[64]{};
  GetPrivateProfileStringW(L"whats_new", L"seen", L"", seen, 64, (directory + L"\\settings.ini").c_str());
  std::string narrow;
  for (const wchar_t* c = seen; *c; ++c)
    narrow += *c < 0x80 ? static_cast<char>(*c) : '?';
  ChangelogVersion value;
  return !parse_changelog_version(narrow, value) || value != installed;
}
inline bool record_whats_new_seen(const std::wstring& directory, const ChangelogVersion& installed) {
  if (directory.empty())
    return false;
  return WritePrivateProfileStringW(L"whats_new", L"seen", changelog_version_text(installed).c_str(),
                                    (directory + L"\\settings.ini").c_str()) != FALSE;
}
}  // namespace taxi_camera::standalone
