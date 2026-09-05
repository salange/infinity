#include "gen/civ_types.hpp"

#include <cstdio>

namespace inf::gen {

const char* to_string(RaceType type) {
  switch (type) {
    case RaceType::Humanoid: return "Humanoid";
    case RaceType::Insectoid: return "Insectoid";
    case RaceType::Reptilian: return "Reptilian";
    case RaceType::Avian: return "Avian";
    case RaceType::Aquatic: return "Aquatic";
    case RaceType::Fungoid: return "Fungoid";
    case RaceType::Machine: return "Machine";
    case RaceType::Crystalline: return "Crystalline";
    case RaceType::Precursor: return "Precursor";
    case RaceType::Count: break;
  }
  return "?";
}

const char* to_string(FactionType type) {
  switch (type) {
    case FactionType::Government: return "Government";
    case FactionType::Independent: return "Independent";
    case FactionType::Outlaw: return "Outlaw";
    case FactionType::AlignedMachine: return "AlignedMachine";
    case FactionType::RenegadeMachine: return "RenegadeMachine";
    case FactionType::Count: break;
  }
  return "?";
}

const char* to_string(DevLevel level) {
  switch (level) {
    case DevLevel::Uninhabited: return "Uninhabited";
    case DevLevel::Founding: return "Founding";
    case DevLevel::IndependentSettlements: return "Independent settlements";
    case DevLevel::FormingStates: return "Forming states";
    case DevLevel::NationStates: return "Nation states";
    case DevLevel::PlanetaryGovernment: return "Planetary government";
    case DevLevel::Metropolises: return "Metropolises";
    case DevLevel::Trantorian: return "Trantorian";
  }
  return "?";
}

const char* to_string(SettlementTier tier) {
  switch (tier) {
    case SettlementTier::None: return "None";
    case SettlementTier::Outpost: return "Outpost";
    case SettlementTier::Hamlet: return "Hamlet";
    case SettlementTier::Village: return "Village";
    case SettlementTier::Town: return "Town";
    case SettlementTier::City: return "City";
    case SettlementTier::Metropolis: return "Metropolis";
    case SettlementTier::Capital: return "Capital";
    case SettlementTier::Ecumenopolis: return "Ecumenopolis";
  }
  return "?";
}

const char* to_string(LayoutFamily family) {
  switch (family) {
    case LayoutFamily::Organic: return "Organic";
    case LayoutFamily::Grid: return "Grid";
    case LayoutFamily::Radial: return "Radial";
    case LayoutFamily::Linear: return "Linear";
    case LayoutFamily::Hive: return "Hive";
    case LayoutFamily::Crystal: return "Crystal";
    case LayoutFamily::Lattice: return "Lattice";
    case LayoutFamily::Terraced: return "Terraced";
    case LayoutFamily::Domed: return "Domed";
    case LayoutFamily::Count: break;
  }
  return "?";
}

const char* to_string(LotUsage usage) {
  switch (usage) {
    case LotUsage::Residential: return "Residential";
    case LotUsage::Civic: return "Civic";
    case LotUsage::Industrial: return "Industrial";
    case LotUsage::Agricultural: return "Agricultural";
    case LotUsage::Pad: return "Pad";
    case LotUsage::Monument: return "Monument";
    case LotUsage::Count: break;
  }
  return "?";
}

const char* faction_type_label(FactionType type, RaceType race, bool human) {
  if (human) {
    switch (type) {
      case FactionType::Government: return "Empire";
      case FactionType::Independent: return "Free settlers";
      case FactionType::Outlaw: return "Outlaws";
      case FactionType::AlignedMachine: return "Aligned androids";
      case FactionType::RenegadeMachine: return "Renegade androids";
      case FactionType::Count: break;
    }
  }
  if (race == RaceType::Machine) {
    switch (type) {
      case FactionType::Government: return "Collective";
      case FactionType::Independent: return "Splinter";
      case FactionType::Outlaw: return "Rogue";
      default: break;
    }
  }
  return to_string(type);
}

std::string RaceParams::to_json() const {
  char buffer[768];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\": \"%s\", \"name\": \"%s\", \"adjective\": \"%s\", "
                "\"variant\": %u, \"tech_tier\": %d, \"peak_level\": %d, "
                "\"home_level\": %d, \"dome_affinity\": %.3f, \"faction_count\": %d, "
                "\"t_0_ns\": %lld, \"speed_ly_yr\": %.1f, \"reproduction\": %.3f, "
                "\"settle_prob\": %.3f, \"r_max_ly\": %.1f, \"falloff_ly\": %.1f, "
                "\"anisotropy\": %.3f, \"extinct_ever\": %s, \"t_end_ns\": %lld, "
                "\"sources\": %zu, \"human\": %s}",
                gen::to_string(type), name.c_str(), adjective.c_str(), variant, tech_tier,
                peak_level, home_level, dome_affinity, faction_count,
                static_cast<long long>(t_0.ns_since_epoch), speed_ly_per_year, reproduction,
                settle_prob, r_max_ly, falloff_ly, anisotropy, extinct_ever ? "true" : "false",
                static_cast<long long>(t_end.ns_since_epoch), sources.size(),
                is_human ? "true" : "false");
  return buffer;
}

std::string FactionParams::to_json() const {
  char buffer[384];
  std::snprintf(buffer, sizeof(buffer),
                "{\"type\": \"%s\", \"name\": \"%s\", \"t_start_ns\": %lld, "
                "\"hostile\": %s, \"speed_mul\": %.3f, \"reproduction_mul\": %.3f, "
                "\"settle_mul\": %.3f, \"dome_mul\": %.3f, \"centres\": %zu}",
                gen::to_string(type), name.c_str(),
                static_cast<long long>(t_start.ns_since_epoch), hostile ? "true" : "false",
                speed_mul, reproduction_mul, settle_mul, dome_mul, centres.size());
  return buffer;
}

std::string CivState::to_json() const {
  char buffer[512];
  std::snprintf(buffer, sizeof(buffer),
                "{\"settled\": %s, \"race_index\": %u, \"faction_index\": %d, "
                "\"faction_type\": \"%s\", \"level\": %d, \"max_level\": %d, "
                "\"ruined\": %s, \"domed\": %s, \"home\": %s, \"settled_at_ns\": %lld, "
                "\"age_s\": %.1f, \"growth\": %.4f, \"suitability\": %.4f, "
                "\"progress\": %.4f}",
                settled ? "true" : "false", race_index, faction_index,
                gen::to_string(faction_type), level, max_level, ruined ? "true" : "false",
                domed ? "true" : "false", is_home ? "true" : "false",
                static_cast<long long>(settled_at.ns_since_epoch), age_s, growth,
                suitability, progress);
  return buffer;
}

}  // namespace inf::gen
