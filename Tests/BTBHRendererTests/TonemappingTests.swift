import Foundation
import XCTest
@testable import BTBHRenderer

final class TonemappingTests: XCTestCase {
    func testToneMapModesKeepLegacyValuesAndExposeACES() {
        XCTAssertEqual(TonemapMode.reinhard.rawValue, 0)
        XCTAssertEqual(TonemapMode.pbrNeutral.rawValue, 1)
        XCTAssertEqual(TonemapMode.aces.rawValue, 2)
    }

    func testPBRNeutralMapsRepresentativeHDRColorsIntoDisplayRange() {
        let colors: [SIMD3<Double>] = [
            .zero,
            .init(repeating: 0.04),
            .init(repeating: 0.76),
            .init(repeating: 1),
            .init(30, 3, 0.5),
            .init(30, 2, 20),
            .init(repeating: 30),
            .init(1_000, 250, 4),
        ]

        for color in colors {
            let mapped = pbrNeutral(color)
            for component in [mapped.x, mapped.y, mapped.z] {
                XCTAssertTrue(component.isFinite, "Non-finite output for \(color)")
                XCTAssertGreaterThanOrEqual(component, 0, "Negative output for \(color)")
                XCTAssertLessThanOrEqual(component, 1, "Over-range output for \(color)")
            }
        }
    }

    func testPBRNeutralIsContinuousAtHighlightCompressionBoundary() {
        let epsilon = 0.000_001
        let below = pbrNeutral(.init(repeating: 0.76 - epsilon))
        let above = pbrNeutral(.init(repeating: 0.76 + epsilon))

        XCTAssertLessThan(abs(above.x - below.x), 0.000_01)
        XCTAssertLessThan(abs(above.y - below.y), 0.000_01)
        XCTAssertLessThan(abs(above.z - below.z), 0.000_01)
    }

    func testPBRNeutralPreservesStrongColorChannelOrdering() {
        let mapped = pbrNeutral(.init(30, 2, 20))

        XCTAssertGreaterThan(mapped.x, mapped.z)
        XCTAssertGreaterThan(mapped.z, mapped.y)
    }

    func testProductionShadersMapPBRNeutralBeforeLUTAndOnlyOnce() throws {
        let root = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        func source(_ path: String) throws -> String {
            try String(
                contentsOf: root.appendingPathComponent(path),
                encoding: .utf8
            )
        }

        let shared = try source("shaders/gpu/production_tonemap.hlsli")
        let lut = try source("shaders/gpu/production_lut.frag.hlsl")
        let post = try source("shaders/gpu/production_post.frag.hlsl")
        let tilt = try source("shaders/gpu/production_tilt_shift.frag.hlsl")
        let nativeHeader = try source("include/btbh_gpu/btbh_gpu.h")
        let nativeScene = try source("src/gpu/btbh_gpu_scene.c")

        XCTAssertTrue(shared.contains("btbh_pbr_neutral_tone_map"))
        XCTAssertTrue(shared.contains("btbh_aces_tone_map"))
        XCTAssertTrue(shared.contains("btbh_apply_color_adjustment"))
        XCTAssertTrue(shared.contains("color = 0.18f * pow(max(color, 0.0f) / 0.18f, contrast);"),
                      "contrast keeps black at black (a linear 0.5 pivot crushed dark scenes)")
        XCTAssertTrue(shared.contains("const float start_compression = 0.8f - 0.04f;"))
        XCTAssertTrue(shared.contains("return lerp(color, new_peak.xxx, desaturation_amount);"))
        XCTAssertTrue(shared.contains("lut_enabled != 0u"))
        XCTAssertTrue(lut.contains("#include \"production_tonemap.hlsli\""))
        XCTAssertTrue(lut.contains("btbh_prepare_lut_input("))
        XCTAssertTrue(post.contains("#include \"production_tonemap.hlsli\""))
        XCTAssertTrue(post.contains("btbh_tone_map_for_output("))
        XCTAssertTrue(tilt.contains("#include \"production_tonemap.hlsli\""))
        XCTAssertTrue(tilt.contains("btbh_tone_map_for_output("))
        XCTAssertTrue(nativeHeader.contains("BTBH_GPU_SCENE_TONEMAP_PBR_NEUTRAL = 1"))
        XCTAssertTrue(nativeHeader.contains("BTBH_GPU_SCENE_TONEMAP_ACES = 2"))
        XCTAssertTrue(nativeHeader.contains("float exposure;"))
        XCTAssertTrue(nativeHeader.contains("float contrast;"))
        XCTAssertTrue(nativeHeader.contains("float saturation;"))
        XCTAssertTrue(nativeHeader.contains("uint8_t tonemap_mode;"))
        XCTAssertTrue(nativeScene.contains("post_uniforms.tonemap_mode = post->tonemap_mode;"))
        XCTAssertTrue(nativeScene.contains("post_uniforms.exposure = post->exposure;"))
        XCTAssertTrue(nativeScene.contains("post_uniforms.contrast = post->contrast;"))
        XCTAssertTrue(nativeScene.contains("post_uniforms.saturation = post->saturation;"))
    }

    private func pbrNeutral(_ input: SIMD3<Double>) -> SIMD3<Double> {
        let startCompression = 0.8 - 0.04
        let desaturation = 0.15
        var color = SIMD3(
            max(input.x, 0),
            max(input.y, 0),
            max(input.z, 0)
        )
        let minimum = min(color.x, min(color.y, color.z))
        let offset = minimum < 0.08
            ? minimum - 6.25 * minimum * minimum
            : 0.04
        color -= .init(repeating: offset)

        let peak = max(color.x, max(color.y, color.z))
        guard peak >= startCompression else { return color }
        let shoulder = 1 - startCompression
        let newPeak = 1 - shoulder * shoulder /
            (peak + shoulder - startCompression)
        color *= newPeak / peak
        let amount = 1 - 1 /
            (desaturation * (peak - newPeak) + 1)
        return color * (1 - amount) + .init(repeating: newPeak * amount)
    }
}
