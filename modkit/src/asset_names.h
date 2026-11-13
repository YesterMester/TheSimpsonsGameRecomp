#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace simpsons::modkit {

// Asset names as people know them. A name containing the key gets the label.
inline constexpr std::pair<const char*, const char*> kAssetLabels[] = {
    {"jetengineupdraft", "Jet engine updraft"},
    {"genericcompactcar", "Compact car"},
    {"genericconvertible", "Convertible"},
    {"genericvan", "Van"},
    {"generictruck", "Pickup truck"},
    {"swattruck", "SWAT truck"},
    {"willyshack", "Groundskeeper Willie's shack"},
    {"doghouse", "Doghouse"},
    {"trashcan", "Trash can"},
    {"firehydrant", "Fire hydrant"},
    {"destructiblefence01", "Wooden fence"},
    {"destructiblefence02", "Picket fence"},
    {"destructiblefence", "Fence"},
    {"trampolinedumpster", "Dumpster trampoline"},
    {"bouncytire", "Bouncy tire"},
    {"maggie_entrance", "Maggie's crawl space"},
    {"vendingmachine", "Vending machine"},
    {"scaffolding_unbuilt", "Scaffolding (unbuilt)"},
    {"scaffolding", "Scaffolding"},
    {"helium_bottle", "Helium tank"},
    {"flandersgrill", "Flanders' barbecue"},
    {"agnes_skinner", "Agnes Skinner"},
    {"07_kid", "Springfield kid"},
    {"06_kid", "Springfield kid (older)"},
    {"metalbox", "Metal box"},
    {"dufftank", "Duff tank"},
    {"quarterpipe", "Quarter pipe"},
    {"woodplank", "Wooden plank"},
    {"lisahandofgod", "Lisa's Hand of God block"},
    {"pileoftrash", "Pile of trash"},
    {"tallpottedflowerplant", "Potted plant"},
    {"hangingstainglasslamp", "Stained-glass lamp"},
    {"lovejoypodium", "Reverend Lovejoy's pulpit"},
    {"croctail", "Crocodile tail"},
    {"moh_tree", "Tree"},
    {"watervalve", "Water valve"},
    {"duffbottlecap", "Duff bottle cap"},
    {"krustycoupon", "Krusty coupon"},
    {"trynsavecoupon", "Try-N-Save coupon"},
    {"malibu", "Malibu Stacy seal"},
    {"lisacoupon", "Malibu Stacy seal"},
    {"airconditioner", "Air conditioner"},
    {"_pipe", "Lead pipe"},
    {"_hammer", "Hammer"},
    {"_pitchfork", "Pitchfork"},
    {"_rock", "Rock"},
    {"_slingshot", "Slingshot"},
    {"_hansmolemancane", "Hans Moleman's cane"},
    {"_torch", "Torch"},
    {"_saw", "Saw"},
    {"_baton", "Baton"},
    {"_crowbar", "Crowbar"},
    {"_controller", "Game controller"},
    {"chief_wiggum", "Chief Wiggum"},
    {"chiefwiggum", "Chief Wiggum"},
    {"wiggum", "Chief Wiggum"},
    {"groundskeeper_willie", "Groundskeeper Willie"},
    {"groundskeeperwillie", "Groundskeeper Willie"},
    {"willie", "Groundskeeper Willie"},
    {"willy", "Groundskeeper Willie"},
    {"comic_book_guy", "Comic Book Guy"},
    {"comicbookguy", "Comic Book Guy"},
    {"prof_frink", "Professor Frink"},
    {"professor_frink", "Professor Frink"},
    {"frink", "Professor Frink"},
    {"mr_burns", "Mr. Burns"},
    {"mrburns", "Mr. Burns"},
    {"waylon_smithers", "Waylon Smithers"},
    {"smithers", "Waylon Smithers"},
    {"ned_flanders", "Ned Flanders"},
    {"flanders", "Ned Flanders"},
    {"rod_flanders", "Rod Flanders"},
    {"todd_flanders", "Todd Flanders"},
    {"principal_skinner", "Principal Skinner"},
    {"seymour_skinner", "Principal Skinner"},
    {"skinner", "Principal Skinner"},
    {"mayor_quimby", "Mayor Quimby"},
    {"quimby", "Mayor Quimby"},
    {"kent_brockman", "Kent Brockman"},
    {"brockman", "Kent Brockman"},
    {"krusty_the_clown", "Krusty the Clown"},
    {"krustytheclown", "Krusty the Clown"},
    {"krusty", "Krusty the Clown"},
    {"sideshow_bob", "Sideshow Bob"},
    {"sideshowbob", "Sideshow Bob"},
    {"sideshow_mel", "Sideshow Mel"},
    {"sideshowmel", "Sideshow Mel"},
    {"bumblebee_man", "Bumblebee Man"},
    {"bumblebeeman", "Bumblebee Man"},
    {"sea_captain", "Sea Captain"},
    {"seacaptain", "Sea Captain"},
    {"hans_moleman", "Hans Moleman"},
    {"moleman", "Hans Moleman"},
    {"fat_tony", "Fat Tony"},
    {"fattony", "Fat Tony"},
    {"squeaky_voiced_teen", "Squeaky-Voiced Teen"},
    {"squeakyvoicedteen", "Squeaky-Voiced Teen"},
    {"dr_hibbert", "Dr. Hibbert"},
    {"hibbert", "Dr. Hibbert"},
    {"dr_nick", "Dr. Nick"},
    {"drnick", "Dr. Nick"},
    {"rev_lovejoy", "Reverend Lovejoy"},
    {"lovejoy", "Reverend Lovejoy"},
    {"grampa", "Grampa Simpson"},
    {"grandpa", "Grampa Simpson"},
    {"milhouse", "Milhouse Van Houten"},
    {"nelson", "Nelson Muntz"},
    {"ralph", "Ralph Wiggum"},
    {"martin_prince", "Martin Prince"},
    {"kearney", "Kearney Zzyzwicz"},
    {"jimbo", "Jimbo Jones"},
    {"dolph", "Dolph Starbeam"},
    {"otto_mann", "Otto Mann"},
    {"barney_gumble", "Barney Gumble"},
    {"barney", "Barney Gumble"},
    {"moe_szyslak", "Moe Szyslak"},
    {"apu_nahasapeemapetilon", "Apu"},
    {"cletus", "Cletus Spuckler"},
    {"snake_jailbird", "Snake Jailbird"},
    {"duffman", "Duffman"},
    {"disco_stu", "Disco Stu"},
    {"discostu", "Disco Stu"},
    {"jasper", "Jasper Beardly"},
    {"patty", "Patty Bouvier"},
    {"selma", "Selma Bouvier"},
    {"lenny", "Lenny Leonard"},
    {"carl_carlson", "Carl Carlson"},
    {"officer_lou", "Officer Lou"},
    {"officer_eddie", "Officer Eddie"},
    {"kang", "Kang"},
    {"kodos", "Kodos"},
    {"lard_lad", "Lard Lad Statue"},
    {"lardlad", "Lard Lad Statue"},
    {"matt_groening", "Matt Groening"},
    {"groening", "Matt Groening"},
    {"shakespeare", "William Shakespeare"},
    {"ben_franklin", "Ben Franklin"},
    {"franklin", "Ben Franklin"},
    {"bartman", "Bartman"},
    {"clobbergirl", "Clobber Girl"},
    {"homergod", "Homer Ball"},
    {"gummivenus", "Gummi Venus"},
    {"whitechocolate", "White Chocolate Bunny"},
    {"chocolatebunny", "Chocolate Bunny"},
    {"sumo", "Sumo Wrestler"},
    {"samurai", "Samurai Warrior"},
    {"redcoat", "British Redcoat"},
    {"lumberjack", "Lumberjack"},
    {"securityguard", "Security Guard"},
    {"footballplayer", "Football Player"},
};

// Resource names and live objects use the same labels. Keep the original
// name alongside this display name so variants and source searches stay exact.
inline std::string FriendlyAssetName(std::string raw) {
  if (const auto slash = raw.find_last_of("/\\"); slash != std::string::npos)
    raw.erase(0, slash + 1);
  std::string lower = raw;
  for (char& c : lower)
    c = char(std::tolower(uint8_t(c)));
  for (const auto& [key, label] : kAssetLabels)
    if (lower.find(key) != std::string::npos)
      return label;
  for (const char* extension : {".meta.xml", ".mmpart", ".dff", ".cha", ".bnk"})
    if (lower.ends_with(extension)) {
      raw.resize(raw.size() - std::strlen(extension));
      lower.resize(raw.size());
      break;
    }
  for (const char* prefix : {"spr_", "scd_", "eb_", "mob_", "chu_", "gts_", "sss_", "80b_",
                             "moh_", "nq_", "bb_", "int_", "brt_"})
    if (lower.starts_with(prefix)) {
      raw.erase(0, std::strlen(prefix));
      lower.erase(0, std::strlen(prefix));
      break;
    }
  for (const char* suffix : {"_mmpart", "_parts", "_des", "_placeholder", "_geo"})
    if (lower.ends_with(suffix)) {
      raw.resize(raw.size() - std::strlen(suffix));
      lower.resize(raw.size());
    }
  while (!raw.empty() && (std::isdigit(uint8_t(raw.back())) || raw.back() == '_'))
    raw.pop_back();
  std::string pretty;
  for (size_t i = 0; i < raw.size(); ++i) {
    const auto c = uint8_t(raw[i]);
    if (c == '_' || c == '-') {
      if (!pretty.empty() && pretty.back() != ' ')
        pretty += ' ';
    } else {
      if (i && std::isupper(c) && std::islower(uint8_t(raw[i - 1])) &&
          !pretty.empty() && pretty.back() != ' ')
        pretty += ' ';
      pretty += char(std::tolower(c));
    }
  }
  while (!pretty.empty() && pretty.back() == ' ')
    pretty.pop_back();
  if (!pretty.empty())
    pretty[0] = char(std::toupper(uint8_t(pretty[0])));
  return pretty;
}

}  // namespace simpsons::modkit
