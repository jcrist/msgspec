import os

from setuptools import setup
from setuptools.extension import Extension

import versioneer

SANITIZE = os.environ.get("MSGSPEC_SANITIZE", False)
COVERAGE = os.environ.get("MSGSPEC_COVERAGE", False)
DEBUG = os.environ.get("MSGSPEC_DEBUG", SANITIZE or COVERAGE)

extra_compile_args = []
extra_link_args = []
if SANITIZE:
    extra_compile_args.extend(["-fsanitize=address", "-fsanitize=undefined"])
    extra_link_args.extend(["-lasan", "-lubsan"])
if COVERAGE:
    extra_compile_args.append("--coverage")
    extra_link_args.append("-lgcov")
if DEBUG:
    extra_compile_args.extend(["-O0", "-g", "-UNDEBUG"])

ext_modules = [
    Extension(
        "msgspec._core",
        [os.path.join("msgspec", "_core.c")],
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
    )
]

yaml_deps = ["pyyaml"]
toml_deps = ['tomli ; python_version < "3.11"', "tomli_w"]
doc_deps = ["sphinx", "furo", "sphinx-copybutton", "sphinx-design", "ipython"]
test_deps = ["pytest", "mypy", "pyright", "msgpack", *yaml_deps, *toml_deps]
dev_deps = ["pre-commit", "coverage", "gcovr", *doc_deps, *test_deps]

extras_require = {
    "yaml": yaml_deps,
    "toml": toml_deps,
    "doc": doc_deps,
    "test": test_deps,
    "dev": dev_deps,
}

setup(
    name="msgspec",
    version=versioneer.get_version(),
    cmdclass=versioneer.get_cmdclass(),
    maintainer="Jim Crist-Harif",
    maintainer_email="jcristharif@gmail.com",
    url="https://jcristharif.com/msgspec/",
    project_urls={
        "Documentation": "https://jcristharif.com/msgspec/",
        "Source": "https://github.com/jcrist/msgspec/",
        "Issue Tracker": "https://github.com/jcrist/msgspec/issues",
    },
    description=(
        "A fast serialization and validation library, with builtin support for "
        "JSON, MessagePack, YAML, and TOML."
    ),
    keywords="JSON msgpack MessagePack TOML YAML serialization validation schema",
    classifiers=[
        "License :: OSI Approved :: BSD License",
        "Development Status :: 4 - Beta",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
    ],
    extras_require=extras_require,
    license="BSD",
    packages=["msgspec"],
    package_data={"msgspec": ["py.typed", "*.pyi"]},
    ext_modules=ext_modules,
    long_description=(
        open("README.md", encoding="utf-8").read()
        if os.path.exists("README.md")
        else ""
    ),
    long_description_content_type="text/markdown",
    python_requires=">=3.8",
    zip_safe=False,
)
