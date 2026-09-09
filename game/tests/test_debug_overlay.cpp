#include "../app/src/debug_state.hpp"
#include <doctest/doctest.h>

using namespace inf::app;

TEST_CASE("debug overlay: independent sections, all toggle, reserved digits, "
          "and hidden input") {
  DebugState state;
  std::array<bool, 10> digits{};
  digits[1] = true;
  state.keys(false, digits, true);
  CHECK(state.expanded[0]); // hidden keys have no effect
  state.keys(true, digits, true);
  REQUIRE(state.visible);
  CHECK(state.expanded[0]); // held key is not a fresh press
  state.keys(false, {}, true);
  state.keys(false, digits, true);
  CHECK_FALSE(state.expanded[0]);
  CHECK(state.expanded[1]);
  state.keys(false, digits, true);
  CHECK_FALSE(state.expanded[0]); // key repeat does not oscillate
  state.keys(false, {}, true);
  digits = {};
  digits[0] = true;
  state.keys(false, digits, true);
  CHECK(state.expanded[0]); // mixed state expands all
  state.keys(false, {}, true);
  state.keys(false, digits, true);
  for (bool open : state.expanded)
    CHECK_FALSE(open);
  state.keys(false, {}, true);
  digits = {};
  digits[4] = true;
  digits[9] = true;
  state.keys(false, digits, true);
  CHECK(state.expanded[3]);
  CHECK_FALSE(state.expanded[0]);
  state.keys(true, {}, true);
  CHECK_FALSE(state.visible);
  state.keys(false, {}, true);
  state.keys(true, {}, false);
  CHECK_FALSE(state.visible); // unfocused global key is ignored
}

TEST_CASE("debug overlay: mouse headers follow folding, resizing and absent "
          "contexts") {
  DebugState state;
  state.visible = true;
  DebugSnapshot snapshot;
  snapshot.sections[0] = {"site", "growth", "mesh"};
  snapshot.sections[1] = {"planet", "radius"};
  snapshot.sections[2] = {"system"};
  snapshot.sections[3] = {"galaxy"};
  DebugLayout layout(1280, 720, state, snapshot);
  CHECK(layout.hit(layout.left + 10, layout.headers[1] + 5) == 1);
  CHECK(layout.hit(layout.left - 1, layout.headers[1] + 5) == -1);
  CHECK(layout.hit(layout.left + 10, layout.top) == -1);
  state.toggle_section(0);
  DebugLayout folded(1280, 720, state, snapshot);
  CHECK(folded.headers[1] < layout.headers[1]);
  CHECK(folded.hit(folded.left + 10, folded.headers[2] + 5) == 2);
  snapshot.sections[0].clear();
  snapshot.sections[1].clear();
  DebugLayout absent(800, 600, state, snapshot);
  CHECK(absent.hit(absent.left + 5, absent.headers[3] + 5) == 3);
  CHECK(snapshot.sections[2].size() == 1);
  CHECK(snapshot.sections[3].size() == 1);
  CHECK(absent.left + absent.width <= 800);
}

TEST_CASE(
    "debug overlay: proximity includes altitude and excludes absent objects") {
  CHECK(debug_body_near(0, 1000));
  CHECK(debug_body_near(1000, 1000));
  CHECK_FALSE(debug_body_near(1001, 1000));
  CHECK_FALSE(debug_body_near(0, 0));
  CHECK(debug_site_near(1999, 1000));
  CHECK_FALSE(debug_site_near(2001, 1000));
  CHECK_FALSE(debug_site_near(0, 0));
}
