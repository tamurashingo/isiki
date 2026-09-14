# disassembler のアドレス領域分類

> 対象: `feature/disassembler`。
> `documents/disasm-backend-decision.md` / `documents/jit-metadata-investigation.md` の続き。

`disassemble` の出力に現れる生アドレスが「何であるか」を、まず**領域**の粒度で
示せるようにした。名前解決(シンボル名の表示)はこの結果を見てから決める。

```
002a  49 bb 0b 74 79 0c 00 00 00 00    movabs r11, 0xc79740b     ; <kernel>
02d5  48 b9 cd 82 d3 0c 00 00 00 00    movabs rcx, 0xcd382cd     ; <immobilized>
```

## 1. 各領域の境界の取得方法

| 領域 | 取得元 | 実装 |
|---|---|---|
| GC ヒープ | `g_from_start` / `g_to_end`(`runtime.c`) | 既存のグローバルをそのまま使う。確保処理には触れていない |
| Immobilized Space | `g_imm_space` / `IMM_SPACE_SIZE`(`runtime.c`) | 同上。16MB の静的配列なので境界は常に確定している |
| カーネル `.text` | `__ImageBase` から PE/COFF ヘッダを辿る | 新規。`classify_init_text_range`(`runtime.c`) |

API は `runtime.h` の `os_classify_addr` / `os_addr_region_name` /
`os_addr_region_bounds`。実装が `runtime.c` にあるのは、3領域の境界のうち2つが
すでに `runtime.c` の static 変数だからで、`disasm.c`(ランタイム非依存の純粋な
デコーダ)から触れると分離が崩れるため。

### なぜ「既知関数のアドレス ±16MB」にしなかったか

指示書 2-1 は `.text` の範囲が取れない場合の代替として ±16MB を挙げているが、
**この配置では成立しない**。

```
$ x86_64-w64-mingw32-objdump -h esp_dir/EFI/BOOT/BOOTX64.EFI
  0 .text  00209bf0  00000002446c1000     (2.1MB)
  5 .bss   01363780  00000002448e7000     (20.4MB)
```

`.bss` は `.text` の先頭からわずか 2.24MB 先で始まり、**その `.bss` の中に 16MB の
`g_imm_space`(Immobilized Space)がある**。±16MB の範囲は Immobilized Space を
丸ごと飲み込むので、JIT が置いたコードをカーネルコードと誤判定する。
セクション表から正確な範囲を取れば、この誤りは構造的に起きない。

UEFI はイメージを起動ごとに再配置するのでビルド時の VMA は使えない。
`__ImageBase`(mingw の PE リンカが定義、`-nostdlib -shared` でもリンクできることを
確認済み)を起点に、DOS ヘッダの `e_lfanew` → PE シグネチャ → COFF ヘッダの
セクション数/オプショナルヘッダ長 → セクション表、と辿って `.text` の
`VirtualAddress` / `VirtualSize` を読む。

ヘッダが期待した形でなければ `.text` の範囲は未確定のままになり、`.text` の判定だけが
行われなくなる(他の2領域は影響を受けない)。**これが黙って起きると困る**ので、
`%%DISASM-REGION-BOUNDS` で境界を外から読めるようにし、
`test/lisp/disassemble_test.lisp` が「3領域すべて境界が確定していること」を
実機で確認している(`documents/pitfalls.md` 原則6)。

### 既存の `os_addr_region` との関係

`runtime.c` には以前から `os_addr_region`(0=From 空間 / 1=To 空間 /
2=Immobilized / 4=その他)がある。**これは変更していない。**
目的が別で、あちらは GC 監査専用に From と To を**区別する**ことに意味がある
(生成コードに焼き込まれた即値が「GC で動く側」を指していないかの判定)。
戻り値の意味は `test/lisp/za_code_imm_test.lisp` が依存している。

`os_classify_addr` は表示用で From/To の区別は意味を持たないため、両方を
`OS_ADDR_GC_HEAP` にまとめている。境界の変数は共有しているので二重管理にはならない。

## 2. 実測値(QEMU 起動時の実値、2026-09-14)

```
#region kernel      0xC787000 .. 0xC9912B0     (2,135,216 B = .text の VirtualSize と一致)
#region immobilized 0xCC52000 .. 0xDC52000     (16,777,216 B = IMM_SPACE_SIZE と一致)
#region gc-heap     0x17802C0 .. 0xBB6C000     (QEMU_MEM=256M 時)
#region dis-add code-base 0xCD37000 (region 2 = immobilized)
```

- 3領域に**重なりは無い**。`runtime_test.c` が確定している領域の組を総当たりで確認する。
- Immobilized Space はカーネルイメージの `.bss` 内にあるため `.text` のすぐ上に来るが、
  `.text` の範囲(`VirtualSize`)には含まれないので誤判定しない。
- GC ヒープは UEFI の最大空き領域から取るのでイメージとは離れた場所にある。
  `0xBB6C000`(ヒープ上端)と `0xC787000`(`.text` 先頭)の間には約 12MB の隙間があり、
  ここは UEFI 側の割り当てで、本 OS が管理する領域ではない。

## 3. `0x0C1D41AA` は何だったか

**現在のビルドで実測すると、この値はどの領域にも属さない(`OS_ADDR_UNKNOWN`)。**
GC ヒープの上端 `0xBB6C000` より上、`.text` の先頭 `0xC787000` より下の隙間に落ちる。

ただしこれは「意味のあるアドレスではなかった」という意味ではなく、
**UEFI がイメージを起動ごとに違うアドレスへ再配置するため、別の起動で採取した
生アドレスはそのまま比較できない**ということである。

同じ命令形を現在の起動で実測すると、結果ははっきりしている。
`(defun probe-f (a b) (+ a b))` が出す `movabs r11, <addr>` を全部並べると:

```
#movabs r11, 0xc79740b <kernel>          ← za_gc_current_head 等のCの関数
#movabs r11, 0xc79742b <kernel>
#movabs r11, 0xc795630 <kernel>
   ...(call r11 の直前に置かれるものは 17 個すべて <kernel>)
#movabs r11, 0x0                         ← 注釈なし(整数の 0。制御転送判定の比較対象)
#movabs r9, 0x8000000000000007           ← 注釈なし(TAG_MASK|FIXNUM_SIGN_BIT のビットマスク)
```

`call r11` の直前に置かれる `movabs r11, <addr>` は**すべて `<kernel>`**、つまり
生 C で実装されたカーネル内の関数である。`0x0C1D41AA` も、採取した起動での
`.text` が `0x0C1D_xxxx` 付近にあったと考えれば同じ性質のものと整合する
(現在の起動では `0x0C79_xxxx` に来ている)。

注釈が付かない `0x0` や `0x8000000000000007` は、アドレスではなく整数・ビットマスクで、
**引けないものに注釈を出さない**という方針どおりの結果になっている。

他関数を名前で呼ぶ関数(`dis-caller`)では `<immobilized>` が1件出る。これは
`za_emit_fn_resolve_cached` が焼き込む Function Cell / 解決結果キャッシュのスロットで、
Immobilized Space 上にある。

### GC ヒープを指す即値は 0 件

`dis-add` / `dis-caller` のいずれにも `<gc-heap>` の注釈は出ない。これは
`documents/pitfalls.md` 原則8(生成コードに GC で動くアドレスを焼き込まない)の
不変条件そのもので、`za_code_imm_test.lisp` が movabs 即値の走査で確認している
のと同じことを、逆アセンブラ側から独立に確認していることになる。
`disassemble_test.lisp` でアサーションにしてある。

## 4. 注釈の対象にしたもの / しなかったもの

| 対象 | 扱い |
|---|---|
| `movabs` の imm64 | 注釈する。本件の主対象 |
| コードブロックの**外**へ飛ぶ `call`/`jmp`/`jcc rel32` の解決先 | 注釈する |
| RIP 相対の実効アドレス | 注釈する(za.c は現状出さないが、対応は入れてある) |
| コードブロックの**内**へ飛ぶ分岐 | **注釈しない**(下記) |
| `imm8` / `disp8` | 注釈しない。値域が狭く誤ヒットする |

ブロック内へ飛ぶ分岐を対象外にしたのは指示書からの逸脱なので理由を書いておく。
飛び先は定義上その関数自身と同じ領域(Immobilized Space)にあるため、注釈しても
情報が増えない。一方で `je` / `jne` は生成コード中に多数現れるので、
全部に `; <immobilized>` が付くと行が伸びるだけ出力が読みにくくなる。
飛び先は `operands` にオフセットとして出ているので、そちらで足りる。

`has_target_addr` / `target_addr` はデコーダ(`disasm.c`)が埋め、注釈文字列
(`comment`)はランタイム側(`disasm_lisp.c`)が `os_classify_addr` を引いて埋める。
デコーダをランタイムから独立に保つための分担で、`disasm_test.c` が単体でリンクできる
性質を維持している。

`comment` は `operands` へ連結していない。連結すると `disassemble-to-list` の利用側が
オペランドと注釈を分離するためにパースする羽目になるため、独立した要素にしてある。

## 5. 未解決 / 次の段階

- **共有トランポリンは `UNKNOWN` になる。** 末尾呼び出しの `jmp .+0x... ; outside` が
  指す先は za.c の `g_jit_code`(512KB の静的配列)で、`.bss` 内にあるため `.text` の
  範囲に入らない。指示書が定めた4値の enum には該当する値が無いので、今回は
  `UNKNOWN` のままにした。専用の領域値を足すかどうかは判断が要るので報告に留める。
- **名前解決は未実装**(指示書 §7)。今回の結果から、必要なのは
  **カーネル `.text` 側のシンボルテーブル**である(`call r11` の飛び先が
  すべて `<kernel>` だったため)。ビルド時にシンボルを抽出してアドレス昇順の
  静的テーブルを生成し、リンクする方式になる。
  Immobilized Space 側(env 走査による逆引き)は、現状 `dis-caller` で1件しか
  出ないため優先度は低い。
