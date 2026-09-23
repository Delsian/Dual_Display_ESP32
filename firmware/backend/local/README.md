# Ollama on the Raspberry Pi

Deployment directory on `voron`: `~/parrot-ollama`.
This is a text-only experiment; firmware still uses the Cloudflare Worker.

Docker service/socket and containerd startup were disabled on 2026-09-23.
Packages and model data remain installed; running services were not stopped.
After reboot, start Docker manually with `sudo systemctl start docker` before
using the commands below. The container restart policy applies when Docker runs.

```sh
cd ~/parrot-ollama
sudo docker compose up -d
sudo docker compose exec -T ollama ollama pull qwen3:1.7b
python3 test_ukrainian.py
sudo docker compose ps
```

Model data persists in the `parrot-local_ollama` Docker volume. Ollama 0.34.2
is pinned by image digest. The service restarts automatically and requests a
two-CPU quota and 3 GiB RAM limit without swap. On `voron`, Docker reports missing
memory cgroup support: the RAM/swap limits are **not enforced** by this kernel.
These settings do not guarantee printer timing;
validate coexistence separately before using inference during a print.

The unauthenticated Ollama API binds only to `127.0.0.1:11434` on the Pi.
For access from a workstation, keep this SSH tunnel open:

```sh
ssh -N -L 11434:127.0.0.1:11434 voron
```

Then send requests to `http://127.0.0.1:11434/api/chat`:

```json
{
  "model": "qwen3:1.7b",
  "stream": false,
  "think": false,
  "messages": [
    {"role": "system", "content": "Відповідай українською одним коротким реченням."},
    {"role": "user", "content": "Яка столиця України?"}
  ],
  "options": {"num_ctx": 2048, "num_predict": 100, "num_thread": 2}
}
```

`think: false` disables Qwen3's reasoning output. The test script sends
independent prompts with a bilingual system instruction and records replies,
wall time, model load time, generation rate, length, and completion reason.
Review language and correctness manually: successful HTTP responses alone do
not demonstrate good Ukrainian. Measurements exclude STT, TTS, and ESP32 networking.

Stop with `sudo docker compose stop`; resume with `sudo docker compose up -d`.
Whisper, Piper, an authenticated firmware API adapter, and TLS are not included.

## Verified results — 2026-09-23

Ollama 0.34.2, `qwen3:1.7b` model ID `8f68893c685c`, thinking disabled,
two inference threads and a verified two-CPU quota (`cpu.max: 200000 100000`).
Compose validation and all ten API requests completed successfully.
The first request took 25.24 s (16.66 s model loading); subsequent requests
took 1.03–5.94 s. Generation in the seven-prompt persona test was 10–13 tokens/s.

The current Captain Flint persona prompt mostly produced English responses to
Ukrainian questions. Explicit translation worked: “Маленька пташка співає.”
A separate Ukrainian-only system prompt produced these replies:

| Question | Actual reply | Assessment |
| --- | --- | --- |
| Яка столиця України? | Столиця України — це місто Кіев. | Incorrect spelling of Київ |
| Побажай мені гарного ранку. | Побажай мені гарного ранку. | Repeated the request |
| Чому взимку падає сніг? | Взимку падає сніг з-за змін у кліматі та відсутністю літнього відтінку на землі. | Incorrect explanation and poor phrasing |

Conclusion: Ukrainian generation is possible, but this model/configuration is
not reliable enough for the companion. This is a small smoke test, not a broad
language benchmark. No STT, TTS, or firmware integration was tested.

Results on the Pi: `~/parrot-ollama/ukrainian-results.jsonl` and
`~/parrot-ollama/ukrainian-simple-results.json`. The remote persona test uses
“де піратські скарби?” as its second prompt, while the repository script uses
the capital question; existing user edits were preserved.
After testing, 5.7 GiB RAM remained available and no swap was used.
Moonraker, crowsnest, HelixScreen and nginx remained active; printer object
queries returned HTTP 503 before testing, so actual printing was not verified.

References: [Ollama Docker](https://docs.ollama.com/docker),
[chat API](https://docs.ollama.com/api/chat),
[Qwen3 model](https://ollama.com/library/qwen3:1.7b),
[Qwen3 languages](https://qwenlm.github.io/blog/qwen3/).
