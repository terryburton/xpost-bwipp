AUTOMAKE_OPTIONS = subdir-objects

ACLOCAL_AMFLAGS = -I m4

MAINTAINERCLEANFILES = \
Makefile.in \
aclocal.m4 \
compile \
config.guess \
config.h.in \
config.h.in~ \
config.sub \
configure \
depcomp \
install-sh \
ltmain.sh \
m4/libtool.m4 \
m4/ltoptions.m4 \
m4/ltsugar.m4 \
m4/ltversion.m4 \
m4/lt~obsolete.m4 \
missing \
$(DIST_ARCHIVES)

EXTRA_DIST =
XPOST_CLEANFILES =

include src/lib/Makefile.mk
include src/bin/Makefile.mk
include data/Makefile.mk
include examples/Makefile.mk
include doc/Makefile.mk
include tests/Makefile.mk

sed_process = \
$(SED) \
-e 's,@VERSION\@,$(VERSION),g' \
-e 's,@VMAJ\@,$(VMAJ),g' \
-e 's,@prefix\@,$(prefix),g' \
-e 's,@exec_prefix\@,$(exec_prefix),g' \
-e 's,@libdir\@,$(libdir),g' \
-e 's,@includedir\@,$(includedir),g' \
-e 's,@pkgincludedir\@,$(pkgincludedir),g' \
-e 's,@xpost_requirements_lib_pc\@,$(xpost_requirements_lib_pc),g' \
-e 's,@xpost_requirements_lib_libs\@,$(xpost_requirements_lib_libs),g' \
< $< > $@ || rm $@

pc_verbose = $(pc_verbose_@AM_V@)
pc_verbose_ = $(pc_verbose_@AM_DEFAULT_V@)
pc_verbose_0 = @echo "  PC      " $@;

xpost.pc: $(top_srcdir)/xpost.pc.in Makefile
	$(AM_V_at)rm -f $@
	$(pc_verbose)$(sed_process)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = xpost.pc

XPOST_CLEANFILES += xpost.pc

# Unit tests

if XPOST_ENABLE_COVERAGE

lcov-reset:
	@rm -rf $(top_builddir)/coverage
	@find $(top_builddir) -name "*.gcda" -delete
	@lcov --zerocounters --directory $(top_builddir)

lcov-report:
	$(MKDIR_P) $(top_builddir)/coverage
	lcov --capture --compat-libtool --output-file $(top_builddir)/coverage/coverage.info --directory $(top_builddir)
	lcov --remove $(top_builddir)/coverage/coverage.info '*.h' --output-file $(top_builddir)/coverage/coverage.cleaned.info
	lcov --remove $(top_builddir)/coverage/coverage.cleaned.info '*/tests/*' --output-file $(top_builddir)/coverage/coverage.cleaned2.info
	genhtml --branch-coverage -t "$(PACKAGE_STRING)" -o $(top_builddir)/coverage/html $(top_builddir)/coverage/coverage.cleaned2.info
	@echo "Coverage Report at $(top_builddir)/coverage/html"

endif

# PLRM operator-example conformance suite (data/test.ps) run through the built
# interpreter -- the real interpreter test, run by `make check` regardless of
# --with-tests (it needs only the xpost binary). XPOST_DATA_DIR points the
# uninstalled binary at the source data. Mirrors the meson test('interpreter').
XPOST_PS_CONFORMANCE = \
	XPOST_DATA_DIR=$(top_srcdir)/data \
	$(top_srcdir)/tests/run-ps-test.sh \
	$(top_builddir)/src/bin/xpost$(EXEEXT) $(top_srcdir)/data/test.ps \
	'DATADIR=($(top_srcdir)/data)'

# Bounding-box device: render a known fill through -d bbox and check the box.
XPOST_BBOX_TEST = \
	XPOST_DATA_DIR=$(top_srcdir)/data \
	$(top_srcdir)/tests/run-bbox-test.sh \
	$(top_builddir)/src/bin/xpost$(EXEEXT) $(top_srcdir)/tests/bbox_test.ps

# PDF device: write a PDF, check its structure, round-trip via gs when present.
XPOST_PDF_TEST = \
	XPOST_DATA_DIR=$(top_srcdir)/data \
	$(top_srcdir)/tests/run-pdf-test.sh \
	$(top_builddir)/src/bin/xpost$(EXEEXT) $(top_srcdir)/tests/pdf_bigfill_test.ps

if XPOST_ENABLE_TESTS

if HAVE_VALGRIND
XPOST_VALGRIND = $(top_srcdir)/valgrind_wrapper.sh
endif

lcov-check:
if XPOST_ENABLE_COVERAGE
	@$(MAKE) $(AM_MAKEFLAGS) lcov-reset
endif
	@$(MAKE) $(AM_MAKEFLAGS) check
if XPOST_ENABLE_COVERAGE
	@$(MAKE) $(AM_MAKEFLAGS) lcov-report
endif

check-local:
	$(XPOST_PS_CONFORMANCE)
	$(XPOST_BBOX_TEST)
	$(XPOST_PDF_TEST)

else

check-local:
	$(XPOST_PS_CONFORMANCE)
	$(XPOST_BBOX_TEST)
	$(XPOST_PDF_TEST)

endif

# The meson build description. A tarball that carries one of the two
# build systems is not the project: meson is what the tests are
# registered in and what CI builds, so a release without it cannot run
# any of them, and the structural guards it now ships are unreachable.
EXTRA_DIST += \
meson.build \
meson_options.txt \
data/meson.build \
doc/meson.build \
src/bin/meson.build \
src/lib/meson.build

EXTRA_DIST += \
COMPLIANCE \
autogen.sh \
xpost.pc.in \
m4/xpost.m4 \
valgrind_wrapper.sh

clean-local:
	find . -type f -name '*.gcno' -delete
	find . -type f -name '*.gcda' -delete
	find . -type f -name check-results.xml -delete
	rm -rf $(XPOST_CLEANFILES) coverage xdump*
