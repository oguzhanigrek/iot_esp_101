-- iot_esp_101 Sistemi - PostgreSQL Initialization Script
-- Bu script container ilk başlatıldığında otomatik çalışır

-- ============================================
-- Extensions
-- ============================================
CREATE EXTENSION IF NOT EXISTS "uuid-ossp";
CREATE EXTENSION IF NOT EXISTS "pg_trgm";

-- ============================================
-- Devices Table
-- ============================================
CREATE TABLE IF NOT EXISTS devices (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    device_id VARCHAR(50) UNIQUE NOT NULL,
    name VARCHAR(100) NOT NULL,
    location VARCHAR(255),
    firmware_version VARCHAR(20),
    last_seen TIMESTAMPTZ,
    is_active BOOLEAN DEFAULT true,
    metadata JSONB DEFAULT '{}',
    created_at TIMESTAMPTZ DEFAULT NOW(),
    updated_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_devices_device_id ON devices(device_id);
CREATE INDEX idx_devices_active ON devices(is_active) WHERE is_active = true;

-- ============================================
-- Sensor Readings Table
-- ============================================
CREATE TABLE IF NOT EXISTS sensor_readings (
    id BIGSERIAL,
    device_id UUID REFERENCES devices(id) ON DELETE CASCADE,
    
    -- Sensör verileri
    soil_moisture DECIMAL(5,2),        -- % (0-100)
    temperature DECIMAL(5,2),          -- °C
    humidity DECIMAL(5,2),             -- % (0-100)
    pressure DECIMAL(7,2),             -- hPa
    uv_index DECIMAL(4,2),             -- UV Index (0-11+)
    rain_detected BOOLEAN,
    rain_intensity DECIMAL(5,2),       -- mm/h
    
    -- Hesaplanan değerler
    quality_score DECIMAL(5,2),
    
    -- Zaman damgası
    recorded_at TIMESTAMPTZ DEFAULT NOW(),
    
    PRIMARY KEY (id, recorded_at)
) PARTITION BY RANGE (recorded_at);

-- Partitions (aylık)
CREATE TABLE sensor_readings_2024_01 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-01-01') TO ('2024-02-01');
CREATE TABLE sensor_readings_2024_02 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-02-01') TO ('2024-03-01');
CREATE TABLE sensor_readings_2024_03 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-03-01') TO ('2024-04-01');
CREATE TABLE sensor_readings_2024_04 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-04-01') TO ('2024-05-01');
CREATE TABLE sensor_readings_2024_05 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-05-01') TO ('2024-06-01');
CREATE TABLE sensor_readings_2024_06 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-06-01') TO ('2024-07-01');
CREATE TABLE sensor_readings_2024_07 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-07-01') TO ('2024-08-01');
CREATE TABLE sensor_readings_2024_08 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-08-01') TO ('2024-09-01');
CREATE TABLE sensor_readings_2024_09 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-09-01') TO ('2024-10-01');
CREATE TABLE sensor_readings_2024_10 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-10-01') TO ('2024-11-01');
CREATE TABLE sensor_readings_2024_11 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-11-01') TO ('2024-12-01');
CREATE TABLE sensor_readings_2024_12 PARTITION OF sensor_readings
    FOR VALUES FROM ('2024-12-01') TO ('2025-01-01');

-- 2025 partitions
CREATE TABLE sensor_readings_2025_01 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-01-01') TO ('2025-02-01');
CREATE TABLE sensor_readings_2025_02 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-02-01') TO ('2025-03-01');
CREATE TABLE sensor_readings_2025_03 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-03-01') TO ('2025-04-01');
CREATE TABLE sensor_readings_2025_04 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-04-01') TO ('2025-05-01');
CREATE TABLE sensor_readings_2025_05 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-05-01') TO ('2025-06-01');
CREATE TABLE sensor_readings_2025_06 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-06-01') TO ('2025-07-01');
CREATE TABLE sensor_readings_2025_07 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-07-01') TO ('2025-08-01');
CREATE TABLE sensor_readings_2025_08 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-08-01') TO ('2025-09-01');
CREATE TABLE sensor_readings_2025_09 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-09-01') TO ('2025-10-01');
CREATE TABLE sensor_readings_2025_10 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-10-01') TO ('2025-11-01');
CREATE TABLE sensor_readings_2025_11 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-11-01') TO ('2025-12-01');
CREATE TABLE sensor_readings_2025_12 PARTITION OF sensor_readings
    FOR VALUES FROM ('2025-12-01') TO ('2026-01-01');

-- 2026 partitions
CREATE TABLE sensor_readings_2026_01 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-01-01') TO ('2026-02-01');
CREATE TABLE sensor_readings_2026_02 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-02-01') TO ('2026-03-01');
CREATE TABLE sensor_readings_2026_03 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-03-01') TO ('2026-04-01');
CREATE TABLE sensor_readings_2026_04 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-04-01') TO ('2026-05-01');
CREATE TABLE sensor_readings_2026_05 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-05-01') TO ('2026-06-01');
CREATE TABLE sensor_readings_2026_06 PARTITION OF sensor_readings
    FOR VALUES FROM ('2026-06-01') TO ('2026-07-01');

CREATE INDEX idx_readings_device_time ON sensor_readings(device_id, recorded_at DESC);

-- ============================================
-- Soil Analyses Table
-- ============================================
CREATE TABLE IF NOT EXISTS soil_analyses (
    id UUID PRIMARY KEY DEFAULT uuid_generate_v4(),
    device_id UUID REFERENCES devices(id) ON DELETE CASCADE,
    
    -- Görüntü bilgileri
    image_path VARCHAR(500),
    image_hash VARCHAR(64),
    
    -- Görüntü analiz sonuçları
    dominant_color_rgb JSONB,
    color_category VARCHAR(50),
    estimated_organic_matter DECIMAL(5,2),
    estimated_moisture_visual DECIMAL(5,2),
    
    -- Sensör snapshot
    sensor_snapshot JSONB,
    
    -- Birleşik analiz
    overall_score DECIMAL(5,2),
    health_status VARCHAR(20),
    recommendations JSONB,
    
    analyzed_at TIMESTAMPTZ DEFAULT NOW(),
    created_at TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX idx_analyses_device ON soil_analyses(device_id, created_at DESC);
CREATE INDEX idx_analyses_status ON soil_analyses(health_status);

-- ============================================
-- Sensor Aggregates Table (Günlük/Saatlik Özetler)
-- ============================================
CREATE TABLE IF NOT EXISTS sensor_aggregates (
    id BIGSERIAL PRIMARY KEY,
    device_id UUID REFERENCES devices(id) ON DELETE CASCADE,
    period_start TIMESTAMPTZ NOT NULL,
    period_type VARCHAR(10) NOT NULL,  -- hourly, daily
    
    avg_soil_moisture DECIMAL(5,2),
    min_soil_moisture DECIMAL(5,2),
    max_soil_moisture DECIMAL(5,2),
    
    avg_temperature DECIMAL(5,2),
    min_temperature DECIMAL(5,2),
    max_temperature DECIMAL(5,2),
    
    avg_humidity DECIMAL(5,2),
    avg_pressure DECIMAL(7,2),
    avg_uv_index DECIMAL(4,2),
    
    rain_count INTEGER DEFAULT 0,
    reading_count INTEGER DEFAULT 0,
    
    UNIQUE(device_id, period_start, period_type)
);

CREATE INDEX idx_aggregates_lookup ON sensor_aggregates(device_id, period_type, period_start DESC);

-- ============================================
-- Updated At Trigger
-- ============================================
CREATE OR REPLACE FUNCTION update_updated_at_column()
RETURNS TRIGGER AS $$
BEGIN
    NEW.updated_at = NOW();
    RETURN NEW;
END;
$$ language 'plpgsql';

CREATE TRIGGER update_devices_updated_at
    BEFORE UPDATE ON devices
    FOR EACH ROW
    EXECUTE FUNCTION update_updated_at_column();

-- ============================================
-- Sample Data (Development)
-- ============================================
INSERT INTO devices (device_id, name, location, firmware_version, is_active)
VALUES ('deneyap-001', 'Sera Sensör 1', 'Ana Sera - Domates Bölümü', '1.0.0', true)
ON CONFLICT (device_id) DO NOTHING;

-- ============================================
-- Grants
-- ============================================
GRANT ALL PRIVILEGES ON ALL TABLES IN SCHEMA public TO iot_esp_101_user;
GRANT ALL PRIVILEGES ON ALL SEQUENCES IN SCHEMA public TO iot_esp_101_user;
