# FlashIO (storage_io)

**フラッシュメモリ (iOS / APFS on NAND) に特化したディスク I/O ミドルウェア。**

アプリが「小さなデータを頻繁に保存する」ときに起きがちな問題を、ストレージ層でまとめて解決します。

| よくある問題 | FlashIO での対処 |
|---|---|
| 小さなファイルを何度も上書きする → 書き込み増幅、NAND の摩耗 | **ログ構造化**: 上書きせず、セグメントファイルへの追記だけで更新する |
| 1 レコードごとに `write` / `fsync` | **書き込みの合体 (coalescing)**: 64 KiB (16 KiB NAND ページ × 4) にまとめて `pwrite` を 1 回 |
| `fsync` の意味が曖昧 (Darwin の `fsync` はデバイスキャッシュをフラッシュしない) | **耐久性を明示**: `none` / `barrier` (`F_BARRIERFSYNC`) / `full` (`F_FULLFSYNC`) |
| クラッシュや電源断で書きかけのデータが残る | **CRC32C 付きフレーム**: 起動時に検証し、壊れた末尾を自動で切り詰める |
| 複数キーをまとめて更新したい | **WriteBatch**: 1 フレーム = 1 CRC なので、全部反映されるか、何も反映されないかのどちらか |
| 削除・上書きでゴミが溜まる | **コンパクション**: ゴミの比率がしきい値を超えたセグメントだけを書き直す。`BGProcessingTask` から実行できる |
| ファイルのコピーでフラッシュに書き込みが発生する | **`clonefile(2)`** (APFS の CoW) で、データブロックを書かずにコピー |

## 構成

```
Sources/
  CFlashIO/            C コア (ポータブル POSIX + Darwin 固有 fcntl)
    include/flashio.h  公開 C API
    platform.c         sync 各種 / preallocate / nocache / atomic write / clone / CRC32C
    store.c            ログ構造化 KV ストア
  FlashIO/
    FlashStore.swift   Swift API (FlashStore, WriteBatch, FlashFile)
tests/c/               C テスト (モデルベースのランダムテスト含む) とベンチ
Tests/FlashIOTests/    XCTest
```

コアを C で書いた理由は 3 つです。iOS・macOS・Linux で同じコードをテストできること。ObjC / C++ からも使えること。ホットパスに ARC やブリッジのコストが入らないこと。

## 使い方 (Swift)

```swift
import FlashIO

let dir = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
    .appendingPathComponent("kv")

var opts = FlashStoreOptions()
opts.autoCompact = false                   // コンパクションは BGProcessingTask から実行する
let store = try FlashStore(directory: dir, options: opts)

try store.put(Data("hello".utf8), forKey: "greeting")          // .none: バッファに積むだけ (最速)
try store.set(profile, forKey: "profile", durability: .barrier) // 保存操作の推奨値
try store.put(receipt, forKey: "receipt", durability: .full)    // 絶対に失えないデータだけ

let batch = WriteBatch()
try batch.put(a, forKey: "a")
try batch.remove(forKey: "b")
try store.write(batch, durability: .barrier)                   // アトミック

let p = try store.get(Profile.self, forKey: "profile")

// BGProcessingTask (requiresExternalPower = true) の中で
try store.compact()

// 単一ファイルの安全な置き換え / APFS クローン
try FlashFile.writeAtomically(json, to: url)
try FlashFile.clone(from: src, to: dst)
```

### 耐久性レベルの選び方

| レベル | 実体 (Darwin) | 失われるケース | コスト |
|---|---|---|---|
| `.none` | プロセス内バッファ | flush 前にプロセスが落ちたとき | ほぼゼロ |
| `.barrier` | `F_BARRIERFSYNC` | 直近の書き込みが電源断で消える可能性あり。**ただし順序は保証され、壊れない** | 小 |
| `.full` | `F_FULLFSYNC` | なし | 大 (ms 単位、フラッシュの摩耗) |

`.none` で書いた分は、次のいずれかでファイルに書き出されます。

- バッファが満杯になったとき
- `flush()` を呼んだとき
- `close()` / deinit
- **アプリがバックグラウンドに移ったとき** (`flushOnBackground`。UIKit 環境のデフォルトは `.barrier`)

典型的な使い方は「`.none` で書き、区切りで `flush(.barrier)`」というグループコミットです。

## オンディスク形式

```
<dir>/LOCK                 flock による排他 (同じディレクトリの二重オープンを検出)
<dir>/0000000001.seg ...   セグメント (追記専用。最新の 1 つだけが書き込み対象)

frame := magic "FIO1" | crc32c | payload_len | count | payload
op    := type(1=put,2=del) | klen | vlen | key | value      (すべてリトルエンディアン)
```

- **リカバリ**: セグメントを ID 順に再生します。長さ不足、CRC 不一致、または不正なフレームが見つかった時点で停止し、そこから後ろを `ftruncate` で切り詰めます。
- **インデックス**: メモリ上のハッシュテーブルで、`key → (segment, value offset, length)` を保持します。値はメモリに持たず、`pread` で読みます。未 flush の値はバッファから直接返します。
- **セグメントの切り替え**: 切り替え時に古いセグメントへ `F_BARRIERFSYNC` を発行します。これで、後から別のセグメントに `.full` を発行したときに、それより前の書き込みがすべて永続化されることが保証されます。
- **コンパクション**: 生きているレコードを最新セグメントへ書き直し、barrier を発行してから古いセグメントを ID の昇順に unlink します。
  - 墓石 (削除レコード) は、より古いセグメントが残っている間だけ引き継ぎます (削除したキーの復活を防ぐため)。
  - 書き直し対象のセグメントは、`F_NOCACHE` を設定した fd で一括して順次読みします。アプリのホットなページキャッシュを汚さないためです。
- **ポイズニング**: `pwrite` / `fsync` が失敗すると、ストアはそれ以降の操作を `FIO_EPOISONED` で拒否します。fsync の失敗後にページキャッシュの状態は信用できないため、開き直して回復させます。

## iOS 固有の注意点

- **データ保護**: 既定ではディレクトリに `completeUntilFirstUserAuthentication` を設定します。新しいセグメントファイルはこのクラスを継承します。`.complete` にすると、ロック中のバックグラウンド処理で読めなくなります。
- **App Group コンテナ**: 共有コンテナ内のファイルロックを保持したままサスペンドすると、アプリが `0xdead10cc` で強制終了されます。共有コンテナに置く場合は、バックグラウンドに移る前に `close()` してください。
- **バックアップ**: 再生成できるデータなら、`excludeFromBackup = true` にするか Caches に置いてください。
- **スレッド**: 読み取りは並行して実行できます (rwlock)。書き込みは直列化されます。`.barrier` / `.full` の呼び出しはブロックするので、メインスレッドから呼ばないでください。

## ビルドとテスト

```sh
make test            # C コアのテスト (ASan + UBSan)。Linux / macOS
make test SAN=-fsanitize=thread -B
make bench           # 保存パターンの比較 (参考値)
swift test           # macOS (Xcode 環境が必要)
```

C のテストは以下をカバーしています。

- 基本操作
- 二重オープンのロック
- バッチの原子性
- 書きかけの末尾 (torn tail) とゴミの切り詰め
- コンパクション後に墓石で消したキーが復活しないこと
- atomic write / clone
- 参照モデルとの比較 (2 万ステップのランダム操作。途中で再オープンとコンパクションを挟む)
- 4 スレッドの並行アクセス

開発用の Linux コンテナで取ったベンチの参考値です (3000 件 × 200 B、300 キー)。

```
file-per-record + sync each :  ~1500 ms  (6000 syncs)
FlashIO put + BARRIER each  :   ~400 ms  (3000 syncs)
FlashIO put + BARRIER /100  :    ~10 ms  (30 syncs, 30 pwrites)
```

実機の数値は、必ず iOS デバイスで計測してください。

## 今後の候補

- 並行する書き込みをまとめて 1 回で sync するグループコミット
- キーのプレフィックス範囲スキャン (ソート済みインデックス)
- インデックスのスナップショット (ヒントファイル) による起動時間の短縮
- 大きな値 (例: 1 MiB 以上) を別ファイルに分離し、`clonefile` で参照する
- 実機用のベンチ (`os_signpost` と書き込みバイト数の計測)
