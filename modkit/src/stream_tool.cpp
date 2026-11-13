#include "streams.h"
#include "models.h"
#include "meshes.h"
#include "textures.h"
#include "resource_id.h"
#include "assets.h"

#include <fstream>
#include <iostream>
#include <charconv>
#include <chrono>
#include <thread>

using namespace simpsons::modkit;

namespace {
size_t Index(std::string_view value) {
  size_t index;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size())
    throw Failure("invalid_argument", "Resource indices are nonnegative whole numbers");
  return index;
}
std::vector<uint8_t> ModelBytes(const fs::path& source) {
  const auto size = fs::file_size(source);
  if (size > 16 * 1024 * 1024)
    throw Failure("resource_limit", "Native asset files are limited to 16 MiB");
  std::ifstream input(source, std::ios::binary);
  std::vector<uint8_t> bytes(size);
  if (!input.read(reinterpret_cast<char*>(bytes.data()), size) || input.peek() != EOF)
    throw Failure("read_failed", "Cannot read a stable model file");
  return bytes;
}
void ModelOutput(const fs::path& destination, std::span<const uint8_t> bytes) {
  if (fs::exists(destination))
    throw Failure("invalid_path", "Choose a new asset output file");
  WriteText(destination,
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}
int Run(int argc, const char* const* argv) {
  try {
    if (argc < 3)
      throw Failure(
          "usage",
          "Usage: modkit_streams inspect FILE | extract FILE FOLDER | read FILE INDEX OUTPUT | "
          "replace FILE INDEX RESOURCE OUTPUT | rebuild FILE OUTPUT | index GAMEDATA OUTPUT | "
          "catalogs GAMEDATA OUTPUT [--overlay FOLDER ...] | "
          "model-export STREAM ENTRY NAME_OR_ID OUTPUT | model-compile STREAM ENTRY DOCUMENT "
          "OUTPUT | "
          "model-inspect RESOURCE [OUTPUT] | model-edit RESOURCE DOCUMENT OUTPUT");
    const std::string command(argv[1]);
    const auto source = fs::absolute(Utf8Path(argv[2]));
    if (command == "asset-library") {
      if (argc < 4 || (argc - 4) % 2)
        throw Failure("usage", "asset-library needs GAMEDATA OUTPUT and optional --overlay folders");
      std::vector<fs::path> roots;
      for (int arg = 4; arg < argc; arg += 2) {
        if (std::string_view(argv[arg]) != "--overlay")
          throw Failure("usage", "Use --overlay FOLDER for higher-priority mod content");
        roots.push_back(fs::absolute(Utf8Path(argv[arg + 1])));
      }
      roots.push_back(source);
      AssetIndex index(std::move(roots));
      auto snapshot = index.Get();
      while (!snapshot->ready) {
        std::cout << Json{{"phase", "indexing"}, {"files", snapshot->files},
                          {"total_files", snapshot->total_files}}.dump() << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        snapshot = index.Get();
      }
      Json assets = Json::array(), objects = Json::array();
      for (const auto& asset : snapshot->assets) {
        Json locations = Json::array();
        for (const auto& location : asset.locations)
          locations.push_back({{"stream", location.stream}, {"entry", location.entry},
                               {"offset", location.offset}, {"bytes", location.bytes},
                               {"container", location.container}, {"resource_id", location.resource_id}});
        assets.push_back({{"id", asset.id}, {"name", asset.name}, {"source", asset.source},
                          {"category", asset.category}, {"native_name", asset.native_name},
                          {"native_class", asset.native_class}, {"object_count", asset.objects.size()},
                          {"objects", asset.objects}, {"locations", std::move(locations)}});
      }
      for (const auto& object : snapshot->objects)
        objects.push_back({{"guid", object.guid}, {"model_resource", object.model_resource},
                           {"character_resource", object.character_resource}, {"stream", object.stream},
                           {"class_hash", object.hash}, {"entry", object.entry},
                           {"record_offset", object.offset}, {"fallback_resource", object.fallback_resource}});
      const auto text = Json{{"api", 1}, {"ready", true}, {"files", snapshot->files},
                              {"categories", snapshot->categories}, {"errors", snapshot->errors},
                              {"items", std::move(assets)}, {"objects", std::move(objects)}}.dump();
      ModelOutput(fs::absolute(Utf8Path(argv[3])),
                  {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      return 0;
    }
    if (command == "texture-inspect" || command == "texture-edit") {
      if (command == "texture-edit") {
        if (argc != 5)
          throw Failure("usage", "texture-edit needs RESOURCE DOCUMENT OUTPUT");
        const TextureResource resource(ModelBytes(source));
        const auto document = Json::parse(ReadText(fs::absolute(Utf8Path(argv[3])), 16 * 1024 * 1024));
        ModelOutput(fs::absolute(Utf8Path(argv[4])), resource.Apply(document));
        return 0;
      }
      if (argc != 3 && argc != 4)
        throw Failure("usage", "texture-inspect needs RESOURCE [OUTPUT]");
      const auto text = TextureResource(ModelBytes(source)).Describe().dump(2);
      if (argc == 3)
        std::cout << text << '\n';
      else
        ModelOutput(fs::absolute(Utf8Path(argv[3])),
                    {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      return 0;
    }
    if (command == "texture-image" || command == "texture-reimport") {
      const bool import = command == "texture-reimport";
      if (argc != (import ? 8 : 7))
        throw Failure("usage",
                      "texture-image/reimport needs STREAM ENTRY DOCUMENT TEXTURE [DDS] OUTPUT");
      auto document = Json::parse(ReadText(fs::absolute(Utf8Path(argv[4])), 16 * 1024 * 1024));
      const StreamFile stream(source);
      const auto entry = Index(argv[3]);
      const auto infos = stream.DescribeAll(entry);
      const ResourceInfo* match = nullptr;
      for (const auto& info : infos)
        if (info.resource_id == document.at("resource_id").get<std::string>()) {
          if (match)
            throw Failure("ambiguous_resource", "Choose an unambiguous texture dictionary");
          match = &info;
        }
      if (!match)
        throw Failure("missing_resource", "The texture dictionary is absent from its stream");
      const auto bytes = stream.Read(entry);
      if (match->offset > bytes.size() || match->size > bytes.size() - match->offset)
        throw Failure("source_changed", "Texture source changed during inspection");
      const TextureResource original(std::span(bytes).subspan(match->offset, match->size));
      const TextureResource edited(original.Apply(document));
      const auto found = std::find_if(original.textures().begin(), original.textures().end(),
                                      [&](const auto& t) { return t.id == argv[5]; });
      if (found == original.textures().end())
        throw Failure("invalid_texture", "Choose a texture:N identity from texture inspect");
      const auto index = size_t(found - original.textures().begin());
      if (!import)
        ModelOutput(fs::absolute(Utf8Path(argv[6])), edited.Dds(index));
      else {
        const auto change = original.Import(index, ModelBytes(fs::absolute(Utf8Path(argv[6]))));
        auto& changes = document.at("changes");
        changes.erase(
            std::remove_if(changes.begin(), changes.end(),
                           [&](const auto& c) { return c.at("texture") == change.at("texture"); }),
            changes.end());
        changes.push_back(change);
        original.Apply(document);
        const auto text = document.dump(2);
        ModelOutput(fs::absolute(Utf8Path(argv[7])),
                    {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      }
      return 0;
    }
    if (command == "mesh-inspect" || command == "mesh-edit") {
      if ((command == "mesh-inspect" && argc != 3 && argc != 4) ||
          (command == "mesh-edit" && argc != 5))
        throw Failure(
            "usage",
            "mesh-inspect needs RESOURCE [OUTPUT]; mesh-edit needs RESOURCE DOCUMENT OUTPUT");
      const MeshResource resource(ModelBytes(source));
      if (command == "mesh-inspect") {
        const auto text = resource.Describe().dump(2);
        if (argc == 3)
          std::cout << text << '\n';
        else
          ModelOutput(fs::absolute(Utf8Path(argv[3])),
                      {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      } else {
        const auto document =
            Json::parse(ReadText(fs::absolute(Utf8Path(argv[3])), 16 * 1024 * 1024));
        ModelOutput(fs::absolute(Utf8Path(argv[4])), resource.Apply(document));
      }
      return 0;
    }
    if (command == "model-inspect" || command == "model-edit") {
      if ((command == "model-inspect" && argc != 3 && argc != 4) ||
          (command == "model-edit" && argc != 5))
        throw Failure(
            "usage",
            "model-inspect needs RESOURCE [OUTPUT]; model-edit needs RESOURCE CHANGES OUTPUT");
      const ModelResource resource(ModelBytes(source));
      if (command == "model-inspect") {
        auto description = resource.Definition().Describe();
        description["name"] = resource.name;
        if (argc == 3)
          std::cout << description.dump(2) << '\n';
        else {
          const auto text = description.dump(2);
          ModelOutput(fs::absolute(Utf8Path(argv[3])),
                      {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
        }
      } else {
        const auto document = Json::parse(ReadText(fs::absolute(Utf8Path(argv[3])), 1024 * 1024));
        const auto edited = resource.Definition().Apply(document);
        auto result = resource.data;
        std::copy(edited.begin(), edited.end(), result.begin() + resource.body_offset);
        ModelResource check(result);
        ModelOutput(fs::absolute(Utf8Path(argv[4])), result);
      }
      return 0;
    }
    if (command == "model-export" || command == "model-compile" || command == "mesh-export" ||
        command == "mesh-compile" || command == "texture-export" || command == "texture-compile") {
      if (argc != 6)
        throw Failure("usage",
                      "model-export/compile needs STREAM ENTRY NAME_OR_ID/DOCUMENT OUTPUT");
      const bool mesh = command.starts_with("mesh-");
      const bool texture = command.starts_with("texture-");
      const bool compile = command.ends_with("-compile");
      const auto document = compile
                                ? Json::parse(ReadText(fs::absolute(Utf8Path(argv[4])),
                                                       (mesh || texture ? 16 : 1) * 1024 * 1024))
                                : Json();
      const StreamFile stream(source);
      const auto index = Index(argv[3]);
      const auto all = stream.DescribeAll(index);
      const auto key =
          compile ? document.at("resource_id").get<std::string>() : std::string(argv[4]);
      const ResourceInfo* match = nullptr;
      for (const auto& info : all) {
        if (info.name != key && info.resource_id != key)
          continue;
        if (match)
          throw Failure("ambiguous_resource", "Choose an unambiguous resource GUID");
        match = &info;
      }
      if (!match)
        throw Failure("missing_resource", "The named resource is absent from this stream entry");
      const auto bytes = stream.Read(index);
      if (match->offset > bytes.size() || match->size > bytes.size() - match->offset)
        throw Failure("source_changed", "Resource stream changed during inspection");
      const auto selected = std::span(bytes).subspan(match->offset, match->size);
      const NamedResource resource(selected);
      if (resource.resource_id != match->resource_id || resource.name != match->name)
        throw Failure("source_changed", "Resource identity changed during inspection");
      auto description = texture ? TextureResource(selected).Describe()
                         : mesh  ? MeshResource(selected).Describe()
                                 : ModelResource(selected).Definition().Describe();
      description["name"] = resource.name;
      description["source"] = {{"stream", Utf8(source)}, {"entry", index}};
      if (compile) {
        if (texture)
          TextureResource(selected).Apply(document);
        else if (mesh)
          MeshResource(selected).Apply(document);
        else
          ModelResource(selected).Definition().ApplyVariant(document);
        Json compact = Json::object();
        for (const auto* key : {"format", "version", "resource_id", "source_sha256", "changes"})
          compact[key] = document.at(key);
        description =
            texture
                ? ValidateTextureVariants({{"version", 1}, {"textures", Json::array({compact})}})
            : mesh ? ValidateMeshVariants({{"version", 1}, {"meshes", Json::array({compact})}})
                   : ValidateModelVariants({{"version", 1}, {"models", Json::array({compact})}});
      }
      const auto text = description.dump(2);
      ModelOutput(fs::absolute(Utf8Path(argv[5])),
                  {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
      return 0;
    }
    if (command == "catalogs") {
      if (argc < 4 || (argc - 4) % 2)
        throw Failure("usage", "catalogs needs GAMEDATA OUTPUT and optional --overlay folders");
      std::vector<fs::path> roots;
      for (int arg = 4; arg < argc; arg += 2) {
        if (std::string_view(argv[arg]) != "--overlay")
          throw Failure("usage", "Use --overlay FOLDER for a higher-priority game-data layer");
        roots.push_back(fs::absolute(Utf8Path(argv[arg + 1])));
      }
      roots.push_back(source);
      std::map<std::string, fs::path> files;
      for (const auto& root : roots) {
        for (const auto& item : fs::recursive_directory_iterator(root)) {
          if (item.is_symlink() || !item.is_regular_file()) continue;
          const auto key = StreamKey(Utf8(item.path().lexically_relative(root)));
          if (key.ends_with(".str")) files.try_emplace(key, item.path());
        }
      }
      const auto sync = SynchronizeStreamCatalogs(files);
      Json patches = Json::array();
      for (const auto& [path, fields] : sync.patches)
        for (const auto& field : fields)
          patches.push_back({{"stream", path}, {"offset", field.offset}, {"bytes", field.bytes}});
      WriteText(fs::absolute(Utf8Path(argv[3])),
                Json{{"api", 1}, {"catalogs", sync.catalogs}, {"reservations", sync.reservations},
                     {"patches", std::move(patches)}}.dump(2));
      return 0;
    }
    if (command == "index") {
      if (argc != 4 && (argc != 5 || std::string_view(argv[4]) != "--deep"))
        throw Failure("usage", "index needs a game-data folder, output JSON and optional --deep");
      const bool deep = argc == 5;
      Json resources = Json::array(), errors = Json::array();
      size_t files = 0;
      for (const auto& item : fs::recursive_directory_iterator(source)) {
        if (!item.is_regular_file() || item.path().extension() != ".str")
          continue;
        ++files;
        try {
          StreamFile stream(item.path());
          for (size_t i = 0; i < stream.entries().size(); ++i) {
            const auto& entry = stream.entries()[i];
            auto descriptions = deep ? stream.DescribeAll(i) : std::vector{stream.Describe(i)};
            for (const auto& info : descriptions) {
              Json objects = Json::array();
              const auto id = [](const auto& guid) {
                return std::any_of(guid.begin(), guid.end(), [](auto word) { return word != 0; })
                           ? ResourceId(guid) : std::string();
              };
              for (const auto& object : info.objects)
                objects.push_back({{"guid", ResourceId(object.guid)}, {"class_hash", object.hash},
                                     {"model_resource", id(object.model)},
                                     {"character_resource", id(object.character)},
                                     {"fallback_resource", id(object.fallback_model)},
                                     {"record_offset", object.offset}});
              resources.push_back({{"stream", Utf8(item.path().lexically_relative(source))},
                                   {"entry", i},
                                   {"name", info.name},
                                   {"source", info.source},
                                   {"pool", entry.pool},
                                   {"codec", entry.codec},
                                   {"type", entry.pool},
                                   {"bytes", entry.size},
                                   {"stored", entry.stored},
                                   {"chunk_offset", info.offset},
                                   {"chunk_size", info.size},
                                   {"chunk_type", info.type},
                                   {"resource_id", info.resource_id},
                                   {"native_class", info.native_class},
                                   {"references", info.references}, {"objects", std::move(objects)}});
            }
          }
        } catch (const std::exception& error) {
          errors.push_back(
              {{"stream", Utf8(item.path().lexically_relative(source))}, {"error", error.what()}});
        }
      }
      const bool valid = errors.empty();
      WriteText(fs::absolute(Utf8Path(argv[3])), Json{{"api", 1},
                                                      {"files", files},
                                                      {"resources", std::move(resources)},
                                                      {"errors", std::move(errors)}}
                                                     .dump(2));
      return valid ? 0 : 1;
    }
    StreamFile stream(source);
    if (command == "read") {
      if (argc != 5)
        throw Failure("usage", "read needs FILE INDEX OUTPUT");
      const auto bytes = stream.Read(Index(argv[3]));
      const auto output = fs::absolute(Utf8Path(argv[4]));
      if (fs::exists(output))
        throw Failure("invalid_path", "Choose a new output file");
      WriteText(output,
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
      return 0;
    }
    if (command == "inspect" || command == "extract") {
      if (command == "extract" && argc != 4)
        throw Failure("usage", "extract needs a new output folder");
      fs::path destination;
      if (command == "extract") {
        destination = fs::absolute(Utf8Path(argv[3]));
        if (!fs::create_directory(destination))
          throw Failure("invalid_path", "Choose a new extraction folder");
      }
      Json entries = Json::array();
      for (size_t i = 0; i < stream.entries().size(); ++i) {
        const auto& entry = stream.entries()[i];
        const auto info = stream.Describe(i);
        Json item = {
            {"index", i},          {"pool", entry.pool},     {"codec", entry.codec},
            {"bytes", entry.size}, {"stored", entry.stored}, {"offset", entry.offset},
            {"allocation", entry.allocation}, {"read_offset", entry.read_offset},
            {"name", info.name},   {"source", info.source}};
        // Keep earlier development exports readable. These two old names
        // described the fields incorrectly; pool and codec are authoritative.
        item["type"] = entry.pool;
        item["allocator"] = entry.codec;
        if (!destination.empty()) {
          const auto name = std::to_string(i) + ".bin";
          const auto bytes = stream.Read(i);
          std::ofstream output(destination / name, std::ios::binary);
          if (!output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
            throw Failure("write_failed", "Cannot extract the resource");
          item["file"] = name;
        }
        entries.push_back(std::move(item));
      }
      Json result = {{"api", 1}, {"stream", Utf8(source)}, {"entries", std::move(entries)}};
      if (!destination.empty())
        WriteText(destination / "stream.json", result.dump(2));
      else
        std::cout << result.dump(2) << '\n';
      return 0;
    }
    if (command == "replace") {
      if (argc != 6)
        throw Failure("usage", "replace needs FILE INDEX RESOURCE OUTPUT");
      const auto index = Index(argv[3]);
      const auto text = ReadText(fs::absolute(Utf8Path(argv[4])), 512 * 1024 * 1024);
      stream.Rebuild(fs::absolute(Utf8Path(argv[5])),
                     {{index, std::vector<uint8_t>(text.begin(), text.end())}});
      return 0;
    }
    if (command == "rebuild") {
      if (argc != 4)
        throw Failure("usage", "rebuild needs FILE OUTPUT");
      stream.Rebuild(fs::absolute(Utf8Path(argv[3])));
      return 0;
    }
    throw Failure("usage", "Unknown stream command");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> arguments;
  std::vector<const char*> pointers;
  for (int i = 0; i < argc; ++i)
    arguments.push_back(Utf8(fs::path(argv[i])));
  for (const auto& argument : arguments)
    pointers.push_back(argument.c_str());
  return Run(argc, pointers.data());
}
#else
int main(int argc, char** argv) {
  return Run(argc, argv);
}
#endif
