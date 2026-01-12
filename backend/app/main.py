"""
iot_esp_101 - Espressif - ESP32 - iot_esp_101
FastAPI uygulaması
"""

from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import HTMLResponse, RedirectResponse
import httpx
import asyncio
import json
import subprocess
import os
import mimetypes
from pathlib import Path
from typing import Optional, List, Dict, Any
from pydantic import BaseModel, Field
from datetime import datetime
import aiomqtt

# ========================================
# Lifespan
# ========================================
@asynccontextmanager
async def lifespan(app: FastAPI):
    print("iot_esp_101 Yönetim Paneli başlatılıyor...")
    # MQTT Manager'ı başlat
    mqtt_task = asyncio.create_task(mqtt_manager.run())
    yield
    # MQTT Manager'ı durdur
    mqtt_manager.stop()
    mqtt_task.cancel()
    try:
        await mqtt_task
    except asyncio.CancelledError:
        pass
    print("👋 Uygulama kapatılıyor...")


# ========================================
# FastAPI App
# ========================================
app = FastAPI(
    title="Espressif - ESP32 - iot_esp_101",
    description="ESP32, Docker ve MQTT yönetimi için birleşik kontrol paneli",
    version="1.0.0",
    lifespan=lifespan
)

# CORS
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


# ========================================
# Models
# ========================================
class ContainerStatus(BaseModel):
    name: str
    status: str
    ports: str
    image: str


class MQTTMessage(BaseModel):
    topic: str
    payload: str
    timestamp: datetime = Field(default_factory=datetime.now)

class DeviceStatus(BaseModel):
    deviceId: str
    ip: str
    status: str
    lastSeen: datetime
    sensors: Dict[str, Any]

# ========================================
# MQTT & Device Manager
# ========================================
class MQTTManager:
    def __init__(self, broker_host=None, broker_port=None):
        self.broker_host = broker_host or os.getenv("MQTT_HOST", "iot_esp_101-mosquitto")
        self.broker_port = int(broker_port or os.getenv("MQTT_PORT", 1883))
        self.is_running = False
        self.connected_clients = set()
        self.devices: Dict[str, Dict[str, Any]] = {}

    async def run(self):
        self.is_running = True
        print(f"📡 MQTT Manager başlatılıyor: {self.broker_host}:{self.broker_port}")
        
        while self.is_running:
            try:
                async with aiomqtt.Client(self.broker_host, self.broker_port) as client:
                    await client.subscribe("iot_esp_101/#")
                    print("✅ MQTT Broker'a bağlandı ve iot_esp_101/# topic'i dinleniyor.")
                    
                    async for message in client.messages:
                        await self.handle_message(message)
                        
            except aiomqtt.MqttError:
                print("❌ MQTT bağlantısı koptu, 5 saniye sonra tekrar denenecek...")
                await asyncio.sleep(5)
            except Exception as e:
                print(f"⚠️ MQTT Hatası: {str(e)}")
                await asyncio.sleep(5)

    def stop(self):
        self.is_running = False

    async def handle_message(self, message):
        topic = message.topic.value
        try:
            payload = message.payload.decode()
            data = json.loads(payload)
        except:
            payload = str(message.payload)
            data = payload

        # Broadcaster: Tüm mesajları WebSocket üzerinden yay
        msg_obj = {
            "topic": topic,
            "payload": data,
            "timestamp": datetime.now().isoformat()
        }
        
        # WebSocket istemcilerine gönder
        disconnected = set()
        for ws in self.connected_clients:
            try:
                await ws.send_json(msg_obj)
            except:
                disconnected.add(ws)
        
        for ws in disconnected:
            self.connected_clients.remove(ws)

        # Cihaz Takibi: iot_esp_101/devices/{id}/status topic'ini işle
        if topic.startswith("iot_esp_101/devices/") and topic.endswith("/status"):
            if isinstance(data, dict) and "deviceId" in data:
                dev_id = data["deviceId"]
                self.devices[dev_id] = {
                    "deviceId": dev_id,
                    "ip": data.get("ip", "unknown"),
                    "status": data.get("status", "online"),
                    "lastSeen": datetime.now().isoformat(),
                    "sensors": data.get("sensors", {})
                }

    def get_devices(self):
        return list(self.devices.values())

mqtt_manager = MQTTManager()


# ========================================
# Docker Status API
# ========================================
@app.get("/api/docker/containers")
async def get_docker_containers():
    """Docker container durumlarını getir"""
    try:
        result = subprocess.run(
            ["docker", "ps", "-a", "--format", "{{json .}}"],
            capture_output=True,
            text=True,
            timeout=10
        )
        
        containers = []
        for line in result.stdout.strip().split('\n'):
            if line:
                try:
                    c = json.loads(line)
                    containers.append({
                        "id": c.get("ID", "")[:12],
                        "name": c.get("Names", ""),
                        "image": c.get("Image", ""),
                        "status": c.get("Status", ""),
                        "ports": c.get("Ports", ""),
                        "state": c.get("State", ""),
                    })
                except json.JSONDecodeError:
                    pass
        
        return {"containers": containers, "count": len(containers)}
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail="Docker timeout")
    except FileNotFoundError:
        raise HTTPException(status_code=500, detail="Docker not found")
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/docker/containers/{name}/restart")
async def restart_container(name: str):
    """Container'ı yeniden başlat"""
    try:
        result = subprocess.run(
            ["docker", "restart", name],
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            return {"success": True, "message": f"{name} yeniden başlatıldı"}
        else:
            raise HTTPException(status_code=500, detail=result.stderr)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/docker/containers/{name}/stop")
async def stop_container(name: str):
    """Container'ı durdur"""
    try:
        result = subprocess.run(
            ["docker", "stop", name],
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            return {"success": True, "message": f"{name} durduruldu"}
        else:
            raise HTTPException(status_code=500, detail=result.stderr)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.post("/api/docker/containers/{name}/start")
async def start_container(name: str):
    """Container'ı başlat"""
    try:
        result = subprocess.run(
            ["docker", "start", name],
            capture_output=True,
            text=True,
            timeout=30
        )
        if result.returncode == 0:
            return {"success": True, "message": f"{name} başlatıldı"}
        else:
            raise HTTPException(status_code=500, detail=result.stderr)
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


@app.get("/api/docker/containers/{name}/logs")
async def get_container_logs(name: str, tail: int = 100):
    """Container loglarını getir"""
    try:
        result = subprocess.run(
            ["docker", "logs", "--tail", str(tail), name],
            capture_output=True,
            text=True,
            timeout=10
        )
        return {"logs": result.stdout + result.stderr}
    except Exception as e:
        raise HTTPException(status_code=500, detail=str(e))


# ========================================
# MQTT & Devices API
# ========================================
@app.get("/api/devices")
async def get_devices():
    """Kayıtlı cihazları getir"""
    return {"devices": mqtt_manager.get_devices()}

@app.websocket("/ws/mqtt")
async def mqtt_websocket(websocket: WebSocket):
    """MQTT mesajları için WebSocket bağlantısı"""
    await websocket.accept()
    mqtt_manager.connected_clients.add(websocket)
    try:
        while True:
            # Bağlantıyı açık tutmak için bekle
            await websocket.receive_text()
    except WebSocketDisconnect:
        mqtt_manager.connected_clients.remove(websocket)
    except Exception:
        if websocket in mqtt_manager.connected_clients:
            mqtt_manager.connected_clients.remove(websocket)
@app.get("/api/serial/ports")
async def list_serial_ports():
    """Mevcut serial portları listele"""
    import serial.tools.list_ports
    
    ports = []
    for port in serial.tools.list_ports.comports():
        ports.append({
            "device": port.device,
            "description": port.description,
            "hwid": port.hwid,
            "manufacturer": port.manufacturer or "Unknown",
            "is_esp32": "CP210" in port.description or "CH340" in port.description or "usbserial" in port.device.lower()
        })
    
    return {"ports": ports, "count": len(ports)}


# WebSocket for Serial Terminal
serial_connections = {}

@app.websocket("/ws/serial/{port_name:path}")
async def serial_websocket(websocket: WebSocket, port_name: str):
    """Serial port için WebSocket bağlantısı"""
    import serial
    
    await websocket.accept()
    
    # Port adını decode et (/ karakterleri için)
    port_name = "/" + port_name if not port_name.startswith("/") else port_name
    
    try:
        ser = serial.Serial(port_name, 115200, timeout=0.1)
        serial_connections[port_name] = ser
        
        await websocket.send_json({"type": "connected", "port": port_name})
        
        # Okuma ve yazma için paralel task'lar
        async def read_serial():
            while True:
                try:
                    if ser.in_waiting:
                        data = ser.read(ser.in_waiting)
                        await websocket.send_json({
                            "type": "data",
                            "data": data.decode('utf-8', errors='replace')
                        })
                    await asyncio.sleep(0.05)
                except Exception as e:
                    await websocket.send_json({"type": "error", "message": str(e)})
                    break
        
        read_task = asyncio.create_task(read_serial())
        
        try:
            while True:
                message = await websocket.receive_json()
                if message.get("type") == "write":
                    data = message.get("data", "")
                    ser.write(data.encode())
        except WebSocketDisconnect:
            pass
        finally:
            read_task.cancel()
            
    except serial.SerialException as e:
        await websocket.send_json({"type": "error", "message": str(e)})
    finally:
        if port_name in serial_connections:
            serial_connections[port_name].close()
            del serial_connections[port_name]
        await websocket.close()


# ========================================
# MQTT Status (basit)
# ========================================
@app.get("/api/mqtt/status")
async def mqtt_status():
    """MQTT broker durumunu kontrol et"""
    try:
        # Mosquitto container'ının durumunu kontrol et
        result = subprocess.run(
            ["docker", "exec", "iot_esp_101-mosquitto", "mosquitto_pub", "-t", "$SYS/test", "-m", "ping", "-q", "0"],
            capture_output=True,
            text=True,
            timeout=5
        )
        return {"status": "online" if result.returncode == 0 else "offline"}
    except:
        return {"status": "unknown"}


# ========================================
# Adminer Proxy
# ========================================
@app.get("/db-yonetimi")
@app.get("/db-yonetimi/{path:path}")
async def adminer_proxy(path: str = ""):
    """Adminer'a yönlendir veya iframe embed et"""
    # Adminer'ı iframe olarak embed ediyoruz
    if not path:
        html = """
<!DOCTYPE html>
<html>
<head>
    <title>Veritabanı Yönetimi - Adminer</title>
    <style>
        body, html { margin: 0; padding: 0; height: 100%; overflow: hidden; }
        iframe { width: 100%; height: 100%; border: none; }
    </style>
</head>
<body>
    <iframe src="http://localhost:8080/?pgsql=iot_esp_101-postgres&username=iot_esp_101_user&db=iot_esp_101"></iframe>
</body>
</html>
        """
        return HTMLResponse(content=html)
    return RedirectResponse(url=f"http://localhost:8080/{path}")


# ========================================
# Health Check
# ========================================
@app.get("/api/health")
async def health_check():
    """Sistem sağlık kontrolü"""
    return {
        "status": "healthy",
        "version": "1.0.0",
        "services": {
            "api": "running",
        }
    }


# ========================================
# Static Files (Frontend)
# ========================================
frontend_path = Path(__file__).parent.parent / "frontend"
if frontend_path.exists():
    app.mount("/", StaticFiles(directory=str(frontend_path), html=True), name="frontend")


# ========================================
# Ana sayfa
# ========================================
@app.get("/", response_class=HTMLResponse)
async def root():
    """Ana sayfa - Frontend varsa onu, yoksa API bilgisini göster"""
    if frontend_path.exists() and (frontend_path / "index.html").exists():
        return (frontend_path / "index.html").read_text()
    
    return """
    <html>
        <head><title>iot_esp_101 API</title></head>
        <body>
            <h1>🌱 Espressif - ESP32 - iot_esp_101</h1>
            <p>API Docs: <a href="/docs">/docs</a></p>
            <p>Frontend henüz oluşturulmadı.</p>
        </body>
    </html>
    """
