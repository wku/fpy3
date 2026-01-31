FROM python:3.12-slim-bookworm

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libssl-dev \
    ninja-build \
    pkg-config \
    wget \
    autoconf \
    automake \
    libtool \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Clone and Build MsQuic
RUN git config --global http.postBuffer 1048576000 && \
    git config --global http.lowSpeedLimit 0 && \
    git config --global http.lowSpeedTime 999 && \
    git clone --recursive https://github.com/microsoft/msquic.git vendor/msquic && \
    cd vendor/msquic && \
    cmake -B build -G Ninja -DQUIC_ENABLE_LOGGING=OFF && \
    cmake --build build && \
    cmake --install build

# Clone and Build nghttp3
RUN git clone --recursive https://github.com/ngtcp2/nghttp3.git vendor/nghttp3 && \
    cd vendor/nghttp3 && \
    autoreconf -i && \
    ./configure --enable-lib-only && \
    make && \
    make install

# Copy project files
COPY . .

# Install fpy3
RUN pip install .

# Set library path
ENV LD_LIBRARY_PATH=/usr/local/lib

# Expose port
EXPOSE 8080/udp

# Run ASGI test server by default
CMD ["python3", "-u", "test_asgi.py"]
