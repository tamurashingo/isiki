# 測定: JIT コールサイト 1 本あたりの生成コード量の内訳

> 測定日: 2026-09-15 / ブランチ: `feature/compiler-optimization`(main 取り込み後、`6da82cb`)
> 目的: 間接参照セル方式(フェーズ3)の判断材料。どこに何 byte 使っているかを出す。

## 結論

**1 コールサイト = 762 byte。** 内訳の上位2つは
**GC の link/unlink が 233 byte(30.6%)**、**関数解決が 185 byte(24.3%)**。

間接参照セルが効くのは関数解決の一部で、**最大 126 byte(16.5%)**。
GC link/unlink のほうが大きい。

## 測定方法

`(defun dN (x) (progn (h x) ... (h x) 0))` の形で呼び出しを 0〜5 本並べ、
`%%DIAG-ZA-CODE-LEN` で生成コード長を測った。末尾の `0` によって全ての `(h x)` が
非末尾呼び出しになる。

| 呼び出し本数 | 生成コード長 | 差分 |
|---|---|---|
| 0 | 519 | — |
| 1 | 1,273 | +754 |
| 2 | 2,092 | +819 |
| 3 | 2,911 | +819 |
| 4 | 3,730 | +819 |
| 5 | 4,549 | +819 |

さらに `disassemble` で d1 / d2 を逆アセンブルし、境界を直接読んだ。結果、
コード量は次のモデルで**全6点が誤差ゼロ**に一致する:

```
390(prologue 2本) + 762 × n(コールサイト) + 57 × (n-1)(progn の中間値破棄) + 121(関数末尾)
```

**差分に出る 819 は 762 + 57 で、57 は `progn` が中間結果を捨てるための
制御転送チェックである。** 呼び出しそのものの費用ではない。
d1 では唯一のコールサイトが 762 byte、d2 では 1 本目が 819(= 762 + 57 の破棄)、
2 本目が 762 byte だった。

呼び出し先の特定は、逆アセンブル結果に出る `call` のターゲットアドレスと
`x86_64-w64-mingw32-objdump -t esp_dir/EFI/BOOT/BOOTX64.EFI` のシンボル表を
突き合わせて行った(image base = 0xc7a6000 と判明し、8件すべて一致)。

| 実行時アドレス | シンボル |
|---|---|
| 0xc7ac0c5 | `os_make_cons` |
| 0xc7ac311 | `os_make_symbol` |
| 0xc7b0bec | `os_get_function_cell` |
| 0xc7b0cc4 | `os_apply_via_cell` |
| 0xc7b61d0 | `os_is_control_transfer` |
| 0xc7b68db | `za_gc_current_head` |
| 0xc7b68fb | `za_gc_link` |
| 0xc7b693a | `za_gc_unlink` |

## 内訳(1 コールサイト = 762 byte)

対象は `(h x)` 1 引数・非末尾。d1 の 0x186〜0x480。
**分類は範囲で隙間なく敷き詰めてあり、合計は 762 byte ちょうど**になる。

| 分類 | byte | 割合 |
|---|---|---|
| **GC link/unlink** | **233** | **30.6%** |
| **関数解決** | **185** | **24.3%** |
| dual-entry 高速パス判定 | 115 | 15.1% |
| 制御転送チェック | 88 | 11.5% |
| 実際の制御転送(call 2本) | 72 | 9.4% |
| 引数リスト構築 | 55 | 7.2% |
| 引数の評価・退避 | 14 | 1.8% |
| 合計 | 762 | 100% |

### 明細

| 範囲 | byte | 分類 | 内容 |
|---|---|---|---|
| 0x186–0x1a3 | 29 | GC | `za_gc_current_head` で現在のルート先頭を退避 |
| 0x1a3–0x1b1 | 14 | 引数 | 引数 `x` のロード |
| 0x1b1–0x1e1 | 48 | 制御転送 | `os_is_control_transfer` + 判定 |
| 0x1e1–0x216 | 53 | GC | 引数値を `za_gc_link` |
| 0x216–0x23e | 40 | 制御転送 | 脱出パス(unlink して関数末尾へ) |
| 0x23e–0x25f | 33 | 関数解決 | cell キャッシュの高速パス判定 + 埋め込みシンボル名 |
| 0x25f–0x2bc | 93 | 関数解決 | 初回解決 `os_make_symbol`→`os_get_function_cell`→キャッシュ書込 |
| 0x2bc–0x2e9 | 45 | GC | cell を `za_gc_link` |
| 0x2e9–0x35c | 115 | dual-entry | nil / magic / dual-entry / entry非null の**4段判定** |
| 0x35c–0x37c | 32 | GC | 高速パス直前の `za_gc_unlink` |
| 0x37c–0x3b7 | 59 | 関数解決 | 呼び出し先 env(word3)の決定 |
| 0x3b7–0x3d2 | 27 | 制御転送 | 高速パスの `call r11` |
| 0x3d2–0x3e4 | 18 | 引数リスト | nil 終端の用意 |
| 0x3e4–0x409 | 37 | GC | 遅いパスの `za_gc_link` |
| 0x409–0x42e | 37 | 引数リスト | `os_make_cons` |
| 0x42e–0x453 | 37 | GC | 遅いパスの `za_gc_unlink` |
| 0x453–0x480 | 45 | 制御転送 | `os_apply_via_cell` |

## 間接参照セル(フェーズ3)が効く範囲

関数解決 185 byte の内訳は次のとおり。

| 部分 | byte | 間接参照セルで消せるか |
|---|---|---|
| cell キャッシュの高速パス判定 + 埋め込みシンボル名 | 33 | **消せる**(解決済みなら判定が不要) |
| 初回解決(`os_make_symbol`→`os_get_function_cell`→キャッシュ書込) | 93 | **消せる** |
| 呼び出し先 env(word3)の決定 | 59 | 消せない(呼び出しごとに要る) |

**上限 126 byte = 1 コールサイトの 16.5%。**

## 所見

**GC の link/unlink(233 byte、30.6%)が最大である。** 1 コールサイトで
`za_gc_link` が 3 回、`za_gc_unlink` が 3 回、`za_gc_current_head` が 1 回、
合わせて 7 回のヘルパー呼び出しが出ている。関数解決より大きい。

**dual-entry 高速パス判定が 115 byte(15.1%)。** nil 判定 → magic 判定 →
dual-entry 判定 → entry 非null 判定の 4 段を、それぞれ `movabs r11, imm64` +
`cmp` + `jcc` で組んでいる。`movabs` は 10 byte あるので、判定 1 段が 20 byte 前後になる。

**高速パスと遅いパスの両方が常に生成される。** 遅いパス
(`os_make_cons` で引数リストを作って `os_apply_via_cell` を呼ぶ)は
156 byte(20.5%)あり、高速パスが成立する呼び出しでは 1 命令も実行されない。

## 再現手順

```lisp
(load "src/lisp/disassemble.lisp")
(defun h (x) x)
(defun d1 (x) (progn (h x) 0))
(disassemble-to-stream *isiki-test-stream* 'd1)
```

を `test/lisp/` へ置き、`make test-qemu-milestone MILESTONE=<boot-entry>` で読み込ませる。
シンボルの突き合わせは上記 objdump。
