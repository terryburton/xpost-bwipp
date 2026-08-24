
.PHONY: doc

PACKAGE_DOCNAME = $(PACKAGE_TARNAME)-$(PACKAGE_VERSION)-doc

if XPOST_BUILD_DOC

XPOST_DOC_CLEANFILES = doc/html/ doc/latex/ $(PACKAGE_DOCNAME).tar*

XPOST_CLEANFILES += $(XPOST_DOC_CLEANFILES)

doc-clean:
	rm -rf $(XPOST_DOC_CLEANFILES)

doc: all doc-clean
	$(DOXYGEN) doc/Doxyfile
	mkdir -p $(PACKAGE_DOCNAME)/doc
	cp -rf doc/html/ doc/latex/ $(PACKAGE_DOCNAME)/doc
	tar cf $(PACKAGE_DOCNAME).tar $(PACKAGE_DOCNAME)/
	bzip2 -9 $(PACKAGE_DOCNAME).tar
	rm -rf $(PACKAGE_DOCNAME)/

else

doc: all
	@echo "Documentation not built. Run ./configure --help"

endif

# Everything under doc that a release carries: the documents themselves,
# and what the doc target above reads -- the Doxyfile, the two .dox
# sources it names as input, and the figures they draw.
#
# tests/check-dist-lists.sh holds this list to the directory in both
# directions, so a document added and not named here fails.
EXTRA_DIST += \
doc/COMPAT \
doc/CONTRIBUTING.md \
doc/COVERAGE.md \
doc/COVERAGE-large.md \
doc/DICTIONARIES.md \
doc/GATING.md \
doc/INTERNALS \
doc/ROOTS \
doc/MANUAL \
doc/Doxyfile \
doc/xpost.dox \
doc/xpost_design.dox \
doc/m.pic \
doc/m.ps \
doc/mtab.eps \
doc/mtab.png \
doc/s.pic \
doc/s.ps \
doc/stack.eps \
doc/stack.png
