# タグはdocker-compose.ymlのbuild.argsから渡す(既定値もそちら側)。
# ESP-IDF v6.0はM5GFXが依存するレガシーI2Cドライバ(driver/i2c.h)を削除したため、
# このプロジェクトはv5系に固定している。main/idf_component.ymlの制約と対で管理する。
ARG DOCKER_TAG
FROM espressif/idf:${DOCKER_TAG}

ENV LC_ALL=C.UTF-8
ENV LANG=C.UTF-8

# curl / ca-certificatesはベースイメージに入っているが、
# タグ更新で消えてもビルドが壊れないよう明示しておく。
# openssh-serverはWindowsからの逆トンネル(ssh -R 3240)を受けるsshd用。
# コンテナのloopback:2222だけで待ち受ける(設定は同ディレクトリのsshd_config、
# 起動はdocker-compose.ymlのcommand側)。
# linux-tools-generic(usbip)はコンテナ内でのattach/detach/status全部に使う。
RUN apt-get update -y \
 && apt-get install -y --no-install-recommends udev curl ca-certificates sudo linux-tools-generic openssh-server \
 && mkdir -p /run/sshd \
 && rm -rf /var/lib/apt/lists/*

COPY sshd_config /etc/ssh/sshd_config

# ベースイメージのUbuntuバージョンによってUID1000のユーザー有無が変わるので、
# "ubuntu"という名前でUID/GID 1000のユーザーが必ず居る状態に揃える。
# ホスト(WSL/Linux)のUIDと一致するので、コンテナが作るbuild/やsdkconfigを
# ホスト側からもそのまま編集できる。
ARG USERNAME=ubuntu
RUN set -eu; \
    if ! id -u "${USERNAME}" >/dev/null 2>&1; then \
        if getent passwd 1000 >/dev/null; then \
            old="$(getent passwd 1000 | cut -d: -f1)"; \
            oldgrp="$(id -gn "${old}")"; \
            usermod  -l "${USERNAME}" -d "/home/${USERNAME}" -m "${old}"; \
            groupmod -n "${USERNAME}" "${oldgrp}" || true; \
        else \
            groupadd -g 1000 "${USERNAME}"; \
            useradd  -u 1000 -g 1000 -m -s /bin/bash "${USERNAME}"; \
        fi; \
    fi; \
    # シリアル(/dev/ttyUSB0)への書き込みにdialoutが要る
    usermod -aG dialout,plugdev "${USERNAME}"; \
    echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/90-${USERNAME}; \
    chmod 0440 /etc/sudoers.d/90-${USERNAME}

# sshdの受け口の準備。
# - ~/.sshはここで作る。compose側でauthorized_keysをファイル単位でbindするため、
#   先にユーザー所有・700で存在していないとsshdのStrictModesが弾く。
# - ベースイメージのユーザーはshadowのパスワード欄が'!'のロック状態(2026-08-31に
#   espressif/idf:v5.5で実測)。sshdはUsePAM無しだとロックされたアカウントを
#   鍵認証でも拒否するため、'*'に変えてロックだけ外す(sshd_configの
#   PasswordAuthentication noにより、パスワードでのログイン自体は引き続き不可)。
#   なお`passwd -S`は'!'でも'*'でも"L"と表示するので、確認には
#   `getent shadow ubuntu`を使うこと。
RUN set -eu; \
    install -d -m 700 -o "${USERNAME}" -g "${USERNAME}" "/home/${USERNAME}/.ssh"; \
    usermod -p '*' "${USERNAME}"

# IDFのPython venvはバージョン込みの名前(例: idf5.5_py3.10_env)になるので、
# 名前に依存しない固定パスを1本用意する。devcontainer.jsonのidf.pythonInstallPathが
# これを指すため、IDFのタグを上げてもjsonを直さずに済む。
RUN set -eu; \
    env_dir="$(ls -d /opt/esp/python_env/*_env 2>/dev/null | head -n1)"; \
    test -n "$env_dir"; \
    ln -sfn "$env_dir" /opt/esp/python_env/current

# Ubuntuの/usr/bin/usbipはuname -rでバイナリを探すラッパーだが、
# コンテナが見るカーネル(=ホストのカーネル)とlinux-toolsのバージョンは一致しない。
# 実体へ直接symlinkしてカーネルバージョン非依存にする。
RUN set -eu; \
    bin_dir="$(ls -d /usr/lib/linux-tools/*/ 2>/dev/null | head -n1)"; \
    test -n "$bin_dir"; \
    ln -sfn "${bin_dir}usbip"  /usr/local/bin/usbip; \
    ln -sfn "${bin_dir}usbipd" /usr/local/bin/usbipd

ARG CLAUDE_PROXY
USER ${USERNAME}
RUN set -eu; \
    p="${CLAUDE_PROXY:-${https_proxy:-}}"; \
    export http_proxy="$p" https_proxy="$p" HTTP_PROXY="$p" HTTPS_PROXY="$p"; \
    curl -fsSL https://claude.ai/install.sh | bash
USER root
ENV PATH="/home/ubuntu/.local/bin:${PATH}"

# 非対話シェルでもidf.pyが使えるようにexport.shを読ませる
RUN echo '. /opt/esp/idf/export.sh > /dev/null 2>&1' \
      | tee /etc/profile.d/esp-idf.sh >> /etc/bash.bashrc

ENTRYPOINT [ "/opt/esp/entrypoint.sh" ]

CMD ["sleep", "infinity"]
