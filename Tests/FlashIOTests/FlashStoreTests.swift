import FlashIO
import XCTest

final class FlashStoreTests: XCTestCase {
    private var dir: URL!

    override func setUpWithError() throws {
        dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("FlashStoreTests-\(UUID().uuidString)")
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: dir)
    }

    func testPutGetRemoveAndReopen() throws {
        var store: FlashStore? = try FlashStore(directory: dir)
        try store!.put(Data("hello".utf8), forKey: "greeting")
        XCTAssertEqual(try store!.value(forKey: "greeting"), Data("hello".utf8))
        try store!.put(Data(), forKey: "empty")
        XCTAssertEqual(try store!.value(forKey: "empty"), Data())
        try store!.removeValue(forKey: "greeting", durability: .barrier)
        XCTAssertNil(try store!.value(forKey: "greeting"))
        store = nil // deinit closes and flushes

        let reopened = try FlashStore(directory: dir)
        XCTAssertNil(try reopened.value(forKey: "greeting"))
        XCTAssertEqual(try reopened.value(forKey: "empty"), Data())
        XCTAssertEqual(try reopened.stats().keys, 1)
    }

    func testSecondOpenIsLocked() throws {
        let store = try FlashStore(directory: dir)
        XCTAssertThrowsError(try FlashStore(directory: dir)) { error in
            XCTAssertEqual(error as? FlashIOError, .locked)
        }
        store.close()
        XCTAssertNoThrow(try FlashStore(directory: dir))
    }

    func testBatchAndCodable() throws {
        struct Profile: Codable, Equatable { var name: String; var level: Int }
        let store = try FlashStore(directory: dir)
        let batch = WriteBatch()
        for i in 0..<10 { try batch.put(Data([UInt8(i)]), forKey: "k\(i)") }
        try batch.remove(forKey: "k3")
        try store.write(batch, durability: .barrier)
        XCTAssertNil(try store.value(forKey: "k3"))
        XCTAssertEqual(try store.value(forKey: "k9"), Data([9]))

        try store.set(Profile(name: "a", level: 3), forKey: "profile")
        XCTAssertEqual(try store.get(Profile.self, forKey: "profile"), Profile(name: "a", level: 3))
        XCTAssertEqual(try store.keys().count, 10)
    }

    func testCompactionReclaimsSpace() throws {
        var opts = FlashStoreOptions()
        opts.segmentSize = 64 << 10
        opts.autoCompact = false
        let store = try FlashStore(directory: dir, options: opts)
        let blob = Data(repeating: 0xAB, count: 1024)
        for _ in 0..<20 { for i in 0..<40 { try store.put(blob, forKey: "k\(i)") } }
        let before = try store.stats()
        try store.compact()
        let after = try store.stats()
        XCTAssertLessThan(after.totalBytes, before.totalBytes)
        XCTAssertEqual(after.keys, 40)
        XCTAssertEqual(try store.value(forKey: "k5"), blob)
    }

    func testAtomicFileWriteAndClone() throws {
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let a = dir.appendingPathComponent("a.bin"), b = dir.appendingPathComponent("b.bin")
        try FlashFile.writeAtomically(Data("v1".utf8), to: a)
        try FlashFile.writeAtomically(Data("v2".utf8), to: a, durability: .full)
        try FlashFile.clone(from: a, to: b)
        XCTAssertEqual(try Data(contentsOf: b), Data("v2".utf8))
    }
}
