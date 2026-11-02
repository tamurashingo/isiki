#!/bin/sh
# 2 パスビルドでシンボルアドレスがずれていないことを確かめる
# (documents/disasm-symbols.md §3-1)。
#
# **これが無いと、将来リンカの挙動が変わったときに黙って古いテーブルを
# 積んだイメージができる。** 名前が 1 つずつずれても、それらしい名前が
# 出てしまうので気づきにくい。
#
# **この検出器自体も試すこと。** 最初の実装は `diff -q - /dev/stdin` を
# ヒアドキュメントと併用していて常に真を返し、**アドレスを書き換えても
# 素通りしていた**(指示書 §6-6 の「試していない検出器は働かない」)。
set -eu
P1="$1"
P2="$2"
TMP1=$(mktemp)
TMP2=$(mktemp)
trap 'rm -f "$TMP1" "$TMP2"' EXIT

extract() {
  grep -E ' [Tt] ' "$1" | grep -v ' \.text$' | sort -k1,1 | awk '{ print $1, $3 }'
}
extract "$P1" > "$TMP1"
extract "$P2" > "$TMP2"

if cmp -s "$TMP1" "$TMP2"; then
  exit 0
fi

echo "" >&2
echo "ERROR: シンボル表を含めて再リンクしたらアドレスが動きました。" >&2
echo "  逆アセンブルの名前が **ずれた状態** になるため、ビルドを止めます。" >&2
echo "  (documents/disasm-symbols.md §3 の鶏と卵)" >&2
echo "" >&2
echo "--- 動いたシンボル(先頭10件) ---" >&2
diff "$TMP1" "$TMP2" | head -20 >&2 || true
exit 1
