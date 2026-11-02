# JIT が即値で参照するオブジェクトの調査

> 調査日: 2026-09-16 / ブランチ: `feature/jit-immediate-operands`(分岐元 `feature/compiler-optimization` `ed7fac2`)
> 関連: `documents/inline-builtin.md`(原則8 / `g_sym_t` の顛末)

Phase 3 で `g_sym_t` を即値に焼いてバグを踏んだ。
**グローバル変数のスロットは `.bss` にあって動かないが、それが指しているシンボル
オブジェクトの実体は GC ヒープ上にあり Cheney コピーで移動する。**
そこから出た 3 つの論点を調査した。

| # | 論点 | 結論 |
|---|---|---|
| (a) | `movabs`(10 byte)が過剰ではないか | **実装した。全生成コードが約 10% 縮む** |
| (b) | 即値に焼いてよいオブジェクトを区別できないか | **B1(一覧)+ B3 相当を実装。B2 は見送り** |
| (c) | nil を 0 番地に置いて `test rax, rax` にできないか | **不要。(a) で足りた** |

---

## 調査 (a): アドレスは imm32 に収まるか

### 実測: RAM 量でアドレスが動く

`%%DISASM-REGION-BOUNDS` で各領域の実アドレスを取り、`QEMU_MEM` を変えて比較した。

| QEMU_MEM | kernel `.text` | Immobilized | GC ヒープ | 2GB 未満 | 4GB 未満 |
|---|---|---|---|---|---|
| 96M | 39 MB | 44 MB | 28–39 MB | ○ | ○ |
| 256M | 199 MB | 204 MB | 24–187 MB | ○ | ○ |
| 1024M | 967 MB | 972 MB | 24–955 MB | ○ | ○ |
| 4096M | **3015 MB** | **3020 MB** | 24–3003 MB | **×** | ○ |
| 6144M | 3017 MB | 3036 MB | **4096–7168 MB** | × | **ヒープのみ ×** |
| 8192M | 3017 MB | 3036 MB | **4096–9216 MB** | × | **ヒープのみ ×** |

**UEFI はメモリ上端付近へイメージをロードするので、アドレスは RAM 量に比例して上がる。**
ただし **6144M / 8192M でも kernel と Immobilized は 3017 / 3036 MB で頭打ち**になる。
QEMU の PC マシンモデルは PCI ホールのため低位 RAM を約 3GB に制限しており、
UEFI はそこへロードするからである。4GB を超えた分は高位領域に置かれ、
**GC ヒープだけがそちらへ伸びる。**

`nil`(`g_nil_cell`)は `.bss` にあり Immobilized の直後(`immobilized_end + 0x31`)。
イメージと一緒に動く。

### 結論: ゼロ拡張の `mov r32, imm32` なら実用上ほぼ常に効く

| 形 | サイズ | 条件 |
|---|---|---|
| `movabs rax, imm64` | 10 byte | 常に可 |
| **`mov r32, imm32`** | **5 byte**(r8〜r15 は 6) | **値が 2^32 未満。上位はゼロ拡張** |
| `cmp rax, imm32` | 7 byte | 値が符号拡張で表せる(2^31 未満) |

**JIT が焼くもの——`nil`、カーネル関数のアドレス、`.bss` のスロットアドレス——は
すべてイメージか `.bss` にあり、実測で 4GB を超えなかった。**
GC ヒープのアドレスは**設計上焼かない**(移動するため)ので、
4GB 超のヒープは問題にならない。

一方 `cmp rax, imm32` は 2^31 未満が要るため、RAM 4GB 以上では使えない。**採らなかった。**

### 実装

`jit_load_imm_reg` が値を見て `mov r32, imm32` か `movabs` を選ぶ。
**収まらない場合は必ず `movabs` へフォールバックする。**

```c
static void jit_load_imm_reg(UINT8 reg, UINT64 imm) {
    if (imm <= 0xFFFFFFFFULL) {
        if (reg >= 8) { jit_emit8(0x41); }   /* REX.B。REX.Wは付けない(ゼロ拡張させる) */
        jit_emit8((UINT8)(0xB8 | (reg & 7)));
        za_record_imm_site(g_jit_used, 4);
        jit_emit32((UINT32)imm);
        return;
    }
    jit_movabs_reg_full(reg, imm);
}
```

**[重要] `jit_movabs_self_ref` は短縮してはならない。**
`patch_offset = g_jit_used + 2` を記録して後から 8 byte を書き換える前提なので、
命令長が変わると壊れる。専用に `jit_movabs_reg_full`(常に 10 byte)を用意した。

### 効果: サイズ(約 10% 減、全生成コードに効く)

| 関数 | 短縮前 | 短縮後 | 差 |
|---|---|---|---|
| `(defun f (x) x)` | 452 | **412** | −40 |
| `(car x)` notinline | 541 | **489** | −52 |
| `(car x)` inline | 590 | **534** | −56 |
| `(null x)` notinline | 533 | **481** | −52 |
| `(null x)` inline | 550 | **489** | −61 |
| `(eq x y)` notinline | 770 | **694** | −76 |
| `(eq x y)` inline | 780 | **699** | −81 |
| `(car (cdr (car (cdr x))))` notinline | 808 | **720** | −88 |
| `(+ a b)` | 815 | **739** | −76 |

**インライン展開に限らず、あらゆる生成コードが縮む。** 1 コールサイトあたり
`movabs` が十数個あるため(`documents/measurement-callsite-breakdown.md`)、
そこが 5 byte ずつ減る。Immobilized Space の消費に直接効く
(`documents/measurement-jit-code-packing-result.md` の作業と同じ方向)。

`QEMU_MEM=4096M`(アドレスが 2GB 超)でもサイズは同じだった。
**4GB 未満なら短縮形が使えるため。**

### 効果: 速度(TCG では解像できない)

同一実行内で notinline / inline を交互に測った(2,000,000 回)。

| | 実行A(5往復) | 実行B(8往復) | 判定 |
|---|---|---|---|
| `car` | −24.7% | −21.2% | **一貫して速い** |
| `cdr` | −22.9% | (未測定) | 速い |
| `null` | **+7.4%** | **−3.9%** | **符号が反転。ノイズ** |
| `eq` | **−6.1%** | **+8.1%** | **符号が反転。ノイズ** |

**`null` と `eq` は実行をまたぐと符号が反転する。** この粒度の差は
QEMU(TCG)では解像できないと判断する。`car` / `cdr` だけが一貫して速い。

**短縮の前後を比較してはいけない。** 前後は別の QEMU 実行であり、
同じコードでも実行間で 20% 以上ドリフトする(notinline `car` が
実行によって 304〜428 tick)。**有効なのは同一実行内の比較だけである。**

**TCG のコストは命令数に比例し、命令長にはあまり効かない。**
短縮は命令数を変えないので、速度への寄与は二次的(I-cache / TB サイズ)である。
**サイズが主目的の変更と理解するのが正しい。**

---

## 調査 (b): 即値に焼いてよいオブジェクトの区別

### B1(必須): 一覧

**za.c が即値で焼いているものの全数。** `jit_movabs_*` の呼び出し元を洗った。

#### 焼いてよい(現に焼いている)

| 対象 | 場所 | なぜ安全か |
|---|---|---|
| `nil` | `g_nil_cell`(`.bss`) | **From/To 空間の外**。`os_bootstrap` が作り、GC は動かさない |
| fixnum / char のリテラル | 即値そのもの | ポインタではない。`os_make_fixnum` は `magnitude << 3` |
| `MAGIC_*` / `TAG_MASK` / arity 等の定数 | 即値 | 同上 |
| カーネル関数のアドレス | `.text` | `os_car_checked` / `os_make_cons` / `za_gc_link` 等。移動しない |
| **スロットのアドレス** | `.bss` | `&g_sym_t` / `&nil` / `&global_environment` / `g_za_quote_slots[i]` / `g_za_number_slots[i]` / `g_za_fn_cell_cache_slots[i]` / gensym スロット。**アドレスを焼いて実行時に deref する** |
| Function Cell のアドレス | Immobilized | 不動。中身は GC が `gc_fixup_all_function_cells` で追う |
| `g_jit_code + off`(自己参照) | 一時 | コピー後に `g_jit_reloc_patch_offsets` で書き換える |

#### 焼いてはいけない(焼いていない)

| 対象 | 実際の扱い |
|---|---|
| **シンボル** | 名前文字列をコードへ埋め、実行時に `os_make_symbol` で再解決(`is_literal=2`) |
| **quote されたヒープ値** | `g_za_quote_slots[i]` に入れ、**スロットのアドレス**を焼いて deref(`is_literal=5`) |
| **float / bignum リテラル** | `g_za_number_slots[i]` + `os_gc_register_root`(`is_literal=7`) |
| **`g_sym_t`** | `&g_sym_t` を焼いて deref。**Phase 3 で値を焼いて事故**(`documents/inline-builtin.md`) |

**規則を一文にすると:「GC ヒープ上のオブジェクトのアドレスを焼いてはならない。
焼いてよいのは、不動領域(`.text` / `.bss` / Immobilized)のアドレスと、
ポインタでない即値だけ」。**

za.c は `za_classify_operand` / `za_classify_quoted_value` の段階でこの規則を
正しく実装していた(`is_literal=1` は **nil / fixnum / char のみ**)。
**Phase 3 で私が足した 1 箇所だけが唯一の違反だった。**
規則が明文化されていなかったのが根本原因である。

### Immobilized Space は GC の走査対象ではない(確認)

- `os_tag_is_heap_ref` は `TAG_RAW_POINTER` に 0 を返す(`runtime.h:41`)ので、
  `cells` / `pages` / `literal-slots` の中身は追われない
- だから `g_za_quote_slots` / `g_za_number_slots` は **`os_gc_register_root` で明示的に
  root 登録**している(`za.c:679, 978`)
- Function Cell の中身は **`gc_fixup_all_function_cells` が個別に再配置**している

**したがって Immobilized へオブジェクトを置くと、そこから GC ヒープへ出る参照は
自分で root 登録しなければ追われない。**

### B2(不動領域へ集める): 見送り

`t` の実体を Immobilized へ移せば「不動領域にあるものだけ焼いてよい」という
機械的な規則になる。**ただし副作用が大きい。**

- **intern との整合**: ソースに `t` と書かれたときリーダが返すオブジェクトが
  不動領域のそれと同一でなければならない。シンボルテーブル側の差し替えが要る
- **GC 走査**: 上記のとおり Immobilized は走査されない。シンボルは名前文字列
  (GC ヒープ上の string)を持つので、そこを root 登録する必要がある
- **効果が小さい**: `&g_sym_t` を deref する現行の形で既に安全であり、
  コストは 1 命令(`mov r10, [r10]`、3 byte)

**Phase 4 で型オブジェクト・クラスオブジェクトを扱うときに、まとめて検討するのが
効率的。** 単独で今やる理由は無いと判断した。

### B3(アサーション): 実装済み。さらに精密化した

`%%ZA-HEAP-IMM-COUNT` が既に存在し、`za_try_compile_defun` の末尾で
「生成コードに GC ヒープのアドレスが焼かれていないか」を数えている。
テストが 0 を期待している(`za_code_imm_test.lisp`、`inline_builtin_test.lisp`)。
**B3 は実質的に既にあった。**

ただし **(a) の短縮でこの監査が壊れかけた。**
従来はバイト走査で `movabs`(10 byte、REX.W + B8+rd + imm64)を探していたが、
短縮形 `mov r32, imm32` は**先頭 1 byte が 0xB8〜0xBF というだけ**なので、
バイト走査に足すと他の命令の途中へ大量に誤マッチする。
**実測で `QEMU_MEM=4096M` のとき 247 件の誤検出が出た**(GC ヒープ範囲が広いため)。

そこで **発行時に位置と幅を記録する方式へ変えた**(`za_record_imm_site`)。
デコードが不要なので誤検出がゼロになり、短縮形も漏れない。
`QEMU_MEM=256M` / `4096M` のどちらでも 0 件になることを確認した。

**短縮を入れるなら監査の作り替えは必須である。** 入れ忘れると安全網が黙って無効になる。

---

## 調査 (c): nil を 0 番地に置く案

**不要。(a) で足りた。**

### 5-3 の代替で足りたか

指示書の 5-3 は「nil を 2GB 未満の固定アドレスに置くだけで `cmp rax, imm32`(7 byte)が
使える」としていた。**実測では nil は RAM 4GB で 3036 MB(2GB 超)になるため、
`cmp rax, imm32` は使えない。**

代わりに **`mov r32, imm32`(5 byte)+ `cmp rax, r11`(3 byte)= 8 byte** になった
(従来は `movabs` 10 + `cmp` 3 = 13 byte)。**5 byte 縮んでおり、
`cmp rax, imm32` の 7 byte と大差ない。** これで十分と判断する。

### 0 番地案を採らない理由

1. **nil は自己参照 cons である。** 0 番地に実際に cons セルを配置するには
   そのページをマップして書き込む必要がある。現在カーネルがページテーブルを
   触るのはスタックガードの `unmap_range` だけで、新たにマップ処理が要る
2. **0 番地を未マップにしておく価値を失う。** ヌルポインタ参照が即フォルトする
   のはデバッグ上ありがたい。実際 `runtime.c` には
   「`env == 0` は `proc->env` 初期値。nil(タグ付きの実アドレス、0 ではない)とは別物で、
   これを `cc_car` へ渡すと低位メモリを cons として辿ってしまう」という
   **この区別に依存したコメントとコードが存在する**(`os_definition_env`、
   旧 `gc_fixup_environment_cells`)。nil を 0 にすると
   **この区別が消え、バグが静かに通るようになる**
3. **変更範囲が広い。** C 側の `== nil` / `!= nil` が **806 箇所**、
   ポインタの真偽判定が 37 箇所。Lisp 側も含めると影響は大きい

**利点(3 byte)に対して代償が大きすぎる。**

---

## まとめ

| 項目 | 判断 |
|---|---|
| (a) imm32 短縮 | **実装。全生成コードが約 10% 縮む。速度は TCG では解像できない** |
| (b) B1 一覧 | **本文書に記載(必須)** |
| (b) B2 不動領域へ集める | **見送り。Phase 4 でまとめて検討** |
| (b) B3 アサーション | **既存。(a) に合わせて記録ベースへ精密化した** |
| (c) nil を 0 番地 | **不要。(a) で足りた。代償が大きすぎる** |

## Phase 4 への引き継ぎ

- **規則は「GC ヒープ上のオブジェクトのアドレスを焼いてはならない」。**
  型オブジェクト・クラスオブジェクトを即値で参照したくなったら、
  `&変数` を焼いて deref するか、スロット + `os_gc_register_root` を使う
- `%%ZA-HEAP-IMM-COUNT` が 0 であることをテストで確認し続けること。
  **新しい命令形を足したら、`za_record_imm_site` で記録することを忘れないこと**
- `(+ x 1)` のような片方だけ定数の形では、定数側が fixnum なら
  `is_literal=1` で即値に焼ける(既に安全)。**2^32 未満なら 5 byte で済む**
- **速度は TCG では ±10% 程度が解像限界。** サイズは正確に測れる
