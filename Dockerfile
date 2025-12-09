FROM ubuntu:24.04

# Basic build + debug + editor helpers for C++ dev
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential gdb cmake ninja-build clang-format git \
    vim nano curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# Prep workspace directory for CMake builds
RUN mkdir -p /workspace

# Optional: set a non-root user for better dev ergonomics
ARG USERNAME=dev
ARG USER_UID=1001
ARG USER_GID=1001
RUN (getent group "${USER_GID}" || groupadd --gid "${USER_GID}" "${USERNAME}") && \
    (id -u "${USERNAME}" >/dev/null 2>&1 || useradd --uid "${USER_UID}" --gid "${USER_GID}" -m "${USERNAME}")

WORKDIR /workspace
USER ${USERNAME}

# Default command keeps container alive for interactive dev
CMD ["sleep", "infinity"]


