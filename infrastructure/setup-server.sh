#!/bin/bash

#######################################
# iot_esp_101 Sistemi - VM Kurulum Script'i
# 
# Bu script, uzak sunucuya SSH ile bağlanarak
# Docker ve gerekli servisleri kurar.
#
# Kullanım:
#   ./setup-server.sh <SSH_HOST> <SSH_USER> [SSH_PORT]
#
# Örnek:
#   ./setup-server.sh 192.168.1.100 ubuntu
#   ./setup-server.sh 10.1.30.50 root 2222
#######################################

set -e

# Renkli çıktı
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Banner
echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║       🌱 iot_esp_101 Sistemi - Sunucu Kurulumu 🌱           ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

# Argümanları kontrol et
if [ -z "$1" ] || [ -z "$2" ]; then
    echo -e "${RED}Hata: Eksik argümanlar!${NC}"
    echo ""
    echo "Kullanım: $0 <SSH_HOST> <SSH_USER> [SSH_PORT]"
    echo ""
    echo "Örnek:"
    echo "  $0 192.168.1.100 ubuntu"
    echo "  $0 10.1.30.50 root 2222"
    exit 1
fi

SSH_HOST="$1"
SSH_USER="$2"
SSH_PORT="${3:-22}"

echo -e "${BLUE}Bağlantı Bilgileri:${NC}"
echo "  Host: $SSH_HOST"
echo "  User: $SSH_USER"
echo "  Port: $SSH_PORT"
echo ""

# SSH bağlantı komutu
SSH_CMD="ssh -o StrictHostKeyChecking=accept-new -p $SSH_PORT $SSH_USER@$SSH_HOST"

# Bağlantı testi
echo -e "${YELLOW}[1/6] SSH bağlantısı test ediliyor...${NC}"
if ! $SSH_CMD "echo 'Bağlantı başarılı!'" 2>/dev/null; then
    echo -e "${RED}SSH bağlantısı başarısız!${NC}"
    echo "Lütfen SSH key'inizi ekleyin veya şifreyi girin."
    exit 1
fi
echo -e "${GREEN}✓ SSH bağlantısı başarılı${NC}"
echo ""

# Sistem güncelleme
echo -e "${YELLOW}[2/6] Sistem güncelleniyor...${NC}"
$SSH_CMD "sudo apt-get update -qq && sudo apt-get upgrade -y -qq"
echo -e "${GREEN}✓ Sistem güncellendi${NC}"
echo ""

# Docker kurulumu
echo -e "${YELLOW}[3/6] Docker kontrol ediliyor/kuruluyor...${NC}"
$SSH_CMD '
if command -v docker &> /dev/null; then
    echo "Docker zaten kurulu: $(docker --version)"
else
    echo "Docker kuruluyor..."
    curl -fsSL https://get.docker.com | sudo sh
    sudo usermod -aG docker $USER
    echo "Docker kuruldu!"
fi

# Docker Compose V2 kontrol
if docker compose version &> /dev/null; then
    echo "Docker Compose zaten kurulu: $(docker compose version)"
else
    echo "Docker Compose kuruluyor..."
    sudo apt-get install -y docker-compose-plugin
fi
'
echo -e "${GREEN}✓ Docker hazır${NC}"
echo ""

# Proje dizinini oluştur
echo -e "${YELLOW}[4/6] Proje dizini hazırlanıyor...${NC}"
$SSH_CMD '
PROJECT_DIR="$HOME/akilli-tarim"
mkdir -p $PROJECT_DIR/{data/postgres,data/redis,data/mosquitto/{config,data,log},backend,frontend,nginx}
cd $PROJECT_DIR
echo "Proje dizini: $PROJECT_DIR"
'
echo -e "${GREEN}✓ Proje dizini oluşturuldu${NC}"
echo ""

# Konfigürasyon dosyalarını oluştur
echo -e "${YELLOW}[5/6] Konfigürasyon dosyaları oluşturuluyor...${NC}"

# Mosquitto config
$SSH_CMD 'cat > $HOME/akilli-tarim/data/mosquitto/config/mosquitto.conf << "MOSQUITTO_CONF"
# Mosquitto MQTT Broker Configuration
listener 1883
allow_anonymous true

# WebSocket listener (opsiyonel)
listener 9001
protocol websockets

# Persistence
persistence true
persistence_location /mosquitto/data/

# Logging
log_dest stdout
log_type all
connection_messages true
log_timestamp true
MOSQUITTO_CONF
'

# Docker Compose
$SSH_CMD 'cat > $HOME/akilli-tarim/docker-compose.yml << "DOCKER_COMPOSE"
services:
  # PostgreSQL Database
  postgres:
    image: postgres:16-alpine
    container_name: tarim-postgres
    restart: unless-stopped
    environment:
      POSTGRES_DB: akilli_tarim
      POSTGRES_USER: tarim_user
      POSTGRES_PASSWORD: tarim_secret_2024
    volumes:
      - ./data/postgres:/var/lib/postgresql/data
    ports:
      - "5432:5432"
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U tarim_user -d akilli_tarim"]
      interval: 10s
      timeout: 5s
      retries: 5

  # Redis Cache
  redis:
    image: redis:7-alpine
    container_name: tarim-redis
    restart: unless-stopped
    command: redis-server --appendonly yes
    volumes:
      - ./data/redis:/data
    ports:
      - "6379:6379"
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 10s
      timeout: 5s
      retries: 5

  # Mosquitto MQTT Broker
  mosquitto:
    image: eclipse-mosquitto:2
    container_name: tarim-mosquitto
    restart: unless-stopped
    volumes:
      - ./data/mosquitto/config:/mosquitto/config
      - ./data/mosquitto/data:/mosquitto/data
      - ./data/mosquitto/log:/mosquitto/log
    ports:
      - "1883:1883"
      - "9001:9001"
    healthcheck:
      test: ["CMD", "mosquitto_sub", "-t", "$$SYS/#", "-C", "1", "-i", "healthcheck", "-W", "3"]
      interval: 30s
      timeout: 10s
      retries: 3

  # Backend API (Python FastAPI) - placeholder
  # backend:
  #   build: ./backend
  #   container_name: tarim-backend
  #   restart: unless-stopped
  #   environment:
  #     DATABASE_URL: postgresql://tarim_user:tarim_secret_2024@postgres:5432/akilli_tarim
  #     REDIS_URL: redis://redis:6379
  #     MQTT_BROKER: mosquitto
  #   ports:
  #     - "8000:8000"
  #   depends_on:
  #     postgres:
  #       condition: service_healthy
  #     redis:
  #       condition: service_healthy
  #     mosquitto:
  #       condition: service_healthy

  # Adminer - Database Web UI (geliştirme için)
  adminer:
    image: adminer:latest
    container_name: tarim-adminer
    restart: unless-stopped
    ports:
      - "8080:8080"
    depends_on:
      - postgres

networks:
  default:
    name: tarim-network
DOCKER_COMPOSE
'

echo -e "${GREEN}✓ Konfigürasyon dosyaları oluşturuldu${NC}"
echo ""

# Servisleri başlat
echo -e "${YELLOW}[6/6] Docker servisleri başlatılıyor...${NC}"
$SSH_CMD '
cd $HOME/akilli-tarim
docker compose up -d

echo ""
echo "Servis durumları:"
docker compose ps
'
echo -e "${GREEN}✓ Servisler başlatıldı${NC}"
echo ""

# Özet bilgiler
echo -e "${CYAN}"
echo "╔══════════════════════════════════════════════════════════════╗"
echo "║                    🎉 Kurulum Tamamlandı! 🎉                 ║"
echo "╚══════════════════════════════════════════════════════════════╝"
echo -e "${NC}"

echo -e "${GREEN}Servis Endpoint'leri:${NC}"
echo ""
echo "  📊 PostgreSQL:"
echo "     Host: ${SSH_HOST}:5432"
echo "     Database: akilli_tarim"
echo "     User: tarim_user"
echo "     Password: tarim_secret_2024"
echo ""
echo "  🔴 Redis:"
echo "     Host: ${SSH_HOST}:6379"
echo ""
echo "  📡 MQTT Broker (Mosquitto):"
echo "     Host: ${SSH_HOST}:1883"
echo "     WebSocket: ${SSH_HOST}:9001"
echo ""
echo "  🗄️ Adminer (DB Web UI):"
echo "     http://${SSH_HOST}:8080"
echo ""
echo -e "${YELLOW}ESP32 Kurulum:${NC}"
echo "  1. ESP32'yi bilgisayara bağlayın"
echo "  2. Firmware'i yükleyin (PlatformIO: Upload)"
echo "  3. Telefon/bilgisayar ile 'AkilliTarim-Setup' WiFi'ına bağlanın"
echo "  4. Açılan sayfada WiFi ve MQTT ayarlarını yapın:"
echo "     - MQTT Host: ${SSH_HOST}"
echo "     - MQTT Port: 1883"
echo ""
echo -e "${BLUE}MQTT Test Komutu:${NC}"
echo "  mosquitto_sub -h ${SSH_HOST} -t 'akilli-tarim/#' -v"
echo ""
