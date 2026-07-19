import Foundation
import XCTest

final class PublicDependencyBoundaryTests: XCTestCase {
    func testProjectSourcesContainNoVendorDependencies() throws {
        let packageRoot = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .deletingLastPathComponent()
        let sourceRoots = [
            packageRoot.appendingPathComponent("Sources/BTBHRendererCore", isDirectory: true),
            packageRoot.appendingPathComponent("Sources/BTBHRenderer", isDirectory: true),
        ]

        let violations = try sourceRoots.flatMap(scanSourceTree)

        XCTAssertEqual(violations, [], violations.joined(separator: "\n"))
    }

    func testScannerIgnoresCommentsStringsAndPermittedIdentifiers() {
        let source = #"""
        // raylib R3D_draw rlgl SDL OpenGL Metal Vulkan
        /* SDL /* Vulkan */ Metal */
        let explanation = "raylib r3d_draw rlgl sdl opengl metal vulkan"
        let raw = #"SDL Metal"#
        let metalness: Float = 0.5
        let backend = RendererBackend()
        """#

        XCTAssertEqual(forbiddenTokens(in: source), [])
    }

    func testScannerRejectsStandaloneVendorTokensAndR3DAPIPrefix() {
        let source = """
        let a = Raylib
        let b = r3d_DrawModel
        let c = RLGL
        let d = sDl
        let e = OpenGL
        let f = METAL
        let g = vulkan
        """

        XCTAssertEqual(
            forbiddenTokens(in: source),
            ["Raylib", "r3d_DrawModel", "RLGL", "sDl", "OpenGL", "METAL", "vulkan"]
        )
    }

    func testScannerRejectsVendorFamilyIdentifiers() {
        let source = """
        import SDL3
        let window = SDL_CreateWindow()
        import MetalKit
        """

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL3", "SDL_CreateWindow", "MetalKit"])
    }

    func testScannerInspectsCodeInsideSwiftStringInterpolation() {
        let source = #"""
        let message = "created \(SDL_CreateWindow())"
        """#

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    func testScannerInspectsPoundExpressionAtStartOfInterpolation() {
        let source = #"""
        let message = "selector \(#selector(SDL_CreateWindow))"
        """#

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    func testScannerRecursesThroughCommentsAndNestedStringsInsideInterpolation() {
        let source = ##"""
        let message = "outer \((/* SDL3 */ "inner \(SDL_CreateWindow())"))"
        """##

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    func testScannerIgnoresRawLiteralInterpolationSyntaxAndEmbeddedQuotes() {
        let source = ##"""
        let raw = #"literal \(SDL_CreateWindow()) " SDL3 MetalKit Vulkan"#
        """##

        XCTAssertEqual(forbiddenTokens(in: source), [])
    }

    func testScannerInspectsSingleHashRawStringInterpolation() {
        let source = ##"""
        let raw = #"created \#(SDL_CreateWindow())"#
        """##

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    func testScannerUsesMatchingHashCountForRawStringInterpolation() {
        let source = ###"""
        let raw = ##"literal \#(SDL_CreateWindow()), code \##(MetalKit)"##
        """###

        XCTAssertEqual(forbiddenTokens(in: source), ["MetalKit"])
    }

    func testScannerRecursesThroughNestedRawStringInterpolation() {
        let source = ###"""
        let outer = #"outer \#(
            #"inner \#(SDL_CreateWindow())"#
        )"#
        """###

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    func testScannerInspectsMultiHashMultilineRawStringInterpolation() {
        let source = ###"""
        let raw = ##"""
        literal SDL3
        \##(SDL_CreateWindow())
        """##
        """###

        XCTAssertEqual(forbiddenTokens(in: source), ["SDL_CreateWindow"])
    }

    private func scanSourceTree(_ root: URL) throws -> [String] {
        let fileManager = FileManager.default
        guard let enumerator = fileManager.enumerator(
            at: root,
            includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else {
            XCTFail("Unable to enumerate \(root.path)")
            return []
        }

        let sourceExtensions: Set<String> = ["c", "cc", "cpp", "h", "hpp", "m", "mm", "swift"]
        var violations: [String] = []
        for case let fileURL as URL in enumerator {
            let values = try fileURL.resourceValues(forKeys: [.isRegularFileKey])
            guard values.isRegularFile == true, sourceExtensions.contains(fileURL.pathExtension.lowercased()) else {
                continue
            }
            let source = try String(contentsOf: fileURL, encoding: .utf8)
            for token in forbiddenTokens(in: source) {
                violations.append("\(fileURL.path): forbidden token '\(token)'")
            }
        }
        return violations.sorted()
    }

    private func forbiddenTokens(in source: String) -> [String] {
        lexicalIdentifiers(in: source).filter { identifier in
            let normalized = identifier.lowercased()
            return ["raylib", "rlgl", "sdl", "sdl3", "opengl", "metal", "metalkit", "vulkan"].contains(normalized)
                || normalized.hasPrefix("r3d_")
                || normalized.hasPrefix("sdl_")
        }
    }

    private func lexicalIdentifiers(in source: String) -> [String] {
        var scanner = SwiftSourceScanner(source: source)
        return scanner.identifiers()
    }
}

private struct SwiftSourceScanner {
    private struct StringDelimiter {
        let hashCount: Int
        let quoteCount: Int

        var openingLength: Int { hashCount + quoteCount }
    }

    private let scalars: [UnicodeScalar]
    private var index = 0
    private var collectedIdentifiers: [String] = []

    init(source: String) {
        scalars = Array(source.unicodeScalars)
    }

    mutating func identifiers() -> [String] {
        scanCode(untilClosingParenthesis: false)
        return collectedIdentifiers
    }

    private mutating func scanCode(untilClosingParenthesis: Bool) {
        var parenthesisDepth = 1
        while index < scalars.count {
            if matches("//", at: index) {
                skipLineComment()
            } else if matches("/*", at: index) {
                skipBlockComment()
            } else if let delimiter = stringDelimiter(at: index) {
                index += delimiter.openingLength
                scanString(delimitedBy: delimiter)
            } else if scalars[index] == "'" {
                skipCharacterLiteral()
            } else if untilClosingParenthesis, scalars[index] == "(" {
                parenthesisDepth += 1
                index += 1
            } else if untilClosingParenthesis, scalars[index] == ")" {
                parenthesisDepth -= 1
                index += 1
                if parenthesisDepth == 0 { return }
            } else if isIdentifierStart(scalars[index]) {
                scanIdentifier()
            } else {
                index += 1
            }
        }
    }

    private mutating func scanString(delimitedBy delimiter: StringDelimiter) {
        while index < scalars.count {
            if matchesStringClose(delimiter, at: index) {
                index += delimiter.quoteCount + delimiter.hashCount
                return
            }

            if matchesInterpolationOpen(delimiter, at: index) {
                index += 1 + delimiter.hashCount + 1
                scanCode(untilClosingParenthesis: true)
            } else if delimiter.hashCount == 0, scalars[index] == "\\" {
                index = min(index + 2, scalars.count)
            } else {
                index += 1
            }
        }
    }

    private mutating func skipLineComment() {
        index += 2
        while index < scalars.count, scalars[index] != "\n" {
            index += 1
        }
    }

    private mutating func skipBlockComment() {
        var depth = 1
        index += 2
        while index < scalars.count, depth > 0 {
            if matches("/*", at: index) {
                depth += 1
                index += 2
            } else if matches("*/", at: index) {
                depth -= 1
                index += 2
            } else {
                index += 1
            }
        }
    }

    private mutating func skipCharacterLiteral() {
        index += 1
        while index < scalars.count {
            if scalars[index] == "\\" {
                index = min(index + 2, scalars.count)
            } else if scalars[index] == "'" {
                index += 1
                return
            } else {
                index += 1
            }
        }
    }

    private mutating func scanIdentifier() {
        let start = index
        index += 1
        while index < scalars.count, isIdentifierContinuation(scalars[index]) {
            index += 1
        }
        collectedIdentifiers.append(String(String.UnicodeScalarView(scalars[start..<index])))
    }

    private func stringDelimiter(at offset: Int) -> StringDelimiter? {
        var quoteOffset = offset
        while quoteOffset < scalars.count, scalars[quoteOffset] == "#" {
            quoteOffset += 1
        }
        guard quoteOffset < scalars.count, scalars[quoteOffset] == "\"" else {
            return nil
        }

        let quoteCount = matchesRepeated("\"", count: 3, at: quoteOffset) ? 3 : 1
        return StringDelimiter(hashCount: quoteOffset - offset, quoteCount: quoteCount)
    }

    private func matchesStringClose(_ delimiter: StringDelimiter, at offset: Int) -> Bool {
        guard matchesRepeated("\"", count: delimiter.quoteCount, at: offset) else {
            return false
        }
        return matchesRepeated("#", count: delimiter.hashCount, at: offset + delimiter.quoteCount)
    }

    private func matchesInterpolationOpen(_ delimiter: StringDelimiter, at offset: Int) -> Bool {
        guard offset < scalars.count, scalars[offset] == "\\" else { return false }
        let hashesStart = offset + 1
        guard matchesRepeated("#", count: delimiter.hashCount, at: hashesStart) else { return false }
        let parenthesisOffset = hashesStart + delimiter.hashCount
        guard parenthesisOffset < scalars.count, scalars[parenthesisOffset] == "(" else { return false }
        return true
    }

    private func matchesRepeated(_ scalar: UnicodeScalar, count: Int, at offset: Int) -> Bool {
        guard offset + count <= scalars.count else { return false }
        return scalars[offset..<(offset + count)].allSatisfy { $0 == scalar }
    }

    private func matches(_ text: String, at offset: Int) -> Bool {
        let expected = Array(text.unicodeScalars)
        guard offset + expected.count <= scalars.count else { return false }
        return scalars[offset..<(offset + expected.count)].elementsEqual(expected)
    }

    private func isIdentifierStart(_ scalar: UnicodeScalar) -> Bool {
        scalar == "_" || CharacterSet.letters.contains(scalar)
    }

    private func isIdentifierContinuation(_ scalar: UnicodeScalar) -> Bool {
        isIdentifierStart(scalar) || CharacterSet.decimalDigits.contains(scalar)
    }
}
