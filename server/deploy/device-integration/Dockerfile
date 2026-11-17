ARG BASE_IMAGE
FROM ${BASE_IMAGE}
WORKDIR /opt/xiaozhi-esp32-server
COPY core/ ./core/
COPY config/ ./config/
COPY tests/ ./tests/
RUN python -m compileall -q core config tests && python -m unittest discover -s tests
