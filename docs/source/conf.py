project = "msgspec"
copyright = "2022, Jim Crist-Harif"
author = "Jim Crist-Harif"

html_theme = "furo"
html_title = ""
templates_path = ["_templates"]
html_static_path = ["_static"]
html_theme_options = {
    "light_logo": "msgspec-logo-light.svg",
    "dark_logo": "msgspec-logo-dark.svg",
    "sidebar_hide_name": True,
}

extensions = [
    "sphinx.ext.autodoc",
    "sphinx.ext.napoleon",
    "sphinx.ext.extlinks",
    "sphinx.ext.intersphinx",
    "sphinx_copybutton",
]
intersphinx_mapping = {"python": ("https://docs.python.org/3", None)}
napoleon_numpy_docstring = True
napoleon_google_docstring = False
default_role = "obj"
extlinks = {
    "issue": ("https://github.com/jcrist/msgspec/issues/%s", "Issue #"),
    "pr": ("https://github.com/jcrist/msgspec/pull/%s", "PR #"),
}
copybutton_prompt_text = r">>> |\.\.\. |\$ "
copybutton_prompt_is_regexp = True
