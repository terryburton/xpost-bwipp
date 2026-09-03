A corpus of your own
====================

This directory is a slot. Nothing is kept here, and nothing is fetched
into it: put PostScript programs in it and the corpus runs them the way
the others beside it are run -- each one through this interpreter and
through the reference engine, page by page, with what they draw compared.

Programs are not committed. The `.gitignore` a level up excludes `*.ps`
and `*.eps` throughout, so what you put here stays yours; only the four
registers below are this repository's.

    cp /somewhere/*.ps tests/corpus/local/
    meson test -C build corpus-local

With nothing in it the test skips, so an empty slot costs a tree nothing.

Declaring what the programs do
------------------------------

A corpus is not a directory of files; it is a directory of files whose
behaviour has been written down. The run compares what it measured
against what the registers declare, and a program the registers do not
mention is a program the run cannot judge -- so adding programs means
adding their entries too.

    pages       one basename and a page count per line, for every
                program that draws a page
    nopage      a basename, or a basename and " pN", for a program or a
                page that draws nothing -- each with the reason
    displaced   a basename and " pN" where the two engines put ink in
                places the other left bare, with the reason
    heldout     a basename per line for a program the run does not run
                at all, with the reason

Each takes `#` comments. A reason that measurement contradicts is worse
than no entry at all: it is read as a known cost and stops anyone looking
again. Write what was measured, and remove an entry the day its reason
stops holding.
