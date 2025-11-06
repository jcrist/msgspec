FROM ghcr.io/astral-sh/uv:latest AS uv

FROM buildpack-deps:24.04-curl AS bin
COPY --chmod=755 --from=uv /uv /usr/local/bin/uv
RUN uv tool install rust-just

FROM mcr.microsoft.com/devcontainers/base:ubuntu-24.04

RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
        gdb \
        valgrind \
        libasan6 \
    && apt-get autoremove -y \
    && apt-get clean -y \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspaces/msgspec

COPY --chmod=755 --from=bin \
    /root/.local/bin/just \
    /usr/local/bin/
COPY --chmod=755 --from=uv \
    /uv \
    /uvx \
    /usr/local/bin/
