# Disable showing recipe lines before execution.
set quiet

# Enable unstable features.
set unstable

# Configure the shell for Windows.
set windows-shell := ["pwsh.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command"]

# We don't want to install any dev dependencies by default.
export UV_NO_DEV := "true"

# Provide a consistent experience across Docker-compatible CLIs.
export DOCKER_CLI_HINTS := "0"

# We apply a unique label to each dev container that is used to find the container ID.
# The devcontainer CLI automatically adds the `devcontainer.local_folder` label but on
# Windows the drive letter is lowercased.
repo_id := sha256(justfile_directory())

# This is the parent directory for virtual environments.
env_base := join(data_local_directory(), "msgspec", "dev", repo_id, "venvs")

# This determines the version of Python with which to use in environments rather than
# the default behavior of using the highest version compatible with the project.
python := ""

# This determines whether to rebuild the C extension for recipes that depend on the project.
rebuild := "false"

# This determines whether to build a debug version of the C extension.
debug := "false"

# List all targets.
[private]
default:
  just --list

# Invoke the Git hook manager.
[group: "Static Analysis"]
prek arg="--help" *args: (
  _env_run "hooks"
  "prek" arg args
)
alias pre-commit := prek

# Run the hooks.
[group: "Static Analysis"]
hooks-fix *args: (
  _env_run "hooks"
  "prek run --show-diff-on-failure"
  args
)
alias hooks := hooks-fix

# Run the hooks without applying fixes.
[group: "Static Analysis"]
hooks-check *args: (
  _env_run "hooks"
  "prek run --show-diff-on-failure --hook-stage manual"
  args
)
alias check := hooks-check

# Run all tests.
[group: "Testing"]
test-all *args: (test-env "pytest" args) (test-doc args)

# Run unit tests.
[group: "Testing"]
test-unit *args: (
  _env_run "test"
  "pytest -m \"not types\""
  args
)
alias test := test-unit

# Run type checker compatibility tests.
[group: "Testing"]
test-types *args: (
  _env_run "test"
  "pytest -m \"types\""
  args
)

# Run doctests.
[group: "Testing"]
test-doc *args: (
  _env_run "test"
  "pytest --doctest-modules --pyargs msgspec"
  args
)

# Run a command within the test environment.
[group: "Testing"]
test-env +cmd: (
  _env_run "test"
  cmd
)

# Build the documentation.
[group: "Documentation"]
doc-build: (
  _env_run "doc"
  "sphinx-build -M html docs/source docs/build --fail-on-warning"
)

# Serve the documentation.
[group: "Documentation"]
doc-serve: (
  _env_run "doc"
  "python -c \"import pathlib,webbrowser;webbrowser.open_new_tab(pathlib.Path('docs/build/html/index.html').absolute().as_uri())\""
)

# Run benchmarks.
[group: "Benchmarking"]
bench-run *args: (
  _env_run "bench"
  "python -m benchmarks.bench_validation"
  args
)


# Sync an environment's dependencies.
[group: "Environment Management"]
env-sync env="test": (
  _with_env env "sync"
)

# Display the path to an environment.
[group: "Environment Management"]
env-path env="":
  echo {{ \
    if env == "" { \
      quote(env_base) \
    } else { \
      quote(join(env_base, env)) \
    } \
  }}

# Start the dev container.
[group: "Dev Container"]
dev-start:
  devcontainer up --workspace-folder . --id-label repo.id={{ repo_id }}

# Open a shell in the dev container.
[group: "Dev Container"]
dev-shell:
  devcontainer exec --workspace-folder . --id-label repo.id={{ repo_id }} -- zsh

# Stop the dev container.
[group: "Dev Container"]
dev-stop: (
  _with_dev_container_id
  "docker stop $id"
)

# Remove the dev container.
[group: "Dev Container"]
dev-remove: (
  _with_dev_container_id
  "docker rm $id"
)

[private]
_with_dev_container_id cmd:
  {{ \
    if os() == "windows" { \
      "$id = docker ps --all --format '{{.ID}}' --filter 'label=repo.id=" + repo_id + "'; " + \
      "if ($id) { " + cmd + " } else { Write-Host 'No devcontainer running' }" \
    } else { \
      "id=$(docker ps --all --format '{{.ID}}' --filter 'label=repo.id=" + repo_id + "'); " + \
      "if [ -n \"$id\" ]; then " + cmd + "; else echo 'No devcontainer running'; fi" \
    } \
  }}

[private]
_env_run env cmd *args: (
  _with_env env "run" "--" cmd args
)

[private]
[no-exit-message]
[no-cd]
_with_env env action *args:
  {{ \
    if os() == "windows" { \
      "$env:UV_PROJECT_ENVIRONMENT=" + quote(join(env_base, env)) + ";" \
    } else { \
      "UV_PROJECT_ENVIRONMENT=" + quote(join(env_base, env)) \
    } \
  }}{{ \
    if debug =~ "^(true|1)$" { \
      if os() == "windows" { \
        " $env:MSGSPEC_DEBUG='1';" \
      } else { \
        " MSGSPEC_DEBUG='1'" \
      } \
    } else { \
      "" \
    } \
  }} uv {{ \
    if action == "run" { \
      "run" \
    } else if action == "sync" { \
      "sync" \
    } else { \
      error("Unknown action: " + action) \
    } \
  }}{{ \
    if python != "" { \
      " --python " + python \
    } else { \
      "" \
    } \
  }} {{ \
    if rebuild =~ "^(true|1)$" { \
      "--reinstall-package msgspec " \
    } else { \
      "" \
    } \
  }}{{ \
    if env == "test" { \
      "--group test" \
    } else if env == "doc" { \
      "--group doc" \
    } else if env == "bench" { \
      "--group bench" \
    } else if env == "hooks" { \
      "--only-group hooks" \
    } else { \
      error("Unknown environment: " + env) \
    } \
  }} {{ args }}
