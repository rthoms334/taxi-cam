// Read-only research scan of a running FlightSimulator2024.exe: which objects
// tie a cockpit display texture name (panel.cfg texture=, e.g. DUS) to the
// native ID3D12Resource the Taxi Cam bridge tracks.
//
// usage: display-identity-scan <pid> <display-candidates.txt> <name> [name...] > report.txt
//
// display-candidates.txt is written by the bridge beside a PFD routing
// snapshot. Opens the explicit PID with PROCESS_QUERY_INFORMATION |
// PROCESS_VM_READ only, after checking the exact executable basename and the
// same user. No write, no thread, no injection. Reads committed, readable,
// non-guard regions in bounded chunks; a failed read is skipped. The report
// holds process-local addresses: keep it local.
// clang-format off: Win32 types must precede PSAPI declarations.
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct Region {
  std::uint64_t base, size;
  DWORD type, protect;
};
HANDLE process{};
std::vector<Region> regions;
std::uint64_t image_base = 0, image_size = 0;

bool same_user(HANDLE target) {
  const auto user = [](HANDLE p, std::vector<unsigned char>& out) {
    HANDLE token{};
    if (!OpenProcessToken(p, TOKEN_QUERY, &token))
      return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    bool ok = bytes && bytes <= 65536 && GetLastError() == ERROR_INSUFFICIENT_BUFFER;
    if (ok) {
      out.resize(bytes);
      ok = GetTokenInformation(token, TokenUser, out.data(), bytes, &bytes) != FALSE;
    }
    CloseHandle(token);
    return ok;
  };
  std::vector<unsigned char> a, b;
  return user(target, a) && user(GetCurrentProcess(), b) &&
         EqualSid(reinterpret_cast<TOKEN_USER*>(a.data())->User.Sid, reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
}
bool readable(DWORD protect) {
  if (protect & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  const DWORD p = protect & 0xff;
  return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
         p == PAGE_EXECUTE_WRITECOPY;
}
void enumerate() {
  std::uint64_t address = 0;
  MEMORY_BASIC_INFORMATION m{};
  while (VirtualQueryEx(process, reinterpret_cast<void*>(address), &m, sizeof(m)) == sizeof(m)) {
    const auto base = reinterpret_cast<std::uint64_t>(m.BaseAddress);
    if (m.State == MEM_COMMIT && readable(m.Protect))
      regions.push_back({base, m.RegionSize, m.Type, m.Protect});
    address = base + m.RegionSize;
    if (address < base)
      break;
  }
}
const Region* region_of(std::uint64_t address) {
  auto it = std::upper_bound(regions.begin(), regions.end(), address, [](std::uint64_t a, const Region& r) { return a < r.base; });
  if (it == regions.begin())
    return nullptr;
  --it;
  return address < it->base + it->size ? &*it : nullptr;
}
bool read(std::uint64_t address, void* out, std::size_t size) {
  SIZE_T got = 0;
  return ReadProcessMemory(process, reinterpret_cast<void*>(address), out, size, &got) && got == size;
}
std::string where(std::uint64_t address) {
  if (address >= image_base && address < image_base + image_size) {
    char text[64];
    std::snprintf(text, sizeof(text), "exe+%#llx", static_cast<unsigned long long>(address - image_base));
    return text;
  }
  const auto* r = region_of(address);
  if (!r)
    return "?";
  return r->type == MEM_IMAGE ? "img" : r->type == MEM_MAPPED ? "map" : "heap";
}
// Printable ASCII or UTF-16 at address, or empty.
std::string text_at(std::uint64_t address) {
  if (!region_of(address))
    return {};
  unsigned char buffer[96]{};
  if (!read(address, buffer, sizeof(buffer)) && !read(address, buffer, 32))
    return {};
  std::string ascii;
  for (unsigned i = 0; i < sizeof(buffer) && buffer[i]; ++i) {
    if (buffer[i] < 32 || buffer[i] > 126)
      return ascii.size() >= 3 && buffer[i] == 0 ? ascii : std::string{};
    ascii += char(buffer[i]);
  }
  if (ascii.size() >= 3)
    return ascii;
  std::string wide;
  for (unsigned i = 0; i + 1 < sizeof(buffer); i += 2) {
    if (!buffer[i] && !buffer[i + 1])
      break;
    if (buffer[i + 1] || buffer[i] < 32 || buffer[i] > 126)
      return {};
    wide += char(buffer[i]);
  }
  return wide.size() >= 3 ? "L\"" + wide + "\"" : std::string{};
}

// Scan every region for aligned qwords in targets.
struct Hit {
  std::uint64_t address, value;
};
std::vector<Hit> scan(const std::set<std::uint64_t>& targets, std::size_t cap = 200000) {
  std::vector<Hit> hits;
  if (targets.empty())
    return hits;
  const std::uint64_t low = *targets.begin(), high = *targets.rbegin();
  std::vector<std::uint64_t> sorted(targets.begin(), targets.end());
  std::vector<std::uint64_t> filter(1u << 20);  // 64 Mbit
  for (auto t : sorted) {
    const auto h = (t * 0x9E3779B97F4A7C15ull) >> 38;
    filter[h >> 6] |= 1ull << (h & 63);
  }
  std::vector<std::uint64_t> chunk(1u << 21);  // 16 MiB
  for (const auto& r : regions) {
    for (std::uint64_t offset = 0; offset < r.size; offset += chunk.size() * 8) {
      const auto bytes = std::min<std::uint64_t>(chunk.size() * 8, r.size - offset);
      if (!read(r.base + offset, chunk.data(), bytes))
        continue;
      const auto n = bytes / 8;
      for (std::uint64_t i = 0; i < n; ++i) {
        const auto v = chunk[i];
        if (v < low || v > high)
          continue;
        const auto h = (v * 0x9E3779B97F4A7C15ull) >> 38;
        if (!(filter[h >> 6] & (1ull << (h & 63))))
          continue;
        if (std::binary_search(sorted.begin(), sorted.end(), v)) {
          hits.push_back({r.base + offset + i * 8, v});
          if (hits.size() >= cap)
            return hits;
        }
      }
    }
  }
  return hits;
}
// Find byte pattern occurrences (strings).
std::vector<std::uint64_t> find_bytes(const std::string& needle, std::size_t cap = 4000) {
  std::vector<std::uint64_t> found;
  std::vector<char> chunk((16u << 20) + 256);
  for (const auto& r : regions) {
    for (std::uint64_t offset = 0; offset < r.size; offset += 16u << 20) {
      const auto bytes = std::min<std::uint64_t>(chunk.size(), r.size - offset);
      if (!read(r.base + offset, chunk.data(), bytes))
        continue;
      const auto limit = std::min<std::uint64_t>(bytes, 16u << 20);
      for (auto it = chunk.begin(); (it = std::search(it, chunk.begin() + bytes, needle.begin(), needle.end())) != chunk.begin() + bytes;
           ++it) {
        const auto at = static_cast<std::uint64_t>(it - chunk.begin());
        if (at >= limit)
          break;
        found.push_back(r.base + offset + at);
        if (found.size() >= cap)
          return found;
      }
    }
  }
  return found;
}
void dump(std::uint64_t center, int before, int after, const char* indent) {
  const auto start = (center & ~7ull) - static_cast<std::uint64_t>(before);
  std::vector<std::uint64_t> words((before + after) / 8);
  if (!read(start, words.data(), words.size() * 8)) {
    std::printf("%s(unreadable)\n", indent);
    return;
  }
  for (std::size_t i = 0; i < words.size(); ++i) {
    const auto address = start + i * 8;
    const auto v = words[i];
    std::string note;
    if (region_of(v)) {
      note = where(v);
      const auto s = text_at(v);
      if (!s.empty())
        note += " -> \"" + s + "\"";
    }
    char inline_text[9]{};
    bool printable = true;
    for (int b = 0; b < 8; ++b) {
      const char c = static_cast<char>((v >> (8 * b)) & 0xff);
      inline_text[b] = c >= 32 && c <= 126 ? c : '.';
      printable &= c == 0 || (c >= 32 && c <= 126);
    }
    std::printf("%s%+5lld %016llx %s%s%s\n", indent, static_cast<long long>(address) - static_cast<long long>(center & ~7ull),
                static_cast<unsigned long long>(v), address == (center & ~7ull) ? "<== " : "    ",
                printable && v ? (std::string("'") + inline_text + "' ").c_str() : "", note.c_str());
  }
}
// Store 1.8.16.0 static decode (panel.cfg reader 0x30f5e00, factory
// 0x30f6470, registry getter 0x30f41f0): a function-local static registry of
// up to 64 VCockpit panel pointers, count (u32) at +0x200. Each 0x690-byte
// panel holds its texture= name inline at +0x88, the section name string at
// +0x528, its index at +0x598 and kind at +0x680. Other builds are refused.
constexpr std::uint32_t kTestedTimestamp = 1787653788, kTestedImageSize = 235963904;
constexpr std::uint64_t kPanelRegistry = 0xa50b160, kPanelSize = 0x690;
void walk_panels(const std::map<std::uint64_t, std::string>& natives) {
  unsigned char header[0x400]{};
  if (!image_base || !read(image_base, header, sizeof(header)) || header[0] != 'M' || header[1] != 'Z') {
    std::printf("\n=== panels: skipped (main image unreadable)\n");
    return;
  }
  std::uint32_t pe = 0, timestamp = 0, size = 0;
  std::memcpy(&pe, header + 0x3c, 4);
  if (pe + 24 + 60 <= sizeof(header)) {
    std::memcpy(&timestamp, header + pe + 8, 4);
    std::memcpy(&size, header + pe + 24 + 56, 4);
  }
  if (timestamp != kTestedTimestamp || size != kTestedImageSize) {
    std::printf("\n=== panels: skipped (untested build timestamp=%u size=%#x)\n", timestamp, size);
    return;
  }
  std::uint64_t registry[65]{};
  if (!read(image_base + kPanelRegistry, registry, sizeof(registry))) {
    std::printf("\n=== panels: registry unreadable\n");
    return;
  }
  const auto count = static_cast<std::uint32_t>(registry[64]);
  std::printf("\n=== panels (registry exe+%#llx count=%u)\n", static_cast<unsigned long long>(kPanelRegistry), count);
  for (std::uint32_t i = 0; i < count && i < 64; ++i) {
    const auto panel = registry[i];
    std::vector<unsigned char> bytes(kPanelSize);
    if (!panel || !read(panel, bytes.data(), bytes.size())) {
      std::printf("panel[%u] %llx unreadable\n", i, static_cast<unsigned long long>(panel));
      continue;
    }
    std::string name;
    for (std::size_t k = 0x88; k < 0x188 && bytes[k] >= 32 && bytes[k] <= 126; ++k)
      name += char(bytes[k]);
    std::uint64_t section_word = 0;
    std::memcpy(&section_word, bytes.data() + 0x528, 8);
    std::string section = text_at(section_word);
    if (section.empty())
      for (std::size_t k = 0x528; k < 0x548 && bytes[k] >= 32 && bytes[k] <= 126; ++k)
        section += char(bytes[k]);
    std::uint32_t index = 0, kind = 0;
    std::memcpy(&index, bytes.data() + 0x598, 4);
    std::memcpy(&kind, bytes.data() + 0x680, 4);
    std::printf("panel[%u] %llx texture='%s' section='%s' index=%u kind=%u\n", i, static_cast<unsigned long long>(panel), name.c_str(),
                section.c_str(), index, kind);
    // Breadth-first search of heap pointers out of the panel, depth 3, for
    // any bridge-tracked native texture address.
    struct Node {
      std::uint64_t address;
      std::string path;
      int depth;
    };
    std::vector<Node> queue{{panel, "panel", 0}};
    std::set<std::uint64_t> visited{panel};
    int found = 0;
    for (std::size_t q = 0; q < queue.size() && q < 6000; ++q) {
      const auto node = queue[q];
      const std::size_t span = node.depth ? 0x400 : kPanelSize;
      std::vector<std::uint64_t> words(span / 8);
      if (!read(node.address, words.data(), span) && !read(node.address, words.data(), 0x100))
        continue;
      for (std::size_t w = 0; w < words.size(); ++w) {
        const auto v = words[w];
        char step[48];
        std::snprintf(step, sizeof(step), "+%#zx", w * 8);
        if (const auto it = natives.find(v); it != natives.end()) {
          std::printf("  FOUND %s%s = native %s\n", node.path.c_str(), step, it->second.c_str());
          ++found;
        } else if (node.depth < 3 && (v & 7) == 0 && where(v) == "heap" && visited.insert(v).second) {
          queue.push_back({v, node.path + step + "->", node.depth + 1});
        }
      }
    }
    if (!found)
      std::printf("  no tracked native within 3 pointer levels (%zu objects)\n", queue.size());
    if (i < 3)
      dump(panel, 0, static_cast<int>(kPanelSize), "    ");
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: display-identity-scan <pid> <display-candidates.txt> <name> [name...]\n");
    return 2;
  }
  const DWORD pid = static_cast<DWORD>(std::strtoul(argv[1], nullptr, 10));
  process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
  if (!process) {
    std::fprintf(stderr, "OpenProcess failed %lu\n", GetLastError());
    return 1;
  }
  wchar_t image[1024];
  DWORD length = 1024;
  const wchar_t* basename = nullptr;
  if (QueryFullProcessImageNameW(process, 0, image, &length)) {
    basename = wcsrchr(image, L'\\');
    basename = basename ? basename + 1 : image;
  }
  if (!basename || _wcsicmp(basename, L"FlightSimulator2024.exe") != 0 || !same_user(process)) {
    std::printf("error=wrong_process\n");
    return 1;
  }
  HMODULE modules[1];
  DWORD needed = 0;
  if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed, LIST_MODULES_ALL)) {
    MODULEINFO info{};
    if (GetModuleInformation(process, modules[0], &info, sizeof(info))) {
      image_base = reinterpret_cast<std::uint64_t>(info.lpBaseOfDll);
      image_size = info.SizeOfImage;
    }
  }
  enumerate();
  std::uint64_t total = 0;
  for (const auto& r : regions)
    total += r.size;
  std::printf("pid=%lu image_size=%#llx regions=%zu readable=%.1f GiB\n", pid, static_cast<unsigned long long>(image_size), regions.size(),
              total / 1073741824.0);

  // Candidates.
  std::map<std::uint64_t, std::string> natives;  // native -> label
  if (std::FILE* file = std::fopen(argv[2], "rb")) {
    char line[512];
    while (std::fgets(line, sizeof(line), file)) {
      unsigned long long id = 0, native = 0;
      unsigned w = 0, h = 0, mips = 0, format = 0;
      if (std::sscanf(line, "id=%llu native=0x%llx size=%ux%u mips=%u format=%u", &id, &native, &w, &h, &mips, &format) == 6) {
        char label[96];
        std::snprintf(label, sizeof(label), "#%llu %ux%u m%u f%u", id, w, h, mips, format);
        natives[native] = label;
      } else {
        std::printf("candidates: %s", line);
      }
    }
    std::fclose(file);
  }
  std::printf("natives=%zu\n", natives.size());
  walk_panels(natives);
  bool full = true;
  for (int a = 3; a < argc; ++a)
    full &= std::strcmp(argv[a], "--panels-only") != 0;
  if (!full) {
    CloseHandle(process);
    return 0;
  }

  // Level 0: holders of each native.
  std::set<std::uint64_t> targets;
  for (const auto& [native, label] : natives)
    targets.insert(native);
  const auto holders = scan(targets);
  std::map<std::uint64_t, std::vector<std::uint64_t>> by_native;
  for (const auto& hit : holders)
    by_native[hit.value].push_back(hit.address);
  std::printf("\n=== native holders (%zu)\n", holders.size());
  for (const auto& [native, list] : by_native) {
    std::printf("%s native=%llx holders=%zu\n", natives[native].c_str(), static_cast<unsigned long long>(native), list.size());
    for (std::size_t i = 0; i < list.size() && i < 40; ++i)
      std::printf("  %llx %s\n", static_cast<unsigned long long>(list[i]), where(list[i]).c_str());
  }

  // Name strings.
  std::map<std::uint64_t, std::string> name_sites;  // address -> name
  for (int a = 3; a < argc; ++a) {
    if (argv[a][0] == '-')
      continue;
    const std::string name = argv[a];
    std::string ascii = name;
    ascii.push_back('\0');
    std::string wide;
    for (char c : name) {
      wide.push_back(c);
      wide.push_back('\0');
    }
    wide.append(2, '\0');
    for (const auto& needle : {ascii, wide}) {
      for (auto at : find_bytes(needle)) {
        // Require a string start: previous byte(s) not printable.
        unsigned char before[2]{};
        read(at - 2, before, 2);
        const bool is_wide = needle.size() > ascii.size();
        const bool start = is_wide ? !(before[0] >= 32 && before[0] <= 126 && before[1] == 0) : !(before[1] >= 32 && before[1] <= 126);
        if (start)
          name_sites[at] = (is_wide ? "L:" : "") + name;
      }
    }
  }
  std::printf("\n=== name strings (%zu)\n", name_sites.size());
  std::map<std::string, int> per_name;
  for (const auto& [at, name] : name_sites)
    if (per_name[name]++ < 25)
      std::printf("  %s at %llx %s\n", name.c_str(), static_cast<unsigned long long>(at), where(at).c_str());

  // Level 1: pointers to name strings (and to inline-string objects starting
  // up to 0x20 before them), and pointers into holder objects (base up to
  // 0x400 before the holder).
  std::map<std::uint64_t, std::pair<std::uint64_t, int>> back;  // target -> (origin, delta)
  for (const auto& [at, name] : name_sites)
    for (int d = 0; d <= 0x20; d += 8)
      back.emplace((at & ~7ull) - d + (at & 7ull ? 0 : 0), std::make_pair(at, -d));
  for (const auto& [at, name] : name_sites)
    back.emplace(at, std::make_pair(at, 0));
  std::set<std::uint64_t> holder_set;
  for (const auto& hit : holders)
    if (where(hit.address) == "heap")
      holder_set.insert(hit.address);
  for (auto h : holder_set)
    for (int d = 0; d <= 0x400; d += 8)
      back.emplace(h - d, std::make_pair(h, -d));
  std::set<std::uint64_t> level1;
  for (const auto& [t, origin] : back)
    level1.insert(t);
  const auto refs = scan(level1, 400000);
  std::printf("\n=== level-1 references (%zu)\n", refs.size());

  // Classify.
  std::vector<std::pair<std::uint64_t, std::string>> name_refs, chain_refs;  // address, description
  for (const auto& hit : refs) {
    const auto [origin, delta] = back[hit.value];
    char text[160];
    if (name_sites.count(origin)) {
      std::snprintf(text, sizeof(text), "-> name %s%+d", name_sites[origin].c_str(), delta);
      name_refs.push_back({hit.address, text});
    } else {
      std::snprintf(text, sizeof(text), "-> holder %llx base%+d", static_cast<unsigned long long>(origin), delta);
      chain_refs.push_back({hit.address, text});
    }
  }
  std::printf("name refs=%zu chain refs=%zu\n", name_refs.size(), chain_refs.size());

  // Proximity: name references within 0x800 bytes of a native holder or a
  // chain reference are candidate identity objects.
  std::vector<std::pair<std::uint64_t, std::string>> anchors;
  for (const auto& hit : holders)
    anchors.push_back({hit.address, "native " + natives[hit.value]});
  for (const auto& c : chain_refs)
    anchors.push_back(c);
  std::sort(anchors.begin(), anchors.end());
  // Inline (small-string) names live inside their object: treat each name
  // site as a reference too.
  for (const auto& [at, name] : name_sites)
    name_refs.push_back({at, "inline name " + name});
  std::printf("\n=== name reference next to texture chain (within 0x800, closest 4 each)\n");
  int shown = 0;
  std::set<std::uint64_t> dumped;
  for (const auto& [address, text] : name_refs) {
    std::vector<std::pair<long long, const std::pair<std::uint64_t, std::string>*>> nearby;
    auto it = std::lower_bound(anchors.begin(), anchors.end(), std::make_pair(address > 0x800 ? address - 0x800 : 0, std::string{}));
    for (; it != anchors.end() && it->first <= address + 0x800; ++it)
      nearby.push_back({static_cast<long long>(it->first) - static_cast<long long>(address), &*it});
    if (nearby.empty())
      continue;
    std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return std::llabs(a.first) < std::llabs(b.first); });
    std::printf("name ref %llx %s %s\n", static_cast<unsigned long long>(address), where(address).c_str(), text.c_str());
    for (std::size_t i = 0; i < nearby.size() && i < 4; ++i)
      std::printf("    %+lld: %llx %s\n", nearby[i].first, static_cast<unsigned long long>(nearby[i].second->first),
                  nearby[i].second->second.c_str());
    if (shown < 30 && dumped.insert(address & ~0xffull).second) {
      ++shown;
      const auto other = nearby[0].second->first;
      const auto low = std::min(address, other), high = std::max(address, other);
      dump(low, 0x40, static_cast<int>(high - low) + 0x48, "      ");
    }
  }

  // Context of the first heap holders per native and the closest chain refs.
  std::printf("\n=== holder context\n");
  for (const auto& [native, list] : by_native) {
    int n = 0;
    for (auto h : list) {
      if (where(h) != "heap" || n++ >= 4)
        continue;
      std::printf("%s holder %llx\n", natives[native].c_str(), static_cast<unsigned long long>(h));
      dump(h, 0x80, 0x80, "    ");
    }
  }
  std::printf("\n=== chain refs (first 60, smallest base delta first)\n");
  std::stable_sort(chain_refs.begin(), chain_refs.end(), [](const auto& a, const auto& b) { return a.second.size() < b.second.size(); });
  for (std::size_t i = 0; i < chain_refs.size() && i < 60; ++i) {
    std::printf("%llx %s %s\n", static_cast<unsigned long long>(chain_refs[i].first), where(chain_refs[i].first).c_str(),
                chain_refs[i].second.c_str());
    if (i < 12)
      dump(chain_refs[i].first, 0x40, 0x48, "    ");
  }
  std::printf("\n=== name refs (first 60)\n");
  for (std::size_t i = 0; i < name_refs.size() && i < 60; ++i) {
    std::printf("%llx %s %s\n", static_cast<unsigned long long>(name_refs[i].first), where(name_refs[i].first).c_str(),
                name_refs[i].second.c_str());
    if (i < 12)
      dump(name_refs[i].first, 0x40, 0x48, "    ");
  }
  CloseHandle(process);
  return 0;
}
