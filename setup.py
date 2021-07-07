import os

import versioneer
from setuptools import setup
from setuptools.extension import Extension

ext_modules = [Extension("msgspec.core", [os.path.join("msgspec", "core.c")])]

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
    description="Fast and friendly msgpack (de)serialization, with type validation",
    keywords="msgpack Messagepack serialization",
    classifiers=[
        "License :: OSI Approved :: BSD License",
        "Development Status :: 4 - Beta",
        "Programming Language :: Python :: 3.8",
        "Programming Language :: Python :: 3.9",
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
