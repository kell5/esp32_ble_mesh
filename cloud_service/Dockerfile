# syntax=docker/dockerfile:1
FROM python:3.12-slim

WORKDIR /app

# Install the service from its package metadata (no dev/test extras).
COPY pyproject.toml ./
COPY src ./src
RUN pip install --no-cache-dir .

# Persist the SQLite database outside the image layer.
ENV CLOUD_DATABASE_PATH=/data/cloud.db \
    CLOUD_API_HOST=0.0.0.0 \
    CLOUD_API_PORT=8000
VOLUME ["/data"]
EXPOSE 8000

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD python -c "import os,urllib.request; urllib.request.urlopen(f\"http://127.0.0.1:{os.getenv('CLOUD_API_PORT','8000')}/health\").read()" || exit 1

CMD ["python", "-m", "cloud_service"]
