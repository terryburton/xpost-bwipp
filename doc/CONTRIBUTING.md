# Working on xpost

What this tree expects of a change, written down because most of it is
not obvious from the files and none of it is enforced by anything a new
contributor would meet first.

Read `doc/MANUAL` for building and running, `doc/xpost_design.dox` for
how the interpreter works -- it is the Design page of the generated
reference, with the root set and the dictionary layout beside it in
`doc/xpost_roots.dox` and `doc/xpost_dicts.dox` -- and `doc/GATING.md`
for the reference version of the test selection this page introduces.

## Two builds, always

The tree builds the PostScript object at two widths. The narrow build is
primary; the wide build is equally important and neither is ever
dropped. Set both up once and keep them:

```
  meson setup build
  meson setup blarge -Dlarge-object=true
  ninja -C build && ninja -C blarge
```

The names matter only in that everything below defaults to them
(`tests/gate.sh` reads `XPOST_NARROW_BUILD` and `XPOST_WIDE_BUILD` if
you would rather they were called something else).

A width bug is not a rare bug. Field widths, composite bounds, the
integer horizon and the shifts that pack an object all move with the
object size, and a change that is right in one build can be wrong in the
other while every test in front of you passes.

## Gate the change, not the tree

```
  tests/gate.sh --narrow build --wide blarge
```

reads what the working tree has changed, works out which areas of the
suite answer for it, runs those in the narrow build, and runs the tests
that read the object width in the wide one. A documentation change gates
in about twenty seconds; a change to the object and the memory it lives
in runs the whole suite at both widths, because that is the layer a
width is a property of.

```
  --area NAME     gate against an area outright, whatever changed
  --since REF     take the change from what REF does not have
  --list          say what would run, and stop
  --batch         every test, both widths
  -j N            tests at once (16 is the cap and the default)
```

Naming paths on the command line gates against those instead of against
the working tree, which is how to ask what a change would cost before
making it.

The table behind it is `tests/gate-map`: which part of the tree each area
answers for, and which tests answer for each area, in one table read both
ways so the two directions cannot come to disagree. `doc/GATING.md`
tabulates the areas and their sizes.

Two things stop the table quietly shrinking a gate. A path no rule
classifies falls through to a catch-all and selects the whole suite at
both widths, so the failure mode of an incomplete table is a slow gate
and never a small one. And every test the build defines must be named by
some area, which `check-gate-map` holds the table to on every gate --
otherwise a test could become one that no proportionate selection ever
runs.

Before a branch goes anywhere:

```
  tests/gate.sh --batch --narrow build --wide blarge
```

Every test, both widths. That is one run per batch of branches rather
than one per branch per rebase: accumulate two or three green branches,
rebase them together, gate once. A rebase whose commits touch no file
the other branch touched does not need re-gating, but the disjointness
has to be shown -- `git diff --name-only` over both, with no name in
common -- rather than assumed.

The cost profiles (`ninja -C build quick`, `full`, ...) are the other
axis and answer a different question: how much of the suite ran, rather
than whether what ran could have noticed what changed. Both are worth
having and neither substitutes for the other.

## The corpora

Under `tests/corpus` are sets of real PostScript programs, rendered to
catch the faults a unit suite does not reach, because a page of real
PostScript is not a test and does things no test thought to. The
programs are **not** committed -- they belong to other people -- so each
corpus is a directory holding only what is ours, and its programs are
fetched on demand:

```
  tests/corpus/fetch.sh [name ...]      populate them from their sources
  tests/corpus/evaluate.sh [name ...]   render and compare what is there
```

Every step degrades to a skip when a corpus is absent, so none of it is
a build-time dependency, and a checkout that has fetched nothing reports
success over a suite that compared nothing. `ninja -C build everything`
is the run that refuses to pass while anything merely skipped.

A rendering difference is a lead, not a failure. What fails a corpus run
is a crash, a hang, a program the corpus named and the run did not
reach, and a disagreement between what drew and what the corpus says
draws -- in both directions, an undeclared absence being a comparison
quietly doing less than it was asked to and a declared absence that
rendered being a reason that has lapsed. The registers that make those
checkable -- `heldout`, `nopage`, `pages` -- and the reasoning behind
each are in `tests/corpus/README.md`.

**Do not fetch a corpus twice.** A fresh git worktree has the
directories and none of the programs, and the obvious move -- run
`fetch.sh` again -- asks other people's machines for a second copy of a
file already on this disk. Take it from the checkout that has one:

```
  tests/corpus/share.sh                 from the default source
  tests/corpus/share.sh /path/to/xpost  or from a named checkout
```

It copies rather than links, deliberately: a worktree is a place work
happens and gets thrown away, and a link would point a harness at
another checkout's files, where a stray write lands on the corpus
everything else is measured against.

## The guards

Beside the tests that ask what the interpreter computes are guards that
ask about the shape of the tree itself: that every device class declares
what a row of its raster costs, that every operator's signature is
written where the writer can find it, that every name a register lists
still exists, that a wrapper cannot pass a run that crashed. They live
in `tests/check-*.sh` and are the `guards` area of the map.

They are cheap and they are the part of the suite most easily made
useless, so the tree holds them to a discipline of their own.

**A guard must be able to fail.** `tests/check-test-quality.sh` encodes
nine defects found in this suite, each of which once let a test report
success over broken code: a counter written into a scratch dictionary
that `end` discards; a wrapper that captures output without the exit
status; a wrapper that accepts an empty golden file; a test whose
content was commented out; a verdict read off the last stage of a
pipeline. Read it before writing a wrapper.

**A guard must be reading the tree it was pointed at.** Every guard that
derives a path from an argument passes it through `tests/guard-paths.sh`
first, and `tests/check-guard-paths.sh` holds each one to three things
at once: it refuses a decoy path, it succeeds on the real one, and it
reports a population it refuses to let be zero. The middle one is what
separates a guard that refuses a wrong path from a guard that refuses
everything; the last is what stops a guard passing over a tree with
nothing in it, which is the dangerous answer because it looks exactly
like coverage.

**Sabotage what you have just written.** A new test or guard that passes
proves nothing until the thing it is supposed to catch has been put in
front of it and it has failed. Break the code, watch it go red, put the
code back. `doc/COVERAGE.md` ranks the untested code by consequence and
puts first the guards nothing has ever made refuse -- conditions the
suite reaches by the hundred million and never once makes come out the
other way -- because the refusing side of a guard is the whole point of
it. A dictionary-growth use-after-free was found on the far side of one
of those.

## Adding a test

Register it in `meson.build` with a cost tag: `fast`, `slow` or
`veryslow`. `check-test-cost` fails a registration that declares none, so
a slow test added later cannot quietly settle into the quick profile.

A guard, or the `golden-render` byte-identity gate, carries
`priority: guard_priority` so meson schedules it in the first seconds of a
run rather than behind the slow tests that start early for the clock -- a
break in a quick, easily-broken check is then seen and fixed at once, not
after a suite. `check-guard-priority` holds every guard to it.

Name it in `tests/gate-map` under the area it answers for.
`check-gate-map` fails a test named by no area, and fails a rule that
names no test or wins no file -- a stale rule being one that quietly
stopped selecting what it names.

A shell wrapper reaches its verdict through `tests/verdict.sh`, which
carries the rule that a run's own verdict counts only when the same run
printed no failure, and that what a run left behind and what it said are
separate answers a pass needs both of. A C test reports through
`tests/xpost_test.h`, which carries the same rule one layer down.

Renders are held to `tests/golden/manifest.sha256` and
`manifest-large.sha256`, one per object width. A refactor that is meant
to change no pixel is gated on those bytes.

## File headers

Every source file opens with a header that names the file, says in a
line or two what it holds, and states the licence by identifier. The
full licence text lives once in `COPYING`; a file points at it rather
than reproducing it, so the file's own description is the first thing
read and not the thirtieth. A PostScript file:

```
%!PS
% dscwrite.ps
%
% Emit a DSC-conformant PostScript document from the recorded page.
%
% Copyright (c) 2013-2016 Michael Joshua Ryan
% SPDX-License-Identifier: BSD-3-Clause
```

The `%!PS` magic line comes first, on its own line, and the header
comment follows it. A C file is the same idea in a block comment, naming
the product on its first line:

```
/*
 * Xpost - a PostScript Level-3 interpreter
 * xpost_dev_generic.c -- the shared PostScript base device class.
 *
 * Copyright (c) 2013-2016 Michael Joshua Ryan
 * SPDX-License-Identifier: BSD-3-Clause
 */
```

The magic line is `%!PS`, never a bare `%!`. These data files are read
by the interpreter rather than spooled, so the line is a convention and
not load-bearing, but it is written in full. `check-file-headers` holds
every file to a conforming header.

### Saying what a file is for

A header states who owns the file and under what licence. It does not say
what the file *is*, and a reader who has just opened it wants that first.
So every C source also carries a doxygen block naming itself and saying in
a line what it holds, with as much beneath that line as the file needs:

```c
/**
 * @file xpost_free.c
 * @brief The free lists: what the collector reclaimed, waiting to be
 *        handed out again.
 *
 * ... what a reader has to know before reading the rest.
 */
```

`@file` names the file it is in, so a block copied from a neighbour is
caught rather than left to mislead. `check-file-purpose` holds every C
source to one.

This arrived late, and `tests/file-purpose` was the list of files that did
not have one yet. **That list only ever shrinks, and it is now empty**: every
C source carries a block. A source in neither the list nor the rule fails, so
a new file joins the day it is added; a source in both fails too, so the list
cannot rot into a standing excuse. Adding a name back is letting one file off
a rule the rest keep, and wants an argument rather than a line.

## Writing PostScript

The data files are the interpreter's own PostScript, read into a sealed
`.xpostsys` and its neighbours at startup. A reader landing in one must
be able to tell, from the procedure in front of them and not from a
search back through the file, three things that are otherwise invisible:
which dictionary is current when it runs, which VM bank its own
definitions land in, and whether it is executed once as the file loads
or defined and left for a later caller. So every top-level procedure
carries a header that says them.

The stack effect sits on the definition line -- operands in, a dot,
results out, as `/proc {  % before  .  after`. The dot stands where the
operator's name sits in a PLRM signature; the name is on that line
already and is not repeated in the effect (`% region source  .  -`,
never `% region source  .setclipregion  -`). A procedure written on one
line is opened out so the effect has somewhere to sit rather than being
left without one. Only the polymorphic procedure states its effect in
the header instead, listing the several forms that will not fit on the
definition line.

The header above the definition is prose, in the same voice as a C
comment: what the procedure does now, never what it replaced. It does
not repeat the stack effect. Within the prose it makes the invisible
things plain, and names a caller where the caller is not obvious, and
states any constraint a change would trip -- a re-entrancy, an ordering,
a setup a caller must have done:

```
%
%  grestore until the graphics state stack stands at depth n, so that a
%  gsave opened inside the bracket -- by the machinery or by the
%  procedure -- is closed, and the state it saved is back in force.
%
%  A helper of .callout below, which is its only caller and bakes it in
%  by immediate evaluation, so it stands above .callout here. It runs
%  under whatever dictionary stack the caller of the bracket had, with
%  .xpostsys nowhere on it: a bracket is fetched out of .xpostsys and
%  executed, not called with that dictionary open.
%
/.gsunwind {  % n  .  -
```

A comment describing a definition sits directly above it, no blank line
between, so the two are never read apart.

`check-proc-spec` holds the effects to that shape, and cannot yet ask it
of the whole tree: most of these files were written before it was asked
of them. `tests/proc-spec` says which have been written up and only
those are held, so a file entered in the register can never quietly lose
an effect again, while the rest stay a work list. A pending file that
has come to satisfy the rule is reported rather than accepted, because a
register that only ever excuses is one nobody notices has stopped being
true. What the check reads is the effect alone; the prose is what
carries the orientation, and review is what holds that.

Two blank lines never separate procedures and no blank line runs them
together: one blank line stands between them. A run of closing brackets
that belongs to one expression is written on one line rather than
stacked -- `} ifelse } ifelse } ifelse`, not four lines each holding
one. An inline comment on a line of code is set off by two spaces:
`x maxx gt { /maxx x def } if  % widen the box`. A comment's prose opens
each paragraph -- the line after a bare `%`, or a block's first line --
with a capital, and leaves a wrapped continuation line, a stack effect,
and a sentence that opens on an operator's own name (which the language
spells in lower case) as they are.

Three idioms were measured on the release build and ruled on; apply them
where a path is hot, not as a blanket rewrite:

- `cond { }{ action } ifelse` (empty true branch) reads and runs faster
  as `cond not { action } if` -- about 9% off the dispatch. Worth it in
  a per-pixel, per-span or per-vertex loop; a wash on a cold path.
- Reducing a `mark a b c ...` to its first element is `counttomark
  1 sub index` / `counttomark 1 add 1 roll` / `cleartomark`, not
  `counttomark 1 sub { pop } repeat` -- the single operators beat the
  interpreted loop three-to-one and the gap grows with the count.
- The fused min/max `.maxmin` (test the far bound first, the near bound
  only when the far one did not move) is worth inlining in the hottest
  bounding-box loop and nowhere else; wrapped in a procedure call it
  gives the saving straight back.

Names resolved at run time cost a dictionary walk every time; where it
does not obscure the source, an internal name a procedure calls is bound
once at definition with `//` immediate evaluation rather than looked up
on every call.

## Changing the C

The one rule that will bite immediately, from `doc/xpost_design.dox`:

> Do Not hold a pointer while allocating.

Virtual memory may move whenever it grows, so a `char *` into it is
valid only until the next thing that allocates -- a new array, string,
dict or name, a dict grown past its maxlength, a push onto a stack past
its current level. Lines commented `//recalc` show how to refresh a
pointer where one really must be held.

Comments state what the code does now, not what it used to do or which
bug it fixes; the history is in the history. Where a comment cites the
language specification it cites it by section, and the section has to
say what the comment claims.

## The commit

The subject is a subsystem prefix, a colon, and a lowercase imperative
phrase:

```
  record: answer an entry carrying no operands with no span
  tests: trim the blanks a count arrives with
  device: state the image row writer's coverage rule as behaviour
```

The body is prose -- sentences and paragraphs, no bullet lists. It says
what was wrong, what the tree now does, and why that is the right answer,
in enough detail that someone reading the log a year later does not have
to reconstruct the reasoning from the diff. Across the last hundred
commits not one body uses a bullet.

No issue numbers. A bare `#NNN` in a message renders as a link to
whatever issue holds that number on whichever repository the message is
read through, which is not the one meant.

Where a message was written with the help of a language model, the last
line says so; every one of the last three hundred commits carries that
line.
