# Ubuntu 26.04 supplies the Qt 6.10 OAuth APIs used by the application.
FROM ubuntu:26.04@sha256:2260313b31c8c011cd2eebe728008efac1b3982be73eb71348ea2648d2c0e09b

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    appstream binutils build-essential ca-certificates cmake curl dbus \
    desktop-file-utils dpkg-dev file git libicu-dev libqt6sql6-sqlite \
    ninja-build pkg-config python3 qt6-base-dev qt6-declarative-dev \
    qt6-networkauth-dev qt6-qpa-plugins qt6-wayland \
    qml6-module-qtqml qml6-module-qtqml-models qml6-module-qtqml-workerscript \
    qml6-module-qtquick qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
    qml6-module-qtquick-layouts qml6-module-qtquick-templates \
    qml6-module-qtquick-window systemd tzdata xz-utils \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /source
