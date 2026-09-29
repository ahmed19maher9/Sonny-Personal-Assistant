# Sonny Vectors — local intelligence and the peer metrics network

Two related features, both **free of charge and free of any external service**:

1. **Local semantic retrieval** — Sonny gets a dense vector memory in-process, so
   retrieval stops being purely keyword-based.
2. **The vectors** — an opt-in network between Sonny instances that shares
   *content-free counters*, so every node benefits from what the others learned
   about tool reliability and latency.

Nothing in either feature downloads content at runtime. The only file Sonny ever
fetches is the LLM brain model you already provide.

---

## 1. Local semantic retrieval (Phase 0)

### What changed

| Before | After |
|---|---|
| `RagEngine` ranked chunks with BM25 only | Hybrid: BM25 **+** cosine similarity over local embeddings |
| The orchestrator dropped any retrieved context ≥ 500 chars | Budget is configurable (`rag_max_context_chars`, default **1500**) |
| `RagEngine::initialize(..., embedding_model_path)` ignored its 3rd argument | The path is honoured; the orchestrator passes it |
| `AppConfig::rag_embedding_model` was a dead setting | It resolves the model (bundled, or on disk) |

### Where the model comes from

The app never downloads an embedding model. It uses, in this order:

1. **Compiled into the executable** (recommended — no loose files at all):

   ```bat
   cmake -DSONNY_EMBED_MODEL_FILE=resources/models/all-MiniLM-L6-v2/model.onnx -B build
   ```

   `CMakeLists.txt` emits an RCDATA resource and `EmbeddingEngine` loads it from
   memory with `Ort::Session(env, data, size, options)`.

2. **A model shipped next to the exe** at `models/all-MiniLM-L6-v2/`
   (`resources/models` is already staged there by the build), or under
   `resources/models/all-MiniLM-L6-v2/`.

Accepted layouts: any `.onnx` file, optionally inside an `onnx/` subfolder, with
`vocab.txt` next to it or one directory up (the HuggingFace repo layout).

### Which model to ship

| Model | Shipped size | License | Notes |
|---|---|---|---|
| **all-MiniLM-L6-v2** (int8 ONNX) | **23.0 MB** + 231 KB vocab | Apache-2.0 | **Recommended.** 384-d, 22.7M params, pre-quantized `model_quint8_avx2.onnx` |
| paraphrase-MiniLM-L3-v2 (int8) | 17.5 MB + 231 KB | Apache-2.0 | Lighter and faster, slightly weaker |
| potion-base-8M (Model2Vec) | ~7.6 MB int8 | MIT | Static table — no transformer pass, smallest, a bit weaker |
| bge-small-en-v1.5 | 133 MB fp32 (needs your own int8 export) | MIT | Best quality/size, but a build-time quantization step |

For scale: the installer already ships ~343 MB of Kokoro assets and ~145 MB of
Whisper weights, so 23 MB is ~4.5 % of the current payload.

**If you ship it as a file**, add a component to `installer/Product.wxs` (the
installer harvests `models/` explicitly, per file):

```xml
<ComponentGroup Id="EmbeddingModels" Directory="MODELSDIR">
  <Component Id="EmbeddingModelDir" Guid="*">
    <CreateFolder />
    <File Source="$(var.StageDir)\models\all-MiniLM-L6-v2\model.onnx" KeyPath="yes" />
    <File Source="$(var.StageDir)\models\all-MiniLM-L6-v2\vocab.txt" />
  </Component>
</ComponentGroup>
```

…and reference it from the `<Feature>` block. Choosing the embedded (RCDATA)
build avoids any installer change.

### Storage layout

| File | Contents |
|---|---|
| `%APPDATA%\Sonny\rag_data\documents\index.json` | Unchanged: the JSONL chunk index |
| `%APPDATA%\Sonny\rag_data\documents\embeddings.bin` | One float32 record per chunk, in chunk order (appended, never rewritten) |

Vectors live in their own file so the human-readable index stays readable and
indexing a new document stays an append rather than a full rewrite. A background
thread embeds existing chunks in batches of 32 **without holding the retrieval
lock**, so voice queries stay responsive while a large corpus is indexed.

If no model is available the engine logs it and continues with BM25 only — a
missing model can never break the assistant.
