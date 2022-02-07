import os

import versioneer
from setuptools import setup
from setuptools.extension import Extension

DEBUG = os.environ.get("MSGSPEC_DEBUG", False)

if DEBUG:
    extra_compile_args = ["-O0", "-g"]
else:
    extra_compile_args = []

ext_modules = [
    Extension(
        "msgspec._core",
        [os.path.join("msgspec", "_core.c")],
        extra_compile_args=extra_compile_args,
    )
]

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
    description="A fast and friendly JSON/MessagePack library, with optional schema validation",
    keywords="JSON msgpack Messagepack serialization schema",
    classifiers=[
        "License :: OSI Approved :: BSD License",
        "Development Status :: 4 - Beta",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
    ],
    license="BSD",
    packages=["msgspec"],
    package_data={"msgspec": ["py.typed", "*.pyi"]},
    include_package_data=True,
    ext_modules=ext_modules,
    long_description=(
        open("README.rst", encoding="utf-8").read()
        if os.path.exists("README.rst")
        else ""
    ),
    python_requires=">=3.8",
    zip_safe=False,
)
