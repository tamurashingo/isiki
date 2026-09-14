# disassembler バックエンドの選定(Phase 1)

> 対象: `feature/disassembler`。`documents/jit-metadata-investigation.md` と対で読む。

## 意思決定

**採用案: C(案C: 手書きデコーダー)** — `src/c/disasm.c`

## 判定根拠

指示書の判定ロジック(「Capstoneが既に使われていれば案A、ビルドの軽さが重要なら案C、
それ以外は案Aを試して駄目なら案C」)を、本リポジトリの制約に当てて評価した。

### 1. 実行環境がフリースタンディングのUEFIカーネルである

これが決定的である。カーネル本体は

```
x86_64-w64-mingw32-gcc -nostdlib -mno-red-zone -O1 -shared -Wl,--subsystem,10 ...
```

でビルドされる(`Makefile` の `$(TARGET)` ルール)。`-nostdlib` であり libc も
動的ローダも無い。`disassemble` は**ゲスト(isiki-os)の中から**呼べる必要があるので、
ホスト上の共有ライブラリである Capstone / LLVM MC はそもそもリンクできない。
Capstone をフリースタンディング向けに移植することは理論上可能だが、
`malloc`/`calloc`/`realloc`/`free` と `snprintf` に依存しており(本リポジトリには
いずれも存在しない)、移植コストは手書きデコーダを大きく上回る。

### 2. 依存性の確認結果

```
$ grep -ic capstone Makefile   → 0
$ grep -ic llvm Makefile       → 0
$ grep -rl "capstone\|llvm" src/ test/
  test/lisp/init_test.lisp   ← 日本語の「capstone(総仕上げ)テスト」というコメントで、
                                逆アセンブラとは無関係
$ docker run --rm isiki-builder apt-cache policy libcapstone-dev
  Installed: (none)   Candidate: 4.0.2-5
```

既存の利用はゼロ。ビルドコンテナにも入っていない。

### 3. デコード対象が「za.c が出力する命令だけ」に閉じている

汎用逆アセンブラが必要なのではなく、**`src/c/za.c` の `jit_*` ヘルパーが出力しうる
エンコーディングだけ**を読めればよい。出力箇所は `jit_emit8` を呼ぶ十数個の
static ヘルパーに集約されており、全数を列挙できる(下表)。この「有限で、同じ
リポジトリ内にあり、変更が追える」という性質が手書きを現実的にしている。

## 実装スコープ

### Phase 1 でサポートする命令

`za.c` が実際に出力するものを網羅する。左列が出力元のヘルパー。

| za.c の出力元 | エンコーディング | 表示 |
|---|---|---|
| `jit_movabs_reg` / `_rax` / `_r11` | `REX.W B8+r imm64` | `movabs rax, 0x...` |
| `jit_push_rbx` / `jit_push_r13` / `jit_push_r14` | `[41] 50+r` | `push rbx` |
| `jit_pop_rbx` / `jit_pop_r13` / `jit_pop_r14` | `[41] 58+r` | `pop rbx` |
| `jit_sub_rsp_imm8` / `jit_add_rsp_imm8` | `REX.W 83 /5,/0 ib` | `sub rsp, 0x20` |
| `jit_sub_rsp_imm32` / `jit_add_rsp_imm32` | `REX.W 81 /5,/0 id` | `sub rsp, 0x1a8` |
| `jit_and_reg_imm8` | `REX.W 83 /4 ib` | `and r10, -8` |
| `jit_call_r11` | `41 FF /2` | `call r11` |
| `jit_jmp_reg` | `[41] FF /4` | `jmp r11` |
| `jit_ret` | `C3` | `ret` |
| `jit_mov_reg_reg` ほか `jit_emit_reg_reg_op` | `REX.W 89/09/01/29/39/85 /r` | `mov rcx, rax` / `or` / `add` / `sub` / `cmp` / `test` |
| `jit_lea_reg_rsp` | `REX.W 8D /r [rsp+disp32]` | `lea rcx, [rsp+0x48]` |
| `jit_mov_reg_from_rsp` / `_rsp_from_reg` | `REX.W 8B,89 /r [rsp+disp32]` | `mov rax, [rsp+0x48]` |
| `jit_mov_reg_from_mem_disp8` / `_mem_disp8_from_reg` | `REX.W 8B,89 /r [base+disp8]` | `mov r11, [r10+0x8]` |
| `jit_emit_je/jne/js/jb_rel32_placeholder` | `0F 84/85/88/82 rel32` | `je 0x4c` |
| `jit_emit_jmp_rel32_placeholder` | `E9 rel32` | `jmp 0x4c` |
| `za_emit_symbol_name` の埋め込み文字列 | (命令ではない) | `.asciz "CAR"` |

汎用性のため、ModRM/SIB/disp の**長さ計算は完全に一般形**で実装してある
(mod=00/01/10/11、rm=100 の SIB、rm=101 の RIP相対、REX.B/X/R の拡張)。
`za.c` が今後 `[reg+reg*8]` のようなアドレッシングを出しても、少なくとも
命令長は正しく求まり、表示も正しく出る。

上表に無い数命令(`xor`, `call rel32`, `jcc rel8`, `jmp rel8`, `nop`, `int3`,
`mov r/m64, imm32`)も、ほぼ同じ表で書けるため対応を入れてある。

### Phase 1 で未実装

- AVX / SSE / x87(`za.c` は浮動小数点を `primitive_*` の call に落とすので出力しない)
- `LOCK` / `REP` などのプレフィックス、セグメントオーバーライド
- オペランドサイズプレフィックス `66`、アドレスサイズプレフィックス `67`
- 上記以外のopcode → `(bad)` として1バイト進める(表示にそう出る)

### 出力記法

**Intel記法**(`mov rcx, rax` = dst, src の順)。指示書 1章の要件に従う。
指示書 3-3 の出力例は `mov %rsp, %rbp` と AT&T のレジスタ接頭辞 `%` が付いた
形になっているが、同じ例の中で Intel のオペランド順になっており混在しているため、
明示的な要件(「Intel記法」)のほうを採った。

分岐先は**関数先頭からのオフセット**で表示する(`je 0x4c`)。同じ行の offset 列と
直接照合できるため、絶対アドレスより読みやすい。コード範囲の外へ飛ぶ場合
(共有トランポリンへの末尾呼び出し `jmp`)はオフセットとして出すと嘘になるため、
`jmp .+0xf4c85b ; outside` のようにその場からの相対変位で示す。

### 埋め込み文字列の扱い(ヒューリスティック)

`za_emit_symbol_name`(`za.c:2017`)は、関数名の解決に使う NUL終端文字列を
**コードストリームの中**に置き、その手前に `jmp rel32` を出して実行時に飛び越える。
素朴な linear sweep はこれを命令として読んでしまい、以降のデコードが全部ずれる。

そこで `os_disasm_item` は各オフセットで「自分は埋め込み文字列の先頭か」を
**ステートレスに**判定する:

1. `offset >= 5` かつ `code[offset-5] == 0xE9`(直前が `jmp rel32`)
2. その rel32 の値 `L` が `1 <= L <= 256` で、`offset + L <= code_len`
3. `code[offset .. offset+L-2]` がすべて表示可能ASCII(0x20〜0x7E)
4. `code[offset+L-1] == 0`(NUL終端)

4条件すべてを満たすときだけ `.asciz` として `L` バイトまとめて1項目にする。
`if` の then節末尾にある「else節を飛び越す `jmp`」が誤爆する可能性は、
条件3(機械語がすべて表示可能ASCIIになること)がほぼ起こり得ないため無視できる。

## 今後の拡張可能性

- 命令表は `disasm.c` 内の単純な `switch` と小さなテーブルなので、
  `za.c` に新しい `jit_*` ヘルパーを足したときの追従は1〜2箇所で済む。
- 案Aへ乗り換える意味があるのは「ホスト側のツールでコードをダンプして解析する」
  用途の場合だけで、その経路には既に `%%DIAG-ZA-CODE-BYTE` による生dumpがある
  (ホストで `objdump`/Capstone に食わせればよい)。ゲスト内で完結させるという
  本機能の目的とは棲み分ける。

---

## 性能の実測(2026-09-14)

### デコーダ単体(ホスト、gcc -O1)

`za.c` が実際に出す並び(プロローグ+`movabs`+`call r11`+スロット参照+分岐)を
繰り返した合成コードを、`os_disasm_item` + `os_disasm_format_line` で端から端まで
1回走査するのにかかる時間。

| コード長 | 1走査あたり |
|---|---|
| 256 B | 0.015 ms |
| 815 B (`(defun f (a b) (+ a b))` の実サイズ) | 0.046 ms |
| 1419 B (他関数を1回呼ぶ関数の実サイズ) | 0.081 ms |
| 4 KB | 0.23 ms |
| 16 KB | 0.90 ms |
| 64 KB | 3.6 ms |

指示書の目標(小規模関数 <1ms、中規模 <10ms)に対して、デコーダ自体は
**1〜2桁の余裕**がある。コード長にきれいに線形で、1命令あたり約0.3µs。

### ゲスト内の end-to-end(QEMU/TCG、KVM無し)

`(disassemble-to-list 'f)` を200回繰り返した実測(PIT tick=10ms)。
Lispラッパー(`disasm-items` / `disassemble-to-list`)はいずれもJITコンパイル済み
であることを `%%ZA-COMPILED-P` で確認した上で測っている。

| 対象 | コード長 | 項目数 | 1回あたり |
|---|---|---|---|
| `(defun f (a b) (+ a b))` | 815 B | 145 | 8.2 ms |
| `(defun g (x) (f x 1))` | 1421 B | 249 | 16.2 ms |

1項目あたり約60µsで、**その大半はデコードではなくLisp層**である
(`%%DISASM-ITEM` は1項目ごとに文字列3つとconsセル6つを確保する)。
デコーダ自体の取り分は上表から1項目あたり0.3µs程度で、0.5%に満たない。

測定環境にKVMが無くQEMUはTCG(ソフトウェアエミュレーション)で走るため、
実機ではこれより桁で速くなる。逆に言えば、**この数字は上限として読むべきもの**で、
「デコーダを速くしても end-to-end はほとんど変わらない」という結論のほうが重要である。
速くする必要が出た場合に効くのは、1項目ごとのLisp値の組み立てをやめて
C側でストリームへ直接書く経路(`disassemble` を `%%DISASM-ITEM` 経由にしない)であり、
デコーダの最適化ではない。

### メモリ安全性

`valgrind --leak-check=full` を `test/c/disasm_test.c` に対して実行:

```
in use at exit: 0 bytes in 0 blocks
total heap usage: 1 allocs, 1 frees, 4,096 bytes allocated   (stdioのバッファのみ)
ERROR SUMMARY: 0 errors from 0 contexts
```

デコーダは確保を一切行わない(呼び出し側が渡した `os_disasm_insn_t` にだけ書く)ため、
リークしうる資源がそもそも無い。指示書 3-1 のAPIは `za_disasm_result_t *` を
malloc して `za_disasm_free` で解放する形だったが、本リポジトリには `malloc`/`free` が
存在しない(フリースタンディング、GCヒープと Immobilized Space しかない)ため、
「1項目ずつ呼び出し側のバッファへ書く」形に変えてある。
