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
      ;;;    **この経路はホスト共有(9p)である**(先頭に区切りが無い名前は
      ;;;    マウント経由にした後もホスト共有へ行く。#193 の契約)。
      ;;;    9p のストリームは C のローカルにあって整理で動かないため、
      ;;;    これは**既存の経路が無傷であることの対照**であり、同時に
      ;;;    「二度評価」を数で捕まえる検出器でもある
      ;;;    (材料の *lgs-count* は 1 ファイルで必ず 2 増える)。
      ;;;    **ディスク経由は 4 で見る。**
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
      ;;; 4. 【#196 からの申し送り】**ディスク経由の load** が、整理を強制しても
      ;;;    1 回ずつしか評価しない
      ;;;
      ;;;    #196 の時点ではディスク経由の load の経路が無く、検出器に入れられ
      ;;;    なかった(read で同じ種別・同じ包み方のストリームを固めるところまで)。
      ;;;    M2-T1(#193)で経路ができたので、**本物の経路で固める。**
      ;;;
      ;;;    材料は母艦側でイメージへ焼き込んだ apps/load-gc-stress.lisp。
      ;;;    **ゲスト内で書いてから読む形にはしない**(書き込みが遅い。読みは速い)。
      ;;;    題材には **(a) 入れ子のリスト** と
      ;;;    **(b) 生存集合を膨らませるフォーム** の両方が入っている。
      ;;;    **どちらが欠けても、穴が開いていても気づけない**(#196 の実測):
      ;;;      - 平らなリストは要素を全部読んでから cons を積むので、
      ;;;        読取中に確保が入らない
      ;;;      - 生存集合が小さいと、複製先が古い複製の位置まで届かないので
      ;;;        古い複製を読み続けても中身が壊れない
      ;;;
      ;;;    **自動の割り当ては無い**ので、読む前に mount を呼ぶ前段が要る。
      ;;;    装置の名前は探査順の連番なので、**名前引きで吸収する。**
      ;;; ------------------------------------------------------------------

      (defglobal *app-lgs-count* 0)
      (defglobal *app-lgs-live* nil)
      (defglobal *app-lgs-nested* nil)
      (defglobal *app-lgs-strings* nil)

      (defglobal *lgs-app-device* (%device-named "ide-primary-slave"))
      ;; **陽性対照: 名前引きが当たっていること。** 外れていれば以後は
      ;; 「読めない」が別の理由で出る
      (assert-equal t (if *lgs-app-device* t nil))
      (assert-equal nil (eq *lgs-app-device* 'blk0))
      (mount "/app" *lgs-app-device* ':fat32)

      ;; **整理の強制は load の全体にかける。** ディスクからの読み出し
      ;; (FAT の運転手は Lisp なので確保する)も、読んで評価する輪も、
      ;; どちらも整理を跨ぐ
      (%%diag-gc-stress 1)
      (assert-equal t (load "/app/load-gc-stress.lisp"))
      (%%diag-gc-stress 0)

      ;; **ここが「同じ式が二度評価される」の検出器である。**
      ;; 二度評価されれば 2 を超え、取りこぼせば 2 に届かない
      (assert-equal 2 *app-lgs-count*)
      (assert-equal '((1 2) (3 (4 5)) ((6) 7)) *app-lgs-nested*)
      (assert-equal '("alpha" "beta" "gamma") *app-lgs-strings*)
      (assert-equal 5 (length *app-lgs-live*))

      ;;; ------------------------------------------------------------------
      ;;; 5. 整理が実際に走ったことを確かめる
      ;;;    **走っていなければ、上の全部は何も確かめていない**(規則 8)
      ;;; ------------------------------------------------------------------

      (assert-equal t (if (> (%%gc-collect-count) 100) t nil))
      ;; stale ポインタの読み出しが 0 件であること。
      ;; **GC_PAINT を併用しないと trap は常に 0 なので、その場合この行は
      ;;   「壊れていない」の証拠にはならない**(GC_PAINT=1 のときだけ意味を持つ)
      (assert-equal 0 (%%diag-gc-trap-hits)))
  nil)
