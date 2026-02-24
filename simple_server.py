#!/usr/bin/env python3
"""
Python FastAPI音频接收服务器
功能：接收ESP32传输的音频数据，保存为WAV文件
"""

import os
import json
import wave
import asyncio
from datetime import datetime
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
import uvicorn

class AudioReceiver:
    """音频接收器类"""

    def __init__(self):
        self.is_recording = False
        self.audio_buffer = bytearray()
        self.sample_rate = 16000  # 16kHz采样率
        self.channels = 1         # 单声道
        self.sample_width = 2     # 16位 = 2字节

    def start_recording(self):
        """开始录音"""
        self.is_recording = True
        self.audio_buffer.clear()
        print(f"开始接收音频数据...")

    def add_audio_data(self, data: bytes):
        """添加音频数据"""
        if self.is_recording:
            self.audio_buffer.extend(data)

    def stop_recording(self) -> str:
        """停止录音并保存文件"""
        self.is_recording = False

        if not self.audio_buffer:
            print("警告：没有接收到音频数据")
            return ""

        # 生成文件名（包含时间戳）
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = f"audio_record_{timestamp}.wav"

        try:
            # 保存为WAV文件
            with wave.open(filename, 'wb') as wav_file:
                wav_file.setnchannels(self.channels)          # 单声道
                wav_file.setsampwidth(self.sample_width)      # 16位
                wav_file.setframerate(self.sample_rate)       # 16kHz
                wav_file.writeframes(self.audio_buffer)

            file_size = len(self.audio_buffer)
            duration = file_size / (self.sample_rate * self.channels * self.sample_width)

            print(f"音频文件已保存: {filename}")
            print(f"   - 文件大小: {file_size} 字节 ({file_size/1024:.1f} KB)")
            print(f"   - 录音时长: {duration:.2f} 秒")
            print(f"   - 采样率: {self.sample_rate} Hz")
            print(f"   - 声道数: {self.channels}")
            print(f"   - 位深度: {self.sample_width * 8} 位")

            return filename

        except Exception as e:
            print(f"保存音频文件失败: {e}")
            return ""

# 创建FastAPI应用
app = FastAPI(title="ESP32音频接收服务器", version="1.0")

# 全局音频接收器
audio_receiver = AudioReceiver()

@app.get("/")
async def root():
    """根路径，显示服务器信息"""
    return {
        "message": "ESP32音频接收服务器",
        "version": "1.0",
        "status": "running",
        "description": "接收ESP32传输的音频数据并保存为WAV文件"
    }

@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: str):
    """WebSocket端点，接收ESP32的音频数据"""

    await websocket.accept()
    client_ip = websocket.client.host
    print(f"\n新的客户端连接: {client_ip} (ID: {client_id})")

    try:
        while True:
            # 接收消息
            try:
                message = await websocket.receive()
            except Exception as e:
                print(f"接收消息时出错: {e}")
                break

            # 处理文本消息（JSON事件）
            if "text" in message:
                try:
                    data = json.loads(message["text"])
                    event = data.get("event")

                    if event == "recording_started":
                        print(f"[{client_ip}] 开始录音事件")
                        audio_receiver.start_recording()

                    elif event == "recording_ended":
                        print(f"[{client_ip}] 录音结束事件")
                        filename = audio_receiver.stop_recording()

                        if filename:
                            # 发送成功响应
                            response = {
                                "event": "file_saved",
                                "filename": filename,
                                "status": "success"
                            }
                            await websocket.send_text(json.dumps(response))
                        else:
                            # 发送失败响应
                            response = {
                                "event": "file_save_failed",
                                "status": "failed"
                            }
                            await websocket.send_text(json.dumps(response))

                    else:
                        print(f"[{client_ip}] 收到事件: {event}")

                except json.JSONDecodeError as e:
                    print(f"[{client_ip}] JSON解析错误: {e}")
                except Exception as e:
                    print(f"[{client_ip}] 处理文本消息时出错: {e}")

            # 处理二进制消息（音频数据）
            elif "bytes" in message:
                audio_chunk = message["bytes"]
                audio_receiver.add_audio_data(audio_chunk)

                # 显示接收进度（可选，避免刷屏）
                total_size = len(audio_receiver.audio_buffer)
                if total_size % 8000 == 0:  # 每8KB显示一次
                    print(f"[{client_ip}] 接收音频数据: {total_size} 字节")

    except WebSocketDisconnect:
        print(f"[{client_ip}] 客户端断开连接")

        # 如果正在录音，保存已接收的数据
        if audio_receiver.is_recording:
            print(f"[{client_ip}] 客户端断开，保存已接收的音频数据...")
            audio_receiver.stop_recording()

    except Exception as e:
        print(f"[{client_ip}] WebSocket连接错误: {e}")

    finally:
        print(f"[{client_ip}] 连接处理完成")

@app.get("/files/{filename}")
async def get_file(filename: str):
    """下载保存的音频文件"""
    file_path = filename
    if os.path.exists(file_path):
        return FileResponse(file_path, media_type='audio/wav', filename=filename)
    else:
        return {"error": "文件不存在"}

@app.get("/list")
async def list_files():
    """列出所有保存的音频文件"""
    try:
        files = []
        for filename in os.listdir('.'):
            if filename.startswith('audio_record_') and filename.endswith('.wav'):
                stat = os.stat(filename)
                files.append({
                    'filename': filename,
                    'size': stat.st_size,
                    'created': datetime.fromtimestamp(stat.st_ctime).strftime("%Y-%m-%d %H:%M:%S")
                })
        return {"files": files, "count": len(files)}
    except Exception as e:
        return {"error": str(e)}

def get_local_ip():
    """获取本机IP地址"""
    import socket
    try:
        # 创建一个UDP socket
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # 连接到一个公网地址（不会实际发送数据）
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"

if __name__ == "__main__":
    # 获取本机IP地址
    local_ip = get_local_ip()
    port = 8000

    print("=" * 60)
    print("Python音频接收服务器")
    print("=" * 60)
    print(f"服务器地址: http://{local_ip}:{port}")
    print(f"WebSocket地址: ws://{local_ip}:{port}/ws/esp32")
    print("=" * 60)
    print("使用说明:")
    print("1. 确保ESP32代码中的WebSocket地址配置正确")
    print("2. 启动ESP32设备")
    print("3. ESP32会自动连接并传输音频数据")
    print("4. 音频文件将保存在当前目录下")
    print("5. 访问 http://localhost:8000/list 查看文件列表")
    print("=" * 60)
    print("等待ESP32连接...\n")

    # 启动服务器
    uvicorn.run(app, host="0.0.0.0", port=port)