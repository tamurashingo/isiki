FROM fukamachi/sbcl

ENV DEBIAN_FRONTEND=noninteractive

# ホストのUID/GIDを渡すと、docker run --user "$(id -u):$(id -g)" 実行時に
# /etc/passwdの該当エントリが解決でき、$HOMEが正しく設定される
# (未解決だとrosがセグフォルトする)。
ARG USER_UID=1000
ARG USER_GID=1000

RUN apt-get update && apt-get install -y \
    curl \
    gcc \
    gcc-mingw-w64 \
    make \
    && rm -rf /var/list/apt/lists/*

RUN (getent group ${USER_GID} || groupadd -g ${USER_GID} builder) > /dev/null \
    && useradd -m -u ${USER_UID} -g ${USER_GID} builder

USER builder
ENV HOME=/home/builder
RUN curl -L https://raw.githubusercontent.com/roswell/roswell/release/scripts/install-for-ci.sh | sh
ENV PATH="/home/builder/.roswell/bin:${PATH}"
# ダミーのeval実行。これによりrosが遅延インストールするsbcl-bin本体のダウンロード/
# 展開/ビルドがこのイメージのレイヤーに焼き込まれ、docker run --rm時(sbcl-binは
# コンテナ内で毎回消える)の再ダウンロードを防ぐ。
RUN ros run --eval '(quit)'

USER root
WORKDIR /workspace

