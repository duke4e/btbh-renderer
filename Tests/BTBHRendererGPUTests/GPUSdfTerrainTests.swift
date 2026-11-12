import BTBHRenderer
import BTBHRendererGPUCore
import Foundation
import XCTest
@testable import BTBHRendererGPU

/// The Swift layer over RFC 0007's destructible terrain: the handle and its queries (no window), its chunks drawn
/// through `GPUPlatform.setSdfTerrain`, `dugByTerrain` floors over a crater, and FSR 3's reactive mark on fresh
/// chunks. The scenes follow `btbh_gpu_sdf_terrain_smoke`: a crater (a scorched bowl and a soil rim) in flat
/// ground at y = 0, seen from above and to the side.
@preconcurrency @MainActor
final class GPUSdfTerrainTests: XCTestCase {
    private static let floorColor = RenderColor(r: 190, g: 190, b: 184)
    /// Asphalt (the floor's own colour, so only the crater's colours show), soil and scorch.
    private static let materialColors: [RenderColor] = [
        floorColor,
        RenderColor(r: 150, g: 104, b: 60),
        RenderColor(r: 52, g: 44, b: 40),
    ]
    private static let bowl = GPUSdfEdit(
        shape: .ellipsoid,
        operation: .subtract,
        position: .init(x: 0, y: 0.6, z: 0),
        size: .init(x: 2.6, y: 1.5, z: 2.6),
        smoothing: 0.3,
        material: 2,
        materialBand: 0.2
    )
    private static let rim = GPUSdfEdit(
        shape: .torus,
        operation: .add,
        position: .init(x: 0, y: -0.25, z: 0),
        size: .init(x: 2.7, y: 0.5, z: 0),
        smoothing: 0.6,
        material: 1
    )
    private static let mound = GPUSdfEdit(
        shape: .sphere,
        operation: .add,
        position: .zero,
        size: .init(x: 1.4, y: 0, z: 0),
        smoothing: 0.3,
        material: 1
    )
    private static let drawableSize = PixelSize2D(width: 256, height: 256)

    private static func descriptor(
        concurrentJobs: Bool = false,
        maxEdits: Int = 4096,
        editCap: GPUSdfEditCap = .freeze
    ) -> GPUSdfTerrainDescriptor {
        .init(
            bounds: .init(minimum: .init(x: -8, y: -4, z: -8), maximum: .init(x: 8, y: 4, z: 8)),
            cellSize: 0.25,
            base: .flat(height: 0),
            chunkCells: 16,
            maxEdits: maxEdits,
            editCap: editCap,
            normals: .faceted,
            surfaceMaterial: 0,
            subsurfaceMaterial: 1,
            subsurfaceDepth: 0.3,
            materialColors: materialColors,
            concurrentJobs: concurrentJobs
        )
    }

    @discardableResult
    private static func settle(_ terrain: GPUSdfTerrain, budget: UInt32 = 0) throws -> GPUSdfTerrainReport {
        var report = try terrain.update(budgetMicroseconds: budget)
        var calls = 1
        while !report.isSettled, calls < 10_000 {
            report = try terrain.update(budgetMicroseconds: budget)
            calls += 1
        }
        XCTAssertTrue(report.isSettled)
        return report
    }

    // MARK: The handle, without a window

    func testEditsUpdatesAndQueriesWithoutAWindow() throws {
        let terrain = try GPUSdfTerrain(Self.descriptor())
        XCTAssertFalse(terrain.isAttached)
        XCTAssertEqual(terrain.chunkCount, 5 * 3 * 5)
        // The flat base: its height, its distance and a ray onto it.
        XCTAssertEqual(try XCTUnwrap(terrain.height(x: 1, z: -2, fromY: 3)), 0, accuracy: 1e-4)
        XCTAssertNil(terrain.height(x: 1, z: -2, fromY: -1), "under the ground there is no surface going down")
        XCTAssertEqual(terrain.distance(at: .init(x: 5, y: 2, z: 5)), 2, accuracy: 1e-3)
        let floorHit = try XCTUnwrap(terrain.raycast(
            origin: .init(x: 1, y: 5, z: 1), direction: .down, maxDistance: 20
        ))
        XCTAssertEqual(floorHit.y, 0, accuracy: 0.01)
        XCTAssertNil(terrain.raycast(origin: .init(x: 1, y: 5, z: 1), direction: .up, maxDistance: 20))

        let bowlID = try XCTUnwrap(terrain.addEdit(Self.bowl))
        let rimID = try XCTUnwrap(terrain.addEdit(Self.rim))
        XCTAssertNotEqual(bowlID, rimID)
        XCTAssertNil(terrain.addEdit(.init(
            shape: .sphere, operation: .add, position: .init(x: .nan, y: 0, z: 0), size: .one
        )), "an invalid edit is refused")
        // Queries read the edit list: the crater's floor is there before any update.
        let floor = try XCTUnwrap(terrain.height(x: 0, z: 0, fromY: 3))
        XCTAssertEqual(floor, -0.9, accuracy: 0.1)
        let craterHit = try XCTUnwrap(terrain.raycast(
            origin: .init(x: 0, y: 5, z: 0), direction: .down, maxDistance: 20
        ))
        XCTAssertEqual(craterHit.y, floor, accuracy: 0.02)
        XCTAssertGreaterThan(terrain.distance(at: .init(x: 0, y: -0.5, z: 0)), 0, "the bowl is open here")
        XCTAssertLessThan(terrain.distance(at: .init(x: 5, y: -0.5, z: 5)), 0, "the ground beside it is not")
        XCTAssertEqual(terrain.takeChanges(), [], "changes show when their mesh is ready")

        let report = try Self.settle(terrain)
        XCTAssertEqual(report.editCount, 2)
        XCTAssertGreaterThan(report.brickCount, 0)
        XCTAssertGreaterThan(report.meshBytes, 0)
        XCTAssertGreaterThan(report.maskBytes, 0)
        // Rectangles inside the bounds, at most a chunk column (4 m) each; the crater's middle among them.
        let changes = terrain.takeChanges()
        XCTAssertFalse(changes.isEmpty)
        for change in changes {
            XCTAssertTrue(change.width > 0 && change.width <= 4.001, "\(change)")
            XCTAssertTrue(change.height > 0 && change.height <= 4.001, "\(change)")
            XCTAssertTrue(change.x >= -8.001 && change.x + change.width <= 8.001, "\(change)")
            XCTAssertTrue(change.y >= -8.001 && change.y + change.height <= 8.001, "\(change)")
        }
        XCTAssertTrue(changes.contains { $0.contains(.init(x: 0.5, y: 0.5)) })
        XCTAssertTrue(changes.contains { $0.contains(.init(x: -0.5, y: -0.5)) })
        XCTAssertEqual(terrain.takeChanges(), [], "taken once")

        // Deepening the bowl keeps its id and moves the floor down.
        var deeper = Self.bowl
        deeper.position.y = 0
        XCTAssertTrue(terrain.setEdit(bowlID, to: deeper))
        XCTAssertEqual(try XCTUnwrap(terrain.height(x: 0, z: 0, fromY: 3)), -1.5, accuracy: 0.1)
        try Self.settle(terrain)
        XCTAssertFalse(terrain.takeChanges().isEmpty)

        // Removing the edits gives the flat ground back, and frees the meshes and the mask.
        XCTAssertTrue(terrain.removeEdit(rimID))
        XCTAssertFalse(terrain.removeEdit(rimID), "an id goes once")
        XCTAssertFalse(terrain.setEdit(rimID, to: Self.rim), "a removed id can't be set: add a new edit")
        XCTAssertTrue(terrain.removeEdit(bowlID))
        let restored = try Self.settle(terrain)
        XCTAssertEqual(restored.editCount, 0)
        XCTAssertEqual(restored.meshBytes, 0)
        XCTAssertEqual(restored.maskBytes, 0)
        XCTAssertEqual(try XCTUnwrap(terrain.height(x: 0, z: 0, fromY: 3)), 0, accuracy: 1e-4)
    }

    func testDescriptorsBasesCapsAndJobs() throws {
        var tooFine = Self.descriptor()
        tooFine.cellSize = 0.05
        XCTAssertThrowsError(try GPUSdfTerrain(tooFine)) { error in
            guard case GPUSdfTerrainError.invalidDescriptor(let message) = error else {
                return XCTFail("\(error)")
            }
            XCTAssertTrue(message.contains("cell_size"), message)
        }
        var shortField = Self.descriptor()
        shortField.base = .heightfield(heights: [0, 0, 0], width: 1, depth: 1, cellSize: 16)
        XCTAssertThrowsError(try GPUSdfTerrain(shortField))
        var nineColours = Self.descriptor()
        nineColours.materialColors = Array(repeating: .white, count: 9)
        XCTAssertThrowsError(try GPUSdfTerrain(nineColours))

        // A heightfield base: 2 x 2 cells of 8 over the bounds' x and z, a slope up along x.
        var sloped = Self.descriptor()
        sloped.base = .heightfield(heights: [0, 1, 2, 0, 1, 2, 0, 1, 2], width: 2, depth: 2, cellSize: 8)
        let field = try GPUSdfTerrain(sloped)
        XCTAssertEqual(try XCTUnwrap(field.height(x: -8, z: 3, fromY: 4)), 0, accuracy: 1e-3)
        XCTAssertEqual(try XCTUnwrap(field.height(x: 4, z: -3, fromY: 4)), 1.5, accuracy: 1e-3)

        // The cap: reject refuses the edit past it; freeze folds the oldest into the base.
        let rejecting = try GPUSdfTerrain(Self.descriptor(maxEdits: 1, editCap: .reject))
        XCTAssertNotNil(rejecting.addEdit(Self.bowl))
        XCTAssertNil(rejecting.addEdit(Self.rim))
        let freezing = try GPUSdfTerrain(Self.descriptor(maxEdits: 1, editCap: .freeze))
        let first = try XCTUnwrap(freezing.addEdit(Self.bowl))
        XCTAssertNotNil(freezing.addEdit(Self.rim))
        XCTAssertFalse(freezing.setEdit(first, to: Self.bowl), "a frozen id stops being valid")
        let frozen = try Self.settle(freezing)
        XCTAssertEqual(frozen.editCount, 1)
        XCTAssertGreaterThanOrEqual(frozen.frozenEdits, 1)
        XCTAssertEqual(try XCTUnwrap(freezing.height(x: 0, z: 0, fromY: 3)), -0.9, accuracy: 0.15)

        // Jobs on DispatchQueue.concurrentPerform give what one thread gives, budget or not.
        let serial = try GPUSdfTerrain(Self.descriptor())
        let jobs = try GPUSdfTerrain(Self.descriptor(concurrentJobs: true))
        for terrain in [serial, jobs] {
            XCTAssertNotNil(terrain.addEdit(Self.bowl))
            XCTAssertNotNil(terrain.addEdit(Self.rim))
        }
        let serialReport = try Self.settle(serial)
        let jobsReport = try Self.settle(jobs, budget: 200)
        XCTAssertEqual(jobsReport.brickCount, serialReport.brickCount)
        XCTAssertEqual(jobsReport.meshBytes, serialReport.meshBytes)
        XCTAssertEqual(jobsReport.maskBytes, serialReport.maskBytes)
        XCTAssertEqual(jobs.takeChanges().count, serial.takeChanges().count)
    }

    // MARK: Drawn through the platform

    @MainActor
    private struct Scene {
        let bundle: GPURenderServicesBundle
        let model: ModelHandle
        let plainFloor: [MaterialOverride]
        let dugFloor: [MaterialOverride]
        let instanceBuffer: InstanceBufferHandle
        let light: LightHandle

        var platform: GPUPlatform { bundle.gpuPlatform }
        var backend: GPUSceneBackend { bundle.scene as! GPUSceneBackend }
    }

    private enum FloorDraw: CaseIterable {
        case instances
        case persistentRangeList
        case transformStreams
    }

    /// BTBH's floor in miniature: an imported slab (24 x 24 m at y = 0) whose material is patched not to cast
    /// shadows, and in the dug copy also to be dug by the terrain; a sun that casts shadows.
    private func openScene(_ title: String) throws -> Scene {
        RenderServices.uninstall()
        let bundle = GPURenderServices.make(request: .platformDefault, debugMode: true)
        try RenderServices.install(bundle)
        try RenderServices.platform.openWindow(.init(
            title: title,
            width: Self.drawableSize.width,
            height: Self.drawableSize.height,
            mode: .windowed,
            frameRatePolicy: .uncapped,
            drawableScalePolicy: .oneToOne
        ))
        try RenderServices.scene.initialize(drawableSize: Self.drawableSize)
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let model = try RenderServices.resources.loadModel(
            path: packageRoot.appendingPathComponent("Tests/Fixtures/upward-lit-slab.obj").path,
            flags: [.meshData, .highQuality]
        )
        let slots = try RenderServices.resources.modelMetadata(model).materialCount
        func overrides(dug: Bool?) throws -> [MaterialOverride] {
            try (0..<slots).map { slot in
                MaterialOverride(slot: slot, material: try RenderServices.resources.createMaterialOverride(
                    from: model,
                    materialIndex: slot,
                    patch: .init(albedoColor: Self.floorColor, castsShadow: false, dugByTerrain: dug)
                ))
            }
        }
        let instanceBuffer = try RenderServices.resources.createInstanceBuffer(.init(
            capacity: 1, stride: 0, includesColor: true, customFloatCount: 4
        ))
        let light = try RenderServices.resources.createLight(.init(
            kind: .directional,
            active: true,
            position: .zero,
            direction: RenderVec3(x: -3, y: -10, z: -2).normalized(),
            color: .white,
            energy: 2,
            range: 0,
            castsShadow: true,
            shadowSoftness: 1
        ))
        return Scene(
            bundle: bundle,
            model: model,
            plainFloor: try overrides(dug: nil),
            dugFloor: try overrides(dug: true),
            instanceBuffer: instanceBuffer,
            light: light
        )
    }

    private func close(_ scene: Scene) {
        try? scene.platform.setSdfTerrain(nil)
        RenderServices.resources.destroyLight(scene.light)
        RenderServices.resources.destroyInstanceBuffer(scene.instanceBuffer)
        for entry in scene.plainFloor + scene.dugFloor {
            RenderServices.resources.destroyMaterial(entry.material)
        }
        RenderServices.resources.destroyModel(scene.model)
        RenderServices.uninstall()
    }

    private func present(
        _ scene: Scene,
        floor: [MaterialOverride],
        draw: FloorDraw = .instances,
        upscaleMode: UpscaleMode = .linear
    ) throws -> FrameReadback {
        let scale = RenderVec3(x: 12, y: 1, z: 12)
        let instance = RenderInstance(
            transform: .init(matrix: .scale(scale)),
            color: .white,
            custom: .init(x: 0, y: 0, z: 0, w: 0)
        )
        RenderServices.canvas.requestPresentedFrameReadback()
        RenderServices.canvas.beginFrame()
        RenderServices.canvas.submit(.clear(.black))
        RenderServices.scene.beginFrame(.init(
            camera: .init(
                position: .init(x: 0, y: 9, z: 5.5),
                target: .init(x: 0, y: -0.4, z: 0),
                up: .up,
                verticalFOVDegrees: 43,
                projection: .perspective
            ),
            environment: .init(
                backgroundColor: .init(r: 255, g: 0, b: 255),
                backgroundEnergy: 1,
                ambientColor: .white,
                ambientEnergy: 0.3,
                ambientMap: .invalid,
                skyCubemap: .invalid,
                ssaoEnabled: false,
                tonemap: .reinhard,
                tonemapWhite: 1
            ),
            renderScale: upscaleMode == .fsr3 ? 0.5 : 1,
            antiAliasing: .off,
            upscaleMode: upscaleMode
        ))
        switch draw {
        case .instances:
            RenderServices.scene.submit(.modelInstances(.init(
                model: scene.model, instances: [instance], overrides: floor, persistentBuffer: nil
            )))
        case .persistentRangeList:
            RenderServices.scene.submitPersistentModelRanges(.init(
                model: scene.model,
                instances: [instance],
                overrides: floor,
                persistentBuffer: scene.instanceBuffer,
                persistentRevision: 1,
                ranges: [.init(
                    sourceStart: 0,
                    count: 1,
                    bounds: .init(minimum: .init(x: -12, y: 0, z: -12), maximum: .init(x: 12, y: 0, z: 12))
                )]
            ))
        case .transformStreams:
            RenderServices.scene.submit(.modelTransformStreams(.init(
                model: scene.model,
                positions: [.zero],
                rotations: [.identity],
                scales: [scale],
                overrides: floor,
                count: 1
            )))
        }
        try RenderServices.scene.endFrame()
        RenderServices.canvas.endFrame()
        let frame = try RenderServices.canvas.readbackPresentedFrame()
        XCTAssertEqual(frame.size, Self.drawableSize)
        return frame
    }

    /// With BTBH_SDF_TERRAIN_PNG_DIR set, writes the frame there as sdf_terrain_swift_<name>.png.
    private func save(_ frame: FrameReadback, _ name: String) {
        guard let directory = ProcessInfo.processInfo.environment["BTBH_SDF_TERRAIN_PNG_DIR"],
              !directory.isEmpty else { return }
        let path = URL(fileURLWithPath: directory).appendingPathComponent("sdf_terrain_swift_\(name).png").path
        let opaque = GPUPresentedFramePNG.opaqueRGBA8(frame.rgba8)
        _ = path.withCString { pathPointer in
            opaque.withUnsafeBufferPointer {
                btbh_gpu_write_png_rgba8(
                    pathPointer, UInt32(frame.size.width), UInt32(frame.size.height), $0.baseAddress
                )
            }
        }
    }

    private func pixel(_ frame: FrameReadback, _ x: Int, _ y: Int) -> (r: Int, g: Int, b: Int) {
        let offset = (y * frame.size.width + x) * 4
        return (Int(frame.rgba8[offset]), Int(frame.rgba8[offset + 1]), Int(frame.rgba8[offset + 2]))
    }

    private func luminance(_ frame: FrameReadback, _ x: Int, _ y: Int) -> Float {
        let p = pixel(frame, x, y)
        return 0.2126 * Float(p.r) + 0.7152 * Float(p.g) + 0.0722 * Float(p.b)
    }

    /// Pixels where the background (magenta) shows: a hole with no patch under it.
    private func backgroundPixels(_ frame: FrameReadback) -> Int {
        let bytes = frame.rgba8
        var count = 0
        for offset in stride(from: 0, to: bytes.count, by: 4) {
            let red: UInt8 = bytes[offset]
            let green: UInt8 = bytes[offset + 1]
            let blue: UInt8 = bytes[offset + 2]
            if red > 90, blue > 90, green < 40 {
                count += 1
            }
        }
        return count
    }

    private func differingPixels(_ a: FrameReadback, _ b: FrameReadback) -> Int {
        var count = 0
        for offset in stride(from: 0, to: a.rgba8.count, by: 4) {
            let same: Bool = a.rgba8[offset] == b.rgba8[offset] &&
                a.rgba8[offset + 1] == b.rgba8[offset + 1] &&
                a.rgba8[offset + 2] == b.rgba8[offset + 2]
            if !same {
                count += 1
            }
        }
        return count
    }

    func testChunksDrawThroughThePlatformAndNothingAttachedDrawsAsBefore() throws {
        let scene = try openScene("BTBH SDF terrain through the platform")
        defer { close(scene) }
        let plain = try present(scene, floor: scene.plainFloor)
        XCTAssertEqual(try present(scene, floor: scene.plainFloor).rgba8, plain.rgba8, "the frame is stable")
        XCTAssertEqual(backgroundPixels(plain), 0, "the floor fills the view")
        XCTAssertEqual(
            try present(scene, floor: scene.dugFloor).rgba8, plain.rgba8,
            "a dug floor with no terrain draws as before"
        )

        let terrain = try GPUSdfTerrain(Self.descriptor())
        XCTAssertEqual(try present(scene, floor: scene.dugFloor).rgba8, plain.rgba8, "created, not attached")
        try scene.platform.setSdfTerrain(terrain)
        XCTAssertTrue(terrain.isAttached)
        XCTAssertTrue(scene.platform.sdfTerrain === terrain)
        XCTAssertEqual(
            try present(scene, floor: scene.dugFloor).rgba8, plain.rgba8,
            "attached, no edit: no chunk mesh and no dig mask"
        )

        // An edit changes the picture: a soil mound on the floor.
        let moundID = try XCTUnwrap(terrain.addEdit(Self.mound))
        try Self.settle(terrain)
        let mound = try present(scene, floor: scene.plainFloor)
        save(plain, "plain")
        save(mound, "mound")
        XCTAssertGreaterThan(differingPixels(plain, mound), 1_000)
        XCTAssertEqual(backgroundPixels(mound), 0)
        let centre = Self.drawableSize.width / 2
        let plainCentre = pixel(plain, centre, centre)
        let moundCentre = pixel(mound, centre, centre)
        XCTAssertLessThan(abs(plainCentre.r - plainCentre.b), 16, "grey floor: \(plainCentre)")
        XCTAssertGreaterThan(moundCentre.r - moundCentre.b, 24, "soil: \(moundCentre)")
        let report = try XCTUnwrap(scene.backend.lastPresentationReport)
        XCTAssertGreaterThan(report.batchCount, 1, "the floor and the mound's chunks")

        // Detached, the picture is the plain one again, byte for byte; attached again, the mound is back.
        try scene.platform.setSdfTerrain(nil)
        XCTAssertFalse(terrain.isAttached)
        XCTAssertNil(scene.platform.sdfTerrain)
        XCTAssertEqual(try present(scene, floor: scene.plainFloor).rgba8, plain.rgba8)
        XCTAssertEqual(try present(scene, floor: scene.dugFloor).rgba8, plain.rgba8)
        try scene.platform.setSdfTerrain(terrain)
        XCTAssertEqual(try present(scene, floor: scene.plainFloor).rgba8, mound.rgba8)

        // Removing the edit gives the plain picture back while attached, with the dug floor too.
        XCTAssertTrue(terrain.removeEdit(moundID))
        try Self.settle(terrain)
        XCTAssertEqual(try present(scene, floor: scene.dugFloor).rgba8, plain.rgba8)

        // Closing the window detaches the terrain; attaching needs a window.
        scene.platform.closeWindow()
        XCTAssertFalse(terrain.isAttached)
        XCTAssertNil(scene.platform.sdfTerrain)
        XCTAssertThrowsError(try scene.platform.setSdfTerrain(terrain))
    }

    func testDugByTerrainFloorShowsTheCraterAndWithoutTheFlagCoversIt() throws {
        let scene = try openScene("BTBH SDF terrain dug floor")
        defer { close(scene) }
        let plain = try present(scene, floor: scene.plainFloor)
        // The crater is built before the terrain is attached; attaching uploads it.
        let terrain = try GPUSdfTerrain(Self.descriptor())
        XCTAssertNotNil(terrain.addEdit(Self.bowl))
        XCTAssertNotNil(terrain.addEdit(Self.rim))
        try Self.settle(terrain)
        try scene.platform.setSdfTerrain(terrain)

        let centre = Self.drawableSize.width / 2
        let last = Self.drawableSize.width - 5
        let corners = [(4, 4), (last, 4), (4, last), (last, last)]
        let plainCentre = luminance(plain, centre, centre)
        for draw in FloorDraw.allCases {
            let dug = try present(scene, floor: scene.dugFloor, draw: draw)
            let covered = try present(scene, floor: scene.plainFloor, draw: draw)
            save(dug, "dug_\(draw)")
            save(covered, "covered_\(draw)")
            // With the flag the crater's scorched floor shows where the floor was, and nowhere a hole.
            XCTAssertLessThan(luminance(dug, centre, centre), 0.5 * plainCentre, "\(draw)")
            XCTAssertEqual(backgroundPixels(dug), 0, "\(draw)")
            // Without it the floor covers the crater (only the rim stands above it).
            XCTAssertEqual(luminance(covered, centre, centre), plainCentre, accuracy: 3, "\(draw)")
            XCTAssertGreaterThan(differingPixels(dug, covered), 1_000, "\(draw)")
            // Far from the crater the floor is the same either way.
            for (x, y) in corners {
                XCTAssertTrue(pixel(dug, x, y) == pixel(covered, x, y), "\(draw) at \(x), \(y)")
            }
        }
    }

    func testFreshChunksAreMarkedForFSR3sReactiveMask() throws {
        let scene = try openScene("BTBH SDF terrain FSR 3 reactive")
        defer { close(scene) }
        let terrain = try GPUSdfTerrain(Self.descriptor())
        try scene.platform.setSdfTerrain(terrain)
        func batches(_ upscaleMode: UpscaleMode) throws -> Int {
            _ = try present(scene, floor: scene.plainFloor, upscaleMode: upscaleMode)
            return try XCTUnwrap(scene.backend.lastPresentationReport).batchCount
        }
        let floorOnly = try batches(.fsr3)
        let moundID = try XCTUnwrap(terrain.addEdit(Self.mound))
        try Self.settle(terrain)
        // For reactiveFrameCount frames each fresh chunk is drawn once more (in the transparent pass), then once.
        let fresh = try batches(.fsr3)
        for _ in 1..<terrain.reactiveFrameCount {
            XCTAssertEqual(try batches(.fsr3), fresh)
        }
        let settled = try batches(.fsr3)
        let chunkBatches = settled - floorOnly
        XCTAssertGreaterThan(chunkBatches, 0)
        XCTAssertEqual(fresh - settled, chunkBatches)
        XCTAssertEqual(try batches(.linear), settled)

        // Without FSR 3 there is no second draw; FSR 3 frames inside the window still get it.
        var wider = Self.mound
        wider.size.x = 2
        XCTAssertTrue(terrain.setEdit(moundID, to: wider))
        try Self.settle(terrain)
        let linear = try batches(.linear)
        XCTAssertGreaterThan(try batches(.fsr3), linear)

        // The second draw adds nothing to the colour: forced into a frame without FSR 3 (whose pixels compare),
        // the picture is the one without it, byte for byte.
        wider.size.x = 1.7
        XCTAssertTrue(terrain.setEdit(moundID, to: wider))
        try Self.settle(terrain)
        let without = try present(scene, floor: scene.plainFloor)
        let withoutCount = try XCTUnwrap(scene.backend.lastPresentationReport).batchCount
        scene.backend.drawsSdfTerrainReactivePassWithoutFSR3ForTesting = true
        let with = try present(scene, floor: scene.plainFloor)
        scene.backend.drawsSdfTerrainReactivePassWithoutFSR3ForTesting = false
        XCTAssertGreaterThan(try XCTUnwrap(scene.backend.lastPresentationReport).batchCount, withoutCount)
        XCTAssertEqual(with.rgba8, without.rgba8)

        // reactiveFrameCount 0 turns it off.
        terrain.reactiveFrameCount = 0
        wider.size.x = 1
        XCTAssertTrue(terrain.setEdit(moundID, to: wider))
        try Self.settle(terrain)
        XCTAssertEqual(try batches(.fsr3), try batches(.linear))
    }

    /// The backend walks every chunk once a scene frame (`chunksForFrame`): its cost at Be The Bullet Hell's scale
    /// (a 1440 m map, 0.5 m cells, 64-cell chunks: 2116 chunks), printed. Without a window no chunk has a GPU mesh.
    func testChunkWalkAtBTBHScale() throws {
        let terrain = try GPUSdfTerrain(.init(
            bounds: .init(minimum: .init(x: -720, y: -8, z: -720), maximum: .init(x: 720, y: 8, z: 720)),
            cellSize: 0.5,
            base: .flat(height: 0)
        ))
        XCTAssertEqual(terrain.chunkCount, 46 * 1 * 46)
        XCTAssertNotNil(terrain.addEdit(Self.bowl))
        XCTAssertNotNil(terrain.addEdit(Self.rim))
        XCTAssertGreaterThan(try Self.settle(terrain).meshBytes, 0)
        let started = Date()
        let frames = 200
        for _ in 0..<frames {
            XCTAssertTrue(terrain.chunksForFrame().isEmpty)
        }
        let microseconds = Date().timeIntervalSince(started) / Double(frames) * 1_000_000
        print("BTBH_SDF_TERRAIN_CHUNK_WALK chunks=\(terrain.chunkCount) microseconds_per_frame=\(microseconds)")
    }
}
