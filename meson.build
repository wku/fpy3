project('fpy', 'c', version: '0.1.0', default_options: ['warning_level=0', 'werror=false'])

py = import('python').find_installation(pure: false)
py_dep = py.dependency()

fs = import('fs')
vendored_include = 'vendor/dist/include'
vendored_lib = meson.current_source_dir() / 'vendor/dist/lib'
use_vendored = fs.is_dir(vendored_include)

# Common Include Directories
base_inc_dirs = [
  'src/fpy/c_impl',
  'src/fpy/protocol/c_impl',
  'src/fpy/request/c_impl',
  'src/fpy/response/c_impl',
  'src/fpy/router/c_impl',
  'src/fpy/parser/c_impl',
  'src/fpy/pipeline/c_impl',
  'src/fpy/picohttpparser'
]
if use_vendored
  inc_dirs = include_directories(base_inc_dirs, vendored_include)
else
  inc_dirs = include_directories(base_inc_dirs)
endif

cc = meson.get_compiler('c')

if use_vendored
  libwtf = cc.find_library('wtf', dirs: [vendored_lib], required: true)
  msquic = cc.find_library('msquic', dirs: [vendored_lib], required: true)
  nghttp3 = cc.find_library('nghttp3', dirs: [vendored_lib], required: true)
else
  libwtf = cc.find_library('wtf', required: false)
  msquic = cc.find_library('msquic', required: true)
  nghttp3 = cc.find_library('nghttp3', required: true)
endif

# Static Lib (Scalar Only to avoid build issues)
pico_lib = static_library('pico_scalar',
  'src/fpy/picohttpparser/picohttpparser.c',
  c_args: [], 
  include_directories: inc_dirs,
  pic: true
)

# Common Sources
capsule_src = 'src/fpy/c_impl/capsule.c'
match_dict_src = 'src/fpy/router/c_impl/match_dict.c'

# Python Sources (Install main package)
install_subdir('src/fpy', 
  install_dir: py.get_install_dir(),
  exclude_directories: [
    'c_impl', 
    'protocol/c_impl', 
    'request/c_impl', 
    'response/c_impl', 
    'router/c_impl', 
    'parser/c_impl', 
    'pipeline/c_impl',
    'picohttpparser'
  ]
)

# Extensions

# 1. request.crequest
py.extension_module(
  'crequest',
  sources: [
    'src/fpy/request/c_impl/crequest.c',
    'src/fpy/response/c_impl/cresponse.c',
    match_dict_src,
    capsule_src
  ],
  include_directories: inc_dirs,
  link_with: pico_lib,
  c_args: ['-DREQUEST_OPAQUE'], 
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/request'
)

# 2. response.cresponse
py.extension_module(
  'cresponse',
  sources: [
     'src/fpy/response/c_impl/cresponse.c',
     capsule_src
  ],
  include_directories: inc_dirs,
  c_args: ['-DRESPONSE_OPAQUE'],
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/response'
)

# 3. parser.cparser
py.extension_module(
  'cparser',
  sources: [
      'src/fpy/parser/c_impl/cparser.c',
      capsule_src
  ],
  include_directories: inc_dirs,
  link_with: pico_lib,
  c_args: [],
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/parser'
)

# 4. pipeline.cpipeline
py.extension_module(
  'cpipeline',
  sources: [
      'src/fpy/pipeline/c_impl/cpipeline.c',
      capsule_src
  ],
  include_directories: inc_dirs,
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/pipeline'
)

# 5. router.cmatcher
py.extension_module(
  'cmatcher',
  sources: [
      'src/fpy/router/c_impl/cmatcher.c',
      match_dict_src,
      capsule_src
  ],
  include_directories: inc_dirs,
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/router'
)

# 6. protocol.cprotocol
py.extension_module(
  'cprotocol',
  sources: [
     'src/fpy/protocol/c_impl/cprotocol.c',
     'src/fpy/protocol/c_impl/creaper.c',
     capsule_src,
     'src/fpy/request/c_impl/crequest.c',
     'src/fpy/response/c_impl/cresponse.c',
     'src/fpy/parser/c_impl/cparser.c',
     'src/fpy/pipeline/c_impl/cpipeline.c'
  ],
  include_directories: inc_dirs,
  link_with: pico_lib,
  c_args: ['-DPIPELINE_PAIR', '-DREAPER_ENABLED'],
  dependencies: [py_dep],
  install: true,
  subdir: 'fpy/protocol'
)

# 7. protocol.creaper
py.extension_module(
  'creaper',
    sources: [
        'src/fpy/protocol/c_impl/creaper.c',
        capsule_src
    ],
    include_directories: inc_dirs,
    dependencies: [py_dep],
    install: true,
    subdir: 'fpy/protocol'
)

# 8. protocol.cquic
cquic_deps = [py_dep, msquic, nghttp3]
if libwtf.found()
  cquic_deps += libwtf
endif

if use_vendored
  py.extension_module(
    'cquic',
    sources: [
      'src/fpy/protocol/c_impl/cquic.c',
      capsule_src
    ],
    include_directories: inc_dirs,
    dependencies: cquic_deps,
    install: true,
    install_rpath: '$ORIGIN',
    subdir: 'fpy/protocol'
  )
  # Install shared libraries to the package directory for self-contained build
  install_data(
    'vendor/dist/lib/libmsquic.so.2',
    'vendor/dist/lib/libnghttp3.so.9',
    install_dir: py.get_install_dir() / 'fpy/protocol'
  )
else
  py.extension_module(
    'cquic',
    sources: [
      'src/fpy/protocol/c_impl/cquic.c',
      capsule_src
    ],
    include_directories: inc_dirs,
    dependencies: cquic_deps,
    install: true,
    subdir: 'fpy/protocol'
  )
endif
