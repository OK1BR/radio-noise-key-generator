project('radio-noise-key-generator', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

add_project_arguments('-D_GNU_SOURCE', language : 'c')
add_project_arguments('-DRNKG_VERSION="@0@"'.format(meson.project_version()),
                      language : 'c')

# --- Dependencies -----------------------------------------------------------
# Same split as the rest of the family: the engine is GLib-only (headless,
# testable, no GTK includes), the app adds GTK4/libadwaita on top.
glib_dep   = dependency('glib-2.0')
gcrypt_dep = dependency('libgcrypt')   # SHAKE-256, secure memory
m_dep      = meson.get_compiler('c').find_library('m')
gtk_dep    = dependency('gtk4')             # M2: front end
adw_dep    = dependency('libadwaita-1')     # M2: front end

# librtlsdr is optional on purpose: everything except the dongle source
# builds and tests without it, which is how the health tests and the
# estimator get exercised on a machine with no hardware attached.
rtlsdr_dep = dependency('librtlsdr', required : false)
if rtlsdr_dep.found()
  add_project_arguments('-DHAVE_LIBRTLSDR', language : 'c')
else
  warning('librtlsdr not found — building without RTL-SDR support; ' +
          'the engine still runs from files (rnkg --file)')
endif

engine_inc = include_directories('src/engine')

# --- Engine (headless, GLib-only) -------------------------------------------
engine_sources = [
  'src/engine/rnkg-health.c',    # SP 800-90B 4.4 continuous tests + 4.5 own
  'src/engine/rnkg-estimate.c',  # SP 800-90B 6.3.1 most common value
  'src/engine/rnkg-extract.c',   # SHAKE-256 conditioning + kernel mix
  'src/engine/rnkg-generate.c',  # rejection sampling into an alphabet
  'src/engine/rnkg-source.c',    # RTL-SDR and file sources
  'src/engine/rnkg-collect.c',   # the collection loop both front ends drive
  'src/engine/rnkg-spectrum.c',  # display-only FFT for the spectrum view
]
engine_deps = [glib_dep, gcrypt_dep, m_dep, rtlsdr_dep]

engine_lib = static_library('rnkg-engine',
  engine_sources,
  include_directories : engine_inc,
  dependencies : engine_deps,
)

# --- Gates ------------------------------------------------------------------
# No hardware required: every test either builds its own sample data or
# checks arithmetic against the numbers printed in SP 800-90B.

health_test = executable('rnkg-health-test',
  ['src/health_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('health', health_test, timeout : 60)

estimate_test = executable('rnkg-estimate-test',
  ['src/estimate_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('estimate', estimate_test, timeout : 60)

collect_test = executable('rnkg-collect-test',
  ['src/collect_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('collect', collect_test, timeout : 60)

generate_test = executable('rnkg-generate-test',
  ['src/generate_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('generate', generate_test, timeout : 60)

spectrum_test = executable('rnkg-spectrum-test',
  ['src/spectrum_test.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
)
test('spectrum', spectrum_test, timeout : 60)

# --- CLI --------------------------------------------------------------------
executable('rnkg',
  ['src/cli/main.c'],
  include_directories : engine_inc,
  dependencies : engine_deps,
  link_with : engine_lib,
  install : true,
)

# --- GTK4/libadwaita front end (M2) -----------------------------------------
executable('radio-noise-key-generator',
  [
    'src/app/main.c',
    'src/app/window.c',
    'src/app/spectrum-view.c',
    'src/app/monitor-view.c',
  ],
  include_directories : engine_inc,
  dependencies : engine_deps + [gtk_dep, adw_dep],
  link_with : engine_lib,
  install : true,
)
