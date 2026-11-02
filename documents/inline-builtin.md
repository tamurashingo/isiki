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

**`null`** — `primitive_null1`(`a == nil ? t : nil`)と等価。**分岐を使わない(`cmov`)。**

```
    movabs r11, <nil>
    cmp    rax, r11        ; ここで立てたフラグをcmovまで壊さないこと
    movabs r10, &g_sym_t   ; g_sym_tは移動するので値ではなくアドレスを焼く
    mov    r10, [r10]      ; movabs も mov r64,[r64] もフラグを変えない
    movabs rax, <nil>      ; 既定値
    cmove  rax, r10        ; 等しければ t
```

**`eq`** — `primitive_eq2`(`a == b ? t : nil`)と等価。
`za_compile_binary` が rcx / rdx にオペランドを揃えた後なのでそのまま比較する。

```
    cmp  rcx, rdx
    (以下 null と同じ真偽値生成)
```

**`cmov` で組んでいる。** 指示書 5-2 は「まず分岐で正しく動くものを作り、必要なら
後で `cmov` にする」としていた。**最初は分岐で組んだが、速度を測ったところ
`null` が呼び出しより 11% 遅く、その「必要なら」に当たったため `cmov` へ変えた**
(下記「速度」)。`car`/`cdr` は元から分岐(cons 判定)が必要なのでそのまま。

## コードサイズ(6-5)

| 式 | notinline | inline | 増分 |
|---|---|---|---|
| `(car x)` | 541 | 590 | **+49** |
| `(cdr x)` | 541 | 590 | **+49** |
| `(null x)` | 533 | 550 | **+17** |
| `(eq x y)` | 770 | 780 | **+10** |
| `(car (cdr (car (cdr x))))` | 808 | 1004 | **+196**(49 × 4) |

**インライン展開はコードサイズを増やす。** 呼び出し 1 箇所あたり `car`/`cdr` で +49、
`null` で +17、`eq` で +10 byte。
(分岐版では `null` +24 / `eq` +17 だった。`cmov` 化で `je` 6 + `jmp` 5 が
`cmov` 4 に置き換わり 7 byte 縮んだ。)

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

**Phase 4 以降で `space` 宣言を導入するかの判断材料になる。**

## 速度(6章に無かったが測った)

`(while (< i k) (progn (setq a (BUILTIN l)) (setq i (+ i 1))))` を 2,000,000 回。
notinline 版と inline 版を**交互に 5 往復**して平均を取る(ドリフトが両者へ等しくかかる)。
tick は 10ms 単位。

| | notinline | inline(`cmov`) | 変化 | 分岐版だったとき |
|---|---|---|---|---|
| `car` | 407.2 | 352.8 | **−13.4%** | 354.4 |
| `cdr` | 346.4 | 320.8 | **−7.4%** | 317.6 |
| `null` | 348.8 | **243.2** | **−30.3%** | 382.4(**+11% 遅い**) |
| `eq` | 352.0 | 320.0 | **−9.1%** | 310.4 |

### 分岐で組んだ最初の版では `null` が遅かった

5 往復すべてで inline 側が遅く(372〜400 vs 340〜348)、レンジが重ならなかった。
**ノイズではない。**

`null` の展開はループ本体に分岐を 2 つ増やす。置き換えた `primitive_null1` は
`a == nil ? t : nil` だけの極小関数なので、call/ret を消した利得より分岐の追加が
上回った。`eq` が同じ真偽値生成なのに遅くならなかったのは、`eq` 側は
`cmp rcx, rdx` で済み 64bit 即値の materialize が要らないためと考えられる**(推測)**。

`cmov` へ変えて **+11% → −30%** に逆転した。`car`/`cdr`/`eq` は誤差の範囲で変わらない
(元から `cmov` の効く経路ではなかった)。

### 引数がリテラルのときは展開が遅い

| | notinline | inline | 変化 |
|---|---|---|---|
| `(null nil)` | 236.8 | 316.8 | **+33.8% 遅い** |

展開側は引数が何であれ **64bit 即値を 2 つ materialize**(`nil` と `&g_sym_t`)し、
さらに `mov r10, [r10]` でメモリを読む。呼び出し版は定数をそのまま `rcx` へ渡して
終わりなので、**リテラル引数では展開が純粋な上乗せになる。**

実コードで `(null nil)` と書くことはまずないので実害は小さいと見ているが、
**「引数が定数のときは展開しない」という判定を入れる余地がある**
(本 Phase では入れていない)。

### 測定条件の注意

**KVM 無しの QEMU(TCG)である。** TCG は分岐でトランスレーションブロックが切れるため、
**分岐のコストが実機より重く出る。** `cmov` 化で `null` が大きく改善したのは、
まさにその特性を突いた形になっている。
**実機では分岐版でもここまでの差は出なかった可能性がある。**
この環境では実機計測ができていない。

なお対照として「builtin を呼ばないループ」も測ったが(272 tick)、
`(setq a l)` と `(setq a (null l))` ではパラメータの読み出し経路が異なるらしく、
**inline 版がこの対照より速いという辻褄の合わない値になった**ため、
上表では対照を引かず notinline と inline の直接比較のみを載せている。

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
- **速度は測ってから決めること。** 本 Phase では `null` の展開が最初 11% 遅く、
  `cmov` へ変えて −30% に逆転した。**展開すれば速くなるとは限らない**

---

## 追記 (inline-arith Phase 2): **`declare` でも効くようになった**

本 Phase では `declaim` だけが `inline` 指定の入口だったが、
算術のインライン化(`documents/inline-arith.md` §7)で
**`za_inline_enabled` がレキシカルスコープを見るようになった副産物として、
`car` / `cdr` / `null` / `eq` も `(declare (inline car))` で展開されるようになった。**

```lisp
(defun f (x) (declare (inline car)) (car x))   ; 展開される(以前は展開されなかった)
```

**意図して作ったものではないが、いま動く。** 依存されてから
「あれは副産物でした」とは言えないので、**仕様として扱い、テストを持たせた**
(`test/lisp/inline_decl_test.lisp` の「既存の car/cdr/null/eq が壊れていないこと」)。

- `declaim` / `declare` のどちらでも展開される
- `declare` は**そのフォームの内側だけ**。`let` の本体に書けば本体だけ
- `let` の**初期化式には効かない**
- `notinline` で打ち消せる(内側が優先)

`%%INLINE-OF` は関数単位、`%%INLINE-HERE` はフォーム単位で現在値を返す。

