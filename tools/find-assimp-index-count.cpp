#include <assimp/cimport.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

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
    std::cerr << "usage: find-assimp-index-count.exe <asset-root> <index-count> "
                 "[index-count ...]\n";
    return 2;
  }

  const std::filesystem::path root(argv[1]);
  std::unordered_set<unsigned int> expected;
  for(int argumentIndex = 2; argumentIndex < argc; argumentIndex++)
    expected.insert(static_cast<unsigned int>(std::stoul(argv[argumentIndex])));
  const std::unordered_set<std::string> extensions = {
      ".obj", ".fbx", ".gltf", ".glb", ".dae", ".3ds", ".ply", ".stl"};
  const unsigned int flags =
      aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenUVCoords |
      aiProcess_FlipUVs | aiProcess_RemoveRedundantMaterials | aiProcess_GenSmoothNormals |
      aiProcess_CalcTangentSpace | aiProcess_PreTransformVertices | aiProcess_SortByPType |
      aiProcess_ImproveCacheLocality;

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
    if(!extensions.contains(extension))
      continue;

    const aiScene *scene = aiImportFile(entry.path().string().c_str(), flags);
    if(scene == nullptr)
    {
      failed++;
      continue;
    }
    imported++;
    for(unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; meshIndex++)
    {
      const aiMesh *mesh = scene->mMeshes[meshIndex];
      if(mesh == nullptr || (mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0)
        continue;
      const unsigned int indexCount = mesh->mNumFaces * 3;
      if(expected.contains(indexCount))
      {
        matches++;
        std::cout << entry.path().string() << "\tindices=" << indexCount
                  << "\tmesh=" << meshIndex
                  << "\tvertices=" << mesh->mNumVertices << "\tfaces=" << mesh->mNumFaces
                  << "\tmaterial=" << mesh->mMaterialIndex << "\n";
      }
    }
    aiReleaseImport(scene);
  }

  std::cerr << "imported=" << imported << " failed=" << failed << " matches=" << matches << "\n";
  return matches == 0 ? 1 : 0;
}
