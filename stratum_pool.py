# stratum_pool_real.py
import socket
import threading
import json
import time
import hashlib

HOST = '0.0.0.0'
PORT = 3333
TARGET = "0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"

workers = {}
current_job = None
job_lock = threading.Lock()

def create_job():
    timestamp = str(int(time.time()))
    data = hashlib.sha256(timestamp.encode()).hexdigest()[:16]
    job_id = str(int(time.time() * 1000))
    return {
        "id": job_id,
        "method": "mining.notify",
        "params": [job_id, data, TARGET]
    }

def get_current_job():
    global current_job
    with job_lock:
        if current_job is None:
            current_job = create_job()
        return current_job

def update_job_periodically():
    global current_job
    while True:
        time.sleep(10)
        with job_lock:
            current_job = create_job()
        print("🔄 Job updated")

def validate_share(data, nonce, target):
    h = hashlib.sha256((data + str(nonce)).encode()).hexdigest()
    return h < target

def handle_worker(conn, addr):
    print(f"Worker connected: {addr}")
    
    # ได้รับ job ปัจจุบัน
    job = get_current_job()
    job_data = job["params"][1]
    
    # เก็บข้อมูลเฉพาะของ worker นี้
    workers[addr] = {
        "conn": conn,
        "shares": 0,
        "job_data": job_data  # ✅ เก็บ data ที่ส่งให้ worker นี้
    }

    try:
        conn.sendall((json.dumps(job) + "\n").encode())

        while True:
            data_bytes = conn.recv(4096)
            if not data_bytes:
                break
            msg = data_bytes.decode().strip()
            if not msg:
                continue

            try:
                parsed = json.loads(msg)
                if parsed.get("method") == "submit":
                    nonce = parsed["params"][0]
                    hash_val = parsed["params"][1]

                    # ✅ ใช้ job_data ที่ worker คนนี้ได้รับตอนแรก
                    job_data = workers[addr]["job_data"]

                    if validate_share(job_data, nonce, TARGET):
                        workers[addr]["shares"] += 1
                        print(f"✅ Valid share from {addr}: nonce={nonce}, hash={hash_val}")
                    else:
                        print(f"❌ Invalid share from {addr}: nonce={nonce}, hash={hash_val}")
            except Exception as e:
                print(f"Invalid message from {addr}: {e}")

    except Exception as e:
        print(f"Worker {addr} error: {e}")
    finally:
        conn.close()
        workers.pop(addr, None)
        print(f"Worker {addr} disconnected")

def main():
    threading.Thread(target=update_job_periodically, daemon=True).start()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen()
    print(f"🔹 Pool Server running on {HOST}:{PORT}")

    while True:
        conn, addr = s.accept()
        threading.Thread(target=handle_worker, args=(conn, addr), daemon=True).start()

if __name__ == "__main__":
    main()