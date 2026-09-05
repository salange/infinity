#include "gen/civ_names.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

#include "gen/names.hpp"

namespace inf::gen {

namespace {

struct Phonology {
  const char* const* onsets;
  int onset_count;
  const char* const* nuclei;
  int nucleus_count;
  const char* const* codas;
  int coda_count;
  int min_syllables;
  int max_syllables;
  double coda_prob;
  const char* const* adjective_suffixes;
  int suffix_count;
};

// Humanoid: Latinate/Germanic/Sinitic blend.
constexpr const char* kHumOn[] = {"", "b", "c", "d", "f", "g", "h", "k", "l", "m", "n",
                                  "p", "r", "s", "t", "v", "z", "th", "sh", "br", "tr", "kr"};
constexpr const char* kHumNu[] = {"a", "e", "i", "o", "u", "ae", "ia", "ei", "ou", "y"};
constexpr const char* kHumCo[] = {"", "n", "r", "l", "s", "th", "m", "nd", "rn", "st"};
constexpr const char* kHumSuf[] = {"i", "an", "ian", "ese", "ic", "ari"};
// Insectoid: clicks, apostrophes, doubled consonants.
constexpr const char* kInsOn[] = {"k", "kh", "t", "tz", "x", "ch", "q", "zz", "kr", "tk", "sk"};
constexpr const char* kInsNu[] = {"a", "i", "e'", "a'", "ii", "ee", "u", "aa"};
constexpr const char* kInsCo[] = {"", "k", "t", "ch", "x", "kk", "tt", "ss"};
constexpr const char* kInsSuf[] = {"ik", "ax", "'tch", "ith"};
// Reptilian: sibilants, hard stops.
constexpr const char* kRepOn[] = {"s", "ss", "sh", "z", "g", "gr", "kr", "th", "dr", "v", "r"};
constexpr const char* kRepNu[] = {"a", "o", "u", "aa", "ao", "au", "i"};
constexpr const char* kRepCo[] = {"", "ss", "sh", "k", "g", "rr", "th", "x"};
constexpr const char* kRepSuf[] = {"ss", "ith", "ak", "oth"};
// Avian: open vowels, liquids.
constexpr const char* kAviOn[] = {"", "l", "r", "w", "y", "h", "v", "fl", "kl", "s", "t"};
constexpr const char* kAviNu[] = {"a", "e", "i", "o", "ai", "ee", "ia", "io", "ea"};
constexpr const char* kAviCo[] = {"", "l", "r", "n", "s", "th"};
constexpr const char* kAviSuf[] = {"i", "el", "ai", "an"};
// Aquatic: flowing, m/n/l, long vowels.
constexpr const char* kAquOn[] = {"", "m", "n", "l", "w", "gl", "ml", "b", "v", "th", "s"};
constexpr const char* kAquNu[] = {"a", "o", "u", "oo", "ua", "ou", "ae", "ei"};
constexpr const char* kAquCo[] = {"", "m", "n", "l", "r", "ng", "lm"};
constexpr const char* kAquSuf[] = {"an", "ul", "oi", "ari"};
// Fungoid: soft, humming.
constexpr const char* kFunOn[] = {"", "m", "n", "f", "ph", "v", "sp", "sm", "h", "l", "th"};
constexpr const char* kFunNu[] = {"o", "u", "a", "oo", "ou", "e", "uo"};
constexpr const char* kFunCo[] = {"", "m", "n", "sh", "ph", "th", "ss"};
constexpr const char* kFunSuf[] = {"um", "ori", "esh", "an"};
// Machine: hard designations (numerals appended separately).
constexpr const char* kMacOn[] = {"k", "x", "v", "z", "t", "d", "kv", "tr", "n", "r", "s"};
constexpr const char* kMacNu[] = {"a", "e", "o", "i", "u", "y"};
constexpr const char* kMacCo[] = {"x", "k", "n", "r", "t", "s", "rn", "kt"};
constexpr const char* kMacSuf[] = {"", "ax", "ik", "on"};
// Crystalline: hard clusters, no soft codas.
constexpr const char* kCryOn[] = {"k", "kr", "tr", "gl", "th", "z", "sk", "st", "kl", "q", "x"};
constexpr const char* kCryNu[] = {"a", "i", "e", "u", "ai", "ei", "y"};
constexpr const char* kCryCo[] = {"k", "th", "x", "st", "rk", "nt", "lt", ""};
constexpr const char* kCrySuf[] = {"ith", "ak", "eth", "ix"};
// Precursor: archaic, long, vowel-rich.
constexpr const char* kPreOn[] = {"", "a", "e", "h", "l", "m", "n", "r", "s", "th", "v", "z"};
constexpr const char* kPreNu[] = {"a", "e", "i", "o", "u", "ae", "ao", "eo", "ia", "ua"};
constexpr const char* kPreCo[] = {"", "n", "r", "l", "s", "th", "h", "m"};
constexpr const char* kPreSuf[] = {"i", "ae", "ari", "eth"};

template <std::size_t N>
constexpr int count_of(const char* const (&)[N]) {
  return static_cast<int>(N);
}

const Phonology& phonology(RaceType type) {
  static const Phonology kTables[] = {
      {kHumOn, count_of(kHumOn), kHumNu, count_of(kHumNu), kHumCo, count_of(kHumCo), 2, 3, 0.5,
       kHumSuf, count_of(kHumSuf)},
      {kInsOn, count_of(kInsOn), kInsNu, count_of(kInsNu), kInsCo, count_of(kInsCo), 2, 3, 0.6,
       kInsSuf, count_of(kInsSuf)},
      {kRepOn, count_of(kRepOn), kRepNu, count_of(kRepNu), kRepCo, count_of(kRepCo), 2, 3, 0.6,
       kRepSuf, count_of(kRepSuf)},
      {kAviOn, count_of(kAviOn), kAviNu, count_of(kAviNu), kAviCo, count_of(kAviCo), 2, 3, 0.35,
       kAviSuf, count_of(kAviSuf)},
      {kAquOn, count_of(kAquOn), kAquNu, count_of(kAquNu), kAquCo, count_of(kAquCo), 2, 3, 0.45,
       kAquSuf, count_of(kAquSuf)},
      {kFunOn, count_of(kFunOn), kFunNu, count_of(kFunNu), kFunCo, count_of(kFunCo), 2, 3, 0.45,
       kFunSuf, count_of(kFunSuf)},
      {kMacOn, count_of(kMacOn), kMacNu, count_of(kMacNu), kMacCo, count_of(kMacCo), 1, 2, 0.9,
       kMacSuf, count_of(kMacSuf)},
      {kCryOn, count_of(kCryOn), kCryNu, count_of(kCryNu), kCryCo, count_of(kCryCo), 2, 3, 0.75,
       kCrySuf, count_of(kCrySuf)},
      {kPreOn, count_of(kPreOn), kPreNu, count_of(kPreNu), kPreCo, count_of(kPreCo), 3, 4, 0.4,
       kPreSuf, count_of(kPreSuf)},
  };
  const auto index = static_cast<std::size_t>(type);
  return kTables[index < 9 ? index : 0];
}

// Sequences that must never appear in a generated name (case-insensitive,
// substring). Deliberately blunt; a hit costs one re-draw.
constexpr const char* kBlocklist[] = {"fuck", "shit", "cunt", "nigg", "fagg", "kike", "rape",
                                      "nazi", "hitl", "cock", "dick", "piss", "twat", "slut",
                                      "whor", "anus", "arse", "tits", "coon", "spic", "chink",
                                      "penis", "vagin", "sex", "kill", "dead", "hell"};

std::string lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string capitalise(std::string s) {
  for (char& c : s) {
    if (std::isalpha(static_cast<unsigned char>(c)) != 0) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      break;
    }
  }
  return s;
}

// One attempt: syllables from (purpose, attempt) draws, attempt bumps on
// a blocklist hit. Returns the raw lowercase word.
std::string draw_word(const core::Key& names_key, const Phonology& ph, std::uint64_t purpose,
                      int syllable_offset) {
  for (std::uint64_t attempt = 0; attempt < 8; ++attempt) {
    const auto d = core::draw_point(names_key, channel::Params,
                                    static_cast<std::int64_t>(purpose),
                                    static_cast<std::int64_t>(attempt), 0);
    std::uint64_t state = d[0];
    const auto next = [&state] {
      state ^= state >> 12U;
      state ^= state << 25U;
      state ^= state >> 27U;
      return state * 0x2545F4914F6CDD1DULL;
    };
    const int span = ph.max_syllables - ph.min_syllables + 1;
    int syllables = ph.min_syllables + static_cast<int>(next() % static_cast<std::uint64_t>(span));
    syllables += syllable_offset;
    if (syllables < 1) syllables = 1;
    std::string word;
    for (int s = 0; s < syllables; ++s) {
      const char* onset = ph.onsets[next() % static_cast<std::uint64_t>(ph.onset_count)];
      const char* nucleus = ph.nuclei[next() % static_cast<std::uint64_t>(ph.nucleus_count)];
      const bool coda = (next() % 1000U) < static_cast<std::uint64_t>(ph.coda_prob * 1000.0) ||
                        s == syllables - 1;
      const char* coda_s = coda ? ph.codas[next() % static_cast<std::uint64_t>(ph.coda_count)] : "";
      // Avoid an empty onset after an empty coda in the middle of a word
      // (vowel pile-ups read badly).
      if (s > 0 && onset[0] == '\0' && !word.empty() &&
          std::strchr("aeiouy'", word.back()) != nullptr) {
        onset = ph.onsets[1 + next() % static_cast<std::uint64_t>(ph.onset_count - 1)];
      }
      word += onset;
      word += nucleus;
      word += coda_s;
    }
    if (!name_blocked(word)) {
      return word;
    }
  }
  return "unnamed";
}

}  // namespace

bool name_blocked(const std::string& candidate) {
  const std::string low = lower(candidate);
  for (const char* bad : kBlocklist) {
    if (low.find(bad) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void race_names(const core::Key& race_key, RaceType type, std::string* name,
                std::string* adjective) {
  const core::Key names_key = core::derive_named(race_key, name::CivNamesV1);
  const Phonology& ph = phonology(type);
  std::string word = draw_word(names_key, ph, 1, 0);
  if (type == RaceType::Machine) {
    const auto d = core::draw_point(names_key, channel::Params, 2, 0, 0);
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), "-%u", static_cast<unsigned>(d[0] % 900U + 100U));
    *adjective = capitalise(word);
    *name = capitalise(word) + suffix;
    return;
  }
  const auto d = core::draw_point(names_key, channel::Params, 3, 0, 0);
  const char* suffix = ph.adjective_suffixes[d[0] % static_cast<std::uint64_t>(ph.suffix_count)];
  *name = capitalise(word);
  std::string adj = word;
  // Drop a trailing vowel before a vowel-initial suffix.
  if (!adj.empty() && suffix[0] != '\0' && std::strchr("aeiou", adj.back()) != nullptr &&
      std::strchr("aeiou", suffix[0]) != nullptr) {
    adj.pop_back();
  }
  adj += suffix;
  *adjective = name_blocked(adj) ? *name : capitalise(adj);
}

std::string faction_name(const core::Key& faction_key, RaceType race, FactionType type,
                         const std::string& race_adjective, bool human) {
  const core::Key names_key = core::derive_named(faction_key, name::CivNamesV1);
  const auto d = core::draw_point(names_key, channel::Params, 1, 0, 0);
  const std::uint64_t roll = d[0] % 4U;
  const std::string proper = capitalise(draw_word(names_key, phonology(race), 2, -1));
  char buffer[128];
  if (human) {
    switch (type) {
      case FactionType::Government: {
        static constexpr const char* kForms[] = {"Terran Compact", "Solar Hegemony",
                                                 "Concordat of %s", "%s Dominion"};
        std::snprintf(buffer, sizeof(buffer), kForms[roll], proper.c_str());
        return buffer;
      }
      case FactionType::Independent: {
        static constexpr const char* kForms[] = {"Free %s League", "%s Settlers' Union",
                                                 "Homesteads of %s", "%s Cooperative"};
        std::snprintf(buffer, sizeof(buffer), kForms[roll], proper.c_str());
        return buffer;
      }
      case FactionType::Outlaw: {
        static constexpr const char* kForms[] = {"%s Reavers", "Black %s Cartel",
                                                 "%s Syndicate", "Corsairs of %s"};
        std::snprintf(buffer, sizeof(buffer), kForms[roll], proper.c_str());
        return buffer;
      }
      case FactionType::AlignedMachine: return "Concord Synthetics";
      case FactionType::RenegadeMachine: return "The Severance";
      case FactionType::Count: break;
    }
  }
  if (race == RaceType::Machine) {
    static constexpr const char* kForms[3][2] = {
        {"%s Collective", "Consensus %s"}, {"%s Splinter", "Fork %s"}, {"Rogue %s", "%s Anomaly"}};
    const int row = type == FactionType::Government ? 0 : (type == FactionType::Independent ? 1 : 2);
    std::snprintf(buffer, sizeof(buffer), kForms[row][roll & 1U], proper.c_str());
    return buffer;
  }
  switch (type) {
    case FactionType::Government: {
      // Half the forms take the race adjective, half a proper word, so
      // two governments of one race rarely share a name.
      static constexpr const char* kForms[] = {"%s Hegemony", "%s Imperium", "Throne of %s",
                                               "%s Ascendancy", "Crown of %s", "%s Concord",
                                               "%s Dominion", "House %s"};
      const std::uint64_t form = d[1] % 8U;
      const bool adjective = form == 0 || form == 1 || form == 3 || form == 6;
      std::snprintf(buffer, sizeof(buffer), kForms[form],
                    adjective ? race_adjective.c_str() : proper.c_str());
      return buffer;
    }
    case FactionType::Independent: {
      static constexpr const char* kForms[] = {"Free %s", "%s Drift", "%s Holds", "The %s Reach"};
      std::snprintf(buffer, sizeof(buffer), kForms[roll], proper.c_str());
      return buffer;
    }
    case FactionType::Outlaw: {
      static constexpr const char* kForms[] = {"%s Marauders", "Broken %s", "%s Raiders",
                                               "The %s Wild"};
      std::snprintf(buffer, sizeof(buffer), kForms[roll], proper.c_str());
      return buffer;
    }
    case FactionType::AlignedMachine: {
      std::snprintf(buffer, sizeof(buffer), "%s Servitors", race_adjective.c_str());
      return buffer;
    }
    case FactionType::RenegadeMachine: {
      std::snprintf(buffer, sizeof(buffer), "Unbound of %s", proper.c_str());
      return buffer;
    }
    case FactionType::Count: break;
  }
  return proper;
}

std::string settlement_name(const core::Key& site_key, RaceType race, bool capital) {
  const core::Key names_key = core::derive_named(site_key, name::CivNamesV1);
  const std::string word = capitalise(draw_word(names_key, phonology(race), 1, 0));
  if (!capital) {
    return word;
  }
  const auto d = core::draw_point(names_key, channel::Params, 4, 0, 0);
  static constexpr const char* kHonorifics[] = {"High %s", "%s Prime", "Great %s", "%s Throne"};
  char buffer[96];
  std::snprintf(buffer, sizeof(buffer), kHonorifics[d[0] % 4U], word.c_str());
  return buffer;
}

}  // namespace inf::gen
