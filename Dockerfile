FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates cmake g++ make python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/
COPY tests/ tests/
RUN cmake -S . -B /opt/build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/build -j 2
CMD ["ctest", "--test-dir", "/opt/build", "--output-on-failure", "-V"]
