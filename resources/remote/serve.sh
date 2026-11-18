# astra-serve v1
# File-streaming server for ASTRA remote grid fitting. Started by the client
# as `ssh <host> sh <path>/serve.sh`; speaks a line-framed request/response
# protocol on stdin/stdout. Pure POSIX sh plus wc/cat/cp/tail/head/find/rm,
# all verified present on the target hosts. Paths must not contain
# whitespace (the client enforces this).
#
#   PING              ->  PONG
#   STAT <path>       ->  SIZE <bytes>            | ERR <code> <msg>
#                         (404 absent, 410 a directory on the way absent,
#                          503 cannot tell)
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
      # 404 only when the file is definitely absent: its directory can be
      # read and the file is not in it. A dead mount or an unreadable
      # directory is 503, so the caller does not take an outage for a hole
      # in a model grid.
      # 410 when a directory on the way does not exist, but the nearest one
      # that does can be listed: the whole subtree is absent from this host
      # (a grid kept on another host), not unreachable.
      if [ -f "$a" ]; then
        printf 'SIZE %s\n' "$(wc -c < "$a")"
      elif [ -r "${a%/*}" ] && ls "${a%/*}" >/dev/null 2>&1; then
        printf 'ERR 404 %s\n' "$a"
      else
        d="${a%/*}"
        while [ -n "$d" ] && [ ! -e "$d" ]; do d="${d%/*}"; done
        [ -n "$d" ] || d=/
        if [ "$d" != "${a%/*}" ] && [ -d "$d" ] && [ -r "$d" ] &&
           ls "$d" >/dev/null 2>&1; then
          printf 'ERR 410 %s\n' "$a"
        else
          printf 'ERR 503 %s\n' "$a"
        fi
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
