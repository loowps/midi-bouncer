#include <clap/clap.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#ifdef MIDI_BOUNCER_DEBUG_LOG
#include <cstdarg>
#endif

namespace {

#ifdef MIDI_BOUNCER_DEBUG_LOG
void debugLog(const char *fmt, ...) {
    static std::FILE *file = [] {
        if (const char *explicitPath = std::getenv("MIDI_BOUNCER_LOG"))
            return std::fopen(explicitPath, "w");
        const char *dir = std::getenv("USERPROFILE");
        if (!dir) dir = std::getenv("HOME");
        if (!dir) dir = std::getenv("TEMP");
        if (!dir) dir = ".";
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/MIDI_BOUNCER.log", dir);
        return std::fopen(path, "w");
    }();
    if (!file) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fflush(file);
}
#define MCG_LOG(...) debugLog(__VA_ARGS__)
#else
#define MCG_LOG(...) ((void)0)
#endif

constexpr uint8_t  kChannelCount = 16;
constexpr uint8_t  kKeyCount     = 128;
constexpr int32_t  kMaxGroup     = 16;
constexpr uint32_t kHoldParamId  = kChannelCount;
constexpr uint32_t kParamCount   = kChannelCount + 1;
constexpr int32_t  kMaxHoldMs    = 5000;
constexpr double   kLookaheadMs  = 5.0;
constexpr uint32_t kEventCapacity = 4096;
constexpr uint32_t kMaxEventBytes = 64;

int32_t clampGroup(double value) {
    long rounded = std::lround(value);
    if (rounded < 0) return 0;
    if (rounded > kMaxGroup) return kMaxGroup;
    return static_cast<int32_t>(rounded);
}

int32_t clampHold(double value) {
    long rounded = std::lround(value);
    if (rounded < 0) return 0;
    if (rounded > kMaxHoldMs) return kMaxHoldMs;
    return static_cast<int32_t>(rounded);
}

struct ChannelNotes {
    std::array<uint8_t, kKeyCount> active{};
    std::array<uint8_t, kKeyCount> pendingRelease{};
    std::array<uint8_t, kKeyCount> arrivedAsMidi{};
    std::array<int32_t, kKeyCount> noteId{};
    uint16_t activeTotal = 0;
    int64_t  lastReleaseSample = -1;
};

struct BufferedEvent {
    int64_t  absTime = 0;
    uint32_t size = 0;
    alignas(8) uint8_t bytes[kMaxEventBytes] = {};
};

// Events are buffered and emitted on a timeline delayed by `latency` samples. Each
// note-on decision is made when the event is emitted, by which point every input
// within the look-ahead window has already been buffered -- so the look-ahead spans
// process-block boundaries.
class ChokeEngine {
public:
    ChokeEngine() {
        for (auto &group : groups)
            group.store(0, std::memory_order_relaxed);
        setSampleRate(48000.0);
    }

    void resetNotes() {
        for (auto &channel : channels)
            channel = ChannelNotes{};
        clock = 0;
        bufferHead = 0;
        bufferTail = 0;
        bufferCount = 0;
    }

    void setSampleRate(double rate) {
        sampleRate = rate > 0.0 ? rate : 48000.0;
        const long samples = std::lround(kLookaheadMs * sampleRate / 1000.0);
        latency = samples > 0 ? static_cast<uint32_t>(samples) : 0;
    }

    uint32_t latencySamples() const { return latency; }
    int64_t  currentSample() const { return clock; }
    void     advance(uint32_t frames) { clock += frames; }

    int32_t group(uint8_t channel) const {
        return groups[channel].load(std::memory_order_relaxed);
    }

    void setGroup(uint8_t channel, double value) {
        groups[channel].store(clampGroup(value), std::memory_order_relaxed);
    }

    int32_t holdMilliseconds() const {
        return holdMs.load(std::memory_order_relaxed);
    }

    void setHoldMilliseconds(double value) {
        holdMs.store(clampHold(value), std::memory_order_relaxed);
    }

    void enqueue(const clap_event_header_t *header, int64_t absTime) {
        if (header->size > kMaxEventBytes || bufferCount == kEventCapacity) {
            MCG_LOG("enqueue dropped size=%u count=%u\n", header->size, bufferCount);
            return;
        }
        BufferedEvent &slot = buffer[bufferTail];
        slot.absTime = absTime;
        slot.size = header->size;
        std::memcpy(slot.bytes, header, header->size);
        bufferTail = (bufferTail + 1) % kEventCapacity;
        ++bufferCount;
    }

    void drain(const clap_output_events_t *out, int64_t blockStart, uint32_t frames) {
        const int64_t blockEnd = blockStart + frames;
        while (bufferCount > 0) {
            BufferedEvent &event = buffer[bufferHead];
            const int64_t emitAbs = event.absTime + latency;
            if (emitAbs >= blockEnd)
                break;
            int64_t local = emitAbs - blockStart;
            if (local < 0)
                local = 0;
            emitEvent(out, event, static_cast<uint32_t>(local));
            bufferHead = (bufferHead + 1) % kEventCapacity;
            --bufferCount;
        }
    }

private:
    static bool inRange(int16_t channel, int16_t key) {
        return channel >= 0 && channel < kChannelCount && key >= 0 && key < kKeyCount;
    }

    static int16_t bufferedNoteOnChannel(const BufferedEvent &event) {
        const auto *header = reinterpret_cast<const clap_event_header_t *>(event.bytes);
        if (header->space_id != CLAP_CORE_EVENT_SPACE_ID)
            return -1;
        if (header->type == CLAP_EVENT_NOTE_ON)
            return reinterpret_cast<const clap_event_note_t *>(event.bytes)->channel;
        if (header->type == CLAP_EVENT_MIDI) {
            const auto *midi = reinterpret_cast<const clap_event_midi_t *>(event.bytes);
            if ((midi->data[0] & 0xF0) == 0x90 && midi->data[2] > 0)
                return midi->data[0] & 0x0F;
        }
        return -1;
    }

    void emitEvent(const clap_output_events_t *out, BufferedEvent &event, uint32_t local) {
        auto *header = reinterpret_cast<clap_event_header_t *>(event.bytes);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID) {
            switch (header->type) {
            case CLAP_EVENT_NOTE_ON: {
                auto *note = reinterpret_cast<clap_event_note_t *>(event.bytes);
                if (!decideNoteOn(out, local, event.absTime, note->channel, note->key,
                                  note->note_id, false))
                    return;
                break;
            }
            case CLAP_EVENT_NOTE_OFF:
            case CLAP_EVENT_NOTE_CHOKE: {
                auto *note = reinterpret_cast<clap_event_note_t *>(event.bytes);
                if (!handleNoteOff(note->channel, note->key, event.absTime))
                    return;
                break;
            }
            case CLAP_EVENT_MIDI: {
                auto *midi = reinterpret_cast<clap_event_midi_t *>(event.bytes);
                const uint8_t status  = midi->data[0] & 0xF0;
                const int16_t channel = midi->data[0] & 0x0F;
                if (status == 0x90 && midi->data[2] > 0) {
                    if (!decideNoteOn(out, local, event.absTime, channel, midi->data[1],
                                      -1, true))
                        return;
                } else if (status == 0x80 || (status == 0x90 && midi->data[2] == 0)) {
                    if (!handleNoteOff(channel, midi->data[1], event.absTime))
                        return;
                } else if (status == 0xB0 && (midi->data[1] == 120 || midi->data[1] == 123)) {
                    allNotesOff(channel, event.absTime);
                }
                break;
            }
            default:
                break;
            }
        }
        header->time = local;
        out->try_push(out, header);
    }

    bool decideNoteOn(const clap_output_events_t *out, uint32_t local, int64_t absTime,
                      int16_t channel, int16_t key, int32_t noteId, bool fromMidi) {
        if (!inRange(channel, key))
            return true;

        const int32_t myGroup = group(static_cast<uint8_t>(channel));
        if (myGroup != 0) {
            bool blocked = higherPriorityNoteOnAhead(absTime, channel, myGroup);
            for (int16_t higher = 0; !blocked && higher < channel; ++higher)
                blocked = group(static_cast<uint8_t>(higher)) == myGroup &&
                          engaged(static_cast<uint8_t>(higher), absTime);
            if (blocked) {
                channels[channel].pendingRelease[key]++;
                MCG_LOG("decide ch=%d key=%d grp=%d -> BLOCK\n", channel, key, myGroup);
                return false;
            }
            for (int16_t lower = channel + 1; lower < kChannelCount; ++lower)
                chokeChannel(out, local, lower, myGroup);
        }

        ChannelNotes &me = channels[channel];
        me.active[key]++;
        me.activeTotal++;
        me.noteId[key]        = noteId;
        me.arrivedAsMidi[key] = fromMidi ? 1 : 0;
        MCG_LOG("decide ch=%d key=%d grp=%d -> pass\n", channel, key, myGroup);
        return true;
    }

    bool handleNoteOff(int16_t channel, int16_t key, int64_t now) {
        if (!inRange(channel, key))
            return true;

        ChannelNotes &notes = channels[channel];
        if (notes.active[key] > 0) {
            notes.active[key]--;
            if (notes.activeTotal > 0 && --notes.activeTotal == 0)
                notes.lastReleaseSample = now;
            return true;
        }
        if (notes.pendingRelease[key] > 0) {
            notes.pendingRelease[key]--;
            return false;
        }
        return true;
    }

    void allNotesOff(int16_t channel, int64_t now) {
        if (channel < 0 || channel >= kChannelCount)
            return;
        ChannelNotes &notes = channels[channel];
        notes.active.fill(0);
        notes.activeTotal = 0;
        notes.lastReleaseSample = now;
    }

    bool higherPriorityNoteOnAhead(int64_t fromAbsTime, int16_t channel,
                                   int32_t targetGroup) const {
        const int64_t limit = fromAbsTime + latency;
        for (uint32_t k = 1; k < bufferCount; ++k) {
            const BufferedEvent &other = buffer[(bufferHead + k) % kEventCapacity];
            if (other.absTime > limit)
                break;
            const int16_t otherChannel = bufferedNoteOnChannel(other);
            if (otherChannel >= 0 && otherChannel < channel &&
                group(static_cast<uint8_t>(otherChannel)) == targetGroup)
                return true;
        }
        return false;
    }

    bool engaged(uint8_t channel, int64_t now) const {
        const ChannelNotes &notes = channels[channel];
        if (notes.activeTotal > 0)
            return true;
        const int32_t hold = holdMs.load(std::memory_order_relaxed);
        if (hold <= 0 || notes.lastReleaseSample < 0)
            return false;
        const int64_t holdSamples = static_cast<int64_t>(hold * sampleRate / 1000.0);
        return (now - notes.lastReleaseSample) < holdSamples;
    }

    void chokeChannel(const clap_output_events_t *out, uint32_t time, int16_t channel,
                      int32_t targetGroup) {
        if (group(static_cast<uint8_t>(channel)) != targetGroup)
            return;

        ChannelNotes &notes = channels[channel];
        if (notes.activeTotal == 0)
            return;

        for (int16_t key = 0; key < kKeyCount; ++key) {
            if (notes.active[key] == 0)
                continue;
            if (notes.arrivedAsMidi[key])
                emitMidiNoteOff(out, time, channel, key);
            else
                emitClapNoteOff(out, time, channel, key, notes.noteId[key]);
            notes.pendingRelease[key] =
                static_cast<uint8_t>(notes.pendingRelease[key] + notes.active[key]);
            notes.active[key] = 0;
        }
        notes.activeTotal = 0;
    }

    static void emitClapNoteOff(const clap_output_events_t *out, uint32_t time,
                                int16_t channel, int16_t key, int32_t noteId) {
        clap_event_note_t event{};
        event.header.size     = sizeof(event);
        event.header.time     = time;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type     = CLAP_EVENT_NOTE_OFF;
        event.note_id    = noteId;
        event.port_index = 0;
        event.channel    = channel;
        event.key        = key;
        event.velocity   = 0.0;
        out->try_push(out, &event.header);
    }

    static void emitMidiNoteOff(const clap_output_events_t *out, uint32_t time,
                                int16_t channel, int16_t key) {
        clap_event_midi_t event{};
        event.header.size     = sizeof(event);
        event.header.time     = time;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type     = CLAP_EVENT_MIDI;
        event.port_index = 0;
        event.data[0] = static_cast<uint8_t>(0x80 | (channel & 0x0F));
        event.data[1] = static_cast<uint8_t>(key);
        event.data[2] = 0;
        out->try_push(out, &event.header);
    }

    std::array<std::atomic<int32_t>, kChannelCount> groups;
    std::array<ChannelNotes, kChannelCount>         channels;
    std::array<BufferedEvent, kEventCapacity>       buffer;
    uint32_t bufferHead = 0;
    uint32_t bufferTail = 0;
    uint32_t bufferCount = 0;
    std::atomic<int32_t> holdMs{0};
    double   sampleRate = 48000.0;
    uint32_t latency = 0;
    int64_t  clock = 0;
};

struct Plugin {
    clap_plugin_t      clap{};
    const clap_host_t *host = nullptr;
    ChokeEngine        engine;
};

Plugin &pluginOf(const clap_plugin_t *plugin) {
    return *static_cast<Plugin *>(plugin->plugin_data);
}

uint32_t notePortsCount(const clap_plugin_t *, bool) { return 1; }

bool notePortsGet(const clap_plugin_t *, uint32_t index, bool isInput,
                  clap_note_port_info_t *info) {
    if (index != 0)
        return false;
    info->id                 = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", isInput ? "MIDI In" : "MIDI Out");
    return true;
}

const clap_plugin_note_ports_t notePortsExtension = {notePortsCount, notePortsGet};

uint32_t audioPortsCount(const clap_plugin_t *, bool) { return 0; }
bool audioPortsGet(const clap_plugin_t *, uint32_t, bool, clap_audio_port_info_t *) {
    return false;
}
const clap_plugin_audio_ports_t audioPortsExtension = {audioPortsCount, audioPortsGet};

uint32_t paramsCount(const clap_plugin_t *) { return kParamCount; }

bool paramsGetInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info) {
    if (index >= kParamCount)
        return false;
    *info = clap_param_info_t{};
    if (index == kHoldParamId) {
        info->id            = kHoldParamId;
        info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
        info->min_value     = 0;
        info->max_value     = kMaxHoldMs;
        info->default_value = 0;
        std::snprintf(info->name, sizeof(info->name), "Choke Hold (ms)");
        return true;
    }
    info->id            = index;
    info->flags         = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
    info->min_value     = 0;
    info->max_value     = kMaxGroup;
    info->default_value = 0;
    std::snprintf(info->name, sizeof(info->name), "Channel %u Group", index + 1);
    return true;
}

bool paramsGetValue(const clap_plugin_t *plugin, clap_id paramId, double *value) {
    ChokeEngine &engine = pluginOf(plugin).engine;
    if (paramId == kHoldParamId) {
        *value = static_cast<double>(engine.holdMilliseconds());
        return true;
    }
    if (paramId < kChannelCount) {
        *value = static_cast<double>(engine.group(static_cast<uint8_t>(paramId)));
        return true;
    }
    return false;
}

bool paramsValueToText(const clap_plugin_t *, clap_id paramId, double value, char *out,
                       uint32_t capacity) {
    if (paramId == kHoldParamId) {
        const int32_t ms = clampHold(value);
        if (ms == 0)
            std::snprintf(out, capacity, "Off");
        else
            std::snprintf(out, capacity, "%d ms", ms);
        return true;
    }
    const int32_t group = clampGroup(value);
    if (group == 0)
        std::snprintf(out, capacity, "Off");
    else
        std::snprintf(out, capacity, "%d", group);
    return true;
}

bool paramsTextToValue(const clap_plugin_t *, clap_id paramId, const char *text,
                       double *value) {
    if (text && (std::strcmp(text, "Off") == 0 || std::strcmp(text, "off") == 0)) {
        *value = 0;
        return true;
    }
    const double parsed = std::atof(text ? text : "0");
    *value = paramId == kHoldParamId ? clampHold(parsed) : clampGroup(parsed);
    return true;
}

void applyParameterEvent(ChokeEngine &engine, const clap_event_param_value_t *event) {
    if (event->param_id == kHoldParamId)
        engine.setHoldMilliseconds(event->value);
    else if (event->param_id < kChannelCount)
        engine.setGroup(static_cast<uint8_t>(event->param_id), event->value);
}

void paramsFlush(const clap_plugin_t *plugin, const clap_input_events_t *in,
                 const clap_output_events_t *) {
    ChokeEngine &engine = pluginOf(plugin).engine;
    const uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t *header = in->get(in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID &&
            header->type == CLAP_EVENT_PARAM_VALUE)
            applyParameterEvent(engine,
                                reinterpret_cast<const clap_event_param_value_t *>(header));
    }
}

const clap_plugin_params_t paramsExtension = {
    paramsCount,    paramsGetInfo,     paramsGetValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

constexpr uint8_t kStateMagic[4] = {'M', 'C', 'G', '1'};
constexpr int32_t kStateVersion  = 2;

void writeLittleEndian32(uint8_t *p, int32_t value) {
    auto bits = static_cast<uint32_t>(value);
    p[0] = static_cast<uint8_t>(bits);
    p[1] = static_cast<uint8_t>(bits >> 8);
    p[2] = static_cast<uint8_t>(bits >> 16);
    p[3] = static_cast<uint8_t>(bits >> 24);
}

int32_t readLittleEndian32(const uint8_t *p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) |
                                (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

bool writeFully(const clap_ostream_t *stream, const void *data, size_t size) {
    size_t written = 0;
    auto *bytes = static_cast<const uint8_t *>(data);
    while (written < size) {
        int64_t result = stream->write(stream, bytes + written, size - written);
        if (result <= 0)
            return false;
        written += static_cast<size_t>(result);
    }
    return true;
}

bool readFully(const clap_istream_t *stream, void *data, size_t size) {
    size_t read = 0;
    auto *bytes = static_cast<uint8_t *>(data);
    while (read < size) {
        int64_t result = stream->read(stream, bytes + read, size - read);
        if (result <= 0)
            return false;
        read += static_cast<size_t>(result);
    }
    return true;
}

bool stateSave(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
    ChokeEngine &engine = pluginOf(plugin).engine;

    uint8_t header[8];
    std::memcpy(header, kStateMagic, 4);
    writeLittleEndian32(header + 4, kStateVersion);
    if (!writeFully(stream, header, sizeof(header)))
        return false;

    uint8_t groups[kChannelCount * 4];
    for (uint8_t channel = 0; channel < kChannelCount; ++channel)
        writeLittleEndian32(groups + channel * 4, engine.group(channel));
    if (!writeFully(stream, groups, sizeof(groups)))
        return false;

    uint8_t hold[4];
    writeLittleEndian32(hold, engine.holdMilliseconds());
    return writeFully(stream, hold, sizeof(hold));
}

bool stateLoad(const clap_plugin_t *plugin, const clap_istream_t *stream) {
    ChokeEngine &engine = pluginOf(plugin).engine;

    uint8_t header[8];
    if (!readFully(stream, header, sizeof(header)))
        return false;
    if (std::memcmp(header, kStateMagic, 4) != 0)
        return false;
    const int32_t version = readLittleEndian32(header + 4);
    if (version < 1 || version > kStateVersion)
        return false;

    uint8_t groups[kChannelCount * 4];
    if (!readFully(stream, groups, sizeof(groups)))
        return false;
    for (uint8_t channel = 0; channel < kChannelCount; ++channel)
        engine.setGroup(channel, static_cast<double>(readLittleEndian32(groups + channel * 4)));

    if (version >= 2) {
        uint8_t hold[4];
        if (!readFully(stream, hold, sizeof(hold)))
            return false;
        engine.setHoldMilliseconds(static_cast<double>(readLittleEndian32(hold)));
    } else {
        engine.setHoldMilliseconds(0);
    }
    return true;
}

const clap_plugin_state_t stateExtension = {stateSave, stateLoad};

bool pluginInit(const clap_plugin_t *) { return true; }

void pluginDestroy(const clap_plugin_t *plugin) {
    delete static_cast<Plugin *>(plugin->plugin_data);
}

bool pluginActivate(const clap_plugin_t *plugin, double sampleRate, uint32_t, uint32_t) {
    ChokeEngine &engine = pluginOf(plugin).engine;
    engine.setSampleRate(sampleRate);
    engine.resetNotes();
    MCG_LOG("activate sampleRate=%.1f (debug log active)\n", sampleRate);
    return true;
}

void pluginDeactivate(const clap_plugin_t *) {}
bool pluginStartProcessing(const clap_plugin_t *) { return true; }
void pluginStopProcessing(const clap_plugin_t *) {}

void pluginReset(const clap_plugin_t *plugin) {
    pluginOf(plugin).engine.resetNotes();
}

void pluginOnMainThread(const clap_plugin_t *) {}

clap_process_status pluginProcess(const clap_plugin_t *plugin, const clap_process_t *process) {
    ChokeEngine &engine = pluginOf(plugin).engine;
    const clap_input_events_t  *in  = process->in_events;
    const clap_output_events_t *out = process->out_events;
    const uint32_t count = in->size(in);
    const int64_t blockStart = engine.currentSample();

    for (uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t *header = in->get(in, i);
        if (header->space_id == CLAP_CORE_EVENT_SPACE_ID) {
            if (header->type == CLAP_EVENT_PARAM_VALUE) {
                applyParameterEvent(engine,
                                    reinterpret_cast<const clap_event_param_value_t *>(header));
                continue;
            }
            if (header->type == CLAP_EVENT_MIDI_SYSEX)
                continue; // variable-length payload cannot be safely delayed
        }
        engine.enqueue(header, blockStart + header->time);
    }

    engine.drain(out, blockStart, process->frames_count);
    engine.advance(process->frames_count);
    return CLAP_PROCESS_CONTINUE;
}

uint32_t latencyGet(const clap_plugin_t *plugin) {
    return pluginOf(plugin).engine.latencySamples();
}

const clap_plugin_latency_t latencyExtension = {latencyGet};

const void *pluginGetExtension(const clap_plugin_t *, const char *id) {
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)  return &notePortsExtension;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPortsExtension;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)      return &paramsExtension;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)       return &stateExtension;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)     return &latencyExtension;
    return nullptr;
}

const char *const pluginFeatures[] = {
    CLAP_PLUGIN_FEATURE_NOTE_EFFECT,
    CLAP_PLUGIN_FEATURE_UTILITY,
    nullptr,
};

const clap_plugin_descriptor_t pluginDescriptor = {
    CLAP_VERSION_INIT,
    "com.loowps.midi-bouncer",
    "MIDI Bouncer",
    "loowps",
    "",
    "",
    "",
    "0.1.0",
    "Per-channel MIDI choke/mute groups.",
    pluginFeatures,
};

const clap_plugin_t *createPlugin(const clap_host_t *host) {
    auto plugin = std::make_unique<Plugin>();
    plugin->host = host;
    plugin->clap.desc             = &pluginDescriptor;
    plugin->clap.plugin_data      = plugin.get();
    plugin->clap.init             = pluginInit;
    plugin->clap.destroy          = pluginDestroy;
    plugin->clap.activate         = pluginActivate;
    plugin->clap.deactivate       = pluginDeactivate;
    plugin->clap.start_processing = pluginStartProcessing;
    plugin->clap.stop_processing  = pluginStopProcessing;
    plugin->clap.reset            = pluginReset;
    plugin->clap.process          = pluginProcess;
    plugin->clap.get_extension    = pluginGetExtension;
    plugin->clap.on_main_thread   = pluginOnMainThread;
    return &plugin.release()->clap;
}

uint32_t factoryGetPluginCount(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factoryGetPluginDescriptor(const clap_plugin_factory_t *,
                                                           uint32_t index) {
    return index == 0 ? &pluginDescriptor : nullptr;
}

const clap_plugin_t *factoryCreatePlugin(const clap_plugin_factory_t *,
                                         const clap_host_t *host, const char *pluginId) {
    if (!pluginId || std::strcmp(pluginId, pluginDescriptor.id) != 0)
        return nullptr;
    return createPlugin(host);
}

const clap_plugin_factory_t pluginFactory = {
    factoryGetPluginCount,
    factoryGetPluginDescriptor,
    factoryCreatePlugin,
};

bool entryInit(const char *) { return true; }
void entryDeinit() {}

const void *entryGetFactory(const char *factoryId) {
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &pluginFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
