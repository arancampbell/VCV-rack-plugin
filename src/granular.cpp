#include "plugin.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <cmath>

// Include for windowing function
#include "dsp/window.hpp"
#include "dr_wav.h"


// Forward-declare the module
struct Granular;

// A simple widget to display the loaded waveform
struct WaveformDisplay : rack::TransparentWidget {
    Granular* module = nullptr;
    std::shared_ptr<rack::Font> font;

    // Cache for high-fidelity waveform drawing
    std::vector<std::pair<float, float>> displayCache;
    float cacheBoxWidth = 0.f;
    size_t cacheBufferSize = 0;

    // Track previous recording state to trigger refresh on stop
    bool wasRecording = false;

    // Interaction state
    enum DragHandle { HANDLE_NONE, HANDLE_POS, HANDLE_START, HANDLE_END };
    DragHandle currentDragHandle = HANDLE_NONE;

    WaveformDisplay() {
        font = APP->window->loadFont(rack::asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

    void regenerateCache();
    void draw(const DrawArgs& args) override;

    // --- INTERACTION HANDLERS ---
    void setParamFromMouse(Vec pos, DragHandle handle);
    void onButton(const ButtonEvent& e) override;
    void onDragStart(const DragStartEvent& e) override;
    void onDragMove(const DragMoveEvent& e) override;
};

// --- Grain Struct ---
struct Grain {
    double bufferPos;
    float life;
    float lifeIncrement;
    double playbackSpeedRatio;
    float finalEnvShape;

    float getSample(const std::vector<float>& buffer) {
        if (buffer.empty()) return 0.f;
        size_t bufferSize = buffer.size();
        int index1 = (int)bufferPos;
        int index2 = (index1 + 1) % bufferSize;
        float frac = bufferPos - index1;

        if (index1 < 0) index1 = 0;
        if (index1 >= (int)bufferSize) index1 = bufferSize - 1;
        if (index2 < 0) index2 = 0;
        if (index2 >= (int)bufferSize) index2 = bufferSize - 1;

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
        // --- NEW RECORDING PARAM ---
        LIVE_REC_PARAM,
        PARAMS_LEN
    };
    enum InputId {
        _1VOCT_INPUT, // Repurposed as Audio In when Rec is active
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
        // --- NEW RECORDING LIGHT ---
        LIVE_REC_LIGHT,
        LIGHTS_LEN
    };

    std::vector<float> audioBuffer;
    unsigned int fileSampleRate = 44100;

    std::vector<Grain> grains;
    static const int MAX_GRAINS = 128;
    float grainSpawnTimer = 0.f;
    float grainSpawnPosition = 0.f;

    std::atomic<bool> isLoading{false};

    // --- Live Recording State ---
    std::atomic<bool> isRecording{false};
    size_t recHead = 0;
    dsp::SchmittTrigger recTrigger; // To detect the moment we start recording

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

        // --- Config Record Latch ---
        configParam(LIVE_REC_PARAM, 0.f, 1.f, 0.f, "Live Input Record");

        configInput(_1VOCT_INPUT, "1V/Oct Pitch / Audio In"); // Updated Label
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

        // --- RECORDING LOGIC ---
        bool recActive = params[LIVE_REC_PARAM].getValue() > 0.5f;

        // Handle Light
        lights[LIVE_REC_LIGHT].setBrightness(recActive ? 1.f : 0.f);

        // Detect start of recording to allocate buffer if empty
        if (recTrigger.process(recActive ? 10.f : 0.f)) {
            // User just hit record.
            // If buffer is empty or very small, allocate 10 seconds of space
            if (audioBuffer.size() < 44100) {
                audioBuffer.resize(args.sampleRate * 10.0f); // 10 seconds
                std::fill(audioBuffer.begin(), audioBuffer.end(), 0.f);
                fileSampleRate = args.sampleRate;
            }
            // Reset head to start (optional, creates loop pedal feel)
            recHead = 0;
        }

        isRecording = recActive;

        if (isRecording) {
            // --- LIVE RECORDING MODE ---
            // If the buffer exists, write input to it
            if (!audioBuffer.empty()) {
                float in = inputs[_1VOCT_INPUT].getVoltage();

                if (recHead < audioBuffer.size()) {
                    audioBuffer[recHead] = in;
                }

                // Increment and Wrap
                recHead++;
                if (recHead >= audioBuffer.size()) {
                    recHead = 0;
                }
            }

            // Mute output while recording
            outputs[SINE_OUTPUT].setVoltage(0.f);
            return;
        }

        // --- STANDARD PLAYBACK MODE ---

        if (isLoading || audioBuffer.empty()) {
            outputs[SINE_OUTPUT].setVoltage(0.f);
            return;
        }

        // Standard Granular DSP...
        float startVal = params[START_PARAM].getValue();
        float endVal = params[END_PARAM].getValue();
        float loopStartNorm = std::min(startVal, endVal);
        float loopEndNorm = std::max(startVal, endVal);
        double loopStartSamp = loopStartNorm * (double)(audioBuffer.size() - 1);
        double loopEndSamp = loopEndNorm * (double)(audioBuffer.size() - 1);

        if (loopEndSamp >= audioBuffer.size()) loopEndSamp = audioBuffer.size() - 1;
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

        // --- CHANGE HERE ---
        // We forcibly ignore the input jack for pitch calculation.
        // The jack is now strictly an "Audio Recorder Input".
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

                g.bufferPos = position_final_norm * (audioBuffer.size() - 1);

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
            float sample = g.getSample(audioBuffer);
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
        fileSampleRate = newSampleRate;
        grains.clear();
        isLoading = false;
        // Ensure we aren't recording if a file is dropped
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

    size_t bufferSize = module->audioBuffer.size();
    if (bufferSize == 0) {
        displayCache.clear();
        return;
    }

    displayCache.resize(box.size.x);
    cacheBoxWidth = box.size.x;
    cacheBufferSize = bufferSize;

    float samplesPerPixel = (float)bufferSize / box.size.x;

    for (int i = 0; i < (int)box.size.x; i++) {
        size_t startSample = (size_t)(i * samplesPerPixel);
        size_t endSample = (size_t)((i + 1) * samplesPerPixel);
        if (endSample > bufferSize) endSample = bufferSize;

        float minSample = 1.f;
        float maxSample = -1.f;

        if (startSample >= endSample) {
            if (startSample < bufferSize) {
                minSample = maxSample = module->audioBuffer[startSample];
            } else {
                minSample = maxSample = 0.f;
            }
        } else {
            for (size_t j = startSample; j < endSample; j++) {
                float sample = module->audioBuffer[j];
                if (sample < minSample) minSample = sample;
                if (sample > maxSample) maxSample = sample;
            }
        }
        displayCache[i] = {minSample, maxSample};
    }
}


void WaveformDisplay::draw(const DrawArgs& args) {
    // Draw background
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillColor(args.vg, nvgRGBA(20, 20, 20, 255));
    nvgFill(args.vg);

    if (!module) return;

    // --- CHECK FOR RECORDING STATE ---
    bool isRec = module->isRecording;

    // If we just finished recording, force a cache regeneration to show new waveform
    if (wasRecording && !isRec) {
        regenerateCache();
    }
    wasRecording = isRec;

    // If recording, show text instead of waveform (Thread Safety)
    if (isRec) {
        nvgFontSize(args.vg, 18);
        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(255, 50, 50, 255));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, "RECORDING...", NULL);
        return;
    }

    size_t bufferSize = module->audioBuffer.size();
    if (bufferSize != cacheBufferSize || box.size.x != cacheBoxWidth) {
        regenerateCache();
    }

    if (displayCache.empty()) {
        nvgFontSize(args.vg, 14);
        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 100));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, "Drop WAV or REC", NULL);
        return;
    }

    // --- Draw WAV (darkened) ---
    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(100, 100, 100, 100));
    nvgStrokeWidth(args.vg, 1.f);
    for (int i = 0; i < (int)displayCache.size(); i++) {
        float minSample = displayCache[i].first;
        float maxSample = displayCache[i].second;
        float y_min = box.size.y - ((minSample + 1.f) / 2.f) * box.size.y;
        float y_max = box.size.y - ((maxSample + 1.f) / 2.f) * box.size.y;
        nvgMoveTo(args.vg, i + 0.5f, y_min);
        nvgLineTo(args.vg, i + 0.5f, y_max);
    }
    nvgStroke(args.vg);

    // --- Draw Active Loop Region (Bright Green) ---
    float startX_norm = module->params[Granular::START_PARAM].getValue();
    float endX_norm = module->params[Granular::END_PARAM].getValue();
    float effectiveStartX_norm = std::min(startX_norm, endX_norm);
    float effectiveEndX_norm = std::max(startX_norm, endX_norm);
    int startPixel = (int)(effectiveStartX_norm * box.size.x);
    int endPixel = (int)(effectiveEndX_norm * box.size.x);

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(0, 255, 100, 255));
    nvgStrokeWidth(args.vg, 1.f);
    for (int i = startPixel; i < endPixel && i < (int)displayCache.size(); i++) {
        float minSample = displayCache[i].first;
        float maxSample = displayCache[i].second;
        float y_min = box.size.y - ((minSample + 1.f) / 2.f) * box.size.y;
        float y_max = box.size.y - ((maxSample + 1.f) / 2.f) * box.size.y;
        nvgMoveTo(args.vg, i + 0.5f, y_min);
        nvgLineTo(args.vg, i + 0.5f, y_max);
    }
    nvgStroke(args.vg);

    // --- Draw Grains ---
    std::vector<Grain>& activeGrains = module->grains;
    nvgStrokeColor(args.vg, nvgRGBA(0, 150, 255, 255));
    nvgStrokeWidth(args.vg, 1.5f);
    for (const Grain& grain : activeGrains) {
        double wrappedBufferPos = std::fmod(grain.bufferPos, (double)bufferSize);
        float grainX = (float)(wrappedBufferPos / bufferSize) * box.size.x;
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, grainX, 0);
        nvgLineTo(args.vg, grainX, box.size.y);
        nvgStroke(args.vg);
    }

    // --- Draw Loop Lines (White) ---
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

    // --- Draw Playhead (RED) ---
    float spawnX = module->grainSpawnPosition * box.size.x;
    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 0, 0, 200));
    nvgStrokeWidth(args.vg, 2.0f);
    nvgMoveTo(args.vg, spawnX, 0);
    nvgLineTo(args.vg, spawnX, box.size.y);
    nvgStroke(args.vg);
}


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

        // --- NEW INPUT: 1V/Oct / Audio In + RECORD BUTTON ---
        // Placing the button slightly above the input jack
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(
            mm2px(Vec(184.573, 67.0)), // Adjusted Y position above input
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

                // Turn off recording if a file is dropped
                granularModule->params[Granular::LIVE_REC_PARAM].setValue(0.f);

                if (display) {
                    display->cacheBufferSize = 0;
                }
            }
        }
    }
};


Model* modelGranular = createModel<Granular, GranularWidget>("granular");