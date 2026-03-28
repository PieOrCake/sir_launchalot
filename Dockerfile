FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    curl \
    file \
    fuse \
    libfuse2 \
    libgl1-mesa-dev \
    libglib2.0-dev \
    qt6-base-dev \
    libqt6svg6-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
