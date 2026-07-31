#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "renderdoc_replay.h"

REPLAY_PROGRAM_MARKER()

int main(int argc, char **argv)
{
  if(argc == 1)
  {
    GlobalEnvironment environment;
    rdcarray<rdcstr> replayArgs;
    RENDERDOC_InitialiseReplay(environment, replayArgs);
    uint32_t ident = 0;
    while((ident = RENDERDOC_EnumerateRemoteTargets("", ident)) != 0)
      std::cout << ident << "\n";
    RENDERDOC_ShutdownReplay();
    return 0;
  }

  if(argc != 2 && argc != 3)
  {
    std::cerr << "usage: renderdoc-trigger-capture.exe [target-ident [delay-ms]]\n";
    return 2;
  }

  const uint32_t delayMs = argc == 3
      ? static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10))
      : 0;
  GlobalEnvironment environment;
  rdcarray<rdcstr> replayArgs;
  RENDERDOC_InitialiseReplay(environment, replayArgs);
  const uint32_t ident = std::string(argv[1]) == "auto"
      ? RENDERDOC_EnumerateRemoteTargets("", 0)
      : static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));

  ITargetControl *target =
      RENDERDOC_CreateTargetControl("", ident, "BTBH profiler", true);
  if(target == nullptr || !target->Connected())
  {
    std::cerr << "Could not connect to target " << ident << "\n";
    if(target != nullptr)
      target->Shutdown();
    RENDERDOC_ShutdownReplay();
    return 3;
  }

  std::cout << "Connected target=" << target->GetTarget().c_str()
            << " api=" << target->GetAPI().c_str() << " pid=" << target->GetPID() << "\n";
  for(uint32_t elapsed = 0; elapsed < delayMs && target->Connected(); elapsed += 50)
  {
    target->ReceiveMessage(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  target->TriggerCapture(1);

  for(int i = 0; i < 120 && target->Connected(); i++)
  {
    target->ReceiveMessage(nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  target->Shutdown();
  RENDERDOC_ShutdownReplay();
  return 0;
}
