FROM python:3.13-slim

ARG BUILD_VERSION
ARG BUILD_ARCH

LABEL \
    io.hass.version="${BUILD_VERSION}" \
    io.hass.type="app" \
    io.hass.arch="${BUILD_ARCH}"

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 \
    https://github.com/ontheview/mammotion-rc.git \
    /opt/mammotion-rc

RUN python -m pip install --no-cache-dir --upgrade pip \
    && python -m pip install --no-cache-dir \
       -r /opt/mammotion-rc/web-server/requirements.txt

COPY run.sh /run.sh

RUN chmod a+x /run.sh

CMD ["/run.sh"]

