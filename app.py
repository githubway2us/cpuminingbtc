# app.py - Simple Flask API to display mining stats
# Run with: python app.py
# Access at http://localhost:5000/stats

from flask import Flask, jsonify, request
import json
import threading
import time
from datetime import datetime

app = Flask(__name__)

# In-memory storage for mining stats (use Redis/DB for production)
stats = {
    'tried': 0,
    'hashrate': 0.0,
    'current_nonce': 0,
    'btc_price': 'N/A',
    'last_update': datetime.now().isoformat(),
    'block_height': 0,
    'search_start': '0x00000000',
    'search_end': '0xffffffff'
}

# Background thread to simulate or keep alive (optional)
def keep_alive():
    while True:
        time.sleep(60)  # Update every minute if needed

@app.route('/stats', methods=['GET'])
def get_stats():
    return jsonify(stats)

@app.route('/update', methods=['POST'])
def update_stats():
    global stats
    data = request.json
    if data:
        stats.update(data)
        stats['last_update'] = datetime.now().isoformat()
        return jsonify({'status': 'updated', 'data': stats}), 200
    return jsonify({'error': 'No data provided'}), 400

@app.route('/', methods=['GET'])
def home():
    html = f"""
    <html>
    <head><title>Mining Stats Dashboard</title></head>
    <body style="font-family: monospace; background: #000; color: #0f0;">
        <h1 style="color: #0f0;">🚀 Hacker Mining Dashboard</h1>
        <div id="stats"></div>
        <script>
            function fetchStats() {{
                fetch('/stats')
                    .then(r => r.json())
                    .then(data => {{
                        document.getElementById('stats').innerHTML = `
                            <p><strong>Block Height:</strong> {data.block_height}</p>
                            <p><strong>Hashes Tried:</strong> {data.tried}</p>
                            <p><strong>Hashrate:</strong> {data.hashrate} H/s</p>
                            <p><strong>Current Nonce:</strong> 0x{data.current_nonce.toString(16).padStart(8, '0')}</p>
                            <p><strong>BTC Price:</strong> ${data.btc_price}</p>
                            <p><strong>Search Range:</strong> [{data.search_start} - {data.search_end}]</p>
                            <p><strong>Last Update:</strong> {data.last_update}</p>
                        `;
                    }});
            }}
            fetchStats();
            setInterval(fetchStats, 5000);  // Refresh every 5s
        </script>
    </body>
    </html>
    """
    return html

if __name__ == '__main__':
    threading.Thread(target=keep_alive, daemon=True).start()
    app.run(host='0.0.0.0', port=5339, debug=True)