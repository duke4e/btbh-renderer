#define CGLTF_IMPLEMENTATION
#include "cgltf.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>

int main(int argc, char **argv)
{
  if(argc < 3)
  {
    std::cerr << "usage: find-cgltf-index-count.exe <asset-root> <index-count> "
                 "[index-count ...]\n";
    return 2;
  }

  const std::filesystem::path root(argv[1]);
  std::unordered_set<cgltf_size> expected;
  for(int argumentIndex = 2; argumentIndex < argc; argumentIndex++)
    expected.insert(static_cast<cgltf_size>(std::stoull(argv[argumentIndex])));

  std::size_t imported = 0;
  std::size_t failed = 0;
  std::size_t matches = 0;
  for(const auto &entry : std::filesystem::recursive_directory_iterator(root))
  {
    if(!entry.is_regular_file())
      continue;
    std::string extension = entry.path().extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if(extension != ".gltf" && extension != ".glb")
      continue;

    cgltf_options options = {};
    cgltf_data *data = nullptr;
    cgltf_result result =
        cgltf_parse_file(&options, entry.path().string().c_str(), &data);
    if(result == cgltf_result_success)
      result = cgltf_load_buffers(&options, data, entry.path().string().c_str());
    if(result == cgltf_result_success)
      result = cgltf_validate(data);
    if(result != cgltf_result_success || data == nullptr)
    {
      failed++;
      cgltf_free(data);
      continue;
    }

    imported++;
    for(cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; nodeIndex++)
    {
      const cgltf_node &node = data->nodes[nodeIndex];
      if(node.mesh == nullptr)
        continue;
      for(cgltf_size primitiveIndex = 0;
          primitiveIndex < node.mesh->primitives_count;
          primitiveIndex++)
      {
        const cgltf_primitive &primitive = node.mesh->primitives[primitiveIndex];
        if(primitive.type != cgltf_primitive_type_triangles)
          continue;
        const cgltf_size indexCount =
            primitive.indices != nullptr
                ? primitive.indices->count
                : primitive.attributes_count > 0 &&
                          primitive.attributes[0].data != nullptr
                      ? primitive.attributes[0].data->count
                      : 0;
        if(!expected.contains(indexCount))
          continue;

        matches++;
        const cgltf_size meshIndex =
            static_cast<cgltf_size>(node.mesh - data->meshes);
        const cgltf_size materialIndex =
            primitive.material != nullptr
                ? static_cast<cgltf_size>(primitive.material - data->materials)
                : data->materials_count;
        std::cout << entry.path().string() << "\tindices=" << indexCount
                  << "\tnode=" << nodeIndex << "\tmesh=" << meshIndex
                  << "\tprimitive=" << primitiveIndex
                  << "\tmaterial=" << materialIndex
                  << "\tmaterialName="
                  << (primitive.material != nullptr &&
                              primitive.material->name != nullptr
                          ? primitive.material->name
                          : "")
                  << "\tdoubleSided="
                  << (primitive.material != nullptr &&
                              primitive.material->double_sided
                          ? 1
                          : 0)
                  << "\n";
      }
    }
    cgltf_free(data);
  }

  std::cerr << "imported=" << imported << " failed=" << failed
            << " matches=" << matches << "\n";
  return matches == 0 ? 1 : 0;
}
