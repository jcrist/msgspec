"""
This benchmark compares the installed library size for a number of different
librairies. It does this by downloading the latest Python 3.10 manylinux_2_17
x86_64 wheel for the library, decompresses the zipfile, and calculates the
total size of all files.
"""

import io
import zipfile

import requests


def get_latest_310_manylinux_x86_64_wheel_size(library):
    """Get the total uncompressed size of the latest Python 3.10 manylinux
    x86_64 wheel for the library"""
    resp = requests.get(f"https://pypi.org/pypi/{library}/json").json()
    version = resp["info"]["version"]
    files = {}
    for file_info in resp["releases"][version]:
        name = file_info["filename"]
        url = file_info["url"]
        if "310" in name and "manylinux_2_17" in name and "x86_64" in name:
            files[name] = url
    if len(files) != 1:
        raise ValueError(
            f"Expected to find only 1 matching file for {library}, got {list(files)}"
        )

    url = list(files.values())[0]

    resp = requests.get(url)
    fil = io.BytesIO(resp.content)
    zfil = zipfile.ZipFile(fil)
    size = sum(f.file_size for f in zfil.filelist)
    return library, version, size


def main():
    data = [
        get_latest_310_manylinux_x86_64_wheel_size(lib)
        for lib in ["msgspec", "orjson", "msgpack", "pydantic"]
    ]
    data.sort(key=lambda x: x[2])
    msgspec_size = next(s for l, _, s in data if l == "msgspec")

    columns = ("", "version", "size (MiB)", "vs. msgspec")
    rows = [
        (
            f"**{lib}**",
            version,
            f"{size / (1024 * 1024):.2f}",
            f"{size / msgspec_size:.2f}x",
        )
        for lib, version, size in data
    ]

    widths = tuple(max(max(map(len, x)), len(c)) for x, c in zip(zip(*rows), columns))
    row_template = ("|" + (" %%-%ds |" * len(columns))) % widths
    header = row_template % tuple(columns)
    bar_underline = "+%s+" % "+".join("=" * (w + 2) for w in widths)
    bar = "+%s+" % "+".join("-" * (w + 2) for w in widths)
    parts = [bar, header, bar_underline]
    for r in rows:
        parts.append(row_template % r)
        parts.append(bar)
    print("\n".join(parts))


if __name__ == "__main__":
    main()
