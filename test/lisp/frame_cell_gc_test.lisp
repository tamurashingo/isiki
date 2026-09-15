;; test/lisp/frame_cell_gc_test.lisp
;;
;; Function Cell が GC で再配置されることの回帰テスト。
;;
;; cell は Immobilized Space 上の生メモリで、環境の cells スロットからは
;; TAG_RAW_POINTER で参照されるため通常の GC スキャンの対象外。個別に再配置しないと
;; 中身(GCヒープ上の関数オブジェクトへのタグ付きポインタ)が from-space の
;; 古いアドレスのまま残る。
;;
;; 以前は global_environment と proc->env の cells しか再配置していなかったため、
;; flet/labels が作る frame の cell が漏れ、**GC 1回で確定的に壊れていた**
;; (documents/known-bug-frame-cell-gc.md)。frame は *environments* にも
;; proc->env にも載らないので環境を辿る方式では列挙できない。
;;
;; ケース名は上記ドキュメントの表に対応する。

;; GC を1回起こす: gc-collect-count が増えるまで cons し続ける
(defun fcg-force-gc ()
  (let ((c (%%gc-collect-count)))
    (while (= (%%gc-collect-count) c) (cons 1 2))
    (%%gc-collect-count)))

;;; --- A: flet 束縛を JIT から呼ぶ(これが壊れていた) ---
(flet ((fcg-a-h () 'alive))
  (defun fcg-a-f () (fcg-a-h))
  (assert-equal t (%%za-compiled-p (function fcg-a-f)))
  (assert-equal 'alive (fcg-a-f))
  (fcg-force-gc)
  (assert-equal 'alive (fcg-a-f))
  ;; 複数回GCを挟んでも壊れないこと(1回目で転送ポインタ、2回目で旧領域が上書きされる)
  (fcg-force-gc)
  (assert-equal 'alive (fcg-a-f))
  (fcg-force-gc)
  (fcg-force-gc)
  (assert-equal 'alive (fcg-a-f)))

;;; --- A2: 同じ関数を flet の外から呼ぶ(frame分離後に到達可能になった経路) ---
(assert-equal 'alive (fcg-a-f))
(fcg-force-gc)
(assert-equal 'alive (fcg-a-f))

;;; --- B: flet 束縛をインタプリタから呼ぶ(元々無事。壊していないことの確認) ---
(flet ((fcg-b-h () 'alive))
  (defun fcg-b-f (x) (progn (setq x 0) (fcg-b-h)))   ; 固定引数setqでフォールバック
  (assert-equal nil (%%za-compiled-p (function fcg-b-f)))
  (assert-equal 'alive (fcg-b-f 1))
  (fcg-force-gc)
  (assert-equal 'alive (fcg-b-f 1)))

;;; --- C: let frame の中で defun、呼び出し先はグローバル ---
(defun fcg-c-h () 'alive)
(let ((z 1))
  (defun fcg-c-f () (fcg-c-h)))
(assert-equal 'alive (fcg-c-f))
(fcg-force-gc)
(assert-equal 'alive (fcg-c-f))

;;; --- D: 完全にグローバル ---
(defun fcg-d-h () 'alive)
(defun fcg-d-f () (fcg-d-h))
(assert-equal 'alive (fcg-d-f))
(fcg-force-gc)
(assert-equal 'alive (fcg-d-f))

;;; --- E: labels 束縛を JIT から呼ぶ ---
(labels ((fcg-e-h () 'alive))
  (defun fcg-e-f () (fcg-e-h))
  (assert-equal t (%%za-compiled-p (function fcg-e-f)))
  (assert-equal 'alive (fcg-e-f))
  (fcg-force-gc)
  (assert-equal 'alive (fcg-e-f)))
(assert-equal 'alive (fcg-e-f))

;;; --- 再定義がGCを跨いでも追従すること(cellの中身が正しく差し替わる) ---
(defun fcg-redef () 1)
(defun fcg-caller () (fcg-redef))
(assert-equal 1 (fcg-caller))
(fcg-force-gc)
(defun fcg-redef () 2)
(assert-equal 2 (fcg-caller))
(fcg-force-gc)
(assert-equal 2 (fcg-caller))

;;; --- 多数の cell がある状態でも走査が正しいこと ---
;;
;; flet は呼び出しのたびに新しい frame を作り、そこに新しい cell を作る。
;; リストが長く伸びた状態で GC を跨いでも、全 cell が正しく追随することを見る。
(defun fcg-spawn () (flet ((fcg-s-h () 'alive)) (fcg-s-h)))
(defun fcg-spawn-many (n)
  (let ((i 0))
    (while (< i n) (progn (fcg-spawn) (setq i (+ i 1))))
    i))
(defglobal *fcg-cells-before* (%%diag-fn-cell-count))
(assert-equal 200 (fcg-spawn-many 200))
;; cell が実際に増えたこと(増えないなら、このテストは何も張れていない)
(assert-equal t (> (%%diag-fn-cell-count) *fcg-cells-before*))
(fcg-force-gc)
(assert-equal 'alive (fcg-spawn))
;; リストが伸びた後でも既存の cell が全て無事であること
(assert-equal 'alive (fcg-a-f))
(assert-equal 'alive (fcg-e-f))
(assert-equal 2 (fcg-caller))
