# タグ拡張 第1段階 実施報告（準備・挙動を変えない）

対象ブランチ: `feature/tag4-step1`（`main` = 421f61c から分岐、PR #75/#76 マージ後）

前提文書:
- 設計: `documents/tag4-design.md`（PR #76）
- JIT 定数の導出: `documents/jit-tag-constants.md`（PR #75）

本段階では**タグ幅は 3bit のまま**であり、`most-positive-fixnum` の値も
`2^60−1` のままである。値を変えるのは第2段階。

---

## 1. 指示書と設計報告の突き合わせ（Step 0）

### 1-1. 報告書にあって指示書になかった作業（報告書を正として追加した）

| 項目 | 実施 | コミット |
|---|---|---|
| ALIGN_AUDIT のタグ添字配列を `[16]`、ループ上限を `TAG_MASK+1` から導出 | 済 | 2836032 |
| `interrupt.c:765` の `~0x7` を `TAG_MASK` から導出 | 済 | 2836032 |
| `MAGIC_*` を下位4bit=`0xE` へ振り直し＋`_Static_assert` | 済 | 2836032 |

### 1-2. 矛盾（作業前に判断した1件）

`src/c/lisp.c:82` の死んだ分岐について、両者の指示が食い違っていた。

| 出典 | 指示 |
|---|---|
| 本指示書 Step 4 | `k & TAG_FIXNUM` が常に偽になる分岐を**削除する** |
| 設計報告 7.1 第1段階の表 | その分岐を `(k & TAG_MASK) == TAG_FIXNUM` に**直す** |

**削除を採った。** 理由は「直す」が挙動を変えるためである。

```c
else if (k & TAG_FIXNUM && (k >> FIXNUM_VALUE_SHIFT) == (key >> FIXNUM_VALUE_SHIFT)) {
```

`TAG_FIXNUM == 0` なのでこの条件は常に偽で、分岐は一度も実行されていない。
これを `(k & TAG_MASK) == TAG_FIXNUM` に直すと分岐が**生きる**。生きたとき、
`k` が fixnum で `key` が cons/symbol/string 等だった場合に
`(k >> 3) == (key >> 3)` が成立しうる（fixnum のマグニチュードが相手の
アドレス `>> 3` とたまたま一致する場合）。つまり**別種の値を誤って一致と
みなす経路が新たに生まれる**。本段階は「挙動を一切変えない」段階なので採れない。

一方、fixnum 同士なら下位3bitはどちらもタグ0なので `k >> 3 == key >> 3` は
`k == key` と同値であり、直前の raw eq で既に拾えている。したがって削除は
挙動を変えない。

指示書 Step 4 は「残す場合は `_Static_assert(TAG_FIXNUM == 0, ...)` を添えて
理由を報告する」という選択肢も認めていたが、残す理由が無いので削除した。

---

## 2. Lisp 側の境界定数の導出経路（Step 1）

C 側の `FIXNUM_MAGNITUDE_MASK` が唯一の真実源であり、Lisp 側には独立した
数値を置いていない。

```
runtime.h:  #define FIXNUM_MAGNITUDE_MASK ((1ULL << 60) - 1)
              |
runtime.c:  primitive_fixnum_magnitude_mask()  →  os_make_fixnum(FIXNUM_MAGNITUDE_MASK)
              |  os_set_function("%%FIXNUM-MAGNITUDE-MASK", ...)
              v
init.lisp:  (defconstant *most-positive-fixnum* (%%fixnum-magnitude-mask))
            (defconstant *most-negative-fixnum* (- 0 *most-positive-fixnum*))
```

- 命名は既存の `*most-positive-float*` / `*most-negative-float*`
  （`src/lisp/init.lisp:896-897`）にそろえた
- 負側は正側の符号反転ちょうどになる。fixnum が2の補数ではなく
  **符号＋絶対値**表現であり、`-2^60` のような非対称な下端が存在しないため
  （設計報告 4.1）
- 組み込みの追加は1個に抑えた。`runtime_test` の `SMALL_HEAP_SIZE` は
  組み込み関数の数に敏感で、増やしすぎるとブート時ライブ量が増えて
  ハングする（過去に発生）。実測では 8176 OK / 0 NG で再調整は不要だった

### C 側の値と一致することの確認テスト

`test/lisp/init_test.lisp`:

```lisp
(assert-equal t (= *most-positive-fixnum* (%%fixnum-magnitude-mask)))
(assert-equal t (fixnump *most-positive-fixnum*))
(assert-equal t (fixnump *most-negative-fixnum*))
(assert-equal t (= *most-negative-fixnum* (- 0 *most-positive-fixnum*)))
(assert-equal nil (fixnump (+ *most-positive-fixnum* 1)))
(assert-equal nil (fixnump (- *most-negative-fixnum* 1)))
```

最後の2行が本質である。定数が C の値と一致するだけでなく、**その値が本当に
処理系の fixnum 境界である**（1つ外に出ると fixnum でなくなる）ことを見ている。

---

## 3. 直書きリテラルの置き換え（Step 2）

`1152921504606846975/6/7` と `#x1000000000000000` の出現回数:

| ファイル | 置換前 | 置換後 |
|---|---|---|
| `test/lisp/init_test.lisp` | 38 | 7（うちコメント3） |
| `test/lisp/za_test.lisp` | 7 | 0 |
| `test/lisp/za_test_ext13.lisp` | 1 | 2（うちコメント1） |
| 合計 | **46** | **9**（コード上は6） |

### 導入した中間定数

```lisp
;; init_test.lisp
(defglobal fx-max   *most-positive-fixnum*)        ; 現在は 2^60-1
(defglobal fx-over  (+ *most-positive-fixnum* 1))  ; 境界の1つ外、bignum
(defglobal fx-over2 (+ *most-positive-fixnum* 2))

;; za_test.lisp
(defglobal za-fx-over (+ *most-positive-fixnum* 1))
(defglobal fixnum-max *most-positive-fixnum*)      ; 既存の名前を値だけ差し替え
```

### あえて直書きのまま残した3箇所と、その理由

いずれも**境界定数ではない**ため、定数経由にすると試験の意味が変わる。
コメントに理由を書いた。

| 箇所 | 内容 | 残した理由 |
|---|---|---|
| `init_test.lisp:854` | `(= -1152921504606846976 (- 0 1152921504606846976))` | 対象は「**負の bignum リテラルを reader が読めること**」。リテラルを式に置き換えると reader を通らなくなる |
| `init_test.lisp:881-882` | `(bignump #x1000000000000000)` ほか | 対象は「**radix リテラルの桁あふれ判定**」。同上 |
| `za_test_ext13.lisp:52` | `(assert-equal 1152921504606846976 (... 1073741824 1073741824))` | 期待値は `2^30 * 2^30` という**積そのもの**であり、境界が動いても値は変わらない。`*most-positive-fixnum*` から導くとむしろ誤りになる |

第2段階で fixnum の上限は `2^60−1` → `2^59−1` と**狭まる方向にしか動かない**ので、
これら3箇所が扱う `2^60` は引き続き fixnum 範囲外であり、テストは成立し続ける。

---

## 4. `_Static_assert` の追加（Step 3）

メッセージはすべて ASCII で書いた。GCC は非 ASCII を8進エスケープで出力するため、
日本語メッセージはコンパイルエラー時に読めなくなる（PR #73 で確認済み）。
日本語の説明は隣接コメントに置いた。

### 4-1. タグ値の不変条件（`runtime.h`、タグ定義の直後）

| 内容 |
|---|
| `TAG_MASK` が下位の連続ビット（`2^n − 1`）であること |
| `TAG_FIXNUM`〜`TAG_RAW_POINTER` の8個が `TAG_MASK` に収まること |
| 8個が互いに重複しないこと（`__builtin_popcountll` で立つビットを数える） |

### 4-2. 64bit 語レイアウトの不変条件（`runtime.h`、`CHAR_VALUE_SHIFT` の直後）

| 内容 |
|---|
| `(1 << FIXNUM_VALUE_SHIFT) > TAG_MASK`（値フィールドがタグに食い込まない） |
| `(1 << CHAR_VALUE_SHIFT) > TAG_MASK`（同上） |
| `FIXNUM_SIGN_BIT` がちょうど1bitであること |
| マグニチュードが符号bitに食い込まないこと |
| マグニチュードがタグに食い込まないこと |
| **符号bit ＋ マグニチュード ＋ タグ が隙間も重なりもなく 64bit を覆うこと** |

最後の1本が要である。3つの定数が同じ64bit語を分け合っているので、どれか1つ
だけを動かすと静かに食い違う。第2段階は「`TAG_MASK`→`0xF`、
`FIXNUM_VALUE_SHIFT`→4、`FIXNUM_MAGNITUDE_MASK`→`(1<<59)-1` を同時に」だが、
1つ忘れた瞬間ここで落ちる。

### 4-3. `MAGIC_*` の不変条件

| 内容 |
|---|
| 全14個の下位4bitが `0xE` であること |
| 14個が互いに重複しないこと |
| （既存）全14個が `MAGIC_MUST_BE_BELOW`(0x1000) 未満であること |

### 4-4. 負のテスト（実際に落ちることの確認）

| 変異 | 出力 |
|---|---|
| `TAG_MASK` を `0xFULL` に | `"FIXNUM_VALUE_SHIFT is too small: the value field would overlap the tag"` / `"CHAR_VALUE_SHIFT is too small: ..."` / `"fixnum magnitude field overlaps the tag"` |
| `MAGIC_VECTOR` を `0xAD` に | `"MAGIC_VECTOR low nibble is not 0xE"` |
| `MAGIC_VECTOR` を `0xBE`（= `MAGIC_FLOAT`）に | `"MAGIC_* values are not all distinct"` |

いずれも ASCII でそのまま読める形で出た。

---

## 5. `MAGIC_*` の振り直しと、移行中の注意

`0x1`〜`0xF`（`0x7` 欠番）だった14個を `0x0E`〜`0xDE` に振り直した。
目的は第2段階で `TAG_FORWARD` が `0xF` になったとき、MAGIC がタグとして
`TAG_FORWARD` に一致することを**構造的に**なくすこと（設計報告 5.2）。

### 移行中は衝突が一時的に増える

タグが 3bit のあいだ、`0xE` の下位3bitは `0x6` = `TAG_FORWARD` である。
したがって**この期間だけ全14個が下位3bitで `TAG_FORWARD` と衝突する**
（振り直し前は `MAGIC_STREAM`(0x6) と `MAGIC_BUILTIN_CLASS`(0xE) の2個だけ）。

安全である根拠:

- 衝突を実際に弾いているのは `gc_copy_value` の**範囲検査**であって、
  タグの一致ではない（設計報告 5.4 が「範囲検査は廃止できない」と結論した理由
  そのもの。`TAG_STRING` の word0 = 生の長さも同じ検査に守られている）
- 余裕は MAGIC 最大 `0xDE`(222) 対 ヒープ先頭 `> 0x1000` で5桁近くある
- `os_heap_init` は起動時にヒープ先頭が `0x1000` 以下なら panic する
- `MAGIC_MUST_BE_BELOW` の `_Static_assert` 14本が引き続き上限を機械的に保証する

第2段階で `TAG_FORWARD` が `0xF` になった時点で、衝突数は 14 → **0** になる。

### 本段階では見送った連動項目

設計報告 5.2 は `GC_DEBUG_TRAP_PATTERN_VALUE` も `0xDEADDEADDEADDEAE` にすると
書いているが、**第2段階に送った**。この値は「塗り潰しパターン ＋ タグ」であり、
タグが 3bit のうちに `…AE` へ動かすと、塗り潰し側が書く値と食い違って
GC_PAINT 監査が機能しなくなる。タグ幅の変更と同時に動かすべき項目である。

---

## 6. 挙動が変わっていないことの検証（Step 5）

### 6-1. ユニットテスト

| 時点 | 結果 |
|---|---|
| 分岐元 `main`(421f61c) | 8170 OK / 0 NG |
| Step 1〜3 適用後 | 8176 OK / 0 NG |
| Step 4（死んだ分岐削除）後 | 8176 OK / 0 NG |
| 報告書由来3項目 適用後 | 8176 OK / 0 NG |

増分の 6 件は §2 で追加した境界定数の確認テストそのもの。NG は一貫して 0。

### 6-2. JIT が出力するバイト列

`MAGIC_*` を振り直したので、**JIT の出力バイト列は変わる**（PR #75 の
「バイト列が一致すること」とは前提が違う）。変化が MAGIC 即値だけに
閉じていることを、`za.o` の逆アセンブル比較で確認した。

手順: `main`(421f61c) と HEAD のそれぞれで `za.c` を同一フラグでコンパイルし、
逆アセンブルからアドレス（RIP 相対変位・関数先頭・分岐先）を正規化して diff。

| 分類 | 行数 | 内容 |
|---|---|---|
| RIP 相対変位のずれ | 80 | 命令長が変わってホスト側のコード配置がずれただけ。JIT の出力ではない |
| `nop`（整列パディング） | 9 | 同上 |
| MAGIC 即値そのもの | 18 | 下記（9箇所 × 旧/新） |
| GCC による同値の再符号化 | 15 | 下記 |
| **合計** | **122** | |

**MAGIC 即値（JIT が実際に埋め込む値）**: `za.c` の `jit_movabs_reg`/
`jit_movabs_r11` に MAGIC を渡している箇所は**ちょうど9箇所**で、
diff に現れた即値の内訳と完全に一致する。

| 定数 | 旧 → 新 | 出現 | za.c の行 |
|---|---|---|---|
| `MAGIC_FUNCTION_NATIVE` | `$0x1` → `$0xe` | 3 | 1678, 3957, 4053 |
| `MAGIC_FUNCTION_INTERPRETED` | `$0x2` → `$0x1e` | 2 | 2945, 5555 |
| `MAGIC_BLOCK_EXIT` | `$0x5` → `$0x4e` | 1 | 4574 |
| `MAGIC_CATCH_EXIT` | `$0x9` → `$0x7e` | 2 | 4836, 4909 |
| `MAGIC_GO_EXIT` | `$0xa` → `$0x8e` | 1 | 5894 |

`jit_movabs_reg` は REX + オペコード + `imm64` の**常に10バイト**を出すので、
即値の大きさが変わっても**生成コードの長さは変わらない**。変わるのは
この9箇所の即値スロットの中身だけである。

**GCC による同値の再符号化**（ホスト側の MAGIC 判定。JIT の出力ではない）:

| 箇所 | 旧 | 新 | 判定している集合 |
|---|---|---|---|
| `os_is_control_transfer` | `lea -0x9(%rdx); cmp $0x1; setbe` ＋ `cmp $0x5` | `lea -0x4e(%rdx); test $~0x40; sete` ＋ `cmp $0x7e` | 旧 {CATCH,GO} ＋ {BLOCK} → 新 {BLOCK,GO} ＋ {CATCH}。**同じ3値の3分岐**を GCC が別のグループ分けで符号化しただけ |
| 数値型判定 | `lea -0xb(%rax); test $~0x2` → {0xB,0xD} | `mov %rax,%rdx; and $~0x20; cmp $0x9e` → {0x9E,0xBE} | どちらも {BIGNUM, FLOAT}。同一集合 |

MAGIC 即値以外に JIT の出力を変える差分は無い。

### 6-3. 実機ブート・ALIGN_AUDIT

実機(QEMU/OVMF)でのブートテスト:

| 条件 | 分岐元 `main` | HEAD |
|---|---|---|
| `make test-qemu QEMU_MEM=256M` | 3269 passed, 0 failed | **3275 passed, 0 failed** |
| `make test-qemu QEMU_MEM=96M` | 3269 passed, 0 failed | **3275 passed, 0 failed** |

増分6件はユニットテストと同じく、§2 で追加した境界定数の確認テスト。

ALIGN_AUDIT（`ALIGN_AUDIT=1`、シリアルを `-serial file:` で捕捉）:

| | 256M | 96M |
|---|---|---|
| `violations` | **0** | **0** |
| `heap-align` | 16 | 16 |
| gc-count | 14 | 73 |
| gc-live peak | 5,623,504 | 5,623,504 |
| ext-region 2 misaligned | 0 / 39,049 | 0 / 197,860 |
| ext-region 4 misaligned | 0 / 259,634 | 0 / 913,282 |
| ext hash table overflow | 0 / 8192 | 0 / 8192 |

確保サイト10種すべてで `misaligned=0`、下位4bitヒストグラムは全件が
バケット0（= 16byte境界ちょうど）に入った。

値の側のヒストグラムも、各タグが**自分のタグ値のバケットだけ**に入っている
（16byte境界なら「下位4bit == タグ値」になる）。

| タグ | 256M | 96M | 下位4bit |
|---|---|---|---|
| CONS | 731,567 | 2,302,212 | 全件 1 |
| SYMBOL | 258,364 | 806,610 | 全件 2 |
| STRING | 35,779 | 180,862 | 全件 4 |
| INSTANCE | 65,354 | 283,108 | 全件 5 |
| RAWPTR | 59,614 | 303,167 | 全件 7 |

`first-bad` は全タグで 0（= 一度も境界外を観測していない）。
`MAGIC_*` の振り直しでヒープ上のオブジェクト配置は変わっていない
（MAGIC は word0 の内部識別子であってサイズにもアラインにも効かない）
ことが、`gc-live peak` が両条件で完全に一致していることからも確かめられる。

なお ALIGN_AUDIT のタグ添字配列を `TAG_MASK+1` から導く形に変えたが、
本段階では `TAG_MASK == 0x7` なので `ALIGN_TAG_COUNT == 8` であり、
**出力の形も内容も変わっていない**。

---

## 7. 判断待ちの整理（PR #76 8章のうち、指示書 §2 で確定していないもの）

指示書 §2 で確定済み: #1（fixnum は符号絶対値のまま）、#2（`<float>` は
binary64 のままヒープ、`0x4` は予約のみ）、#4（文字列の文字単位化は別フェーズ）。
#6（`MAGIC_*` の振り直し）は本段階で実施済み。残る5件を以下に整理する。

### 7-1. character のペイロード幅（8章 #3）

| | |
|---|---|
| **選択肢** | (a) 32bit（bit32-63、`CHAR_VALUE_SHIFT` = 32） / (b) 21bit（Unicode コードポイントの実幅、bit4-24） |
| **推奨** | **(a) 32bit** |
| **理由** | `UINT32` をそのまま置けるのでシフト後のマスクが要らず、`os_make_char` の符号拡張問題（`const char` が 0x80 以上で符号拡張し上位ビットを汚す、`runtime.c:3220`）が**表現の変更と同時に消える**。21bit にすると上位11bitが未使用のまま残り、「そこに何を入れてよいか」の規約が新たに必要になる |
| **第2段階への影響** | `CHAR_VALUE_SHIFT` を 3 → 32 に変え、`os_make_char` の引数を `const char` → `UINT32` にする。第2段階の分割不可セットの3番目そのもの。指示書 §2 は符号拡張の修正を「先送り」としたが、**(a) を採ると第2段階で自動的に解消される**ので、独立した修正フェーズは不要になる |

### 7-2. 空きタグ `0xB` / `0xD`（8章 #5）

| | |
|---|---|
| **選択肢** | (a) 空けておく / (b) vector・bignum をタグに昇格させて型判定を速くする |
| **推奨** | **(a) 空けておく** |
| **理由** | vector も bignum も本体が可変長でヒープ上にあり、即値にはできない。タグに昇格しても「`TAG_INSTANCE` ＋ word0 比較」が「専用タグ比較」になるだけで、ヒープ参照は残る。得られるのは分岐1回分で、失うのは将来 double-float / ratio を入れる枠である |
| **第2段階への影響** | なし。(a) なら第2段階は何もしない。`os_tag_is_heap_ref` が未割当タグに対して何を返すかだけ決めておけばよく、それは 7-5 と同じ話 |

### 7-3. `TAG_FORWARD` の値（8章 #7）

| | |
|---|---|
| **選択肢** | (a) `0xF` / (b) 他のポインタ用の値 |
| **推奨** | **(a) `0xF`** |
| **理由** | 本段階で `MAGIC_*` を下位4bit=`0xE` にそろえたので、`0xF` にすれば MAGIC との衝突が構造的に消える。**この振り直しは既にコミット済みなので、(b) を採ると本段階の作業が無駄になる**。`0xF` はポインタタグとしても最後の値で、他の用途と競合しない |
| **第2段階への影響** | `TAG_FORWARD` を `0x6` → `0xF` に変える。同時に「MAGIC が `TAG_FORWARD` と下位4bitで一致しないこと」の `_Static_assert` を足せる（本段階では `MAGIC_LOW_NIBBLE == 0xE` の形で先取りしてある） |

### 7-4. STRING のヘッダ（8章 #8）

| | |
|---|---|
| **選択肢** | (a) 生の長さのまま（範囲検査に頼る） / (b) 型付きヘッダワードを持たせる |
| **推奨** | **(a) のまま。第2段階の範囲外として記録だけする** |
| **理由** | 長さは Lisp プログラムが決める任意の整数なので下位4bitを制御できず、`…F` で終わる長さの文字列は `TAG_FORWARD` と必ず下位4bitが一致する。つまり**タグをどう振っても範囲検査は消せない**。(b) は全 STRING のレイアウト変更で、アロケータ・GC・`create-string`・`string-append`・`elt` すべてに波及する。タグ拡張とは独立した大きさの改修である |
| **第2段階への影響** | なし。ただし「範囲検査は本質的に必要」という前提が第2段階でも続くことを、`gc_copy_value` のコメントに明記しておくべき |

### 7-5. 未割当の即値タグ `0x6` / `0x8` / `0xA` / `0xC`（8章 #9）

| | |
|---|---|
| **選択肢** | (a) 未定のまま / (b) 今のうちに用途を決める |
| **推奨** | **(a) 未定のまま** |
| **理由** | 用途が無いのに割り当てると、後から別の用途が来たときに振り直しになる。空けておく costs は無い |
| **第2段階への影響** | `os_tag_is_heap_ref` がこれら4値に対して**偽**を返すことだけは保証が要る。真を返すと GC が即値をポインタとして追いかける。本段階で追加した「`TAG_*` が互いに重複しない」`_Static_assert` は未割当値までは見ないので、第2段階で `os_tag_is_heap_ref` の全16値に対する表を作り、テストで固定するのがよい（設計報告 7.3 の T-4） |

### 7-6. 併せて記録: `os_make_char` の符号拡張

指示書 §2 で「先送り」と確定した項目だが、7-1 と直結するので記録しておく。

```c
lisp_val_t os_make_char(const char c) {          // runtime.c:3220
    return ((lisp_val_t)c) << CHAR_VALUE_SHIFT | TAG_CHAR;
}
```

`char` が符号付きの環境では `0x80` 以上の文字で符号拡張が起き、上位ビットが
すべて 1 になる。現状 8bit 文字しか作らない経路では実害が出ていないが、
7-1 の (a) を採って引数を `UINT32` にすれば**同時に消える**。独立したフェーズを
立てるより、第2段階の character 変更に含めるほうが手数が少ない。

---

## 8. コミット構成

| コミット | 内容 | ロールバック単位 |
|---|---|---|
| 4036cfa | fixnum 境界定数の導入・直書きリテラルの置換・語レイアウトの `_Static_assert` | Step 1〜3 |
| fa20059 | `cc_assoc_eq` の死んだ分岐の削除 | Step 4（独立） |
| 2836032 | ALIGN_AUDIT / `interrupt.c` のマスク導出、`MAGIC_*` の振り直し | 報告書由来3項目 |

指示書 Step 4 の「この変更は他の作業と独立しているので、コミットを分ける」に
従い、死んだ分岐の削除は単独コミットにしてある。
