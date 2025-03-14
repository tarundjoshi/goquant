import asyncio
from cryptography.hazmat.primitives import serialization
import websockets

import base64
import json
from datetime import datetime

# client_id received when creating the API key
client_id='zavRq-oY'

with open('private.pem', 'rb') as private_pem:
    private_key = serialization.load_pem_private_key(private_pem.read(), password=None)

timestamp = round(datetime.now().timestamp() * 1000)
nonce = "abcd"
data = ""

data_to_sign = bytes('{}\n{}\n{}'.format(timestamp, nonce, data), "latin-1")
signature = base64.urlsafe_b64encode(private_key.sign(data_to_sign)).decode('utf-8').rstrip('=')

msg = {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "public/auth",
    "params": {
        "grant_type": "client_signature",
        "client_id": client_id,
        "timestamp": timestamp,
        "signature": signature,
        "nonce": nonce,
        "data": data
    }
}

print(signature)

async def call_api(msg):
    async with websockets.connect('wss://test.deribit.com/ws/api/v2') as websocket:
        await websocket.send(msg)
        while websocket.open:
            response = await websocket.recv()
            # do something with the response...
            print(response)
            await websocket.send(json.dumps({
                "jsonrpc": "2.0",
                "id": 2,
                "method": "private/get_positions",
                "params": {
                    "currency": "btc"
                }
            }))
            response = await websocket.recv()
            print(response)
            exit()

asyncio.get_event_loop().run_until_complete(call_api(json.dumps(msg)))