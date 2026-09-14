;;;; test/lisp/aot_leaf_gc_test.lisp
;;;;
;;;; [ABI刷新] AOT改善: transpile.lispのtranspile-call-args-guardedが導入した
;;;; leaf判定(aot-form-is-leaf、documents/performance-measurement.md
;;;; 2026-09-11「GC_PROTECT削減」節参照)のGC整合性回帰テスト。
;;;;
;;;; 非box化ローカル変数を関数呼び出しの引数として渡す際、その変数が束縛時に
;;;; 一度だけGC_PROTECTされていれば、呼び出しのたびに再保護しなくてもGCによる
;;;; 再配置後も正しい値を指し続ける、という前提を検証する。テスト対象は
;;;; write-vector-to-file/read-file-into-vector(src/lisp/file-cmd.lisp、
;;;; transpile.lisp経由でAOTトランスパイルされる)自身——vec/stream/iといった
;;;; leaf判定される変数を毎バイト引数として渡す、まさに本改善の対象そのもの。
;;;; 意図的にGCを複数回誘発しながら往復させ、内容が破損しないことを確認する。

;; (%%leaf-gc-stress-make-vector n) : 0〜255循環のnbyte vectorを作る。単純な
;; set-elt呼び出しだけなので、defun自身はJITコンパイルされうるが(この関数の
;; leaf判定はza.c側の対象であり本テストの主眼ではない)、生成するデータの
;; 中身の正しさだけが目的
(defun %%leaf-gc-stress-make-vector (n)
  (let ((v (create-vector n 0)) (i 0) (b 0))
    (while (< i n)
      (progn
        (set-elt b v i)
        (setq b (if (>= b 255) 0 (+ b 1)))
        (setq i (+ i 1))))
    v))

;; (%%leaf-gc-stress-churn-garbage n size) : GCを追加で誘発するための、使い捨て
;; vectorをn個確保するだけのループ
(defun %%leaf-gc-stress-churn-garbage (n size)
  (let ((i 0))
    (while (< i n)
      (progn
        (create-vector size 0)
        (setq i (+ i 1))))))

;; 監査モード(*isiki-audit*)では規模を落とす。
;; 通常の回帰では300000byteの往復でleaf判定のGC整合性を検証するが、塗り潰し監査は
;; 「各アサーションがGCを1回以上跨いだか」を見る用途で、確保量そのものは目的ではない。
;; AUDIT_STRESS=100 だとこの規模では約4万回のGCになり打ち切られてしまい、
;; **かえって1件も監査できなくなる**。通す方が被覆は上がる。
;; 経路(write-vector-to-file/read-file-into-vectorのAOTコード)は同じものを通る。
(defglobal leaf-gc-stress-len (if *isiki-audit* 3000 300000))
(defglobal leaf-gc-stress-path "/9p/tmp/aot-leaf-gc-stress.bin")
(defglobal leaf-gc-stress-src (%%leaf-gc-stress-make-vector leaf-gc-stress-len))

(defglobal leaf-gc-stress-gc-before (%%gc-collect-count))

;; write-vector-to-file(AOT、write-byteを毎バイト呼ぶ)実行中にもGCが起こりうる
(assert-equal t (write-vector-to-file leaf-gc-stress-path leaf-gc-stress-src))

;; 追加のゴミ確保でさらにGCを誘発してから読み戻す(書き込み直後のキャッシュ効果
;; だけに依存しないようにするため)。ヒープ総量(%%heap-total-bytes)の数倍に
;; 相当する量を確保し、ヒープサイズに関わらず確実に複数回のGCを誘発する
(if *isiki-audit*
    (%%leaf-gc-stress-churn-garbage 40 5000)
    (%%leaf-gc-stress-churn-garbage 4000 5000))

;; read-file-into-vector(AOT、本改善のleaf判定の対象そのもの)で読み戻す
(defglobal leaf-gc-stress-readback (read-file-into-vector leaf-gc-stress-path))

(defglobal leaf-gc-stress-gc-after (%%gc-collect-count))

;; このテスト自体がGCを複数回誘発したことを確認する(誘発できていなければ
;; このテストはGC整合性を何も検証していないことになるため、前提条件として
;; 明示的にチェックする)
(assert-equal t (> (- leaf-gc-stress-gc-after leaf-gc-stress-gc-before) 1))

;; サイズと内容が完全に一致することを確認する(GCによる再配置後に古い
;; アドレスを指したままになっていれば、この時点で値化けとして検出できるはず)
(assert-equal leaf-gc-stress-len (length leaf-gc-stress-readback))
(assert-equal t (equal leaf-gc-stress-src leaf-gc-stress-readback))

;; read-file-into-vector自身がAOT(transpile.lisp経由)でコンパイルされている
;; ことの回帰確認(%%za-compiled-pはJITコンパイル済み関数の判定なのでnilに
;; なるはず=このテストが本当にtranspile.lispの生成コードを経由していることの
;; 確認)
(assert-equal nil (%%za-compiled-p (function read-file-into-vector)))
(assert-equal nil (%%za-compiled-p (function write-vector-to-file)))

(close (open-output-file "/9p/tmp/ckpt-1-aot-leaf-gc-test.txt"))
