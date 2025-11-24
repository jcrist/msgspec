import re
import webbrowser
from pathlib import Path
from urllib.parse import urlencode


def main():
    changelog_path = Path(__file__).parents[1] / "docs" / "changelog.md"
    changelog = changelog_path.read_text(encoding="utf-8")

    match = re.search(
        r"^##\s+Version\s+(?P<version>\S+)[^\n]*\n+(?P<notes>.*?)(?=^##\s+Version|\Z)",
        changelog,
        re.DOTALL | re.MULTILINE,
    )
    if not match:
        msg = f"Unable to parse changelog at {changelog_path}"
        raise RuntimeError(msg)

    version = match.group("version").strip()
    notes = match.group("notes").strip()
    params = urlencode(
        {
            "title": f"Version {version}",
            "tag": version,
            "body": re.sub(r"\{pr\}`(\d+)`", r"#\1", notes),
        }
    )

    url = f"https://github.com/jcrist/msgspec/releases/new?{params}"
    webbrowser.open_new_tab(url)


if __name__ == "__main__":
    main()
