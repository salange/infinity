#include "lookdev.hpp"
#include "city/materials.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace cb {
Scene generate_lookdev(const std::string &kit, const std::string &manifest) {
  if (kit.empty())
    throw std::runtime_error("lookdev requires the authored kit");
  Scene scene;
  scene.materials = make_materials();
  scene.asset_library = load_asset_library(
      kit, static_cast<std::uint32_t>(scene.materials.size()));
  scene.materials.insert(scene.materials.end(),
                         scene.asset_library.materials.begin(),
                         scene.asset_library.materials.end());
  std::ifstream input(manifest);
  if (!input)
    throw std::runtime_error("lookdev assembly manifest missing");
  std::string line;
  int line_number = 0;
  bool camera = false;
  while (std::getline(input, line)) {
    ++line_number;
    std::istringstream values(line);
    std::string kind;
    values >> kind;
    if (kind.empty() || kind[0] == '#')
      continue;
    if (kind == "camera") {
      auto &p = scene.camera_position;
      auto &t = scene.camera_target;
      values >> p.x >> p.y >> p.z >> t.x >> t.y >> t.z >>
          scene.camera_fov_degrees;
      camera = true;
    } else if (kind == "instance") {
      std::string name;
      Vec3 p, scale;
      float yaw;
      values >> name >> p.x >> p.y >> p.z >> yaw >> scale.x >> scale.y >>
          scale.z;
      if (values)
        add_asset_instance(scene, name, p, radians(yaw), scale);
    } else if (kind == "box") {
      std::string name;
      Vec3 p, half;
      values >> name >> p.x >> p.y >> p.z >> half.x >> half.y >> half.z;
      std::size_t material = 0;
      for (; material < scene.materials.size(); ++material)
        if (scene.materials[material].name == name)
          break;
      if (material == scene.materials.size())
        throw std::runtime_error("unknown lookdev box material " + name);
      if (values)
        Emit(&scene.opaque, static_cast<Mat>(material)).box(p, half);
    } else if (kind == "light") {
      PointLight l;
      values >> l.position.x >> l.position.y >> l.position.z >> l.radius >>
          l.color.x >> l.color.y >> l.color.z >> l.intensity;
      if (values)
        scene.lights.push_back(l);
    } else if (kind == "sun") {
      auto &d = scene.sun_direction;
      auto &c = scene.sun_irradiance;
      values >> d.x >> d.y >> d.z >> c.x >> c.y >> c.z;
      scene.has_lighting_override = true;
    } else if (kind == "exposure") {
      values >> scene.lighting_exposure;
    } else
      throw std::runtime_error("unknown lookdev record " + kind);
    if (!values)
      throw std::runtime_error("invalid lookdev record at line " +
                               std::to_string(line_number));
  }
  if (!camera)
    throw std::runtime_error("lookdev camera missing");
  scene.finalize_draws();
  scene.city_size = "architectural material assembly";
  scene.city_radius = 24;
  return scene;
}
} // namespace cb
