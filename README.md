# Sonny — a native C++ voice and vision desktop assistant

Sonny is a Windows desktop assistant that runs as a **single process**.It is high-performance, production-ready cross platform GPU agnostic natively programmed in C++ with in-process implementation and privacy-safe embeddings vectors & LoRA adapters synchronization,with faster whisper/playwright libraries reimplemented into cpp. It listens for you, transcribes speech with a local Whisper model, reasons with a local LLM through `llama.cpp`, answers with a local neural voice, and acts — browsing the web by voice, controlling your machine, playing media, and looking through your camera.

Everything runs **in-process and on your machine**: speech recognition, text-to-speech,
the language model, the vector memory, the embedding model, the 3D avatar, and the browser
automation. There is no Python runtime, no server process, and no cloud service in the
running product. The only file Sonny ever fetches is the LLM model you provide yourself.

<table>
  <tr>
    <td align="center"><img src="resources/images/sonny-screenshot.png" alt="Sonny Screenshot" width="480"></td>
    <td align="center"><img src="resources/images/iron-man-screenshot.png" alt="Iron Man Screenshot" width="480"></td>
  </tr>
  <tr>
    <td align="center"><em>Sonny</em></td>
    <td align="center"><em>Iron Man</em></td>
  </tr>
</table>

---

## Table of Contents

- [What Sonny Is](#what-sonny-is)
- [Feature Overview](#feature-overview)
- [Architecture](#architecture)
- [The Voice Pipeline](#the-voice-pipeline)
- [The Tool System](#the-tool-system)
- [Browser Automation](#browser-automation)
- [Vision](#vision)
- [Memory and RAG](#memory-and-rag)
- [The Vectors Network](#the-vectors-network)
- [Interface](#interface)
- [Project Structure](#project-structure)
- [Technical Specifications](#technical-specifications)
- [System Requirements](#system-requirements)
- [Building](#building)
- [Configuration](#configuration)
- [Running](#running)
- [Settings Dialog](#settings-dialog)
- [Where Sonny Keeps Data](#where-sonny-keeps-data)
- [Troubleshooting](#troubleshooting)
- [Dependencies and Licenses](#dependencies-and-licenses)

---

## What Sonny Is

The voice loop is a real, closed loop running in one thread chain:

```
microphone → wake word → record 4 s → Whisper (CTranslate2) → LLM (llama.cpp)
           → optional tool calls → Kokoro TTS → speakers
```

Wake-word detection uses **openWakeWord** ONNX models, transcription uses a
**CTranslate2 Whisper** model, speech synthesis uses **Kokoro** linked in as a static
library, and the brain is a GGUF model run through **llama.cpp** with CUDA or Vulkan GPU
offload. Sonny's retrieval memory is a hybrid BM25 + dense-cosine index built on an
**ONNX sentence-embedding model** that it embeds in-process through ONNX Runtime.

On top of that, Sonny can:

- **Drive a real Chromium browser** over the Chrome DevTools Protocol, implemented from
  scratch in C++ (own RFC 6455 WebSocket client, own CDP session, own injected page
  runtime). It navigates, reads, clicks, fills forms, autofills from an encrypted profile,
  and takes screenshots and PDFs.
- **Look through your camera** and answer questions about what it sees, routing captured
  frames to a multimodal GGUF model through `mtmd`.
- **Act on 16 registered tools** covering search, files, system control, media playback,
  clipboard, and the camera.
- **Share only content-free aggregates** with other Sonny instances on a LAN or through a
  relay — tool success/latency counters, corpus scale, and (optionally) differential-
  privacy-noised embedding vectors and LoRA adapters. No conversations, queries, file
  names, or personal data ever leave the machine.

---

## Feature Overview

### Speech input

- **Wake word** — openWakeWord ONNX classifier (`sonny.onnx` + `melspectrogram.onnx` +
  `embedding_model.onnx`), score threshold 0.5, streaming over a 10 s rolling buffer.
- **Microphone capture** — WASAPI, shared mode, 16 kHz mono, 16-bit, with
  `AUTOCONVERTPCM` so Windows handles device resampling. 50 ms period, four buffers.
- **Speech-to-text** — CTranslate2 Whisper (`whisper-base-ct2` by default), beam size 5,
  English, with the full faster-whisper temperature fallback chain
  (`0, 0.2, 0.4, 0.6, 0.8, 1.0`), `compression_ratio ≤ 2.4`, `avg_logprob ≥ -1.0`,
  `no_speech ≤ 0.6`.
- **Voice activity detection** — Silero V5 ONNX (`silero_vad.onnx`), 512-sample (32 ms)
  stateful frames. Used to segment utterances when the wake word is disabled.
- **Echo suppression** — while the assistant is speaking, a detected wake word still plays
  the confirmation tone and resets the model but does not start a recording.
- **Microphone gain** — configurable input multiplier (1.0 = unity, 5.0 ≈ +14 dB).

### Brain

- **LLM** — llama.cpp, statically linked, with automatic CUDA / Vulkan / CPU backend
  selection based on the primary display adapter.
- **Context management** — 4096-token context, 512-token prefill micro-batch, and a
  byte-stable system prompt so the KV prefix cache is reused instead of re-prefilled on
  every turn.
- **Model pre-flight** — before the slow model load, the GGUF header is parsed and its
  `general.architecture` is checked against the architectures the linked llama.cpp
  actually implements (the list is generated at configure time from
  `external/llama.cpp/src/llama-arch.cpp`). An unsupported model produces a clear
  explanation and a prompt to pick another file, not a bare "failed to load".
- **LoRA** — local adapter loading/scaling, plus peer adapters received over the vectors.
- **Sampling** — temperature sampling with a repetition penalty (1.1 over the last 64
  tokens) and a greedy temperature floor; `n_gpu_layers = 99` to offload the whole model.

### Speech output

- **Kokoro TTS** — the `kokoro` static library is linked directly into the executable
  (no `kokoro-server.exe` subprocess and no HTTP hop). It loads the encoder, HAR
  generator and decoder ONNX sessions, the voice pack, eSpeak NG data and Misaki data
  from a single model directory, and outputs 24 kHz audio.
- **Voice selection** — every `.npy` in the voice pack is offered (the default is
  `af_heart`); only `en-US` vs `en-GB` changes synthesis.
- **Speed** — configurable multiplier, default 1.15.
- **Browser speech fallback** — an embedded HTTP server can serve a Web Speech API page
  and use Chrome for recognition and/or synthesis instead.

### Vision

- **Camera capture** — DirectShow, RGB frames from a selected or default device.
- **Screen capture** — the primary display can be grabbed as a fallback.
- **Multimodal routing** — when a `mmproj-*.gguf` projector sits next to the model,
  `mtmd` is initialised and image-bearing turns route through
  `generate_stream_with_images()` instead of the text-only path.
- **Photo and video capture** — a `camera` tool can take a photo, start/stop a recording,
  and open the result.

### Interface

- **3D avatar** — DirectX 11 overlay window that loads a GLB model with skeletal
  animation, skinning, and alpha blending, positioned over the desktop.
- **Video overlay** — Media Foundation playback of the startup WMVs with chroma key and
  `UpdateLayeredWindow` compositing.
- **CAVA visualizer** — a terminal audio visualiser of your own speech spectrum.
- **System tray** — background operation with a context menu (Settings, Mesh, Toggle
  console, Exit). The console is detached and re-created rather than merely hidden, and
  its buffered output is replayed when it comes back.
- **Mesh dialog** — a WebView2 window rendering the live peer mesh (vis-network) fed by
  `AssistantOrchestrator::mesh_json()`.

---

## Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  personal_assistant.exe  (single process, /SUBSYSTEM:CONSOLE)                │
│                                                                              │
│  main.cpp                                                                       │
│   ├── ConfigManager (%APPDATA%\Sonny\config.ini)                              │
│   ├── FirstRunWizard / SettingsDialog (5 tabs)                                │
│   ├── SystemTray + MeshDialog (WebView2)                                      │
│   └── ToolRegistry — 16 tools registered                                       │
│                                    │                                          │
│                                    ▼                                          │
│  ┌──────────────────────────────────────────────────────────────────────────┐│
│  │  AssistantOrchestrator                                                    ││
│  │   listening state · tool-call loop · vision routing · TTS playback        ││
│  └───┬────────┬────────┬─────────┬──────────┬──────────┬─────────────┬──────┘│
│      │        │        │         │          │          │             │       │
│      ▼        ▼        ▼         ▼          ▼          ▼             ▼       │
│  Audio     STTEngine WakeWord  Llama      Kokoro    RagEngine    VectorsSync │
│ Capture   (CTranslate2+VAD)          (llama.cpp) (ONNX embed)   (src/net)  │
│  (WASAPI)                                                            │        │
│      │                                                                │        │
│      │        ┌───────────────┐                                       │        │
│      ├───────▶│ WebSpeech     │    ┌──────────┐  ┌──────────────┐    │        │
│      │        │ Wrapper (HTTP)│    │ Avatar   │  │ VideoPlayer  │    │        │
│      │        │ (Chrome STT/  │    │ (D3D11)  │  │ (Media Found.)│   │        │
│      │        │  TTS fallback)│    └──────────┘  └──────────────┘    │        │
│      │        └───────────────┘  ┌──────────┐  ┌──────────────┐     │        │
│      └──────────────────────────▶│ Camera   │  │ CavaVisualiz.│     │        │
│                                 │(DirectSh.)│  └──────────────┘     │        │
│                                 └──────────┘                         │        │
│                                                                          ▼      │
│  ┌──────────────────────────────────────────────────────────────────────────┐│
│  │  BrowserTool → BrowserServiceImpl → ChromiumLauncher (process)            ││
│  │                                  → ChromiumCdpSession → BrowserWebSocket ││
│  │                                  → BrowserPageScripts / SiteFlows         ││
│  │                                  → UserProfileStore / BrowserCrypto       ││
│  └──────────────────────────────────────────────────────────────────────────┘│
└──────────────────────────────────────────────────────────────────────────────┘
```

### Component reference

| Component | File | Responsibility |
|---|---|---|
| Entry point | `src/core/main.cpp` | Config load, resource-base resolution, tool registration, subsystem init, tray message loop, self-restart |
| Orchestrator | `src/core/AssistantOrchestrator.{h,cpp}` | Central coordinator: init order, listening state, transcription handling, LLM generation, tool-call loop, vision routing, TTS playback, peer-vector wiring |
| Config | `src/core/ConfigManager.{h,cpp}` | Reads/writes `config.ini` under `%APPDATA%\Sonny` |
| Settings | `src/core/SettingsDialog.{h,cpp}`, `FirstRunWizard.{h,cpp}` | Win32 tabbed settings UI and the first-run model picker |
| Tray / Mesh | `src/core/SystemTray.{h,cpp}`, `MeshDialog.{h,cpp}` | Tray icon and menu; WebView2 mesh visualisation |
| Camera | `src/core/CameraCapture.{h,cpp}`, `VideoRecorder.{h,cpp}` | DirectShow capture, photo and recording management |
| LLM | `src/ai/LlamaWrapper.{h,cpp}` | llama.cpp model/context/samplers, streaming, tool-loop, mtmd vision, LoRA, GGUF pre-flight |
| TTS | `src/ai/KokoroWrapper.{h,cpp}` | In-process Kokoro engine wrapper |
| RAG | `src/ai/RagEngine.{h,cpp}` | Document index, user knowledge, conversation memory, hybrid retrieval |
| Embeddings | `src/ai/EmbeddingEngine.{h,cpp}` | ONNX Runtime sentence embedder + WordPiece tokenizer |
| STT | `src/audio/STTEngineWrapper.{h,cpp}` | CTranslate2 Whisper recognizer and the wake-word/VAD listening loop |
| Wake word | `src/audio/WakeWordEngine.{h,cpp}` | openWakeWord ONNX pipeline |
| VAD | `src/audio/VADEngine.{h,cpp}` | Silero V5 streaming segmentation |
| Audio | `src/audio/AudioCapture.{h,cpp}`, `AudioPlayback.{h,cpp}` | WASAPI capture, waveOut playback |
| Web speech | `src/audio/WebSpeechWrapper.{h,cpp}` | Embedded HTTP server for Chrome-based STT/TTS |
| Browser | `src/browser/*` | Chromium launch, CDP session, WebSocket, page runtime, autofill vault |
| Peer network | `src/net/*` | Wire types, crypto, LAN/WAN transport, sync brain |
| UI | `src/ui/AvatarOverlay.{h,cpp}`, `VideoPlayer.{h,cpp}`, `CavaVisualizer.{h,cpp}` | 3D avatar, chroma-keyed video, terminal visualiser |
| Tools | `src/tools/*` | 16 registered tools (see below) |
| Utilities | `src/utils/Tool.{h,cpp}`, `Logger.h`, `PerfTimer.{h,cpp}`, `Trie.{h,cpp}`, `PathUtil.h`, `ToolUtils.h`, `ZlibCompress.h` | Tool base + registry, logging, timing, prefix matching |

---

## The Voice Pipeline

### One turn

```
USER SPEAKS
    │  16 kHz mono PCM (WASAPI)
    ▼
AudioCapture  ──►  STTEngineWrapper
                    │
                    ├─ 512-sample frames
                    ├─ openWakeWord: melspectrogram → embedding → wake classifier
                    │      score > 0.5  ──► play sonny_yes.wav, start 4 s recording
                    ├─ (wake word off) Silero VAD RealTimeCutVAD segments the utterance
                    └─ 4 s segment queued to the transcription thread
                              │
                              ▼
                    CTranslate2 Whisper (beam 5, language=en, temperature fallback)
                              │ text
                              ▼
                    is_vision_request(text)?
                       yes ─► CameraCapture / screen grab → RGB image
                              │
                              ▼
                    AssistantOrchestrator::generate_response
                       ├── refresh_llama_system_prompt()  (persona + tool schemas +
                       │                                  file-explorer + browser/camera docs)
                       ├── RagEngine::getContextForLLM()  (hybrid retrieval, ≤1500 chars)
                       └── LlamaWrapper::generate_stream[_with_images]()
                              │
                              ▼
                    parse_llm_tool_call(response)
                       hit  ─► ToolRegistry::executeTool(name, params)
                                 │ ToolResult
                                 └──► fed back as the next turn (capped, see below)
                              │
                              ▼
                    KokoroWrapper::synthesize → AudioPlayback (waveOut, 24 kHz)
```

### Tool-call loop

Tool decisions are made **only by the LLM**. There is no keyword or regex pre-routing:
the model receives every registered tool's JSON schema in its system prompt and decides
on its own. The orchestrator parses a tool call from the response, executes it, appends
the result, and calls the LLM again.

Two JSON shapes are accepted:

```json
{"tool": "tool_name", "params": {"key": "value"}}
{"tool_name": {"key": "value"}}
```

A follow-up chain is capped at `kMaxToolFollowups = 2` so a failing tool cannot produce an
unbounded retry loop; each fresh user utterance resets the counter. A successful final
response is spoken exactly once.

### Utterance segmentation

When the wake word is enabled, Sonny records a fixed 4 seconds (64 000 samples) after the
wake detection — a deliberate parity choice with the reference listening loop, and the
reason transcriptions are stable rather than cut short. When the wake word is disabled,
Silero VAD's `RealTimeCutVAD` parameters apply: start after 4 frames (~128 ms) with a
40 % true ratio above probability 0.35, end after 16 frames (~512 ms) with a 70 % false
ratio, a ~1.2 s pre-speech buffer, and segments clamped to 0.3–30 s.

---

## The Tool System

Seventeen tools are registered in `main.cpp`:

```cpp
registry.registerTool("browser",             std::make_unique<Jarvis::BrowserTool>());
registry.registerTool("google_search",       std::make_unique<Jarvis::GoogleSearchTool>());
registry.registerTool("web_search",          std::make_unique<Jarvis::WebSearchTool>());
registry.registerTool("youtube_search",     std::make_unique<Jarvis::YouTubeSearchTool>());
registry.registerTool("youtube_video",      std::make_unique<Jarvis::YouTubeVideoTool>());
registry.registerTool("system_command",      std::make_unique<Jarvis::SystemCommandTool>());
registry.registerTool("calculator",         std::make_unique<Jarvis::CalculatorTool>());
registry.registerTool("get_time",           std::make_unique<Jarvis::TimeTool>());
registry.registerTool("clipboard",          std::make_unique<Jarvis::ClipboardTool>());
registry.registerTool("file_operation",     std::make_unique<Jarvis::FileTool>());
registry.registerTool("file_explorer",      std::make_unique<Jarvis::FileExplorerTool>());
registry.registerTool("music_player",       std::make_unique<Jarvis::MusicPlayerTool>());
registry.registerTool("add_media_directory",    std::make_unique<Jarvis::AddMediaDirectoryTool>());
registry.registerTool("remove_media_directory", std::make_unique<Jarvis::RemoveMediaDirectoryTool>());
registry.registerTool("list_media_library",     std::make_unique<Jarvis::ListMediaLibraryTool>());
registry.registerTool("camera",             std::make_unique<Jarvis::CameraCaptureTool>());
```

| Tool | Class | Source | Parameters | What it does |
|---|---|---|---|---|
| `browser` | `BrowserTool` | `src/browser/` | `action`, `url`, `index`, `query`, `selector`, `text`, `direction`, `key`, `wait_ms` | Drives the Chromium session (see [Browser Automation](#browser-automation)) |
| `google_search` | `GoogleSearchTool` | `src/tools/search/` | `query`, `open_first` | Google search, scraped results; optionally opens the best result in the session |
| `web_search` | `WebSearchTool` | `src/tools/search/` | `query` | General web search for current information |
| `youtube_search` | `YouTubeSearchTool` | `src/tools/search/` | `query`, `open_first` | YouTube search; by default opens and plays the first result |
| `youtube_video` | `YouTubeVideoTool` | `src/tools/search/` | `query`, `index` | Opens the best-matching (or Nth) YouTube result and starts playback |
| `system_command` | `SystemCommandTool` | `src/tools/system/` | `command` | Runs a system command |
| `calculator` | `CalculatorTool` | `src/tools/information/` | `expression` | Evaluates a mathematical expression |
| `get_time` | `TimeTool` | `src/tools/information/` | `format` (`full`/`date`/`time`/`day`) | Current date and time |
| `clipboard` | `ClipboardTool` | `src/tools/file/` | `action` (`get`/`set`), `text` | Reads or writes clipboard text |
| `file_operation` | `FileTool` | `src/tools/file/` | `action` (`read`/`write`/`list`/`delete`), `path`, `content` | File operations |
| `file_explorer` | `FileExplorerTool` | `src/tools/file/` | `action` (`list`/`open`/`run`/`back`/`forward`/`suggest`), `path` | Smart path resolution, fuzzy matching, natural-language paths |
| `music_player` | `MusicPlayerTool` | `src/tools/media/` | `action` (`play`/`pause`/`resume`/`stop`/`next`/`previous`/`shuffle`/`queue`/`status`/`volume`/`seek`/`list_dirs`), `search_term`, `path`, `volume`, `seek_position` | Plays media from the registered libraries |
| `add_media_directory` | `AddMediaDirectoryTool` | `src/tools/media/` | `path`, `type` (`music`/`video`/`media`) | Registers a folder as a media library |
| `remove_media_directory` | `RemoveMediaDirectoryTool` | `src/tools/media/` | `path` | Removes a registered library |
| `list_media_library` | `ListMediaLibraryTool` | `src/tools/media/` | `action` (`libraries`/`contents`/`search`), `path`, `search_term` | Browses the media libraries |
| `camera` | `CameraCaptureTool` | `src/tools/media/` | `action` (`photo`/`start_recording`/`stop_recording`/`open_last`/`open_folder`), `kind` (`videos`/`photos`) | Photo, recording, and opening the result |

The registry exposes each tool's description and parameters to the LLM as a JSON schema
via `ToolRegistry::getToolSchemas()`, and the orchestrator embeds that schema in the
system prompt. Tool executions are timed and their outcome (name, success, duration —
never arguments or results) is recorded for the vectors network.

`src/tools/` also contains tool classes that exist but are **not** registered by
`main.cpp`: `ProcessTool`, `ScreenControlTool`, `BatteryTool`, `WifiTool`,
`VolumeControlTool`, `WeatherTool`, `NewsTool`, `UnitConverterTool`, `NoteTool`,
`JokeTool`, `SpellingTool`, `StopwatchTool`, `ReminderTool`, `AppLauncherTool`. They are
available to add to the registry if you want them.

---

## Browser Automation

`BrowserTool` gives Sonny a long-lived Chromium it can drive by voice. The whole stack is
**pure C++ over the Chrome DevTools Protocol** — the WebSocket client, the CDP session,
the page runtime and the crypto are all implemented in this repository. There is no
Node.js, no Playwright, and no Python in the running process.

### Files

| File | Responsibility |
|---|---|
| `BrowserTool.{h,cpp}` | The LLM-facing tool: action dispatch, parameter normalisation, natural-language fallbacks |
| `BrowserServiceImpl.{h,cpp}` | CDP orchestration: tab management, navigation, reading, interaction, capture |
| `ChromiumLauncher.{h,cpp}` | Finds Chrome/Edge, launches the process with a dedicated profile, reads the DevTools endpoint, re-attaches to a running instance |
| `ChromiumCdpSession.{h,cpp}` | One CDP WebSocket session — request/response calls and event subscription |
| `BrowserWebSocket.{h,cpp}` | RFC 6455 WebSocket client (the CDP transport) |
| `BrowserPageScripts.{h,cpp}` | The JavaScript runtime injected into pages: locators, forms, extraction |
| `SiteFlows.{h,cpp}` | Site-specific recipes (YouTube, Google) |
| `UserProfileStore.{h,cpp}` | Autofill values and DPAPI-encrypted credentials |
| `BrowserCrypto.{h,cpp}` | SHA-1, Base64, DPAPI blobs |
| `BrowserUtil.h` | `%APPDATA%` resolution, Winsock setup, shared helpers |

### Actions

`open`, `navigate`, `search`, `open_result`, `new_tab`, `close_tab`, `tabs`,
`switch_tab`, `read`, `find`, `summarize`, `eval`, `click`, `fill`, `scroll`, `key`,
`actions`, `screenshot`, `pdf`, `wait`, `back`, `forward`, `reload`, `fields`,
`autofill`, `hover`, `select`, `text`, `links`, `buttons`

### Profiles and sessions

Sonny drives a **dedicated** Chromium profile, not your everyday Chrome:

- Chrome and Edge 136+ refuse to enable `--remote-debugging-port` on the default
  profile, so DevTools automation against your daily profile is blocked by the browser.
- A Chrome window that is already running cannot be attached to — launching `chrome.exe`
  with debugging flags while Chrome is open forwards the request to the running instance
  and exits, leaving no endpoint to connect to.

The workflow is therefore "sign in once": ask Sonny to open the site in its own browser,
sign in there a single time, and the session persists in the profile directory for every
later command. If a Sonny browser is still running, the next launch re-attaches to it
instead of failing on the locked profile.

| What you say | Browser you see |
|---|---|
| "search Google for X", "search YouTube for Y", "play X on YouTube" | Sonny's interactive Chromium (results are scraped and played there) |
| "open X and read/click/fill…", "screenshot this page", "fill this application" | Sonny's interactive Chromium |

### Autofill

`UserProfileStore` keeps autofill values and DPAPI-encrypted credentials. `autofill` maps
saved values onto fields using `autocomplete`, name, id, placeholder and label signals,
with a per-field confidence score. Fields Sonny is not confident about are reported
rather than guessed, and form submission stays an explicit step. Synonyms for
site-specific labels can be added to the profile JSON.

### Visual debugging

`Page.captureScreenshot` produces full-page or viewport PNG/JPEG captures and
`Page.printToPDF` produces PDFs, saved under `%APPDATA%\Sonny\screenshots` and
`%USERPROFILE%\Downloads\Sonny` respectively — the result can be shown to the user or fed
back to the vision model.

---

## Vision

The camera path is a DirectShow graph producing RGB frames:

- `CameraCapture::initialize()` opens the device, `start()` begins streaming, and
  `ensure_frame_ready()` blocks until a frame is available.
- `enumerate_cameras()` lists devices; the configured device name selects one, empty means
  the default.
- `save_photo()` and `start_recording()` / `stop_recording()` write to disk via
  `VideoRecorder`.

When the user asks a vision question — "what do you see", "look at my screen", "can you
see me through the camera", and a long list of equivalents matched by
`is_vision_request()` — the orchestrator captures a camera frame when the camera feed is
on, and falls back to a primary-display screenshot when there is no camera or no frame
yet. The RGB image plus its dimensions are passed to
`LlamaWrapper::generate_stream_with_images()`, which routes through `mtmd` when a
projector was initialised.

Vision is enabled whenever a camera is available; the `camera_feed_enabled` setting
controls the live feed and `camera_device_name` selects the device.

---

## Memory and RAG

`RagEngine` is native C++. It stores documents, learned facts, and conversation memory on
disk under `%APPDATA%\Sonny\rag_data` and ranks them with a hybrid of BM25 and dense
cosine similarity over local embeddings.

### Storage

| Collection | Format | Location |
|---|---|---|
| Document index | JSONL (`index.json`) + float32 vectors (`embeddings.bin`) | `%APPDATA%\Sonny\rag_data\documents\` |
| User knowledge | JSONL, one taught fact per line | `%APPDATA%\Sonny\rag_data\user_knowledge\` |
| Conversation memory | One JSON file per session | `%APPDATA%\Sonny\rag_data\conversation_memory\` |

Documents are split into 512-character chunks with a 64-character overlap. The embedding
worker runs on its own thread and appends vectors in chunk order, so indexing does not
block the conversation.

### Retrieval

- BM25 term scoring plus a filename-match bonus.
- Dense cosine similarity over the local embedding vectors, weighted and floored
  (`dense_weight_`, `dense_min_similarity_`), and only counted above the floor.
- Cross-node vectors received from peers are scored at a reduced weight.

The retrieved context is truncated to `rag_max_context_chars` (default 1500 characters)
before being injected into the prompt.

### API

```cpp
// The first two parameters of initialize() are legacy subprocess-era arguments and are
// ignored; the engine is native. Only the embedding model name is honoured.
bool     initialize(python_path, script_path, embedding_model);
std::string query(text, top_k = 5, collection = "default");
std::string getContextForLLM(text, top_k = 5);
bool     indexDocument(filepath, collection);
bool     indexDirectory(directory, collection, recursive = true);
bool     teach(fact, category = "user_knowledge");
int      deleteUserFact(fact_prefix, category);
std::string remember(category = "user_knowledge");
std::string getUserKnowledgeForLLM();
std::string summarizeConversation(history);
void     saveConversationMemory(session_id, summary, key_points = {});
std::string health();
std::string stats(collection = "default");
```

`query`, `remember`, `health` and `stats` return JSON strings.

### Embedding model

`EmbeddingEngine` runs an ONNX sentence-embedding model through ONNX Runtime (CPU, two
intra-op threads) with its own WordPiece tokenizer, maximum sequence length 256. The model
is resolved in this order:

1. **Compiled into the executable** — build with
   `-DSONNY_EMBED_MODEL_FILE=<path>/model.onnx`; CMake emits RCDATA resources
   (`IDR_EMBEDDING_MODEL`, `IDR_EMBEDDING_VOCAB`) and the engine loads them from memory
   with `Ort::Session(env, data, size, options)`.
2. **On disk** — `models/<name>/`, `models/<name>-onnx/`, `resources/models/<name>`, or the
   HuggingFace layout `models/<name>/onnx/model.onnx`, with `vocab.txt` beside the model or
   one directory up.

This repository ships `resources/models/bge-large-en-v1.5/` (`model_int8.onnx` +
`vocab.txt`). If none is found, the engine logs a warning and retrieval falls back to
BM25 only.

---

## The Vectors Network

The vectors are an **opt-in** peer network between Sonny instances. They exchange only
content-free aggregates, so each node benefits from what the others learned about tool
reliability — without any conversation, query, file name, or personal data crossing the
wire. See `docs/VECTORS.md` for the full design.

| File | Responsibility |
|---|---|
| `VectorsConfig.h` | Gates: enabled, mode (`off`/`lan`/`wan`), room, room secret, relay URL, ports, intervals, k-anonymity, noise sigma, max remote weight |
| `VectorsTypes.{h,cpp}` | Wire payload, deterministic JSON, base64 float arrays, HMAC signing |
| `VectorsCrypto.{h,cpp}` | HMAC-SHA256 (BCrypt), constant-time compare, random tokens, Gaussian noise, bucketing |
| `PeerTransport.{h,cpp}` | LAN UDP discovery beacon + TCP HTTP endpoint; WAN relay POST/GET |
| `VectorsSync.{h,cpp}` | Contribution building, peer pulls, consensus, reputation weighting, mesh view |
| `tools/relay/SonnyRelay.cpp` | The standalone relay binary |

**Privacy gates applied before publishing:** corpus counts are bucketed and noised, tool
counters are noised, rare terms are dropped, embedding vectors are L2-clipped and noised,
and LoRA adapters are content-free weights only. Peers only accept contributions from the
same room, and payloads are HMAC-signed when a room secret is configured. Consensus values
are withheld until the k-anonymity threshold is met.

**Integration:** after RAG initialisation the orchestrator publishes corpus scale, term
frequencies and local embedding vectors, receives aggregated vectors and LoRA adapters
back, and applies peer adapters through `LlamaWrapper::load_lora_adapter()`.

---

## Interface

### Startup sequence

1. Load (or create) `%APPDATA%\Sonny\config.ini`; on first run show the first-run wizard.
2. Resolve the resource base — assets next to the executable (installed) or under
   `resources/` in the project root (development).
3. Pre-flight the GGUF model; if it is unusable, explain and offer the model picker.
4. Auto-detect an `mmproj-*.gguf` projector next to the model and enable vision if found.
5. Create the tray icon and its menu callbacks.
6. Register the 16 tools and build the tool-schema system prompt.
7. Initialise the assistant (audio, STT stack, LLM, TTS, avatar, video, RAG, vectors) and
   start listening.
8. Hand the RAG engine to the tool registry so tools can reach the knowledge base.
9. Hide the console (detaching it) unless launched from a terminal, then run the tray
   message loop.

### Tray menu

- **Settings** — open the settings dialog and apply changes live.
- **Mesh** — open the WebView2 peer-mesh visualisation.
- **Toggle console** — detach or re-attach the console; buffered output is replayed on
  return.
- **Exit** — shut down gracefully.

### Video and chroma key

The startup sequence plays `logo-loading-green.wmv` with chroma key while components
initialise, then `logo-start-green.wmv` with `sonny.wav`. Compositing uses
`UpdateLayeredWindow` on a layered window. The key colour is green `(0, 1, 0)` with a
tolerance of 1.0 by default, which is deliberately aggressive because WMV compression
smears the green screen edges.

### Avatar

`AvatarOverlay` creates a transparent DirectX 11 overlay window, loads a GLB model
(`sonny.glb` or `iron-man.glb` per `avatar_type`), and renders it with skeletal animation,
linear blend skinning, and a directional light with per-pixel normal interpolation. It can
be rotated and positioned anywhere on the desktop, and its visibility is toggleable.

---

## Project Structure

```
sonny/
├── CMakeLists.txt                  # Build, dependency self-provisioning, arch-list generation
├── build.bat                       # Incremental MSBuild (assumes a configured build dir)
├── cmake.bat                       # Configure + build  (cmake.bat clean wipes build/)
├── build_installer.bat             # MSI installer build (WiX)
├── README.md
│
├── src/
│   ├── core/                       # main.cpp, AssistantOrchestrator, ConfigManager,
│   │                               # SystemTray, SettingsDialog, FirstRunWizard,
│   │                               # MeshDialog, CameraCapture, VideoRecorder
│   ├── ai/                         # LlamaWrapper, KokoroWrapper, RagEngine, EmbeddingEngine
│   ├── audio/                      # AudioCapture, AudioPlayback, STTEngineWrapper,
│   │                               # WakeWordEngine, VADEngine, WebSpeechWrapper,
│   │                               # WhisperFeatures
│   ├── browser/                    # BrowserTool, BrowserServiceImpl, ChromiumLauncher,
│   │                               # ChromiumCdpSession, BrowserWebSocket, BrowserPageScripts,
│   │                               # SiteFlows, UserProfileStore, BrowserCrypto, BrowserUtil
│   ├── net/                        # HttpClient, VectorsConfig, VectorsTypes, VectorsCrypto,
│   │                               # PeerTransport, VectorsSync
│   ├── tools/
│   │   ├── system/                 # SystemCommandTool, ProcessTool, ScreenControlTool,
│   │   │                           # BatteryTool, WifiTool, VolumeControlTool
│   │   ├── information/            # CalculatorTool, TimeTool, WeatherTool, NewsTool,
│   │   │                           # UnitConverterTool, NoteTool
│   │   ├── search/                 # WebSearchTool, GoogleSearchTool, YouTubeSearchTool,
│   │   │                           # YouTubeVideoTool
│   │   ├── file/                   # FileTool, FileExplorerTool, ClipboardTool
│   │   ├── media/                  # MusicPlayerTool, AddMediaDirectoryTool,
│   │   │                           # RemoveMediaDirectoryTool, ListMediaLibraryTool,
│   │   │                           # CameraCaptureTool, FileIndex
│   │   └── productivity/           # JokeTool, SpellingTool, StopwatchTool, ReminderTool,
│   │                               # AppLauncherTool
│   ├── ui/                         # AvatarOverlay, VideoPlayer, CavaVisualizer
│   ├── utils/                      # Tool, PerfTimer, Trie, Logger, PathUtil, ToolUtils,
│   │                               # ZlibCompress
│   ├── resources/                  # resources.rc, resource_ids.h
│   └── sonny_pch.h                 # Precompiled header
│
├── resources/
│   ├── avatars/                    # sonny.glb, iron-man.glb (+ tinygltf/stb_image/json.hpp)
│   ├── images/                     # logo.ico, logo.png
│   ├── videos/                     # logo-loading-green.wmv, logo-start-green.wmv
│   ├── sounds/                     # sonny.wav, sonny_yes.wav, sonny_my_name_is_sonny.wav
│   ├── models/
│   │   ├── silero_vad.onnx         # VAD
│   │   ├── openwakeword/           # sonny.onnx, melspectrogram.onnx, embedding_model.onnx
│   │   ├── whisper-base-ct2/       # CTranslate2 Whisper (config.json, model.bin, tokenizer)
│   │   └── bge-large-en-v1.5/      # model_int8.onnx, vocab.txt (RAG embeddings)
│   ├── rag_data/                   # Legacy seeded chroma_db (the native index writes
│   │                               # to %APPDATA%\Sonny\rag_data instead)
│   ├── web/                        # web_speech.html, mesh.html, vis-network.min.js
│   └── mesh.html
│
├── docs/
│   ├── BUILD_CONFIG.md
│   └── VECTORS.md
│
├── tools/
│   └── relay/SonnyRelay.cpp        # Standalone vectors relay
│
├── installer/                      # Product.wxs, License.rtf (WiX)
├── external/                       # Third-party dependencies (see below)
├── build/                          # Build output
└── installer_output/               # Generated MSI
```

### External dependencies

CMake provisions what is missing, so a fresh checkout builds without manual steps:

| Dependency | Source | Used for |
|---|---|---|
| `llama.cpp` | Git clone, pinned revision | LLM inference, sampling, `mtmd` vision |
| `kokoro-server` | Git clone, pinned revision | Kokoro TTS engine sources (statically linked) |
| `CTranslate2` | Git clone, pinned revision (v4.8.2), built from source | Whisper STT |
| `espeak-ng` | Git clone, pinned revision, built from source | Phonemization for Kokoro |
| `OpenBLAS` | Git clone `v0.3.29`, built from source | BLAS kernels for the Kokoro iSTFT |
| ONNX Runtime | Official release archive download (1.28.0) | Silero VAD, openWakeWord, embeddings |
| WebView2 SDK | NuGet download | Compile-time header for the mesh dialog |
| CUDA runtime DLLs | Copied from the local CUDA toolkit, if present | `ggml-cuda` runtime |

`llama.cpp` GPU backends are `AUTO` by default: CUDA is enabled when `nvcc` is found
(`CUDA_PATH`), Vulkan when the Vulkan SDK and `glslc` are found (`VULKAN_SDK`), and a CUDA
build failure under `AUTO` retries without CUDA rather than failing the configure step.
Set `-DSONNY_LLAMA_CUDA=ON|OFF|AUTO` and `-DSONNY_LLAMA_VULKAN=ON|OFF|AUTO` to control
this explicitly, and `-DSONNY_LLAMA_CUDA_ARCH` to pick the CUDA architectures
(`native` by default).

---

## Technical Specifications

### Language model

| Parameter | Value | Note |
|---|---|---|
| `n_ctx` | 4096 | Context window |
| `n_batch` | 512 | Prefill micro-batch |
| `n_gpu_layers` | 99 | Whole model offloaded; falls back to 0 on GPU OOM |
| `n_threads` | `clamp(cores, 4, 32)` | Inference |
| `n_threads_batch` | `clamp(cores, 8, 32)` | Batch |
| `max_tokens` | 512 (default) | Generation cap per call |
| temperature | 0.7 (default) | Per call |
| repetition penalty | 1.1 | Over the last 64 tokens |
| sampler | temperature → penalties → greedy | No top-k / top-p stages |

### Audio

| Path | API | Rate | Channels | Format | Buffering |
|---|---|---|---|---|---|
| Capture | WASAPI, shared, `AUTOCONVERTPCM` | 16 000 Hz | 1 | 16-bit PCM → float | 50 ms period, 4 buffers |
| Playback | waveOut | 24 000 Hz | 1 | 16-bit PCM | One `WAVEHDR` per write, 5 ms poll |

### Speech models

| Component | Engine | Model | Rate / threshold |
|---|---|---|---|
| Wake word | openWakeWord (ONNX Runtime, CPU) | `sonny.onnx` + melspec + embedding | 16 kHz, 80 ms chunks, score > 0.5 |
| VAD | Silero V5 (ONNX Runtime) | `silero_vad.onnx` | 16 kHz, 32 ms frames, p = 0.35 |
| STT | CTranslate2 Whisper | `whisper-base-ct2` | beam 5, `en`, temperature fallback |
| TTS | Kokoro (in-process) | voice pack under `kokoro/` | 24 kHz, speed 1.15 |

### Retrieval

| Parameter | Value |
|---|---|
| Chunk size | 512 characters, 64-character overlap |
| Retrieval | BM25 + dense cosine (`dense_weight_`, `dense_min_similarity_ = 0.2`) |
| Prompt budget | `rag_max_context_chars` = 1500 characters |
| Embedding sequence length | 256 tokens |

### GPU backend selection

There is no backend setting to pick — Sonny detects the accelerator at startup. Both the
CUDA and Vulkan backends are compiled into the executable, and `LlamaWrapper::initialize()`
reads the vendor of the primary DXGI adapter and hides the *other* backend's devices in the
process environment before `llama_backend_init()` builds the ggml backend registry:

| Primary adapter | Backend used | Other backend hidden via |
|---|---|---|
| NVIDIA (`0x10DE`) | CUDA | `GGML_VK_VISIBLE_DEVICES=none` |
| AMD (`0x1002`) / Intel (`0x8086`) | Vulkan | `CUDA_VISIBLE_DEVICES=-1` |
| No GPU adapter | CPU only | both variables set |

Hiding matters: with both backends active the same physical GPU would be exposed twice
(`CUDA0` + `Vulkan0`) and the model would be split across two backends. If no GPU context
can be created, Sonny falls back to CPU inference.

### Build flags

MSVC Release builds use `/MT /O2 /Oi /Ot /Oy /GL /GF /Gy /arch:AVX2`, with `/MP` for
multi-process compilation, a conforming preprocessor (`/Zc:preprocessor`), link-time code
generation and `/OPT:REF /OPT:ICF`. The target uses a **unity build** with a batch size of
8 and a precompiled header; files with conflicting anonymous-namespace symbols are
excluded from the unity batches and listed explicitly in `CMakeLists.txt`.

---

## System Requirements

### Minimum

- **OS:** Windows 10, 64-bit
- **CPU:** Intel Core i5 8th Gen or equivalent
- **RAM:** 8 GB
- **GPU:** NVIDIA GTX 1060 6 GB (or any Vulkan-capable GPU)
- **Storage:** ~10 GB free, plus your GGUF model
- **Audio:** a Windows-compatible microphone and speakers
- **Browser:** Chrome or Edge installed (for the automation tools)

### Recommended

- **OS:** Windows 11, 64-bit
- **CPU:** Intel Core i7 11th Gen or equivalent
- **RAM:** 16 GB
- **GPU:** NVIDIA RTX 3060 12 GB or better
- **Storage:** SSD

### Development

- **Visual Studio 2022** Build Tools with the MSVC x64 toolchain
- **CMake** 3.14 or later
- **CUDA Toolkit** 12.6+ (for the CUDA backend; 12.9+ avoids the nvcc/MSVC host-compiler
  rejection)
- **Vulkan SDK** (for the Vulkan backend)
- **Git** and network access on the first configure (dependencies are cloned or downloaded)
- **WiX Toolset** (only for the MSI installer)

---

## Building

### Full configure and build

```batch
cmake.bat
```

This configures with the Visual Studio 17 2022 generator (x64) in Release and builds with
one job per logical core. Dependencies that are missing are cloned and built during the
configure step, so the first run takes a while.

To wipe `build/` and start over:

```batch
cmake.bat clean
```

### Incremental rebuild

```batch
build.bat
```

Assumes the project is already configured; invokes MSBuild directly on
`personal_assistant.vcxproj` in Release with `/maxcpucount`.

### Manual

```batch
mkdir build
cd build
cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j
```

### Useful CMake options

| Option | Default | Effect |
|---|---|---|
| `SONNY_LLAMA_CUDA` | `AUTO` | CUDA backend for llama.cpp |
| `SONNY_LLAMA_VULKAN` | `AUTO` | Vulkan backend for llama.cpp |
| `SONNY_LLAMA_CUDA_ARCH` | `native` | CUDA architectures |
| `SONNY_EMBED_MODEL_FILE` | *(unset)* | Compile an ONNX embedding model into the executable |
| `SONNY_EMBED_VOCAB_FILE` | *(auto)* | Override the `vocab.txt` used with the embedded model |

### Installer

```batch
build_installer.bat
```

Requires the WiX Toolset and produces an MSI from `installer/Product.wxs`.

---

## Configuration

`config.ini` lives at **`%APPDATA%\Sonny\config.ini`** (created on first run), not next to
the executable. All keys live in the `[Settings]` section.

```ini
[Settings]
llama_model_path=C:\ai_models\Llama-3.2-3B-Instruct-Q8_0.gguf
kokoro_voice=af_heart
language=en
tts_speed=1.15
avatar_type=sonny
system_prompt=You are Sonny, a professional butler AI. ...
use_browser_stt=false
use_browser_tts=false
browser_voice=
show_avatar=true
rag_embedding_model=all-MiniLM-L6-v2
wake_word_enabled=false
debug_mode=false
enable_cava_visualizer=true
console_listening_mode=false
mic_gain=1
rag_max_context_chars=1500
camera_feed_enabled=true
camera_device_name=
vectors_enabled=true
vectors_mode=wan
vectors_room=global
vectors_room_secret=
vectors_rendezvous_url=http://vectorsync.ddns.net:47830/sonny/vectors/v1/publish
vectors_share_tool_metrics=true
vectors_k_anonymity=3
vectors_dp_sigma=1
```

| Key | Default | Meaning |
|---|---|---|
| `llama_model_path` | *(empty)* | Path to the GGUF brain model. Required. |
| `kokoro_voice` | `af_heart` | Voice pack `.npy` name |
| `language` | `en` | TTS/STT language code |
| `tts_speed` | `1.15` | Speech rate multiplier |
| `avatar_type` | `sonny` | `sonny` or `iron-man` |
| `system_prompt` | butler persona | Assistant personality; also where tool use is described |
| `use_browser_stt` | `false` | Use Chrome Web Speech recognition instead of local STT |
| `use_browser_tts` | `false` | Use Chrome speech synthesis instead of Kokoro |
| `browser_voice` | *(empty)* | Selected browser voice |
| `show_avatar` | `true` | Show the 3D avatar overlay |
| `rag_embedding_model` | `all-MiniLM-L6-v2` | Embedding model name resolved on disk |
| `wake_word_enabled` | `false` | Wake-word gating (when off, Silero VAD segments instead) |
| `debug_mode` | `false` | Performance timing logs |
| `enable_cava_visualizer` | `true` | Terminal CAVA visualiser |
| `console_listening_mode` | `false` | Inject console stdin text as if transcribed |
| `mic_gain` | `1.0` | Microphone input gain |
| `rag_max_context_chars` | `1500` | Retrieval budget injected into the prompt |
| `camera_feed_enabled` | `true` | Enable the camera feed for vision |
| `camera_device_name` | *(empty)* | Specific camera, empty = default |
| `vectors_*` | see above | Peer network settings |

### Command line

```batch
personal_assistant.exe <llama_model_path> [llama_server_path] [tts_model_dir] [avatar_path]
```

Positional arguments override the configured values in that order. The fifth historical
argument (a Python STT script path) is no longer used — STT is native.

### Console input

`console_listening_mode` makes the console behave as a text input channel: whatever you
type is fed to the orchestrator exactly as if it had been transcribed, which is how you
drive Sonny without a microphone.

---

## Running

```batch
cd build\Release
personal_assistant.exe
```

Sonny starts hidden in the system tray. Use the tray menu to open settings, view the mesh,
bring back the console, or exit. If launched from a terminal it keeps that terminal, so
you can watch the telemetry output.

The LLM model path must be configured: on the first run the first-run wizard asks for it,
and any later run with a missing or unusable model opens the settings dialog so you can
pick a file.

### Vision models

Put a vision-capable GGUF next to an `mmproj-*.gguf` projector in the same directory.
Sonny scans the model's directory and then the models directory for a file whose name
contains `mmproj` or `vision` and ends in `.gguf`, initialises `mtmd`, and turns image
routing on automatically — there is no config toggle for it. If the GGUF declares a
`clip` architecture it is treated as a projector rather than a language model. Note that
the pinned llama.cpp revision does not implement the `mllama` architecture (Llama 3.2
Vision / Ollama's vision builds); the pre-flight reports this by name rather than letting
it fail as a generic load error.

---

## Settings Dialog

A tabbed Win32 dialog (`IDD_SETTINGS_TABBED`):

**AI & Voice**
- Llama model path with a file browser
- Kokoro voice (enumerated from the voice pack)
- Language
- TTS speed
- Microphone gain

**Browser STT/TTS**
- Use browser STT
- Use browser TTS
- Browser voice (enumerated from the Web Speech API)

**Avatar & Prompt**
- Show avatar
- Avatar: Sonny or Iron Man
- System prompt
- Camera feed enabled
- Camera device

**RAG Engine**
- Embedding model path with a file browser
- Wake word enabled
- Debug mode
- RAG context budget

**Vectors**
- Enabled
- Mode (`off` / `lan` / `wan`)
- Room
- Relay URL
- Share tool metrics

The first-run wizard (`IDD_FIRST_RUN_WIZARD`) asks only for the essentials: model path,
voice, avatar and language.

---

## Where Sonny Keeps Data

| Data | Location |
|---|---|
| Configuration | `%APPDATA%\Sonny\config.ini` |
| RAG document index, vectors, user knowledge, conversation memory | `%APPDATA%\Sonny\rag_data\` |
| Browser profile (cookies, logins, local storage) | `%APPDATA%\Sonny\browser-profile` |
| Autofill values and DPAPI-encrypted credentials | `%APPDATA%\Sonny\browser_profile.json` |
| Screenshots | `%APPDATA%\Sonny\screenshots` |
| Downloads and PDFs | `%USERPROFILE%\Downloads\Sonny` |
| Vectors node identity | `%APPDATA%\Sonny\vectors\node_id.txt` |
| Camera photos and videos | the folder the `camera` tool opens |

**Resetting the browser session** — close Sonny's browser window and delete
`%APPDATA%\Sonny\browser-profile`. Autofill values and saved credentials are untouched, and
you simply sign in again next time.

---

## Troubleshooting

**"This model cannot be loaded by the bundled llama.cpp"**
The GGUF's `general.architecture` is not implemented by the pinned llama.cpp revision.
The message names the architecture and lists what this build does support. Pick a
compatible GGUF, or build llama.cpp from a newer revision.

**No voice is heard / transcription is empty**
Check the microphone in Windows Settings → Privacy & security. Verify the input device
level, and raise `mic_gain` if the signal is quiet. With the wake word enabled, make sure
you say the wake word clearly before the command — the recording window is a fixed four
seconds.

**Wake word never fires**
`wake_word_enabled` defaults to `false`; enable it in the RAG tab. Check that all three
`models/openwakeword/*.onnx` files are present. A noisy room makes the classifier noisy.

**TTS produces no sound or fails to load**
KokoroWrapper resolves `config.json`, `onnx/kokoro_encoder.onnx`,
`onnx/har_generator.onnx`, `onnx/kokoro_decoder.onnx`, `voices_npy/`, `espeak-ng-data/` and
`misaki-data/` under the model root. Any missing file fails initialisation — check the
kokoro directory that the build staged next to the executable.

**No embedding model found**
The log names the paths it checked. Either ship a model under `models/<name>/` with
`vocab.txt`, or compile one in with `-DSONNY_EMBED_MODEL_FILE=...`. Until then, retrieval
is BM25-only.

**"UpdateLayeredWindow failed"**
The layered window handle was invalid — usually because the video window was destroyed and
recreated while a frame was still being composited. Restart the app; if it persists, the
WMV assets are likely missing or corrupt.

**Avatar does not appear**
Confirm `show_avatar` is set and the GLB file exists at the resolved path. The overlay
requires a working DirectX 11 device.

**Browser automation never starts**
Another browser instance may hold `%APPDATA%\Sonny\browser-profile`. Close the Sonny
browser window, or simply retry — Sonny reads the endpoint record / `DevToolsActivePort`
from the previous run and re-attaches instead of failing.

**The automation browser is not signed in**
That is the dedicated profile. Sign in once inside that window and it persists.

**A click lands on the wrong element**
The target matched several elements. Ask Sonny to `find` it first, then use the 1-based
`index`; the tool reports nearest matches when a target is ambiguous.

**Autofill skips a field**
The match confidence was too low, so Sonny reported it rather than guessing. Add the
page's exact label as a synonym or key in `%APPDATA%\Sonny\browser_profile.json`.

**CUDA build failure during configure**
With `SONNY_LLAMA_CUDA=AUTO`, a failed CUDA build is retried without CUDA (Vulkan/CPU
still work) and CMake prints a warning. Upgrade to CUDA 12.9+ to build against this MSVC.
Setting `SONNY_LLAMA_CUDA=ON` explicitly makes the failure loud instead.

**`LNK1104: cannot open file`**
The executable is running and locked. Close it before rebuilding.

**Vulkan loader missing**
CMake warns when `C:\Windows\System32\vulkan-1.dll` is absent. Install a graphics driver
that ships the Vulkan runtime; the loader is deliberately not bundled app-local, because
shadowing the system loader is a well-known source of driver breakage.

---

## Dependencies and Licenses

| Dependency | Role | License |
|---|---|---|
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | LLM inference, sampling, multimodal | MIT |
| [CTranslate2](https://github.com/OpenNMT/CTranslate2) | Whisper inference | MIT |
| [kokoro-server](https://github.com/marty1885/kokoro-server) | Kokoro TTS engine | MIT |
| [Misaki](https://github.com/hexgrad/misaki) | G2P for Kokoro | MIT |
| [eSpeak NG](https://github.com/espeak-ng/espeak-ng) | Phonemization | GPL-3.0 (used as a library) |
| [ONNX Runtime](https://github.com/microsoft/onnxruntime) | VAD, wake word, embeddings | MIT |
| [OpenBLAS](https://github.com/OpenMathLib/OpenBLAS) | BLAS for the Kokoro iSTFT | BSD-3-Clause |
| [WebView2](https://learn.microsoft.com/microsoft-edge/webview2/) | Mesh dialog | Runtime is free; SDK is a NuGet package |
| [tinygltf](https://github.com/syiyu/tinygltf) | GLB parsing (vendored) | MIT |
| [vis-network](https://github.com/visjs/vis-network) | Mesh visualisation | Apache-2.0 / MIT |
| [WiX Toolset](https://wixtoolset.org/) | MSI installer | MS-RL |

Windows components used through the platform SDK — Media Foundation, DirectX 11, WASAPI,
WinHTTP, DirectShow, DPAPI, BCrypt — are covered by the Windows SDK terms.
