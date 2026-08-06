FROM fukamachi/sbcl

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    gcc \
    gcc-mingw-w64 \
    make \
    && rm -rf /var/list/apt/lists/*


WORKDIR /workspace

