# TODO

This list was written in March 2016 and not touched again. Read as a
statement of what is outstanding it was simply wrong: half of it had been
done and went on being listed as work to do, which is worse than an empty
file, because a stale backlog is read as a live one and stops anyone
looking.

So it is kept as what it honestly is -- the 2016 list, checked item by
item against the tree on 2026-08-14 and again on 2026-08-26, each item
marked with where it stands and where to look. The second pass was worth
making: three of the entries the first had marked open or accurate had
stopped being either, one of them the day after it was written. It is not maintained as work arrives and never
was, so an item's absence from it says nothing whatever.

## Short term (as of 2016)

**Size parameters to `xpost_create()`.** Done. The signature takes
`Xpost_Set_Size set_size, int width, int height` (`src/lib/xpost.h`).

**Auto-kerning control parameter.** Not done, and settled the other way:
`show` does not kern, pair adjustment being the program's business
through `kshow` and `ashow`, which are implemented
(`src/lib/xpost_op_font.c`, COMPLIANCE). Nothing is waiting on this.

**Collapse the object flags `extended_int`, `extended_real` and
`opargsinhold`.** Open. All three tag bits are still there, spelt
`XPOST_OBJECT_TAG_DATA_EXTENDED_INT`, `..._EXTENDED_REAL` and
`XPOST_OBJECT_TAG_DATA_FLAG_OPARGSINHOLD` in `src/lib/xpost_object.h`.

**GC controls in `xpost_op_param.c:vmreclaim()`.** Done. All five
operands act: -2 and -1 turn automatic collection off for both banks or
for the local one, 0 turns it on again, and 1 and 2 perform an immediate
collection of the local bank or of both (PLRM 8.2). `doc/ROOTS` says what
a collection marks from.

**More unit tests.** The suite is 430 tests at two object widths, with
corpora of real programs beside it. Retired as an item: it named no
particular gap, and `doc/COVERAGE.md` ranks the ones that exist by
consequence, which is the form that can actually be worked from.

**Infrastructure for coverage reports.** Done. `tools/coverage.sh`
builds an instrumented tree, runs a named profile in it and writes
`doc/COVERAGE.md` and `doc/COVERAGE-large.md`, one per object width.

**Documentation on the web site.** Nothing in this tree can answer this
one; it is about a site, not about the source.

**Visual Studio installer.** Settled the other way, and no longer
possible to want: the `visual_studio/` directory this described was
deleted on 2026-08-15 ("build: delete three artefacts no build has read
for a decade"), the day after the entry was written, and MSVC is not a
supported toolchain. Windows is built with msys2/mingw, which CI covers.

## Longer term (as of 2016)

**Re-examine the split between `xpost_matrix.c` and `xpost_op_matrix.c`,
which converts back and forth between matrix formats.** Open. Both files
are still there. `doc/INTERNALS` explains why the internal matrix is
the transpose of the PLRM's.

**Remove optab from VM, thus removing all pointers; remove
`xpost_free_realloc()`.** Half done. `xpost_free_realloc` is gone, taken
out when an operator's signatures moved inside the table that names them.
The operator table is still a special entity of global virtual memory
(`XPOST_MEMORY_TABLE_SPECIAL_OPERATOR_TABLE`), so the first half stands.

**Extensible search for the PostScript init files, or resource-compile
them into the executable.** Partly, and the second half has been answered
another way. `-I DIR` adds a resource search directory for what a program
asks for, and the files the interpreter boots from are still read from
the data directory. What compiling them in was for -- not paying to build
the language on every run -- is what the image of virtual memory now
does: a run reads the language back instead of assembling it, in about a
sixth of the time. See "The image of virtual memory" in `doc/INTERNALS`.

**Anti-aliasing, Porter/Duff compositing, an alpha channel, and the
`/DeviceN` colour space.** Three of the four are done. `/DeviceN` is
implemented. Glyphs are anti-aliased -- `/TextAlphaBits` is a device
parameter naming how many bits of a glyph's edge coverage the device is
sent, 8 by default (`data/image.ps`), and a bilevel page takes it back
down to 1. An output alpha channel is the `pngalpha` device.
Porter/Duff compositing is not implemented and no operator asks for it.

**Expose Type 1 font data to PostScript, and accept a modified Type 1
font dict in `definefont`.** Done; COMPLIANCE marks both, and a face
whose program cannot be read publishes glyph indices instead.

**Vatti or Weiler/Atherton clipping for `clip` and `eoclip`, and with it
`fill` and `eofill`.** Done as a capability: `fill` and `clip` with
complex paths, `eofill` and `eoclip` are all implemented and marked so in
COMPLIANCE. Which algorithm is behind them is not recorded and the item
should not be read as naming one.
