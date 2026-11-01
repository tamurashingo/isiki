;; test/lisp/heap_exhaustion_test.lisp
;;
;; [可用性/回帰] **ヒープ枯渇が機械停止ではなくエラーになる。** issue #110、
;; documents/heap-exhaustion.md(D9)。スタック側は test/lisp/stack_guard_test.lisp。
;;
;; 直す前の症状:
;;   `(create-array '(1000000000) 0)` のような**上限検査の無い巨大な配列 1 つ**で
;;   `os_alloc_bytes` が枯渇し、`os_panic` が走って**電源が切れる**
;;   (`g_panic_hook`)。しかも電源が切れると `test-results.txt` が書かれないので、
;;   **PR #151 で族として扱った「結果が無い」flake と外から区別が付かない。**
;;   再試行の仕組みが入ったぶん、本物の枯渇を 2 回試してから落ちる。
;;
;; 直したあと:
;;   確保を要求する primitive(create-array / create-vector / create-string)が
;;   確保の**前に**「入るか」を見て、入らなければ `<storage-exhausted>` を signal する。
;;   **signal になれば結果ファイルは書かれる。** それがこのファイルの存在意義である
;;   (= このファイルが結果を残せていること自体が T6 の答えになっている)。
;;
;; **確保の要求は 3 経路(インタプリタ / AOT / JIT)が同じ C の primitive を通る。**
;; だから検査は 1 箇所でよい。スタック側は深さが呼ぶ側のフレームで増えるため
;; **JIT だけしか守れなかった**(documents/stack-guard.md §4)。ここが違う。
;;
;; 注意: `<storage-exhausted>` は `<serious-condition>` の直接の子であって
;; **`<error>` の子ではない。** だから `ignore-errors` では捕まらない
;; (ISLisp のクラス階層どおり)。`with-handler` を使う。
;;
;; **守っていない経路を書く(規則 1 / スタック側と同じ作法)**:
;;   小口の確保(cons / instance)が枯渇したときは、確保器が**印**を立てて
;;   予備領域から確保を続け、**os_eval の評価の入口**か大口の primitive で知らせる。
;;   つまり **JIT と AOT の生成コードが小口の確保だけを続ける区間は守られていない。**
;;   そこは印が立ったまま誰も見ないので、床まで達すると panic する。
;;   小口の枯渇(印 / 予備の開閉 / 重ねない / エラー値へ落ちる)は**ネイティブ側**
;;   (test/c/runtime_test.c の test_heap_exhaustion_*)が本物の枯渇で見ている。
;;   ゲストでやらないのは、**本物に枯渇させるとこの後ろの試験が全部道連れになる**ため。

;;; ---------------------------------------------------------------------------
;;; 1. 大口の確保 — インタプリタ経路
;;; ---------------------------------------------------------------------------
;;
;; 1e11 要素 = 8e11 byte(800GB)。QEMU_MEM 256M のヒープには**どう GC しても入らない**。

(assert-error-class '<storage-exhausted> (create-vector 100000000000))
(assert-error-class '<storage-exhausted> (create-string 100000000000))
(assert-error-class '<storage-exhausted> (create-array '(100000000000)))
;; 多次元。1e6 × 1e6 = 1e12 要素(桁あふれはしないが入らない)
(assert-error-class '<storage-exhausted> (create-array '(1000000 1000000)))
;; make-array は create-array と同じ実体
(assert-error-class '<storage-exhausted> (make-array 100000000000))

;; 階層(<storage-exhausted> < <serious-condition> < <condition>)。
;; **<error> ではないことも固定する**(ここが逆になると ignore-errors で
;; 握り潰される側へ移ってしまう)
(assert-error-class '<serious-condition> (create-vector 100000000000))
(assert-error-class '<condition> (create-vector 100000000000))
(assert-equal nil (subclassp (class <storage-exhausted>) (class <error>)))

;;; ---------------------------------------------------------------------------
;;; 2. 判定は**掛け算より前**にある
;;; ---------------------------------------------------------------------------
;;
;; 入れる前の `alloc_vector_block` は `total *= dims[i]` を素で回していた。
;; **桁あふれの検査も負値の検査も無かった。**
;;
;;   2^32 × 2^32 = 2^64 → 巻き上がって **ちょうど 0**。
;;   total = 0 なので 8*(1+2+0) = 24 byte の塊を確保し、ヘッダには
;;   「各次元 2^32」と書く。aref / set-aref の範囲検査はそのヘッダを見るので、
;;   `(set-aref 1 a 0 0)` は**範囲内と判定されたうえで塊の外へ書く。**
;;   つまりこれは枯渇ではなく**ヒープの踏み越え**だった。
;;
;; **枯渇の判定を掛け算の後ろに置くと、まさにこの場合だけ検査を通り抜ける。**
;; だから判定は掛けているその場で行う(`__builtin_mul_overflow`)。
;; 桁あふれの答えも `<storage-exhausted>` にする(入らないのだから)。

(assert-error-class '<storage-exhausted> (create-array '(4294967296 4294967296)))
;; 巻き上がり先が巨大になる場合(2e9 の 3 乗 = 8e27)。
;; byte 数が 1.7e19 になるので、ポインタ演算で見ると**回り込んで「空きがある」**に見える
(assert-error-class '<storage-exhausted> (create-array '(2000000000 2000000000 2000000000)))

;;; ---------------------------------------------------------------------------
;;; 3. 負の長さ / 非整数の長さ
;;; ---------------------------------------------------------------------------
;;
;; **fixnum は 2 の補数ではなく「符号 bit63 + マグニチュード bit4-62」である。**
;; だから `v >> FIXNUM_VALUE_SHIFT` と生で書くと符号ビットが値に混ざり、
;; `(create-vector -1)` は **1e17 規模の「非負の長さ」**として通っていた。
;;
;; クラスは `<domain-error>`(引数が非負整数でない)。
;; **仕様に <non-negative-integer> のようなクラスは無い**ので、期待クラスには
;; 受け付けるクラスの側(<integer>)を入れている
;; (index_signal_test.lisp が <basic-vector> を入れているのと同じ扱い)。

(assert-error-class '<domain-error> (create-vector -1))
(assert-error-class '<domain-error> (create-string -1))
(assert-error-class '<domain-error> (create-array '(-1)))
(assert-error-class '<domain-error> (create-array -1))
;; 長さが整数でない場合も同じ入口で弾く(生シフトすると番地が長さに化ける)
(assert-error-class '<domain-error> (create-vector "abc"))
(assert-error-class '<domain-error> (create-vector 'a))

;;; ---------------------------------------------------------------------------
;;; 4. JIT 経路
;;; ---------------------------------------------------------------------------
;;
;; **同じ primitive を通るので、同じ検査に当たる。**
;; `%%za-compiled-p` が nil になったらこの節は JIT を見ていない(= 経路の確認が
;; 空振りしている)ので、**先にそれを固定する**(規則 12)。

(defun hx-jit-vector (n) (create-vector n 0))
(assert-equal t (%%za-compiled-p (function hx-jit-vector)))
(assert-equal 2 (length (hx-jit-vector 2)))
(assert-error-class '<storage-exhausted> (hx-jit-vector 100000000000))
;; JIT 関数は枯渇のあとも壊れていない
(assert-equal 2 (length (hx-jit-vector 2)))

(defun hx-jit-string (n) (create-string n))
(assert-equal t (%%za-compiled-p (function hx-jit-string)))
(assert-equal 3 (length (hx-jit-string 3)))
(assert-error-class '<storage-exhausted> (hx-jit-string 100000000000))

(defun hx-jit-array (d) (create-array d 0))
(assert-equal t (%%za-compiled-p (function hx-jit-array)))
(assert-error-class '<storage-exhausted> (hx-jit-array '(4294967296 4294967296)))

;;; ---------------------------------------------------------------------------
;;; 5. AOT 経路
;;; ---------------------------------------------------------------------------
;;
;; **AOT 済みの関数の中の `create-vector` も同じ primitive を呼ぶ**
;; (transpile.lisp の対応表で `create-vector` → `primitive_create_vector`)。
;;
;; 題材に `%ide-bytes-from-addr`(src/lisp/ide.lisp。Makefile の
;; TRANSPILE_LISP_SRC に入っているので C へトランスパイルされている)を使う。
;; `(create-vector count 0)` の count が**引数そのまま**なので、大きさを外から
;; 決められる AOT 関数がこれしか無い。
;;
;; **なぜ試験専用の AOT 関数を足さないのか**: AOT の defun は 1 つ足すごとに
;; 組み込み関数が 1 つ増え、os_bootstrap の恒久消費が増える。
;; test/c/runtime_test.c の SMALL_HEAP_SIZE は**通る窓が 5KB 未満**で、
;; そこが動くと NG ではなく**ハング**として現れる(memory:
;; builtin_count_small_heap_calibration)。この作業では組み込みを 1 つも増やさない。
;;
;; `%%peek` は 1 要素も書く前に signal されるので踏まない(踏むなら
;; アドレス 0 の読み出しで別の形で落ちるため、そこでも気づける)。

(assert-error-class '<storage-exhausted> (%ide-bytes-from-addr 0 0 100000000000))
;; AOT 関数は枯渇のあとも壊れていない(0 byte なら空の vector が返る)
(assert-equal 0 (length (%ide-bytes-from-addr 0 0 0)))

;;; ---------------------------------------------------------------------------
;;; 6. 知らせたあとも続行できる / 打ち切りに当たっていない
;;; ---------------------------------------------------------------------------
;;
;; **ここが通っていること = 結果ファイルが書かれていること**である。
;; 枯渇で電源が切れていたら、この下は 1 件も出ない。

(assert-equal 3 (+ 1 2))
(assert-equal '(1 2 3) (list 1 2 3))
(assert-equal 5 (length (create-vector 5 0)))
(assert-equal 5 (length (create-string 5)))
(assert-equal '(2 3) (array-dimensions (create-array '(2 3) 0)))

;; ハンドラは signal の動的文脈で走り、完走する(クラス名を引いてリストを組む
;; 「現実的な形」のハンドラ。stack_guard_test.lisp §7b と同じ題材)
(defun hx-handler-realistic ()
  (block done
    (with-handler
        (lambda (c)
          (return-from done
            (list 'handled (%%class-name (class-of c))
                  (length (list 1 2 3 4 5 6 7 8 9 10)))))
      (create-vector 100000000000))))
(assert-equal '(handled <storage-exhausted> 10) (hx-handler-realistic))

;; ハンドラの中で**もう一度**大口を要求しても戻ってこられる(重ねないこと)。
;;
;; ハンドラは signal の動的文脈で走るので、**そこはまだ知らせている最中**である。
;; 内側の枯渇は signal を重ねず、シンボル `EVAL-ERROR` を**値として**返す
;; (重ねると予備領域を食い潰してから panic し、「エラーを出そうとして機械が
;;  止まる」というもっと分かりにくい症状になる)。
;; **ここが <storage-exhausted> になったら重なっている。**
(defun hx-handler-nested ()
  (block done
    (with-handler
        (lambda (c)
          (return-from done
            (list 'outer (%%class-name (class-of c))
                  (create-vector 100000000000))))
      (create-vector 100000000000))))
(assert-equal '(outer <storage-exhausted> EVAL-ERROR) (hx-handler-nested))

;; signal の深さ打ち切り(SIGNAL_MAX_DEPTH 16)に当たっていないこと。
;; 当たると**signal しようとして何も起きない**(os_signal_condition が
;; EVAL-ERROR を返す)。上で何度も枯渇させたあとで 0 = 1 回も打ち切られていない
(assert-equal 0 (%%diag-signal-overflows))

;;; ---------------------------------------------------------------------------
;;; 7. 1 回だけではない。何度でも枯渇して戻れる
;;; ---------------------------------------------------------------------------

(assert-error-class '<storage-exhausted> (create-vector 100000000000))
(assert-error-class '<storage-exhausted> (create-vector 100000000000))
(assert-error-class '<storage-exhausted> (hx-jit-vector 100000000000))
(assert-equal 3 (+ 1 2))
(assert-equal 0 (%%diag-signal-overflows))
