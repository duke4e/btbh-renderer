"""Extract per-event and per-BTBH-marker GPU durations from a RenderDoc capture.

Run this through qrenderdoc's embedded Python interpreter:

    qrenderdoc --python tools/renderdoc-pass-timings.py

Set BTBH_RENDERDOC_CAPTURE to the input .rdc path and
BTBH_RENDERDOC_TIMINGS to the output JSON path.
"""

import json
import os
import sys

_output_path = os.environ.get("BTBH_RENDERDOC_TIMINGS")
if _output_path:
    with open(_output_path + ".started", "w") as _started:
        _started.write("started\n")

import renderdoc as rd


def action_name(action, structured_file):
    custom_name = str(getattr(action, "customName", "") or "")
    if custom_name:
        return custom_name
    try:
        return str(action.GetName(structured_file))
    except Exception:
        return ""


def flatten_actions(actions, structured_file, output, depth=0):
    for action in actions:
        output.append(
            {
                "eventId": int(action.eventId),
                "name": action_name(action, structured_file),
                "depth": depth,
                "childCount": len(action.children),
                "flags": str(action.flags),
            }
        )
        flatten_actions(
            action.children,
            structured_file,
            output,
            depth + 1,
        )


def main():
    capture_path = os.environ.get("BTBH_RENDERDOC_CAPTURE")
    output_path = os.environ.get("BTBH_RENDERDOC_TIMINGS")
    if not capture_path or not output_path:
        raise RuntimeError(
            "BTBH_RENDERDOC_CAPTURE and BTBH_RENDERDOC_TIMINGS are required"
        )

    capture = rd.OpenCaptureFile()
    status = capture.OpenFile(capture_path, "", None)
    if status != rd.ReplayStatus.Succeeded:
        raise RuntimeError("Could not open capture: {0}".format(status))
    if not capture.LocalReplaySupport():
        raise RuntimeError("Capture does not support local replay")

    status, controller = capture.OpenCapture(rd.ReplayOptions(), None)
    if status != rd.ReplayStatus.Succeeded:
        raise RuntimeError("Could not initialise replay: {0}".format(status))

    try:
        available = list(controller.EnumerateCounters())
        duration_counter = rd.GPUCounter.EventGPUDuration
        if duration_counter not in available:
            raise RuntimeError("EventGPUDuration is not available")

        structured_file = controller.GetStructuredFile()
        actions = []
        flatten_actions(
            controller.GetRootActions(),
            structured_file,
            actions,
        )
        actions.sort(key=lambda action: action["eventId"])

        result_by_event = {}
        for result in controller.FetchCounters([duration_counter]):
            result_by_event[int(result.eventId)] = (
                float(result.value.d) * 1000.0
            )

        current_marker = "Unmarked"
        marker_totals = {}
        timed_actions = []
        for action in actions:
            if action["name"].startswith("BTBH "):
                current_marker = action["name"]
                marker_totals.setdefault(current_marker, 0.0)
            duration = result_by_event.get(action["eventId"])
            if duration is None:
                continue
            marker_totals[current_marker] = (
                marker_totals.get(current_marker, 0.0) + duration
            )
            timed_action = dict(action)
            timed_action["durationMilliseconds"] = duration
            timed_action["marker"] = current_marker
            timed_actions.append(timed_action)

        payload = {
            "capture": capture_path,
            "counter": "EventGPUDuration",
            "timedEventCount": len(timed_actions),
            "totalEventMilliseconds": sum(result_by_event.values()),
            "markerMilliseconds": dict(
                sorted(
                    marker_totals.items(),
                    key=lambda item: item[1],
                    reverse=True,
                )
            ),
            "actions": actions,
            "timedActions": timed_actions,
        }
        with open(output_path, "w") as output_file:
            json.dump(payload, output_file, indent=2, sort_keys=True)
            output_file.write("\n")
    finally:
        controller.Shutdown()
        capture.Shutdown()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print("BTBH_RENDERDOC_TIMINGS_FAILED: {0}".format(error))
        sys.exit(2)
    print("BTBH_RENDERDOC_TIMINGS_OK")
    sys.exit(0)
