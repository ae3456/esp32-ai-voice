# 智能音箱 + LED 远程控制项目分析报告

## 项目架构

```
┌─────────────┐      WebSocket       ┌─────────────────────────────────────┐
│   ESP32     │ ◄──────────────────► │  FastAPI 服务器 (阿里云 139.196.221.55) │
│  智能音箱    │    ws://host:8888    │         /ws/esp32 端点               │
└─────────────┘                      └─────────────────────────────────────┘
                                               │
                                               │ 转发 "1"/"0"
                                               ▼
                                      ┌─────────────────┐
                                      │  LED 控制器 ESP32 │
                                      │  /ws/led 端点    │
                                      └─────────────────┘
                                               │
                                               │ RabbitMQ
                                               ▼
                              ┌────────────────────────────────┐
                              │       RabbitMQ 消息队列         │
                              │  llm_request_queue             │
                              │  tts_request_queue             │
                              │  websocket_response_queue      │
                              └────────────────────────────────┘
                                               │
                    ┌──────────────────────────┴──────────────────────────┐
                    ▼                                                      ▼
           ┌─────────────────┐                                   ┌─────────────────┐
           │   LLM Worker    │                                   │   TTS Worker    │
           │  语音识别 + LLM  │                                   │  文本转语音      │
           └─────────────────┘                                   └─────────────────┘
```

## 通信流程

### 1. 智能音箱 (ESP32) 信号发送
- 收到音频数据时发送 `"1"` → LED 灯亮
- 播放结束时发送 `"0"` → LED 灯灭

### 2. 服务器端 (fastapi_app.py)
```python
# 处理来自 ESP32 的 "1" 和 "0" 消息
if text == "1" or text == "0":
    if led_controller_connection:
        await led_controller_connection.send_text(text)
```

### 3. LED 控制器端点 (/ws/led)
```python
@app.websocket("/ws/led")
async def led_websocket_endpoint(websocket: WebSocket):
    led_controller_connection = websocket
    # 接收 "1" 或 "0" 控制 LED
```

## 当前问题诊断

### 问题现象
```
I (47353) 语音识别: 收到WebSocket二进制数据，长度: 24 字节
E (47421) websocket_client: Websocket client is not connected
```

### 根本原因
**TTS Worker 返回了空音频数据！**

在 `tts_worker.py` 中：
```python
response_audio_bytes = synthesize_speech_stream(ai_response_text)
encoded_audio = base64.b64encode(response_audio_bytes).decode('utf-8')
```

如果 `synthesize_speech_stream()` 返回 `b""`（TTS 失败），则 `encoded_audio` 为空字符串。

在 `fastapi_app.py` 中：
```python
audio_b64 = data.get("audio_data_base64")
if audio_b64:  # ← 空字符串为 False，跳过音频发送！
    # 发送音频...
# 只发送结束标志
await ws.send_text(json.dumps({"event": "response_finished"}))
```

24 字节就是 `{"event": "response_finished"}` 的长度！

### TTS 失败可能原因
1. **百度语音 API 密钥配置错误** (APP_ID, API_KEY, SECRET_KEY)
2. **网络问题** (阿里云服务器无法访问百度 API)
3. **文本问题** (LLM 返回的文本包含特殊字符或太长)

## 修复方案

### 1. 修复 TTS Worker 错误处理
在 `tts_worker.py` 中：

```python
def callback(ch, method, properties, body):
    try:
        task_data = json.loads(body)
        user_id = task_data['user_id']
        task_id = task_data['task_id']
        ai_response_text = task_data['ai_response_text']
        
        logger.info(f"[{user_id}][{task_id}] TTS Worker 收到任务。文本: {ai_response_text[:50]}...")
        
        # TTS 合成
        response_audio_bytes = synthesize_speech_stream(ai_response_text)
        
        # 【修复】检查 TTS 是否成功
        if not response_audio_bytes:
            logger.error(f"[{user_id}][{task_id}] TTS 合成失败！")
            # 发送错误消息给客户端
            error_message = {
                "user_id": user_id,
                "ai_response": "抱歉，语音合成失败，请重试。",
                "audio_data_base64": ""  # 空音频
            }
            ch.basic_publish(
                exchange='',
                routing_key='websocket_response_queue',
                body=json.dumps(error_message),
                properties=pika.BasicProperties(
                    delivery_mode=pika.DeliveryMode.Persistent
                )
            )
            ch.basic_ack(delivery_tag=method.delivery_tag)
            return
        
        # 正常处理...
        encoded_audio = base64.b64encode(response_audio_bytes).decode('utf-8')
        final_message = {
            "ai_response": ai_response_text,
            "audio_data_base64": encoded_audio,
            "user_id": user_id,
        }
        # ...
```

### 2. 修复 FastAPI 空数据处理
在 `fastapi_app.py` 中：

```python
# 处理音频数据
audio_b64 = data.get("audio_data_base64")
if audio_b64:
    audio_bytes = base64.b64decode(audio_b64)
    total_size = len(audio_bytes)
    logger.info(f"[{user_id}] 开始流式发送音频 ({total_size} bytes)...")
    
    # 【修复】检查音频大小
    if total_size == 0:
        logger.error(f"[{user_id}] 收到空音频数据！")
        await ws.send_text(json.dumps({
            "event": "error",
            "message": "语音合成失败"
        }))
    else:
        # 正常流式发送...
        for i in range(0, total_size, CHUNK_SIZE):
            chunk = audio_bytes[i : i + CHUNK_SIZE]
            try:
                await ws.send_bytes(chunk)
            except Exception:
                logger.warning(f"[{user_id}] 发送音频时连接断开")
                break
            
            burst_count += 1
            if burst_count >= BURST_SIZE:
                burst_count = 0
                await asyncio.sleep(0.01)
        
        logger.info(f"[{user_id}] 音频流发送完毕")

# 发送结束标志
await ws.send_text(json.dumps({"event": "response_finished"}))
```

### 3. 检查百度语音 API 配置
在阿里云服务器上检查环境变量：
```bash
echo $BAIDU_VOICE_APP_ID
echo $BAIDU_VOICE_API_KEY
echo $BAIDU_VOICE_SECRET_KEY
```

如果不正确，更新 `.env` 文件并重启服务。

### 4. 测试百度 API
在服务器上运行测试脚本：
```python
from aip import AipSpeech
import os

APP_ID = os.getenv("BAIDU_VOICE_APP_ID")
API_KEY = os.getenv("BAIDU_VOICE_API_KEY")
SECRET_KEY = os.getenv("BAIDU_VOICE_SECRET_KEY")

client = AipSpeech(APP_ID, API_KEY, SECRET_KEY)

# 测试 TTS
result = client.synthesis('你好，这是一个测试', 'zh', 1, {'vol': 5})
if not isinstance(result, dict):
    print("TTS 成功！")
    print(f"音频大小: {len(result)} bytes")
else:
    print(f"TTS 失败: {result}")
```

## 本地测试步骤

### 1. 启动本地服务器
```bash
cd /home/ae/AIoT-Projects/ai-tutor-server-ch23
docker-compose up -d  # 启动 RabbitMQ 和 Redis

# 安装依赖
pip install -r requirements.txt

# 启动 FastAPI
python code/fastapi_app.py

# 在另一个终端启动 Workers
python code/llm_worker.py
python code/tts_worker.py
```

### 2. 修改 ESP32 连接地址
在 `main/main.cc` 中：
```cpp
#define WS_URI "ws://你的电脑IP:8888/ws/esp32"
```

### 3. 测试 LED 控制器
使用 wscat 或浏览器控制台连接：
```javascript
// 连接 LED 端点
const ws = new WebSocket('ws://localhost:8888/ws/led');
ws.onmessage = (e) => console.log('收到:', e.data);
// 应该会收到 "1" 和 "0"
```

## 总结

当前问题是 **TTS 返回空音频** 导致 ESP32 只收到 `response_finished` 消息而没有实际音频数据。

修复步骤：
1. 检查百度语音 API 密钥配置
2. 添加 TTS 错误处理
3. 添加空音频检查
4. 测试网络连接

修复后，LED 控制功能应该能正常工作！
