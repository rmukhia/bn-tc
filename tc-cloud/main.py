import asyncio
import io
import json
import logging
import os
import sqlite3
from contextlib import asynccontextmanager
from datetime import datetime
from typing import Any, Dict, List

from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, Response
from fastapi.templating import Jinja2Templates
from fastapi_mqtt import FastMQTT, MQTTConfig
import pandas as pd

# ------------------------------------------------------------
# Setup
# ------------------------------------------------------------
load_dotenv()
logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO)

MQTT_HOST = os.getenv("MQTT_HOST", "localhost")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_ENABLED = os.getenv("MQTT_ENABLED", "true").lower() not in {"0", "false", "no", "off"}
DATABASE_PATH = os.getenv("DATABASE_PATH", os.path.join(os.getcwd(), "database.db"))

# ------------------------------------------------------------
# SQLite Database
# ------------------------------------------------------------
class SQLite:
    def __init__(self, path: str):
        self._path = path
        self._conn = sqlite3.connect(self._path, check_same_thread=False)
        self._conn.row_factory = sqlite3.Row
        self._init()

    def _init(self) -> None:
        cur = self._conn.cursor()
        cur.execute(
            """
            CREATE TABLE IF NOT EXISTS telemetry (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                device_id TEXT NOT NULL,
                longitude REAL,
                latitude REAL,
                battery INTEGER,
                date TEXT NOT NULL,
                time TEXT NOT NULL,
                inserted_at TEXT NOT NULL DEFAULT (datetime('now'))
            )
            """
        )
        self._conn.commit()

    def insert(self, record: Dict[str, Any]) -> None:
        self._conn.execute(
            "INSERT INTO telemetry (device_id, longitude, latitude, battery, date, time) VALUES (?, ?, ?, ?, ?, ?)",
            (record["device_id"], record["longitude"], record["latitude"], record["battery"], record["date"], record["time"]),
        )
        self._conn.commit()

    def list(self) -> List[Dict[str, Any]]:
        sql = """
            SELECT id, device_id, longitude, latitude, battery, date, time, inserted_at 
            FROM telemetry 
            ORDER BY inserted_at DESC 
        """
        cur = self._conn.execute(sql,)
        rows = cur.fetchall()
        return [dict(r) for r in rows]

db = SQLite(DATABASE_PATH)

# ------------------------------------------------------------
# Payload Processing
# ------------------------------------------------------------
def decode_payload(payload: str) -> Dict[str, float]:
    """Convert hexadecimal payload into numeric values"""
    payload = payload.strip().upper()
    if len(payload) != 10:
        raise ValueError("Payload must be exactly 10 hex characters")
    
    try:
        # Longitude: first 2 hex digits integral, next 2 hex digits fractional
        lon_hex_integral = payload[0:2]
        lon_hex_fractional = payload[2:4]
        longitude = int(lon_hex_integral, 16) + int(lon_hex_fractional, 16) / 100.0

        # Latitude: first 2 hex digits integral, next 2 hex digits fractional
        lat_hex_integral = payload[4:6]
        lat_hex_fractional = payload[6:8]
        latitude = int(lat_hex_integral, 16) + int(lat_hex_fractional, 16) / 100.0
        
        # Battery: last 2 hex digits
        bat_hex = payload[8:10]
        battery = int(bat_hex, 16)
        
        return {
            "longitude": round(longitude, 2),
            "latitude": round(latitude, 2),
            "battery": battery
        }
    except ValueError as e:
        raise ValueError(f"Invalid hex payload: {payload}") from e

def process_telemetry_message(message: Dict[str, str]) -> Dict[str, Any]:
    """Process incoming telemetry message and return database record"""
    required_fields = ["id", "payload", "date", "time"]
    for field in required_fields:
        if field not in message:
            raise ValueError(f"Missing required field: {field}")
    
    device_id = message["id"]
    payload = message["payload"]
    date = message["date"]
    time = message["time"]
    
    # Validate payload format
    decoded = decode_payload(payload)
    
    return {
        "device_id": device_id,
        "longitude": decoded["longitude"],
        "latitude": decoded["latitude"],
        "battery": decoded["battery"],
        "date": date,
        "time": time,
    }

# ------------------------------------------------------------
# FastAPI + MQTT
# ------------------------------------------------------------
mqtt_config = MQTTConfig(host=MQTT_HOST, port=MQTT_PORT, keepalive=60)
fast_mqtt = FastMQTT(config=mqtt_config)

@asynccontextmanager
async def lifespan(_app: FastAPI):
    # Start MQTT client if enabled
    started = False
    if MQTT_ENABLED:
        try:
            await fast_mqtt.mqtt_startup()
            started = True
            logger.info("MQTT client started successfully")
        except Exception as e:
            logger.warning("MQTT startup failed (%s:%s): %s. Continuing without MQTT.", MQTT_HOST, MQTT_PORT, e)
    else:
        logger.info("MQTT is disabled via MQTT_ENABLED env var.")
    
    try:
        yield
    finally:
        if started:
            try:
                await fast_mqtt.mqtt_shutdown()
            except Exception:
                pass

app = FastAPI(lifespan=lifespan)
templates = Jinja2Templates(directory="templates")

@fast_mqtt.on_connect()
def _on_connect(client, flags, rc, properties):
    logger.info("MQTT connected: rc=%s host=%s", rc, MQTT_HOST)

@fast_mqtt.on_disconnect()
def _on_disconnect(client, packet, exc=None):
    logger.info("MQTT disconnected")

# Subscribe to telemetry topic pattern
@fast_mqtt.subscribe("tc-bn/telemetry/+")
async def _on_message(client, topic: str, payload: bytes, qos: int, properties):
    try:
        message = json.loads(payload.decode())
        logger.info("Received MQTT message on topic %s: %s", topic, message)
        
        record = process_telemetry_message(message)
        logger.info(record)
        db.insert(record)
        logger.info("MQTT telemetry stored: device_id=%s", record["device_id"])
        
    except Exception as e:
        logger.exception("Failed to process MQTT message on %s: %s", topic, e)

# ------------------------------------------------------------
# HTTP Endpoints
# ------------------------------------------------------------
@app.post("/ingest")
async def ingest(request: Request):
    """Receive telemetry data via HTTP POST"""
    try:
        body = await request.json()
    except Exception:
        raise HTTPException(status_code=400, detail="Invalid JSON")
    
    try:
        record = process_telemetry_message(body)
        logger.info(record)
        db.insert(record)
        return JSONResponse(content={
            "status": "success",
            "device_id": record["device_id"],
            "longitude": record["longitude"],
            "latitude": record["latitude"], 
            "battery": record["battery"]
        })
    except ValueError as ve:
        raise HTTPException(status_code=400, detail=str(ve))
    except Exception as e:
        logger.exception("Failed to ingest via HTTP: %s", e)
        raise HTTPException(status_code=500, detail="Internal server error")

@app.get("/records")
async def list_records():
    """Get recent telemetry records"""
    items = db.list()
    return {
        "count": len(items),
        "items": items
    }

@app.get("/")
async def dashboard(request: Request):
    """Display telemetry dashboard"""
    items = db.list()
    
    # Format records for display
    view = []
    for item in items:
        view.append({
            "id": item["device_id"],
            "longitude": f"{item['longitude']:.2f}",
            "latitude": f"{item['latitude']:.2f}",
            "battery": f"{item['battery']}%",
            "date": item["date"],
            "time": item["time"],
        })
    
    return templates.TemplateResponse(
        "index.html",
        {"request": request, "records": view},
    )

@app.get("/download-csv-raw")
async def download_csv():
    """Download telemetry records as CSV"""
    items = db.list()
    
    # Create CSV content
    csv_content = "Device ID,Longitude,Latitude,Battery,Date,Time,Inserted At\n"
    for item in items:
        csv_content += f"{item['device_id']},{item['longitude']},{item['latitude']},{item['battery']},{item['date']},{item['time']},{item['inserted_at']}\n"
    
    # Return CSV file
    return Response(
        content=csv_content,
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=telemetry_data.csv"}
    )


@app.get("/download-csv-processed")
async def download_csv_processed():
    """Download processed telemetry records as CSV, with one hour intervals for a span of 12 hours"""
    items = db.list()
    df = pd.DataFrame(items)
    # create datetime column
    df['datetime'] = pd.to_datetime(df['date'] + ' ' + df['time'])

    # sort by datetime descending
    df = df.sort_values(by='datetime', ascending=False)

    # round to nearest hour boundary to assign each record to the closest hour
    df['hour'] = df['datetime'].dt.round('H')

    # calculate the distance from each record to its assigned hour boundary
    df['distance_to_hour'] = abs(df['datetime'] - df['hour'])

    # sort by hour and distance, keeping the closest record to each hour
    df = df.sort_values(by=['hour', 'distance_to_hour'], ascending=[False, True])

    # drop duplicates to keep only one record per hour (the closest one)
    df = df.drop_duplicates(subset=['hour'])

    # keep only the last 12 hours
    df = df.head(12)

    df.drop(['id', 'datetime', 'distance_to_hour'], axis=1, inplace=True)

    items = df.to_dict(orient='records')
    writeBuffer = io.StringIO()
    df.to_csv(writeBuffer, index=False)

    # Return CSV file
    return Response(
        content=writeBuffer.getvalue(),
        media_type="text/csv",
        headers={"Content-Disposition": "attachment; filename=processed_telemetry_data.csv"}
    )