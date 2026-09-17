# オブジェクト配置の16byte境界調査(タグ拡張の前提確認)

調査日: 2026-09-17 / ブランチ: `feature/alignment-audit` / 起点コミット: `5e4f0d3`

本書は「タグを下位4bitへ拡張できるか」の判定材料をそろえるための調査記録である。
**実装は行っていない**(既存タグ体系は一切変更していない)。追加したのは
`ALIGN_AUDIT=1` でのみ有効になる計測コードだけで、既定ビルドの挙動・性能は
変わらない(→ 第6章「計測用コードの影響」)。

---

## 1. 結論

### 問1: すべての確保経路で、返されるアドレスは16byte境界か

**いいえ。** 16byte境界に揃っていない経路が実在し、実測でも常時発生している。

| | 16byte境界か |
|---|---|
| `os_alloc_bytes`(Lispヒープのバンプ確保) | **揃わない**。切り上げが8byte単位(`runtime.c:287`)で、可変長オブジェクトが8の奇数倍を要求する |
| `gc_to_alloc`(Cheneyのコピー先) | **揃わない**。同じく8byte切り上げ(`runtime.c:1523`) |
| `os_boot_alloc`(ヒープ初期化前) | **揃わない**。呼び出し側が`align=8`を渡し、サイズも16の倍数でない(`block_device.c:79,84`、`sizeof(block_device_t)==104`) |
| `g_nil_cell`(静的領域のnil) | **保証が無い**。`__attribute__((aligned(8)))`(`runtime.c:266`)。既定ビルドのBOOTX64.EFIでは実際に `...808`(8 mod 16)に置かれている。`GC_DEBUG=1` ビルドやネイティブのユニットテストでは16境界に落ちており、**ビルドによって変わる** |
| `&g_za_*_slots[i]`(JITリテラルスロット) | **揃わない**。8byte刻みの`lisp_val_t`配列の要素アドレスで、奇数添字は必ず8 mod 16 |
| `os_imm_slot_alloc` | 揃う。16byte切り上げ(`runtime.c:1413`) |
| `os_imm_page_alloc` | 揃う。4096byte単位 |

### 問2: 設計として保証されているのか、結果として揃っているだけなのか

**どちらでもない。「保証されておらず、実際に揃っていない」。**

16byte境界を保証しているのは`os_imm_slot_alloc`(`(size + 15) & ~15ULL`)と
ページ確保だけで、これはFunction Cell/`za_fn_meta_t`のために後から入った規律である。
Lispヒープ側は設計時から一貫して**8byte単位**であり、`os_alloc_bytes`の
doc comment も「nバイト(8byte境界に整列)」と明記している(`runtime.c:269`)。
「16byteに揃えよう」という意図はコード上のどこにも無い。

### 問3: 下位4bitのうち、現時点で既に何かに使われているビットはないか

**bit0〜2 は8値すべて使用中で空きは無い。bit3 も FIXNUM/CHAR では既に使用中。**

- bit0〜2: `TAG_MASK`(`runtime.h:8`)。0x0〜0x7の8エンコードがすべて割り当て済み
  (FIXNUM/CONS/SYMBOL/CHAR/STRING/INSTANCE/FORWARD/RAW_POINTER)。
  `runtime.c:551-559` のコメントも「**「タグとして不正な値」は作れない** —
  0x0〜0x7の8値はすべて使用中である」と明言している。
- bit3: ポインタ系タグではアドレスの一部だが、**FIXNUM/CHARでは値の一部**である。
  `os_make_fixnum`は`fixnum << 3`(`runtime.h:542`)、`os_make_char`は`code << 3 | TAG_CHAR`
  (`runtime.c:2834`)で、マグニチュードの最下位bitがbit3に入る。したがって
  「下位4bitをタグにする」は即値表現の変更(FIXNUMを59bitに縮める等)とセットになる。

---

## 2. 現行タグ体系(Step 0)

### 2.1 ビット位置 × 用途

| bit | 用途 | 定義箇所 | 備考 |
|---|---|---|---|
| 0〜2 | タグ(8種すべて使用中) | `runtime.h:8-24` | `TAG_MASK 0x7` |
| 3〜62 | FIXNUM のマグニチュード(60bit) | `runtime.h:47` `FIXNUM_MAGNITUDE_MASK` | `os_fixnum_magnitude` = `(val >> 3) & mask` (`runtime.c:2627`) |
| 3〜 | CHAR の文字コード | `runtime.c:2834` | `code << 3 \| TAG_CHAR` |
| 3〜63 | ポインタ系タグではアドレス本体 | — | bit3 は現在アドレスビットとして**使われている**(0/1 両方の値を取る) |
| 63 | FIXNUM の符号 | `runtime.h:45` `FIXNUM_SIGN_BIT` | `os_make_fixnum_signed` (`runtime.c:2613`) |

### 2.2 タグ値の割り当て

| 値 | 名前 | 種別 | 定義 |
|---|---|---|---|
| 0x0 | `TAG_FIXNUM` | 即値 | `runtime.h:10` |
| 0x1 | `TAG_CONS` | アドレス | `runtime.h:12` |
| 0x2 | `TAG_SYMBOL` | アドレス | `runtime.h:14` |
| 0x3 | `TAG_CHAR` | 即値 | `runtime.h:16` |
| 0x4 | `TAG_STRING` | アドレス | `runtime.h:18` |
| 0x5 | `TAG_INSTANCE` | アドレス | `runtime.h:20` |
| 0x6 | `TAG_FORWARD` | アドレス(GC内部、word0のみ) | `runtime.h:22` |
| 0x7 | `TAG_RAW_POINTER` | アドレス(GC管理外、即値扱い) | `runtime.h:24` |

判定の単一の真実源は `os_tag_is_heap_ref`(`runtime.h:35-42`)。

### 2.3 タグ操作の使用箇所

**C側(すべて`TAG_MASK`を経由。値を直書きしている箇所は無い)**

| ファイル | 件数 | 主な用途 |
|---|---|---|
| `src/c/runtime.c` | 141 | タグ付け/外し、GC、型判定 |
| `src/c/za.c` | 136 | JITコンパイル時の形の判定 |
| `src/c/eval.c` | 23 | 評価器の型判定 |
| `src/c/print.c` / `reader.c` / `stream_lisp.c` / `mount.c` / `ide_subprimitive.c` / `interrupt.c` / `lisp.c` / `process.c` / `format.c` / `stream.c` | 各1〜9 | 型判定・タグ外し |
| `src/c/lisp_compiled.c`(AOT生成) | **0** | 生成コードはタグを直接触らない(すべて`os_*`/`cc_*`/`primitive_*`経由) |

`grep -c "TAG_MASK\|TAG_CONS\|TAG_FIXNUM\|& 0x7\|>> 3\|<< 3" src/c/lisp_compiled.c` は 0。
AOTトランスパイラ(`src/lisp/transpile.lisp`)側にもタグ値のリテラルは無い
(`TAG_`の出現は2箇所のコメントのみ)。

**JIT が出力する機械語**

za.c は AT&T構文の文字列ではなく、`jit_*` ヘルパでバイト列を直接組む。タグに
関係する即値は次の通り。**`0xF8`(= `~TAG_MASK` を8bit即値にしたもの)が9箇所で
直書きされている**ため、`TAG_MASK`を0xFへ変えてもここは自動追随しない。

| 箇所 | 命令 | 即値 | 用途 |
|---|---|---|---|
| `za.c:501` | `and r/m64, imm8` | `0xF8` | `za_emit_untag_instance` |
| `za.c:1574` / `1579` | 同上 | `0xF8` | Function Cell / fn_obj のタグ外し |
| `za.c:3854` / `3856` | 同上 | `0xF8` | 同上(呼び出し高速化経路) |
| `za.c:3952` / `3954` | 同上 | `0xF8` | 同上 |
| `za.c:5581` / `5583` | 同上 | `0xF8` | `za_emit_load_callee_env_rcx` |
| `za.c:4731` | `and rax, imm8` | `(UINT8)TAG_MASK` | catch/throw判定(マクロ経由なので追随する) |
| `za.c:2341` | `movabs r9, imm64` | `TAG_MASK \| FIXNUM_SIGN_BIT` | fixnum高速路の一括判定(追随する) |

`.align` / `.p2align` / `.balign` 指令は C ソース中に存在しない(JITは自前で
バイト列を並べるだけで、アセンブラのアライン指令を使わない)。

**生成コードに焼き込まれるLisp値**

`primitive_za_scan_synth`(`za.c:20-46`)と `test/lisp/za_code_imm_test.lisp` が、
「GCで動く領域を指す即値」がJITコードに焼き込まれていないことを常時検査している。
したがってJITコードに現れるアドレス即値は、Immobilized Space上の固定アドレス
(Function Cell、リテラルスロット)とCの関数アドレスに限られる。

---

## 3. 確保経路ごとの判定(Step 1)

判定は「保証あり / 結果的に揃う / 揃わない / 該当なし」。

| # | 経路 | 判定 | 根拠 |
|---|---|---|---|
| 1 | バンプアロケータ(通常確保) | **揃わない** | `os_alloc_bytes`: `aligned = (n + 7) & ~7ULL`(`runtime.c:287`)。切り上げ単位は8。要求が8の奇数倍なら次の確保が8 mod 16になる |
| 2 | ヒープ領域の開始アドレス | **揃わない(保証が無い)** | `os_heap_init`: `half = (heap_size / 2) & ~7ULL`(`runtime.c:1048`)で To空間先頭が8byte単位。base自体も `os_boot_alloc_finalize`: `(bump + 7) & ~7ULL`(`runtime.c:1180`)で8byte単位。元のUEFI領域は4KB境界(`main.c:337`)だが、boot allocatorが16の倍数でない量を切り出した後の残りが渡されるため16境界は保たれない |
| 3 | Cheneyコピー時のTo空間確保 | **揃わない** | `gc_to_alloc`: `aligned = (size + 7) & ~7ULL`(`runtime.c:1523`)。サイズ再計算は通常確保と一致(CONS=16 / SYMBOL=32 / INSTANCE=32 / STRING=8+len、`runtime.c:1671-1675`)だが、可変長の生ブロック(bignumのlimb配列 `8*count`、vector本体、`os_stream_t`の文字列バッファ `str_cap`(`runtime.c:1858`))も同じTo空間に交互に置かれるため、そこで境界がずれる |
| 4 | 可変長オブジェクト | **揃わない** | STRING: `os_alloc_bytes(8 + len)`(`runtime.c:1017`, `7020`, `stream_lisp.c:110`)。長さ0で8byte、長さ9〜16で24byte。VECTOR本体: `os_alloc_bytes(8 * (1 + rank + total))`(`runtime.c:6648`)。BIGNUMのlimb配列: `os_alloc_bytes(8 * count)`(`runtime.c:4041`)。いずれも8の奇数倍になりうる |
| 5 | Immobilized Space | **保証あり** | `os_imm_slot_alloc`: `aligned = (size + 15) & ~15ULL`(`runtime.c:1413`)。ページは`IMM_PAGE_SIZE`(4096)単位、領域先頭も `g_imm_space` が `__attribute__((aligned(IMM_PAGE_SIZE)))`(`runtime.c:1221`) |
| 6 | C の静的オブジェクト | **揃わない** | `static UINT8 g_nil_cell[16] __attribute__((aligned(8)))`(`runtime.c:266`)。**16ではなく8を明示している**。実機バイナリでも `nm` で `g_nil_cell` が `...808`(8 mod 16)に置かれていることを確認済み(→ 第4章) |
| 7 | AOT生成の静的定数 | **該当なし** | `src/c/lisp_compiled.c` に静的なLispオブジェクトは存在しない。`static` の出現はすべて `static int __quote_sym_idx = -1;`(`os_make_symbol_cached`用の添字キャッシュ)であり、値は実行時に`os_make_*`で作られる |
| 8 | JITのリテラルスロット・コードページ内オブジェクト | **揃わない** | コードページは4096境界(`os_imm_pages_alloc_contiguous`)で問題ないが、**リテラルスロットは静的 `lisp_val_t` 配列の要素アドレス**である: `&g_za_quote_slots[idx]`(`za.c:590`)、`&g_za_number_slots[idx]`(`za.c:889`)、`&g_za_lambda_slots[idx]`(`za.c:2828`, `5440`)、`&g_za_fn_cell_cache_slots[idx]`(`za.c:1106`)。これらは `os_environment_register_literal_slot`(`runtime.c:3683-3691`)で `TAG_RAW_POINTER` 付きの**Lisp値として流通する**。配列は8byte刻みなので奇数添字は必ず8 mod 16。`nm` 上 `g_za_quote_slots` は `...260`(16境界)なので、奇数添字が外れる |
| 9 | C スタック上の一時オブジェクト | **該当なし(ただし本番経路に限る)** | `\| TAG_` によるタグ付け箇所を全数列挙したところ、Lisp値の生成元はヒープ/Immobilized Space/`g_nil_cell` のみで、スタックアドレスをタグ付けする本番コードは無い。例外は `test/c/runtime_test.c:1616-1617` で、テストが `os_environment_register_literal_slot(env, &fake_slot_a)` にCローカル変数のアドレスを渡している(API自体は任意のポインタを受け付けるため、将来同種の使い方が入りうる) |
| 10 | FS層(mount.c / ide_subprimitive.c / subprimitive.c) | **揃わない** | `ide_subprimitive.c:37` が `block_device_t*` をそのまま `\| TAG_RAW_POINTER` して返す。その実体は `os_boot_alloc(sizeof(block_device_t), 8)`(`block_device.c:84`)で確保され、`sizeof(block_device_t) == 104`(16の倍数でない)。`mount.c:390,397` がこのハンドルを `& ~TAG_MASK` で取り出す。`mount.c:154` の `os_alloc_raw(len)`、`subprimitive.c` の`TAG_CHAR`生成は境界に関係しない |
| 11 | 起動初期化時の確保 | **揃わない** | `os_boot_alloc(size, align)` は呼び出し側指定のalign(`runtime.c:1165`)。実呼び出しはいずれも `align=8`(`block_device.c:79,84`)。`os_boot_alloc_finalize` も8byte切り上げ(`runtime.c:1180`) |

### 3.1 アロケータ以外の要因

| 観点 | 判定 | 根拠 |
|---|---|---|
| ポインタが指す位置(ヘッダかペイロードか) | **問題なし** | すべてのタグ付き値がオブジェクト先頭を指す。STRING も `addr`(word0=長さ)を指し、ペイロードは `addr+8` を都度計算する(`runtime.c:1017-1024`, `os_string_to_cstr`) |
| 内部ポインタ | **あり** | 上記 #8 のリテラルスロット。`g_za_*_slots` 配列**の途中**を指すアドレスが `TAG_RAW_POINTER` 付きのLisp値として環境に登録される(`runtime.c:3691`) |
| 関数ポインタ・コードアドレス | **問題なし** | `za_fn_meta_t.cons_entry` / `fixed_entry` は**タグを付けずに**生の値として `MAGIC_FUNCTION_NATIVE` の word1 が指す meta 構造体に格納される(`runtime.c:1434-1440`)。Lisp値としてタグ付けされることはない |
| forwarding pointer | **揃わない** | `words[0] = dst \| TAG_FORWARD`(`runtime.c:1780`)。`dst` は `gc_to_alloc` 由来なので8byte境界しか保証されない。下位4bitをタグにするならここが直接壊れる |
| GCマーク・作業用フラグ | **無し** | 低位ビットを借用する仕組みは `TAG_FORWARD` 以外に存在しない(`os_mark_constant` は alist への登録であってビット操作ではない) |
| 塗り潰しトラップパターン | 参考 | `GC_DEBUG_TRAP_PATTERN_VALUE 0xDEADDEADDEADDEA7`(`runtime.h:1034`)の下位4bitは `0x7`。4bitタグ化するとこの定数の意味も変わる |

---

## 4. 動的検証(Step 2)

### 4.1 計測方法

`ALIGN_AUDIT=1` でビルドしたときだけ有効になる計数を2系統入れた
(`runtime.h` の `ALIGN_AUDIT_NOTE_*` マクロと `runtime.c` の実装)。

1. **確保サイト**: 各アロケータが返したアドレスの下位4bitを、サイト別に数える。
   併せて「切り上げ後のサイズが16の倍数でなかった回数(stride-break)」も数える
   — 確保自体が16境界でも、**次の**確保をずらすのはこれである。
2. **流通している値**: `gc_copy_value` の入口で数える。この関数はGCのたびに
   **全ルート(グローバル、シンボル表、追加GCルート=JITスロットプール、
   全プロセスのshadow stack)と、全生存オブジェクトの全フィールド**を通るので、
   Lisp値として現れるポインタをタグ別に網羅できる
   (`os_gc_collect_body`, `runtime.c:2102`〜)。
   ヒープを線形走査する方法は取れない: bignumのlimb配列やvector本体といった
   可変長の生ブロックがヘッダを持たず、境界を判別できないため。

出力は、QEMU試験では `run_qemu_boot_test` の電源断直前(`kernel.c:87-96`)に
シリアルへ、ネイティブのユニットテストでは `atexit` で標準出力へ出す
(テスト本体には一切手を入れていない)。

### 4.2 実行条件と結果

#### 実行条件

すべて `feature/alignment-audit` の計測コード入りバイナリ。`ALIGN_AUDIT=1` 以外の
ビルドフラグは条件4のみ `GC_DEBUG=1` を追加。OVMF は `/usr/share/OVMF/OVMF_CODE_4M.fd`、
ホストは KVM 無し。

| # | 条件 | QEMU_MEM | 試験 | 失敗件数 | GC回数 |
|---|---|---|---|---|---|
| 1 | ネイティブのCユニットテスト `make test ALIGN_AUDIT=1`(19バイナリ) | — | 全件 | **8158 OK / 0 NG** | 45(全バイナリ合計) |
| 2 | QEMU 全試験 `qemu_boot_test.lisp` | 256M | 全件 | **3269 passed / 0 failed** | 14 |
| 3 | QEMU 全試験(GC圧を上げる) | 96M | 全件 | **3269 passed / 0 failed** | 70 |
| 4 | QEMU 強制GC(`(%%diag-gc-stress 1000)`、`GC_DEBUG=1`)+ `init_test` + `za_test` | 256M | 該当分 | **733 passed / 0 failed** | 231 |
| 5 | defun 600個 + 呼び出し(Immobilized Space を消費させる) | 256M | アサーション無し | 起動〜完走(異常終了なし) | 0 |
| 6 | flet / labels を20000回(`for`ループ) | 256M | アサーション無し | 起動〜完走(CPU例外なし) | 0 |
| 7 | 22試験を1ファイル1起動で個別実行(`run_paint_audit.sh`、`AUDIT_CONTROL=0`) | 256M | 1932アサーション | **21 OK / 1 FAIL**(下記) | 0(全22試験) |
| 8 | `QEMU_MEM=64M` | 64M | — | **起動しない**(OVMF が `BdsDxe: … Out of Resources` で EFI をロードできない) | — |

条件7の FAIL は `aot_leaf_gc_test` の1件
(`(> (- LEAF-GC-STRESS-GC-AFTER LEAF-GC-STRESS-GC-BEFORE) 1)` が NIL)。
**計測コードとは無関係で、同じ起動条件なら既定ビルドでも同じ1件が落ちる**ことを
確認済み(`ALIGN_AUDIT` 無しで `tmp/audit_boot_aot_leaf_gc_test.lisp` を単独起動 →
同じ `5 passed, 1 failed`)。この試験は「ストレスループ中にGCが2回以上走ること」を
要求するが、その試験だけを新しいヒープで起動するとGCが1回も走らないため落ちる。
条件2/3(全試験を1ブートで流す)では 0 failed である。

#### 確保サイトのヒストグラム

数字は「返却アドレスの下位4bit」。Lispヒープ系は bin0 と bin8 にしか値が立たない
(すべての確保が8の倍数なので、残差は0か8のどちらか)。

| 条件 | サイト | 件数 | 8 mod 16 の件数(割合) | stride-break |
|---|---|---|---|---|
| 1 ネイティブ | `os_alloc_bytes` | 990,029 | 881,845(**89.1%**) | 2,219 |
| | `gc_to_alloc` | 49,173 | 25,842(**52.6%**) | 2,466 |
| | `os_imm_slot_alloc` | 6,322 | 0(0%) | 0 |
| | `os_imm_page_alloc` | 60 | 0(0%) | 0 |
| | `os_boot_alloc` | 12 | 6(**50.0%**) | 12 |
| | `g_nil_cell` | 21 | 0(0%) | 0 |
| | `jit_literal_slot` | 2 | 1(50%)※テストがCローカル変数を渡す経路 | 2 |
| | `block_device_handle` | 1 | 1(**100%**) | 1 |
| 2 QEMU 256M | `os_alloc_bytes` | 60,617,742 | 4,925,146(8.1%) | 10,750 |
| | `gc_to_alloc` | 557,598 | 154,824(**27.8%**) | 18,358 |
| | `os_imm_slot_alloc` | 4,423 | 0(0%) | 0 |
| | `os_imm_pages_alloc_contiguous` | 1,039 | 0(0%) | 0 |
| | `os_imm_page_alloc` | 24 | 0(0%) | 0 |
| | `env_page_handle` | 1,119 | 0(0%) | 0 |
| | `jit_literal_slot` | 1,533 | 765(**49.9%**) | 1,533 |
| | `block_device_handle` | 2 | 2(**100%**) | 2 |
| | `os_boot_alloc` | 8 | 4(**50.0%**) | 8 |
| | `g_nil_cell` | 1 | 1(**100%**) | 0 |
| 3 QEMU 96M | `os_alloc_bytes` | 2,946,272 | 1,986,423(**67.4%**) | 10,750 |
| | `gc_to_alloc` | 1,809,028 | 598,226(**33.1%**) | 89,768 |
| | `jit_literal_slot` | 1,533 | 765(**49.9%**) | 1,533 |
| | `block_device_handle` | 2 | 2(**100%**) | 2 |
| | `g_nil_cell` | 1 | 1(**100%**) | 0 |
| 5 defun大量 | `os_alloc_bytes` | 68,252 | 48,655(**71.3%**) | 1,004 |
| | `os_imm_pages_alloc_contiguous` | 666 | 0(0%) | 0 |
| | `jit_literal_slot` | 197 | 97(**49.2%**) | 197 |
| 6 flet/labels反復 | `os_alloc_bytes` | 4,295,336 | 4,282,640(**99.7%**) | 405 |
| | `os_imm_slot_alloc` | 1,369 | 0(0%) | 0 |
| | `jit_literal_slot` | 201 | 99(**49.3%**) | 201 |
| 7 22試験(合算) | `os_alloc_bytes` | 1,802,667 | 1,235,188(**68.5%**) | 18,934 |
| | `os_imm_slot_alloc` | 31,046 | 0(0%) | 0 |
| | `os_imm_pages_alloc_contiguous` | 1,839 | 0(0%) | 0 |
| | `env_page_handle` | 2,476 | 0(0%) | 0 |
| | `jit_literal_slot` | 5,070 | 2,519(**49.7%**) | 5,070 |
| | `block_device_handle` | 44 | 44(**100%**) | 44 |
| | `os_boot_alloc` | 176 | 88(**50.0%**) | 176 |
| | `g_nil_cell` | 22 | 22(**100%**) | 0 |

`jit_literal_slot` が常に 49.2〜49.9% なのは、8byte刻みの静的配列の要素アドレスを
そのまま `TAG_RAW_POINTER` 化しているため(偶数添字が16境界、奇数添字が8 mod 16)で、
理論値50%と一致する。

`g_nil_cell` の残差が条件によって変わる(QEMUの既定ビルドでは 8 mod 16、
ネイティブのユニットテストでは16境界)のは、`aligned(8)` 指定のもとで
リンカ/コンパイラがどう置くかに依存しているためであり、揃うほうが偶然である。
実機向けバイナリでは `nm` でも確認できる:

```
0000000245b8a808 b g_nil_cell        ← 0x808 → 8 mod 16
0000000245bb9260 b g_za_quote_slots  ← 0x260 → 16境界(奇数添字が 8 mod 16)
0000000244b8a000 b g_imm_space       ← 4096境界
```

#### 流通している値のヒストグラム(タグ別・下位4bit)

`gc_copy_value` が見たすべてのポインタ値。全ルート(グローバル変数群、
`g_symbol_table`、追加GCルート=JITスロットプール、全プロセスのshadow stack)と
全生存オブジェクトの全フィールドが含まれる。
**16byte境界に揃っていれば index=タグ値の1本しか立たないはずだが、
すべてのタグで index=タグ値+8 にも立っている。**

条件2(QEMU 256M、GC 14回):

| タグ | 総数 | index=タグ値(16境界) | index=タグ値+8(8 mod 16) | 不揃いの割合 |
|---|---|---|---|---|
| CONS | 730,993 | 358,612 | 372,381 | **50.9%** |
| SYMBOL | 258,098 | 253,637 | 4,461 | 1.7% |
| STRING | 35,681 | 18,245 | 17,436 | **48.9%** |
| INSTANCE | 65,284 | 36,198 | 29,086 | **44.6%** |
| RAW_POINTER | 59,600 | 49,300 | 10,300 | **17.3%** |

条件3(QEMU 96M、GC 70回):

| タグ | 総数 | 16境界 | 8 mod 16 | 不揃いの割合 |
|---|---|---|---|---|
| CONS | 2,223,383 | 967,710 | 1,255,673 | **56.5%** |
| SYMBOL | 778,906 | 774,170 | 4,736 | 0.6% |
| STRING | 172,981 | 88,623 | 84,358 | **48.8%** |
| INSTANCE | 271,817 | 163,670 | 108,147 | **39.8%** |
| RAW_POINTER | 290,663 | 240,107 | 50,556 | **17.4%** |

条件1(ネイティブ、GC 45回):

| タグ | 総数 | 16境界 | 8 mod 16 | 不揃いの割合 |
|---|---|---|---|---|
| CONS | 52,294 | 35,000 | 17,294 | **33.1%** |
| SYMBOL | 22,738 | 21,277 | 1,461 | 6.4% |
| STRING | 9,304 | 5,521 | 3,783 | **40.7%** |
| INSTANCE | 11,101 | 3,061 | 8,040 | **72.4%** |
| RAW_POINTER | 5,475 | 5,475 | 0 | 0%(ネイティブテストはJITスロットをほぼ作らない) |

条件4(強制GC `(%%diag-gc-stress 1000)`、`GC_DEBUG=1`、GC 231回):

| タグ | 総数 | 16境界 | 8 mod 16 | 不揃いの割合 |
|---|---|---|---|---|
| CONS | 2,716,101 | 1,949,564 | 766,537 | **28.2%** |
| SYMBOL | 1,025,951 | 1,023,592 | 2,359 | 0.2% |
| STRING | 276,207 | 143,374 | 132,833 | **48.1%** |
| INSTANCE | 405,544 | 203,023 | 202,521 | **49.9%** |
| RAW_POINTER | 271,560 | 240,931 | 30,629 | **11.3%** |

同条件の確保サイト: `os_alloc_bytes` 288,303件中 126,827件(44.0%)が 8 mod 16、
stride-break 9,619。`gc_to_alloc` 2,268,779件中 987,178件(**43.5%**)が 8 mod 16、
stride-break 106,854。`jit_literal_slot` 299件中 148件(49.5%)。
`block_device_handle` 2件中 2件(100%)。
なお、このビルド(`GC_DEBUG=1`)では `g_nil_cell` が 16境界に落ちており
(条件2/3の既定ビルドでは 8 mod 16)、静的配置がビルド依存であることの実例になっている。

`TAG_FORWARD` はこのヒストグラムには現れない(`gc_copy_value` が数えるのは
**渡された値**であって、書き込む転送ヘッダではないため)。ただし転送ヘッダは
`dst | TAG_FORWARD` で作られ(`runtime.c:1780`)、`dst` は `gc_to_alloc` の返り値なので、
上表の `gc_to_alloc` の不揃い率(27.8〜52.6%)がそのまま転送ポインタの不揃い率になる。

SYMBOL だけ不揃い率が低い(0.2〜6.4%)のは、symbol が32byte固定で、
`os_make_symbol` が直前に文字列(可変長)を確保するため、残差が片側に偏りやすいこと
による。**0ではない**ので、揃っているとは言えない。


---

## 5. ビットごとの使用可否判定(Step 3)

### 5.1 判定

| bit | 現在の用途 | 判定 |
|---|---|---|
| 0 | `TAG_MASK` | **使用中**。空きは無い |
| 1 | `TAG_MASK` | **使用中**。空きは無い |
| 2 | `TAG_MASK` | **使用中**。空きは無い |
| 3 | FIXNUM/CHAR では値の最下位bit、ポインタ系タグではアドレスbit | **現状では使えない** |

bit0〜2 は8エンコードすべてが埋まっており、新しいタグを足す余地は無い
(`runtime.h:10-24`)。したがって「タグ拡張」は実質 bit3 を取れるかどうかの問題になる。

### 5.2 bit3 が使えない理由(阻害要因)

いずれも本調査で確認済みで、推測ではない。

| # | 阻害要因 | 該当する経路 | 根拠 |
|---|---|---|---|
| B1 | FIXNUM/CHAR の値表現が bit3 を使っている | 即値全般 | `os_make_fixnum` = `fixnum << 3`、`os_make_char` = `code << 3 \| TAG_CHAR`(静的) |
| B2 | Lispヒープのバンプ確保が8byte切り上げ | `os_alloc_bytes` | `runtime.c:287`。実測で全条件において bin0 と bin8 の両方が埋まる |
| B3 | 可変長オブジェクトが8の奇数倍を要求し、以降の確保をずらす | STRING / VECTOR本体 / bignum limb / `os_alloc_raw` | `runtime.c:1017,4041,6648,7020`。実測 stride-break |
| B4 | Cheney の To空間確保も8byte切り上げ | `gc_to_alloc` | `runtime.c:1523`。実測 27.8〜52.6% が 8 mod 16 |
| B5 | 転送ポインタが `dst \| TAG_FORWARD` で作られる | GC内部 | `runtime.c:1780`。`dst` は B4 の産物なので同率で 8 mod 16 |
| B6 | JITリテラルスロットが8byte刻みの静的配列の要素アドレス | `&g_za_*_slots[i]` → `TAG_RAW_POINTER` | `za.c:590,889,1106,2828,5440` + `runtime.c:3683`。実測 **49.2〜49.9% が 8 mod 16**(理論値50%と一致) |
| B7 | `block_device_t*` を `TAG_RAW_POINTER` として返す | `%%IDE-DEVICE-AT` | `ide_subprimitive.c:37`。`os_boot_alloc(…, 8)` + `sizeof(block_device_t)==104`。実測 **100% が 8 mod 16** |
| B8 | 静的な nil セルが `aligned(8)` | `g_nil_cell` | `runtime.c:266`。実測は**ビルド依存**: 既定ビルドでは 8 mod 16(`nm` で `...808`)、GC_DEBUG ビルドでは 16境界、ネイティブのユニットテストでは 16境界。つまり「たまたま揃うことがある」だけで保証が無い典型 |
| B9 | ヒープ先頭・半空間境界が8byte単位 | `os_heap_init` / `os_boot_alloc_finalize` | `runtime.c:1048,1180` |

### 5.3 bit3 を空けるために必要な変更(実装はしない)

| # | 変更 | 対象 | 対応する阻害要因 |
|---|---|---|---|
| C1 | 切り上げを `(n + 15) & ~15ULL` にする | `os_alloc_bytes`(`runtime.c:287`)、`gc_to_alloc`(`runtime.c:1523`) | B2 / B3 / B4 / B5 |
| C2 | 半空間分割とヒープ先頭を16byte単位にする | `os_heap_init` の `& ~7ULL` → `& ~15ULL`(`runtime.c:1048`)、`os_boot_alloc_finalize`(`runtime.c:1180`) | B9 |
| C3 | `g_nil_cell` を `__attribute__((aligned(16)))` にする | `runtime.c:266` | B8 |
| C4 | `os_boot_alloc` の呼び出しを `align=16` にする | `block_device.c:79,84` | B7 |
| C5 | JITリテラルスロットを16byte刻みにする(配列を `lisp_val_t [N][2]` にする、または `os_imm_slot_alloc` へ移す) | `za.c:561,634,1050,1078` の4プール | B6 |
| C6 | `os_environment_register_literal_slot` にアライン要件を課す(受け取り時に検査する) | `runtime.c:3683` | B6 の再発防止 |
| C7 | JITの `0xF8` 直書き9箇所を `TAG_MASK` から導出する | `za.c:501,1574,1579,3854,3856,3952,3954,5581,5583` | タグ幅変更そのもの |
| C8 | 即値表現を変更する(FIXNUM/CHAR を `<< 4` にしてマグニチュードを59bitへ、または「4bitタグはポインタ系のみ」という非一様方式にする) | `runtime.h:542`、`runtime.c:2613,2627,2834`、`za.c:2341` の高速路 | B1 |
| C9 | `GC_DEBUG_TRAP_PATTERN_VALUE` の下位4bitを見直す | `runtime.h:1034` | 監査基盤の整合 |

### 5.4 見込まれるコスト

**メモリ**

- C1 で増えるのは「切り上げ後サイズが16の倍数でない確保」だけである。CONS(16)/
  SYMBOL(32)/INSTANCE(32) は既に16の倍数なので**一切増えない**。増えるのは
  STRING と可変長の生ブロック(vector本体・bignum limb・`os_stream_t` とその
  バッファ)で、1件あたり +8byte。
- 実測の比率(stride-break / total):
  - `os_alloc_bytes`: 0.009%(flet/labels反復)〜3.3%(強制GC条件)。累計確保量に対しては小さい
  - `gc_to_alloc`: 3.3%(256M)/ 5.0%(96M・ネイティブ)/ 4.7%(強制GC)
- **生存サイズへの影響の見積り**: 平均オブジェクトサイズを最小の16byteと仮定した
  場合の上限見積りで、GC1回あたり +1.6〜2.5%。実際の平均サイズはこれより大きいので
  実効はこれ未満になる。**この見積りはバイト数を直接計測したものではなく、
  件数比からの換算である**(バイト数の計測は本調査では行っていない)。
- C5 で JITリテラルスロットのプールを16byte刻みにすると、BSSが
  (1024 + 512 + 256 + 2048) × 8 = **30,720 byte** 増える(`ZA_MAX_QUOTE_SLOTS` 1024 /
  `ZA_MAX_NUMBER_SLOTS` 512 / `ZA_MAX_LAMBDA_SLOTS` 256 / `ZA_MAX_FN_CELL_CACHE_SLOTS` 2048)。
- C4 は最大4デバイス分なので無視できる。

**性能**

- C1/C2 は定数の変更だけで、バンプ加算の命令数は変わらない。影響は「生存サイズが
  わずかに増える分だけGC頻度が上がりうる」という間接的なものにとどまる。
- C8 は影響が大きい。`os_make_fixnum` は生成コード中に1,513箇所あり
  (`runtime.h` の性能測定コメント)、`za.c:2341` の fixnum 高速路も
  `TAG_MASK \| FIXNUM_SIGN_BIT` の一括判定に依存している。シフト量が変わるだけなら
  命令数は同じだが、FIXNUM の表現可能範囲が 60bit → 59bit に縮むため、
  bignum へ昇格する境界が動く(`os_make_integer` の正規化)。
- C7 は生成される機械語のバイト列が変わるため、JIT関連の回帰テスト
  (`za_test*`, `za_code_imm_test`)を全面的に流し直す必要がある。

### 5.5 補足: 「bit3だけ」を狙う代替案

B1(即値が bit3 を使っている)を避けるなら、「**ポインタ系タグだけ4bit、即値は
3bitのまま**」という非一様な割り当てが考えられる。この場合 C8 は不要になるが、
判定側が「まず下位3bitを見て、ポインタ系なら4bit目も見る」という2段構えになり、
`os_tag_is_heap_ref` を単一の真実源として保っている現在の構造
(`runtime.h:26-42`)と、JITの1命令マスク(`and reg, 0xF8`)の単純さが失われる。
どちらを取るかは設計判断であり、本調査では結論を出さない。


---

## 6. 計測用コードの影響(既定ビルドへの非侵襲性)

`ALIGN_AUDIT` を渡さない場合、`runtime.h` の `ALIGN_AUDIT_NOTE_ALLOC` /
`ALIGN_AUDIT_NOTE_VALUE` は `((void)0)` に展開され、実装本体も
`#ifdef ISIKIOS_ALIGN_AUDIT` ごと消える。

確認方法と結果:

1. **オブジェクトファイルのバイト一致**。変更前(`HEAD`)のソースと、変更後
   かつフラグ無しのソースを同じコマンドラインでコンパイルし、`.o` を `cmp` した。
   計測コードを入れた3ファイルすべてが**バイト単位で同一**である。

   ```
   x86_64-w64-mingw32-gcc -nostdlib -mno-red-zone -O1 -c -Wall -Wextra \
       -mno-stack-arg-probe -DISIKIOS_BUILD_HASH=\"x\" -DISIKIOS_BUILD_DATE=\"y\" \
       -I<src> -o <out>.o <src>/<file>.c
   ```

   | ファイル | 結果 |
   |---|---|
   | `runtime.o` | IDENTICAL |
   | `kernel.o` | IDENTICAL |
   | `ide_subprimitive.o` | IDENTICAL |

2. **既定ビルドの回帰テスト**。`make test`(フラグ無し)は 8158 OK / 0 NG で、
   変更前と同じ。出力中の `[ALIGN]` 行は **0行**(計測コードごと消えている)。
   `make build`(フラグ無し)も成功する。

3. ドライバ側(`tools/bench/run_paint_audit.sh`)への変更は、`ALIGN_AUDIT` 環境変数を
   `make` へ素通しするだけで、未設定時は空文字が渡るため従来と同じ挙動になる。

---

## 7. 調査中に見つけたタグ関連の疑わしい箇所

### 7.1 `0xF8` の直書き9箇所(構造的リスク)

za.c の `jit_and_reg_imm8(..., 0xF8)` は `~TAG_MASK` を8bit即値で表したものだが、
`TAG_MASK` から導出されておらず独立した定数になっている(第2.3節の表)。
**現行の `TAG_MASK=0x7` とは一致しており、今バグではない。** ただしタグ幅を
変える改修では、ここが追随しないまま静かに通る典型的な omission list である。
`runtime.h` の `os_tag_is_heap_ref` が「単一の真実源」として導入された経緯
(`runtime.h:26-42`)と同じ形の問題であり、同じ扱いにするのが望ましい。

### 7.2 `TAG_FORWARD`(0x6)と MAGIC 値の下位3bit衝突(既知・文書化済み)

`MAGIC_STREAM`(0x6)・`MAGIC_BUILTIN_CLASS`(0xE)・STRINGのword0(生の長さ)は
下位3bitが `TAG_FORWARD` と衝突する。これを弾いているのは転送先アドレスの
**範囲検査の下限**だけで、`runtime.h:79-89` と `os_heap_init`(`runtime.c:1075-1077`)が
不変条件として明文化・実行時検査している。4bit化してもこの構造自体は変わらない
(衝突する MAGIC の顔ぶれが変わるだけ)なので、改修時は同じ検査を維持する必要がある。

### 7.3 `os_environment_register_literal_slot` がアライン前提を要求していない

`runtime.c:3683` は任意の `lisp_val_t*` を受け取り `TAG_RAW_POINTER` を付けて
Lisp値にする。呼び出し側(za.c の4つのスロットプール)は8byte刻みの静的配列の
要素アドレスを渡しており、奇数添字は必ず 8 mod 16 になる。
`test/c/runtime_test.c:1616` は C のローカル変数のアドレスを渡している。
タグ幅を広げるなら、このAPIにアライン要件を課す(または受け取り側で
16byte刻みのスロットへ移す)必要がある。

### 7.4 `for` 間欠バグとの関連

**本調査の範囲では、タグ表(`TAG_MASK` とその使用箇所)の誤りは見つからなかった。**

- C側のタグ操作はすべて `TAG_MASK` / `TAG_*` マクロ経由で、値の直書きは無い
  (第2.3節)。
- AOT生成コード(`src/c/lisp_compiled.c`)にタグ操作は 0 件。
- JITが直書きしている `0xF8` は現行の `~TAG_MASK` と一致している。
- 既存の焼き込み検出器(`%%ZA-HEAP-IMM-COUNT` / `za_code_imm_test.lisp`)は
  「GCで動く領域を指す即値」を対象としており、今回の監査でもそれと矛盾する
  観測は出ていない。

したがって「タグ表誤一致」を `for` 間欠バグの候補として維持する根拠は、
本調査からは得られていない。残るリスクは 7.1 の構造的なもの(将来の改修で
不一致を生みうる)であって、現時点の不一致ではない。

---

## 8. 未確認事項

| 項目 | 理由 |
|---|---|
| 22試験を個別に流したときの**値**側ヒストグラム | 値の計数は `gc_copy_value` 経由なので、GCが1回も走らない実行では何も取れない。実測でも22試験を個別起動するとGC 0回になり(各試験のアサーション1932件すべてがGC 0回)、値側は観測できていない。値のデータは1ブートで全試験を流す `qemu_boot_test.lisp` と、GC圧を上げた条件からのみ得ている |
| `test-qemu-all` の FS milestone 群(fat16 / fat32 / partition / 9p) | ディスクイメージを毎回作り直す破壊的テストで所要時間が大きいため未実施。FS層で問題になる `block_device_t*` ハンドルは静的に判定済みで、かつ `block_device_handle` サイトとして計数もしている(100% 8 mod 16) |
| `QEMU_MEM=64M` | OVMF が EFI をロードできず(`BdsDxe: ... Out of Resources`)ゲストが起動しない。GC圧を上げる下限は 96M |
| 実機(非QEMU)での確認 | 環境が無いため未実施。静的領域の配置はリンカとビルドフラグに依存するので、`g_nil_cell` の実際のアドレスは実機ビルドで再確認が要る(→ 第5章の注記) |
| JITコードに焼き込まれた即値そのものの下位4bit分布 | 既存の `%%ZA-HEAP-IMM-COUNT` 検出器(ヒープ参照かどうか)の範囲にとどめ、独自の走査は行っていない |
| `os_alloc_raw` が返す生バッファ(`os_stream_t` 等) | `os_alloc_bytes` と同一実体なので `os_alloc_bytes` サイトに合算されている。生バッファはタグ付きLisp値にならないため、アライン要件の対象外(ただし**次の確保をずらす**side effectは stride-break として計上済み) |

---

## 9. 計測コードの使い方(再現手順)

```bash
# ネイティブのユニットテスト(atexitで標準出力へ [ALIGN] 行を出す)
make test ALIGN_AUDIT=1

# QEMU試験(電源断の直前にシリアルへ [ALIGN] 行を出す)
make test-qemu-audit-run ALIGN_AUDIT=1 QEMU_MEM=96M \
     MILESTONE=test/lisp/qemu_boot_test.lisp \
     QEMU_EXTRA_FLAGS="-serial file:$PWD/tmp/align/out.serial"

# 22試験の監査ドライバ経由(グループごとに serial が分かれる)
ALIGN_AUDIT=1 AUDIT_CONTROL=0 AUDIT_OUTDIR=tmp/align_audit22 \
     tools/bench/run_paint_audit.sh
```

出力行の読み方:

```
[ALIGN] alloc site=<サイト名> total=<件数> misaligned=<(addr&0xF)!=0の件数> \
        stride-break=<切り上げ後サイズが16の倍数でない件数> hist=<下位4bitごとの件数×16>
[ALIGN] value tag=<タグ名> total=<件数> misaligned=<下位4bitがタグ値と一致しない件数> \
        first-bad=<最初に観測した不一致値> hist=<下位4bitごとの件数×16>
```

`value` 行の `hist` は下位4bitそのものなので、16byte境界に揃っていれば
**タグ値の位置(index=タグ値)にのみ**件数が立つ。index=タグ値+8 に立っている分が
8 mod 16 のアドレスである。
