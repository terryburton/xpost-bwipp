[![Linux CI](https://github.com/luser-dr00g/xpost/actions/workflows/linux.yml/badge.svg)](https://github.com/luser-dr00g/xpost/actions/workflows/linux.yml)
[![Windows CI](https://github.com/luser-dr00g/xpost/actions/workflows/msys2.yml/badge.svg)](https://github.com/luser-dr00g/xpost/actions/workflows/msys2.yml)
[![OS X CI](https://github.com/luser-dr00g/xpost/actions/workflows/osx.yml/badge.svg)](https://github.com/luser-dr00g/xpost/actions/workflows/osx.yml)

## Xpost

Xpost is a cross-platform interpreter for the PostScript Language
written in C. It implements LanguageLevel 3, graphics included, and is
built and tested on Linux, Windows and macOS.

The whole interpreter is a library, `libxpost`. The `xpost` program is a
small application over it (`src/bin/xpost_main.c`), and
`src/bin/xpost_client.c` is a smaller example of embedding it in
something else.

A page can be painted into a raster (PGM, PPM, PBM, TIFF, PNG, JPEG), a
window (X11 or Windows), a page description (PDF, PostScript with the
Document Structuring Conventions, SVG), a framebuffer lent by the
calling program, or nothing at all. Large pages need not be held whole:
six of the raster devices can take their page a band at a time.
`doc/MANUAL` lists every device and says how to choose one.

The core of the interpreter was written by M Joshua Ryan (luser droog).
The autotools build system, logging system, and win32 device were
written by Vincent Torri. Individual files bear the copyright of their
respective contributors.

Xpost is distributed under the BSD 3-clause licence; see `COPYING`.

## Building

Meson is the build the tree is developed and released against, and the
one every CI lane uses. It wants meson 1.0 or later, ninja, and a C
compiler.

```
  meson setup builddir
  ninja -C builddir
  ninja -C builddir install
```

Every library it uses is optional and sought rather than required, so a
build with none of them present still produces an interpreter: libpng
buys the png and pngalpha devices, libjpeg the jpeg device, freetype and
fontconfig the scalable fonts and finding them by name, xcb the X11
window device, and zlib the Flate filters and the compressed streams in
a PDF.

Install somewhere else:

```
  meson setup -Dprefix=/foo/bar builddir
```

and to see what an already configured build took:

```
  meson configure builddir
```

The option worth knowing about is `-Dlarge-object=true`, which widens
the fields of the PostScript object. The tree is built and tested at
both widths; the narrow one is primary and neither is ever dropped.

To build the Doxygen documentation, into `builddir/doc`:

```
  meson compile -C builddir doc
```

`meson compile -C builddir splint` runs splint, where splint is
installed; the target does not exist in a build configured without it.

The tree also carries an autotools build (`./autogen.sh`, `make`). It
builds the same programs, but it is not what CI builds and not what the
tree is developed against, so meson is the one to reach for.

## The test suite

```
  meson test -C builddir
```

That runs all of it. Five named profiles say how much, from the one you
can afford between edits to the one that leaves nothing out:

```
  ninja -C builddir quick        the fast tests             -- while editing
  ninja -C builddir full         every cost                 -- before a commit
  ninja -C builddir corpus       the differential corpora
  ninja -C builddir vendor       a downstream consumer's suite
  ninja -C builddir everything   all of it, and nothing skipped
```

`everything` is the only one of the five that is a verdict on the tree:
it refuses to pass while any test merely skipped, so in a checkout whose
corpora have not been fetched it fails, and says which tests it failed
over.

They are meson suite selections, and the suites are two independent
axes: what a test is about (`xpost`, `corpus`, `vendor`, `memacct`) and
what it costs (`fast`, `slow`, `veryslow`). Either can be named without
the other, so any crossing is available by hand -- the corpora that run
quickly, without the ones that take minutes, being the useful one:

```
  meson test -C builddir --suite corpus --no-suite veryslow
```

Prefer the targets to their raw filters. A meson filter naming a suite
no test carries matches nothing and still exits zero, so a mistyped
`--no-suite` runs the whole suite and a mistyped `--suite` runs none of
it, both reporting success. The targets go through
`tests/run-profile.sh`, which works out what the profile names from the
test listing and refuses unless the filter selects exactly that.

Every test declares its cost where it is registered in `meson.build`,
and `check-test-cost` fails if one does not, so a slow test added later
cannot quietly settle into the quick profile.

There are two cost profiles and three cost tags because nothing the
tree runs out of itself is `veryslow` -- the tag is carried by two of
the corpora -- so a selection stopping at the top of `slow` and one
going past it name the same tests. `quick` is also the profile without
a leak checker in it: the three tests that run one are all `slow`,
because the checker's cost here is interpreter start-up rather than the
workload. What survives `quick` is what leaves every assertion true --
memory held past its last use, a read outside the object it belongs to
that hands back a plausible value -- which is why the run before a
commit is `full`.

The differential corpora under `tests/corpus` are part of the suite;
they are fetched on demand and skip until you populate them (see
`tests/corpus/README.md`).

The `vendor` profile runs the test suite of a downstream consumer,
Barcode Writer in Pure PostScript, out of a checkout of its own. It is
large real-world PostScript and it exercises the language the way a
program does rather than the way a conformance test does. Point
`BWIPP_DIR` at the checkout, build its monolithic `barcode.ps`, and the
profile runs it; without one it skips, so it is never a build
dependency.

A profile is not the only way to select. `tests/gate.sh` runs the tests
a *change* can be answered by, at the widths it can be wrong at, which
is what to run in the edit-run-edit loop. `doc/GATING.md` says which run
answers which question.

## Where the rest is written down

| | |
| --- | --- |
| `doc/MANUAL` | using xpost: building it, running it, the devices, the interactive session, and the language by example |
| `doc/CONTRIBUTING.md` | working on xpost: the gate, the guards, the corpora, and what a commit looks like |
| `doc/GATING.md` | which test run answers which question |
| `doc/xpost_design.dox` | how it works: the module map, the memory, the object, the operators, the devices, the recorded page and the band loop. The Design page of the generated reference |
| `doc/xpost_roots.dox` | what the garbage collector marks from, and which bank each root lives in |
| `doc/COMPAT` | where each user-visible name comes from |
| `COMPLIANCE` | operator by operator: implemented, partly, or not; then the behavioural deviations and the implementation limits |
| `doc/xpost_dicts.dox` | every dictionary the interpreter carries, and what belongs in it |

## Support

Questions about Xpost can be addressed in the Google Group
[xpost-discuss](https://groups.google.com/g/xpost-discuss) or in the
GitHub issues.
