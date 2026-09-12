;; test/lisp/audit_control_pre.lisp
;;
;; [GC監査] 塗り潰し監査(tools/bench/run_paint_audit.sh)の**前置対照**。
;; 各グループの先頭で流し、「この回の計測に感度と特異度があったか」を
;; 監査本体と同じビルド・同じフレームワークで毎回確認する。
;;
;; documents/pitfalls.md 原則6: 落ちなかったことを「バグがない」の根拠にする
;; には、まず計器が効いていることを同じ条件で示さなければならない。
;; 前置対照は**意図的なNGを含まない**(グループ内の最初のNGが本物かどうかを
;; 判定できるようにするため。意図的なNGは後置対照audit_control_post.lispに置く)。
;;
;; ISIKIOS_GC_DEBUGビルド専用(%%DIAG-*がそのビルドにしか存在しない)。

;; 対照は強制GC間隔ではなく「GCをちょうど1回」で確定させる(%%DIAG-STALE-* の
;; 引数0)。塗り潰した旧From空間は次のGCのコピー先になるため、2回以上GCを跨ぐと
;; トラップではなく新しい生きたオブジェクトを指してしまい検出できなくなる。
;; 監査本体のGC圧力はドライバ(AUDIT_STRESS)が別途決める

;; 陽性対照: 保護されていないlisp_val_tローカルが確保を跨ぐ。塗り潰しが
;; 効いていればトラップパターンを読んで999を返す。
;; 陰性対照: 同じ構造でGC_PROTECTを正しく行う。必ず11を返す。
(defglobal *audit-positive* (%%diag-stale-positive 0))
(defglobal *audit-negative* (%%diag-stale-negative 0))

;; 検出値そのものはビルド構成(塗り潰しの有無)に依存するのでアサートせず、
;; 記録として残す。ホスト側のドライバがこの行を見て感度の有無を判定する
(format *isiki-test-stream* "#control pre positive=~D negative=~D~%"
        *audit-positive* *audit-negative*)
(finish-output *isiki-test-stream*)

;; 特異度: 正しく保護されたものをstaleと誤判定しないこと。
;; ここがNGなら、監査結果ではなく検出器のほうを疑う
(assert-equal 11 *audit-negative*)

;; 対照は意図的にstaleを作るので、その検出を監査本体の計数に混ぜない。
;; ここでリセットし、以後の検出はすべて本体由来にする
(%%diag-gc-trap-reset)
