#include "plugin.hpp"
#include <vector>
#include <string>
#include <atomic>
#include <cmath> // Include for std::cos, M_PI, and std::fmod

// Include for windowing function
#include "dsp/window.hpp"

// --- dr_wav ---
// Include the dr_wav *header* only.
// The IMPLEMENTATION is defined in another file (like waves.cpp).
#include "dr_wav.h"
// --- /dr_wav ---


// Forward-declare the module
struct Granular;

// A simple widget to display the loaded waveform
struct WaveformDisplay : rack::TransparentWidget {
    Granular* module = nullptr;
    std::shared_ptr<rack::Font> font;

    WaveformDisplay() {
        font = APP->window->loadFont(rack::asset::system("res/fonts/ShareTechMono-Regular.ttf"));
    }

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

    // Get envelope value (using a Hann window)
    float getEnvelope() {
        // The formula for a Hann window value is 0.5 * (1 - cos(2 * PI * t))
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
    // Use the ParamId enums from BasicModule2
    enum ParamId {
        POSITION_PARAM, // Renamed for clarity
        GRAIN_SIZE_PARAM, // Renamed for clarity
        GRAIN_DENSITY_PARAM, // Renamed for clarity
        PARAMS_LEN
    };
    // Use the InputId enums from BasicModule2
    enum InputId {
        POSITION_INPUT,    // Renamed for clarity
        INPUTS_LEN
    };
    // Use the OutputId enums from BasicModule2
    enum OutputId {
        AUDIO_OUTPUT,    // Renamed for clarity
        OUTPUTS_LEN
    };
    // Use the LightId enums from BasicModule2
    enum LightId {
        LOADING_LIGHT,    // Renamed for clarity
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

        // Configure params using the BasicModule2 layout
        configParam(POSITION_PARAM, 0.f, 1.f, 0.5f, "Position");
        configParam(GRAIN_SIZE_PARAM, 0.01f, 1.0f, 0.1f, "Grain Size", " s"); // Adjusted range
        configParam(GRAIN_DENSITY_PARAM, 1.f, 200.f, 20.f, "Grain Density", " Hz"); // Adjusted range

        configInput(POSITION_INPUT, "Position CV");
        configOutput(AUDIO_OUTPUT, "Audio");

        grains.reserve(MAX_GRAINS);
    }

    // Main audio processing function - NOW A GRANULAR ENGINE
    void process(const ProcessArgs& args) override {
        // Use the LOADING_LIGHT as our loading light
        lights[LOADING_LIGHT].setBrightness(isLoading);

        // If loading or buffer is empty, output silence
        if (isLoading || audioBuffer.empty()) {
            outputs[AUDIO_OUTPUT].setVoltage(0.f);
            return;
        }

        // --- Read Controls ---
        float density = params[GRAIN_DENSITY_PARAM].getValue();
        float grainSize = params[GRAIN_SIZE_PARAM].getValue();

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

        // Mixdown: divide by sqrt of grain count to avoid harsh clipping
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

// --- WaveformDisplay draw() Implementation ---
// DEFINE the draw function here, *after* Granular is fully defined
void WaveformDisplay::draw(const DrawArgs& args) {
    // Draw background
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillColor(args.vg, nvgRGBA(20, 20, 20, 200));
    nvgFill(args.vg);

    if (!module)
        return;

    size_t bufferSize = module->audioBuffer.size();

    // If buffer is empty, draw text
    if (bufferSize == 0) {
        nvgFontSize(args.vg, 14);
        nvgFontFaceId(args.vg, font->handle);
        nvgFillColor(args.vg, nvgRGBA(255, 255, 255, 100));
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(args.vg, box.size.x / 2, box.size.y / 2, "Drop .WAV file here", NULL);
        return;
    }

    // Draw the waveform
    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(0, 255, 100, 255));
    nvgStrokeWidth(args.vg, 1.5);

    for (int i = 0; i < box.size.x; i++) {
        // Map pixel X to buffer index
        size_t sampleIndex = (size_t)((float)i / box.size.x * bufferSize);
        if (sampleIndex >= bufferSize) sampleIndex = bufferSize - 1; // Safety clamp
        float sample = module->audioBuffer[sampleIndex];

        // Map sample value (-1 to 1) to Y position
        float y = box.size.y - ((sample + 1.f) / 2.f) * box.size.y;

        if (i == 0)
            nvgMoveTo(args.vg, i, y);
        else
            nvgLineTo(args.vg, i, y);
    }
    nvgStroke(args.vg);

    // --- Draw Playback Head (Spawn Position) ---
    float spawnX = module->grainSpawnPosition * box.size.x;

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 0, 0, 200)); // Bright red for spawn position
    nvgStrokeWidth(args.vg, 2.0f);
    nvgMoveTo(args.vg, spawnX, 0);
    nvgLineTo(args.vg, spawnX, box.size.y);
    nvgStroke(args.vg);

    // --- Draw individual grain heads ---
    std::vector<Grain>& activeGrains = module->grains;

    nvgStrokeColor(args.vg, nvgRGBA(0, 255, 0, 100)); // Semi-transparent green for active grains
    nvgStrokeWidth(args.vg, 1.0f);

    for (const Grain& grain : activeGrains) {
        // --- FIX: Use fmod to wrap the grain's visual position ---
        // This ensures grains that wrap around the buffer are drawn at the beginning
        double wrappedBufferPos = std::fmod(grain.bufferPos, (double)bufferSize);
        float grainX = (float)(wrappedBufferPos / bufferSize) * box.size.x;
        // --- END FIX ---

        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, grainX, 0);
        nvgLineTo(args.vg, grainX, box.size.y);
        nvgStroke(args.vg);
    }
}


struct GranularWidget : ModuleWidget {
    GranularWidget(Granular* module) {
        setModule(module);
        // Use the new granular.svg file
        setPanel(createPanel(asset::plugin(pluginInstance, "res/granular.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Add the waveform display, placing it where the old scope was
        WaveformDisplay* display = new WaveformDisplay();
        display->module = module;
        display->box.pos = mm2px(Vec(39, 47.0));    // Position from BasicModule2
        display->box.size = mm2px(Vec(50, 30)); // Size from BasicModule2
        addChild(display);

        // --- Add Knobs, Ports, and Lights using BasicModule2 positions ---

        // POSITION_PARAM (Position)
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 46.063)), module, Granular::POSITION_PARAM));
        // POSITION_INPUT (Position CV)
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24, 77.478)), module, Granular::POSITION_INPUT));
        // AUDIO_OUTPUT (Audio)
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.24, 108.713)), module, Granular::AUDIO_OUTPUT));
        // LOADING_LIGHT (Loading)
        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(15.24, 30.224)), module, Granular::LOADING_LIGHT));

        // GRAIN_SIZE_PARAM (Grain Size)
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(54.277, 84.016)), module, Granular::GRAIN_SIZE_PARAM));

        // GRAIN_DENSITY_PARAM (Grain Density)
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(74.327, 84.016)), module, Granular::GRAIN_DENSITY_PARAM));
    }

    // Handle file drag-and-drop
    void onPathDrop(const PathDropEvent& e) override {
        if (e.paths.empty())
            return;

        std::string path = e.paths[0];
        std::string extension = rack::system::getExtension(path);

        if (extension == ".wav") {
            if (module) {
                // Cast to our module type
                Granular* granularModule = dynamic_cast<Granular*>(module);
                if (!granularModule)
                    return;

                // --- Start Loading Logic (runs on main thread) ---
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

                // Directly call setBuffer now that loading is done
                granularModule->setBuffer(newBuffer, sampleRate);
                // isLoading is set to false inside setBuffer
            }
        }
    }
};


Model* modelGranular = createModel<Granular, GranularWidget>("granular");

