import BTBHRenderer
import BTBHRendererGPUCore
import Foundation

struct GPUFileDialogConfiguration: Equatable {
    enum Kind: Equatable {
        case open
        case save
    }

    let kind: Kind
    let title: String
    let filterNames: [String]
    let filterPatterns: [String]
    let allowsMultipleSelection: Bool
    let location: String?

    init(request: FileDialogRequest) throws {
        guard request.kind != .save ||
                !request.allowsMultipleSelection else {
            throw FileDialogError.invalidRequest(
                "Save dialogs cannot select multiple files"
            )
        }
        kind = request.kind == .save ? .save : .open
        title = request.title
        let extensions = request.allowedExtensions.isEmpty
            ? ["*"]
            : request.allowedExtensions
        filterNames = extensions.map { $0.uppercased() }
        filterPatterns = extensions
        allowsMultipleSelection = request.allowsMultipleSelection
        if let directory = request.initialDirectory,
           let fileName = request.suggestedFileName {
            location = directory
                .appendingPathComponent(fileName)
                .path
        } else if let directory = request.initialDirectory {
            location = directory.path
        } else {
            location = request.suggestedFileName
        }
    }
}

final class GPUFileDialogCallbackBox: @unchecked Sendable {
    private let completion: @Sendable (
        Result<[URL], FileDialogError>
    ) -> Void

    init(
        completion: @escaping @Sendable (
            Result<[URL], FileDialogError>
        ) -> Void
    ) {
        self.completion = completion
    }

    func complete(paths: [String]?, errorMessage: String?) {
        guard let paths else {
            let message = errorMessage.flatMap {
                $0.isEmpty ? nil : $0
            } ?? "SDL file-dialog callback failed"
            completion(.failure(.backendFailure(message)))
            return
        }
        completion(.success(
            paths.map { URL(fileURLWithPath: $0) }
        ))
    }

    static let nativeCallback: BTBHGPUFileDialogCallback = {
        userdata,
        paths,
        errorMessage in
        guard let userdata else { return }
        let box = Unmanaged<GPUFileDialogCallbackBox>
            .fromOpaque(userdata)
            .takeRetainedValue()
        let copiedPaths: [String]?
        if let paths {
            var values: [String] = []
            var index = 0
            while let path = paths[index] {
                values.append(String(cString: path))
                index += 1
            }
            copiedPaths = values
        } else {
            copiedPaths = nil
        }
        let copiedError = errorMessage.map { String(cString: $0) }
        box.complete(
            paths: copiedPaths,
            errorMessage: copiedError
        )
    }
}

enum GPUFileDialogNativeLauncher {
    static func launch(
        window: OpaquePointer,
        configuration: GPUFileDialogConfiguration,
        completion: @escaping @Sendable (
            Result<[URL], FileDialogError>
        ) -> Void
    ) {
        let box = GPUFileDialogCallbackBox(completion: completion)
        let userdata = Unmanaged.passRetained(box).toOpaque()
        var report = BTBHGPUFileDialogReport()
        let result = configuration.withNativeDescriptor {
            descriptor in
            var descriptor = descriptor
            return btbh_gpu_window_show_file_dialog(
                window,
                &descriptor,
                GPUFileDialogCallbackBox.nativeCallback,
                userdata,
                &report
            )
        }
        guard result == BTBH_GPU_OK else {
            Unmanaged<GPUFileDialogCallbackBox>
                .fromOpaque(userdata)
                .release()
            let message = reportString(from: &report.message)
            completion(.failure(.backendFailure(
                message.isEmpty
                    ? "GPU file-dialog launch failed"
                    : message
            )))
            return
        }
    }

    private static func reportString<T>(from value: inout T) -> String {
        withUnsafePointer(to: &value) { pointer in
            pointer.withMemoryRebound(
                to: CChar.self,
                capacity: MemoryLayout<T>.size
            ) {
                String(cString: $0)
            }
        }
    }
}

private extension GPUFileDialogConfiguration {
    func withNativeDescriptor<Result>(
        _ body: (BTBHGPUFileDialogDescriptor) -> Result
    ) -> Result {
        title.withCString { titlePointer in
            withCStringArray(filterNames) { namePointers in
                withCStringArray(filterPatterns) {
                    patternPointers in
                    if let location {
                        return location.withCString {
                            locationPointer in
                            body(nativeDescriptor(
                                title: titlePointer,
                                filterNames: namePointers,
                                filterPatterns: patternPointers,
                                location: locationPointer
                            ))
                        }
                    }
                    return body(nativeDescriptor(
                        title: titlePointer,
                        filterNames: namePointers,
                        filterPatterns: patternPointers,
                        location: nil
                    ))
                }
            }
        }
    }

    func nativeDescriptor(
        title: UnsafePointer<CChar>,
        filterNames: UnsafePointer<UnsafePointer<CChar>?>?,
        filterPatterns: UnsafePointer<UnsafePointer<CChar>?>?,
        location: UnsafePointer<CChar>?
    ) -> BTBHGPUFileDialogDescriptor {
        var descriptor = BTBHGPUFileDialogDescriptor()
        descriptor.kind = kind == .save
            ? BTBH_GPU_FILE_DIALOG_SAVE
            : BTBH_GPU_FILE_DIALOG_OPEN
        descriptor.title = title
        descriptor.filter_names = filterNames
        descriptor.filter_patterns = filterPatterns
        descriptor.filter_count = UInt32(self.filterPatterns.count)
        descriptor.allows_multiple_selection =
            allowsMultipleSelection ? 1 : 0
        descriptor.location = location
        return descriptor
    }

    func withCStringArray<Result>(
        _ strings: [String],
        _ body: (
            UnsafePointer<UnsafePointer<CChar>?>?
        ) -> Result
    ) -> Result {
        var pointers: [UnsafePointer<CChar>?] = []
        pointers.reserveCapacity(strings.count)

        func visit(_ index: Int) -> Result {
            guard index < strings.count else {
                return pointers.withUnsafeBufferPointer {
                    body($0.baseAddress)
                }
            }
            return strings[index].withCString { pointer in
                pointers.append(pointer)
                defer { pointers.removeLast() }
                return visit(index + 1)
            }
        }

        return visit(0)
    }
}
