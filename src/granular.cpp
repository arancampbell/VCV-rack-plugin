#include "plugin.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <cmath>
#include <algorithm> // For std::max, std::min

#include "dsp/window.hpp"
#include "dr_wav.h"

struct Granular;

struct WaveformDisplay : rack::TransparentWidget {
    Granular* module = nullptr;
    std::shared_ptr<rack::Font> font;

    std::vector<std::pair<float, float>> displayCache;
    std::vector<NVGcolor> displayColorCache; // Store colors per pixel column
    float cacheBoxWidth = 0.f;
    size_t cacheBufferSize = 0;

    // Track previous recording state to trigger zoom-snap on stop
    bool wasRecording = false;

    enum DragHandle { HANDLE_NONE, HANDLE_POS, HANDLE_START, HANDLE_END };
    DragHandle currentDragHandle = HANDLE_NONE;

    WaveformDisplay() {
        font = APP->window->loadFont(rack::asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

    // Helper: Interpolate between two colors
    NVGcolor lerpColor(NVGcolor c1, NVGcolor c2, float t) {
        t = rack::math::clamp(t, 0.f, 1.f);
        return nvgRGBAf(
            c1.r + (c2.r - c1.r) * t,
            c1.g + (c2.g - c1.g) * t,
            c1.b + (c2.b - c1.b) * t,
            1.0f
        );
    }

    // Helper: Map frequency to specific spectrum gradient
    // 0hz(Dark Red) -> 150hz(Red) -> 200hz(Orange) -> 350hz(Green) -> 1000hz(Cyan) -> 5000hz(Blue) -> 15000hz+(Dark Blue)
    NVGcolor getFreqColor(float freq) {
        // 0Hz (Dark Red) -> 150Hz (Red)
        if (freq < 150.f) {
            float t = freq / 150.f;
            return lerpColor(nvgRGB(100, 0, 0), nvgRGB(255, 0, 0), t);
        }
        // 150Hz (Red) -> 200Hz (Orange)
        else if (freq < 200.f) {
            float t = (freq - 150.f) / (200.f - 150.f);
            return lerpColor(nvgRGB(255, 0, 0), nvgRGB(255, 165, 0), t);
        }
        // 200Hz (Orange) -> 350Hz (Green)
        else if (freq < 350.f) {
            float t = (freq - 200.f) / (350.f - 200.f);
            return lerpColor(nvgRGB(255, 165, 0), nvgRGB(0, 255, 0), t);
        }
        // 350Hz (Green) -> 1000Hz (Cyan)
        else if (freq < 1000.f) {
            float t = (freq - 350.f) / (1000.f - 350.f);
            return lerpColor(nvgRGB(0, 255, 0), nvgRGB(0, 255, 255), t);
        }
        // 1000Hz (Cyan) -> 5000Hz (Blue)
        else if (freq < 5000.f) {
            float t = (freq - 1000.f) / (5000.f - 1000.f);
            return lerpColor(nvgRGB(0, 255, 255), nvgRGB(0, 0, 255), t);
        }
        // 5000Hz (Blue) -> 15000Hz (Dark Blue)
        else if (freq < 15000.f) {
            float t = (freq - 5000.f) / (15000.f - 5000.f);
            return lerpColor(nvgRGB(0, 0, 255), nvgRGB(0, 0, 100), t);
        }
        // > 15000Hz (Dark Blue)
        else {
            return nvgRGB(0, 0, 100);
        }
    }

    void regenerateCache();
    void draw(const DrawArgs& args) override;
    void setParamFromMouse(Vec pos, DragHandle handle);
    void onButton(const ButtonEvent& e) override;
    void onDragStart(const DragStartEvent& e) override;
    void onDragMove(const DragMoveEvent& e) override;
};

struct Grain {
    double bufferPos;
    float life;
    float lifeIncrement;
    double playbackSpeedRatio;
    float finalEnvShape;

    float getSample(const std::vector<float>& buffer, size_t activeLen) {
        if (buffer.empty() || activeLen == 0) return 0.f;

        size_t effectiveSize = activeLen;

        int index1 = (int)bufferPos;
        int index2 = (index1 + 1) % effectiveSize;
        float frac = bufferPos - index1;

        if (index1 < 0) index1 = 0;
        if (index1 >= (int)effectiveSize) index1 = effectiveSize - 1;
        if (index2 < 0) index2 = 0;
        if (index2 >= (int)effectiveSize) index2 = effectiveSize - 1;

        float s1 = buffer[index1];
        float s2 = buffer[index2];
        return (1.f - frac) * s1 + frac * s2;
    }

    float getEnvelope(float envShape) {
        float shape_square = 1.f;
        float shape_triangle = 1.f - (std::abs(life - 0.5f) * 2.f);
        float shape_sine = 0.5f * (1.f - std::cos(2.f * M_PI * life));

        if (envShape <= 0.5f) {
            float t = envShape * 2.f;
            return (1.f - t) * shape_square + t * shape_triangle;
        } else {
            float t = (envShape - 0.5f) * 2.f;
            return (1.f - t) * shape_triangle + t * shape_sine;
        }
    }

    void advance(double loopStart, double loopEnd) {
        bufferPos += playbackSpeedRatio;
        if (bufferPos >= loopEnd) {
            double overflow = bufferPos - loopEnd;
            double loopWidth = loopEnd - loopStart;
            if (loopWidth > 0.00001) {
                 bufferPos = loopStart + std::fmod(overflow, loopWidth);
            } else {
                 bufferPos = loopStart;
            }
        }
        else if (bufferPos < loopStart) {
            bufferPos = loopStart;
        }
        life += lifeIncrement;
    }

    bool isAlive() { return life < 1.f; }
};


struct Granular : Module {
    enum ParamId {
        COMPRESSION_PARAM,
        SIZE_PARAM,
        DENSITY_PARAM,
        ENV_SHAPE_PARAM,
        POSITION_PARAM,
        PITCH_PARAM,
        R_SIZE_PARAM,
        R_DENSITY_PARAM,
        R_ENV_SHAPE_PARAM,
        R_POSITION_PARAM,
        R_PITCH_PARAM,
        M_SIZE_PARAM,
        M_DENSITY_PARAM,
        M_AMOUNT_ENV_SHAPE_PARAM,
        M_AMOUNT_POSITION_PARAM,
        M_AMOUNT_PITCH_PARAM,
        START_PARAM,
        END_PARAM,
        LIVE_REC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        _1VOCT_INPUT,
        M_SIZE_INPUT,
        M_DENSITY_INPUT,
        M_ENV_SHAPE_INPUT,
        M_POSITION_INPUT,
        M_PITCH_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SINE_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        BLINK_LIGHT,
        LIVE_REC_LIGHT,
        LIGHTS_LEN
    };

    std::vector<float> audioBuffer;
    unsigned int fileSampleRate = 44100;

    // Logical size of audio (Recorded length OR File length)
    size_t activeBufferLen = 0;

    // <--- CHANGED: Added flag to track if buffer contains raw 5V audio or normalized WAV data
    bool bufferIsRawVoltage = false;

    std::vector<Grain> grains;
    static const int MAX_GRAINS = 128;
    float grainSpawnTimer = 0.f;
    float grainSpawnPosition = 0.f;

    std::atomic<bool> isLoading{false};
    std::atomic<bool> isRecording{false};

    // Make recHead public so Display can see it
    size_t recHead = 0;

    bool wasRecordingPrev = false;
    bool bufferWrapped = false;
    dsp::SchmittTrigger recTrigger;

    float getClampedRandomizedValue(float base_0_to_1, float r_knob_0_to_1) {
        float max_deviation = r_knob_0_to_1 * 0.5f;
        float random_offset = (rack::random::uniform() * 2.f - 1.f) * max_deviation;
        return rack::math::clamp(base_0_to_1 + random_offset, 0.f, 1.f);
    }

    Granular() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(COMPRESSION_PARAM, 0.f, 1.f, 0.f, "Compression / Drive");
        configParam(SIZE_PARAM, 0.01f, 2.0f, 0.5f, "Grain Size", " s");
        configParam(DENSITY_PARAM, 1.f, 100.f, 1.f, "Grain Density", " Hz");
        configParam(ENV_SHAPE_PARAM, 0.f, 1.f, 0.5f, "Envelope Shape");
        configParam(POSITION_PARAM, 0.f, 1.f, 0.f, "Position");
        configParam(PITCH_PARAM, 0.f, 1.f, 0.5f, "Pitch Offset");
        configParam(R_SIZE_PARAM, 0.f, 1.f, 0.f, "Randomise Size");
        configParam(R_DENSITY_PARAM, 0.f, 1.f, 0.f, "Randomise Density");
        configParam(R_ENV_SHAPE_PARAM, 0.f, 1.f, 0.f, "Randomise Shape");
        configParam(R_POSITION_PARAM, 0.f, 1.f, 0.f, "Randomise Position");
        configParam(R_PITCH_PARAM, 0.f, 1.f, 0.f, "Randomise Pitch");
        configParam(M_SIZE_PARAM, -1.f, 1.f, 0.f, "Size Mod Amount", "%", 0.f, 100.f);
        configParam(M_DENSITY_PARAM, -1.f, 1.f, 0.f, "Density Mod Amount", "%", 0.f, 100.f);
        configParam(M_AMOUNT_ENV_SHAPE_PARAM, -1.f, 1.f, 0.f, "Shape Mod Amount", "%", 0.f, 100.f);
        configParam(M_AMOUNT_POSITION_PARAM, -1.f, 1.f, 0.f, "Position Mod Amount", "%", 0.f, 100.f);
        configParam(M_AMOUNT_PITCH_PARAM, -1.f, 1.f, 0.f, "Pitch Mod Amount", "%", 0.f, 100.f);
        configParam(START_PARAM, 0.f, 1.f, 0.f, "Loop Start");
        configParam(END_PARAM, 0.f, 1.f, 1.f, "Loop End");
        configParam(LIVE_REC_PARAM, 0.f, 1.f, 0.f, "Live Input Record");

        configInput(_1VOCT_INPUT, "1V/Oct Pitch / Audio In");
        configInput(M_SIZE_INPUT, "Size Mod CV");
        configInput(M_DENSITY_INPUT, "Density Mod CV");
        configInput(M_ENV_SHAPE_INPUT, "Shape Mod CV");
        configInput(M_POSITION_INPUT, "Position Mod CV");
        configInput(M_PITCH_INPUT, "Pitch Mod CV");
        configOutput(SINE_OUTPUT, "Audio Output");

        grains.reserve(MAX_GRAINS);
    }

    void process(const ProcessArgs& args) override {
        lights[BLINK_LIGHT].setBrightness(isLoading);

        bool recActive = params[LIVE_REC_PARAM].getValue() > 0.5f;
        lights[LIVE_REC_LIGHT].setBrightness(recActive ? 1.f : 0.f);

        // --- TRIGGER RECORD START ---
        if (recTrigger.process(recActive ? 10.f : 0.f)) {
            isLoading = true;

            // Allocate 10s if buffer is small
            if (audioBuffer.size() < 44100) {
                audioBuffer.resize(args.sampleRate * 10.0f);
                fileSampleRate = args.sampleRate;
            }

            // --- FIX 1: CLEAR BUFFER ON START TO PREVENT GHOSTS ---
            std::fill(audioBuffer.begin(), audioBuffer.end(), 0.f);

            // <--- CHANGED: Mark buffer as raw voltage (requires scaling in display)
            bufferIsRawVoltage = true;

            recHead = 0;
            bufferWrapped = false;
            isLoading = false;
        }

        // --- HANDLE RECORD STOP (Trimming logic) ---
        if (wasRecordingPrev && !recActive) {
            if (bufferWrapped) {
                activeBufferLen = audioBuffer.size();
            } else {
                activeBufferLen = (recHead > 100) ? recHead : 44100;
            }
        }
        wasRecordingPrev = recActive;
        isRecording = recActive;

        if (isRecording) {
            if (!audioBuffer.empty()) {
                float in = inputs[_1VOCT_INPUT].getVoltage();

                if (recHead < audioBuffer.size()) {
                    audioBuffer[recHead] = in;
                }

                recHead++;
                if (recHead >= audioBuffer.size()) {
                    recHead = 0;
                    bufferWrapped = true;
                }

                activeBufferLen = audioBuffer.size();
            }
            outputs[SINE_OUTPUT].setVoltage(0.f);
            return;
        }

        if (isLoading || audioBuffer.empty() || activeBufferLen == 0) {
            outputs[SINE_OUTPUT].setVoltage(0.f);
            return;
        }

        // --- STANDARD PLAYBACK (Using activeBufferLen) ---

        float startVal = params[START_PARAM].getValue();
        float endVal = params[END_PARAM].getValue();
        float loopStartNorm = std::min(startVal, endVal);
        float loopEndNorm = std::max(startVal, endVal);

        double loopStartSamp = loopStartNorm * (double)(activeBufferLen - 1);
        double loopEndSamp = loopEndNorm * (double)(activeBufferLen - 1);

        if (loopEndSamp >= activeBufferLen) loopEndSamp = activeBufferLen - 1;
        if (loopStartSamp < 0) loopStartSamp = 0;
        if (loopStartSamp >= loopEndSamp) loopStartSamp = loopEndSamp - 1;

        float density_raw = params[DENSITY_PARAM].getValue();
        float density_norm = rack::math::rescale(density_raw, 1.f, 100.f, 0.f, 1.f);
        float density_mod_amount = params[M_DENSITY_PARAM].getValue();
        density_norm += inputs[M_DENSITY_INPUT].getVoltage() * density_mod_amount * 0.1f;
        density_norm = rack::math::clamp(density_norm, 0.f, 1.f);
        float density_hz_base = rack::math::rescale(density_norm, 0.f, 1.f, 1.f, 100.f);

        float size_raw = params[SIZE_PARAM].getValue();
        float size_norm = rack::math::rescale(size_raw, 0.01f, 2.0f, 0.f, 1.f);
        float size_mod_amount = params[M_SIZE_PARAM].getValue();
        size_norm += inputs[M_SIZE_INPUT].getVoltage() * size_mod_amount * 0.1f;
        size_norm = rack::math::clamp(size_norm, 0.f, 1.f);
        float grainSize_sec_base = rack::math::rescale(size_norm, 0.f, 1.f, 0.01f, 2.0f);

        float envShape_base = params[ENV_SHAPE_PARAM].getValue();
        float shape_mod_amount = params[M_AMOUNT_ENV_SHAPE_PARAM].getValue();
        envShape_base += inputs[M_ENV_SHAPE_INPUT].getVoltage() * shape_mod_amount * 0.1f;
        envShape_base = rack::math::clamp(envShape_base, 0.f, 1.f);

        grainSpawnPosition = params[POSITION_PARAM].getValue();
        float pos_mod_amount = params[M_AMOUNT_POSITION_PARAM].getValue();
        grainSpawnPosition += inputs[M_POSITION_INPUT].getVoltage() * pos_mod_amount * 0.1f;
        grainSpawnPosition = rack::math::clamp(grainSpawnPosition, 0.f, 1.f);

        float pitchKnob = params[PITCH_PARAM].getValue();
        float pitch_mod_amount = params[M_AMOUNT_PITCH_PARAM].getValue();
        pitchKnob += inputs[M_PITCH_INPUT].getVoltage() * pitch_mod_amount * 0.1f;
        pitchKnob = rack::math::clamp(pitchKnob, 0.f, 1.f);

        float pitchOffsetOctaves = (pitchKnob - 0.5f) * 4.f;
        float pitchCV = 0.f;
        float basePitchVolts = pitchCV + pitchOffsetOctaves;

        float density_base_0_to_1 = density_norm;
        float size_base_0_to_1 = size_norm;

        float r_density_knob = params[R_DENSITY_PARAM].getValue();
        float r_size_knob = params[R_SIZE_PARAM].getValue();
        float r_envShape_knob = params[R_ENV_SHAPE_PARAM].getValue();
        float r_position_knob = params[R_POSITION_PARAM].getValue();
        float r_pitch_knob = params[R_PITCH_PARAM].getValue();

        float compression_amount = params[COMPRESSION_PARAM].getValue();

        grainSpawnTimer -= args.sampleTime;
        if (grainSpawnTimer <= 0.f) {
            float density_final_0_to_1 = getClampedRandomizedValue(density_base_0_to_1, r_density_knob);
            float density_hz = rack::math::rescale(density_final_0_to_1, 0.f, 1.f, 1.f, 100.f);
            grainSpawnTimer = 1.f / density_hz;

            if (grains.size() < MAX_GRAINS) {
                Grain g;
                float position_final_norm = getClampedRandomizedValue(grainSpawnPosition, r_position_knob);
                if (position_final_norm < loopStartNorm) position_final_norm = loopStartNorm;
                if (position_final_norm > loopEndNorm) position_final_norm = loopEndNorm;

                g.bufferPos = position_final_norm * (activeBufferLen - 1);

                float maxRandomOctaves = r_pitch_knob * 1.f;
                float randomOctaveOffset = (rack::random::uniform() * 2.f - 1.f) * maxRandomOctaves;

                float totalPitchVolts = basePitchVolts + randomOctaveOffset;
                g.playbackSpeedRatio = std::pow(2.f, totalPitchVolts);

                float size_final_0_to_1 = getClampedRandomizedValue(size_base_0_to_1, r_size_knob);
                float grainSize_sec = rack::math::rescale(size_final_0_to_1, 0.f, 1.f, 0.01f, 2.0f);

                g.finalEnvShape = getClampedRandomizedValue(envShape_base, r_envShape_knob);
                g.life = 0.f;
                float grainSizeInSamples = grainSize_sec * fileSampleRate;
                if (grainSizeInSamples < 1.f) grainSizeInSamples = 1.f;
                g.lifeIncrement = 1.f / grainSizeInSamples;

                grains.push_back(g);
            }
        }

        float out = 0.f;
        for (size_t i = 0; i < grains.size(); ++i) {
            Grain& g = grains[i];
            float sample = g.getSample(audioBuffer, activeBufferLen);
            float env = g.getEnvelope(g.finalEnvShape);
            out += sample * env;
            g.advance(loopStartSamp, loopEndSamp);
        }

        for (int i = grains.size() - 1; i >= 0; i--) {
            if (!grains[i].isAlive()) {
                grains.erase(grains.begin() + i);
            }
        }

        if (!grains.empty()) {
            out /= std::sqrt(grains.size());
        }

        float makeupGain = 1.0f + (compression_amount * 3.0f);
        out *= makeupGain;
        out = 5.0f * std::tanh(out);

        outputs[SINE_OUTPUT].setVoltage(out);
    }


    void setBuffer(std::vector<float> newBuffer, unsigned int newSampleRate) {
        audioBuffer = newBuffer;
        activeBufferLen = audioBuffer.size();
        fileSampleRate = newSampleRate;
        grains.clear();

        // <--- CHANGED: Reset raw voltage flag because WAVs are usually normalized
        bufferIsRawVoltage = false;

        isLoading = false;
        isRecording = false;
    }
};

// --- WaveformDisplay Implementation ---

void WaveformDisplay::setParamFromMouse(Vec pos, DragHandle handle) {
    if (!module || box.size.x <= 0.f) return;
    float newPos = pos.x / box.size.x;
    newPos = rack::math::clamp(newPos, 0.f, 1.f);

    if (handle == HANDLE_POS) {
        module->params[Granular::POSITION_PARAM].setValue(newPos);
    } else if (handle == HANDLE_START) {
        module->params[Granular::START_PARAM].setValue(newPos);
    } else if (handle == HANDLE_END) {
        module->params[Granular::END_PARAM].setValue(newPos);
    }
}

void WaveformDisplay::onButton(const ButtonEvent& e) {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT && e.action == GLFW_PRESS) {
        if (!module) return;
        float mouseX = e.pos.x;
        float width = box.size.x;

        float posX = module->params[Granular::POSITION_PARAM].getValue() * width;
        float startX = module->params[Granular::START_PARAM].getValue() * width;
        float endX = module->params[Granular::END_PARAM].getValue() * width;

        float threshold = 10.0f;
        float distPos = std::abs(mouseX - posX);
        float distStart = std::abs(mouseX - startX);
        float distEnd = std::abs(mouseX - endX);

        if (distStart < threshold && distStart < distEnd) {
            currentDragHandle = HANDLE_START;
        } else if (distEnd < threshold) {
            currentDragHandle = HANDLE_END;
        } else if (distPos < threshold) {
            currentDragHandle = HANDLE_POS;
        } else {
            currentDragHandle = HANDLE_POS;
            setParamFromMouse(e.pos, HANDLE_POS);
        }
        e.consume(this);
    }
}

void WaveformDisplay::onDragStart(const DragStartEvent& e) {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
        e.consume(this);
    }
}

void WaveformDisplay::onDragMove(const DragMoveEvent& e) {
    if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
        Vec localPos = APP->scene->mousePos - getAbsoluteOffset(Vec(0, 0));
        setParamFromMouse(localPos, currentDragHandle);
        e.consume(this);
    }
}


void WaveformDisplay::regenerateCache() {
    if (!module || box.size.x <= 0) return;

    size_t targetLen = module->isRecording ? module->audioBuffer.size() : module->activeBufferLen;

    if (targetLen == 0 || targetLen > module->audioBuffer.size()) {
        displayCache.clear();
        displayColorCache.clear();
        return;
    }

    displayCache.resize(box.size.x);
    displayColorCache.resize(box.size.x);
    cacheBoxWidth = box.size.x;
    cacheBufferSize = targetLen;

    float samplesPerPixel = (float)targetLen / box.size.x;

    for (int i = 0; i < (int)box.size.x; i++) {
        size_t startSample = (size_t)(i * samplesPerPixel);
        size_t endSample = (size_t)((i + 1) * samplesPerPixel);
        if (endSample > targetLen) endSample = targetLen;

        float minSample = 100.0f;
        float maxSample = -100.0f;

        // --- Zero Crossing Frequency Estimation ---
        int crossings = 0;
        float prev = 0.f;

        // Initialize prev for ZCR check
        if (startSample < module->audioBuffer.size()) {
             if (startSample > 0) prev = module->audioBuffer[startSample - 1];
             else prev = module->audioBuffer[startSample];
        }

        if (startSample >= endSample) {
            if (startSample < module->audioBuffer.size()) {
                 minSample = maxSample = module->audioBuffer[startSample];
            } else {
                 minSample = maxSample = 0.f;
            }
        } else {
            for (size_t j = startSample; j < endSample; j++) {
                if (j >= module->audioBuffer.size()) break;
                float sample = module->audioBuffer[j];

                // Count Zero Crossings (Simple sign change detection)
                if ((sample >= 0 && prev < 0) || (sample < 0 && prev >= 0)) {
                    crossings++;
                }
                prev = sample;

                // --- FIX 2A: SCALING FOR DISPLAY ---
                // If the buffer contains raw 5V audio (recorded), scale down to 1.0 range
                if (module->bufferIsRawVoltage) sample /= 5.0f;

                if (sample < minSample) minSample = sample;
                if (sample > maxSample) maxSample = sample;
            }
        }

        // --- FIX 2B: CLAMPING FOR DISPLAY ---
        // Force visual range to -1 to 1 to stay within box
        minSample = rack::math::clamp(minSample, -1.f, 1.f);
        maxSample = rack::math::clamp(maxSample, -1.f, 1.f);

        displayCache[i] = {minSample, maxSample};

        // Calculate Frequency from ZCR
        float duration = (float)(endSample - startSample) / (float)module->fileSampleRate;
        if (duration <= 0.00001f) duration = 1.0f; // Prevent div by zero
        float freq = (crossings / 2.0f) / duration;
        displayColorCache[i] = getFreqColor(freq);
    }
}


void WaveformDisplay::draw(const DrawArgs& args) {
    // --- FIX 3: SCISSORING (CROP) ---
    // Prevent any drawing from spilling outside the widget box
    nvgScissor(args.vg, 0, 0, box.size.x, box.size.y);

    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillColor(args.vg, nvgRGBA(20, 20, 20, 255));
    nvgFill(args.vg);

    if (!module || module->isLoading) {
        nvgResetScissor(args.vg);
        return;
    }

    bool isRec = module->isRecording;

    if (wasRecording && !isRec) {
        regenerateCache();
    }
    wasRecording = isRec;

    size_t currentLen = isRec ? module->audioBuffer.size() : module->activeBufferLen;

    if (isRec) {
        regenerateCache();
    } else {
        if (currentLen != cacheBufferSize || box.size.x != cacheBoxWidth) {
            regenerateCache();
        }
    }

    if (displayCache.empty()) {
        nvgFontSize(args.vg, 14);
        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 100));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, "Drop WAV or REC", NULL);
        nvgResetScissor(args.vg);
        return;
    }

    // --- Draw WAV (Background/Inactive) ---
    // Now drawn per-segment to allow for colored "ghosting"
    nvgStrokeWidth(args.vg, 1.f);
    for (int i = 0; i < (int)displayCache.size(); i++) {
        nvgBeginPath(args.vg);

        if (isRec) {
            nvgStrokeColor(args.vg, nvgRGBA(255, 100, 100, 150)); // Red if recording
        } else {
            NVGcolor col = displayColorCache[i];
            col.a = 0.4f; // Dim the frequency colors for inactive background
            nvgStrokeColor(args.vg, col);
        }

        float minSample = displayCache[i].first;
        float maxSample = displayCache[i].second;
        float y_min = box.size.y - ((minSample + 1.f) / 2.f) * box.size.y;
        float y_max = box.size.y - ((maxSample + 1.f) / 2.f) * box.size.y;

        nvgMoveTo(args.vg, i + 0.5f, y_min);
        nvgLineTo(args.vg, i + 0.5f, y_max);
        nvgStroke(args.vg);
    }

    // --- Draw Recording Head ---
    if (isRec && module->audioBuffer.size() > 0) {
        float recPos = (float)module->recHead / (float)module->audioBuffer.size();
        float recPixel = recPos * box.size.x;

        nvgBeginPath(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(255, 0, 0, 255));
        nvgStrokeWidth(args.vg, 1.5f);
        nvgMoveTo(args.vg, recPixel, 0);
        nvgLineTo(args.vg, recPixel, box.size.y);
        nvgStroke(args.vg);

        nvgResetScissor(args.vg);
        return;
    }

    // --- Draw Active Loop Region (Colored by Frequency) ---
    float startX_norm = module->params[Granular::START_PARAM].getValue();
    float endX_norm = module->params[Granular::END_PARAM].getValue();
    float effectiveStartX_norm = std::min(startX_norm, endX_norm);
    float effectiveEndX_norm = std::max(startX_norm, endX_norm);
    int startPixel = (int)(effectiveStartX_norm * box.size.x);
    int endPixel = (int)(effectiveEndX_norm * box.size.x);

    nvgStrokeWidth(args.vg, 1.f);
    for (int i = startPixel; i < endPixel && i < (int)displayCache.size(); i++) {
        nvgBeginPath(args.vg);

        // Use full brightness frequency color for the active region
        nvgStrokeColor(args.vg, displayColorCache[i]);

        float minSample = displayCache[i].first;
        float maxSample = displayCache[i].second;
        float y_min = box.size.y - ((minSample + 1.f) / 2.f) * box.size.y;
        float y_max = box.size.y - ((maxSample + 1.f) / 2.f) * box.size.y;

        nvgMoveTo(args.vg, i + 0.5f, y_min);
        nvgLineTo(args.vg, i + 0.5f, y_max);
        nvgStroke(args.vg);
    }

    // --- Draw Grains ---
    std::vector<Grain>& activeGrains = module->grains;
    nvgStrokeColor(args.vg, nvgRGBA(0, 150, 255, 255));
    nvgStrokeWidth(args.vg, 1.5f);
    for (const Grain& grain : activeGrains) {
        double wrappedBufferPos = std::fmod(grain.bufferPos, (double)currentLen);
        float grainX = (float)(wrappedBufferPos / currentLen) * box.size.x;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, grainX, 0);
        nvgLineTo(args.vg, grainX, box.size.y);
        nvgStroke(args.vg);
    }

    // --- Draw Loop Lines ---
    float startX = module->params[Granular::START_PARAM].getValue() * box.size.x;
    float endX = module->params[Granular::END_PARAM].getValue() * box.size.x;

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 200));
    nvgStrokeWidth(args.vg, 3.0f);
    nvgMoveTo(args.vg, startX, 0);
    nvgLineTo(args.vg, startX, box.size.y);
    nvgStroke(args.vg);

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 255, 255, 200));
    nvgStrokeWidth(args.vg, 3.0f);
    nvgMoveTo(args.vg, endX, 0);
    nvgLineTo(args.vg, endX, box.size.y);
    nvgStroke(args.vg);

    // --- Draw Playhead ---
    float spawnX = module->grainSpawnPosition * box.size.x;
    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 0, 0, 200));
    nvgStrokeWidth(args.vg, 2.0f);
    nvgMoveTo(args.vg, spawnX, 0);
    nvgLineTo(args.vg, spawnX, box.size.y);
    nvgStroke(args.vg);

    // --- RESET SCISSOR ---
    nvgResetScissor(args.vg);
}


// --- ADDED: Small visualizer for Envelope Shape ---
struct ShapeDisplay : rack::TransparentWidget {
    Granular* module = nullptr;

    void draw(const DrawArgs& args) override {
        if (!module || box.size.x <= 0.f) return;

        // Calculate effective shape (knob + modulation)
        float envShape = module->params[Granular::ENV_SHAPE_PARAM].getValue();

        // Add modulation visualization if you want it "live"
        if (module->inputs[Granular::M_ENV_SHAPE_INPUT].isConnected()) {
             float shape_mod_amount = module->params[Granular::M_AMOUNT_ENV_SHAPE_PARAM].getValue();
             envShape += module->inputs[Granular::M_ENV_SHAPE_INPUT].getVoltage() * shape_mod_amount * 0.1f;
             envShape = rack::math::clamp(envShape, 0.f, 1.f);
        }

        nvgBeginPath(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0, 0, 0, 255)); // Black line
        nvgStrokeWidth(args.vg, 1.5f);

        // Start at bottom-left corner to draw the "walls" of the shape
        nvgMoveTo(args.vg, 0, box.size.y);

        // Draw curve across width of box
        // Use ceil to ensure we iterate enough times to cover the fractional width
        int limit = (int)std::ceil(box.size.x);
        for (int i = 0; i <= limit; i++) {
            // Clamp x to the actual box width so the last point is exactly on the edge
            float x = (i < box.size.x) ? (float)i : box.size.x;

            // Normalize life based on the clamped x
            float life = x / box.size.x;

            // Envelope Math (Must match Grain::getEnvelope)
            float shape_square = 1.f;
            float shape_triangle = 1.f - (std::abs(life - 0.5f) * 2.f);
            float shape_sine = 0.5f * (1.f - std::cos(2.f * M_PI * life));

            float val = 0.f;
            if (envShape <= 0.5f) {
                float t = envShape * 2.f;
                val = (1.f - t) * shape_square + t * shape_triangle;
            } else {
                float t = (envShape - 0.5f) * 2.f;
                val = (1.f - t) * shape_triangle + t * shape_sine;
            }

            // Map 0..1 value to box height (invert Y because 0 is top)
            float y = box.size.y - (val * box.size.y);

            nvgLineTo(args.vg, x, y);
        }

        // End at bottom-right corner to close the shape visually
        nvgLineTo(args.vg, box.size.x, box.size.y);

        nvgStroke(args.vg);
    }
};


struct GranularWidget : ModuleWidget {
    WaveformDisplay* display = nullptr;

    GranularWidget(Granular* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/granular.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // COMPRESSION_PARAM
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(184.573, 46.063)), module, Granular::COMPRESSION_PARAM));

        // MAIN ROW
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.0, 87.175)), module, Granular::SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80.0, 87.175)), module, Granular::DENSITY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(105.0, 87.175)), module, Granular::ENV_SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(130.0, 87.175)), module, Granular::POSITION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(155.0, 87.175)), module, Granular::PITCH_PARAM));

        // RANDOM ROW
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(55.0, 100.964)), module, Granular::R_SIZE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(80.0, 100.964)), module, Granular::R_DENSITY_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(105.0, 100.964)), module, Granular::R_ENV_SHAPE_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(130.0, 100.964)), module, Granular::R_POSITION_PARAM));
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(155.0, 100.964)), module, Granular::R_PITCH_PARAM));

        // MODULATION AMOUNT KNOBS
        addParam(createParamCentered<Trimpot>(mm2px(Vec(55.0, 114.0)), module, Granular::M_SIZE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(80.0, 114.0)), module, Granular::M_DENSITY_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(105.0, 114.0)), module, Granular::M_AMOUNT_ENV_SHAPE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(130.0, 114.0)), module, Granular::M_AMOUNT_POSITION_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(155.0, 114.0)), module, Granular::M_AMOUNT_PITCH_PARAM));

        // INPUT: 1V/Oct / Audio In + RECORD BUTTON
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
            mm2px(Vec(184.573, 67.0)),
            module,
            Granular::LIVE_REC_PARAM,
            Granular::LIVE_REC_LIGHT
        ));

        // _1VOCT_INPUT (Shared with Audio Rec)
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(184.573, 77.478)), module, Granular::_1VOCT_INPUT));

        // Modulation Inputs
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(62.938, 113.822)), module, Granular::M_SIZE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(87.937, 113.822)), module, Granular::M_DENSITY_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(112.937, 113.822)), module, Granular::M_ENV_SHAPE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(137.937, 113.822)), module, Granular::M_POSITION_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(162.937, 113.822)), module, Granular::M_PITCH_INPUT));

        // OUTPUT
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(184.573, 108.713)), module, Granular::SINE_OUTPUT));

        // LIGHT
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(184.573, 30.224)), module, Granular::BLINK_LIGHT));

        // WAVEFORM DISPLAY
        display = new WaveformDisplay();
        display->module = module;
        display->box.pos = mm2px(Vec(20.0, 30.0));
        display->box.size = mm2px(Vec(150, 45));
        addChild(display);

        // --- ADDED: Shape Display Widget ---
        ShapeDisplay* shapeDisplay = new ShapeDisplay();
        shapeDisplay->module = module;
        // Placed centered above the Shape Knob (x=105)
        shapeDisplay->box.pos = mm2px(Vec(113, 85));
        shapeDisplay->box.size = mm2px(Vec(6, 4)); // made smaller whilst maintaining original 3:2 ratio (prev 15, 10)
        addChild(shapeDisplay);
    }

    void onPathDrop(const PathDropEvent& e) override {
        if (e.paths.empty()) return;

        std::string path = e.paths[0];
        std::string extension = rack::system::getExtension(path);

        if (extension == ".wav") {
            if (module) {
                Granular* granularModule = dynamic_cast<Granular*>(module);
                if (!granularModule) return;

                granularModule->isLoading = true;

                unsigned int channels;
                unsigned int sampleRate;
                drwav_uint64 totalFrames;
                float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sampleRate, &totalFrames, NULL);

                if (pSampleData == NULL) {
                    granularModule->isLoading = false;
                    return;
                }

                std::vector<float> newBuffer;
                newBuffer.resize(totalFrames);

                if (channels == 1) {
                    std::memcpy(newBuffer.data(), pSampleData, totalFrames * sizeof(float));
                } else {
                    for (drwav_uint64 i = 0; i < totalFrames; i++) {
                        newBuffer[i] = (pSampleData[i * channels + 0] + pSampleData[i * channels + 1]) * 0.5f;
                    }
                }

                drwav_free(pSampleData, NULL);
                granularModule->setBuffer(newBuffer, sampleRate);

                granularModule->params[Granular::LIVE_REC_PARAM].setValue(0.f);

                if (display) {
                    display->cacheBufferSize = 0;
                }
            }
        }
    }
};


Model* modelGranular = createModel<Granular, GranularWidget>("granular");