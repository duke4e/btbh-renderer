# Production GPU File Dialog Design

Status: auto-approved for implementation on `codex/renderer-phase-0-1`.

## Goal

Replace the GPU platform's placeholder file-dialog failure with a production
SDL 3 path that supports open and save dialogs without exposing SDL types in
the renderer's public Swift API.

## Contract

- `FileDialogRequest` remains the only project-facing request type.
- Open dialogs may select one or many files.
- Save dialogs reject multiple selection before entering native code with
  `invalidRequest("Save dialogs cannot select multiple files")`.
- An empty extension list produces one wildcard filter named and patterned
  `"*"`. Otherwise each extension is passed as its exact pattern and its
  uppercased display name.
- The initial location is:
  - `initialDirectory/suggestedFileName` when both exist;
  - `initialDirectory.path` when only the directory exists;
  - `suggestedFileName` when only the name exists;
  - absent otherwise.
- User cancellation completes with `.success([])`.
- A native callback with no file list completes with
  `.failure(.backendFailure(message))`.
- A request without an open GPU window fails synchronously and deterministically.

## Native Boundary and Ownership

Add a neutral `btbh_gpu_window_show_file_dialog` C ABI. Its descriptor carries
only the dialog kind, title, filter name/pattern arrays, selection mode, and
optional location. It does not expose SDL types.

The native function validates its descriptor and copies every caller-owned
string before returning. A heap-owned callback context retains:

- the copied filter names and patterns;
- the SDL filter array;
- the SDL properties identifier;
- the caller callback and userdata.

The SDL callback invokes the caller callback exactly once and then destroys
the properties and all copied allocations. The bridge does not retain or
dereference the GPU window after launch, so an outstanding dialog does not
couple callback lifetime to renderer shutdown.

Swift retains a one-shot callback box with `Unmanaged.passRetained`. The C
callback consumes it with `takeRetainedValue`; an immediate launch failure
releases it without waiting for a callback. Paths and error text are copied
inside the callback before native storage is released.

## Testing

1. Swift unit tests prove request validation, wildcard and extension mapping,
   location mapping, cancellation, selected-path, error, and immediate launch
   failure behavior.
2. Native tests prove invalid descriptors fail before callback ownership is
   transferred.
3. Existing GPU and non-GPU suites remain non-interactive.
4. An environment-gated Windows Vulkan test opens the production dialog and
   verifies real cancellation through the OS UI.
5. Full renderer and game build/test/audit gates run before integration.

No golden images, visual thresholds, shader behavior, or backend authority
rules change in this slice.
