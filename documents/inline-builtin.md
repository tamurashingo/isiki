# builtin のインライン化(Phase 3)

> 作成日: 2026-09-16 / ブランチ: `feature/inline-optimization`(分岐元 `feature/compiler-optimization` `a2051f7`)
>
> `(declaim (inline car cdr null eq))` で指定された builtin を、`call` ではなく
> 命令列として直接展開する。**既定は無効**で、宣言が無ければ従来どおり `call` を出す。

## 調査結果

### 4-1. 現在の builtin 呼び出し

**指示書の前提は部分的に古かった。** `car` / `cdr` / `null` / `eq` は既に za.c で
特別扱いされており、**汎用コールサイト(1本 762 byte、
`documents/measurement-callsite-breakdown.md`)ではなく専用ヘルパーへの直接 call**
に落ちている。

| builtin | 落ちる先 | 経由する emitter |
|---|---|---|
| `car` | `os_car_checked(x, env)` | `za_compile_unary_env` |
| `cdr` | `os_cdr_checked(x, env)` | `za_compile_unary_env` |
| `null` / `not` | `primitive_null1(a)` | `za_compile_unary` |
| `eq` | `primitive_eq2(a, b)` | `za_compile_binary` |

実測したコードサイズ(`%%DISASM-CODE-LEN`、関数全体の byte 数):

| 関数 | byte | 素の関数との差 |
|---|---|---|
| `(defun f (x) x)` | 452 | — |
| `(defun f (x) (car x))` | 541 | **+89** |
| `(defun f (x) (cdr x))` | 541 | +89 |
| `(defun f (x) (null x))` | 533 | +81 |
| `(defun f (x y) (eq x y))` | 770 | — |
| `(defun f (a b) (+ a b))` | 815 | — |
| `(defun f (x) (car (cdr (car (cdr x)))))` | 808 | +356(89 × 4) |

`(car x)` の 89 byte の内訳(逆アセンブルで確認):

| 部分 | byte |
|---|---|
| 引数のロード | 8 |
| **オペランドの制御転送チェック**(`os_is_control_transfer` 呼び出し + 判定) | 59 |
| **`os_car_checked` の呼び出し**(`mov rcx,rax` / `mov rdx,env` / `movabs` / `sub+call+add`) | 35 |

**インライン展開が置き換えられるのは後者の 35 byte だけ**で、制御転送チェックは
オペランド評価側の都合なので残る。

### 4-2. inline 指定の保持方法

**Phase 2 の declaim 値(10 番目のスロットの fixnum)へ相乗りさせた。**
environment にスロットを増やさずに済み、`za_try_compile_defun` へ渡す値も 1 つのまま
でよい。

```
fixnum のマグニチュード:
  bit 0-1  speed
  bit 2-3  safety
  bit 4-5  space
  bit 8-39 inline ビット(32 個分。現在は car/cdr/null/eq の 4 つ)
```

fixnum は即値なので **GC ルートが増えず、走査コストも乗らない**。
リスト(Lisp オブジェクト)で持つ案は採らなかった。
ユーザー定義関数のインライン化を入れる段階では名前の集合が必要になるが、
それは本 Phase の対象外であり、そのときに拡張すればよい。

コンパイラ側は `g_za_declaim`(file-static、`za_try_compile_defun` 冒頭で owner から
読んだ値を入れる)を見る。emitter へ引数で持ち回ると署名が広範囲に波及するため、
`g_jit_overflow` 等と同じくコンパイル単位の file-static にしている。

### 4-3. タグ表現とオブジェクト配置

`src/c/runtime.h` の定義と `cc_car` / `os_car_checked` の実装を読んで確認した。
**推測では書いていない。**

| 項目 | 内容 |
|---|---|
| `TAG_MASK` | `0x7`(下位 3bit) |
| `TAG_CONS` | `0x1` |
| cons のレイアウト | タグを外したアドレスの **+0 が car、+8 が cdr** |
| タグの剥がし方 | `and rax, -8`(= `~7`) |
| `nil` | `g_nil_cell` のアドレス `\| TAG_CONS`。**car も cdr も自分自身を指す自己参照 cons** |
| 真 | `g_sym_t`(シンボル `T`) |
| 偽 | `nil` |

**`primitive_car` は単なるタグ剥がし + オフセット読みではない。**

```c
lisp_val_t os_car_checked(lisp_val_t x, lisp_val_t env) {
    if (x == nil || (x & TAG_MASK) != TAG_CONS) {
        return signal_domain_error_for_class(x, "<CONS>", env);
    }
    return cc_car(x);
}
```

ISLisp 仕様 §21.2 により、cons でない引数(nil を含む)は `<domain-error>` になる。
**`nil` は自己参照 cons なのでタグ検査だけでは弾けず、読めてしまう**
(`(car nil)` が domain-error ではなく `nil` を返すようになる)。
したがって展開には `nil` との比較が別途要る。

### 4-4. GC との関係

**`car` / `cdr` / `null` / `eq` はいずれもアロケーションしないので GC を起こさない。**

- `cc_car` / `cc_cdr` はメモリを読むだけ
- `primitive_null1` は `a == nil ? g_sym_t : nil`、`primitive_eq2` は `a == b ? ...`。
  どちらも比較のみ
- `nil` は `g_nil_cell` という **From/To 空間の外の固定領域**にあり移動しない
  (`os_bootstrap`)。したがって値を即値として焼き込んでよい。
  既存コードも `jit_movabs_rax(nil)` を多数使っている
- **`g_sym_t` は GC ヒープ上にあり移動する**(`os_gc_collect_body` が
  `g_sym_t = gc_copy_value(g_sym_t)` で更新する、`runtime.c:2472`)。
  **値を即値として焼き込んではならない。**
  グローバル変数のアドレスを `movabs` して deref する

### [原則8] 実際に踏んだ

最初の実装で `jit_movabs_rax(g_sym_t)` と書いた。GC が 1 回走った時点で
生成コードが旧 From 空間のアドレスを返すようになり、呼び出し元がゴミを掴んだ。
**print がその壊れたオブジェクトを辿って test-results.txt に 50MB のバイナリを吐いた。**

`documents/pitfalls.md` 原則8(生成コードに焼き込まれたアドレスを守る機構は存在しない)
そのものである。`g_sym_t` を即値で焼いているのは za.c 全体でこの 1 箇所だけで、
**既存コードには無かった**(`nil` を焼いている箇所は多数あるが、上記のとおり安全)。

修正後は次の形にしている(+3 byte)。

```
movabs rax, &g_sym_t
mov    rax, [rax]
```

GC 監査の `%%ZA-HEAP-IMM-COUNT`(生成コードに GC 対象アドレスが焼き込まれていないかを
コンパイルのたびに数える、`za.c`)がこれを検出する立場にあるので、
**回帰テストでその値が 0 であることを確認している。**

展開後に中間値が GC から見えなくなる窓は生じない。展開する命令列の中に
アロケーションが 1 つも無いためで、これは展開前のヘルパー呼び出しでも同じだった。

## 実装

### 展開する命令列

**`car` / `cdr`** — `os_car_checked` / `os_cdr_checked` と等価。
判定を外れたら従来と同じヘルパー呼び出しへ落ちるので、**エラー経路の振る舞いは
1 命令も変わらない**。

```
    mov  r10, rax
    and  r10, 7
    cmp  r10, 1            ; TAG_CONS
    jne  slow
    movabs r11, <nil>      ; nil は TAG_CONS を持つので別途弾く
    cmp  rax, r11
    je   slow
    and  rax, -8           ; タグ剥がし
    mov  rax, [rax + 0]    ; car。cdr は +8
    jmp  done
  slow:
    mov  rcx, rax
    mov  rdx, [rsp+env]
    movabs r11, os_car_checked
    sub rsp,0x20 / call r11 / add rsp,0x20
  done:
```

**`null`** — `primitive_null1`(`a == nil ? t : nil`)と等価。

```
    movabs r11, <nil>
    cmp  rax, r11
    je   is_true
    movabs rax, <nil>
    jmp  done
  is_true:
    movabs rax, &g_sym_t   ; g_sym_tは移動するので値ではなくアドレスを焼く
    mov    rax, [rax]
  done:
```

**`eq`** — `primitive_eq2`(`a == b ? t : nil`)と等価。
`za_compile_binary` が rcx / rdx にオペランドを揃えた後なのでそのまま比較する。

```
    cmp  rcx, rdx
    (以下 null と同じ真偽値生成)
```

**分岐で組んでいる。`cmov` は使っていない**(指示書 5-2「まず分岐で正しく動くものを
作り、必要なら後で `cmov` にする」)。

## コードサイズ(6-5)

| 式 | notinline | inline | 増分 |
|---|---|---|---|
| `(car x)` | 541 | 590 | **+49** |
| `(cdr x)` | 541 | 590 | **+49** |
| `(null x)` | 533 | 557 | **+24** |
| `(eq x y)` | 770 | 787 | **+17** |
| `(car (cdr (car (cdr x))))` | 808 | 1004 | **+196**(49 × 4) |

**インライン展開はコードサイズを増やす。** 呼び出し 1 箇所あたり `car`/`cdr` で +49、
`null` で +24、`eq` で +17 byte。

理由は 4-1 のとおり、**置き換える対象が既に 35 byte 程度の軽い直接 call** である一方、
展開側は 64bit 即値の materialize(`movabs` が 10 byte)と分岐(rel32 の `jcc` が
6 byte)を要するため。`car` の場合、`nil` との比較だけで 10 + 3 + 6 = 19 byte かかる。

**縮める余地(本 Phase では未実施)**:

- `nil` / `g_sym_t` のアドレスは実測で 2^31 未満(`0x0DC4F031` 等)なので、
  `cmp rax, imm32`(6 byte)で済ませられる。`movabs` + `cmp` の 13 byte が 6 byte になる。
  ただしアドレスが 2^31 未満であることは**保証されていない**ので、
  即値が収まるときだけ短い形を出す分岐が要る
- rel8 の `jcc`(2 byte)を使えば分岐 1 つにつき 4 byte 縮む。
  ただし za.c は現在 **rel8 を出す emitter を 1 つも持っていない**
  (`documents/known-issue-deep-if-nesting-jit.md` の調査で確認済み)
- `cmov` を使えば `null` / `eq` の分岐 2 つが消える

**Phase 4 以降で `space` 宣言を導入するかの判断材料になる。**

## 本 Phase で実装しなかったもの

| 項目 | 理由 |
|---|---|
| `+` / `-` 等の算術 | fixnum / bignum の分岐が必要。Phase 4(型導入)で `the fixnum` 等が使えるようになってから |
| `cons` 等のアロケーション | GC との兼ね合いで難易度が上がる |
| ユーザー定義関数のインライン化 | 再定義時の扱い(展開先が再コンパイルされない)を別途検討する |
| `safety` による型チェック省略 | 現在そもそも型チェックを出していない。例外機構が無く検証できない |
| `speed` の利用 | 自然な用途がユーザー定義関数の自動展開であり、上記とセット |
| 型の導入(`fixnum` / `bignum`) | Phase 4 の本体 |

## Phase 4 への引き継ぎ

本 Phase で作った枠組みは算術にそのまま使える。

- 判定 → 速い path → 外れたら従来のヘルパー呼び出し、という形は
  `(+ a b)` で「両方 fixnum なら `add`、そうでなければ `primitive_add2`」と同型
- `g_za_declaim` に型情報のビットを足す余地がある(bit 40 以降が空いている)
- **ただしコードサイズの増分は算術のほうが大きくなる見込み**。
  オーバーフロー検査(`jo`)とタグ操作が加わるため
