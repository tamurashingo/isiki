;; test/lisp/load_gc_stress_fixture.lisp
;;
;; [M2-T0 / #196] load_gc_stress_test.lisp が load する材料。
;;
;; **ここに書くフォームは「1 回だけ評価されたか」を数えられる形にしてある。**
;; 1 つの式を読む途中で記憶域の整理が走ると、以後その式は古い複製から読まれ、
;; 新しい実体の読取位置が進まない。症状は「同じ式が二度評価される」
;; 「途中から中身が化ける」で、**どちらも load は成功(t)を返す。**
;; したがって戻り値では見分けられず、**数えるしかない。**
;;
;; **入れ子のリストを使うのは意図である。** 平らなリストは要素を全部読んでから
;; cons を積むので、読んでいる途中に確保が入らない。入れ子なら内側のリストの
;; cons が先に確保され、**そのあと外側の読み取りが続く**(= 整理を跨いで
;; 読み取りが続く窓が開く。src/c/reader.c の read_list / read_list_rest)。
;;
;; 題材を小さく保つこと。%%diag-gc-stress 1(確保ごとに整理)は非常に遅い。

(setq *lgs-count* (+ *lgs-count* 1))

(setq *lgs-nested* '((1 2) (3 (4 5)) ((6) 7)))

(setq *lgs-strings* (list "alpha" "beta" "gamma"))

(setq *lgs-count* (+ *lgs-count* 1))
