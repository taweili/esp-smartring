# GEODIS Buddy — Physical-AI voice buddy (research note)

**What this is.** A survey of `hermes-ws/geodis-buddy` — the materials for
**GEODIS Buddy V1**, a physical ("embodied") AI voice assistant built by
**CuteAI** for GEODIS's Innovation & Brand Experience team, destined for a
2026-10-20 trade show. It is a **peer project to this bench**: a small
voice-interactive companion on a Linux SBC (Raspberry Pi 5, not an M10), with
an animated face, a mic array, a speaker, and a cloud LLM behind it. The
part that is genuinely reusable knowledge here is not the hardware — it is
the **voice backend**, which GEODIS runs on **Azure OpenAI Realtime
(`gpt-realtime-2.1`) over WebRTC**, with **RAG via an MCP/MongoDB knowledge
base**. The whole thing was probed live (no auth needed) and the findings are
below.

This is a **survey, not a spec**, in the same spirit as the hosted-speech
surveys folded into the `m10-dev` skill's `references/speech.md`. Everything
below was read first-party on **2026-11-03**
from the materials in `hermes-ws/geodis-buddy/` (docs, probe scripts, live UAT
endpoint probing, WebRTC session captures). Treat every claim here as evidence
from that directory, not as a GEODIS promise.

> **Naming note.** The filename is `geodis-ai-pai.md`; "PAI" is read here as
> **Physical AI** (i.e. embodied/robotic AI), which is exactly what the GEODIS
> Buddy is — a physical conversational assistant, explicitly *not* an
> autonomous robot (per the GEODIS brief).

## Why this matters to the g-buddy bench

- It is a **working example of the exact product class g-buddy is**: a small
  voice-in / voice-out companion with a face, on a Linux SBC, talking to a
  cloud LLM. GEODIS's version proves the pattern at trade-show scale.
- The **audio/STT/TTS contract** is the transferable part: **PCM 24 kHz mono
  S16** in and out, **speech-to-speech** (the LLM synthesizes directly; there
  is *no* separate TTS/STT service), **server-side VAD** for turn detection,
  and **STT defaulting OFF** (you must opt in).
- The **integration lesson** is the sharpest: the vendor backend does **not**
  execute tool calls. The *client* must catch the function call, hit a
  `/tool-call` endpoint itself, and write the result back into the Realtime
  session — otherwise the agent says "let me check" and **hangs forever**.
- The **security finding** is worth remembering: the UAT endpoint is
  completely unauthenticated (empty body returns a usable Azure Realtime
  token), so **never assume a public demo endpoint is protected**.

## 1. What the GEODIS Buddy is

Per `attachments/Brief_GEODIS_Buddy_2.docx` and
`attachments/Proposal_V1_Trade-Show_Demo_Prototype_Development_(Revised).docx`:

- **Positioning.** A physical conversational assistant that gives GEODIS
  customers/prospects "natural access to the key services available on
  Geodis.com" **without navigating the website**. Not an autonomous robot —
  an **intelligent access point**, embodied as a voice-interactive digital
  companion.
- **Form factor.** ~10 cm H × 8.5 cm W × 5.5 cm D body, round IPS display
  (~6.2×6.2 cm) as the animated face, servo-driven ears (up = listening,
  down = processing, wiggle = success), NeoPixel RGB ring (GEODIS blue =
  standby, green = success, orange = attention, red = error), ReSpeaker Mic
  Array V2 for far-field capture, 10 W speaker + MAX98357 amp in a 3D-printed
  acoustic chamber, backlit push-to-talk button, battery (8–12 h, UN38.3) plus
  USB-C wall power.
- **Compute.** Raspberry Pi 5 (8 GB) per unit, 64 GB industrial microSD.
- **Voice states on the face.** Standby, Active Listening, Processing/Thinking,
  Parcel Alert, Success Confirmation.
- **Target journeys.** (1) **Track & Trace** (priority: query a tracking/
  shipment/reference number, speak the status, show it on screen, optional
  email), (2) **Transportation Quote Request** (collect cargo/route/urgency,
  transfer to the pricing app), (3) **Connect with a GEODIS Expert** (identify
  domain, pick a contact, trigger follow-up), (4) **Information about GEODIS**
  (activities, footprint, key figures, services, sustainability, innovation).
- **Four demo workflows** (from the proposal): Express Track & Trace; Smart
  Freight Quotation; GEODIS Specialist Routing (cold chain / aerospace /
  customs); Corporate Navigator & ESG Q&A.
- **Demo architecture.** Each Buddy pairs 1:1 with a **companion PC** over a
  **private direct local link** (wired Ethernet or private AP), bypassing
  venue Wi-Fi; the PC has 4G/5G failover. The proposal explicitly avoids live
  integration with GEODIS production APIs — the four workflows are
  **simulated** by the companion app driving a browser/LLM. This is the
  "zero-risk backend" pattern: demo the experience, fake the data.

## 2. The voice backend — what actually runs it

Source: `GEODIS-Voice-Agent-平台与API完整档案.md`,
`geodis-voice-agent-api-2026-11-03.md`,
`geodis-voice-agent-tts-stt-2026-11-03.md`, `geodis_buddy_demo.py`, all live-
probed 2026-11-03.

This is **not** a "voice API" with /tts and /stt endpoints. It is a **deployed
speech-to-speech product**: Azure OpenAI Realtime (`gpt-realtime-2.1`,
resource `geodisopenai`) reached over **WebRTC**, wrapped by a GEODIS app
backend (Python, published via Azure APIM), backed by an **MCP + MongoDB
vector knowledge base** on Azure Red Hat OpenShift (West Europe). The only
public surface is a single-page web client plus a few tool endpoints.

### Live session config (from `session.created`)

- Model **`gpt-realtime-2.1`**, `output_modalities: ["audio"]` (audio only).
- Audio **PCM 24 kHz mono S16** in and out; output voice **`coral`**, speed 1.0.
- **`audio.input.transcription: null` — STT is OFF by default.**
- Turn detection: **server VAD** (threshold 0.5, prefix 300 ms, silence 500 ms,
  `create_response: true`, `interrupt_response: true` — it auto-replies and
  allows barge-in).
- One tool registered: **`rechercher_base_geodis`** (a RAG function),
  `tool_choice: auto`.

### Environment (two different hostnames)

| Env | Host | Reachable? |
|---|---|---|
| **UAT** | `https://connect-weu-uat.api.geodis.com/corp/voice-agent/v1` | ✅ public, 200 |
| **DEV** | `https://connect-weu-dev-api.intranet.geodis.org/...` | ❌ intranet only |
| Swagger | `publish-api-uat.apps.caas-noprod.intranet.geodis.org` | ❌ intranet |

The official Swagger PDF (`attachments/GEODIS_Voice_Agent_Publish_API.pdf`)
documents the **DEV** address; the usable one is the **UAT** host from the
email. They are different domains, not a typo.

### Endpoints (all unauthenticated on UAT)

| Method | Path | Purpose |
|---|---|---|
| `POST` | `/token` | Ephemeral WebRTC token (`ek_…`). Body optional: `user_token` (Azure AD OBO), `system_prompt`, `existing_session_id`, `chat_session_id`. |
| `POST` | `/tool-call` | **RAG tool execution** (not in the Swagger main list; found in the UI). `{name, arguments, session_id, mcp_database?, mcp_collection?}`. |
| `POST` | `/api/v1/save-message` | Persist one transcript message (`{chat_session_id, role, text}` → `{"ok":true}`). |
| `GET` | `/api/v1/transcripts/{chat_session_id}` | Fetch a session's transcript (`{"count":N,"messages":[…]}`; unknown id → empty, not 403). |
| `GET` | `/api/v1/info` | Agent metadata: `agent_type:"voice"`, `capabilities:["webrtc","rag"]`, `kb_count`, `mcp_url`. |
| `GET` | `/health`, `/api/v1/health` | Health checks. |
| `GET` | `/` , `/logo.png` | The web client ("Assistant Vocal GEODIS", 28,907 B, French UI). |

**None of the REST chat endpoints exist** (`/chat`, `/completions`,
`/api/v1/chat`, `/api/v1/tts`, `/api/v1/stt`, … all 404). Every real-time
interaction — including *text* input — happens inside a WebRTC session.

### The five client responsibilities (the anti-intuitive part)

1. **Write the WebRTC client yourself** (SDP / ICE / SRTP / DataChannel). The
   vendor does not ship one you can import; the web UI is the only client.
2. **Turn STT on.** `session.update` →
   `session.audio.input.transcription = {"model":"whisper-1"}`. Wrong shape
   (`session.input_audio_transcription`) → `unknown_parameter`. Without this,
   you never get the user's words.
3. **Execute the tool call yourself.** On
   `response.function_call_arguments.done`, `POST /tool-call` with the
   arguments, then send a `function_call_output` item back and a
   `response.create`. **Skipping this = the agent says "let me check" and
   hangs indefinitely** (measured 90 s+ with no events).
4. **Persist the transcript yourself.** The server does **not** auto-save; the
   UI calls `/api/v1/save-message` per bubble.
5. **Handle PCM 24 kHz mono** in and out (media side).

### `/tool-call` quirks

- `arguments` must be a **JSON string**, not an object (object → HTTP 500
  `"the JSON object must be str, bytes or bytearray, not dict"`).
- `name` is **ignored** — seven different names returned identical results.
  The real tool is `rechercher_base_geodis`.
- The response is a JSON-RPC wrapper whose `text` is itself a JSON string
  (double-escaped; needs a second `json.loads`).

### What the RAG actually returned

- `geodisxx2026` → GEODIS "Distribution & Express 2026" brochure
  (score 0.7458, source `geodis.sharepoint.com`).
- `tracking number` → GPS/GSM Tracker product page.
- `test` → `annual reports#alloga france 2020.pdf#page_00039`.

**Critical gap:** the knowledge base is corporate marketing documents — it has
**no live consignment data**. The demo's tracking scenario (Shanghai → Paris,
in transit, ETA 2026-11-05) cannot be answered from RAG; it must be injected
via `mcp_database`/`mcp_collection` or faked in the companion app.

### Measured behavior notes

- **Multi-turn works** in-session (round 1 "My name is David" → reply used
  "Hi David"), but **no cross-session memory** (`existing_session_id` resumes,
  does not remember).
- **STT accuracy** good; "geodisxx2026" transcribed as "geodes xx 2026".
- **TTS**: 2,815 audio frames (~56 s) recorded from the remote track;
  assistant text also comes via `response.output_audio_transcript.done`.
- **Content guardrail is soft.** "What is the capital of France?" was refused
  once ("I only handle GEODIS-related questions") then answered ("Paris") —
  do not rely on it as a content-safety boundary.
- **Mic capture on the probing machine failed** (PipeWire/PulseAudio had no
  usable source; `parecord` got 0 bytes) — the file and text paths were fully
  validated, mic path was not. Also, aiortc's PyAV wheel often lacks pulse
  capture (`no container format 'pulse'`), so the demo shells out to
  `parecord`.

## 3. Security findings (the important one)

The UAT `/token` is **completely unauthenticated**: an empty `{}` body
returns a usable `ek_` token that swaps directly for an Azure Realtime SDP
answer (HTTP 201). Consequences: anyone with the URL can burn GEODIS's Azure
OpenAI budget (Realtime is metered per minute), read/write any
`chat_session_id` transcript, and inject any `system_prompt`. Fine for a
staging demo; **must be gated before production** — and a good thing to flag
back to the vendor.

## 4. What this teaches g-buddy

- **Speech-to-speech vs two services.** GEODIS's whole audio path is one
  Realtime model — there is no `/tts` or `/stt` to call. g-buddy's current
  split (Vosk/Piper offline, Qianwen cloud) is a different, more portable
  design; Azure Realtime is the "single-model, WebRTC" alternative worth
  knowing about if a customer already runs it.
- **Tool execution is the client's job.** For any Realtime/agent backend,
  plan for the model calling a function and *you* having to run it and write
  the result back — don't assume the vendor executes it.
- **The ephemeral-token pattern** (`POST /token` → `ek_` → `Authorization:
  Bearer` → Azure Realtime) is a clean way to let an edge device use a cloud
  Realtime session without holding long-lived credentials.
- **Opt-in STT + server VAD** is a sensible default for a noisy-booth buddy:
  keep transcription off unless the UI needs it, let the server decide turns.
- **The 24 kHz mono contract** is the hard requirement on the media side —
  plan the capture/playback pipeline around it.
- **Demo data is the demo's hard part.** The RAG gap (no consignment data) is
  the same lesson as any "voice + knowledge base" demo: you need real or
  well-faked data behind the intents, or the assistant will confidently
  answer from the wrong document.

## Sources

All under `hermes-ws/geodis-buddy/`, first-party, 2026-11-03:

- `README-STT-TTS-demo.md` — runnable demo + pitfall list.
- `geodis_buddy_demo.py` — complete working WebRTC client (14 KB).
- `geodis-voice-agent-api-2026-11-03.md` — endpoint analysis.
- `geodis-voice-agent-tts-stt-2026-11-03.md` — STT/TTS capability probing.
- `GEODIS-Voice-Agent-平台与API完整档案.md` — consolidated technical archive
  (architecture, session lifecycle, security, full pitfall table).
- `attachments/GEODIS_Voice_Agent_Publish_API.pdf` — official Swagger printout
  (DEV endpoint documented).
- `attachments/Brief_GEODIS_Buddy_2.docx` — GEODIS functional/technical brief
  (vision, journeys, prototype architecture).
- `attachments/Proposal_V1_Trade-Show_Demo_Prototype_Development_(Revised).docx`
  — CuteAI proposal (hardware BOM, demo architecture, 4 workflows, timeline).
- `attachments/MNDA_Geodis_Interservices_GEODIS_Buddy_draft.docx` — mutual NDA
  (prototype scope, no-training clause).
- Probe scripts (not in the repo): `/home/david/.hermes/cache/scratch/voice_probe{,2,3,4,5,6}.py`, `conv_probe.py`, `persist_probe.py`, `diag_rag.py`; venv `webrtc-venv` (aiortc 1.15.0).

## 5. Verification update (2026-11-10)

Re-probed live and exercised the full chain with
`scripts/test_geodis_api.py` (HTTP + WebRTC, aiortc 1.15.0). New / confirmed
findings, in the order the ring's `ring_geodis` module meets them:

- **`/token` is still unauthenticated** (empty body → 200, `ek_…` token).
- **WebSocket is not exposed**: `GET /openai/v1/realtime?model=…` with upgrade
  headers returns **404** from the Azure APIM. WebRTC is the only transport.
- **`session.created` nests `turn_detection` under `audio.input`, not at the
  session top level.** Default config captured:

  ```json
  {
    "type": "realtime", "object": "realtime.session",
    "id": "sess_…", "model": "gpt-realtime-2.1",
    "output_modalities": ["audio"],
    "instructions": "Vous êtes standardiste téléphonique chez GEODIS…",
    "audio": {
      "input": {
        "transcription": null,
        "format": {"type": "audio/pcm", "rate": 24000},
        "turn_detection": {
          "type": "server_vad", "threshold": 0.5,
          "prefix_padding_ms": 300, "silence_duration_ms": 500,
          "idle_timeout_ms": null,
          "create_response": true, "interrupt_response": true
        }
      },
      "output": {
        "format": {"type": "audio/pcm", "rate": 24000},
        "voice": "coral", "speed": 1.0
      }
    },
    "include": null
  }
  ```

- **Do not send `session.turn_detection` in `session.update`** — the GA
  endpoint rejects it with `unknown_parameter` (`Unknown parameter:
  'session.turn_detection'`). The server configures server_vad itself.
  `modalities` is likewise read-only.
- **`idle_timeout_ms` is `null`** in the default VAD config — no VAD idle
  timeout is advertised (a keepalive is still prudent; the absolute session
  lifetime is undocumented).
- **The assistant transcript event is `response.output_audio_transcript.done`**
  (Qianwen uses `response.audio_transcript.done` — the main copy-paste trap).
- **The client-executed tool call works end-to-end**:
  `response.function_call_arguments.done` → `POST /tool-call` (arguments as a
  JSON string) → `conversation.item.create` (`function_call_output`) →
  `response.create`. The `/tool-call` response is double-escaped
  (`result.result.content[0].text` is a JSON string).
- **STT opt-in confirmed**: `session.updated` echoes
  `audio.input.transcription = {"model": "whisper-1"}`; a spoken clip is
  transcribed correctly (`"Hello? Where is my shipment? The reference number
  is geodesxx2026."`).
- The SDP route exists (bogus offer → 400), and the SDP answer is a normal
  `application/sdp` body (HTTP 201).

Run it with:

```bash
python3 scripts/test_geodis_api.py --mode http
scripts/.venv/bin/python scripts/test_geodis_api.py --mode webrtc
scripts/.venv/bin/python scripts/test_geodis_api.py --mode voice
```
