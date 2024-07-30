project(
    'msgspec',
    default_options: ['warning_level=3'],
    license: 'BSD-3-Clause',
    license_files: ['LICENSE'],
    meson_version: '>=1.1.0',
    version: run_command(
        'python3',
        [
            '-c',
            'from versioneer import get_version;print(get_version())',
        ],
        check: true,
    ).stdout().strip(),
)
add_languages('c')
fs = import('fs')
pymod = import('python')
python = pymod.find_installation()
python_readlines = 'f=open("@0@","r");print(*f.readlines(),sep="",end="");f.close();'
if get_option('use-mimalloc') not in ''
    cmake = import('cmake')
    mimalloc = cmake.subproject('mimalloc')
endif
subdir('msgspec')
