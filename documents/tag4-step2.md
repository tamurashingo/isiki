# タグ拡張 第2段階 実施報告（3bit → 4bit の一括変更）

対象ブランチ: `feature/tag4-step2`（`main` = b376b84、PR #77 マージ後から分岐）

前提文書:
- 設計: `documents/tag4-design.md`（PR #76）
- 第1段階: `documents/tag4-step1.md`（PR #77）
- JIT 定数の導出: `documents/jit-tag-constants.md`（PR #75）

---

## 1. Step 0 の確認結果

### 1-1. forwarding pointer の判定箇所

`TAG_FORWARD` を参照している箇所を全数（`src/c` / `test/c` / `src/lisp`）洗い、
さらに `word0` / `words[0]` の下位ビットを見ている式を機械的に探した。

**下位ビットを見て転送済みと判定している箇所は `gc_copy_value` の1箇所だけ**である。

| 場所 | 種別 |
|---|---|
| `src/c/runtime.c:2022` `if ((word0 & TAG_MASK) == TAG_FORWARD)` | **判定。直後（2023-2038行）がTo空間の範囲検査** |
| `runtime.c:693` `align_tag_name` | 監査レポートのラベル |
| `runtime.c:1483` `g_gc_uncopyable_tag_hits` | 計数のコメント |
| `runtime.c:1391` / `2026` / `2061` / `2407` / `2603` | コメント・診断メッセージ |
| `za.c:6293` / `bench_subprimitive.c:286` | コメント |
| `test/c/runtime_test.c:1681` | 誤検知テストの前提条件 |

つまり **範囲検査の外にある判定は存在しない**。PR #77 で「MAGIC 全14個の下位3bitが
`TAG_FORWARD` と衝突している」状態を作ったが、それを弾いていたのは設計どおり
範囲検査のみであり、他に漏れ口は無かった。

### 1-2. 突き合わせで見つかった、表に載っていなかった依存

Step 0 の洗い出しで、設計報告の表に無い依存が3つ見つかった。

1. **`os_tag_is_heap_ref` が除外リストだった**（`runtime.h`）。
   `tag != FIXNUM && tag != CHAR && tag != RAW_POINTER` という書き方のため、
   タグを16値に広げると**未割当の10値すべてが「GCが追いかける」側に落ちる**。
   列挙へ書き換えた（3章）。

2. **「アドレスを持つ」と「GCが追いかける」が同一視されていた**。
   3bit時代は `align_tag_is_enforced`（16byte境界の強制対象）が
   `!= FIXNUM && != CHAR` という別の除外リストで書かれており、たまたま
   正しい集合になっていた。4bitでは `TAG_RAW_POINTER`(0x9) と `TAG_FORWARD`(0xF) が
   「アドレスを持つがGCは追いかけない」に分かれるため、2つの述語が要る。
   `os_tag_holds_address`（bit0）を新設した。

3. **塗り潰しパターンの説明コメントが実態と食い違っていた**（`runtime.c:2603`）。
   「塗る値のタグは `TAG_FORWARD`(0x6)」と書かれていたが、実際の値は
   `0xDEADDEADDEADDEA7` で `TAG_RAW_POINTER`(0x7) だった。`TAG_FORWARD` を
   使っていたのは初期実装で、GCが転送ポインタとして追いかけてGP例外になったため
   移した経緯が `runtime.c:867` に残っている。コメントのほうが古いまま残っていた。
   本段階で `TAG_MARKER` へ移すのに合わせて記述を直した。

---

## 2. 変更した定数の一覧

| 定数 | 旧 | 新 |
|---|---|---|
| `TAG_MASK` | `0x7` | `0xF` |
| `TAG_FIXNUM` | `0x0` | `0x0` |
| `TAG_CONS` | `0x1` | `0x1` |
| `TAG_CHAR` | `0x3` | `0x2` |
| `TAG_SYMBOL` | `0x2` | `0x3` |
| `TAG_SINGLE_FLOAT` | （無し） | `0x4`（**予約のみ。実装しない**） |
| `TAG_STRING` | `0x4` | `0x5` |
| `TAG_INSTANCE` | `0x5` | `0x7` |
| `TAG_RAW_POINTER` | `0x7` | `0x9` |
| `TAG_MARKER` | （無し） | `0xE`（新設。`MAGIC_*` の下位4bit） |
| `TAG_FORWARD` | `0x6` | `0xF` |
| `TAG_IS_POINTER_BIT` | （無し） | `0x1`（新設。bit0） |
| `FIXNUM_VALUE_SHIFT` | `3` | `4` |
| `FIXNUM_MAGNITUDE_MASK` | `(1<<60)-1` | `(1<<59)-1` |
| `CHAR_VALUE_SHIFT` | `3` | `32` |
| `GC_DEBUG_TRAP_PATTERN_VALUE` | `0xDEADDEADDEADDEA7` | `BASE \| TAG_MARKER` = `0xDEADDEADDEADDEAE` |
| `GC_PAINT_TRAP_PATTERN_BASE` | （無し） | `0xDEADDEADDEADDEA0`（新設。`#ifdef` の外） |
| `MAGIC_LOW_NIBBLE` | `0xE`（直値） | `TAG_MARKER`（導出） |
| `os_make_char` の引数 | `const char` | `const UINT32` |

未割当: 即値側 `0x6` / `0x8` / `0xA` / `0xC`、アドレス側 `0xB` / `0xD`。

### 自動追随したもの（手で触っていない）

| 対象 | 追随した理由 |
|---|---|
| JIT の `JIT_IMM8_UNTAG_MASK` / `JIT_IMM8_TAG_MASK` | PR #75 で `TAG_MASK` から導出済み。`_Static_assert`（imm8 に収まること・符号拡張が `~TAG_MASK` になること）も新しい値で成立した |
| ALIGN_AUDIT のタグ添字配列・報告ループ | PR #77 で `TAG_MASK+1` から導出済み。8要素 → **16要素**に自動で伸びた |
| Lisp の `*most-positive-fixnum*` / `*most-negative-fixnum*` | PR #77 で `%%FIXNUM-MAGNITUDE-MASK` 経由にしてある |
| C 側の算術の昇格判定（`runtime.c` 5箇所） | `FIXNUM_MAGNITUDE_MASK` をシンボル参照している |
| `interrupt.c` の塗り潰しパターン比較 | PR #77 で `~TAG_MASK` から導出済み。本段階で土台の定数も `GC_PAINT_TRAP_PATTERN_BASE` から導くようにした |

**PR #75 の漏れは見つからなかった。** JIT 側でタグ幅に手で追随が必要な箇所は0件。

### PR #77 で直書きのまま残した3箇所

上限が `2^60−1` → `2^59−1` と**狭まる方向**なので、いずれも引き続き成立する（確認済み）。

| 箇所 | 値 | 判定 |
|---|---|---|
| `init_test.lisp:854` | `-2^60` | ±(2^59−1) の外 → bignum のまま |
| `init_test.lisp:881-882` | `#x1000000000000000` = `2^60` | 同上 |
| `za_test_ext13.lisp:52` | `2^30 * 2^30` = `2^60` | 積は境界に依存しない。昇格も変わらず |

---

## 3. `os_tag_is_heap_ref` の全16値

| tag | 種別 | 型 | `os_tag_holds_address`（bit0） | `os_tag_is_heap_ref`（GCが追いかける） |
|---|---|---|---|---|
| `0x0` | 即値 | fixnum | 0 | 0 |
| `0x1` | アドレス | cons | 1 | **1** |
| `0x2` | 即値 | character | 0 | 0 |
| `0x3` | アドレス | symbol | 1 | **1** |
| `0x4` | 即値 | single-float（予約） | 0 | 0 |
| `0x5` | アドレス | string | 1 | **1** |
| `0x6` | 即値 | 未割当 | 0 | 0 |
| `0x7` | アドレス | instance | 1 | **1** |
| `0x8` | 即値 | 未割当 | 0 | 0 |
| `0x9` | アドレス | raw pointer | 1 | 0 |
| `0xA` | 即値 | 未割当 | 0 | 0 |
| `0xB` | アドレス | 未割当 | 1 | 0 |
| `0xC` | 即値 | 未割当 | 0 | 0 |
| `0xD` | アドレス | 未割当 | 1 | 0 |
| `0xE` | 即値 | marker（予約） | 0 | 0 |
| `0xF` | アドレス | forward | 1 | **1** |

### 2つの述語を分けた理由

3bit時代は「即値か」と「GCが追いかけるか」がほぼ同じ集合で、除外リスト2本が
別々に書かれていても事故にならなかった。4bitでは分かれる。

- `os_tag_holds_address`（bit0）= **16byte境界を要求してよいか**。
  `TAG_RAW_POINTER` と `TAG_FORWARD` はアドレスを持つので真
- `os_tag_is_heap_ref` = **GCが再配置するか**。上の2つは偽

`_Static_assert` で「GCが追いかけるタグに即値は1つも無い」ことを
テスト側（`test_tag_immediate_pointer_split_is_bit0`）で固定している。

### 未割当タグを偽にした理由

**即値側（`0x6` `0x8` `0xA` `0xC`）と予約（`0x4` `0xE`）**: bit0=0 なのでアドレスを
持たない。真にするとGCが即値をポインタとして追いかけ、生のビットパターンを
ヒープオブジェクトとして複製して word0 へ転送先を書き込む（静かなヒープ破壊）。

**アドレス側（`0xB` `0xD`）**: ここが判断の要る枠だった。
設計上ここに入る予定の double-float / ratio は**どちらもヒープ上に置かれる**ので、
実際に型を割り当てる段階では**真へ移す**ことになる。それでも現時点で偽にしたのは、
**値が1つも存在しないから**である。存在しない型のために真にしておくと、
何らかの理由でこのタグのビットパターンが現れたとき（生データの誤読、
未初期化メモリ、バグ）にGCがそれを追いかける。追いかけないほうが、
先回りして真にしておくより安全側に倒れる。

第3段階以降で `0xB` / `0xD` に型を割り当てるときは、**同じコミットで
`os_tag_is_heap_ref` を真へ移すこと**。テスト（`test_tag_table_covers_all_sixteen_values`）
が期待値テーブルを持っているので、片方だけ変えると落ちる。

---

## 4. 落ちたテストの一覧と対処

一括変更の直後、ビルドは `os_make_char` のシグネチャ変更に伴う1件
（`'c' undeclared`）だけで通り、**`_Static_assert` は1本も落ちなかった**。
つまり定数の変更漏れは無かった。

### 4-1. `make test`（C ユニットテスト）: 42件

| 分類 | 件数 | 原因 | 対処 |
|---|---|---|---|
| `>> 3` の直書き | 40 | テストが fixnum / character を**リテラルのシフト量**でデコードしていた | `FIXNUM_VALUE_SHIFT`（76箇所）/ `CHAR_VALUE_SHIFT`（6箇所）へ。8ファイル |
| リテラルスロットの重複検出 | 1 | 下記 | テスト用ダミーを16byte境界へ |
| 転送タグ衝突テストの前提条件 | 1 | 下記 | 長さを `TAG_FORWARD` から導出 |

**期待値を書き換えたものは1件も無い。** すべて「テスト側が持っていた旧定数を
新定数への参照に変えた」だけで、比較する値そのものは変えていない。

#### リテラルスロットの重複検出（`runtime_test.c:1629`）

```
assert(g_literal_slot_reclaim_seen[0] != g_literal_slot_reclaim_seen[1], ...)  ... NG
```

`os_environment_register_literal_slot` はスロットのアドレスを
`addr | TAG_RAW_POINTER` で保存する。テストは**8byte間隔のスタック変数**2つを
登録していたため、`& ~0xF` で下位4bitが落ちて**同一アドレスに潰れた**。

これは実装の誤りではなく、**4bitタグが本来の不変条件を検出した**ものである。
本番のリテラルスロットは `za_slot_t`（16byteストライド、PR #74）なので条件を
満たしている。テスト側のダミーも16byte境界にそろえた。
3bitタグのころは8byte間隔でもたまたま区別できていた。

#### 転送タグ衝突テスト（`runtime_test.c:1681`）

`TAG_FORWARD` が `0x6` → `0xF` に動いたので、長さ6の文字列はもう衝突しない。
長さを直書きの6から `TAG_FORWARD` 由来に変え、**衝突する長さを自動で作る**形にした。
直書きのままだと、テストが「衝突しない長さ」を試し続けて黙って無意味になる。

### 4-2. `make test-qemu`（実機ブート）: 5件

すべて `%%ZA-SCAN-SYNTH`（JIT焼き込み検出器の陽性対照）で、旧3bitのタグ番号
`0`〜`7` を直書きしていたもの。落ち方は新しい割り当てと完全に一致していた。

| 入力 | 旧の意味 | 新の意味 | 結果 |
|---|---|---|---|
| `2` | SYMBOL（真） | CHAR（偽） | `=> 0 (expected 1)` |
| `4` | STRING（真） | SINGLE_FLOAT（偽） | `=> 0 (expected 1)` |
| `6` | FORWARD（真） | 未割当（偽） | `=> 0 (expected 1)` |
| `3` | CHAR（偽） | SYMBOL（真） | `=> 1 (expected 0)` |
| `7` | RAW_POINTER（偽） | INSTANCE（真） | `=> 1 (expected 0)` |

番号を振り直すだけでなく、**全16値**を並べる形にした。使用中の8値だけを試すと、
未割当タグを検出器がどう扱うかがテストから見えない。

### 4-3. ハング・ブート不能

**いずれも発生しなかった。** `SMALL_HEAP_SIZE` の窓（PR #73 で3KB幅と判明）にも
当たらず、掃引も調整も不要だった（7章）。

---

## 5. 新体系のテスト

`test/c/runtime_test.c` に6本、`test/lisp/za_code_imm_test.lisp` に全16値の
陽性対照を追加した。

| テスト | 内容 |
|---|---|
| `test_tag_table_covers_all_sixteen_values` | 全16値について `os_tag_is_heap_ref` / `os_tag_holds_address` が期待値テーブルと一致すること。名前付き定数が表のどの位置に座っているかも固定 |
| `test_tag_immediate_pointer_split_is_bit0` | 即値とアドレスの区別が全16値で bit0 だけで決まること。**GCが追いかけるタグに即値が1つも無い**こと |
| `test_unassigned_and_reserved_tags_are_not_followed_by_gc` | 未割当6値・予約2値・raw pointer が偽であること |
| `test_immediate_encode_decode_round_trip` | fixnum（0・1・42・上限−1・上限、負数、−0の正規化）と character（0x00 / 0x41 / 0x7F / 0x80 / 0xFF / 0x3042 / 0xFFFF / 0x10FFFF / 0xFFFFFFFF）のエンコード→デコード往復 |
| `test_char_high_code_points_do_not_corrupt_upper_bits` | 0x80 以上で上位ビットが1で埋まらないこと。タグ領域とコードポイント領域の**外側**（bit4-31）が0のままであること |
| `test_immediates_survive_gc_unchanged` | 即値がGCを跨いで変わらないこと。**未割当タグ・予約タグの値をGCルートに置いて**GCを回し、追いかけられていないことを確認 |
| `za_code_imm_test.lisp` | `%%ZA-SCAN-SYNTH` を全16値へ。JIT側の検出器と `os_tag_is_heap_ref` が全域で一致すること |

結果は6章にまとめる。

### character の符号拡張

`os_make_char` の引数を `const char` → `UINT32` にし、呼び出し側4箇所を
`UINT8` 経由にした。

| 場所 | 旧 | 新 |
|---|---|---|
| `stream_lisp.c:204` / `409` | `os_make_char(ch)`（`char ch`） | `os_make_char((UINT8)ch)` |
| `reader.c:652` | `os_make_char(buf[0])`（`char buf[]`） | `os_make_char((UINT8)buf[0])` |
| `runtime.c:7471` / `7545` | `os_make_char((char)bytes[idx])` | `os_make_char(bytes[idx])`（`bytes` は `UINT8*`） |

`(UINT8)` マスクでデコードしていた箇所（`print.c` / `format.c` /
`runtime.c` の5箇所）は、シフト量が32になっても**低位8bitを取る意味は変わらない**
のでそのまま動く。マスクを外すと 0xFF 超のコードポイントが通るようになるが、
文字列は依然としてバイト列なので、外すのは文字列の文字単位化（第3段階以降）と
同時でなければ意味がない。

なお `CHAR_VALUE_SHIFT` が 3 → 32 と大きく離れたおかげで、
**character と fixnum のシフトを取り違えると必ずテストが落ちる**ようになった。
実際、機械置換で1箇所取り違えた（`subprimitive_test.c:257`）のがこれで露見した。

---

## 6. 検証結果

### 6-1. ユニットテスト・実機ブート

| 条件 | 分岐元 `main`(b376b84) | HEAD |
|---|---|---|
| `make test` | 8176 OK / 0 NG | **8218 OK / 0 NG** |
| `make test-qemu` 256M | 3275 passed, 0 failed | **3283 passed, 0 failed** |
| `make test-qemu` 96M | 3275 passed, 0 failed | **3283 passed, 0 failed** |

増分（C: +42、Lisp: +8）はすべて5章で追加したテストそのもの。

### 6-2. ALIGN_AUDIT

| | 256M | 96M |
|---|---|---|
| `violations` | **0** | **0** |
| VIOLATION 行 | **0** | **0** |
| `heap-align` | 16 | 16 |
| gc-count | 14（main と同じ） | 73（main と同じ） |
| ext hash table overflow | 0 / 8192 | 0 / 8192 |
| ext-region 2 misaligned | 0 / 39,049 | 0 / 197,860 |
| ext-region 4 misaligned | 0 / 259,634 | 0 / 913,098 |

**タグ別ヒストグラムが新しいタグ値の位置へ移っている。** 16byte境界なら
「下位4bit == タグ値」になるので、バケットの位置そのものが割り当ての証拠になる。
3bit時代は8バケットだったが、`TAG_MASK+1` から導出しているので16バケットへ
自動で伸びた（PR #77）。

| タグ | 旧バケット | **新バケット** | 観測数(256M) | misaligned |
|---|---|---|---|---|
| CONS | 1 | **1** | 731,567 | 0 |
| SYMBOL | 2 | **3** | 258,364 | 0 |
| STRING | 4 | **5** | 35,779 | 0 |
| INSTANCE | 5 | **7** | 65,354 | 0 |
| RAWPTR | 7 | **9** | 59,614 | 0 |

いずれも**自分のタグ値のバケットにだけ**入っており、他のバケットは全部0。
`first-bad` も全タグで0（= 一度も境界外を観測していない）。

### 6-3. JIT が出力するバイト列

`JIT_DUMP=1` で `main`(b376b84) と HEAD の両方を 256M で起動し、
コンパイルされた関数ごとの `size` / `maskedbytes` / `hash` を突き合わせた。

| | base | head |
|---|---|---|
| コンパイルされた関数 | 1038 | 1038 |
| **`size`（関数ごと）** | **全1038関数で完全一致** | |
| 出力バイト数の合計 | 1,655,605 | 1,655,605 |
| `hash` が異なる関数 | 814 / 1038 | |

**生成コードの長さは1バイトも変わっていない。** 変わったのはタグ関連の
即値スロットの中身だけである。

#### 変化する即値の全数

`za.c` で `jit_*` に渡している定数のうち、本段階で値が変わるのは**12箇所**。

| 定数 | 旧 → 新 | 箇所数 | za.c の行 |
|---|---|---|---|
| `JIT_IMM8_UNTAG_MASK` | `0xF8` → `0xF0` | 9 | 542, 1669, 1674, 3949, 3951, 4047, 4049, 5676, 5678 |
| `JIT_IMM8_TAG_MASK` | `0x07` → `0x0F` | 1 | 4826 |
| `TAG_MASK \| FIXNUM_SIGN_BIT` | `…0007` → `…000F` | 1 | 2436 |
| `TAG_INSTANCE` | `0x5` → `0x7` | 1 | 4827 |

`MAGIC_*` を渡している9箇所は本段階では値が変わらない（PR #77 で振り直し済み）。
`za.c` にはほかに約115箇所のタグ参照があるが、**すべてJITコンパイラ自身の
ホスト側の型判定**であって、生成コードには現れない。

長さが変わらない理由: `jit_movabs_reg` は REX + オペコード + `imm64` の常に10バイト。
`jit_and_reg_imm8` の `and r/m64, imm8` は、PR #75 の `_Static_assert`
（`TAG_MASK <= 0x7F` であること、`JIT_IMM8_UNTAG_MASK` が `~TAG_MASK` へ
符号拡張すること）が新しい値でも成立するため imm8 のまま。

#### `maskedbytes` の差（37関数、計 +448 バイト）

`maskedbytes` は JIT_DUMP 自身の**アドレス伏せ字ヒューリスティック**
（`jit_dump_value_is_address()` が `[0x10000, 2^48)` の即値をアドレスとみなす）が
伏せたバイト数である。生成コードの変化ではない。

`FIXNUM_VALUE_SHIFT` が 3 → 4 になったことで fixnum リテラルの符号化値が倍になり、
リテラル値 `[4096, 8192)` が新たにこの閾値を跨ぐようになった（旧: 8192以上が対象、
新: 4096以上が対象）。差はすべて8の倍数（= 伏せた即値が1〜2個増えた）で、
`size` には一切影響していない。

#### ホスト側（`za.o`）の差について

参考までに `za.o` の逆アセンブルも比較したが、**こちらは「タグ即値だけ」には
ならない**（737行の差、即値を伏せても約301行残る）。理由は生成コードの変化では
なく、(1) GCC が新しい定数の大きさに合わせてレジスタ割り当てと符号化を変える、
(2) `os_tag_is_heap_ref` が3つの比較から16分岐の `switch` になり、
各インライン先で展開が変わる、の2つである。
**JITが書き出すバイト列を測るなら JIT_DUMP のほうが直接の指標**なので、
本段階の根拠はそちらを採る（PR #75/#77 は `MAGIC_*` のように
ホスト側の変化が小さい改修だったため `za.o` の比較で足りていた）。

### 6-4. 塗り潰し監査（22試験）

`tools/bench/run_paint_audit.sh`（`GC_DEBUG=1 GC_PAINT=1 ALIGN_AUDIT=1`）で22試験。

| | 結果 |
|---|---|
| OK | **21件** |
| FAIL | **1件**（`aot_leaf_gc_test` — PR #74 時点からの既存 FAIL） |
| trap回数 | **全22試験で0** |
| trap箇所 | **全22試験で0** |

内訳・件数とも PR #74 時点のベースラインと一致しており、タグ幅の変更で
新たに壊れたものは無い。

#### 素で流した監査は「監査できていない」

上の22試験は `AUDIT_STRESS` 無しで、**GCを跨いだアサーションが0件**だった
（1938件すべてがGC 0回）。塗り潰し監査はstale領域の参照を捕まえる仕掛けなので、
GCが走らない実行では**事実上何も検査していない**
（documents/pitfalls.md / 既知の運用制約）。「21件OK」はこの意味での結果であって、
これだけを根拠にはできない。

本段階はGCの中心（`gc_copy_value` の転送判定、塗り潰しパターンのタグ、
GCのホットパスにある `os_tag_is_heap_ref`）を触っているので、
`AUDIT_STRESS=1000`（実用上限）で回し直した。

| | 素 | **`AUDIT_STRESS=1000`** |
|---|---|---|
| OK | 21件 | **22件** |
| FAIL | 1件（`aot_leaf_gc_test`） | **0件** |
| GCを跨いだアサーション | 0 / 1938（**0.0%**） | **479 / 1938（24.7%）** |
| GC発火回数の合計 | 0 | 552 |
| **trap回数** | 全試験0 | **全試験0** |
| **trap箇所** | 全試験0 | **全試験0** |

**479件がGCを跨いだうえで、トラップは1件も発火していない。**
生きた構造のフィールドが塗り潰し済み領域を指す、という形の保護漏れは
検出されなかった。

なお `aot_leaf_gc_test` は素の実行では FAIL、stress 実行では OK だった。
この試験はもともと PR #74 の時点から素の実行で FAIL していた既存事例で、
本段階で状態が変わったわけではない。

### 6-5. `_Static_assert` の負のテスト

「分割不可のセットのうち1つだけを取り残す」が実際に検出されることを確認した。

| 変異 | 出力 |
|---|---|
| `FIXNUM_MAGNITUDE_MASK` だけ旧値 `(1<<60)-1` | `"fixnum magnitude field overlaps FIXNUM_SIGN_BIT"` |
| `FIXNUM_VALUE_SHIFT` だけ旧値 `3` | `"FIXNUM_VALUE_SHIFT is too small: ..."` / `"fixnum magnitude field overlaps the tag"` / **`"sign bit + fixnum magnitude + tag do not cover all 64 bits"`** |
| `TAG_MARKER` を `TAG_FORWARD` と同値に | `"MAGIC low nibble (TAG_MARKER) collides with TAG_FORWARD"` / `"TAG_* values are not all distinct"` / `"TAG_MARKER must be an immediate (bit0 = 0)"` / MAGIC 14本 |
| `TAG_CHAR` を奇数(bit0=1)に | `"TAG_CHAR must be an immediate (bit0 = 0)"` |

2番目が本段階の要である。PR #77 で入れた「符号bit＋マグニチュード＋タグが
隙間も重なりもなく64bitを覆う」assert が、**まさに想定していた
「4つの定数のうち1つを忘れる」事故を捕まえている**。

---

## 7. `SMALL_HEAP_SIZE`

**調整していない。** `79 * 1024`（PR #73 で掃引して決めた値）のまま
`make test` が 0 NG で通った。

fixnum のシフト量が 3 → 4 に変わってもブート直後の生存量は動かない。
fixnum は即値でヒープを消費しないためである。character も同様。
ヒープ上のオブジェクトのサイズ・アラインはタグ幅と独立（16byte境界は
PR #73/#74 で確定済み）なので、この段階で生存量が動く経路が無い。

8章の `gc-live peak` が実測でこれを裏付けている。

---

## 8. `gc-live peak` の変化

| 条件 | 分岐元 `main`(b376b84) | HEAD | 差 |
|---|---|---|---|
| 256M `gc-live peak` | 5,623,504 | **5,623,504** | **±0** |
| 96M `gc-live peak` | 5,623,504 | **5,623,504** | **±0** |
| 256M `gc-live total` | 73,211,392 | 73,211,392 | ±0 |
| 96M `gc-live total` | 240,418,864 | 240,411,472 | −7,392（−0.003%） |
| 256M gc-count | 14 | 14 | ±0 |
| 96M gc-count | 73 | 73 | ±0 |

**ピーク生存量は1バイトも動いていない。** 7章の推論どおりで、fixnum と character は
即値なのでシフト量を変えてもヒープを消費せず、ヒープ上のオブジェクトの
サイズ・アラインはタグ幅と独立している（16byte境界は PR #73/#74 で確定済み）。

96M の `total`（GCのたびの生存量の累計）がわずかに減っているのは、GC回数が
同じ73回のまま、テストを足したことで各回の生存量の内訳が少し動いたため。
ピークと回数が変わっていないので、ヒープの使われ方は変わっていない。

---

## 9. 第3段階以降に送る項目

### 9-1. `%%CODE-CHAR` の範囲検査（指示書 §4 で「入れない」と確定）

現状の挙動を実測で記録する。

| 入力 | 結果 |
|---|---|
| `0x41` / `0x10FFFF` | 往復する |
| `0x110000` 〜 `0xFFFFFFFF` | **往復する**（Unicode 上限超でもエラーにならない） |
| `0x100000000`（2^32）以上 | **黙って 2^32 で切られる**（`0x100000041` → `A`） |

範囲検査は旧体系にも無かったので「検査が無い」こと自体は変わらない。
ただし**切られ方は変わった**: 旧体系は `CHAR_VALUE_SHIFT = 3` だったため
あふれ始めるのは 2^61 で、新体系は 2^32 である。つまり
**コードポイント `[2^32, 2^61)` は、以前は往復していたが今は切られる**。
文字としては意味の無い値域だが、「一切挙動が変わらない」とは言えないので記録する。

検査を入れるなら `0x10FFFF` 超をエラーにする形になるが、これは挙動の変更なので
別途判断する。

### 9-2. `0xB` / `0xD` に型を割り当てるときの手順

double-float / ratio を入れるときは、**同じコミットで `os_tag_is_heap_ref` を
真へ移す**こと（3章）。`test_tag_table_covers_all_sixteen_values` の期待値
テーブルが両方を見ているので、片方だけ変えると落ちる。

### 9-3. `TAG_SINGLE_FLOAT`（`0x4`）の実装

枠を予約しただけで、演算・リーダ・プリンタは一切無い。`<float>` は
binary64 のままヒープ（`MAGIC_FLOAT`）に置いている。
実装する場合、`0x4` は即値（bit0=0）なので `os_tag_is_heap_ref` は偽のままでよい。

### 9-4. 文字列の文字単位化

`elt` / `length` の意味が変わる大きな判断（設計報告 4.4）。本段階に含めていない。
`CHAR_VALUE_SHIFT` が32になり character 側は32bitコードポイントを持てるように
なったが、**文字列は依然としてバイト列**である。

### 9-5. STRING のヘッダの型付け

word0 が生の長さである限り、長さの下位4bitが `TAG_FORWARD` と一致する文字列は
必ず存在し、**範囲検査は本質的に必要**（設計報告 5.4）。
本段階でもそのままにしてある。

### 9-6. `os_make_char` に符号付き `char` を渡す経路の監視

引数を `UINT32` にしたので、呼び出し側が `char` をそのまま渡すと
**暗黙変換で符号拡張する**（コンパイラは警告しない）。今回4箇所を
`UINT8` 経由に直したが、新しい呼び出しを足すときは同じ注意が要る。
`-Wconversion` は既存コードに大量の警告を出すため導入していない。
