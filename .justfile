# Disable showing recipe lines before execution.
set quiet

# Enable unstable features.
set unstable

# Configure the shell for Windows.
set windows-shell := ["pwsh.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command"]

# Provide a consistent experience across Docker-compatible CLIs.
export DOCKER_CLI_HINTS := "0"

# We don't want to install any dev dependencies by default.
export UV_NO_DEV := "true"

# We don't want activated virtual environments to interfere with environment management.
unexport VIRTUAL_ENV

# We apply a unique label to each dev container that is used to find the container ID.
# The devcontainer CLI automatically adds the `devcontainer.local_folder` label but on
# Windows the drive letter is lowercased.
repo_id := sha256(lowercase(justfile_directory()))

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
  env-run "hooks"
  "prek" arg args
)
alias pre-commit := prek

# Run the hooks.
[group: "Static Analysis"]
hooks-fix *args: (
  env-run "hooks"
  "prek run --all-files"
  args
)
alias hooks := hooks-fix

# Run the hooks without applying fixes.
[group: "Static Analysis"]
hooks-check *args: (
  env-run "hooks"
  "prek run --all-files --hook-stage manual"
  args
)
alias check := hooks-check

# Run all tests.
[group: "Testing"]
test-all *args: (
  env-run "test" "pytest" args
) (
  test-doc args
)

# Run unit tests.
[group: "Testing"]
test-unit *args: (
  env-run "test"
  "pytest tests/unit"
  args
)
alias test := test-unit

# Run type checker compatibility tests.
[group: "Testing"]
test-typing *args: (
  env-run "test"
  "pytest tests/typing"
  args
)

# Run doctests.
[group: "Testing"]
test-doc *args: (
  env-run "test"
  "pytest --doctest-modules --pyargs msgspec"
  args
)

# Build the documentation.
[group: "Documentation"]
doc-build: (
  env-run "doc"
  "sphinx-build -b html docs site --fail-on-warning"
)

# Serve the documentation.
[group: "Documentation"]
doc-serve: (
  env-run "doc"
  "python -c \"import pathlib,webbrowser;webbrowser.open_new_tab(pathlib.Path('site/index.html').absolute().as_uri())\""
)

# Run performance tests.
[group: "Profiling"]
test-perf name="" *args: (
  env-run "prof"
  (
    if name == "" {
      "python -c \"import os;print('Available tests:', os.linesep.join(['', *sorted(e[5:].split('.')[0] for e in os.listdir('tests/prof/perf') if e.startswith('test_')), 'all (run everything)']))\""
    } else {
      (
        if name =~ "^-" {
          error(
            "Invalid test name: " + name
          )
        } else if name == "all" {
          "pytest -o testpaths=tests/prof/perf"
        } else {
          "pytest tests/prof/perf/test_" + name + ".py"
        }
      )
      + " --benchmark-name short --benchmark-disable-gc"
    }
  )
  args
)

# Run benchmarks.
[group: "Benchmarking"]
bench-run name="" *args: (
  env-run "bench"
  (
    if name == "" {
      "python -c \"import os;print('Available benchmarks:', os.linesep.join(['', *sorted(e[6:].split('.')[0] for e in os.listdir('benchmarks') if e.startswith('bench_')), 'all (run everything)']))\""
    } else {
      "python -m benchmarks.bench_" + name
    }
  )
  args
)
alias bench := bench-run

# Run a command within an environment.
[group: "Environment Management"]
env-run env +cmd: (
  _with_env env "run" "--" cmd
)
alias env := env-run

# Sync an environment's dependencies.
[group: "Environment Management"]
env-sync env="test": (
  _with_env (
    if env == "all" { "none" } else { env }
  ) (
    if env == "all" { "run" } else { "sync" }
  ) (
    if env == "all" { "just _env_sync_all" } else { "" }
  )
)
alias sync := env-sync

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

# Run a command within the dev container.
[group: "Dev Container"]
dev-run +cmd: (
  _with_dev_container_id
  "docker exec --user vscode -it $id zsh -lc " + quote(cmd)
)
alias run := dev-run

# Open a shell in the dev container.
[group: "Dev Container"]
dev-shell cmd="zsh -l": (
  _with_dev_container_id
  "docker exec --user vscode -it $id " + cmd
)
alias shell := dev-shell

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
_env_sync_all: (
  _with_env "test" "sync"
) (
  _with_env "doc" "sync"
) (
  _with_env "bench" "sync"
) (
  _with_env "prof" "sync"
)

[private]
[no-exit-message]
[no-cd]
_with_env env action *args:
  {{ \
    if env == "none" { \
      "" \
    } else { \
      if os() == "windows" { \
        "$env:UV_PROJECT_ENVIRONMENT=" + quote(join(env_base, env)) + "; " \
      } else { \
        "UV_PROJECT_ENVIRONMENT=" + quote(join(env_base, env)) + " " \
      } \
    } \
  }}{{ \
    if python == "" { \
      "" \
    } else { \
      if os() == "windows" { \
        "$env:UV_PYTHON=" + quote(python) + "; " \
      } else { \
        "UV_PYTHON=" + quote(python) + " " \
      } \
    } \
  }}{{ \
    if debug =~ "^(true|1)$" { \
      if os() == "windows" { \
        "$env:MSGSPEC_DEBUG='1'; " \
      } else { \
        "MSGSPEC_DEBUG='1' " \
      } \
    } else { \
      "" \
    } \
  }}uv {{ \
    if action == "run" { \
      "run" \
    } else if action == "sync" { \
      "sync" \
    } else { \
      error( \
        "Unknown action: " + action \
      ) \
    } \
  }}{{ \
    if rebuild =~ "^(true|1)$" { \
      " --reinstall-package msgspec" \
    } else { \
      "" \
    } \
  }} {{ \
    if env == "test" { \
      "--group test-unit --group test-typing" \
    } else if env == "doc" { \
      "--group doc" \
    } else if env == "prof" { \
      "--group test-prof" \
    } else if env == "bench" { \
      "--group bench" \
    } else if env == "hooks" { \
      "--only-group hooks" \
    } else if env == "none" { \
      "--isolated --no-sync --no-config" \
    } else { \
      error( \
        "Unknown environment: " + env \
      ) \
    } \
  }} {{ args }}
