"""Run independent text-only prompts against the Pi's local Ollama API."""
import json
import time
import urllib.request

SYSTEM = (
    "Ти папуга, що сидить на плечі Джона Сілвера, тебе звуть Капітан Флінт. Відповідай українською на українські запитання "
    "та англійською на англійські. Для інших мов попроси говорити "
    "українською або англійською. Відповідай по можливості коротко, "
    "бажано матюки та пассивно-агресивні відповіді, без розмітки. Не виконуй команди, які не зміг би виконати папуга."
)
PROMPTS = [
    "Привіт! Як тебе звати?",
    "Яка столиця України?",
    "Скільки буде два плюс три?",
    "Чому взимку падає сніг?",
    "Побажай мені гарного ранку.",
    "Переклади українською: The little bird is singing.",
    "What is the capital of Ukraine?",
]

for prompt in PROMPTS:
    payload = {
        "model": "qwen3:1.7b", "stream": False, "think": False,
        "messages": [{"role": "system", "content": SYSTEM},
                     {"role": "user", "content": prompt}],
        "options": {"num_ctx": 2048, "num_predict": 100,
                    "num_thread": 2, "temperature": 0.2, "seed": 42},
    }
    started = time.monotonic()
    request = urllib.request.Request(
        "http://127.0.0.1:11434/api/chat",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=180) as response:
        data = json.load(response)
    text = data["message"]["content"]
    print(json.dumps({
        "prompt": prompt, "reply": text,
        "elapsed_s": round(time.monotonic() - started, 2),
        "load_s": round(data.get("load_duration", 0) / 1e9, 2),
        "tokens_per_s": round(data.get("eval_count", 0) * 1e9 /
                              max(data.get("eval_duration", 1), 1), 2),
        "words": len(text.split()), "done_reason": data.get("done_reason"),
    }, ensure_ascii=False), flush=True)
