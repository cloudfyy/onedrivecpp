ARG UBUNTU_VERSION
FROM ubuntu:${UBUNTU_VERSION}

ARG BUILD_PROFILE
ENV DEBIAN_FRONTEND=noninteractive
ENV DEB_BUILD_PROFILES=${BUILD_PROFILE}

COPY debian/control /tmp/package/debian/control
RUN printf 'path-include=/usr/share/man/*\npath-include=/usr/share/doc/*\n' \
        > /etc/dpkg/dpkg.cfg.d/zz-onedrive-tests \
    && if dpkg-divert --list /usr/bin/man | grep -q .; then \
        rm -f /usr/bin/man && dpkg-divert --remove --rename /usr/bin/man; \
    fi \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates ccache devscripts git lintian python3 \
        dbus desktop-file-utils man-db systemd \
    && apt-get build-dep -y --no-install-recommends /tmp/package \
    && if [ -z "$DEB_BUILD_PROFILES" ]; then \
        apt-get install -y --no-install-recommends qt6-qpa-plugins qt6-wayland; \
    fi \
    && find /var/lib/apt/lists -type f -delete
