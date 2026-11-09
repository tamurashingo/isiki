#!/bin/sh
# [M1-T6 / issue #185] 同梱した基盤ライブラリを起動ごとに読む費用を、
# **同梱あり / 同梱なしを 1 台で続けて測って**表に組む。
#
# 記録と予測: documents/boot-embedded-init-cost.md
#
# ## なぜ 1 本のスクリプトなのか
#
#   - **QEMU は直列**(同時に走らせない)。起動を順に回す必要がある
#   - 2 つの構成は**ビルドフラグが違う**(-DISIKIOS_NO_EMBEDDED_INIT。設計判断 L5)。
#     フラグは BUILD_FLAG_SIGNATURE に入っているので、make が自動で作り直す。
#     **人が作り直しを覚えておく必要は無い**(「設定を変えたつもりで古い成果物を
#     測る」事故を防ぐ仕組みが既にある)
#   - test-results.txt は毎回上書きされるので、**走らせた直後に退避**しないと差が取れない
#
# ## なぜ 3 回ずつ回すのか(規則 4 / 規則 10)
#
# **tick は計器のぶれが ±20 ある**(documents/records-and-measurement-rules.md 規則 9 の表)。
# 1 回の値では「見えた差」なのか判断できない。3 回回して **min / median / max** を出し、
# **ぶれの幅と差の大きさを並べて見せる**。
# 確保量(ヒープ・imm)は同じ表で「ぶれ無し」とされているので、
# **3 回で完全一致するかどうかがその前提の検算になる**(規則 2: 完全一致は疑えだが、
# ここは一致を**予測して**いるので一致が裏づけになる)。
#
# ## 陽性対照(規則 8)
#
# 同梱を外すと **約 83KB の Lisp を読み・解析し・機械語化する作業がまるごと消える**。
# imm / ヒープ / tick の **3 つがすべて下がる**ことを、このスクリプトが判定する。
# 下がらない量があれば「その計器は対象を見られていない」と出す(現象ではなく計器の限界)。
#
# ## 陰性対照(規則 5)
#
# **同梱と無関係な量は動いてはいけない。** boot allocator の使用量
# (run_boot_entry より前に確定する)・ヒープ総量・imm 総量の 3 つがそれで、
# 差が 0 でなければ**測定が壊れている**。これも判定に入れてある。
#
# ## 使い方
#
#   make boot-cost                  # 3 回ずつ(既定)
#   BOOT_COST_RUNS=5 make boot-cost # 回数を変える
#
# 出力は標準出力と tmp/boot_cost_report.txt。素の test-results.txt は
# tmp/boot_cost_<構成>_<n>.txt に残る。

set -e
cd "$(dirname "$0")/.."
mkdir -p tmp

OUT=tmp/boot_cost_report.txt
MILESTONE=test/lisp/qemu_boot_m1_cost.lisp
RUNS=${BOOT_COST_RUNS:-3}
KEYS="tick heap-used imm-used gc-count fn-cells boot-alloc-used heap-total imm-total"
# 陽性対照で下がることを見る量 / 陰性対照で動いてはいけない量
POS_KEYS="imm-used heap-used tick"
NEG_KEYS="boot-alloc-used heap-total imm-total"

run_config() {   # $1=構成名 $2...=make へ渡す追加の変数
    name=$1; shift
    echo "=== [M1-T6 / #185] $name ($RUNS 回) ==="
    # **ビルドの印を出力へ埋める**(規則 9 の「設定値は出力に埋める」)
    echo "--- build flag signature: [$(make -s print-build-flag-signature "$@")]"
    i=1
    while [ "$i" -le "$RUNS" ]; do
        rm -f test-results.txt
        make test-qemu-milestone MILESTONE="$MILESTONE" "$@"
        cp test-results.txt "tmp/boot_cost_${name}_${i}.txt"
        i=$((i + 1))
    done
    echo
}

# $1=構成名 $2=キー -> その構成の R 個の値を空白区切りで
vals() {
    i=1
    while [ "$i" -le "$RUNS" ]; do
        sed -n "s/^#m1 $2=\([0-9]*\)$/\1/p" "tmp/boot_cost_$1_${i}.txt" | head -1 | tr '\n' ' '
        i=$((i + 1))
    done
}

# 空白区切りの数列 -> "min median max"(median は奇数回なら中央、偶数回なら下側中央)
stats() {
    echo "$1" | tr ' ' '\n' | grep -v '^$' | sort -n | awk '
        {v[NR]=$1}
        END{ if (NR==0) {print "? ? ?"; exit}
             printf "%s %s %s", v[1], v[int((NR+1)/2)], v[NR] }'
}
median() { stats "$1" | awk '{print $2}'; }

run_config embedded
run_config no_embedded NO_EMBEDDED_INIT=1

{
  echo "# 同梱した基盤ライブラリを起動ごとに読む費用(M1-T6 / issue #185)"
  echo
  echo "測定日時: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "HEAD: $(git rev-parse --short HEAD)$(git diff --quiet || echo ' (作業ツリーに未コミットの差分あり)')"
  echo "QEMU: $(qemu-system-x86_64 --version | head -1)"
  echo "QEMU_MEM: $(make -s print-qemu-mem) / KVM: 使っていない(TCG)"
  echo "台本: $MILESTONE / 1 構成あたり $RUNS 回"
  echo
  echo "## 同梱したソースの大きさ(導出。規則 13)"
  echo
  wc -c src/lisp/init.lisp src/lisp/disassemble.lisp | sed 's/^/    /'
  echo
  echo "## 定義の数(導出。規則 13)"
  echo
  for k in defun defmacro defmethod defclass defgeneric defdynamic defconstant defglobal; do
      n=$(grep -hc "^($k " src/lisp/init.lisp src/lisp/disassemble.lisp | paste -sd+ - | bc)
      printf '    %-12s %s\n' "$k" "$n"
  done
  echo
  echo "## 実測(min / median / max。差は median − median)"
  echo
  printf '    %-17s %-26s %-26s %12s\n' 量 "同梱あり" "同梱なし" "差(median)"
  for k in $KEYS; do
      sa=$(stats "$(vals embedded "$k")")
      sb=$(stats "$(vals no_embedded "$k")")
      ma=$(echo "$sa" | awk '{print $2}'); mb=$(echo "$sb" | awk '{print $2}')
      fa=$(echo "$sa" | awk '{printf "%s / %s / %s", $1,$2,$3}')
      fb=$(echo "$sb" | awk '{printf "%s / %s / %s", $1,$2,$3}')
      printf '    %-17s %-26s %-26s %12s\n' "$k" "$fa" "$fb" "$((ma - mb))"
  done
  echo
  imm_a=$(median "$(vals embedded imm-used)")
  imm_b=$(median "$(vals no_embedded imm-used)")
  imm_t=$(median "$(vals embedded imm-total)")
  echo "    imm 消費の総量に対する割合(同梱あり): $(awk "BEGIN{printf \"%.3f%%\", 100*$imm_a/$imm_t}")"
  echo "    imm 消費の総量に対する割合(同梱なし): $(awk "BEGIN{printf \"%.3f%%\", 100*$imm_b/$imm_t}")"
  echo "    同梱ぶんの割合(差):                   $(awk "BEGIN{printf \"%.3f%%\", 100*($imm_a-$imm_b)/$imm_t}")"
  echo
  echo "    ## 独立な裏づけ: 1 定義あたりの imm 消費(規則 14 の「別の形の証拠」)"
  ndef=$(grep -hc '^(defun \|^(defmacro \|^(defmethod ' src/lisp/init.lisp src/lisp/disassemble.lisp | paste -sd+ - | bc)
  nfc_a=$(median "$(vals embedded fn-cells)"); nfc_b=$(median "$(vals no_embedded fn-cells)")
  echo "      defun+defmacro+defmethod の件数 = $ndef(導出)"
  echo "      imm の差 / 件数                  = $(awk "BEGIN{printf \"%.0f byte/定義\", ($imm_a-$imm_b)/$ndef}")"
  echo "      Function Cell の増加             = $((nfc_a - nfc_b)) 個"
  echo "      imm の差 / Function Cell         = $(awk "BEGIN{printf \"%.0f byte/cell\", ($imm_a-$imm_b)/($nfc_a-$nfc_b)}")"
  echo "      比較対象(既存の実測値): documents/performance-measurement.md は"
  echo "      20 defun で 71,008 byte = **約 3,550 byte/defun** を記録している"
  echo
  tick_a=$(median "$(vals embedded tick)"); tick_b=$(median "$(vals no_embedded tick)")
  echo "    tick の差を秒に直すと: $(awk "BEGIN{printf \"%.2f 秒\", ($tick_a-$tick_b)/100}") (1 tick = 10ms)"
  ta_s=$(stats "$(vals embedded tick)")
  echo "    tick のぶれ(同梱あり、$RUNS 回の max − min): $(echo "$ta_s" | awk '{print $3-$1}')"
  tb_s=$(stats "$(vals no_embedded tick)")
  echo "    tick のぶれ(同梱なし、$RUNS 回の max − min): $(echo "$tb_s" | awk '{print $3-$1}')"
  echo
  echo "## 確保量は本当にぶれないか(規則 2 / 規則 9 の検算)"
  echo
  for k in heap-used imm-used; do
      for c in embedded no_embedded; do
          s=$(stats "$(vals $c "$k")")
          w=$(echo "$s" | awk '{print $3-$1}')
          if [ "$w" = 0 ]; then
              printf '    [一致] %-10s %-12s %s 回すべて同じ値\n' "$k" "$c" "$RUNS"
          else
              printf '    [ぶれ] %-10s %-12s 幅 %s。**「確保量はぶれない」が成り立っていない**\n' "$k" "$c" "$w"
          fi
      done
  done
  echo
  echo "## 陽性対照の判定(規則 8)"
  echo
  echo "    同梱を外して **3 つとも下がる**ことが発火の条件である。"
  pc_ok=0
  for k in $POS_KEYS; do
      a=$(median "$(vals embedded "$k")"); b=$(median "$(vals no_embedded "$k")")
      if [ "$a" -gt "$b" ]; then
          printf '    [発火] %-10s %s -> %s(下がった)\n' "$k" "$a" "$b"
      else
          printf '    [不発] %-10s %s -> %s。**この計器は同梱を見られていない**\n' "$k" "$a" "$b"
          pc_ok=1
      fi
  done
  echo
  if [ "$pc_ok" = 0 ]; then
      echo "    陽性対照: **合格**(3 つとも下がった)"
  else
      echo "    陽性対照: **不発あり**。不発の量については「差が無い」ではなく"
      echo "              「この計器では見えなかった」と書くこと(規則 10)"
  fi
  echo
  echo "## 陰性対照の判定(規則 5)"
  echo
  echo "    同梱と無関係な量は動いてはいけない(boot allocator は run_boot_entry より前に確定する)。"
  nc_ok=0
  for k in $NEG_KEYS; do
      a=$(median "$(vals embedded "$k")"); b=$(median "$(vals no_embedded "$k")")
      if [ "$a" = "$b" ]; then
          printf '    [静止] %-17s %s = %s\n' "$k" "$a" "$b"
      else
          printf '    [動いた] %-17s %s -> %s。**測定が壊れている可能性**\n' "$k" "$a" "$b"
          nc_ok=1
      fi
  done
  echo
  if [ "$nc_ok" = 0 ]; then
      echo "    陰性対照: **合格**(3 つとも動かなかった)"
  else
      echo "    陰性対照: **動いた量がある。原因を書くまで数字を使ってはいけない**"
  fi
} | tee "$OUT"

echo
echo "書き出し: $OUT(素の出力は tmp/boot_cost_<構成>_<n>.txt)"
