doxygen = find_program('doxygen', required: false)
sh = find_program('sh', required: false)
if doxygen.found() and sh.found()
  # every path in the Doxyfile is written relative to the tree root
  # (INPUT, IMAGE_PATH, OUTPUT_DIRECTORY), which is also where the
  # autotools doc target runs doxygen; run it from the same place
  # Always stale, because what doxygen reads is every source and every
  # page the Doxyfile points it at, and naming those here would be a
  # second list of them to keep in step with the first. Declaring only
  # the Doxyfile made the target report success without running: an
  # edited page left the built reference as it was, which is the one
  # failure a documentation target must not have.
  custom_target('doc',
    input: 'Doxyfile',
    output: '.',
    command: [ sh, '-c', 'cd "$0" && "$1" doc/Doxyfile',
               meson.project_source_root(), doxygen ],
    build_always_stale: true,
    install: false
  )
endif
