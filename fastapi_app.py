# fastapi_app.py

import os
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from starlette.websockets import WebSocketState
from pydantic import BaseModel
from typing import Optional
import asyncio
import json

# 复用我们之前定义的所有 LangChain 和 Redis 相关组件
from langchain_openai import ChatOpenAI
from langchain.prompts import PromptTemplate
from langchain.chains import LLMChain
from langchain_community.chat_message_histories import RedisChatMessageHistory
from langchain.memory import ConversationBufferMemory
from aip import AipSpeech

# --- 1. 初始化所有客户端和服务 (无变化) ---
# 百度语音 API
APP_ID = os.getenv("BAIDU_VOICE_APP_ID")
API_KEY = os.getenv("BAIDU_VOICE_API_KEY")
SECRET_KEY = os.getenv("BAIDU_VOICE_SECRET_KEY")
print(APP_ID)
speech_client = AipSpeech(APP_ID, API_KEY, SECRET_KEY)

# --- 2. 核心业务逻辑函数 (无变化, 但做了精简修正) ---
def transcribe_audio_stream(audio_bytes: bytes) -> str:
    """接收PCM音频流，调用百度ASR API进行转录。"""
    # dev_pid=1537 是普通话模型
    stt_result = speech_client.asr(audio_bytes, 'pcm', 16000, {'dev_pid': 1537})
    if stt_result and stt_result.get('err_no') == 0 and stt_result.get('result'):
        return stt_result['result'][0]
    else:
        print(f"! STT (ASR) 错误: {stt_result}")
        return ""

def synthesize_speech_stream(text: str) -> bytes:
    """接收文本，调用百度TTS API合成语音。"""
    tts_result = speech_client.synthesis(
        text, 'zh', 1, 
        {
            'vol': 5, 
            'per': 5118, 
            'pit': 5, 
            'aue': 4,
            'audio_ctrl': {"sampling_rate":16000}}
    )
    if not isinstance(tts_result, dict):
        return tts_result
    else:
        print(f"! TTS (文本转语音) 错误: {tts_result}")
        return b""


# --- 1. 配置与初始化 (复用逻辑) ---
LLM_ENDPOINT_ID = os.getenv("LLM_ENDPOINT_ID")
llm = ChatOpenAI(
    model=LLM_ENDPOINT_ID,
    base_url=os.getenv("LLM_BASE_URL"),
    api_key=os.getenv("LLM_API_KEY"),
    temperature=0.7
)

template = """
# Role (角色设定)
你叫“韩立”，是一位在比亚迪工作多年的资深汽车工程师，参与过腾势 Z9GT 的研发。同时，你是一位耐心的中文私教。你的学生叫 Emma，她想学习新能源汽车设计。

# Style & Constraints (风格约束)
1. 优先专业性：以严谨、专业的口吻回答。
2. 语言教学：如果 Emma 的中文有明显错误，必须在回答完问题后，另起一段，温柔地指出并修正。

# Context (对话历史)
{chat_history}

# Current Turn (当前对话)
Emma: {question}
韩立:
"""
prompt = PromptTemplate(input_variables=["chat_history", "question"], template=template)

REDIS_URL = os.getenv("REDIS_URL", "redis://redis:6379/0")  # Docker环境下使用服务名

# 创建 FastAPI 应用实例
app = FastAPI()

# LED控制器连接（用于转发说话状态）
led_controller_connection: Optional[WebSocket] = None

# --- 2. 使用 Pydantic 定义请求体模型 ---
class ChatRequest(BaseModel):
    user_input: str
    user_id: str

# --- 3. Redis 驱动的 Agent 核心 (同步函数) ---
# 复用我们 12.1.2 节的 get_ai_response_with_redis 函数
def get_ai_response_with_redis(user_input: str, user_id: str) -> str:
    """
    接收用户输入和 user_id, 从 Redis 加载记忆, 计算后将更新后的记忆存回 Redis。
    """
    # 1. 创建 Redis 记忆对象
    # session_id 就是我们的 user_id, LangChain 会用它作为 Redis 的 Key
    history = RedisChatMessageHistory(
        session_id=user_id,
        url=REDIS_URL
    )
    
    # 2. 创建 Memory 实例, 并注入 Redis 历史
    memory = ConversationBufferMemory(
        memory_key="chat_history", 
        chat_memory=history
    )

    # 3. 创建 Chain (核心逻辑)
    # 注意: 我们不再手动传入 chat_history, 而是让 memory 对象自动处理
    tutor_chain = LLMChain(llm=llm, prompt=prompt, memory=memory)
    
    # 4. 调用 Chain, 让 Memory 自动更新 Redis
    # .invoke() 方法会自动: 加载历史 -> 运行 LLM -> 将新问答对存回 Redis
    response = tutor_chain.invoke({"question": user_input})
    
    return response["text"]

# --- 4. FastAPI 路由 (异步) ---
@app.websocket("/ws/{client_id}")
async def websocket_endpoint(websocket: WebSocket, client_id: str):
    global led_controller_connection  # global 声明必须在函数开头
    await websocket.accept()
    client_ip = websocket.client.host
    print(f"\n新的客户端连接: {client_ip} (ID: {client_id})")

    # 为每个连接维护一个独立的状态
    client_state = {
        "is_recording": False,
        "audio_buffer": bytearray()
    }

    try:
        while True:
            message = await websocket.receive()

            # --- 处理文本消息 (JSON 事件) ---
            if "text" in message:
                text = message["text"]
                
                # 处理 LED 控制信号 "1" 和 "0"
                if text == "1" or text == "0":
                    if led_controller_connection:
                        try:
                            await led_controller_connection.send_text(text)
                            print(f"[{client_ip}] 转发说话状态 '{text}' 到 LED 控制器")
                        except Exception as e:
                            print(f"转发到 LED 控制器失败: {e}")
                            led_controller_connection = None  # global 已在函数开头声明
                    continue
                
                data = json.loads(text)
                event = data.get("event")

                if event == "wake_word_detected":
                    print(f"[{client_ip}] 检测到唤醒词！")

                elif event == "recording_started":
                    print(f"[{client_ip}] 开始录音...")
                    client_state["is_recording"] = True
                    client_state["audio_buffer"].clear()

                elif event == "recording_ended":
                    print(f"[{client_ip}] 录音结束")
                    client_state["is_recording"] = False
                    
                    if not client_state["audio_buffer"]:
                        print("警告：音频缓冲区为空，不处理。")
                        continue

                    print(f"  - 音频总大小: {len(client_state['audio_buffer'])} 字节")
                    print("  - 开始 AI 处理流程...")

                    # 1. ASR
                    user_text = await asyncio.to_thread(transcribe_audio_stream, bytes(client_state['audio_buffer']))
                    if not user_text:
                        print("  - ASR 失败，对话中止。")
                        continue
                    print(f"  -  用户说 (ASR): '{user_text}'")

                    # 2. LLM
                    ai_response_text = await asyncio.to_thread(get_ai_response_with_redis, user_text, client_id)
                    print(f"  - AI 回复: '{ai_response_text}'")

                    # 3. TTS
                    response_audio_bytes = await asyncio.to_thread(synthesize_speech_stream, ai_response_text)
                    if not response_audio_bytes:
                        print("  -  TTS 失败，对话中止。")
                        continue
                    
                    # 短暂延时，确保文本先被处理
                    await asyncio.sleep(0.1)
                    
                    # 4. 【新策略】分块发送音频回 ESP32 (Burst and Yield)
                    print(f"  -  开始流式发送 {len(response_audio_bytes)} 字节的回复音频...")
                    CHUNK_SIZE = 1024  # 每次发送的数据块大小
                    BURST_SIZE = 8     # 定义一次“爆发”发送多少个数据块 (8 * 1024 = 8KB)
                    burst_count = 0

                    for i in range(0, len(response_audio_bytes), CHUNK_SIZE):
                        chunk = response_audio_bytes[i:i + CHUNK_SIZE]
                        
                        try:
                            await websocket.send_bytes(chunk)
                            burst_count += 1
                            
                            # 每发送 BURST_SIZE 个数据块后，就“谦让”一次
                            if burst_count >= BURST_SIZE:
                                burst_count = 0
                                # 使用极小的休眠来让出控制权，防止阻塞
                                await asyncio.sleep(0.001)

                        except Exception as e:
                            print(f"\n  在发送音频时客户端断开连接: {e}")
                            break

                    # 确保在 ESP32 客户端调用 finishStreamingPlayback()
                    # 我们可以发送一个特殊的JSON消息作为结束标志
                    try:
                        await websocket.send_text(json.dumps({"event": "response_finished"}))

                        # await websocket.close()
                        # break
                    except Exception:
                        pass # 如果此时客户端已断开，忽略错误

                    print(" 对话流程结束\n")

                elif event == "recording_cancelled":
                    print(f"  [{client_ip}] 录音取消")
                    client_state["is_recording"] = False
                    client_state["audio_buffer"].clear()

            # --- 处理二进制消息 (音频数据) ---
            elif "bytes" in message:
                if client_state["is_recording"]:
                    audio_chunk = message["bytes"]
                    client_state["audio_buffer"].extend(audio_chunk)
                    # 为了避免刷屏，可以注释掉下面这行
                    # print(f"  接收到音频数据块: {len(audio_chunk)} 字节 (总计: {len(client_state['audio_buffer'])})")

    except WebSocketDisconnect:
        print(f" [{client_ip}] 客户端断开连接")
    except Exception as e:
        print(f" [{client_ip}] 连接出现未知错误: {e}")
    finally:
        # 无论如何，确保连接被关闭（如果它仍然打开）
        # 检查状态以避免在已经关闭的连接上再次关闭
        if websocket.client_state != WebSocketState.DISCONNECTED:
            await websocket.close()
            print(f"[{client_ip}] 服务器端强制关闭连接。")
        # # 确保即使出错也关闭连接
        # if websocket.client_state != "DISCONNECTED":
        #     await websocket.close()

# --- 5. LED控制器 WebSocket 端点 ---
@app.websocket("/ws/led")
async def led_websocket_endpoint(websocket: WebSocket):
    """
    供组员的ESP32连接，接收说话状态控制LED
    """
    global led_controller_connection
    await websocket.accept()
    led_controller_connection = websocket
    print("🟢 LED控制器已连接")
    
    try:
        while True:
            # 保持连接，等待断开
            data = await websocket.receive_text()
            print(f"LED控制器消息: {data}")
    except WebSocketDisconnect:
        print("🔴 LED控制器断开连接")
    except Exception as e:
        print(f"LED控制器连接错误: {e}")
    finally:
        led_controller_connection = None

# --- 6. 运行服务器 ---
if __name__ == "__main__":
    import uvicorn
    # 官方示例默认使用 8888 端口
    port = 8000
    print("=" * 60)
    print("🎙️ 小智智能音箱服务器")
    print(f"🌐 监听: http://0.0.0.0:{port}")
    print(f"🔌 WebSocket: ws://<IP>:8888/ws/esp32")
    print(f"💡 LED端点: ws://<IP>:8888/ws/led")
    print("=" * 60)
    
    uvicorn.run(app, host="0.0.0.0", port=port)

