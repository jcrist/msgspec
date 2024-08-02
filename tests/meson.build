test_files = [
    'basic_typing_examples.py',
    'conftest.py',
    'test_common.py',
    'test_constraints.py',
    'test_convert.py',
    'test_cpylint.py',
    'test_inspect.py',
    'test_integration.py',
    'test_json.py',
    'test_JSONTestSuite.py',
    'test_msgpack.py',
    'test_mypy.py',
    'test_performance.py',
    'test_pyright.py',
    'test_raw.py',
    'test_schema.py',
    'test_struct.py',
    'test_to_builtins.py',
    'test_toml.py',
    'test_utils.py',
    'test_yaml.py',
    'utils.py',
]
foreach file: test_files
    fs.copyfile(file)
    if not meson.is_subproject() or get_option('install-subprojects').enabled()
        python.install_sources(file, subdir: 'msgspec' / 'tests')
    endif
endforeach
