import Foundation
import XCTest
@testable import BTBHRendererGPU

/// `gpuShaderFileStem` names a shader for the surface and screen classifiers without touching the file system.
final class GPUShaderFileStemTests: XCTestCase {
    func testTheStemIsTheLowerCasedFileNameWithoutItsLastExtension() {
        XCTAssertEqual(gpuShaderFileStem("Shaders/glitter_static.glsl"), "glitter_static")
        XCTAssertEqual(gpuShaderFileStem("C:\\Game\\GPUShaders\\Soft_Particle.glsl"), "soft_particle")
        XCTAssertEqual(gpuShaderFileStem("post/lut.frag.hlsl"), "lut.frag")
        XCTAssertEqual(gpuShaderFileStem("TiltShift"), "tiltshift")
        XCTAssertEqual(gpuShaderFileStem("shaders/.hidden"), ".hidden")
        XCTAssertEqual(gpuShaderFileStem(""), "")
    }

    func testItNamesPlainPathsAsURLDid() {
        for path in ["shaders/water_surface.glsl", "a/b/c.d.e", "dissolve.glsl", "dir/lut", "x/y/cas.frag"] {
            let expected = URL(fileURLWithPath: path, isDirectory: false)
                .deletingPathExtension().lastPathComponent.lowercased()
            XCTAssertEqual(gpuShaderFileStem(path), expected, path)
        }
    }
}
