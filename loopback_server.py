"""
ESP32音频回环服务器 - 固定WAV文件回复版
功能：接收ESP32音频 -> 返回一个固定的本地WAV音频文件
"""

import os
import json
import asyncio
import wave  # <-- 1. 引入wave库来处理.wav文件
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
import socket

app = FastAPI(title="ESP32音频回环服务器", version="1.0")

# --- 2. 指定你要回复的WAV文件名 ---
REPLY_WAV_FILE = "audio_record_20251111_095832.wav"
REPLY_AUDIO_DATA = None  # 用于在启动时加载音频数据

def load_wav_audio(filepath: str):
    """从WAV文件中加载音频数据，并检查格式是否兼容ESP32"""
    try:
        with wave.open(filepath, 'rb') as wf:
            # --- 关键：检查音频参数是否与ESP32匹配 ---
            channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            framerate = wf.getframerate()

            print(f"正在加载WAV文件: {os.path.basename(filepath)}")
            print(f"  - 声道数: {channels}")
            print(f"  - 位深度: {sample_width * 8}-bit")
            print(f"  - 采样率: {framerate} Hz")

            if channels != 1 or sample_width != 2 or framerate != 16000:
                print("\n错误：WAV文件格式与ESP32不兼容！")
                print("请确保音频为: 单声道 (Mono), 16-bit, 16000 Hz 采样率。\n")
                return None
            
            # 读取所有音频帧
            audio_data = wf.readframes(wf.getnframes())
            duration = len(audio_data) / (16000 * 2)
            print(f"WAV文件加载成功: {len(audio_data)} 字节 ({duration:.2f}秒)")
            return audio_data

    except FileNotFoundError:
        print(f"\n错误：找不到指定的音频文件 '{filepath}'")
        print("请确保该文件与Python脚本在同一个目录下。\n")
        return None
    except Exception as e:
        print(f"加载WAV文件失败: {e}")
        return None

@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: str):
    """
    WebSocket端点 - ESP32音频回环
    """
    await websocket.accept()
    client_ip = websocket.client.host
    print(f"\nESP32客户端连接: {client_ip} (ID: {client_id})")

    # 客户端状态
    client_state = {
        "is_recording": False,
        "audio_buffer": bytearray(),
        "conversation_count": 0
    }

    try:
        while True:
            # 接收消息
            message = await websocket.receive()

            # 处理文本消息 (JSON事件)
            if "text" in message:
                data = json.loads(message["text"])
                event = data.get("event")

                if event == "recording_started":
                    print(f"[{client_ip}] 开始录音...")
                    client_state["is_recording"] = True
                    client_state["audio_buffer"].clear()

                elif event == "recording_ended":
                    print(f"[{client_ip}] 录音结束")
                    client_state["is_recording"] = False
                    client_state["conversation_count"] += 1

                    print(f"  - 接收到音频: {len(client_state['audio_buffer'])} 字节")
                    print("  - 开始回环测试...")

                    # --- 3. 修改回复逻辑 ---
                    # 直接使用预加载的WAV音频数据
                    if REPLY_AUDIO_DATA:
                        print(f"  - 返回固定WAV音频: {REPLY_WAV_FILE} ({len(REPLY_AUDIO_DATA)} 字节)")
                        await send_audio_stream(websocket, REPLY_AUDIO_DATA)
                    else:
                        print("警告：回复音频未加载，无法发送回复。")

                    print(f"回环测试完成 (第{client_state['conversation_count']}轮)\n")

            # 处理二进制消息 (音频数据)
            elif "bytes" in message:
                if client_state["is_recording"]:
                    audio_chunk = message["bytes"]
                    client_state["audio_buffer"].extend(audio_chunk)

    except WebSocketDisconnect:
        print(f"[{client_ip}] 客户端断开连接")
    except Exception as e:
        print(f"[{client_ip}] 连接错误: {e}")
        if websocket.client_state != "DISCONNECTED":
            await websocket.close()

async def send_audio_stream(websocket: WebSocket, audio_data: bytes):
    """
    流式发送音频数据到ESP32
    """
    CHUNK_SIZE = 3200  # 每次发送3200字节（200ms的音频）
    
    sent_bytes = 0
    for i in range(0, len(audio_data), CHUNK_SIZE):
        chunk = audio_data[i:i + CHUNK_SIZE]
        try:
            await websocket.send_bytes(chunk)
            sent_bytes += len(chunk)
            await asyncio.sleep(0.01)  # 短暂延时，模拟真实网络情况
        except Exception as e:
            print(f"发送音频失败: {e}")
            break

    # 发送ping包作为结束标志
    try:
        await websocket.ping()
        print(f"音频流发送完成: {sent_bytes}/{len(audio_data)} 字节")
    except:
        pass

def get_local_ip():
    """获取本机IP地址"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"

if __name__ == "__main__":
    import uvicorn

    PORT = 8000
    local_ip = get_local_ip()

    print("=" * 60)
    print("ESP32音频回环服务器 (固定WAV回复版)")
    print("=" * 60)
    print(f"服务器地址: http://{local_ip}:{PORT}")
    print(f"WebSocket地址: ws://{local_ip}:{PORT}/ws/esp32")
    print("=" * 60)
    
    # --- 4. 在服务器启动时加载WAV文件 ---
    REPLY_AUDIO_DATA = load_wav_audio(REPLY_WAV_FILE)
    
    if not REPLY_AUDIO_DATA:
        print("!!! 服务器将无法回复音频，请检查错误信息 !!!")

    print("=" * 60)
    print("\n服务器启动中...\n")

    uvicorn.run(app, host="0.0.0.0", port=PORT)