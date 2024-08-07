source_files = [
    '__init__.py',
    '__init__.pyi',
    '_json_schema.py',
    '_utils.py',
    'inspect.py',
    'json.py',
    'json.pyi',
    'msgpack.py',
    'msgpack.pyi',
    'py.typed',
    'structs.py',
    'structs.pyi',
    'toml.py',
    'yaml.py',
]
message('CMake targets:\n - ' + '\n - '.join(mimalloc.target_list()))
configure_file(
    input: 'generate_atof_consts.py',
    output: 'generate_atof_consts.py',
    copy: true,
    install: true,
    install_dir: 'msgspec',
)
atof_consts = custom_target(
    'atof_consts.h',
    output: 'atof_consts.h',
    capture: true,
    command: [python, 'msgspec'/'generate_atof_consts.py'],
    install: true,
    install_dir: 'msgspec',
    build_by_default: true,
)
c_source_files = [
    '_core.c',
    'itoa.h',
    'atof.h',
    'common.h',
    'ryu.h',
    atof_consts,
]
if get_option('use-mimalloc') not in ''
    python.extension_module('_core', c_source_files)
else
    python.extension_module(
        '_core',
        c_source_files,
        dependencies: [mimalloc.dependency(get_option('use-mimalloc'))],
        link_whole: mimalloc.target(get_option('use-mimalloc')),
    )
endif
foreach file : files([source_files, '_core.c', 'itoa.h', 'atof.h', 'common.h', 'ryu.h'])
    fs.copyfile(file)
    if not meson.is_subproject() or get_option('install-subprojects').enabled()
        python.install_sources(file, pure: true, subdir: 'msgspec')
    endif
endforeach