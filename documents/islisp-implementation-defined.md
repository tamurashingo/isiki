# isiki-os の実装定義の挙動(エラーと脱出)

ISLisp 仕様が「未定義」「実装定義」としている点、あるいは**仕様に記述が無い**点について、
isiki-os が採った挙動をここにまとめる。**仕様そのものの挙動と誤解しないための一覧**である。

対象は `feature/error-unwind`(P0〜P6)で決めたもの。仕様の引用は
`documents/error-unwind-survey.md` と同じ `spec:NNNN`(ISLisp Working Draft 23.0 を
`pdftotext -layout` で起こしたテキストの行番号)。

---

## 1. 脱出の設計

### 1-1. ハンドラが非継続コンディションで正常に戻ったときの行き先

**その `with-handler` まで戻り、ハンドラの戻り値が `with-handler` 式の値になる。**

spec:6929 は「The consequences are undefined if the handler returns normally」で、
**未定義**である。以前は `%abort-top-level` でトップレベルまで飛んでいたが、
外側の `with-handler` も `block` も飛び越えるため利用者から見て驚きが大きかった。

外側のハンドラへ譲りたい場合は、ハンドラ内で `signal-condition` を明示的に呼ぶ
(spec:6932「A handler may defer to previously established handlers by calling
`signal-condition`」)。

脱出タグは**動的な出現ごとに実行時 `gensym`** で作る。マクロ展開時に 1 つ焼き込むと、
同じ `with-handler` フォームへ再帰的に入ったときに内側の `catch` が
外側向けの `throw` を捕まえてしまう。

### 1-2. ハンドラが無い場合

- **非継続**: トップレベルへ abort する(`%abort-top-level` → `(return-from %top-level condition)`)。
  spec:6912 が「such as return to toplevel」を実装定義の選択肢として明示的に許している
- **継続可能**: **`nil` を返して続行する。** spec は「active handler は system が必ず 1 つ
  確立している」(spec:6910)前提で「ハンドラが無い」状況を記述していない。**仕様未規定**

### 1-3. 脱出の機構

**`setjmp` / `longjmp` は使わない。** 戻り値に埋めた制御転送値
(`MAGIC_BLOCK_EXIT` / `MAGIC_CATCH_EXIT` / `MAGIC_GO_EXIT`)のバケツリレーに相乗りする。
`GC_PROTECT` が `__attribute__((cleanup))` であること、動的束縛の復元が
`unwind-protect` に依存していることから、この方式以外は採れない。

### 1-4. エラーで打ち切られたフォームの environment

**打ち切られたフォームの中で行われた `switch-environment` だけを巻き戻す。**
正常終了したフォームの `switch-environment` は従来どおり有効なままにする。
REPL(`os_repl_step`)と `load`(`cc_load`)の両方で行う。

`switch-environment` は `proc->env` を恒久的に書き換えるプリミティブで、
`with-environment` と違って `unwind-protect` で戻さない。やりかけのまま次のフォームへ
持ち越さないための措置である。仕様に `switch-environment` は無い(実装独自)。

### 1-5. 条件システムが使えないときのフォールバック

`os_signal_condition` は、**条件システムが使えないと判断したら `EVAL-ERROR` シンボルを
返す**(signal しない)。判断の条件は次のいずれか。

- `make-instance` / `signal-condition` が未定義(`init.lisp` も AOT フォームも走っていない)
- **クラスが引けない**(AOT フォームは走っているが `init.lisp` を読んでいない起動。
  条件クラスは `init.lisp` の `defclass` で登録されるため 1 つも無い)
- 再入の深さが `SIGNAL_MAX_DEPTH`(16)に達した(最後の砦。通常は発動しない)

2 つめが無いと `length` → signal → `make-instance` → `%%class-slots` → `length` の
無限再帰でスタックを食い潰す。回帰は
`test/lisp/qemu_boot_no_init_signal.lisp`(**`init.lisp` を読まない唯一の boot-entry**)。

---

## 2. コンディションのクラス選択(仕様に error-id が無いもの)

| 箇所 | 採ったクラス | 仕様の状況 |
|---|---|---|
| `aref` / `set-aref` の範囲外 | `<program-error>` | 仕様が挙げる誤りは「basic-array でない」「添字が非負整数でない」の 2 つだけで、「非負整数だが次元より大きい」は**書かれていない**(spec:5049-5056、`set-aref` は spec:5085) |
| `string-elt` の範囲外 | `<program-error>` | **`string-elt` は ISLisp 仕様に無い**(実装独自)。文字列に対する `elt` に揃えた |
| `/` のゼロ除算 | `<division-by-zero>` | **`/` は ISLisp 仕様に無い**(仕様にあるのは `quotient`)。`quotient`(spec:4365)と `div`(spec:4889)に揃えた |
| `open-input-file` / `open-output-file` / `open-io-file` が開けない | `<simple-error>` | 仕様が挙げる誤りは「filename が文字列でない」だけで、開く操作は "implementation-defined"(spec:6266-6268)。**開けなかった場合の error-id は無い** |
| `load` の失敗(開けない / 構文エラー / I/O エラー) | `<simple-error>` | **`load` は ISLisp 仕様に無い**(実装独自) |
| `#na` 配列リテラルの構造が壊れている | 読み取りエラー(reader が `g_sym_read_error` へ翻訳する。条件は signal しない) | 書き方は定めるが誤り時の error-id は挙げていない(spec:811-818 / spec:5584-5590) |
| `div` / `mod` に非整数 | **未対応**(黙って 0 扱いのまま) | 仕様が挙げるのは `division-by-zero` だけ(spec:4888-4889)。`gcd` / `lcm` は非整数に domain-error を定めている(spec:4935)ので、そちらと扱いが違う |

`<stream-error>` を入出力の失敗に使わなかった理由: スロットが `stream` ただ 1 つで、
仕様が「the stream on which the error occurred」(spec:7146-7147)と定めている。
**開く操作そのものが失敗しているので載せられるストリームが存在しない。**
`<simple-error>` なら失敗したパスと下位層のメッセージをそのまま運べる。
`format-string` は `"~A"` 固定にして本文は `format-arguments` 側へ渡す
(**パスに `~` が含まれていても指示子として解釈されないようにするため**)。

---

## 3. スロットの中身が仕様と違うもの

| スロット | 入れている値 | 仕様 |
|---|---|---|
| `<arithmetic-error>` の `operation` | **シンボル**(`DIV` / `MOD` / `/`) | 型は `<function>`。`init.lisp` の `%quotient2` が以前からシンボルを入れており、`report-condition` の `~A` も関数オブジェクトでは `#<FUNCTION-BUILTIN>` としか出ない。既存の形に合わせた |
| `<domain-error>` の `expected-class`(シーケンス系) | **受け付けるクラスの一方**(`<BASIC-VECTOR>`) | 仕様の「basic-vector でも list でもない」は 1 つのクラスで表せない(**ISLisp に `<sequence>` クラスは無い**) |
| `<undefined-entity>` の `name`(JIT 経由) | **nil** | JIT は Function Cell 経由で呼ぶため名前が実行時まで運ばれてこない。`namespace` は `function` が正しく入る。名前を運ぶには呼び出し規約の変更が要る |

### スロットを持たないクラスで詳細を運べないもの

`<program-error>` はスロットを持たない。したがって次の情報は **condition に載らない**。

- `index-out-of-range`: どのシーケンスのどの添字だったか(§29.4 spec:7249-7252)
- `arity-error`: 期待した個数と実際の個数(§29.4 spec:7191-7194)
- `immutable-binding`: どのシンボルだったか(§29.4 spec:7239-7241)

専用クラス(`<index-out-of-range>` 等)は仕様のクラス階層(spec:994-1010)に無い。
**仕様に無いクラスを増やさない**方針で通している。

---

## 4. 検出しないことにしたもの

| 箇所 | 理由 |
|---|---|
| マクロ展開時の arity 不一致 | 展開は評価より前の段階で、ここで signal すると「展開中のエラー」で評価が止まる。行き先の設計が評価中の arity error とは別 |
| ネイティブプリミティブの arity(`(car)` が nil を返す等) | 数百箇所あり、`bind_params` を直しても閉まらない |
| `setq` が未束縛の変数に新しい束縛を作る | spec:2167-2169「setq can be used only for modifying bindings, and not for establishing a variable」に反するが未修正。issue で追っている |
| `os_get_variable`(内部呼び出し用)が未束縛で nil を返す | `process.c` / `interrupt.c` が `*RUN-QUEUE*` / `*CURRENT-PROCESS*` の「まだ束縛されていない」を nil で判定している。**`interrupt.c` の呼び出しは割り込みハンドラの中**なので、そこから Lisp の `make-instance` を呼ぶわけにはいかない。ISLisp の変数参照は `os_get_variable_checked` を使う |

---

## 5. 仕様に無い言語機能

次はいずれも **ISLisp 仕様に存在しない**実装独自の拡張である
(`tmp/islisp-spec.txt` を grep して 0 件)。

- `declare`(型宣言)、`declaim`、`optimize`(`speed` / `safety` / `space`)
- `switch-environment` / `with-environment` / `make-environment`
- `load`、`/`、`string-elt`、`open-input-stream`

**`safety 0` は仕様違反モードである。** 仕様は spec:4283-4284 等で「An error shall be
signaled」を**無条件に**要求しているので、検査を外すのは「宣言した人の責任」という
位置づけになる(`documents/declare-typed-add.md` が `declare` の型宣言について
「CommonLisp の `(safety 0)` と同じ立場」と書いているのと同じ)。
