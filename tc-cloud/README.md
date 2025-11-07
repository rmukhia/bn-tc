# TC Cloud Telemetry Dashboard

FastAPI app that ingests telemetry via MQTT and HTTP, decodes a 10‑character hex payload into longitude, latitude, and battery, stores records in SQLite, and serves a simple dashboard with CSV downloads.

- Dashboard: recent telemetry and CSV exports
- Ingestion: MQTT topic and HTTP endpoint
- Storage: SQLite file (`database.db`)

## Quickstart

1) Create and activate a virtual environment

Linux / macOS:

```bash
python3 -m venv .venv
source .venv/bin/activate
```

Windows (PowerShell):

```powershell
py -3 -m venv .venv
.venv\Scripts\Activate.ps1
```

2) Install dependencies

```bash
pip install -r requirements.txt
```

3) Configure environment (optional)

Create or edit `.env` at the project root:

```
MQTT_HOST=broker.emqx.io
MQTT_PORT=1883
DATABASE_PATH=./database.db
```

The default points to the free public EMQX broker:
https://www.emqx.com/en/mqtt/public-mqtt5-broker

4) Run the app

```bash
fastapi run main.py
```

Open the dashboard:

http://127.0.0.1:8000/

## HTTP Endpoints

```
POST /ingest
```
End device post JSON payload to this webhook.

## MQTT Topic

Subscribed topic:

```
tc-bn/telemetry/+
```

End devices publish JSON payloads on this topic (same shape as the HTTP `/ingest` body).

## Functional Requirements
1. **Implement a button to download the GPS location with timestamps in CSV format:**
In the dashboard  http://127.0.0.1:8000 click `Download CSV` or 
`Download 12 Hour CSV with 1 Hour Interval`
2. **The transmission interval should be 1 hour within the 12-hour duration:** The transmission interval can be set in the end device to arbitary duration. The dashboard also provides the feature to download the past 12 hour data in 1 hour iterval using `Download 12 Hour CSV with 1 Hour Interval`. The data processing happens in the cloud.