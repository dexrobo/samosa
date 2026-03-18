FROM ubuntu:jammy

# Avoid prompts during apt-get
ENV DEBIAN_FRONTEND=noninteractive

# Install dependencies and Bazelisk in a single layer
RUN apt-get update && apt-get install -y \
    curl \
    gnupg \
    build-essential \
    gcc-12 \
    g++-12 \
    python3 \
    nodejs \
    git \
    wget \
    libxml2 \
    libglib2.0-0 \
    sudo \
    && ARCH=$(dpkg --print-architecture) && \
    curl -Lo /usr/local/bin/bazel "https://github.com/bazelbuild/bazelisk/releases/download/v1.19.0/bazelisk-linux-${ARCH}" && \
    chmod +x /usr/local/bin/bazel \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME

USER $USERNAME
WORKDIR /workspace
