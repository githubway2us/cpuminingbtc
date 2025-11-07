# Bitcoin Solo & Pool Miner (Educational)

โปรเจกต์นี้เป็นตัวอย่าง **Solo Miner และ Pool Worker สำหรับ Bitcoin** แบบเรียนรู้และทดลองเท่านั้น  
⚠️ **คำเตือน:** การขุด Bitcoin จริงบน Mainnet ด้วย CPU เป็นไปไม่ได้ในทางปฏิบัติ ใช้ **Regtest** เพื่อทดลอง

## 🛠️ การติดตั้ง

### 1. ติดตั้ง OpenSSL
```bash
sudo apt install libssl-dev
```

### 2. ดาวน์โหลดไฟล์ `json.hpp`
```bash
wget https://github.com/nlohmann/json/releases/latest/download/json.hpp
```

## 💻 การคอมไพล์

### Pool Worker
```bash
g++ -std=c++11 -O2 -pthread pool_worker.cpp -lssl -lcrypto -o pool_worker
```

### Solo Miner
```bash
g++ -std=c++11 -O2 solo_miner.cpp -lcurl -lcrypto -o solo_miner
```

## 🚀 การรัน

### Pool Worker
```bash
./pool_worker
```

### Solo Miner
```bash
./solo_miner
```

## ⚡ หมายเหตุ
- ใช้ **Legacy P2PKH addresses** เท่านั้น
- สำหรับ Mainnet ควรระวัง difficulty สูง
- สามารถทดสอบบน **Regtest** ได้อย่างปลอดภัย

## 📂 โครงสร้างไฟล์
```
.
├── pool_worker.cpp
├── pool_worker2
├── solo_miner.cpp
├── solo_miner
├── stratum_pool.py
├── app.py
├── json.hpp
└── วิธีใช้งาน.ini
```
