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
    size_t cacheBufferSize = 0; // Detect when a new file is loaded

    WaveformDisplay() {
        font = APP->window->loadFont(rack::asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

    // DECLARE the function here, move the definition after Granular
    void regenerateCache();

    // DECLARE the draw function here
    void draw(const DrawArgs& args) override;
};

// --- Grain Struct ---
// Defines a single grain of audio
struct Grain {
    double bufferPos;     // Current playback position in the audioBuffer
    float life;           // Current position in the envelope (0.0 to 1.0)
    float lifeIncrement;  // How much to advance life per sample

    // Simple linear interpolation
    float getSample(const std::vector<float>& buffer) {
        if (buffer.empty()) return 0.f;
        size_t bufferSize = buffer.size();
        int index1 = (int)bufferPos;
        // Use modulo to wrap the read position
        int index2 = (index1 + 1) % bufferSize;
        float frac = bufferPos - index1;
        float s1 = buffer[index1 % bufferSize]; // Ensure index1 is within bounds
        float s2 = buffer[index2 % bufferSize]; // Ensure index2 is within bounds
        return (1.f - frac) * s1 + frac * s2;
    }

    // Get envelope value using a Hann window: 0.5 * (1 - cos(2 * PI * t))
    float getEnvelope() {
        return 0.5f * (1.f - std::cos(2.f * M_PI * life));
    }

    // Advance the grain
    void advance() {
        bufferPos += 1.0; // Grains play at 1x speed for now
        life += lifeIncrement;
    }

    bool isAlive() {
        return life < 1.f;
    }
};


struct Granular : Module {
    // Use the ParamId enums
    enum ParamId {
        POSITION_PARAM,
        GRAIN_SIZE_PARAM,
        GRAIN_DENSITY_PARAM,
        ENV_SHAPE_PARAM,
        RANDOM_PARAM,
        PARAMS_LEN
    };
    // Use the InputId enums
    enum InputId {
        POSITION_INPUT,
        INPUTS_LEN
    };
    // Use the OutputId enums
    enum OutputId {
        AUDIO_OUTPUT,
        OUTPUTS_LEN
    };
    // Use the LightId enums
    enum LightId {
        LOADING_LIGHT,
        LIGHTS_LEN
    };

    // Buffer to hold the entire audio file (mono)
    std::vector<float> audioBuffer;
    std::string lastPath = "";
    unsigned int fileSampleRate = 44100;

    // --- Granular Engine ---
    std::vector<Grain> grains;
    static const int MAX_GRAINS = 128; // Increased max grains
    float grainSpawnTimer = 0.f;

    // This will be read by the WaveformDisplay to show the playhead
    float grainSpawnPosition = 0.f;

    // Atomic flag to signal when loading is in progress
    std::atomic<bool> isLoading{false};

    Granular() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

        // Configure params using original enums
        configParam(POSITION_PARAM, 0.f, 1.f, 0.f, "Position");
        configParam(GRAIN_SIZE_PARAM, 0.01f, 2.0f, 0.1f, "Grain Size", " s"); // Adjusted range
        configParam(GRAIN_DENSITY_PARAM, 1.f, 100.f, 0.f, "Grain Density", " Hz"); // Adjusted range

        // Configure new params (functionality not yet implemented)
        configParam(ENV_SHAPE_PARAM, 0.f, 1.f, 0.f, "Env. Shape");
        configParam(RANDOM_PARAM, 0.f, 1.f, 0.f, "Random");

        configInput(POSITION_INPUT, "Position CV");
        configOutput(AUDIO_OUTPUT, "Audio");

        grains.reserve(MAX_GRAINS);
    }

    // MAIN AUDIO PROCESSING (AKA THE AC3 GRANULAR ENGINE)
    void process(const ProcessArgs& args) override {
        lights[LOADING_LIGHT].setBrightness(isLoading);

        // output silence if loading or buffer is empty
        if (isLoading || audioBuffer.empty()) {
            outputs[AUDIO_OUTPUT].setVoltage(0.f);
            return;
        }

        // --- Read Controls ---
        float density = params[GRAIN_DENSITY_PARAM].getValue();
        float grainSize = params[GRAIN_SIZE_PARAM].getValue();
        // Read new knob values (don't use them yet)
        // float envShape = params[ENV_SHAPE_PARAM].getValue();
        // float random = params[RANDOM_PARAM].getValue();


        // Get position from knob and CV
        grainSpawnPosition = params[POSITION_PARAM].getValue();
        grainSpawnPosition += inputs[POSITION_INPUT].getVoltage() * 0.1f; // 1V/Oct = 10% change
        grainSpawnPosition = rack::math::clamp(grainSpawnPosition, 0.f, 1.f);

        // --- Grain Spawning ---
        grainSpawnTimer -= args.sampleTime;
        if (grainSpawnTimer <= 0.f) {
            grainSpawnTimer = 1.f / density; // Reset timer

            if (grains.size() < MAX_GRAINS) {
                Grain g;

                // Spawn at the current position
                g.bufferPos = grainSpawnPosition * (audioBuffer.size() - 1);

                g.life = 0.f;
                float grainSizeInSamples = grainSize * fileSampleRate;
                // Ensure grainSizeInSamples is at least 1 to avoid division by zero
                if (grainSizeInSamples < 1.f) grainSizeInSamples = 1.f;
                g.lifeIncrement = 1.f / grainSizeInSamples;

                grains.push_back(g);
            }
        }

        // --- Process Grains ---
        float out = 0.f;
        for (size_t i = 0; i < grains.size(); ++i) {
            Grain& g = grains[i];

            float sample = g.getSample(audioBuffer);
            float env = g.getEnvelope();

            out += sample * env;

            g.advance();
        }

        // Remove dead grains (iterate backwards to safely erase)
        for (int i = grains.size() - 1; i >= 0; i--) {
            if (!grains[i].isAlive()) {
                grains.erase(grains.begin() + i);
            }
        }

        // Mixdown: divide by sqrt of grain count to avoid harsh clipping - DO NOT REMOVE THIS OR ELSE
        if (!grains.empty()) {
            out /= std::sqrt(grains.size());
        }

        // Output the sample (scaled to +/- 5V)
        outputs[AUDIO_OUTPUT].setVoltage(5.f * out);
    }


    // Function to set the new buffer (called from the widget/main thread)
    void setBuffer(std::vector<float> newBuffer, unsigned int newSampleRate) {
        audioBuffer = newBuffer;
        fileSampleRate = newSampleRate;
        grains.clear(); // Clear all active grains
        isLoading = false;
    }
};

// --- WaveformDisplay regenerateCache() Implementation ---
// DEFINE the function here, *after* Granular is fully defined
void WaveformDisplay::regenerateCache() {
    if (!module || box.size.x <= 0) return;

    size_t bufferSize = module->audioBuffer.size();
    if (bufferSize == 0) {
        displayCache.clear();
        return;
    }

    // Resize cache to match pixel width
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
            // Find the min/max values for this pixel column
            for (size_t j = startSample; j < endSample; j++) {
                float sample = module->audioBuffer[j];
                if (sample < minSample) minSample = sample;
                if (sample > maxSample) maxSample = sample;
            }
        }
        displayCache[i] = {minSample, maxSample};
    }
}


// --- WaveformDisplay drawing ---
// DEFINE the draw function here, after Granular is fully defined
void WaveformDisplay::draw(const DrawArgs& args) {
    // Draw background
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillColor(args.vg, nvgRGBA(20, 20, 20, 255));
    nvgFill(args.vg);

    if (!module)
        return;

    // --- Check if cache needs regenerating ---
    size_t bufferSize = module->audioBuffer.size();
    if (bufferSize != cacheBufferSize || box.size.x != cacheBoxWidth) {
        regenerateCache();
    }

    // If buffer is empty (or cache is), draw text
    if (displayCache.empty()) {
        nvgFontSize(args.vg, 14);
        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 100));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, "Drop .WAV file here", NULL);
        return;
    }

    // --- Draw the WAV's waveform ---
    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(0, 255, 100, 255));
    nvgStrokeWidth(args.vg, 1.f);

    for (int i = 0; i < (int)displayCache.size(); i++) {
        float minSample = displayCache[i].first;
        float maxSample = displayCache[i].second;

        // Map min and max to Y coordinates of the sample
        float y_min = box.size.y - ((minSample + 1.f) / 2.f) * box.size.y;
        float y_max = box.size.y - ((maxSample + 1.f) / 2.f) * box.size.y;

        // Draw a slice of the waveform
        nvgMoveTo(args.vg, i + 0.5f, y_min); // +0.5f for sharper pixels
        nvgLineTo(args.vg, i + 0.5f, y_max);
    }
    nvgStroke(args.vg); // draw all the vertical lines at once

    // --- Draw Playback Head (grain start position) ---
    float spawnX = module->grainSpawnPosition * box.size.x;

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 0, 0, 200));
    nvgStrokeWidth(args.vg, 2.0f);
    nvgMoveTo(args.vg, spawnX, 0);
    nvgLineTo(args.vg, spawnX, box.size.y);
    nvgStroke(args.vg);

    // --- Draw individual grain heads ---
    std::vector<Grain>& activeGrains = module->grains;

    nvgStrokeColor(args.vg, nvgRGBA(0, 150, 255, 100)); //playback head for active grains (blue)
    nvgStrokeWidth(args.vg, 1.0f);

    for (const Grain& grain : activeGrains) {
        // ensure grains that wrap around the buffer are drawn at the beginning
        double wrappedBufferPos = std::fmod(grain.bufferPos, (double)bufferSize);
        float grainX = (float)(wrappedBufferPos / bufferSize) * box.size.x;

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, grainX, 0);
        nvgLineTo(args.vg, grainX, box.size.y);
        nvgStroke(args.vg);
    }
}


struct GranularWidget : ModuleWidget {
    // Keep a pointer to the display for future invalidating of its cache
    WaveformDisplay* display = nullptr;

    GranularWidget(Granular* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/granular.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // waveform display
        display = new WaveformDisplay(); // Assign to member variable
        display->module = module;
        display->box.pos = mm2px(Vec(20.0, 30.0));
        display->box.size = mm2px(Vec(140, 40));
        addChild(display);

        // --- Use new helper.py positions with original ParamIds ---

        // RANDOM_PARAM (Random) - New Pos
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(184.573, 46.063)), module, Granular::RANDOM_PARAM));
        // GRAIN_SIZE_PARAM (Grain Size) - New Pos
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(54.0, 84.0)), module, Granular::GRAIN_SIZE_PARAM));
        // GRAIN_DENSITY_PARAM (Grain Density) - New Pos
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(84.0, 84.0)), module, Granular::GRAIN_DENSITY_PARAM));
        // ENV_SHAPE_PARAM (Env Shape) - New Pos
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(114.0, 84.0)), module, Granular::ENV_SHAPE_PARAM));
        // POSITION_PARAM (Position) - New Pos
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(144.0, 84.0)), module, Granular::POSITION_PARAM));

        // POSITION_INPUT (Position CV) - New Pos
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(184.573, 77.478)), module, Granular::POSITION_INPUT));

        // AUDIO_OUTPUT (Audio) - New Pos
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(184.573, 108.713)), module, Granular::AUDIO_OUTPUT));

        // LOADING_LIGHT (Loading) - New Pos
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(184.573, 30.224)), module, Granular::LOADING_LIGHT));
    }

    // Handle file drag-and-drop
    void onPathDrop(const PathDropEvent& e) override {
        if (e.paths.empty())
            return;

        std::string path = e.paths[0];
        std::string extension = rack::system::getExtension(path);

        if (extension == ".wav") {
            if (module) {
                // Cast to the module type
                Granular* granularModule = dynamic_cast<Granular*>(module);
                if (!granularModule)
                    return;

                // --- Start Loading Logic (on main thread) ---
                granularModule->isLoading = true;

                unsigned int channels;
                unsigned int sampleRate;
                drwav_uint64 totalFrames;
                float* pSampleData = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &sampleRate, &totalFrames, NULL);

                if (pSampleData == NULL) {
                    // Failed to load
                    granularModule->isLoading = false;
                    return;
                }

                std::vector<float> newBuffer;
                newBuffer.resize(totalFrames);

                if (channels == 1) {
                    // Mono: just copy
                    std::memcpy(newBuffer.data(), pSampleData, totalFrames * sizeof(float));
                } else {
                    // Stereo: mix down to mono
                    for (drwav_uint64 i = 0; i < totalFrames; i++) {
                        newBuffer[i] = (pSampleData[i * channels + 0] + pSampleData[i * channels + 1]) * 0.5f;
                    }
                }

                // Free the data loaded by dr_wav
                drwav_free(pSampleData, NULL);

                // Directly call setBuffer now that loading is done (isLoading is set to false inside setBuffer)
                granularModule->setBuffer(newBuffer, sampleRate);

                // --- Invalidate the display cache ---
                if (display) {
                    display->cacheBufferSize = 0; // Force cache regen on next draw
                }
            }
        }
    }
};


Model* modelGranular = createModel<Granular, GranularWidget>("granular");

