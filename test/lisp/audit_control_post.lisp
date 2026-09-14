;; test/lisp/audit_control_post.lisp
;;
;; [GC監査] 塗り潰し監査(tools/bench/run_paint_audit.sh)の**後置対照**。
;; 各グループの末尾で流し、次の2点を確認する:
;;   1. 監査本体を流した後でも検出器がまだ効いていること(感度が途中で失われて
;;      いない。失われていれば、そのグループの「0件」は根拠にならない)。
;;   2. NGが出ても中断せず、first-ngに通番が記録され、後続が走り続けること。
;;
;; **意図的に1件だけNGを出す。** そのためこのファイルはmake test-qemuの試験列
;; (qemu_boot_test.lisp)には入れない。監査ドライバだけが読み込む。
;;
;; ISIKIOS_GC_DEBUGビルド専用。

;; 対照は強制GC間隔ではなく「GCをちょうど1回」で確定させる(%%DIAG-STALE-* の
;; 引数0)。塗り潰した旧From空間は次のGCのコピー先になるため、2回以上GCを跨ぐと
;; トラップではなく新しい生きたオブジェクトを指してしまい検出できなくなる。
;; 監査本体のGC圧力はドライバ(AUDIT_STRESS)が別途決める

(defglobal *audit-post-positive* (%%diag-stale-positive 0))
(defglobal *audit-post-negative* (%%diag-stale-negative 0))

(format *isiki-test-stream* "#control post positive=~D negative=~D~%"
        *audit-post-positive* *audit-post-negative*)
(finish-output *isiki-test-stream*)

(assert-equal 11 *audit-post-negative*)

;; 継続性の確認のための**意図的なNG**。検出が出ても中断せず、first-ngに通番が
;; 記録され、後続が走り続けることをこの1件で確かめる
(assert-equal 'audit-sentinel-expected 'audit-sentinel-actual)

;; 意図的なNGより後ろのアサーションが実際に走っていること
(assert-equal 3 (+ 1 2))
(assert-equal 11 (%%diag-stale-negative 0))

