# 10.4.2 节代码：app.py
import os
from flask import Flask, request, jsonify, Response
from langchain_community.llms import QianfanLLMEndpoint
from langchain.prompts import PromptTemplate
from langchain.chains import LLMChain
import time
import json
import os
from aip import AipSpeech # 导入百度语音 API SDK
import base64 # 导入 Base64 库

from dotenv import load_dotenv
load_dotenv()

# --- 语音 API 客户端初始化 ---
APP_ID = os.getenv("BAIDU_VOICE_APP_ID")
API_KEY = os.getenv("BAIDU_VOICE_API_KEY")
SECRET_KEY = os.getenv("BAIDU_VOICE_SECRET_KEY")
# 初始化百度语音 API 客户端
try:
    speech_client = AipSpeech(APP_ID, API_KEY, SECRET_KEY)
except Exception as e:
    print(f"Warning: Baidu AipSpeech Client initialization failed. Check environment variables. Error: {e}")
# -----------------------------

def transcribe_audio_stream(audio_bytes: bytes) -> str:
    """
    接收音频二进制流，调用百度 STT (ASR) API 进行转录。
    """
    # dev_pid=1737 是百度语音识别的英语识别模型 ID
    stt_result = speech_client.asr(audio_bytes, 'wav', 16000, {'dev_pid': 1737})
    
    if stt_result and stt_result.get('err_no') == 0 and stt_result.get('result'):
        # 返回转录结果中的第一个句子
        return stt_result['result'][0]
    else:
        # 如果 ASR 失败，返回一个空字符串或错误信息
        print(f"! STT (ASR) 错误: {stt_result}")
        return ""

def synthesize_speech_stream(text: str) -> bytes:
    """
    接收文本，调用百度 TTS API 合成语音，返回音频二进制流。
    """
    # per=4 (女声), spd=5 (语速), pit=5 (语调)
    tts_result = speech_client.synthesis(text, 'zh', 1, {'vol': 5, 'per': 4, 'spd': 5, 'pit': 5})

    # 百度 API 在成功时返回 bytes，失败时返回一个包含错误信息的字典
    if not isinstance(tts_result, dict):
        return tts_result # 成功，返回音频二进制数据
    else:
        print(f"! TTS (文本转语音) 错误: {tts_result}")
        return b"" # 失败，返回空 bytes

# --- 第1部分：导入与配置 ---
# 初始化我们的 Flask 应用
app = Flask(__name__)
app.config['JSON_AS_ASCII'] = False
# 从环境变量加载百度 API 密钥 (假设已在终端中通过 source ~/.bashrc 设置)
os.environ["QIANFAN_ACCESS_KEY"] = os.getenv("QIANFAN_ACCESS_KEY")
os.environ["QIANFAN_SECRET_KEY"] = os.getenv("QIANFAN_SECRET_KEY")

# --- 第2部分：创建并配置 AI 智能体 (无状态核心) ---
# 这是一个至关重要的优化：服务器启动时只创建一次 Agent
llm = QianfanLLMEndpoint()

# 使用我们在第八章设计的 Alex 导师人设 Prompt
template = """
你是一个名叫 Alex 的友好、耐心且鼓励人心的英语辅导老师。
你的目标是帮助一位非母语者练习英语会话。
总是提出一个后续问题来让对话继续下去。
保持你的回答简洁，通常一到两句话。
如果用户犯了语法错误，你必须温和地纠正它。首先，提供修正后的版本，然后，另起一段，用一个简单的单句解释该语法规则。

当前对话:
{chat_history}
人类: {question}
AI:
"""
prompt = PromptTemplate(input_variables=["chat_history", "question"], template=template)

# 将 Chain 核心封装在一个函数中，使其可以被路由调用
def get_ai_response(user_input: str, current_history: str) -> str:
    """
    接收用户输入和完整的对话历史，返回 AI 回复。
    注意：这里我们使用 LLMChain 的 invoke 方法，手动传入 chat_history。
    """
    # 这里的 LLMChain 实例是临时的，不存储记忆
    # 关键：我们没有传入 memory 参数，使其保持无状态
    tutor_chain = LLMChain(llm=llm, prompt=prompt)
    
    # 传入所有需要的变量
    # user_input 对应 {question}，current_history 对应 {chat_history}
    response = tutor_chain.invoke({
        "chat_history": current_history,
        "question": user_input
    })
    
    return response["text"]

print("--- AI 辅导链核心已初始化为无状态函数 ---")

# --- 第3部分：定义 API 端点 ---

@app.route("/api/text_chat", methods=["POST"])
def text_chat_endpoint():
    """
    处理来自客户端的文本请求，接收对话历史，返回更新后的历史和 AI 回复。
    """
    print("--- /api/text_chat 收到一个新请求 ---")

    # 步骤1：接收并解析客户端发送的 JSON 数据
    data = request.get_json()
    
    # 专业的接口校验：确保关键字段存在
    if not data or 'user_input' not in data or 'chat_history' not in data:
        # HTTP 状态码 400 表示客户端请求有误
        return jsonify({"error": "Missing required fields (user_input or chat_history) in request body"}), 400

    # 接收用户输入和历史
    user_text = data['user_input']
    current_history = data['chat_history'] 
    
    # 预留接口：接收 user_id，如果缺失则使用默认 ID
    user_id = data.get('user_id', 'default_user') 
    
    print(f"用户ID: {user_id}, 历史长度: {len(current_history)} 字符, 文本: {user_text[:20]}...")

    # 步骤2：获取 AI 回复 (使用无状态函数)
    start_time = time.time()
    ai_response_text = get_ai_response(user_text, current_history)
    end_time = time.time()
    print(f"LLM 思考耗时: {end_time - start_time:.2f} 秒")


    # 步骤3：更新对话历史
    # 服务器手动将新的对话回合添加到历史中，准备返回给客户端
    # 客户端收到这个新历史后，会在下一次请求时原封不动地传回来
    new_history = f"{current_history}\nHuman: {user_text}\nAI: {ai_response_text}"

    print(f"AI 回复: '{ai_response_text[:30]}...'")

    # 步骤4：返回 AI 回复和更新后的历史 (JSON 格式)
    # 4.1 准备要返回的数据字典
    response_data = {
        "status": "success",
        "ai_response": ai_response_text,
        "new_chat_history": new_history,
        "user_id": user_id 
    }
    # 4.2 手动将字典转换为 JSON 字符串，关键：ensure_ascii=False
    # 这会强制 json.dumps 生成真正的中文字符。
    json_string = json.dumps(response_data, ensure_ascii=False)

    # 4.3 创建一个 Flask 的 Response 对象
    # 我们将字符串、MIME 类型和字符集完全手动指定
    response = Response(
        json_string,
        mimetype='application/json; charset=utf-8'
    )
    
    # 4.4 返回响应对象
    return response

# --- 定义 API 端点 ---

@app.route("/api/voice_chat", methods=["POST"])
def voice_chat_endpoint():
    """
    处理来自客户端的语音请求，接收音频流，返回回复音频流和 JSON 元数据。
    """
    print("--- /api/voice_chat 收到一个新请求 ---")

    # 步骤1：接收并解析客户端发送的数据
    # request.data 接收原始音频二进制流（来自 ESP32 的录音）
    audio_bytes = request.data 
    
    # request.form 接收附带的文本数据，用于携带 user_id 和 chat_history
    # 架构意图：这里预留了多用户接口
    user_id = request.form.get('user_id', 'default_user')
    chat_history = request.form.get('chat_history', '')
    
    if not audio_bytes:
        return jsonify({"error": "Missing audio data in request body"}), 400

    print(f"用户ID: {user_id}, 历史长度: {len(chat_history)} 字符, 音频大小: {len(audio_bytes)} bytes")


    # 步骤2：将音频转录为文本 (ASR)
    user_text = transcribe_audio_stream(audio_bytes)
    
    if not user_text:
        return jsonify({"error": "Could not understand audio or ASR error occurred"}), 500

    print(f"用户说 (ASR): '{user_text}'")

    # 步骤3：从 LangChain 智能体获取 AI 的回应 (使用无状态核心)
    # 假设 get_ai_response 函数已在全局定义
    ai_response_text = get_ai_response(user_text, chat_history)
    
    # 步骤4：更新对话历史
    new_history = f"{chat_history}\nHuman: {user_text}\nAI: {ai_response_text}"
    
    print(f"AI 回复: '{ai_response_text[:30]}...'")

    # 步骤5：将 AI 的文本回应合成为音频 (TTS)
    response_audio_bytes = synthesize_speech_stream(ai_response_text)

    if not response_audio_bytes:
        return jsonify({"error": "TTS synthesis failed"}), 500

    # 步骤6：将音频数据和元数据发送回客户端
    # 关键：使用 Base64 编码将二进制音频数据转换为文本字符串，以便封装在 JSON 中
    encoded_audio = base64.b64encode(response_audio_bytes).decode('utf-8')

    print("--- 正在将 Base64 编码的音频回应发送回客户端 ---")
    return jsonify({
        "status": "success",
        "ai_response": ai_response_text,
        "new_chat_history": new_history,
        "audio_data_base64": encoded_audio, # 客户端需要解码后播放
        "mimetype": "audio/mpeg", # 告知客户端音频类型
        "user_id": user_id
    })



# --- 第4部分：运行服务器 ---

if __name__ == "__main__":
    print("--- 正在启动 Flask 服务器 ---")
    # host='0.0.0.0' 使其可以从你网络上的其他设备（如 ESP32）访问
    # debug=True 允许在开发时自动重启服务器
    app.run(host='0.0.0.0', port=5001, debug=True)