ARG BASE_IMAGE=xiaozhi-esp32-server:custom-20260914-tts-diag
FROM ${BASE_IMAGE}
WORKDIR /opt/xiaozhi-esp32-server
COPY app.py ./app.py
COPY config/manage_api_client.py ./config/manage_api_client.py
COPY core/connection.py ./core/connection.py
COPY core/handle/helloHandle.py core/handle/reportHandle.py core/handle/receiveAudioHandle.py core/handle/sendAudioHandle.py core/handle/textMessageProcessor.py ./core/handle/
COPY core/handle/textHandler/listenMessageHandler.py ./core/handle/textHandler/listenMessageHandler.py
COPY core/utils/user_binding.py core/utils/report_outbox.py core/utils/transcript_queue.py ./core/utils/
COPY core/providers/tts/base.py ./core/providers/tts/base.py
COPY core/providers/asr/base.py ./core/providers/asr/base.py
COPY tests/test_user_binding.py tests/test_report_outbox.py ./tests/
RUN python -m compileall -q app.py core config/manage_api_client.py && python -m unittest discover -s tests
