import CFlashIO
import Foundation
#if canImport(UIKit) && !os(watchOS)
import UIKit
#endif

// MARK: - Errors / durability

public struct FlashIOError: Error, CustomStringConvertible, Equatable {
    public let code: Int32
    public var description: String { String(cString: fio_strerror(code)) }

    public static let notFound = FlashIOError(code: FIO_ENOTFOUND)
    public static let corrupt = FlashIOError(code: FIO_ECORRUPT)
    public static let locked = FlashIOError(code: FIO_ELOCKED)
    public static let invalidArgument = FlashIOError(code: FIO_EINVAL)
    public static let poisoned = FlashIOError(code: FIO_EPOISONED)

    @inline(__always)
    static func check(_ rc: Int32) throws {
        if rc != FIO_OK { throw FlashIOError(code: rc) }
    }
}

/// How far a write must travel before the call returns.
public enum Durability: Sendable {
    /// Kept in the store's write-coalescing buffer. Cheapest; lost if the
    /// process dies before the next flush (see `FlashStore.flush`).
    case none
    /// `F_BARRIERFSYNC`: ordered and persisted without a full drive-cache
    /// flush. The right default for "save" operations on iOS.
    case barrier
    /// `F_FULLFSYNC`: flush the device cache. Slow and costs flash wear;
    /// reserve for data that must never be lost (purchases, credentials…).
    case full

    var c: fio_durability {
        switch self {
        case .none: return FIO_DURABILITY_NONE
        case .barrier: return FIO_DURABILITY_BARRIER
        case .full: return FIO_DURABILITY_FULL
        }
    }
}

// MARK: - Options / stats

public struct FlashStoreOptions {
    /// A new segment file is started after this many bytes.
    public var segmentSize: UInt64 = 8 << 20
    /// Small writes are coalesced up to this size before a single `pwrite`.
    /// Keep it a multiple of 16 KiB (NAND page size on Apple devices).
    public var writeBufferSize: UInt32 = 64 << 10
    /// Sealed segments whose garbage ratio reaches this are compacted.
    public var compactionThreshold: Double = 0.5
    /// Compact automatically on segment roll. Turn off and call
    /// `compact()` from a `BGProcessingTask` to keep the foreground smooth.
    public var autoCompact: Bool = true
    /// Reserve blocks for new segments with `F_PREALLOCATE`.
    public var preallocate: Bool = false
    /// Sync level used by `close()`.
    public var closeDurability: Durability = .barrier
    /// Data protection class applied to the store directory (iOS family).
    /// `.completeUntilFirstUserAuthentication` lets background tasks read
    /// the store while the device is locked.
    #if os(iOS) || os(tvOS) || os(watchOS) || os(visionOS)
    public var fileProtection: FileProtectionType? = .completeUntilFirstUserAuthentication
    #endif
    /// Mark the directory as excluded from iCloud / iTunes backup.
    public var excludeFromBackup: Bool = false
    /// Flush the write buffer when the app moves to the background (UIKit).
    public var flushOnBackground: Durability? = .barrier

    public init() {}

    var c: fio_options {
        var o = fio_options()
        fio_options_init(&o)
        o.segment_size = segmentSize
        o.write_buffer_size = writeBufferSize
        o.compaction_threshold = compactionThreshold
        o.auto_compact = autoCompact ? 1 : 0
        o.preallocate = preallocate ? 1 : 0
        o.close_durability = closeDurability.c
        return o
    }
}

public struct FlashStoreStats: Sendable {
    public let keys: UInt64
    public let segments: UInt64
    public let liveBytes: UInt64
    public let totalBytes: UInt64
    public let userBytes: UInt64
    public let logBytes: UInt64
    public let compactionBytes: UInt64
    public let physicalBytes: UInt64
    public let writeCalls: UInt64
    public let barrierSyncs: UInt64
    public let fullSyncs: UInt64
    public let compactions: UInt64
    public let reclaimedBytes: UInt64
    public let recoveredTruncatedBytes: UInt64

    /// Bytes handed to the file system per byte the app asked to store.
    public var writeAmplification: Double {
        userBytes == 0 ? 0 : Double(logBytes + compactionBytes) / Double(userBytes)
    }
    /// Fraction of on-disk bytes that are dead and reclaimable.
    public var garbageRatio: Double {
        totalBytes == 0 ? 0 : 1 - Double(liveBytes) / Double(totalBytes)
    }

    init(_ s: fio_stats) {
        keys = s.keys; segments = s.segments
        liveBytes = s.live_bytes; totalBytes = s.total_bytes
        userBytes = s.user_bytes; logBytes = s.log_bytes
        compactionBytes = s.compaction_bytes; physicalBytes = s.physical_bytes
        writeCalls = s.write_calls; barrierSyncs = s.barrier_syncs
        fullSyncs = s.full_syncs; compactions = s.compactions
        reclaimedBytes = s.reclaimed_bytes
        recoveredTruncatedBytes = s.recovered_truncated_bytes
    }
}

// MARK: - Batch

/// Mutations applied atomically: after a crash either all or none are visible.
public final class WriteBatch {
    let raw: OpaquePointer

    public init() {
        guard let b = fio_batch_new() else { fatalError("FlashIO: out of memory") }
        raw = b
    }
    deinit { fio_batch_free(raw) }

    public var count: Int { fio_batch_count(raw) }

    public func put(_ value: Data, forKey key: Data) throws {
        try key.withUnsafeBytes { k in
            try value.withUnsafeBytes { v in
                try FlashIOError.check(fio_batch_put(raw, k.baseAddress, k.count, v.baseAddress, v.count))
            }
        }
    }

    public func put(_ value: Data, forKey key: String) throws {
        try put(value, forKey: Data(key.utf8))
    }

    public func remove(forKey key: Data) throws {
        try key.withUnsafeBytes { k in
            try FlashIOError.check(fio_batch_delete(raw, k.baseAddress, k.count))
        }
    }

    public func remove(forKey key: String) throws {
        try remove(forKey: Data(key.utf8))
    }

    public func removeAll() { fio_batch_clear(raw) }
}

// MARK: - Store

/// Log-structured key/value store tuned for flash.
///
/// Thread-safe: reads run concurrently, writes are serialized inside the C
/// core. Calls block on file I/O, so keep them off the main thread for
/// `.barrier` / `.full` durability or large values.
public final class FlashStore: @unchecked Sendable {
    public let directory: URL
    private var raw: OpaquePointer?
    private let lock = NSLock()
    private var observers: [NSObjectProtocol] = []

    public init(directory: URL, options: FlashStoreOptions = FlashStoreOptions()) throws {
        self.directory = directory
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        try FlashStore.applyDirectoryAttributes(directory, options)

        var o = options.c
        var handle: OpaquePointer?
        try FlashIOError.check(directory.withUnsafeFileSystemRepresentation { path in
            fio_store_open(path, &o, &handle)
        })
        raw = handle
        installLifecycleHooks(options.flushOnBackground)
    }

    deinit { close() }

    /// Flushes (with `closeDurability`) and releases the directory lock.
    /// On iOS, close stores that live in an App Group container before the
    /// app suspends: holding a file lock there gets the app killed (0xdead10cc).
    public func close() {
        lock.lock()
        let h = raw
        raw = nil
        let obs = observers
        observers = []
        lock.unlock()
        for o in obs { NotificationCenter.default.removeObserver(o) }
        if let h { fio_store_close(h) }
    }

    private func handle() throws -> OpaquePointer {
        lock.lock(); defer { lock.unlock() }
        guard let h = raw else { throw FlashIOError.invalidArgument }
        return h
    }

    // MARK: read / write

    public func put(_ value: Data, forKey key: Data, durability: Durability = .none) throws {
        let h = try handle()
        try key.withUnsafeBytes { k in
            try value.withUnsafeBytes { v in
                try FlashIOError.check(fio_store_put(h, k.baseAddress, k.count, v.baseAddress, v.count, durability.c))
            }
        }
    }

    public func put(_ value: Data, forKey key: String, durability: Durability = .none) throws {
        try put(value, forKey: Data(key.utf8), durability: durability)
    }

    public func value(forKey key: Data) throws -> Data? {
        let h = try handle()
        var out: UnsafeMutableRawPointer?
        var len = 0
        let rc = key.withUnsafeBytes { k in fio_store_get(h, k.baseAddress, k.count, &out, &len) }
        if rc == FIO_ENOTFOUND { return nil }
        try FlashIOError.check(rc)
        guard let p = out else { return Data() }
        return Data(bytesNoCopy: p, count: len, deallocator: .custom { ptr, _ in fio_free(ptr) })
    }

    public func value(forKey key: String) throws -> Data? {
        try value(forKey: Data(key.utf8))
    }

    public func removeValue(forKey key: Data, durability: Durability = .none) throws {
        let h = try handle()
        try key.withUnsafeBytes { k in
            try FlashIOError.check(fio_store_delete(h, k.baseAddress, k.count, durability.c))
        }
    }

    public func removeValue(forKey key: String, durability: Durability = .none) throws {
        try removeValue(forKey: Data(key.utf8), durability: durability)
    }

    public func write(_ batch: WriteBatch, durability: Durability = .none) throws {
        try FlashIOError.check(fio_store_write(try handle(), batch.raw, durability.c))
    }

    // MARK: Codable convenience

    public func set<T: Encodable>(_ value: T, forKey key: String, durability: Durability = .none,
                                  encoder: JSONEncoder = JSONEncoder()) throws {
        try put(encoder.encode(value), forKey: key, durability: durability)
    }

    public func get<T: Decodable>(_ type: T.Type, forKey key: String,
                                  decoder: JSONDecoder = JSONDecoder()) throws -> T? {
        guard let data = try value(forKey: key) else { return nil }
        return try decoder.decode(type, from: data)
    }

    // MARK: maintenance

    /// Push buffered writes to the file system with the given durability.
    public func flush(_ durability: Durability = .barrier) throws {
        try FlashIOError.check(fio_store_sync(try handle(), durability.c))
    }

    /// Rewrite live records out of mostly-dead segments and delete them.
    /// Ideal place: a `BGProcessingTask` with `requiresExternalPower = true`.
    public func compact() throws {
        try FlashIOError.check(fio_store_compact(try handle()))
    }

    public func stats() throws -> FlashStoreStats {
        var s = fio_stats()
        try FlashIOError.check(fio_store_stats(try handle(), &s))
        return FlashStoreStats(s)
    }

    /// All live keys (unordered).
    public func keys() throws -> [Data] {
        final class Box { var keys: [Data] = [] }
        let box = Box()
        let ctx = Unmanaged.passUnretained(box).toOpaque()
        try FlashIOError.check(fio_store_foreach_key(try handle(), { k, n, ctx in
            let b = Unmanaged<Box>.fromOpaque(ctx!).takeUnretainedValue()
            b.keys.append(Data(bytes: k!, count: n))
            return 0
        }, ctx))
        return box.keys
    }

    // MARK: iOS integration

    private static func applyDirectoryAttributes(_ url: URL, _ o: FlashStoreOptions) throws {
        #if os(iOS) || os(tvOS) || os(watchOS) || os(visionOS)
        if let p = o.fileProtection {
            // New segment files inherit the directory's protection class.
            try FileManager.default.setAttributes([.protectionKey: p], ofItemAtPath: url.path)
        }
        #endif
        #if canImport(Darwin)
        if o.excludeFromBackup {
            var u = url
            var rv = URLResourceValues()
            rv.isExcludedFromBackup = true
            try u.setResourceValues(rv)
        }
        #endif
    }

    private func installLifecycleHooks(_ durability: Durability?) {
        #if canImport(UIKit) && !os(watchOS)
        guard let durability else { return }
        let names: [Notification.Name] = [
            UIApplication.didEnterBackgroundNotification,
            UIApplication.willTerminateNotification,
        ]
        observers = names.map { name in
            NotificationCenter.default.addObserver(forName: name, object: nil, queue: nil) { [weak self] _ in
                try? self?.flush(durability)
            }
        }
        #endif
    }
}

// MARK: - File helpers

public enum FlashFile {
    /// Replace the file at `url` so that readers (and crash recovery) see either
    /// the old or the new content, never a mix.
    public static func writeAtomically(_ data: Data, to url: URL, durability: Durability = .barrier) throws {
        try url.withUnsafeFileSystemRepresentation { path in
            try data.withUnsafeBytes { b in
                try FlashIOError.check(fio_atomic_write_file(path, b.baseAddress, b.count, durability.c))
            }
        }
    }

    /// Copy using APFS `clonefile` (no data blocks written) when possible.
    public static func clone(from src: URL, to dst: URL) throws {
        try src.withUnsafeFileSystemRepresentation { s in
            try dst.withUnsafeFileSystemRepresentation { d in
                try FlashIOError.check(fio_clone_file(s, d))
            }
        }
    }
}
