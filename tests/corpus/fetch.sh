#!/bin/sh
# Populate the differential corpora. Each corpus lives in its own
# directory here; the programs it holds are fetched or copied from
# their own source, never committed (see README.md). Every corpus is
# best-effort and independent: one that cannot be obtained leaves the
# others alone.
#
#   fetch.sh                 populate every corpus it can
#   fetch.sh ghostscript     just one
#   BWIPP=/path/to/checkout fetch.sh bwipp
#
# Two of them are copied off the machine rather than downloaded: the
# consumer's examples, and the Type 1 font programs, which belong to
# their makers and are already installed wherever anything typesets.
#
# Best-effort is about which corpora are obtainable, not about whether
# the caller is told. A program that did not arrive is counted and the
# script ends non-zero, because the alternative is a populated corpus
# that is quietly one program short: the run then evaluates what is
# there, reports what it evaluated, and the missing program is a
# question nobody knows went unasked. What a failed download left behind
# is removed for the same reason -- an interrupted transfer leaves a
# partial file and a refused one can leave an empty file, and either
# reads as a program of the corpus.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
missing=0

get() {   # url outfile
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --max-time 60 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -T 60 -O "$2" "$1"
    else
        echo "fetch: need curl or wget" >&2; return 1
    fi
}

fetch_ghostscript() {
    base="https://raw.githubusercontent.com/ArtifexSoftware/ghostpdl/master/examples"
    d="$here/ghostscript"; mkdir -p "$d"
    for f in alphabet.ps colorcir.ps doretree.ps escher.ps golfer.eps \
             grayalph.ps ridt91.eps snowflak.ps spots.ps tiger.eps \
             vasarely.ps waterfal.ps; do
        if get "$base/$f" "$d/$f" && [ -s "$d/$f" ]; then
            echo "  ghostscript/$f"
        else
            echo "  MISS ghostscript/$f"; rm -f "$d/$f"; missing=$((missing + 1))
        fi
    done
}

fetch_casselman() {
    base="https://personal.math.ubc.ca/~cass/graphics/manual/pdf"
    d="$here/casselman"; mkdir -p "$d"
    n=1
    while [ "$n" -le 15 ]; do
        if get "$base/ch$n.ps" "$d/ch$n.ps" && [ -s "$d/ch$n.ps" ]; then
            echo "  casselman/ch$n.ps"
        else
            echo "  MISS casselman/ch$n.ps"; rm -f "$d/ch$n.ps"
            missing=$((missing + 1))
        fi
        n=$((n + 1))
    done
}

fetch_eps() {
    base="https://people.sc.fsu.edu/~jburkardt/data/eps"
    d="$here/eps"; mkdir -p "$d"
    for f in circle football_logo fsu_logo heawood icam_logo knightstour \
             mathematica petersen sc_logo scs_logo triangular_1 tutte; do
        if get "$base/$f.eps" "$d/$f.eps" && [ -s "$d/$f.eps" ]; then
            echo "  eps/$f.eps"
        else
            echo "  MISS eps/$f.eps"; rm -f "$d/$f.eps"; missing=$((missing + 1))
        fi
    done
}

fetch_bwipp() {
    # BWIPP is a local checkout, not a download: the barcode resource
    # is generated, so it is copied rather than vendored. The
    # monolithic resource becomes this corpus's prelude, prepended to
    # every example. The packaged flavour is preferred: it loads its
    # data through 125 ASCII85Decode filters, so it exercises the
    # decode-filter path the plain flavour does not.
    src=${BWIPP:-"$HOME/src/postscriptbarcode"}
    ex="$src/contrib/Examples"
    mono="$src/build/monolithic_package/barcode.ps"
    [ -f "$mono" ] || mono="$src/build/monolithic/barcode.ps"
    if [ ! -f "$mono" ] || [ ! -d "$ex" ]; then
        echo "  bwipp: no checkout at $src (set BWIPP=... ; build the monolithic resource)"
        return
    fi
    d="$here/bwipp"; mkdir -p "$d"
    if cp "$mono" "$d/prelude" && [ -s "$d/prelude" ]; then
        echo "  bwipp/prelude ($(basename "$(dirname "$mono")")/barcode.ps)"
    else
        echo "  MISS bwipp/prelude"; rm -f "$d/prelude"; missing=$((missing + 1))
    fi
    for f in "$ex"/*.ps; do
        b=$(basename "$f")
        if cp "$f" "$d/$b" && [ -s "$d/$b" ]; then
            echo "  bwipp/$b"
        else
            echo "  MISS bwipp/$b"; rm -f "$d/$b"; missing=$((missing + 1))
        fi
    done
}

fetch_type1() {
    # Type 1 font programs are already on any machine that typesets, so
    # this corpus is copied rather than downloaded -- the fonts belong
    # to their makers and neither this tree nor a download of ours is
    # the right way to get them. Each name in the register is looked for
    # in the places the systems that ship it put it, and the first copy
    # found is taken; a name nothing here has is reported and is not a
    # miss, because which fonts a machine carries is not this script's
    # to decide.
    d="$here/type1"; mkdir -p "$d"
    for want in crimson.pfb freeeuro.pfa charter.pfb nimbusroman.pfb; do
        case $want in
        crimson.pfb)
            set -- /usr/share/texlive/*/fonts/type1/*/crimson/Crimson-Roman.pfb \
                   /usr/local/texlive/*/texmf-dist/fonts/type1/*/crimson/Crimson-Roman.pfb \
                   /usr/share/texmf*/fonts/type1/*/crimson/Crimson-Roman.pfb ;;
        freeeuro.pfa)
            set -- /usr/share/groff/*/font/devps/freeeuro.pfa \
                   /usr/local/share/groff/*/font/devps/freeeuro.pfa \
                   /opt/homebrew/share/groff/*/font/devps/freeeuro.pfa ;;
        charter.pfb)
            set -- /usr/share/fonts/type1/texlive-fonts-recommended/bchr8a.pfb \
                   /usr/share/texlive/*/fonts/type1/bitstrea/charter/bchr8a.pfb \
                   /usr/local/texlive/*/texmf-dist/fonts/type1/bitstrea/charter/bchr8a.pfb \
                   /usr/share/texmf*/fonts/type1/bitstrea/charter/bchr8a.pfb ;;
        nimbusroman.pfb)
            set -- /usr/share/fonts/type1/urw-base35/NimbusRoman-Regular.t1 \
                   /usr/share/texlive/*/fonts/type1/urw/times/utmr8a.pfb \
                   /usr/local/texlive/*/texmf-dist/fonts/type1/urw/times/utmr8a.pfb ;;
        esac
        got=
        for cand in "$@"; do
            [ -f "$cand" ] || continue
            if cp "$cand" "$d/$want" && [ -s "$d/$want" ]; then
                got=$cand
                break
            fi
            rm -f "$d/$want"
        done
        if [ -n "$got" ]; then
            echo "  type1/$want ($got)"
        else
            echo "  type1/$want: this machine has no copy of it"
        fi
    done
}

fetch_adobe() {
    echo "  adobe: not fetchable (Adobe copyright, no canonical download)."
    echo "         Place flat *.ps files under $here/adobe/ -- see README SOURCES."
}

for name in ${*:-ghostscript casselman eps bwipp type1 adobe}; do
    echo "populating $name ..."
    case "$name" in
        ghostscript) fetch_ghostscript;;
        casselman)   fetch_casselman;;
        eps)         fetch_eps;;
        bwipp)       fetch_bwipp;;
        type1)       fetch_type1;;
        adobe)       fetch_adobe;;
        *)           echo "  unknown corpus: $name" >&2; missing=$((missing + 1));;
    esac
done

if [ "$missing" -ne 0 ]; then
    echo "fetch: $missing program(s) did not arrive; the corpora above are"
    echo "fetch: short by that many and a run over them will not say so" >&2
    exit 1
fi
