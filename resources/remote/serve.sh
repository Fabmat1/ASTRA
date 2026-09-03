# astra-serve v1
# File-streaming server for ASTRA remote grid fitting. Started by the client
# as `ssh <host> sh <path>/serve.sh`; speaks a line-framed request/response
# protocol on stdin/stdout. Pure POSIX sh plus wc/cat/cp/tail/head/find/rm,
# all verified present on the target hosts. Paths must not contain
# whitespace (the client enforces this).
#
#   PING              ->  PONG
#   STAT <path>       ->  SIZE <bytes>            | ERR <code> <msg>
#   GET <path>        ->  DATA <bytes>\n<bytes raw>| ERR <code> <msg>
#   TAIL <off> <path> ->  DATA <bytes>\n<bytes raw> (from offset to EOF)
#   LIST <depth> <dir>->  BEGIN ... one path per line ... END
#   BYE               ->  exit
#
# GET snapshots the file to a temp copy first: files like status.json are
# atomically replaced by their writer, and announcing one inode's size but
# streaming another's bytes would corrupt the framing.

tmp="${TMPDIR:-/tmp}/astra_serve_$$"
trap 'rm -f "$tmp"' EXIT INT TERM

while read -r cmd a b; do
  case "$cmd" in
    PING)
      printf 'PONG\n' ;;
    STAT)
      if [ -f "$a" ]; then
        printf 'SIZE %s\n' "$(wc -c < "$a")"
      else
        printf 'ERR 404 %s\n' "$a"
      fi ;;
    GET)
      if [ -f "$a" ] && cp "$a" "$tmp" 2>/dev/null; then
        printf 'DATA %s\n' "$(wc -c < "$tmp")"
        cat "$tmp"
        rm -f "$tmp"
      else
        printf 'ERR 404 %s\n' "$a"
      fi ;;
    TAIL)
      if [ -f "$b" ]; then
        s=$(wc -c < "$b")
        n=$((s - a))
        [ "$n" -lt 0 ] && n=0
        printf 'DATA %s\n' "$n"
        if [ "$n" -gt 0 ]; then
          tail -c "+$((a + 1))" "$b" | head -c "$n"
        fi
      else
        printf 'ERR 404 %s\n' "$b"
      fi ;;
    LIST)
      printf 'BEGIN\n'
      find "$b" -maxdepth "$a" -name grid.fits 2>/dev/null
      printf 'END\n' ;;
    BYE)
      exit 0 ;;
    *)
      printf 'ERR 400 %s\n' "$cmd" ;;
  esac
done
