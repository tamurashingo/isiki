;; test/lisp/load_gc_stress_test.lisp
;;
;; [GCデバッグ / M2-T0 / #196] **記憶域の整理を強制した状態で、
;; ディスク経由の読み取りと読み込みが壊れないこと。**
;;
;; docs/investigation/m2-load-from-disk.logic.md §6 / .refs.md §A-5。
;;
;; 何が危ないか:
;;   ディスク(FAT)経由の読み込み元は言語側の値(MAGIC_STREAM)に包まないと
;;   中身ごと消える。包むと今度は**整理のたびに実体とバッファ全体が別の場所へ
;;   複製される**(runtime.c の gc_relocate_stream)。したがって
;;   **実体を指す生の参照を、確保を跨いで持ち続けてはならない。**
;;
;;   いちばん深い穴は「1 つの式を読み切る処理の内部」である。
;;   read_list / read_list_rest は要素ごとに cons を確保し、read_atom は
;;   新しい symbol / string / 整数を確保する。その途中で整理が走ると、
;;   以後その式は古い複製から読まれ、**新しい実体の読取位置は進まない。**
;;   症状は「同じ式が二度評価される」「途中から中身が化ける」で、
;;   **どちらも読み取りも読み込みも成功を返す**(= 静かな破壊)。
;;
;; **GC_DEBUG ビルドでしか意味が無い**ので、通常ビルドで走ったら落ちる
;; (test/lisp/gc_debug_guard.lisp)。
;;
;; **題材は小さく保つこと。** %%diag-gc-stress 1 は確保ごとに整理を走らせるので
;; 非常に遅い(documents に「stress=1000 が上限」「時間予測は成立しない」という
;; 運用上の記録がある)。入れ子は 3 要素まで、ファイルは数フォームまでに留める。

(if (require-gc-debug-build "load_gc_stress_test")
    (progn

      ;;; ------------------------------------------------------------------
      ;;; 1. 包んだストリームからの read が、式の途中の整理に耐える
      ;;;
      ;;;    create-string-input-stream が作るのは STREAM_STRING_INPUT を
      ;;;    MAGIC_STREAM で包んだもので、**ディスク(FAT)経由の
      ;;;    open-input-file が作るものと同じ種別・同じ包み方である**
      ;;;    (src/c/stream_lisp.c の cc_open_input_stream は FAT 経路で
      ;;;     ファイル全体を読み出して STREAM_STRING_INPUT にする)。
      ;;;    ディスクを要らずに同じ窓を踏めるので、先にここで固める。
      ;;; ------------------------------------------------------------------

      (defglobal *lgs-str-stream*
        (create-string-input-stream "((1 2) (3 (4 5))) ((6) 7) lgs-tail-symbol"))

      (%%diag-gc-stress 1)
      ;; 1 つめ: 入れ子なので、内側の cons を確保したあとに外側の読み取りが続く
      (assert-equal '((1 2) (3 (4 5))) (read *lgs-str-stream*))
      ;; 2 つめ: **1 つめの読み取り位置が正しく進んでいなければここが化ける**
      ;;         (古い複製から読み続けると、1 つめの途中を二度読む)
      (assert-equal '((6) 7) (read *lgs-str-stream*))
      ;; 3 つめ: 新しい symbol の確保を跨いでも終端まで届く
      (assert-equal 'lgs-tail-symbol (read *lgs-str-stream*))
      (%%diag-gc-stress 0)
      (close *lgs-str-stream*)

      ;;; ------------------------------------------------------------------
      ;;; 2. ディスク(FAT16)経由の read が、式の途中の整理に耐える
      ;;;
      ;;;    材料はゲスト内で書く。**書くときは整理を強制しない**
      ;;;    (ディスクの書き込みは遅いという既知の性質。読みは速い。
      ;;;     documents/performance-measurement.md)。
      ;;;    母艦側で焼き込む作法もあるが、既存の FAT16 イメージの
      ;;;    ルート一覧の期待値を動かしてしまうので採らない。
      ;;;    旧方式(FAT16)は 8.3 名しか扱えないので名前は *.LSP である。
      ;;; ------------------------------------------------------------------

      (mount "/lgs" 'blk0 ':fat16)

      (defglobal *lgs-out* (open-output-file "/lgs/GCSTRES.LSP"))
      (format *lgs-out* "((1 2) (3 (4 5))) ((6) 7) lgs-disk-symbol~%")
      (close *lgs-out*)

      (defglobal *lgs-in* (open-input-file "/lgs/GCSTRES.LSP"))
      (%%diag-gc-stress 1)
      (assert-equal '((1 2) (3 (4 5))) (read *lgs-in*))
      (assert-equal '((6) 7) (read *lgs-in*))
      (assert-equal 'lgs-disk-symbol (read *lgs-in*))
      (%%diag-gc-stress 0)
      (close *lgs-in*)

      ;;; ------------------------------------------------------------------
      ;;; 3. 読み込み(load)が、整理を強制しても 1 回ずつしか評価しない
      ;;;
      ;;;    **この経路はホスト共有(9p)である。** load をマウント経由にするのは
      ;;;    #193 で別にやるので、ここでは載せ替えない。
      ;;;    9p のストリームは C のローカルにあって整理で動かないため、
      ;;;    これは**既存の経路が無傷であることの対照**であり、同時に
      ;;;    「二度評価」を数で捕まえる検出器でもある
      ;;;    (材料の *lgs-count* は 1 ファイルで必ず 2 増える)。
      ;;; ------------------------------------------------------------------

      (defglobal *lgs-count* 0)
      (defglobal *lgs-nested* nil)
      (defglobal *lgs-strings* nil)

      (%%diag-gc-stress 1)
      (assert-equal t (load "test/lisp/load_gc_stress_fixture.lisp"))
      (%%diag-gc-stress 0)

      ;; **ここが「同じ式が二度評価される」の検出器である。**
      ;; 二度評価されれば 2 を超え、取りこぼせば 2 に届かない
      (assert-equal 2 *lgs-count*)
      (assert-equal '((1 2) (3 (4 5)) ((6) 7)) *lgs-nested*)
      (assert-equal '("alpha" "beta" "gamma") *lgs-strings*)

      ;;; ------------------------------------------------------------------
      ;;; 4. 整理が実際に走ったことを確かめる
      ;;;    **走っていなければ、上の全部は何も確かめていない**(規則 8)
      ;;; ------------------------------------------------------------------

      (assert-equal t (if (> (%%gc-collect-count) 100) t nil))
      ;; stale ポインタの読み出しが 0 件であること。
      ;; **GC_PAINT を併用しないと trap は常に 0 なので、その場合この行は
      ;;   「壊れていない」の証拠にはならない**(GC_PAINT=1 のときだけ意味を持つ)
      (assert-equal 0 (%%diag-gc-trap-hits)))
  nil)
