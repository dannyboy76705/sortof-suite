#!/bin/bash
# runall.sh - the whole test in one go: encode -> natural list -> histogram orders -> archive sizes.
#   bash runall.sh -y EP1.y4m [-y EP2.y4m ...] -w BASEDIR [-q 90] [-R "1"] [-j "5"] [-c 10] [-D] [-r] [-C]
#                  [-t N] [-z "zstd -9 -q"] [-x TOOLSDIR] [-m MJPEGSTORE]
#   -y FILE   a y4m episode (repeat for more episodes: each goes into its own clip folder ep1, ep2, ...)
#   -w DIR    work folder; one subfolder per restart setting (R1, R9, ...), plus DIR/results.tsv
#   -q N      JPEG quality (default 90)         -R "LIST"  restart settings to try, MCUs per piece (default "1";
#                                                  0 = one piece per row of blocks, the old way), e.g. -R "0 2 3 5 9"
#   -j "LIST" palscore -j values to try (default "5")   -c PCT  palscore scene cut (default 10)
#   -D        also chain byte-identical files behind their first copy (natural and every pal order)
#   -r        also print the repeats-more-than-128-files-back line for the natural list (needs cdcmaster, from the dos-games folder; slow)
#   -C        delete the encoded pictures of a setting when it is finished (lists and results are kept)
#   -z CMD    compressor used to size the lists (default "zstd -9 -q"); the archive is only counted, never kept
#   -t N      threads (default 4)   -x DIR  where sortof-natural, sortof-palscore (and cdcmaster for -r) are (default: next to this script)
#   -m FILE   the mjpegstore binary (default TOOLSDIR/mjpegstore)
# results.tsv is appended to, so a restarted run keeps the rows already in it.
# Prints one table: size of every list's archive and its change against the natural list of the same setting.
set -u
tools="$(dirname "$0")"; work=""; q=90; rlist="1"; jlist="5"; cut=10; dup=0; rep=0; clean=0; thr=4
zc="zstd -9 -q"; mstore=""; ys=()
while getopts "y:w:q:R:j:c:t:z:x:m:DrCh" o; do case $o in
  y) ys+=("$OPTARG");; w) work=$OPTARG;; q) q=$OPTARG;; R) rlist=$OPTARG;; j) jlist=$OPTARG;; c) cut=$OPTARG;; t) thr=$OPTARG;;
  z) zc=$OPTARG;; x) tools=$OPTARG;; m) mstore=$OPTARG;; D) dup=1;; r) rep=1;; C) clean=1;;
  *) sed -n '2,19p' "$0"; exit 1;; esac; done
[ ${#ys[@]} -ge 1 ] && [ -n "$work" ] || { sed -n '2,19p' "$0"; exit 1; }
[ -n "$mstore" ] || mstore=$tools/mjpegstore
for t in "$tools/sortof-natural" "$tools/sortof-palscore" "$mstore"; do [ -x "$t" ] || { echo "need $t" >&2; exit 1; }; done
[ $rep = 0 ] || [ -x "$tools/cdcmaster" ] || { echo "need $tools/cdcmaster for -r" >&2; exit 1; }
for y in "${ys[@]}"; do [ -f "$y" ] || { echo "no such file: $y" >&2; exit 1; }; done
$zc < /dev/null > /dev/null 2>&1 || { echo "compressor '$zc' does not run" >&2; exit 1; }
mkdir -p "$work" || exit 1
res="$work/results.tsv"
[ -s "$res" ] || printf "setting\tlist\tbytes\tvs_natural_pct\n" > "$res"

size_of() { tar -cf - -T "$1" | $zc | wc -c; }

# chain duplicates: chain_dups HASHTSV LIST > OUT
chain_dups() {
  awk -F'\t' 'NR==FNR { h[$2]=$1; next }
    { p=$0; if (!(p in h)) miss++; x=h[p]; list[++n]=p; hx[n]=x
      if (x in first) { nxt[last[x]]=n; last[x]=n; d++ } else { first[x]=n; last[x]=n } }
    END { if (miss) { print "ERROR: " miss " paths without hash" > "/dev/stderr"; exit 1 }
          for (i=1;i<=n;i++) { if (first[hx[i]]!=i) continue; for (j=i; j!=""; j=nxt[j]) print list[j] } }' "$1" "$2"
}

for R in $rlist; do
  d="$work/R$R"
  mkdir -p "$d"
  echo "=== restart $R: encode (q$q) ===" >&2
  if [ ! -d "$d/set" ]; then
    ropt=""; [ "$R" = 0 ] || ropt="-R $R"
    i=0
    for y in "${ys[@]}"; do
      i=$((i+1))
      "$mstore" -y -t "$thr" -i "$y" -j "$q" $ropt "$d/set/ep$i" < /dev/null || { echo "encode failed ($y)" >&2; exit 1; }
    done
  else
    echo "(pictures already in $d/set, reusing)" >&2
  fi
  find "$d/set" -type f > "$d/in.txt"
  "$tools/sortof-natural" -t "$thr" -q -f "$d/natural.tmp" - < "$d/in.txt" || exit 1
  { grep '/specs\.txt$' "$d/natural.tmp"; grep -v '/specs\.txt$' "$d/natural.tmp"; } > "$d/natural.txt"   # specs.txt first, as the store's own list has it
  rm -f "$d/natural.tmp"
  nat_sz=$(size_of "$d/natural.txt")
  printf "R%s\tnatural\t%s\t0.00\n" "$R" "$nat_sz" >> "$res"
  echo "R$R: $(wc -l < "$d/natural.txt") files, natural archive $nat_sz bytes" >&2
  lists="natural"
  if [ $rep = 1 ]; then
    mkdir -p "$d/spill"
    "$tools/cdcmaster" -a 8 -t "$thr" -J -q -d "$d/spill" "$d/natural.txt" > "$d/rep.natural.txt" 2>/dev/null
    awk -v k="R$R" '/^Repeats:/{r=1;next} r&&/^  [0-9]/{ split($1,a,"-"); lo=a[1]+0; n=$2+0; tot+=n; if(lo>=128)f128+=n; if(lo>=2048)f2k+=n }
      END{printf "R%s natural  repeats more than 128 files back: %5.2f%%   more than 2048 back: %5.2f%%\n",substr(k,2),100*f128/tot,100*f2k/tot}' "$d/rep.natural.txt" >&2
    rm -rf "$d/spill"
  fi
  for j in $jlist; do
    echo "=== restart $R: histogram order -j $j ===" >&2
    "$tools/sortof-palscore" -q -t "$thr" -c "$cut" -j "$j" -o "$d/pal$j.txt" -w "$d/sigs.bin" "$d/natural.txt" | grep '^order' >&2
    lists="$lists pal$j"
  done
  if [ $dup = 1 ]; then
    echo "=== restart $R: hashing for duplicates ===" >&2
    tr '\n' '\0' < "$d/natural.txt" | xargs -0 md5sum | awk '{ print substr($0,1,32) "\t" substr($0,35) }' > "$d/hash.tsv" || exit 1
    for k in $lists; do
      chain_dups "$d/hash.tsv" "$d/$k.txt" > "$d/$k.D.txt" || exit 1
    done
    for k in $lists; do lists="$lists $k.D"; done
  fi
  for k in $lists; do
    [ "$k" = natural ] && continue
    sz=$(size_of "$d/$k.txt")
    pct=$(awk -v a="$sz" -v b="$nat_sz" 'BEGIN{printf "%.2f", 100*(a-b)/b}')
    printf "R%s\t%s\t%s\t%s\n" "$R" "$k" "$sz" "$pct" >> "$res"
    echo "R$R: $k archive $sz bytes ($pct% vs natural)" >&2
  done
  [ $clean = 0 ] || { rm -rf "$d/set"; echo "(pictures of R$R deleted)" >&2; }
  echo "=== restart $R done ===" >&2
done
echo
if command -v column >/dev/null; then column -t -s "$(printf '\t')" "$res"; else cat "$res"; fi
echo "table also in $res"
