FROM python:3.12-slim

WORKDIR /app
COPY web/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY web/app.py scripts/prepare_image.py ./
COPY web/static ./static

USER nobody
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
