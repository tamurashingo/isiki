# Lispヒープのバンプアロケータを16byte境界へ変更する

実施日: 2026-09-17 / ブランチ: `feature/heap-align16`
派生元: **`feature/alignment-audit`(PR #72、2026-09-17時点で未マージ)**。
PR #72 で入れた `ALIGN_AUDIT` の計測基盤をそのまま検証に使うため、main ではなく
そちらから派生している。PR #72 が先にマージされる前提。

タグ体系・タグ幅・`TAG_MASK` は**変更していない**。タグは引き続き下位3bitで、
外から見える挙動は変わらない。本フェーズで保証するのは「Lispヒープ上の
オブジェクトが16byte境界に配置されること」だけである。

---

## 1. 変更内容の要約

| 変更 | 内容 |
|---|---|
| 定数の新設 | `OS_HEAP_ALIGN`(既定16)と `os_heap_align_up()` を `runtime.h` に置き、切り上げを1箇所へ集約した。2のべき乗であること・8以上であること・`TAG_MASK` より大きいことを `_Static_assert` で保証する |
| From空間 | `os_alloc_bytes` の切り上げを 8 → `OS_HEAP_ALIGN` |
| To空間 | `gc_to_alloc` の切り上げを 8 → `OS_HEAP_ALIGN`(From側と**必ず同じ**単位) |
| 半空間の分割 | `os_heap_init` が、先頭アドレスを `OS_HEAP_ALIGN` へ切り上げ、半空間サイズを `OS_HEAP_ALIGN` の倍数にし、**From/To を等サイズ**にする |
| ヒープ先頭 | `os_boot_alloc_finalize` の切り上げを 8 → `OS_HEAP_ALIGN`(この戻り値がそのままLispヒープの先頭になる) |
| Immobilized Space | `os_imm_slot_alloc` の `(size + 15) & ~15ULL` を `os_heap_align_up()` へ(値は16のまま、**挙動は不変**。直書きを消すためだけの変更) |
| 実行時検査 | `os_heap_init` の最後で、From/To 両空間の先頭が境界に乗っているかを確認し、外れていれば `os_panic` |
| 監査の強制化 | `ALIGN_AUDIT` で、Lispヒープ上のオブジェクトの境界外れを**失敗扱い**(記録して停止)にした。対象外の経路は従来どおり集計のみ |
| テスト追加 | 可変長オブジェクトの端数長ケースと、半空間の境界・等サイズ性の検査 |

### 1.1 半空間を等サイズにした理由(指示書の追加要求)

変更前は `g_to_end = heap_base + heap_size`、つまり **To空間は「残り全部」**だった。

```c
UINT64 half = (heap_size / 2) & ~7ULL;   // From空間だけ切り捨て
g_from_end = g_from_start + half;
g_to_start = g_from_end;
g_to_end   = (UINT8 *)(heap_base + heap_size);  // ← 残り全部
```

この形には2つの問題があった。

1. `g_to_start = heap_base + half` なので、`half` が16の倍数でなければ
   **To空間の先頭が境界から外れる**。From空間の確保だけ16に揃えても、
   フリップした瞬間に全オブジェクトが 8 mod 16 へずれる。
2. To空間は From空間より最大8byte大きく、フリップすると今度は From空間の方が
   大きい状態になる。コピーGCは「From空間を埋めきっても To空間へ必ず入る」ことが
   前提なので、この非対称性は本来望ましくない。

変更後は次のようにした。

```c
UINT64 base   = os_heap_align_up(heap_base);
UINT64 lost   = base - heap_base;
UINT64 usable = (heap_size > lost) ? (heap_size - lost) : 0;
UINT64 half   = (usable / 2) & ~(OS_HEAP_ALIGN - 1);

g_from_start = (UINT8 *)base;          // 16境界
g_from_end   = g_from_start + half;    // half が16の倍数
g_to_start   = g_from_end;             // ⇒ 16境界
g_to_end     = g_to_start + half;      // 両半空間が等サイズ
```

`base` の切り上げを呼び出し元(`os_boot_alloc_finalize`)だけに任せていない理由は、
ユニットテストのように `malloc` の戻り値を直接 `os_heap_init` へ渡す経路があるため。
入口で1回揃えておけば、どの経路から来ても不変条件が成立する。

---

## 2. Step 1: サイズ計算箇所の洗い出し

変更前に、確保サイズ・走査幅・コピー長を扱うすべての箇所を列挙した。

| # | 箇所 | 種類 | 変更前の単位 | 変更要否 | 根拠 |
|---|---|---|---|---|---|
| 1 | `runtime.c` `os_alloc_bytes` | 確保の切り上げ | 8 | **要** | Lispヒープ From空間。タグ付き値になるアドレスの出どころ |
| 2 | `runtime.c` `gc_to_alloc` | 確保の切り上げ | 8 | **要** | To空間。#1と同じ単位でなければ、満杯のFrom空間がTo空間へ入らなくなる |
| 3 | `runtime.c` `os_heap_init` の `half` | 区画サイズ | 8 | **要** | To空間の先頭が境界に乗るかを決める |
| 4 | `runtime.c` `os_boot_alloc_finalize` | 先頭アドレス | 8 | **要** | 戻り値がそのままLispヒープの先頭になる |
| 5 | `runtime.c` `os_imm_slot_alloc` | 確保の切り上げ | 16(直書き) | **要**(集約のみ) | 既に16。`TAG_RAW_POINTER` 化されるので同じ制約下にある。値は変えず定数経由にした |
| 6 | `runtime.c` `gc_scan_queue` | 走査の進め幅 | — | **否** | このGCは**明示キュー方式**(`g_gc_queue` にタグ付きポインタを積む)で、Cheneyの線形スキャンポインタを持たない。「次のオブジェクトの位置を求める」計算自体が存在しないので、確保側と同期させるべき第2の単位が無い |
| 7 | `runtime.c` `gc_copy_value` の `size` 算出 | コピー長 | — | **否** | CONS=16 / SYMBOL=32 / STRING=`8+word0` / INSTANCE=32。これは**中身の長さ**であって切り上げではない。切り上げは `gc_to_alloc` 側の1箇所だけで行う |
| 8 | `runtime.c` `gc_relocate_bignum`(`8*count`) | コピー長 | — | **否** | 確保側 `os_alloc_bytes(8 * count)` と一致。どちらも切り上げは呼び先で行う |
| 9 | `runtime.c` `gc_relocate_vector_block`(`8*(1+rank+total)`) | コピー長 | — | **否** | 確保側と一致。ブロック内オフセット `dst + 8*(1+rank)` は**相対位置**なので、ブロック先頭が動いても不変 |
| 10 | `runtime.c` `gc_relocate_stream`(`sizeof(os_stream_t)` / `str_cap`) | コピー長 | — | **否** | 同上 |
| 11 | 可変長の確保呼び出し(STRING `8+len` ×3箇所、vector本体、bignum limb、`os_alloc_raw`) | 要求サイズ | — | **否** | 要求は中身の長さのまま渡す。切り上げを呼び出し側に散らさないため |
| 12 | `runtime.c` GC_PAINT の塗り潰しループ | ヒープ走査 | — | **否** | `UINT64` 1語ずつ領域を端から端まで塗るだけで、オブジェクト境界を見ていない |
| 13 | `os_heap_used_ratio` / `primitive_heap_{total,used}_bytes` / `os_addr_region` / 転送済み判定の範囲検査 | 残量・領域判定 | — | **否** | ポインタ差と範囲比較のみ。単位に依存しない |
| 14 | `runtime.c` ALIGN_AUDIT の計数 | 監査 | — | **要**(強制化) | ヒープを走査せず `gc_copy_value` 経由で見る方式なので単位には非依存。Step 3-3 で「失敗扱い」にした |
| 15 | JIT(`src/c/za.c`) | インライン確保 | — | **否(該当なし)** | JITはヒープポインタを触らない。`g_from_ptr` / `os_alloc_bytes` の出現は**0件**で、確保はすべて `movabs`+`call` による `os_make_cons` / `os_make_instance` / `os_make_symbol` 等の呼び出し |
| 16 | AOT生成コード(`src/c/lisp_compiled.c`)とジェネレータ(`src/lisp/transpile.lisp`) | インライン確保 | — | **否(該当なし)** | `g_from_ptr` / `os_alloc_bytes` の出現は両方とも**0件**。すべて `os_make_*` 呼び出しに展開される |
| 17 | `os_boot_alloc(size, align)` の `align` 引数 | 確保 | 呼び出し側指定(8) | **否(対象外)** | `block_device_t` / `ide_bd_slot_t`。6章の対象外リスト |

**#6 が本フェーズの安全性の鍵である。** 指示書が警告している「確保サイズと
スキャンポインタの進め幅がずれるとヒープが壊れる」という危険は、このGCが
線形スキャンではなく明示キューを使っているために**構造的に存在しない**。
`gc_scan_queue` はキューから取り出したタグ付きポインタのフィールドを辿るだけで、
アドレスを加算して次のオブジェクトを探すことがない。

---

## 3. Step 2: ヒープ先頭アドレスの確認

| 確認項目 | 変更前 | 変更後 |
|---|---|---|
| UEFIから渡る領域の先頭 | `EfiConventionalMemory` 記述子の `PhysicalStart` = 4KB境界(`main.c`) | 同左(変更なし) |
| boot allocator 消費後の残り先頭(=Lispヒープ先頭) | `(bump + 7) & ~7` = **8byte境界** | `os_heap_align_up(bump)` = **16byte境界** |
| From空間の先頭 | `heap_base` をそのまま | `os_heap_align_up(heap_base)` |
| 半空間サイズ | `(heap_size / 2) & ~7` = 8の倍数 | `(usable / 2) & ~(OS_HEAP_ALIGN-1)` = 16の倍数 |
| To空間の先頭 | `heap_base + half` → **8 mod 16 になりうる** | `from_start + half` → **常に16境界** |
| To空間の終端 | `heap_base + heap_size`(残り全部) | `to_start + half`(From と等サイズ) |
| From/To のサイズ関係 | To が最大8byte大きい(フリップ後は From が大きい) | **完全に等しい** |

**容量への影響**: 先頭の切り上げで最大15byte、半空間サイズの丸めで最大15byte、
To空間を「残り全部」から「等サイズ」にしたことで最大8byte。合計しても
1半空間あたり数十byteで、QEMU既定(`QEMU_MEM=256M`)の半空間は数十MBあるため、
実測でも `%%heap-total-bytes` は変わらない範囲に収まっている(4章参照)。

`os_heap_init` の末尾に、両半空間の先頭が実際に境界へ乗っているかを起動時に
1回だけ確かめる検査を追加した。外れていれば `os_panic` で即座に止まる。

---

## 4. Step 4: 検証結果

### 4.1 正しさ

| 検証 | 結果 |
|---|---|
| `make test`(既定ビルド、フラグ無し) | **8170 OK / 0 NG**。変更前の 8158 に対し +12 で、内訳は本フェーズで追加した2テストのアサーション数(境界ケース6 + 半空間6)と一致する。既存テストの失敗は0件 |
| `make test-qemu` 相当 `QEMU_MEM=256M` | **3269 passed / 0 failed**(変更前と同じ) |
| `make test-qemu` 相当 `QEMU_MEM=96M` | **3269 passed / 0 failed**(変更前と同じ) |
| 強制GC(`(%%diag-gc-stress 1000)` + `GC_DEBUG=1`、GC 231回) | **733 passed / 0 failed**(変更前と同じ) |
| 塗り潰し監査22試験 | **21 OK / 1 FAIL**(変更前と同じ)。全22グループが `heap-align=16 violations=0` で完走し、`VIOLATION` 行は1件も出ていない。FAIL は `aot_leaf_gc_test` の既存1件(下記) |
| ALIGN_AUDIT の違反件数(`violations=`) | 全条件(96M / 256M / 強制GC231回 / 22グループ)で **0件** |
| 可変長オブジェクトの境界ケース(新規テスト) | 長さ 0/1/7/8/9/15/16/17/23/24/25 の string と、その間の cons を確保 → GC発火 → 再検査。確保直後・GC後ともに **misaligned 0件**、内容も全件一致 |
| 半空間の境界・等サイズ(新規テスト) | `os_heap_init` に 8 mod 16 の base を渡しても、From/To 両空間の先頭が16境界で、サイズが等しく、16の倍数であることを確認 |

`ALIGN_AUDIT=1` ビルドでは、Lispヒープ上のオブジェクトが16境界を外れた時点で
`VIOLATION` 行を出して即座に停止する(ゲストは `os_panic`、ネイティブテストは
`exit(1)`)。**上記の各試験が完走して `violations=0` で終わっていること自体が、
1件も外れなかったことの証明になっている。**

この検出器が実際に発火することは、本フェーズの途中で確認済みである(6.1 参照):
`nil` を範囲で除外する前は、最初のGCで
`[ALIGN] VIOLATION value CONS value=63273577 low4bit=9` を出して停止していた。
「0件だから通った」ではなく「発火する仕組みが 0件を返した」と言える。

**`aot_leaf_gc_test` の FAIL について**: 落ちているのは
`(> (- LEAF-GC-STRESS-GC-AFTER LEAF-GC-STRESS-GC-BEFORE) 1)`、つまり
「ストレスループ中にGCが2回以上走ったか」という前提条件のアサーションで、
`gc=0` が記録されている。これは**この1ファイルだけを新しいヒープで単独起動すると
GCが1回も走らない**という既存の性質によるもので、PR #72 で
「同じ起動条件なら既定ビルドでも同じ1件が落ちる」ことを確認済みである。
全試験を1ブートで流す条件(`QEMU_MEM` 256M / 96M)では 0 failed。
**変更前後で同じ試験が同じ内容で落ちている**ことをもって、既存の性質として扱う。

### 4.2 影響の計測(変更前後の比較)

同じソース・同じバイナリ構成のまま `-DOS_HEAP_ALIGN=8ULL` で切り上げ単位だけ
戻したものを「変更前」として、**2つの設定を交互に**回して取った
(交互にしたのは 6.2 の再ビルド漏れを踏んだため。`heap-align=N` が毎回
期待どおり切り替わっていることを出力で確認している)。

`gc-live` は各GCがTo空間へコピーし終えた総バイト数、つまり**そのGC時点の生存量**。

#### `QEMU_MEM=96M`(全試験、GC 70回)

| | GC回数 | 生存量(peak) | 生存量(全GC合計) | QEMU実行時間 | 結果 |
|---|---|---|---|---|---|
| 8byte切り上げ | 70 | 5,612,160 | 231,069,712 | 23s | 3269 passed / 0 failed |
| 16byte切り上げ | 70 | 5,622,704 | 231,781,648 | 23s | 3269 passed / 0 failed |
| **差** | **±0** | **+10,544(+0.19%)** | +711,936(+0.31%) | **±0** | 同じ |

#### `QEMU_MEM=256M`(全試験、GC 14回)

| | GC回数 | 生存量(peak) | 生存量(全GC合計) | QEMU実行時間 | 結果 |
|---|---|---|---|---|---|
| 8byte切り上げ | 14 | 5,612,160 | 73,053,328 | 177s | 3269 passed / 0 failed |
| 16byte切り上げ | 14 | 5,622,704 | 73,200,192 | 176s | 3269 passed / 0 failed |
| **差** | **±0** | **+10,544(+0.19%)** | +146,864(+0.20%) | **-1s(誤差)** | 同じ |

#### 強制GC(`(%%diag-gc-stress 1000)`、`GC_DEBUG=1`、GC 231回)

| | GC回数 | 生存量(peak) | 生存量(全GC合計) | QEMU実行時間 | 結果 |
|---|---|---|---|---|---|
| 8byte切り上げ | 231 | 224,104 | 48,051,232 | 16s | 733 passed / 0 failed |
| 16byte切り上げ | 231 | 228,016 | 48,906,064 | 16s | 733 passed / 0 failed |
| **差** | **±0** | **+3,912(+1.75%)** | +854,832(+1.78%) | **±0** | 同じ |

強制GC条件だけ +1.75% とやや大きいのは、この構成(`init_test` + `za_test`)の
生存量が 224KB と小さく、その中で `os_bootstrap` の固定分(+416 byte)と
テスト自身が持つ文字列の比率が相対的に高いため。

#### `os_bootstrap` 直後(ネイティブ、同じ測定コードを新旧ソースへ当てて比較)

| | 生存バイト数 | intern済みシンボル数 |
|---|---|---|
| 8byte切り上げ | 21,120 | 175 |
| 16byte切り上げ | 21,536 | 175 |
| **差** | **+416(+1.97%)** | 同じ |

**まとめ**: GC回数は**まったく変わらない**(70回/14回とも一致)。生存量の増加は
**+0.19%**、実行時間は誤差の範囲。起動直後だけ +1.97% と比率が大きいのは、
ブート時に作られるオブジェクトに文字列(symbol名)の比率が高いためで、
ワークロードが回り始めると cons が支配的になって比率が下がる。

### 4.3 容量(半空間サイズ)への影響

`%%heap-total-bytes`(= From空間の総バイト数)は QEMU の各条件で変化しなかった。
`os_heap_init` の変更で失われうるのは、先頭の切り上げ(最大15byte)+
半空間サイズの丸め(最大15byte)+ To空間を「残り全部」から等サイズにした分
(最大8byte)で、半空間が数十MBある構成では観測できない大きさである。

### 4.4 型ごとの増加バイト数

切り上げ単位を8→16にして**サイズが増える型は STRING と可変長の生ブロックだけ**である。

| 型 | 確保要求 | 8byte切り上げ | 16byte切り上げ | +8 になる条件 |
|---|---|---|---|---|
| CONS | 16 | 16 | 16 | **なし**(元から16の倍数) |
| SYMBOL | 32 | 32 | 32 | **なし** |
| INSTANCE(全MAGIC共通) | 32 | 32 | 32 | **なし** |
| STRING | `8 + len` | `8 + 8⌈len/8⌉` | `16⌈(8+len)/16⌉` | `len % 16 ∈ {0, 9, 10, …, 15}`(16通り中8通り = ちょうど半分) |
| bignum の limb配列 | `8 * count` | 同左 | 16へ切り上げ | `count` が奇数 |
| vector 本体(1次元) | `8 * (2 + n)` | 同左 | 16へ切り上げ | 要素数 `n` が奇数 |
| `os_stream_t` | `sizeof` = **1648** | 1648 | 1648 | **なし**(1648 % 16 == 0) |
| stream の文字列バッファ | `str_cap` | 同左 | 16へ切り上げ | `str_cap % 16 ∈ {1..8}` |

`os_bootstrap` 直後の +416 byte = **52 × 8 byte** は、この表の STRING 行だけで
説明がつく(CONS/SYMBOL/INSTANCE は1byteも増えないため)。

### 4.5 非侵襲性

`ALIGN_AUDIT` を渡さない既定ビルドでは、`ALIGN_AUDIT_NOTE_*` が `((void)0)` に
展開され、監査の実装本体も `#ifdef` ごと消える。`make test`(フラグ無し)の出力に
`[ALIGN]` 行は **0行**。

ただし本フェーズの変更のうち **`OS_HEAP_ALIGN` による切り上げ自体は既定ビルドにも
入る**(それが目的なので当然である)。PR #72 のような「オブジェクトファイルが
バイト一致する」性質は、当然ながら成立しない。

---

## 5. 期待値を書き換えたテスト

期待値を書き換えたテストは **2件**。どちらも「値を変えないと通らないから変えた」の
ではなく、**なぜ変わったかを実測で確定させてから**変えている。

### 5.1 `SMALL_HEAP_SIZE` 77KB → 79KB(`test/c/runtime_test.c`)

**症状**: 77KB のままだと `runtime_test` が `test_gc_fires_during_gcd_and_isqrt_*`
で停止する(アサーション失敗ではなく**ハング**)。ユニットテストビルドでは
`os_panic` が `for(;;)` に落ちるため、ヒープ枯渇がハングとして現れる。

**原因**: 切り上げ単位を8→16にしたことで `os_bootstrap` 直後の生存量が増えた。
実測(同じ測定コードを新旧のソースへ当てて比較):

| | `os_bootstrap` 直後の生存バイト数 | intern済みシンボル数 |
|---|---|---|
| 8byte切り上げ | 21,120 | 175 |
| 16byte切り上げ | **21,536** | 175 |
| 差 | **+416 byte(+1.97%)** | 同じ |

+416 = 52 × 8 byte。増えるのは **STRING だけ**で、CONS(16)/SYMBOL(32)/
INSTANCE(32)は元から16の倍数なので1byteも増えない(4.4節の型別表)。
`SMALL_HEAP_SIZE` のもとでは半空間が 39,424 byte しかなく、
そのうち 21,536 byte が起動直後から埋まっているため、この +416 が
gcd/isqrt テストの作業領域を削り切ってしまう。

**新しい値の決め方**: 推測せず、`-DSMALL_HEAP_SIZE=...` で掃引して
「通る窓」を新旧それぞれ実測した。

| SMALL_HEAP_SIZE | 8byte切り上げ(変更前) | 16byte切り上げ(変更後) |
|---|---|---|
| 75KB | ハング | — |
| 76KB | ハング | — |
| 77KB | 5319 OK / 0 NG | **ハング** |
| 78KB | 5319 OK / 0 NG | 5331 OK / 0 NG |
| **79KB** | **5319 OK / 0 NG** | **5331 OK / 0 NG** |
| 80KB | NG 1件(isqrt の「GCが発火する」が不成立) | 5331 OK / 0 NG |
| 81KB | NG 2件 | — |
| 82KB | — | NG 1件(isqrt) |
| 84KB | — | NG 2件(bignum加算 + isqrt) |

上限側は**テストの意図**で決まる。ヒープを広げすぎると計算の途中でGCが発火せず、
`gc_count_after > gc_count_before` が不成立になる。つまりこのテストは
「完走できるだけの広さ」と「GCが起きるだけの狭さ」の両方を要求する。

窓は **77〜79KB → 78〜80KB** へ、**幅3KBのまま1KBぶん上へずれただけ**である。
これは生存量が一定量だけ増えたという原因と正確に整合する。
採用した **79KB は新旧どちらの窓にも入る唯一の値**で、「新しい実装でだけ通る値」
へ逃げていないことがこの掃引から言える。

併せて `SMALL_HEAP_SIZE` を `#ifndef` で囲み、`-D` から上書きできるようにした。
次に再調整が要るときも、掃引で実測して決められる。

### 5.2 `os_boot_alloc_finalize` の整列アサーション(`test/c/runtime_test.c`)

```c
-  assert(out_base % 8 == 0, "残り領域の先頭は8byte境界に整列される");
+  assert(out_base % OS_HEAP_ALIGN == 0,
+         "残り領域の先頭はOS_HEAP_ALIGN境界に整列される(Lispヒープの先頭になるため)");
```

これは**失敗していたから変えたのではない**(16境界は8境界を含むので、旧
アサーションは変更後も通る)。保証が強くなったのにテストが古い弱い条件しか
見ていないと、`os_boot_alloc_finalize` を8byte切り上げへ戻す退行を検出できない。
新しい保証に合わせて条件を締めた。

---

## 6. 対象外として残した16境界外れ

本フェーズでは Lispヒープと Immobilized Space だけを対象にした。以下は
PR #72 の調査で16境界から外れることが分かっているが、**意図的に未対応のまま
残している**。`ALIGN_AUDIT` でも集計のみ続け、失敗扱いにはしない
(`align_site_is_enforced` / `align_tag_is_enforced` がその線引きである)。

| 対象 | 状況 | 次フェーズでの対応候補(PR #72 報告書 5.3) |
|---|---|---|
| JITリテラルスロット `&g_za_*_slots[i]` | 8byte刻みの静的 `lisp_val_t` 配列の要素アドレスを `TAG_RAW_POINTER` 化している。奇数添字は必ず 8 mod 16(実測 49.2〜49.9%) | C5: 配列を16byte刻みにする、または `os_imm_slot_alloc` へ移す。C6: `os_environment_register_literal_slot` にアライン要件を課す |
| `block_device_t*` ハンドル | `os_boot_alloc(…, 8)` + `sizeof(block_device_t) == 104`。実測 100% が 8 mod 16 | C4: 呼び出しを `align=16` にする |
| `g_nil_cell` | `__attribute__((aligned(8)))`。既定ビルドでは 8 mod 16、GC_DEBUG ビルドでは16境界という**ビルド依存** | C3: `aligned(16)` を付ける |
| `os_boot_alloc(size, align)` 一般 | `align` は呼び出し側指定のまま | 呼び出し側ごとに判断 |
| FIXNUM / CHAR の即値表現 | bit3 を値の最下位bitとして使用中(`<< 3`) | C8: タグ拡張そのものとセットの設計判断 |
| JIT の `0xF8` 直書き9箇所 | `~TAG_MASK` を独立定数として持っている。現行値とは一致しており今バグではない | C7: `TAG_MASK` から導出する |

なお `os_imm_slot_alloc` / `os_imm_page_alloc` / `os_imm_pages_alloc_contiguous` /
環境のページハンドルは元から16境界を満たしていたので、**強制対象に含めた**
(集計のみに留めず、外れたら失敗にする)。

### 6.1 本フェーズで新たに見つけたもの

| 対象 | 内容 |
|---|---|
| `nil`(= `g_nil_cell \| TAG_CONS`)が **TAG_CONS 付きの値として** 16境界を外れる | PR #72 では「静的オブジェクト `g_nil_cell` が `aligned(8)`」として記録していたが、本フェーズで監査を失敗扱いにした結果、**`gc_copy_value` を通る CONS 値としても現れる**ことが実測で分かった。既定ビルドの `g_nil_cell` は `...a68`、つまり `nil` の下位4bitは `9` になる。<br>これに合わせて、値側の強制対象を「タグ」だけでなく「**アドレスがFrom/To空間の中にあること**」でも絞った(`align_addr_is_in_lisp_heap`)。本フェーズの保証範囲がLispヒープに限られる以上、述語も範囲で書くのが正しく、「nilだけ例外」という列挙を足さずに済む。<br>`g_nil_cell` を `aligned(16)` にすれば解消するが、2章の対象外リストに入っているため本フェーズでは触っていない |
| 監査の「報告」と「強制」は別基準にする必要があった | ヒストグラムは常に下位4bit(16ビン)で取りたい一方、違反判定は `OS_HEAP_ALIGN` 基準でなければ `-DOS_HEAP_ALIGN=8ULL` の比較計測ができない(自分自身の違反で停止してしまう)。`ALIGN_AUDIT_OFFENDS` として分離した |
| **`EXTRA_CFLAGS` が再ビルドのスタンプに入っていなかった**(Makefile) | 下記 6.2 |

### 6.2 `EXTRA_CFLAGS` がビルドスタンプから漏れていた(本フェーズで踏んだ)

`Makefile` は `GC_DEBUG` / `GC_PAINT` については「フラグだけ変えても
`$(TARGET)` は再ビルドされない」問題を `GC_DEBUG_STAMP` で解決していた。
しかし **`EXTRA_CFLAGS` はスタンプに入っていなかった**。

`EXTRA_CFLAGS` は Makefile 自身が「計測のための一時的なフラグを渡す口」として
説明しているもので、まさに同じセッション中に付けたり外したりする使われ方をする。
本フェーズの前後比較でそれを踏んだ:

```
make ... EXTRA_CFLAGS=-DOS_HEAP_ALIGN=8ULL   # 比較用(8byte版)をビルドして測る
make ...                                     # 戻したつもりで16byte版を測る
```

2回目は `$(SRC)` / `$(HDR)` が更新されていないため再ビルドされず、
**8byte版のバイナリのまま測って `heap-align=8` と報告された**
(`BOOTX64.EFI` 11:07:13 > `runtime.c` 11:04:35)。

ALIGN_AUDIT が使用した切り上げ単位を `heap-align=N` として出力へ混ぜていたおかげで
その場で気づけたが、出していなければ「16byte化してもGC回数も消費量も**まったく**
変わらなかった」という、もっともらしいが誤った比較表になっていた。

対処として `BUILD_FLAG_SIGNATURE = $(GC_DEBUG_FLAGS) $(ALIGN_AUDIT_FLAGS) $(EXTRA_CFLAGS)`
をスタンプの内容にした。4章の計測はこの修正後に、**2つの設定を交互に**回して
取り直してある(`heap-align=N` が毎回期待どおり切り替わることを確認済み)。

---

## 7. 再現手順

```bash
# 境界が保たれていることの検証(違反があればゲストが即座にpanicで止まる)
make test-qemu-audit-run ALIGN_AUDIT=1 QEMU_MEM=96M \
     MILESTONE=test/lisp/qemu_boot_test.lisp \
     QEMU_EXTRA_FLAGS="-serial file:$PWD/tmp/h16/out.serial"
grep '^\[ALIGN\]' tmp/h16/out.serial

# 変更前(8byte切り上げ)との比較。同じバイナリ構成で切り上げだけ戻す
make test-qemu-audit-run ALIGN_AUDIT=1 EXTRA_CFLAGS=-DOS_HEAP_ALIGN=8ULL ...

# 小ヒープテストの通る窓を測り直す(SMALL_HEAP_SIZE を変える必要が出たとき)
gcc -DISIKIOS_UNIT_TEST "-DSMALL_HEAP_SIZE=(79*1024)" ... test/c/runtime_test.c
```

`[ALIGN]` の報告行には `heap-align=N violations=M` が出るので、どの切り上げ単位で
測った結果かは出力自体に残る。
