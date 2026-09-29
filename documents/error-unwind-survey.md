# 調査: `<error>` 発生時の脱出処理(現状実装)

> 調査日: 2026-09-23
> **これは調査であり実装ではない。** 調査中、追跡対象のファイルは 1 行も変更していない。
> 再現用のハーネスは `tmp/errsurvey/`(gitignore 対象)に置いた。
> 関連: `documents/control-transfer-survey.md`、`documents/jit.md`、`documents/abi-redesign.md`
>
> **行番号について。** 調査そのものは `feature/instcount-gate`(`8ead49d`)の上で行ったが、
> 本書は `feature/compiler-optimization`(`bf68e80`)を起点とする `feature/error-unwind` に置くため、
> **`ファイル:行` はすべて後者に合わせて振り直してある**(184 箇所中 85 箇所が移動した)。
> 両ブランチの差分(inline 宣言 Phase 3・命令数ゲート・マクロ前方参照チェッカ)は
> **条件システムの実装に一切触れていない**ので、本書の内容そのものは変わらない。
> ただし次の 2 点だけは `feature/instcount-gate` 側にしか無く、本書では扱いを分けてある。
> - `tools/check_macro_forward_refs.py` と `make check-macro-order`
> - `test_framework.lisp` の `isiki-file-end-check`(ファイル単位の `#count` 検査)
>
> 生成物(`src/c/lisp_compiled.c`)だけは、再生成のたびに行番号が動くので関数名で指す。
>
> 仕様の引用元は ISLisp Working Draft 23.0 の PDF を `pdftotext -layout` で起こした
> テキストで、行番号は既存ドキュメント(`src/lisp/init.lisp` の「tmp/islisp-spec.txt 994-1010行」等)と
> **同じ採番**であることを確認した(6966 行目が `cerror`、994 行目がクラス階層図)。
> 本文では `spec:NNNN` と書く。

---

## 1. 要約

1. **コンディションを「signal した」経路は、既に正しく脱出している。** `error` /
   `signal-condition` / C の `signal_domain_error` はいずれも `%abort-top-level` →
   `(return-from %top-level condition)` に行き着き、その **`MAGIC_BLOCK_EXIT` が
   インタプリタ・JIT・AOT の 3 経路すべてを正しく遡る**。`unwind-protect` の cleanup も
   内側から順に実行される。実機(QEMU、JIT 有効)で確認した(§3)。
2. **壊れているのは「signal していない」エラーである。** C プリミティブの大半
   (**`return g_sym_eval_error` が 50 箇所**、シンボルの出現は 80 箇所)は、コンディションを作らずシンボル
   `EVAL-ERROR` を**普通の値として返す**。`(list 1 (div 1 0) 2)` は `(1 EVAL-ERROR 2)` になる。
   これが「エラーが値として返り処理が継続する」の本体である。
3. さらに悪い 2 パターンがある。**未束縛変数は `nil` を返す**(`runtime.c:3462` に TODO)、
   **`(+ 1 'a)` は `1` を返す**(型違いを黙って無視)。どちらもエラーの痕跡すら残らない。
4. **`with-environment` は中のエラーを握り潰す。** `%%eval-in-environment` が
   `os_eval_top_level`(= 内側の `block %TOP-LEVEL`)を張るため、`%abort-top-level` が
   そこで止まり、**condition がその式の値になって外側の評価が続く**(§3-3)。
   `<error>` オブジェクトが値として返る唯一の実測ケースはこれである。
5. **非局所脱出は setjmp/longjmp ではない。** `block`/`catch`/`go` は
   「戻り値に埋めた `TAG_INSTANCE` のシグナルをバケツリレーする」方式で、
   伝播は **eval.c 17 箇所 + JIT 25 箇所 + AOT 7,242 箇所**の個別チェックで成り立っている。
   エラー脱出はこの仕組みに**そのまま相乗りできる**(§C-4)。
6. **`GC_PROTECT` は `__attribute__((cleanup))` である**(`runtime.h:1073`)。
   longjmp はこのハンドラを呼ばない。**longjmp を入れるなら `proc->gc_roots` の
   明示的な復元が必須**。幸い shadow stack は単一ポインタ headed なので復元は O(1) で、
   JIT は既に `ZA_OFF_ENV_SAVED_HEAD` + `za_gc_unlink` で同じことをやっている。
7. **動的束縛(`*handlers*`/`dynamic-let`)は「スタック」ではない。** グローバルな
   flat alist を破壊的に書き換え、**復元は Lisp の `unwind-protect` に完全に依存している**
   (`init.lisp:552`、`init.lisp:880`)。したがって **cleanup を飛ばす脱出は実装不可**である
   (これは要件「途中の `unwind-protect` の後始末は必ず実行する」と一致する)。
8. ハンドラが非継続コンディションで正常 return した場合、isiki は**外側のハンドラを飛ばして
   一気にトップレベルへ abort する**(§4-2 #1)。spec は「consequences are undefined」なので
   仕様違反ではないが、実装方針では明示的に決めておく必要がある。
9. トップレベルは既に**フォーム単位**で閉じている(REPL / `load` / テストランナーとも
   `os_eval_top_level` 経由)。要件のうち「フォーム単位で打ち切って次へ」は**既に満たされている**。
10. 残る実装差分は実質 4 つ: **(a) 50 箇所の `g_sym_eval_error` 返しを signal へ寄せる**、
    **(b) 未束縛変数/型違いを検出する**、**(c) 内側 `%TOP-LEVEL` の握り潰しを直す**、
    **(d) トップレベルでの environment 巻き戻し(`repl.c` 1 箇所)**。

---

## 2. 調査結果

### A. エラーの生成と伝播

#### A-1. `<error>` とそのサブクラスを生成している箇所

コンディションは ILOS インスタンス(`MAGIC_CLASS_INSTANCE`)であり、生成口は
**`make-instance` ただ 1 つ**である。専用の C 構造体は無い。

| 層 | 箇所 | 生成するクラス |
|---|---|---|
| **Lisp(直接)** | `init_aot.lisp:762` `error` | `<simple-error>` |
| | `init_aot.lisp:774` `cerror` | `<simple-error>`(continuable) |
| | `init.lisp:652` `%convert-error` | `<domain-error>` |
| | `init.lisp:1002` / `1088` | `<domain-error>`(アクセサの型検査) |
| **Lisp(`(error ...)` 呼び出し)** | `init.lisp` 10 箇所<br>(`275` setf / `413` assure / `800` / `1064`,`1065`,`1072` expt / `1149`,`1167`,`1230` environment)、`disassemble.lisp:45` | `<simple-error>` |
| **C プリミティブ** | `runtime.c:4548` `signal_domain_error_for_class` ← `car`/`cdr`/`set-car`/`set-cdr`(`runtime.c:5770/5783/5790/5798`) | `<domain-error>` |
| | `runtime.c:4566` `signal_domain_error` ← `sqrt`(`7244`,`7253`)/`log`(`7309`) | `<domain-error>` |
| | `runtime.c:4534` `os_signal_control_error` | `<control-error>` |
| | `eval.c:561` `declaim` の optimize 値域外 | `<domain-error>` |
| | `eval.c:879` 死んだ block への `return-from` / `eval.c:905` cleanup 中の脱出衝突 | `<control-error>` |
| | `reader.c:1019` `parse-number` の失敗 | `<parse-error>` |
| | `stream_lisp.c:75` `handle_end_of_stream` ← 入力系 5 箇所(`202/230/408/436/468`) | `<end-of-stream>` |
| **JIT ランタイムヘルパ** | `za.c:6298` — 生成コードが `os_signal_control_error` を `call` する **1 箇所だけ** | `<control-error>` |
| **AOT 生成コード** | `lisp_compiled.c` の `lisp_ll_error` / `lisp_ll_cerror` / `lisp_ll_signal_condition`(`init_aot.lisp` 由来)。**AOT 自身が独自にコンディションを作る箇所は無い** | — |

**C 側で signal する呼び出し箇所は全部で 17 箇所しかない**(ヘルパ 5 種類経由: domain-error 4+3、control-error 3、declaim 1、parse-error 1、end-of-stream 5)。残り(§A-4 の `return g_sym_eval_error` 50 箇所)は
コンディションを作らずに `EVAL-ERROR` を返す。これが本件の中心である。

#### A-2. `error` / `cerror` / `signal-condition` の実装

`src/lisp/init_aot.lisp:736`(AOT 済み。生成物 `lisp_compiled.c` の `lisp_ll_signal_condition__step` に対応する C が出ている。**生成物なので行番号は再生成で変わる**):

```lisp
(defun signal-condition (condition continuable)
  (let ((handlers (dynamic *handlers*)))
    (if (null handlers)
        (if continuable nil (%abort-top-level condition))
        (let ((tag (gensym)))
          (set-slot-value condition '%continuable continuable)
          (set-slot-value condition '%continue-tag tag)
          (catch tag
            (unwind-protect
                (progn
                  (%%set-dynamic '*handlers* (cdr handlers))
                  (let ((result (funcall (car handlers) condition)))
                    (if continuable result (%abort-top-level condition))))
              (%%set-dynamic '*handlers* handlers)))))))
```

返す値は 4 通り:

| 状況 | 戻り値 |
|---|---|
| ハンドラ無し・非継続 | `%abort-top-level` の結果 = **`MAGIC_BLOCK_EXIT(%TOP-LEVEL, condition)`** |
| ハンドラ無し・継続可 | `nil` |
| ハンドラが非局所脱出した | その脱出シグナル(`catch tag` に一致すれば `continue-condition` の値) |
| ハンドラが正常 return・非継続 | **`%abort-top-level`**(= 上と同じ block-exit) |
| ハンドラが正常 return・継続可 | ハンドラの戻り値 |

`%abort-top-level`(`init_aot.lisp:717`)は `(return-from %top-level condition)` のみ。
`error`(`762`)は `<simple-error>` を作って `(signal-condition c nil)`、
`cerror`(`774`)は continuable に「continue-string を format した文字列」を渡す
(spec:6966-6980 の等価定義どおり)。

呼び出し元: Lisp からは §A-1 の表のとおり。C からは `os_signal_condition`
(`runtime.c:4466`)が `os_get_function` で `MAKE-INSTANCE` と `SIGNAL-CONDITION` を
引いて `os_apply_function` で呼び戻す。**init.lisp 未ロード時は `g_sym_eval_error` に
フォールバックする**(`runtime.c:4473`)。

#### A-3. 伝播パターン(`if (is_error(x)) return x;` 相当)

**「エラーを見て上に返す」パターンは存在しない。** 存在するのは
**「制御転送シグナルを見て上に返す」パターン**で、コンディションはその
`MAGIC_BLOCK_EXIT` に相乗りしている。判定は `eval.h:96`:

```c
static inline int os_is_control_transfer(lisp_val_t v) {
    if ((v & TAG_MASK) != TAG_INSTANCE) return 0;
    UINT64 *obj = (UINT64 *)(v & ~TAG_MASK);
    return obj[0] == MAGIC_BLOCK_EXIT || obj[0] == MAGIC_CATCH_EXIT || obj[0] == MAGIC_GO_EXIT;
}
```

箇所数(実測):

| 層 | 箇所数 | 代表例 |
|---|---:|---|
| インタプリタ | **17**(`if (is_control_transfer(...))`) | `eval.c:41,45` `eval_args`、`eval.c:66` `eval_progn`、`eval.c:191` `eval_form` |
| JIT | **25**(`za_emit_ct_check_and_jmp_if_transfer()` の呼び出し) | `za.c:3651` `if` の test、`za.c:4407` 各引数 |
| AOT 生成コード | **7,242**(`os_is_control_transfer` の出現数) | `lisp_compiled.c` の `__call_arg_* ? ... : ...` |
| C プリミティブ(ランタイム) | **3** | `runtime.c:4477,4554,4570` |
| reader.c | **1** | `reader.c:1014` |
| **stream.c / mount.c** | **0** | §F-6 参照 |

**穴: `stream.c` と `mount.c` は Lisp を呼び戻しておきながら一切チェックしていない。**
`stream.c:195` は `os_apply_function(READ-INTO! ...)` の結果を
`os_fixnum_magnitude(result)` にそのまま通す。Lisp 側が脱出すると
block-exit オブジェクトのアドレスをバイト数として読むことになる
(**コード読みのみ。FAT マウントを用意した実機再現はしていない**)。

#### A-4. 伝播されずに処理が継続する具体例

§3 に実行結果つきで全パターンを挙げる。要約すると 4 パターン:

| # | パターン | 例 | 結果 |
|---|---|---|---|
| 1 | C プリミティブが `EVAL-ERROR` を値として返す | `(list 1 (div 1 0) 2)` | `(1 EVAL-ERROR 2)` |
| 2 | 未束縛変数が `nil` になる | `(list 1 undefined-var 2)` | `(1 NIL 2)` |
| 3 | 算術の型違いが黙って無視される | `(list 1 (+ 1 'a) 2)` | `(1 1 2)` |
| 4 | 内側 `%TOP-LEVEL` が condition を握り潰す | `(progn (with-environment e (error "x")) (note 'after))` | `after` まで評価が進む |

`g_sym_eval_error` の分布(実測、`.c` のみ。`return` 文だけを数えた数 / シンボルの総出現数):

| ファイル | `return` 箇所 | 総出現 |
|---|---:|---:|
| `runtime.c` | 31 | 55 |
| `stream_lisp.c` | 12 | 12 |
| `eval.c` | 4 | 8 |
| `load.c` | 3 | 3 |
| `reader.c` | 0 | 2 |
| **合計** | **50** | **80** |

代表的な発生源: 0 除算(`runtime.c:6588`)、`aref`/`elt`/`string-elt` の範囲外
(`9098`,`9291-9353`,`9257`)、`length`(`9270`)、未定義関数呼び出し(`eval.c:188`)、
`(function 未定義)`(`eval.c:473`)、defconstant への `setq`、
`open` 失敗(`stream_lisp.c`)、`load` の構文エラー/オープン失敗(`load.c`)。

#### A-5. condition オブジェクトの内部表現とクラス階層

- **表現**: `os_make_instance(MAGIC_CLASS_INSTANCE, class, slots-vector, 未使用)`
  (`runtime.h:186`、`runtime.c:9541` `%%MAKE-INSTANCE-RAW`)。
  クラスは `MAGIC_STANDARD_CLASS`(`name`, `supers`, `slots`)。
  スロットは `MAGIC_VECTOR` に位置で格納され、名前→添字は
  `%slot-index`(`init_aot.lisp`)が線形探索する。
- **`<condition>` は 2 つの内部スロットを持つ**(`init.lisp:513`):
  `%continuable`(signal 時の continuable 引数)と `%continue-tag`
  (`continue-condition` が `throw` する gensym タグ)。
- **階層**(`init.lisp:513-542`)は spec:994-1010 の図と一致する。ただし
  `<condition>` は **spec に無い実装独自のルート**で、spec 上のルートである
  `<serious-condition>` の親として後方互換のために置かれている(同ファイルのコメントに明記)。
  `<floating-point-overflow>` / `<floating-point-underflow>` は発生源を持たない。
- クラス登録は `defdynamic *classes*`(`init_aot.lisp:348`)への alist。GC ルートは
  `g_dynamic_bindings` 経由(`runtime.c:2992`)。

---

### B. `with-handler` の現状

#### B-1. 実装箇所とハンドラスタック

```lisp
;; src/lisp/init.lisp:546-560
(defdynamic *handlers* nil)
(defmacro with-handler (handler-form &rest body)
  (let ((saved (gensym)))
    `(let ((,saved (dynamic *handlers*)))
       (%%set-dynamic '*handlers* (cons ,handler-form ,saved))
       (unwind-protect (progn ,@body) (%%set-dynamic '*handlers* ,saved)))))
```

- 保持場所は **`g_dynamic_bindings`(`runtime.c:141`)という全プロセス共有の flat alist**
  の `*HANDLERS*` エントリ。`os_set_dynamic`(`runtime.c:4143`)が **既存 pair の cdr を
  破壊的に書き換える**(スタックではない)。
- **GC ルートになっている**: `os_gc_collect_body` が `g_dynamic_bindings` を
  `gc_copy_value` する(`runtime.c:2992`)。
- **復元手段は `unwind-protect` だけ。** C 側にハンドラスタックの概念も保存/復元 API も無い。
  → **cleanup を飛ばす脱出を入れると `*handlers*` が壊れる。**
- `with-handler` はマクロなので常にインタプリタ実行(`main` が defun 以外を AOT しない)。
  ただし**展開後の本体は JIT される**(実機で `probe` 関数の code-base が非 nil であることを確認済み)。

#### B-2. ハンドラが呼ばれる動的文脈

`signal-condition` の中、すなわち **signal した地点の C スタックの上**で
`(funcall (car handlers) condition)` される(§A-2)。スタックは一切巻き戻らない。
`*handlers*` は呼び出しの直前に `(cdr handlers)` へ差し替えられ、`unwind-protect` で
戻される(spec:6920-6922「handler context is re-bound」に相当)。

#### B-3. ハンドラが正常に戻った場合

実機で観測した(§3-4):

| condition | ハンドラの戻り | 現在の挙動 |
|---|---|---|
| 非継続(`error`) | 正常 return | **`%abort-top-level` で一気にトップレベルへ。外側の `with-handler` も `block` も飛ばす** |
| 継続可(`cerror` / `(signal-condition c t)`) | 正常 return | その値が `signal-condition` の戻り値になり、**signal 地点から実行が続く** |

実測(`tmp/errsurvey/` 実機ログ):

```
wh trace after abort = (BODY HANDLER-RAN)        ; body → handler まで走り、after-error は走らない
== B-3b: 外側 handler がいる場合 ==               ; ← 内側ハンドラが正常 return した結果、
                                                 ;   外側 with-handler も probe の block も飛び越えて
                                                 ;   トップレベルへ abort し、この行の後に何も出ていない
```

#### B-4. `continue-condition`

```lisp
;; src/lisp/init_aot.lisp:758
(defun continue-condition (condition &rest value)
  (throw (slot-value condition '%continue-tag) (if value (car value) nil)))
```

`signal-condition` が張った `(catch tag ...)` へ `MAGIC_CATCH_EXIT` で戻る。
実機で `(with-handler (lambda (c) (continue-condition c 99)) (cerror "cont" "wh2"))` が
`99` を返すことを確認した。`condition-continuable`(`753`)は `%continuable` スロットをそのまま返す。

#### B-5. 仕様適合性の判定 → **本書 §4「`with-handler` の仕様適合性の判定」に独立の節として書く。**

#### B-6. 既存のテスト

| ファイル | 件数 |
|---|---:|
| `test/lisp/init_test.lisp` | `with-handler` を含む行 32(§308-540 の Condition System 節、`report-condition`・`ignore-errors`・`cerror`/`continue-condition` を含む) |
| `test/lisp/isiki_test.lisp` | `with-handler` を含む行 6 / `assert-error` 14 件 |
| `test/lisp/isiki_test_jit.lisp` | `with-handler` を含む行 6 / `assert-error` 12 件(同じ仕様例を defun 本体にして JIT に通す版) |
| `test/lisp/test_framework.lisp` | `assert-error` マクロ本体(`:247`) |
| その他 | `convert_float_test` 15 / `inline_builtin_test` 5 / `divide_direct_call_test` 3 / `declaim_test` 2 / `disassemble_test` 2 / `za_test_ext7` 1 |

代表:

```lisp
;; test/lisp/init_test.lisp:311-315
(assert-equal 'caught
  (block b (with-handler (lambda (c) (return-from b 'caught)) (error "boom"))))
;; :330-336 型が合わなければ外側へ渡す
;; :337-342 continuable は handler の戻り値が結果になる
```

---

### C. 既存の非局所脱出の仕組み

#### C-1. 方式

**setjmp/longjmp は使っていない。** `documents/jit.md:16-21` が明記しているとおり、
**「戻り値に埋めた制御転送値を呼び出し元へバケツリレーする」**方式である。

| 形式 | 実装 | シグナル |
|---|---|---|
| `block` / `return-from` | `eval.c:834` / `eval.c:864` | `MAGIC_BLOCK_EXIT`(word1=name, word2=value) |
| `catch` / `throw` | `eval.c:921` / `eval.c:950` | `MAGIC_CATCH_EXIT`(word1=tag, word2=value) |
| `tagbody` / `go` | `eval.c:989` / `eval.c:1032` | `MAGIC_GO_EXIT`(word1=tag) |
| `unwind-protect` | `eval.c:895` | シグナルを作らない(下記) |

`return-from` は **`os_live_block_p`(`runtime.c:4524`)で宛先 block が動的に生きているかを
確認**し、死んでいれば `<control-error>` を signal する(spec §14.7 の例に対応)。
`live_blocks` はプロセスごとのリストで、GC ルートに登録されている(`process.c:404`)。

#### C-2. インタプリタ / JIT / AOT

| | block/return-from | catch/throw | tagbody/go | unwind-protect | live_blocks |
|---|---|---|---|---|---|
| **インタプリタ** | `eval.c:834/864` | `eval.c:921/950` | `eval.c:989/1032` | `eval.c:895` | push/restore する |
| **JIT (`za.c`)** | `za_compile_block`(`za.c:5030-`)/`za_compile_return_from` | `za_compile_catch`/`za_compile_throw` | `za_compile_tagbody`/`za_compile_go`。**同一関数内でスパンを跨がない `go` は直接 `jmp`** に落ちる | `za_compile_unwind_protect`(`za.c:6286-`) | `os_live_block_push/pop` を **call で発行**(`za.c:5006/5021`)。`go` が block を飛び越えるときは**飛び越える段数だけ pop を出す**(`za.c:6506-6506`) |
| **AOT (`transpile.lisp`)** | `transpile-block`(`:1375`)/`transpile-return-from` | `transpile-catch`(`:1479`)/`transpile-throw` | `transpile-tagbody`/`transpile-go`(`:1333`)。届く `go` は C の `goto` | `transpile-unwind-protect`(`:1453`) | **一切扱わない**(`lisp_compiled.c` に `os_live_block` が 0 箇所) |

**AOT は live_blocks を維持しない。** したがって AOT 関数の中の `block` へ
インタプリタ側から `return-from` すると `<control-error>` になる(既知の非対称)。
`%abort-top-level` は AOT 済みだが、`%TOP-LEVEL` block を張っているのは
`eval.c` の `eval_block` なので live 判定は通る。

#### C-3. `unwind-protect` の cleanup が走る契機

**「protected-form の評価から戻ってきたこと」が唯一の契機**である。
`eval.c:895`:

```c
lisp_val_t result = os_eval(protected_form, env);   // 通常値でも脱出シグナルでも必ず戻ってくる
GC_PROTECT(result);
lisp_val_t cleanup_result = eval_progn(cleanup_forms, env);   // 無条件に実行
```

スタックが巻き戻らない方式なので、**どんな脱出でも必ず物理的に unwind-protect の
フレームへ戻ってくる**。そこで cleanup を実行してから `result` をさらに上へ返す。
実機で入れ子の順序も確認した: `note=(INNER-BODY INNER-CLEANUP OUTER-CLEANUP)`(§3-5)。

cleanup 自身が脱出した場合の扱いは 3 実装で食い違う:

- インタプリタ(`eval.c:903-907`): protected も脱出中なら `<control-error>`、
  そうでなければ cleanup の脱出を優先(spec §14.7.2 どおり)。
- AOT(`transpile.lisp:1453` の docstring): **cleanup の脱出は無視して protected の
  結果を優先する**(既知の簡略化と明記)。
- JIT(`za.c:6273-6299`): インタプリタと同じく両方の制御転送を判定している。

#### C-4. エラー脱出を 1 の仕組みに相乗りさせられるか → **所見: させられる。というより既にしている。**

`%abort-top-level` は `return-from %top-level` そのものであり、
**エラー脱出は今日すでに `MAGIC_BLOCK_EXIT` の一種として全経路を通っている。**
実機(JIT 有効)で `error` / `car` の domain-error のいずれも、
JIT 関数・`unwind-protect`・`catch`・`tagbody` ループを正しく貫通して
トップレベルまで届くことを確認した(§3-2)。

したがって **「最も近い `with-handler` まで戻す」も、同じ方式で実装できる**:
`with-handler` が自分用の block(または catch タグ)を張り、
ハンドラが正常 return したらそこへ `return-from` すればよい。
**新しい脱出機構(setjmp/longjmp)は要らない。**

> 相乗りの代償は、`documents/control-transfer-survey.md` が指摘した
> 「45 箇所(eval.c 17 + za.c 25 + AOT 7,242)がそれぞれ独立にチェックしている」
> 不変条件に**もう 1 種類の意味を載せる**ことである。ただし `MAGIC_BLOCK_EXIT` を
> 増やすわけではないので、**チェック箇所は 1 箇所も増えない。**

---

### D. 脱出時に復元が必要になりうる状態

| # | 状態 | 保持場所 | 増減 | 現在の巻き戻し手段 |
|---|---|---|---|---|
| 1 | **GC_PROTECT(ルートスタック)** | `process_t.gc_roots`(`process.h:47`)を先頭とする、**C スタック上の `gc_rootnode` の単方向リスト** | `GC_PROTECT` が push、`gc_unprotect_node`(`runtime.h:1052`)が pop | **`__attribute__((cleanup))` によるスコープ脱出任せ**(`runtime.h:1073-1082`)。JIT だけは別で、プロローグで `za_gc_current_head()` を `ZA_OFF_ENV_SAVED_HEAD` に保存し、3 つの出口で `za_gc_unlink(saved)` により**まとめて復元**する(`za.c:6703`,`6826`) |
| 2 | **dynamic 束縛** | `g_dynamic_bindings`(`runtime.c:141`)= 全プロセス共有の flat alist。`os_set_dynamic` が**既存 pair を破壊的に上書き**(`runtime.c:4143`) | `dynamic-let`(`init.lisp:880`)/`with-handler`(`init.lisp:552`)が「保存 → `%%set-dynamic` → `unwind-protect` で戻す」 | **Lisp の `unwind-protect` のみ。C 側に保存/復元 API は無い** |
| 3 | **現在の environment / frame** | 2 つある。**(a) レキシカル env**: `os_eval`/JIT/AOT に引数として渡されるだけで、グローバルな「現在値」は持たない。**(b) `proc->env`**(`process.h:39`)= ブックキーピング用の「現在の環境」。GC ルート(`process.c:402` で `os_gc_register_root`) | `switch-environment`(`init.lisp:1163`)→ `%%set-current-environment`(`runtime.c:8533`)が `proc->env` を**恒久的に**書き換える。`with-environment`(`init.lisp:1191`)は `unwind-protect` で戻す | `with-environment` の `unwind-protect` のみ。`switch-environment` は意図的に戻さない |
| 4 | **JIT のコンパイル再帰段数** | `g_za_compile_nest`(`za.c:2460`)、上限 `ZA_MAX_COMPILE_NEST = 60`(`za.c:2451`) | `za.c:3849/3852` で ++/-- | **`za_try_compile_defun` の冒頭で 0 に戻す**(`za.c:6645`)。**コメントに「前回の試行が途中で長いジャンプ(setjmp等)で抜けた場合に備えて」と既に書かれている**。よってこのカウンタだけは脱出に強い |
| 5 | **ハンドラスタック** | D-2 と同じ(`*HANDLERS*` は `g_dynamic_bindings` のエントリ) | 同上 | 同上 |
| 6 | **その他のグローバル可変状態** | 下表 | | |

**D-6 の内訳**:

| 状態 | 場所 | 脱出で壊れるか |
|---|---|---|
| `g_jit_used`(JIT コードバッファの先端) | `za.c:114` | **壊れる**。失敗パスは `g_jit_used = entry` で巻き戻すが(`za.c:6750`/`6839`/`6862`)、それを飛ばすと**書きかけのコードが永久に残る**(リセットは無い) |
| リテラルスロットの確保記録 `g_za_literal_slot_alloc_count` | `za.c` | **壊れる**。失敗パスの `za_release_literal_slot_allocs()` を飛ばすとスロットがリークする |
| `g_za_use_param_slots` / `g_za_block_level_mask` / `g_za_declaim` / `g_za_local_decl_count` | `za.c:1793/1586/2466/2477` | 次の `za_try_compile_defun` 冒頭で再初期化されるので自己修復する |
| **GC 中フラグ** `g_gc_debug_in_gc` | `runtime.c:814`(`ISIKIOS_GC_DEBUG` ビルドのみ) | GC 中に Lisp を呼ばないので脱出は起きえない(§F-1) |
| **割り込み許可状態** | `os_alloc_bytes` の `cli`/`sti`(`runtime.c:222/248`、`ISIKIOS_UNIT_TEST` 以外)、`os_gc_collect` の RFLAGS 退避(`runtime.c:2970`)、`stream.c:122/181/208` | **区間内に Lisp 呼び出しが無い**ので現状は安全。ただし `cli` 区間を longjmp で飛び越えると**割り込みが永久に止まる** |
| スケジューラ `g_current_process_index` | `process.c:11` | 脱出とは独立(タイマー割り込み側) |
| ストリームの書き込みバッファ `stream->write_buf_len` / `next_offset` | `stream.c` | **壊れうる**。flush の途中で Lisp 側(`WRITE-INTO!`)が脱出すると、バッファ長が未確定のまま残る(§F-6、**未確認**) |
| テストランナーの `*isiki-test-attempt*` 等 | `test_framework.lisp:156` | 既に「abort されたフォーム」を検出する設計になっている(§H) |

---

### E. JIT / AOT のフレーム構造

#### E-1. JIT 生成コードのプロローグ / エピローグ

**プロローグ**(`za.c:6714-6729`、固定引数エントリは `6745-6770` に同型のものがもう 1 つ):

```
push rbx
push r13
(ISIKIOS_PINNED_NIL のときだけ r12 へ nil)
sub  rsp, ZA_FRAME_TOTAL            ; = 0x28(シャドウスペース) + ZA_FRAME_EXTRA
mov  [rsp+ZA_OFF_SAVED_R14], r14    ; [ABI] callee-saved r14 の退避
mov  [rsp+ZA_OFF_ARGS_VAL], rcx
mov  [rsp+ZA_OFF_ENV_VAL],  rdx
call za_gc_current_head             ; ← shadow stack の現在の先頭を
mov  [rsp+ZA_OFF_ENV_SAVED_HEAD], rax  ;    フレームに保存
call za_gc_link (env), za_gc_link (args)
```

**エピローグ**(`za.c:6757-6769`):

```
mov  r13, rax                       ; 結果を退避
mov  rcx, [rsp+ZA_OFF_ENV_SAVED_HEAD]
call za_gc_unlink                   ; ← shadow stack を入口の状態へ一括復元
mov  rax, r13
mov  r14, [rsp+ZA_OFF_SAVED_R14]    ; [ABI] r14 を復元
add  rsp, ZA_FRAME_TOTAL
pop  r13
pop  rbx
ret
```

- **callee-saved の扱い**: `rbx` / `r13` は push/pop、**`r14` はフレームスロット**
  (`ZA_OFF_SAVED_R14`)経由。`r12` は `ISIKIOS_PINNED_NIL` 実験のときだけ触る。
  `rbp` / `r15` は生成コードが使わない。
- **b1719be 以降の状態**: r14 の保存/復元は **入口 2 箇所(`za.c:6691`, `6781`)と
  出口 3 箇所(通常エピローグ `6826`、末尾呼び出し 2 箇所 `4692`, `4716`)**に入っている。
  当時のコミットメッセージどおり「フレームに `ZA_OFF_SAVED_R14` を追加(+16 で 16byte
  整列を維持)」であり、`push`/`pop` で囲む案は rsp 相対のスロット参照がずれるため採れない。
- **アラインメント**: `push rbx` + `push r13` の 16 byte と、16 の倍数である
  `ZA_FRAME_TOTAL` の組み合わせで、`call` 直前の rsp 16byte 整列を保つ。

#### E-2. JIT → C ランタイムヘルパの呼び出し規約

**MS x64。** `rcx, rdx, r8, r9` に引数、戻り値 `rax`、シャドウスペース 32 byte は
`ZA_FRAME_TOTAL` の 0x28 部分にあらかじめ含まれており、**呼び出しごとに `sub rsp` はしない**。
呼び先アドレスは `movabs r11, imm64` → `call r11`(`jit_movabs_reg` + `jit_call_r11`)。
volatile レジスタが壊れるため、跨いで生かしたい値は callee-saved の `r13` へ退避する
(`za.c:2944-2948` などに定型が出てくる)。

#### E-3. ヘルパ内から JIT フレームを飛び越えて脱出した場合に壊れるもの

**JIT フレームは 3 種類の状態を持っており、どれもエピローグでしか片付かない。**

1. **`gc_rootnode` の実体がフレーム上にある。** env/args/params/呼び出し引数/
   算術アキュムレータの各スロットに `gc_rootnode` が埋め込まれ、`za_gc_link`
   (`za.c:1527`)で `proc->gc_roots` に**繋がれたまま**である。フレームを飛び越えると
   **`proc->gc_roots` が死んだスタック領域を指し続ける**。次の GC が
   `node->var_ptr` を辿って上書き済みのスタックを読む。
2. **`live_blocks`。** `block` の `os_live_block_push` に対応する `os_live_block_pop`
   (`za.c:5021`)を飛ばすと、**既に抜けた block が「生きている」ままになる**。
   以後の `return-from` がそこへ飛べてしまう。
3. **`r14` / `r13` / `rbx` / `rsp`。** 復元は出口 3 箇所にしかない。

**ただし復元自体は安い。** 1 は `ZA_OFF_ENV_SAVED_HEAD` に入口の値があるので
`za_gc_unlink(saved)` 1 回で戻る。2 も eval.c 側が既に
`os_live_block_restore(saved)`(`runtime.c:4513`)という一括復元 API を持っている。
つまり **「脱出先で `gc_roots` と `live_blocks` を保存値へ戻す」だけで足りる**構造には
なっている(現行方式ではそもそもフレームを飛び越えないので使われていない)。

#### E-4. AOT 生成コードのエラー処理パターン

AOT は関数ごとに `tco_result_t f__step(...)` を出し、式は GCC の statement expression
(`({ ... })`)を入れ子にする。エラー/脱出の扱いは**三項演算子による早期打ち切り**:

```c
/* src/c/lisp_compiled.c(生成物)の典型 */
({ lisp_val_t __call_arg_535 = (...); GC_PROTECT(__call_arg_535);
   os_is_control_transfer(__call_arg_535) ? __call_arg_535
   : ( ...続きの評価... ); })
```

- `GC_PROTECT` は statement expression のブロック末尾で cleanup される。
- `block` は `os_control_transfer_magic/name/value` で自分宛てかを判定する
  (`transpile.lisp:1375`)。
- `unwind-protect` は `({ lisp_val_t t = (protected); GC_PROTECT(t); (void)(cleanup); t; })`
  (`transpile.lisp:1471`)。**cleanup の戻り値は捨てる**ため、cleanup 自身の脱出は消える。
- `%abort-top-level` の生成結果(生成物 `lisp_compiled.c` の `lisp_ll_percent_abort_top_level__step`)は
  `os_is_control_transfer(v) ? v : os_make_instance(MAGIC_BLOCK_EXIT, "%TOP-LEVEL", v, nil)` で、
  **`os_live_block_p` の検査を行わない**(C-2 の非対称)。

---

### F. 脱出してはいけない区間の候補

| 区間 | エラーが通知されうるか | 根拠 |
|---|---|---|
| **GC の実行中** | **通知されえない** | `os_gc_collect_body`(`runtime.c:3080-`)は `gc_copy_value` と C のワークキューだけで、Lisp を一切呼ばない。失敗は `os_panic`(`runtime.c:2431` to-space 枯渇、`2747` 不正サイズ)。しかも `os_gc_collect` は RFLAGS を退避して `cli` する(`runtime.c:2970`) |
| **アロケータ(バンプ)の内部** | **通知されえない** | `os_alloc_bytes`(`runtime.c:221-251`)は `cli` 区間で、枯渇時は GC を 1 回試し、なお足りなければ `os_panic("out of memory")`。Lisp 呼び出しは無い |
| **Immobilized Space の確保中** | **通知されえない** | `os_imm_page_alloc`(`runtime.c:1776`)は枯渇時 `os_panic("immobilized space exhausted")`(`runtime.c:1784`)。`os_imm_pages_alloc_contiguous`(`2015`)/`os_imm_slot_alloc`(`2041`)/`os_imm_code_alloc`(`2166`)も Lisp を呼ばない |
| **environment(alist)の更新中** | **通知されえない** | `os_set_variable`(`runtime.c:4699-`)/`os_set_function` は `os_make_cons` と `cc_assoc_eq` だけ。signal 経路は無い |
| **defun の登録中(`za_try_compile_defun` を含む)** | **通知されうる。経路は 1 つ** | `za_macroexpand`(`za.c:2009`)→ `primitive_macroexpand_1`(`eval.c:1047`)→ `apply_function` → **ユーザ定義マクロの Lisp コードが走る**。ここで `error` を呼べる |

**F-5 の実測(実機、JIT 有効)**: マクロ展開器が `error` を呼ぶ場合、

```
defmacro ok
defun returned; code-base=NIL          ← JIT は断念してインタプリタへ落ちた
(uses-bad-macro 1) => (SIGNALED <SIMPLE-ERROR>)   ← エラーは「呼び出し時」に初めて出る
after-bad code-base=214451856 value=2  ← 次の defun は普通に JIT される
```

つまり**今日は「signal が値として返り、`za_macroexpand` がそれを展開結果とみなして諦める」**
ことで偶然無害になっている。`primitive_macroexpand_1` は `TAG_CONS` でない入力を
そのまま返すので(`eval.c:1049`)、block-exit は「収束した」と判定されてループを抜ける。
**ここを本物の脱出に変えると、`g_jit_used` の巻き戻しとリテラルスロットの返却
(D-6)を飛ばすことになる。**

#### F-6(追加): C から Lisp を呼び戻す区間

調査項目には挙がっていないが、**同じ性質の危険区間なので記録する。**

| 箇所 | 呼ぶ Lisp 関数 | 戻り値のチェック |
|---|---|---|
| `stream.c:140` | `WRITE-INTO!` | **無し**。`result == nil` だけ見る |
| `stream.c:195` | `READ-INTO!` | **無し**。`os_fixnum_magnitude(result)` に直接通す |
| `mount.c:122,136,173,198,211,224,239,253,257,272,275,295,309,549` | `%DEVICE-HANDLE` / `FAT16-READ-FILE` / `FAT32-*` ほか | **14 箇所すべて無し** |

`os_is_control_transfer` の呼び出し数は `stream.c` / `mount.c` とも **0**。
**コード読みのみで、FAT マウントを用意した実機再現はしていない(未確認)。**

---

### G. トップレベルの入口

#### G-1. REPL

`src/c/repl.c:14-40` `os_repl_step`:

```c
lisp_val_t form = os_read(proc);
if (form == nil) return;
lisp_val_t result = os_eval_top_level(form, proc->env);
os_print(result, proc->stdout_buffer);
proc->stdout_buffer->write_char(proc->stdout_buffer, '\n');
if (os_heap_used_ratio() > GC_TRIGGER_HEAP_RATIO) os_gc_collect();
```

`os_eval_top_level`(`eval.c:1231`)は form を `(block %TOP-LEVEL form)` に包んで
`os_eval` するだけ。docstring に「生の脱出シグナルがドライバや print まで漏れるのを防ぐ」
と明記されている。

#### G-2. ファイルロードとテストランナー

- `src/c/load.c:38-58` `cc_load`: `for(;;) { form = os_read_stream(&stream);
  ...; os_eval_top_level(form, env); }` — **1 フォームごとに `%TOP-LEVEL` を張り、
  戻り値は捨てて次へ進む。** 要件の「そのフォームだけ打ち切り、次のフォームへ進む」は
  **既に実現されている**。
- テストランナー: `isiki-test-load`(`test_framework.lisp:69-`)→ `(load path)` →
  `cc_load`。したがって同じ。
- **`env` の扱いが REPL と違う**: `cc_load` は自分が受け取った `env` を毎回渡す
  (`load.c:57`)ので、ロード中に `switch-environment` しても次のフォームには効かない。
  REPL は毎回 `proc->env` を読み直す(`repl.c:30`)ので効く。

#### G-3. 評価結果の表示

`os_print`(`print.c`)。`MAGIC_CLASS_INSTANCE` は `print.c:490-493` で
**`#<INSTANCE-OF <クラス名>>` とだけ表示する。**
`report-condition`(`init.lisp:579-608`)は定義されているが、**トップレベルからは
一度も呼ばれていない**(呼び出しは `test/lisp/init_test.lisp:431-466` のテストのみ)。

実機での表示例: `(error "boom ~A" 42)` → `#<INSTANCE-OF <SIMPLE-ERROR>>`。
**メッセージは出ない。**

#### G-4. フォーム評価開始時の environment を記録できる場所

- **REPL**: `repl.c:30` の直前。`proc->env` を読んだ値をローカルに控え、
  `os_eval_top_level` の直後に書き戻せばよい(1 箇所)。
- **load / テストランナー**: `load.c` はそもそも自分の `env` 変数を使い回しており
  `proc->env` を見ないので、`switch-environment` の影響を受けない。
  ただし **`proc->env` 自体は書き換わる**ので、ロード後の REPL に残る。
  同じ復元を入れるなら `load.c:57` の前後になる。
- `%%CURRENT-ENVIRONMENT` / `%%SET-CURRENT-ENVIRONMENT` は
  `runtime.c:8513` / `8645` にあり、どちらも副作用が `proc->env` だけなので保存・復元は容易。

---

### H. テスト基盤

#### H-1. エラー系のテストと期待値の書き方

**2 つの流儀がある。**

**(a) `assert-error`**(`test/lisp/test_framework.lisp:201`)— signal されることを期待する:

```lisp
(defmacro assert-error (form)
  `(let ((%isiki-actual
          (block %isiki-assert-error
            (isiki-test-begin ',form)
            (with-handler (lambda (%isiki-c) (return-from %isiki-assert-error '%isiki-signaled))
              (list '%isiki-no-error ,form)))))
     ...))
```

**(b) `(assert-equal 'eval-error ...)`** — **signal されない**ことを期待する。
`test/lisp/fn_cell_cache_test.lisp:19` に明記がある:
「未定義の関数呼び出しはconditionをsignalせず、シンボルEVAL-ERRORを値として返す」。

さらに**中断検出**がある(`test_framework.lisp:150-167`)。`assert-*` は評価前に
`*isiki-test-attempt*` を進め、次の `assert-*` の入口で `attempt > pass + fail` なら
「直前のフォームが中断した」と判定して `[ABORT] ... => aborted by error` を出す。
(`isiki-file-end-check` によるファイル単位の `#count` 検査は
`feature/instcount-gate` 側の追加で、**本ブランチにはまだ無い**。)

`make test`(ホスト、docker + gcc)は C 単体テスト 21 本。`make test-qemu` は実機で
`test/lisp/qemu_boot_test.lisp` を load し、`test-results.txt` の `" 0 failed"` を grep する。

#### H-2. エラーが脱出するようになったときに変更が必要になる既存テスト

| 種別 | 件数 | 内訳(実測) |
|---|---:|---|
| Lisp: `(assert-equal 'eval-error ...)` | **23** | `declare_typed_div_test.lisp` 9(`:101-107`, `:162-164`)、`fn_cell_cache_test.lisp` 4(`:21,43,44,45`)、`frame_definition_test.lisp` 3(`:65,69,96`)、`frame_variable_test.lisp` 3(`:37,43,47`)、`init_test.lisp` 3(`:921,922,937`)、`za_test_ext19.lisp` 1(`:90`) |
| C: `assert(... == g_sym_eval_error, ...)` | **28** | `runtime_test.c` 17(0 除算・`sqrt`/`log`・`parse-number`・`aref`/`elt`/`string-elt` の範囲外)、`eval_test.c` 5(`:199,907,932,1027,1050`)、`lisp_compiled_test.c` 3(`:932,940,1030`)、`load_test.c` 2(`:218,238`)、`stream_lisp_test.c` 1(`:306`) |
| 既存の `assert-error` | **54**(ほかに `test_framework.lisp` のマクロ定義 4 行) | 変更不要(むしろ増える側) |

**規模感: 51 件の期待値差し替え + 各テストファイルのコメント修正。**
関係するテストファイルは Lisp 6 本 + C 5 本の計 11 本。
**C 側が Lisp 側より多い**のが効くところで、`runtime_test.c` だけで 17 件ある
(ここは init.lisp をロードしないユニットテストなので、`os_signal_condition` の
フォールバック(§5-5)に当たる可能性が高い。P4 の各サブフェーズで個別に判断すること)。

加えて、**`load.c` / `stream_lisp.c` の `g_sym_eval_error` を signal に変えると、
`cc_load` の戻り値契約(成功 `t` / 失敗 `EVAL-ERROR`)が変わる**。
`qemu_boot_m1_base.lisp:6-7` がこの戻り値で構文エラーを検出しているので、
そこも同時に見直しが要る。

---

## 3. 現状の問題の再現例(A-4)

### 3-0. ハーネス

- **ホスト**: `tmp/errsurvey/repro_main.c`(`test/c/script_test.c` の複製を改造。
  production は無改変)。`src/lisp/init.lisp` を `global_environment` へロードしたあと、
  本番と同じ `os_eval_top_level` で 1 フォームずつ評価して `os_print` する。
  **このビルドでは JIT が常に断念する**(`za.c:6543` の `ISIKIOS_UNIT_TEST` ガード)。
- **実機**: `make test-qemu-milestone MILESTONE=tmp/errsurvey/qemu_boot_errsurvey.lisp`。
  **JIT が有効であることを `%%disasm-code-base` が非 nil を返すことで確認**した。

### 3-1. パターン 1〜3 — `EVAL-ERROR` / `nil` / 黙って違う値(実機、JIT 有効)

```
== EVAL-ERROR 系(捕捉できない。評価が続く) ==
(jdiv 1)   => (1 EVAL-ERROR 2)  note=NIL     ; (defun jdiv (x) (list 1 (div x 0) 2)) ← JIT 済み
div        => (1 EVAL-ERROR 2)  note=NIL
unbound    => (1 NIL 2)         note=NIL     ; (list 1 some-undefined-variable 2)
undef-fn   => (1 EVAL-ERROR 2)  note=NIL     ; (list 1 (some-undefined-fn 1) 2)
(+ 1 'a)   => (1 1 2)           note=NIL     ; ← 型違いが黙って無視される
(length 5) => (1 EVAL-ERROR 2)  note=NIL
(elt l 99) => (1 EVAL-ERROR 2)  note=NIL
(aref v 9) => (1 EVAL-ERROR 2)  note=NIL
(sqrt -1)  => (SIGNALED <DOMAIN-ERROR>)      ; ← sqrt だけは signal している
```

`note` は「エラー地点より後ろの副作用が走ったか」の足跡で、ここでは
`(list 1 ... 2)` の両隣がすべて評価されたことが `(1 ... 2)` という形から分かる。

同じことはホスト(インタプリタのみ)でも再現する:

```
FORM> (LIST (NOTE 3) (NO-SUCH-FUNCTION 1) (NOTE 4))
  => [3][4](3 EVAL-ERROR 4)          ; ← note 3 と note 4 の両方が走っている
FORM> (LIST (NOTE 7) (+ 1 (QUOTE A)) (NOTE 8))
  => [7][8](7 1 8)
```

### 3-2. 対照: signal 系は正しく脱出している(実機、JIT 有効)

```
== JIT code-base(非 nil = JIT 済み) ==
jerr=214451856 jerr2=214515712 jcar=214517808 jseq=214526144 jcatch=214529648 jtag=214533344 probe=214540288

== signal 系(handler で捕捉) ==
(jerr 1)   => (SIGNALED <SIMPLE-ERROR>)  note=NIL
(jerr2 1)  => (SIGNALED <SIMPLE-ERROR>)  note=(CLEANUP)      ; unwind-protect の cleanup が走る
(jcar 5)   => (SIGNALED <DOMAIN-ERROR>)  note=NIL
(jseq 1)   => (SIGNALED <SIMPLE-ERROR>)  note=(A)            ; (list (note 'a) (error ..) (note 'b)) の b は走らない
(jcatch 1) => (SIGNALED <SIMPLE-ERROR>)  note=(C1)           ; catch を貫通する
(jtag 1)   => (SIGNALED <SIMPLE-ERROR>)  note=(2 1 0)        ; while ループの途中で止まる
mapcar     => (SIGNALED <SIMPLE-ERROR>)  note=(1)            ; AOT 関数経由でも止まる
```

**JIT 済みコードでも、`unwind-protect` でも、`catch` でも、ループでも、AOT 関数経由でも、
signal されたコンディションは正しくその場で評価を打ち切っている。**

### 3-3. パターン 4 — 内側 `%TOP-LEVEL` による握り潰し(実機)

```lisp
(defglobal *e* (make-environment 'errsurvey-env (%%global-environment)))
(defglobal *r2* (progn (note 'before)
                       (with-environment *e* (note 'in-body) (error "inside-we") (note 'after-error))
                       (note 'after-we)))
```

```
B: with-environment の body でエラー。内側 %TOP-LEVEL で止まるか
(R2 AFTER-WE TRACE (BEFORE IN-BODY AFTER-WE))
C: 比較 - with-environment 無しの同じ形
(R3 NIL TRACE (BEFORE))
```

- **B**: `error` は `after-error` を打ち切ったが、**`with-environment` の外へは出ず**、
  `(note 'after-we)` が走り、`*r2*` は `AFTER-WE` になった。
- **C**: 同じ形で `with-environment` を外すと、`before` だけで正しく打ち切られる。

原因は `with-environment`(`init.lisp:1191-1199`)が
`%%eval-in-environment`(`runtime.c:8556`)を使い、その実体が
**`os_eval_top_level`、すなわちもう 1 つの `block %TOP-LEVEL`** だからである。

```c
/* src/c/runtime.c:8668-8672 */
lisp_val_t primitive_eval_in_environment(lisp_val_t args, lisp_val_t env) {
    ...
    return os_eval_top_level(form, target_env);   /* ← ここで内側の %TOP-LEVEL を張ってしまう */
}
```

**これが「`<error>` オブジェクトが値として呼出元へ返り、処理がそのまま継続する」の
実測ケースである。** `%%eval-in-environment` は `with-environment` のほか
`frame_definition_test.lisp` / `declaim_test.lisp` からも直接使われている。

### 3-4. `with-handler` のハンドラが正常 return した場合(実機)

```
== B-3: ハンドラが正常 return(非継続) ==
wh trace after abort = (BODY HANDLER-RAN)
== B-3b: 外側 handler がいる場合 ==
（この行の後に出力なし = probe の format まで到達していない）
```

- **B-3**: `(with-handler (lambda (c) 'ignored) (progn body (error "wh") after-error))` は
  `body` → `handler-ran` まで進み、`after-error` は走らず、
  **`(format ...)` を含むトップレベルフォーム全体が abort された**。
- **B-3b**: 内側ハンドラが正常 return すると、**外側の `with-handler` も
  `block` も飛び越えてトップレベルまで abort する**(`probe` の `format` が出ない)。

### 3-5. `unwind-protect` の入れ子(実機、JIT 有効)

```lisp
(defun up-nest (x)
  (unwind-protect (unwind-protect (progn (note 'inner-body) (error "up-nest")) (note 'inner-cleanup))
    (note 'outer-cleanup)))
```

```
up-nest code-base=214528000                                  ; JIT 済み
up-nest => (SIGNALED <SIMPLE-ERROR>)  note=(INNER-BODY INNER-CLEANUP OUTER-CLEANUP)
```

**内側 → 外側の順で cleanup が走る。** `catch` と混ぜた場合も同様:

```lisp
(defun mix (x) (catch 'tg (unwind-protect (progn (note 'a) (error "mix") (note 'b)) (note 'cleanup))))
;; mix => (SIGNALED <SIMPLE-ERROR>)  note=(A CLEANUP)
```

---

## 4. `with-handler` の仕様適合性の判定(B-5)

### 4-1. 根拠となる仕様の原文

**spec:6927-6930(§29.2)**:

> When a handler is called, it must handle the condition by transferring control to a point outside
> of the call to `signal-condition`. Such a transfer of control might be made explicitly by use of
> `go`, `throw`, or `return-from` or implicitly by use of an abstract operation such as
> `continue-condition` that has an equivalent effect. **The consequences are undefined if the
> handler returns normally; the handler is required to transfer control.**

**spec:6989-6991(`signal-condition`)**:

> If continuable is nil, the results of attempting to "continue" (see `continue-condition`) are not
> defined except that **the call to `signal-condition` will not return normally.**

**spec:6993-6996**:

> If continuable is not nil, **it will be possible to return from the call to `signal-condition`**
> (see `continue-condition`).

**spec:7045-7052(`with-handler`)**:

> Evaluates handler , which must yield a function (called the "handler function"). The handler
> function is established as active handler (see §29.2) and then the forms are executed.
> **If execution of forms finishes normally, the value of the last form (or nil if there are no
> forms) is returned.**

**spec:865-869(§9.1(a)「an error shall be signaled」)**:

> **Evaluation of the current form shall stop.** If no active handler is established by
> `with-handler`, **it is implementation defined whether the entire running process exits, a
> debugger is entered, or control is transferred elsewhere within the process.**

**spec:6910-6913(§29.2)**:

> An initial active handler will have been established by the system; it will provide some
> **implementation-defined action (such as return to toplevel, program exit, or entry into an
> interactive debugger).**

### 4-2. 判定

| # | 項目 | 現在の挙動 | 判定 | 根拠 |
|---|---|---|---|---|
| 1 | ハンドラが**非継続**コンディションで正常 return | `%abort-top-level` へ落ちる(外側ハンドラも飛ばす) | **仕様どおり(と言ってよい)** | spec:6929「consequences are undefined if the handler returns normally」。どう振る舞っても適合。`signal-condition` が「正常 return しない」(spec:6990)ことも守れている |
| 2 | ハンドラが**継続可**コンディションで正常 return | その値が `signal-condition` の戻り値になる | **仕様どおり** | spec:6993-6996「it will be possible to return from the call to `signal-condition`」 |
| 3 | `continue-condition` | `%continue-tag` への `throw` で `signal-condition` の `catch` へ戻り、値を返す | **仕様どおり** | spec:7038-7042「finding the call to `signal-condition` and arranging for it to perform a normal return of the value, which defaults to nil」 |
| 4 | ハンドラ無し・非継続 | トップレベルへ abort(`return-from %top-level`) | **仕様どおり** | spec:6912「such as return to toplevel」が明示的に許されている実装定義の選択肢 |
| 5 | ハンドラ無し・**継続可** | `nil` を返して続行 | **仕様の明文は無い。未規定** | spec は「active handler は system が必ず 1 つ確立している」(spec:6910)前提で、「ハンドラが無い」状況を記述していない。isiki の `nil` 返しは spec:6996 の「return from the call」と矛盾はしない |
| 6 | `with-handler` が正常終了時に最後のフォームの値を返す | そのとおり | **仕様どおり** | spec:7050-7052 |
| 7 | **エラーの発生でカレントフォームの評価が止まる** | **signal 経路は止まる。`EVAL-ERROR` 経路は止まらない** | **§9.1(a) 違反** | spec:866「Evaluation of the current form shall stop」。§3-1 の 8 パターンはいずれも評価が続いている |
| 8 | **`(car 5)` のような domain error** | signal する(適合) | 適合 | spec:893-895 |
| 9 | **未定義関数 / 未束縛変数** | `EVAL-ERROR` / `nil`(**signal しない**) | **§9.2(3) 違反** | spec:899-902「an error shall be signaled if the entity denoted by an identifier does not exist」。`<undefined-function>` / `<unbound-variable>` クラスは定義済みだが**生成する箇所が 1 つも無い** |
| 10 | **arity error** | `bind_params`(`eval.c:96-118`)が不足分を `nil` で埋め、余りを捨てる | **§9.2(2) 違反** | spec:896-898 |

### 4-3. まとめ

**`with-handler` / `signal-condition` / `continue-condition` そのものは、
調べた範囲で仕様に適合している。** 不適合なのは「エラーの検出側」で、
**§9.1(a)「Evaluation of the current form shall stop」を 50 箇所が守っていない**ことと、
**§9.2 の undefined-entity / arity を一切検出していない**ことである。

ただし 1 点、**実装方針で決めるべき挙動がある**: §4-2 の #1 で「外側ハンドラも飛ばして
トップレベルへ行く」のは仕様上自由だが、利用者から見ると
`(with-handler outer (with-handler inner ...))` の outer が無視されるので驚きやすい。
spec:6932「A handler may defer to previously established handlers by calling
`signal-condition`」が想定する「外側へ譲る」動きと**見た目が近いのに結果が違う**。

---

## 5. 実装上のリスクと注意点

### 5-1. `GC_PROTECT` — longjmp と両立しない

```c
/* src/c/runtime.h:1115-1118 */
#define GC_PROTECT(var) \
    gc_rootnode _gcnode_##var __attribute__((cleanup(gc_unprotect_node))) = \
        { (lisp_val_t *)&(var), get_current_process()->gc_roots }; \
    get_current_process()->gc_roots = &_gcnode_##var
```

- **push/pop は完全に C のスコープ脱出任せ**である。`longjmp` は `cleanup` 属性の
  ハンドラを呼ばない(C 標準にも GCC にもそんな保証は無い)。
- 飛ばすと `proc->gc_roots` が**死んだスタックフレーム上の `gc_rootnode`** を指し続け、
  次の GC が `node->var_ptr` の指す先(既に別の呼び出しが上書きしている)を
  ルートとして読む。`documents/pitfalls.md` の原則そのもの。
- **緩和策**: shadow stack は単一ポインタ headed のリストなので、
  脱出先で `get_current_process()->gc_roots = saved;` 1 行で完全に戻せる。
  JIT が既に `za_gc_unlink`(`za.c:1534`)で同じことをしている。
  `ISIKIOS_GC_DEBUG` ビルドには LIFO 違反カウンタ(`g_gc_lifo_violations`、`runtime.h:995` 宣言 / `1059` 加算)があるので、**入れた直後にこれで検算できる。**
- **ただし本件では longjmp を入れる必要が無い**(§C-4 / §7)。

### 5-2. JIT フレーム — 飛び越えると 3 つ壊れる

§E-3 のとおり **gc_rootnode の実体・`live_blocks` の段数・callee-saved レジスタ**が
エピローグでしか片付かない。**現行のバケツリレー方式なら 1 つも壊れない**
(必ず物理的にエピローグへ戻る)ので、**この方式を維持することが最大のリスク回避になる。**

もし将来 longjmp を入れるなら、最低限:

1. `proc->gc_roots` を setjmp 地点の値へ戻す
2. `os_live_block_restore(saved)` で `live_blocks` を戻す
3. `cli` 区間を飛び越えないことを保証する(`runtime.c:222`, `stream.c:122/181/208`)
4. `g_jit_used` / リテラルスロット(§D-6)の巻き戻し

### 5-3. 禁止区間

- **GC・アロケータ・Immobilized Space・environment 更新の 4 つは、そもそも
  エラーを通知しえない**(§F)。ここは心配しなくてよい。
- **危ないのは 2 つだけ**:
  - **`za_try_compile_defun` の中のマクロ展開**(§F-5)。脱出させると
    `g_jit_used` とリテラルスロットがリークする。
    → **`za_macroexpand` の戻り値が制御転送だったら、今と同じく「コンパイル断念」
    (`ZA_BAIL_LINE(); return nil;`)へ落とす**のが素直。脱出はインタプリタ側
    (呼び出し時のマクロ展開)に任せればよい。
  - **`stream.c` / `mount.c` の Lisp 呼び戻し 16 箇所**(§F-6)。
    ここは**現在すでに穴が開いている**。脱出を実装するかどうかに関係なく
    `os_is_control_transfer` のチェックを入れる価値がある。

### 5-4. 動的束縛の復元は `unwind-protect` に依存しきっている

`*handlers*` も `dynamic-let` も、**復元コードは Lisp の `unwind-protect` にしかない**
(§D-2/D-5)。したがって:

- **cleanup を飛ばす実装は選べない。** 要件の「途中の `unwind-protect` の後始末は
  必ず実行する」は、実は**選択肢ではなく制約**である。
- 逆に言えば、cleanup を必ず通す方式(= 現行のバケツリレー)を維持する限り、
  `*handlers*` の復元は**追加作業ゼロ**で正しくなる。

### 5-5. `EVAL-ERROR` → signal への移行は「意味の変わる 50 箇所」

- 50 箇所のうち **`os_signal_condition` は Lisp を呼び戻す**(`make-instance` と
  `signal-condition`)。**init.lisp ロード前に呼ばれる経路が無いかを箇所ごとに
  確認する必要がある**(`runtime.c:4471` のフォールバックが効くとはいえ、
  そこでは `EVAL-ERROR` に戻ってしまう)。
- **`os_signal_condition` は必ず 2 回の Lisp 呼び出しを伴う。**
  0 除算や `elt` の範囲外のようなホットパスに置くと、**正常系には 1 命令も増えないが
  異常系のコストは桁違いになる**。ベンチの異常系(`declare_typed_div_test.lisp` の
  0 除算を回す類)があれば影響を測ること。
- **`load.c` / `stream_lisp.c` の戻り値契約が変わる**(§H-2)。

### 5-6. AOT の `unwind-protect` は cleanup の脱出を捨てている

`transpile.lisp:1471` は `(void)(cleanup)` である。cleanup 自身が signal した場合、
**AOT では黙って消える**(インタプリタ・JIT は `<control-error>` にする)。
エラー脱出を強めると、この差が表面化しやすくなる。

### 5-7. トップレベルの表示

`report-condition` は**定義済みで未使用**(§G-3)。「発生した `<error>` をそのフォームの
評価結果として表示する」を満たすには `repl.c:32` の `os_print` を
`report-condition` 経由にする必要があるが、**`os_print` は C、`report-condition` は
generic function**なので、C から `os_apply_function` で呼び戻す形になる。
その呼び戻しの中でさらにエラーが起きた場合の扱い(spec:6923 の注記:
「handlers are not expected to handle errors in themselves」)を決めておくこと。

---

## 6. 未確認事項と判断が必要な点

### 6-1. 未確認(コードは読んだが実行で確かめていない)

1. **`stream.c` / `mount.c` の Lisp 呼び戻しからの脱出**(§F-6)。
   チェックが 0 箇所であることはコードで確認したが、**FAT マウントを用意して
   実際に脱出させる再現はしていない**。
2. **`za_try_compile_defun` の途中で本物の脱出を起こした場合のリーク量**(§D-6)。
   現在は脱出が起きないため、`g_jit_used` が巻き戻らない現象を**観測していない**。
3. **`with-environment` の中で `defun` した場合の JIT 状態**。§3-3 では
   `error` の握り潰しだけを確認した。
4. **AOT 関数の中で `block` を張り、インタプリタから `return-from` した場合**の
   `<control-error>`(C-2 の非対称)。コードから導いただけで実行していない。
5. **マルチプロセス下の挙動全般。** `gc_roots` / `live_blocks` / `proc->env` は
   プロセスごとだが、`g_dynamic_bindings`(= `*handlers*`)は**全プロセス共有**である。
   プロセス切り替えとハンドラスタックの関係は調べていない。
6. **1 回だけ観測したハング。** `(defun we-test () ... (with-environment 'errsurvey-env ...))`
   という形(環境**名**を渡す誤った使い方)で実機が 70 秒以上応答しなくなった。
   環境オブジェクトを渡す正しい形では再現しない(§3-3 は完走している)。
   **原因未特定。本件と関係があるかも未確認。**

### 6-2. 仕様未確認

7. **「ハンドラが無い状態で continuable なコンディションを signal した場合」**の
   規定を spec に見つけられなかった(§4-2 #5)。spec は system が初期ハンドラを
   必ず確立している前提で書かれている。
8. **`<storage-exhausted>`** をどこで signal すべきか。現在は `os_panic` で止めている
   (§F)。spec のクラス階層(spec:1010)には存在するが、
   signal するタイミングの規定は読んだ範囲では見当たらなかった。

### 6-3. 判断が必要な点(実装前に決めるもの)

| # | 論点 | 選択肢 |
|---|---|---|
| A | **ハンドラが非継続コンディションで正常 return したとき** | (a) 現状維持(トップレベルへ abort) / (b) 外側ハンドラへ譲る / (c) `<control-error>` を signal する。**spec 上はどれも合法**(§4-2 #1) |
| B | **`EVAL-ERROR` 返し 50 箇所をどこまで signal に寄せるか** | 全部 / ISLisp 仕様が「an error shall be signaled」と書いている箇所だけ / 段階的に |
| C | **未束縛変数(`nil` 返し)と arity 不一致を直すか** | 直すなら `<unbound-variable>` / `<undefined-function>` の生成口を新設する。**性能影響が最も大きい**(全変数参照・全関数呼び出しに判定が入る) |
| D | **`%%eval-in-environment` の内側 `%TOP-LEVEL`**(§3-3) | `os_eval_top_level` をやめて `os_eval` にする / `%TOP-LEVEL` とは別の block 名にする。後者なら `with-environment` の `unwind-protect` は今のまま効く |
| E | **`switch-environment` の巻き戻し範囲** | REPL だけ(`repl.c:30`) / `load` も(`load.c:57`) |
| F | **トップレベルの表示** | `#<INSTANCE-OF ...>` のまま / `report-condition` を呼ぶ(§5-7) |
| G | **`za_macroexpand` 中のエラー**(§F-5) | コンパイル断念に落とす / 脱出させる(リーク対策が要る) |

---

## 7. 実装方針への所見(任意)

**実装はしない。以下は調査から見えた観測にすぎない。**

1. **新しい脱出機構は要らない。** エラー脱出は既に `MAGIC_BLOCK_EXIT` として
   全 3 経路(インタプリタ・JIT・AOT)を正しく通っている(§3-2 で実機確認)。
   `setjmp`/`longjmp` を入れると §5-1/§5-2 の問題を新たに背負うだけで、
   得られるものは(現状の観測範囲では)無い。

2. **したがって作業の主軸は「脱出機構を作る」ではなく
   「エラーを signal へ寄せる」である。** §6-3 の B/C が実質的な本体で、
   それ以外(A/D/E/F/G)は各 1〜数箇所の局所的な修正に見える。

3. **「最も近い `with-handler` まで戻す」は、`with-handler` 側に block を
   張るだけで実現できそうに見える。** 現在の `with-handler` は
   `*handlers*` を push するだけで、自分の脱出先を持っていない。
   ハンドラが正常 return したときの行き先をその block にすれば、
   §4-2 #1 の選択肢 (b)/(c) も同じ仕組みで書ける。

4. **environment の巻き戻しは `repl.c` の 1 箇所で足りる**(§G-4)。
   `proc->env` を `os_eval_top_level` の前後で保存・復元するだけ。

5. **検出器を先に置くこと。** `documents/control-transfer-survey.md` §7 と
   `test_framework.lisp:148-155`(中断検出の由来を書いたコメント)が言っているとおり、
   「不変条件を観測する仕掛け」が無いまま `EVAL-ERROR` を消すと、
   **どこか 1 箇所の漏れが静かに残る**。候補:
   - `g_sym_eval_error` の残り箇所数をビルド時に数えて上限を切る
     (P0 で実施)
   - `ISIKIOS_GC_DEBUG` の `g_gc_lifo_violations`(§5-1)を
     エラー脱出のテストで 0 であることを確認する
   - `assert-error` に**クラスを指定する版**を足し、
     `(assert-error-class '<division-by-zero> (div 1 0))` のように
     「どのクラスが signal されたか」まで固定する

6. **移行は「signal するが `EVAL-ERROR` も返す」中間状態を作れない**点に注意。
   戻り値は 1 つしかないので、1 箇所ずつ切り替えて**そのたびにテストの
   期待値を直す**ことになる(§H-2 の 32 件)。
   `declare_typed_div_test.lisp` の 0 除算 10 件のように 1 ファイルに
   集中しているものから始めると、差分が読みやすい。

---

## 付録: 再現手順

```bash
# ホスト(インタプリタのみ。JIT は ISIKIOS_UNIT_TEST で常に断念する)
docker run --rm --user "$(id -u):$(id -g)" --entrypoint gcc -v "$PWD":/workspace isiki-builder \
  -std=c11 -Wall -Wextra -DISIKIOS_UNIT_TEST -Isrc/c -Itest/c \
  -o tmp/errsurvey/repro \
  src/c/runtime.c src/c/lisp.c src/c/process.c src/c/reader.c src/c/stream.c src/c/mount.c \
  src/c/stream_lisp.c src/c/za.c src/c/disasm.c src/c/disasm_symtab_lookup.c src/c/disasm_lisp.c \
  src/c/eval.c src/c/print.c src/c/format.c src/c/subprimitive.c src/c/drivers/ide.c \
  src/c/block_device.c src/c/ide_subprimitive.c src/c/lisp_compiled.c tmp/errsurvey/repro_main.c -lm
docker run --rm --user "$(id -u):$(id -g)" --entrypoint /workspace/tmp/errsurvey/repro \
  -v "$PWD":/workspace isiki-builder tmp/errsurvey/repro3.lisp

# 実機(JIT 有効)
make test-qemu-milestone MILESTONE=tmp/errsurvey/qemu_boot_errsurvey.lisp
cat test-results.txt
```

`tmp/` は `.gitignore` 対象なので、追跡対象のファイルは 1 つも変更していない。
