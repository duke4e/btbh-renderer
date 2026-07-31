#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "renderdoc_replay.h"

REPLAY_PROGRAM_MARKER()

namespace
{
struct FlatAction
{
  uint32_t eventId;
  std::string name;
  uint32_t numIndices;
  uint32_t numInstances;
  ResourceId target;
};

struct TimedEvent
{
  uint32_t eventId;
  std::string group;
  std::string target;
  std::string action;
  uint32_t numIndices;
  uint32_t numInstances;
  double gpuMs;
  std::string pipeline;
  std::string vertexShader;
  std::string fragmentShader;
  std::string vertexBuffer;
  std::string indexBuffer;
  uint32_t cullMode = 0;
};

void flattenActions(const rdcarray<ActionDescription> &actions, const SDFile &structured,
                    std::vector<FlatAction> &flat)
{
  for(const ActionDescription &action : actions)
  {
    ResourceId target = action.depthOut;
    if(target == ResourceId::Null())
    {
      for(const ResourceId output : action.outputs)
      {
        if(output != ResourceId::Null())
        {
          target = output;
          break;
        }
      }
    }
    flat.push_back(
        {action.eventId, action.GetName(structured).c_str(), action.numIndices,
         action.numInstances, target});
    flattenActions(action.children, structured, flat);
  }
}

double counterSeconds(const CounterResult &result, const CounterDescription &description)
{
  if(description.resultType == CompType::Float && description.resultByteWidth == sizeof(double))
    return result.value.d;
  if(description.resultType == CompType::Float && description.resultByteWidth == sizeof(float))
    return static_cast<double>(result.value.f);
  if(description.resultByteWidth == sizeof(uint64_t))
    return static_cast<double>(result.value.u64);
  return static_cast<double>(result.value.u32);
}

std::string jsonEscape(const std::string &value)
{
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for(char c : value)
  {
    switch(c)
    {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default: escaped += c; break;
    }
  }
  return escaped;
}
} // namespace

int main(int argc, char **argv)
{
  if(argc != 2)
  {
    std::cerr << "usage: renderdoc-pass-timings.exe <capture.rdc>\n";
    return 2;
  }

  GlobalEnvironment environment;
  rdcarray<rdcstr> replayArgs;
  std::cerr << "initialising replay\n";
  RENDERDOC_InitialiseReplay(environment, replayArgs);

  ICaptureFile *capture = RENDERDOC_OpenCaptureFile();
  if(capture == nullptr)
  {
    std::cerr << "RENDERDOC_OpenCaptureFile returned null\n";
    RENDERDOC_ShutdownReplay();
    return 3;
  }

  ResultDetails openFile = capture->OpenFile(argv[1], "", nullptr);
  if(!openFile.OK())
  {
    std::cerr << "OpenFile failed with code " << static_cast<uint32_t>(openFile.code) << "\n";
    capture->Shutdown();
    RENDERDOC_ShutdownReplay();
    return 4;
  }

  ReplayOptions replayOptions;
  std::cerr << "opening capture\n";
  rdcpair<ResultDetails, IReplayController *> opened =
      capture->OpenCapture(replayOptions, nullptr);
  if(!opened.first.OK() || opened.second == nullptr)
  {
    std::cerr << "OpenCapture failed with code " << static_cast<uint32_t>(opened.first.code) << "\n";
    capture->Shutdown();
    RENDERDOC_ShutdownReplay();
    return 5;
  }

  IReplayController *controller = opened.second;
  const SDFile &structured = controller->GetStructuredFile();
  std::vector<FlatAction> actions;
  flattenActions(controller->GetRootActions(), structured, actions);
  std::sort(actions.begin(), actions.end(),
            [](const FlatAction &a, const FlatAction &b) { return a.eventId < b.eventId; });
  std::map<uint32_t, std::string> actionNames;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> actionWork;
  std::map<uint32_t, ResourceId> actionTargets;
  for(const FlatAction &action : actions)
  {
    actionNames[action.eventId] = action.name;
    actionWork[action.eventId] = {action.numIndices, action.numInstances};
    actionTargets[action.eventId] = action.target;
  }

  rdcarray<GPUCounter> requested;
  requested.push_back(GPUCounter::EventGPUDuration);
  CounterDescription description = controller->DescribeCounter(GPUCounter::EventGPUDuration);
  std::cerr << "fetching cold counters\n";
  rdcarray<CounterResult> coldResults = controller->FetchCounters(requested);
  std::cerr << "fetching warm counters\n";
  rdcarray<CounterResult> results = controller->FetchCounters(requested);
  std::cerr << "processing results\n";
  std::sort(results.begin(), results.end(),
            [](const CounterResult &a, const CounterResult &b) { return a.eventId < b.eventId; });

  std::map<std::string, double> groupMs;
  std::map<std::string, uint32_t> groupEvents;
  std::map<std::string, uint64_t> groupIndexInstances;
  std::map<std::string, double> targetMs;
  std::map<std::string, uint32_t> targetEvents;
  std::map<std::string, uint64_t> targetIndexInstances;
  std::map<ResourceId, std::string> resourceNames;
  for(const ResourceDescription &resource : controller->GetResources())
    resourceNames[resource.resourceId] = resource.name.c_str();
  for(const TextureDescription &texture : controller->GetTextures())
  {
    resourceNames[texture.resourceId] +=
        " " + std::to_string(texture.width) + "x" + std::to_string(texture.height);
  }
  std::vector<std::pair<uint32_t, std::string>> markers;
  for(const FlatAction &action : actions)
  {
    if(action.name.rfind("BTBH ", 0) == 0)
      markers.push_back({action.eventId, action.name});
  }

  size_t markerIndex = 0;
  std::string currentGroup = "Unmarked";
  double totalMs = 0.0;
  std::vector<TimedEvent> timedEvents;
  double coldTotalMs = 0.0;
  for(const CounterResult &result : coldResults)
    coldTotalMs += counterSeconds(result, description) * 1000.0;
  for(const CounterResult &result : results)
  {
    while(markerIndex < markers.size() && markers[markerIndex].first <= result.eventId)
    {
      currentGroup = markers[markerIndex].second;
      markerIndex++;
    }

    const double milliseconds = counterSeconds(result, description) * 1000.0;
    const ResourceId targetResource = actionTargets[result.eventId];
    const std::string target = targetResource == ResourceId::Null()
                                   ? "No Target"
                                   : resourceNames[targetResource];
    totalMs += milliseconds;
    groupMs[currentGroup] += milliseconds;
    groupEvents[currentGroup]++;
    const uint64_t indexInstances =
        static_cast<uint64_t>(actionWork[result.eventId].first) *
        static_cast<uint64_t>(actionWork[result.eventId].second);
    groupIndexInstances[currentGroup] += indexInstances;
    targetMs[target] += milliseconds;
    targetEvents[target]++;
    targetIndexInstances[target] += indexInstances;
    timedEvents.push_back(
        {result.eventId, currentGroup, target, actionNames[result.eventId],
         actionWork[result.eventId].first, actionWork[result.eventId].second, milliseconds});
  }
  std::sort(timedEvents.begin(), timedEvents.end(),
            [](const TimedEvent &a, const TimedEvent &b) { return a.gpuMs > b.gpuMs; });
  const auto populateEventState = [&](TimedEvent &event) {
    controller->SetFrameEvent(event.eventId, true);
    const VKPipe::State *state = controller->GetVulkanPipelineState();
    if(state == nullptr)
      return;
    event.pipeline = resourceNames[state->graphics.pipelineResourceId];
    event.vertexShader = resourceNames[state->vertexShader.resourceId];
    event.fragmentShader = resourceNames[state->fragmentShader.resourceId];
    if(!state->vertexInput.vertexBuffers.empty())
      event.vertexBuffer = resourceNames[state->vertexInput.vertexBuffers[0].resourceId];
    event.indexBuffer = resourceNames[state->inputAssembly.indexBuffer.resourceId];
    event.cullMode = static_cast<uint32_t>(state->rasterizer.cullMode);
  };

  std::cout << std::fixed << std::setprecision(6);
  std::cout << "{\n";
  std::cout << "  \"capture\": \"" << jsonEscape(argv[1]) << "\",\n";
  std::cout << "  \"counter\": \"" << jsonEscape(description.name.c_str()) << "\",\n";
  std::cout << "  \"counterResultType\": " << static_cast<uint32_t>(description.resultType) << ",\n";
  std::cout << "  \"counterResultByteWidth\": " << description.resultByteWidth << ",\n";
  std::cout << "  \"actionCount\": " << actions.size() << ",\n";
  std::cout << "  \"counterEventCount\": " << results.size() << ",\n";
  std::cout << "  \"markerCount\": " << markers.size() << ",\n";
  std::cout << "  \"coldTotalGpuMs\": " << coldTotalMs << ",\n";
  std::cout << "  \"totalGpuMs\": " << totalMs << ",\n";
  std::cout << "  \"groups\": [\n";

  bool first = true;
  for(const auto &entry : groupMs)
  {
    if(!first)
      std::cout << ",\n";
    first = false;
    std::cout << "    {\"name\": \"" << jsonEscape(entry.first) << "\", \"gpuMs\": "
              << entry.second << ", \"eventCount\": " << groupEvents[entry.first]
              << ", \"indexInstances\": " << groupIndexInstances[entry.first] << "}";
  }

  std::cout << "\n  ],\n";
  std::cout << "  \"targets\": [\n";
  first = true;
  for(const auto &entry : targetMs)
  {
    if(!first)
      std::cout << ",\n";
    first = false;
    std::cout << "    {\"name\": \"" << jsonEscape(entry.first) << "\", \"gpuMs\": "
              << entry.second << ", \"eventCount\": " << targetEvents[entry.first]
              << ", \"indexInstances\": " << targetIndexInstances[entry.first] << "}";
  }
  std::cout << "\n  ],\n";
  std::cout << "  \"topEvents\": [\n";
  const size_t topEventCount = std::min<size_t>(timedEvents.size(), 20);
  for(size_t i = 0; i < topEventCount; i++)
  {
    if(i > 0)
      std::cout << ",\n";
    TimedEvent &event = timedEvents[i];
    populateEventState(event);
    std::cout << "    {\"eventId\": " << event.eventId << ", \"group\": \""
              << jsonEscape(event.group) << "\", \"target\": \"" << jsonEscape(event.target)
              << "\", \"action\": \"" << jsonEscape(event.action)
              << "\", \"numIndices\": " << event.numIndices << ", \"numInstances\": "
              << event.numInstances << ", \"gpuMs\": " << event.gpuMs
              << ", \"pipeline\": \"" << jsonEscape(event.pipeline)
              << "\", \"vertexShader\": \"" << jsonEscape(event.vertexShader)
              << "\", \"fragmentShader\": \"" << jsonEscape(event.fragmentShader)
              << "\", \"vertexBuffer\": \"" << jsonEscape(event.vertexBuffer)
              << "\", \"indexBuffer\": \"" << jsonEscape(event.indexBuffer)
              << "\", \"cullMode\": " << event.cullMode << "}";
  }
  std::cout << "\n  ],\n";
  std::cout << "  \"hotWorkEvents\": [\n";
  first = true;
  for(TimedEvent &event : timedEvents)
  {
    if(event.numIndices != 9126 || event.numInstances != 16)
      continue;
    populateEventState(event);
    if(!first)
      std::cout << ",\n";
    first = false;
    std::cout << "    {\"eventId\": " << event.eventId << ", \"group\": \""
              << jsonEscape(event.group) << "\", \"target\": \"" << jsonEscape(event.target)
              << "\", \"gpuMs\": " << event.gpuMs << ", \"pipeline\": \""
              << jsonEscape(event.pipeline) << "\", \"vertexShader\": \""
              << jsonEscape(event.vertexShader) << "\", \"fragmentShader\": \""
              << jsonEscape(event.fragmentShader) << "\", \"vertexBuffer\": \""
              << jsonEscape(event.vertexBuffer) << "\", \"indexBuffer\": \""
              << jsonEscape(event.indexBuffer) << "\", \"cullMode\": "
              << event.cullMode << "}";
  }
  std::cout << "\n  ],\n";
  std::cout << "  \"markers\": [\n";
  for(size_t i = 0; i < markers.size(); i++)
  {
    if(i > 0)
      std::cout << ",\n";
    std::cout << "    {\"eventId\": " << markers[i].first << ", \"name\": \""
              << jsonEscape(markers[i].second) << "\"}";
  }
  std::cout << "\n  ]\n";
  std::cout << "}\n";

  controller->Shutdown();
  capture->Shutdown();
  RENDERDOC_ShutdownReplay();
  return 0;
}
